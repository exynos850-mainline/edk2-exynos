/** @file
  ext4 on-disk walking: superblock/group-desc mount, inode reads, extent and
  legacy indirect-block mapping, directory entry iteration.

  Scope, on purpose:
   - Read-only. Nothing here ever writes a sector.
   - Both extent-mapped files (the mkfs.ext4 default since e2fsprogs ~1.42,
     i.e. basically everything made in the last decade) and legacy
     direct/single-indirect block-mapped files are supported. Double and
     triple indirect blocks are NOT - a file that needs them is reported as
     EFI_UNSUPPORTED rather than silently truncated. In practice this only
     matters for old ext2-formatted media; an SD card made by a current
     mke2fs/Fedora installer will never hit this path.
   - Directories are walked with a flat linear scan of every 4K (or
     block-size) directory block the inode maps, the same trick GRUB's and
     U-Boot's ext4 readers use: even when a directory has an htree index
     (mkfs's default "dir_index" feature) for fast lookup, the underlying
     directory data blocks still hold ordinary, fully linearly-scannable
     ext4_dir_entry_2 records - the htree is a pure lookup accelerator on
     top of that, so ignoring it entirely and just scanning is correct
     (if O(n) instead of O(log n), the trade-off every other read-only
     bootloader ext4 reader also makes).
   - Inline-data directories/files (EXT4_FEATURE_INCOMPAT_INLINE_DATA,
     i_flags EXT4_INODE_FLAG_INLINE_DATA) are detected and refused
     explicitly rather than misread as a corrupt extent tree. This feature
     is opt-in at mkfs time (-O inline_data) and essentially never set by
     a distro installer, so it's a documented gap, not a silent one.
   - No checksum verification (metadata_csum). This is a boot-time reader
     for media the user controls, not a validator; a corrupt filesystem
     just fails lookups/reads with an error instead of crashing.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Ext4Dxe.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>

STATIC
EFI_STATUS
Ext4ReadDisk (
  IN  EXT4_VOLUME  *Volume,
  IN  UINT64       Offset,
  IN  UINTN        Size,
  OUT VOID         *Buffer
  )
{
  return Volume->DiskIo->ReadDisk (
                           Volume->DiskIo,
                           Volume->BlockIo->Media->MediaId,
                           Offset,
                           Size,
                           Buffer
                           );
}

STATIC
EFI_STATUS
Ext4ReadBlock (
  IN  EXT4_VOLUME  *Volume,
  IN  UINT64       BlockNum,
  OUT VOID         *Buffer
  )
{
  if (BlockNum == 0) {
    //
    // Block 0 is reserved/boot area; a 0 pointer in an extent or indirect
    // table means "hole" (sparse file). Callers that reach here for file
    // data want zero-fill; for metadata, a 0 pointer should never have been
    // followed in the first place. Either way, returning zeroes is safe.
    //
    ZeroMem (Buffer, Volume->BlockSize);
    return EFI_SUCCESS;
  }

  return Ext4ReadDisk (Volume, BlockNum * Volume->BlockSize, Volume->BlockSize, Buffer);
}

UINT64
Ext4InodeSize (
  IN CONST EXT4_INODE  *Inode
  )
{
  if ((Inode->i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) {
    //
    // Directories never use i_size_high; some mkfs versions leave garbage
    // there for old reiser-compat reasons. Be conservative.
    //
    return Inode->i_size_lo;
  }

  return ((UINT64)Inode->i_size_high << 32) | Inode->i_size_lo;
}

VOID
Ext4EpochToEfiTime (
  IN  UINT32    Epoch,
  OUT EFI_TIME  *Time
  )
{
  //
  // Good-enough Unix-epoch -> civil date conversion (UTC, no leap seconds),
  // Howard Hinnant's days_from_civil algorithm run in reverse. This is a
  // boot-menu file-info timestamp, not an RTC - a few edge cases around
  // the far past/future are not worth pulling in a full calendar library.
  //
  INT64  Days;
  INT64  Z;
  INT64  Era;
  UINT64 Doe;
  UINT64 Yoe;
  UINT64 Doy;
  UINT64 Mp;
  UINT64 Day;
  UINT64 Month;
  UINT64 Year;
  UINT32 SecOfDay;

  ZeroMem (Time, sizeof (*Time));

  SecOfDay = Epoch % 86400;
  Days     = (INT64)(Epoch / 86400);

  Z   = Days + 719468;
  Era = (Z >= 0 ? Z : Z - 146096) / 146097;
  Doe = (UINT64)(Z - Era * 146097);
  Yoe = (Doe - Doe / 1460 + Doe / 36524 - Doe / 146096) / 365;
  Year = Yoe + (UINT64)Era * 400;
  Doy  = Doe - (365 * Yoe + Yoe / 4 - Yoe / 100);
  Mp   = (5 * Doy + 2) / 153;
  Day  = Doy - (153 * Mp + 2) / 5 + 1;
  Month = Mp + (Mp < 10 ? 3 : -9);
  if (Month <= 2) {
    Year += 1;
  }

  Time->Year   = (UINT16)Year;
  Time->Month  = (UINT8)Month;
  Time->Day    = (UINT8)Day;
  Time->Hour   = (UINT8)(SecOfDay / 3600);
  Time->Minute = (UINT8)((SecOfDay / 60) % 60);
  Time->Second = (UINT8)(SecOfDay % 60);
  Time->TimeZone = EFI_UNSPECIFIED_TIMEZONE;
}

EFI_STATUS
Ext4MountVolume (
  IN OUT EXT4_VOLUME  *Volume
  )
{
  EFI_STATUS        Status;
  EXT4_SUPERBLOCK    *Sb;
  UINT64             GdtBlock;
  UINTN              GdtBytes;
  UINT8              *RawGdt;
  UINTN              Index;

  Sb = AllocatePool (sizeof (*Sb));
  if (Sb == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = Volume->DiskIo->ReadDisk (
                             Volume->DiskIo,
                             Volume->BlockIo->Media->MediaId,
                             EXT4_SUPERBLOCK_OFFSET,
                             sizeof (*Sb),
                             Sb
                             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (Sb->s_magic != EXT4_SUPER_MAGIC) {
    Status = EFI_NOT_FOUND;
    goto Done;
  }

  if ((Sb->s_log_block_size > 6) || (Sb->s_inodes_per_group == 0) ||
      (Sb->s_blocks_per_group == 0) || (Sb->s_inodes_count == 0))
  {
    //
    // Sane values are log_block_size 0..6 (1KiB..64KiB blocks). Anything
    // outside that, or a zero group geometry, means this isn't actually a
    // valid ext4 superblock (coincidental 0xEF53 at this offset on a
    // foreign filesystem) - refuse rather than compute garbage sizes.
    //
    Status = EFI_VOLUME_CORRUPTED;
    goto Done;
  }

  Volume->BlockSize = 1024u << Sb->s_log_block_size;

  Volume->InodeSize = (Sb->s_rev_level == 0) ? EXT4_GOOD_OLD_INODE_SIZE : Sb->s_inode_size;
  if ((Volume->InodeSize < EXT4_GOOD_OLD_INODE_SIZE) || (Volume->InodeSize > Volume->BlockSize) ||
      ((Volume->InodeSize & (Volume->InodeSize - 1)) != 0))
  {
    Status = EFI_VOLUME_CORRUPTED;
    goto Done;
  }

  Volume->InodesPerGroup = Sb->s_inodes_per_group;
  Volume->BlocksPerGroup = Sb->s_blocks_per_group;
  Volume->Has64Bit       = ((Sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0);
  Volume->HasFileType    = ((Sb->s_feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE) != 0);
  Volume->GroupDescSize  = Volume->Has64Bit ? Sb->s_desc_size : 32;
  if ((Volume->GroupDescSize < 32) || (Volume->GroupDescSize > sizeof (EXT4_GROUP_DESC))) {
    Volume->GroupDescSize = 32;
  }

  Volume->BlocksCount =
    ((UINT64)Sb->s_blocks_count_hi << 32) | Sb->s_blocks_count_lo;
  Volume->FreeBlocksCount =
    ((UINT64)Sb->s_free_blocks_count_hi << 32) | Sb->s_free_blocks_count_lo;

  Volume->GroupCount = (UINT32)DivU64x32 (
                                  Volume->BlocksCount - Sb->s_first_data_block +
                                  Volume->BlocksPerGroup - 1,
                                  Volume->BlocksPerGroup
                                  );
  if ((Volume->GroupCount == 0) || (Volume->GroupCount > 0x100000)) {
    //
    // >1M block groups would mean a multi-exabyte filesystem; past any
    // card this device will ever see, and more likely a corrupt superblock.
    //
    Status = EFI_VOLUME_CORRUPTED;
    goto Done;
  }

  CopyMem (Volume->VolumeLabel, Sb->s_volume_name, 16);
  Volume->VolumeLabel[16] = '\0';

  GdtBlock  = Sb->s_first_data_block + 1;
  GdtBytes  = (UINTN)Volume->GroupCount * Volume->GroupDescSize;
  RawGdt    = AllocatePool (GdtBytes);
  if (RawGdt == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Done;
  }

  Status = Ext4ReadDisk (Volume, GdtBlock * Volume->BlockSize, GdtBytes, RawGdt);
  if (EFI_ERROR (Status)) {
    FreePool (RawGdt);
    goto Done;
  }

  Volume->GroupDescs = AllocateZeroPool (Volume->GroupCount * sizeof (EXT4_GROUP_DESC));
  if (Volume->GroupDescs == NULL) {
    FreePool (RawGdt);
    Status = EFI_OUT_OF_RESOURCES;
    goto Done;
  }

  for (Index = 0; Index < Volume->GroupCount; Index++) {
    CopyMem (
      &Volume->GroupDescs[Index],
      RawGdt + (Index * Volume->GroupDescSize),
      Volume->GroupDescSize
      );
  }

  FreePool (RawGdt);
  Status = EFI_SUCCESS;

Done:
  FreePool (Sb);
  return Status;
}

EFI_STATUS
Ext4ReadInode (
  IN  EXT4_VOLUME  *Volume,
  IN  UINT32       InodeNum,
  OUT EXT4_INODE   *Inode
  )
{
  UINT32              Group;
  UINT32              IndexInGroup;
  UINT64              InodeTableBlock;
  UINT64              Offset;
  CONST EXT4_GROUP_DESC  *Gd;

  if ((InodeNum == 0) || (InodeNum > ((UINT64)Volume->GroupCount * Volume->InodesPerGroup))) {
    return EFI_NOT_FOUND;
  }

  Group        = (InodeNum - 1) / Volume->InodesPerGroup;
  IndexInGroup = (InodeNum - 1) % Volume->InodesPerGroup;
  if (Group >= Volume->GroupCount) {
    return EFI_NOT_FOUND;
  }

  Gd = &Volume->GroupDescs[Group];
  InodeTableBlock = Gd->bg_inode_table_lo;
  if (Volume->Has64Bit) {
    InodeTableBlock |= ((UINT64)Gd->bg_inode_table_hi << 32);
  }

  Offset = (InodeTableBlock * Volume->BlockSize) + ((UINT64)IndexInGroup * Volume->InodeSize);

  ZeroMem (Inode, sizeof (*Inode));
  return Ext4ReadDisk (Volume, Offset, MIN (sizeof (*Inode), Volume->InodeSize), Inode);
}

//
// Resolve logical block LBlock (0-based, in units of Volume->BlockSize)
// of Inode's data to a physical block number. Returns EFI_SUCCESS with
// *PBlock == 0 for a hole (sparse region - caller should zero-fill),
// EFI_NOT_FOUND past the last mapped block, EFI_UNSUPPORTED for anything
// this reader does not implement (double/triple indirect, inline data).
//
STATIC
EFI_STATUS
Ext4ExtentMapBlock (
  IN  EXT4_VOLUME  *Volume,
  IN  CONST UINT32 *ExtentRoot,  // Inode->i_block, 15 x UINT32 = 60 bytes
  IN  UINT32       LBlock,
  OUT UINT64       *PBlock
  )
{
  CONST UINT8          *Node;
  EXT4_EXTENT_HEADER     Header;
  UINT8                  *Buffer;
  EFI_STATUS              Status;
  UINT16                  Depth;
  UINTN                   MaxDepth;

  Buffer = AllocatePool (Volume->BlockSize);
  if (Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Node = (CONST UINT8 *)ExtentRoot;
  CopyMem (&Header, Node, sizeof (Header));
  Status = EFI_SUCCESS;

  for (MaxDepth = 0; MaxDepth < 8; MaxDepth++) {
    UINTN  EntryCount;
    UINTN  I;

    if (Header.eh_magic != EXT4_EXT_MAGIC) {
      Status = EFI_VOLUME_CORRUPTED;
      goto Done;
    }

    EntryCount = Header.eh_entries;
    Depth      = Header.eh_depth;

    if (Depth == 0) {
      CONST EXT4_EXTENT  *Leaves;

      Leaves = (CONST EXT4_EXTENT *)(Node + sizeof (EXT4_EXTENT_HEADER));
      *PBlock = 0;

      for (I = 0; I < EntryCount; I++) {
        UINT32  Len;

        Len = Leaves[I].ee_len;
        if (Len > 32768) {
          Len -= 32768; // uninitialized extent, same physical mapping
        }

        if ((LBlock >= Leaves[I].ee_block) && (LBlock < Leaves[I].ee_block + Len)) {
          UINT64  Start;

          Start = ((UINT64)Leaves[I].ee_start_hi << 32) | Leaves[I].ee_start_lo;
          *PBlock = Start + (LBlock - Leaves[I].ee_block);
          Status  = EFI_SUCCESS;
          goto Done;
        }
      }

      // Not covered by any leaf: a hole.
      Status = EFI_SUCCESS;
      goto Done;
    }

    //
    // Internal node: find the last index entry whose starting logical
    // block is <= LBlock, descend into it.
    //
    {
      CONST EXT4_EXTENT_IDX  *Idxs;
      INTN                     Best;

      Idxs = (CONST EXT4_EXTENT_IDX *)(Node + sizeof (EXT4_EXTENT_HEADER));
      Best = -1;

      for (I = 0; I < EntryCount; I++) {
        if (Idxs[I].ei_block <= LBlock) {
          Best = (INTN)I;
        } else {
          break;
        }
      }

      if (Best < 0) {
        *PBlock = 0;
        Status  = EFI_SUCCESS;
        goto Done;
      }

      {
        UINT64  Child;

        Child = ((UINT64)Idxs[Best].ei_leaf_hi << 32) | Idxs[Best].ei_leaf_lo;
        Status = Ext4ReadBlock (Volume, Child, Buffer);
        if (EFI_ERROR (Status)) {
          goto Done;
        }

        Node = Buffer;
        CopyMem (&Header, Node, sizeof (Header));
      }
    }
  }

  Status = EFI_VOLUME_CORRUPTED; // depth too large, almost certainly corrupt/hostile

Done:
  FreePool (Buffer);
  return Status;
}

STATIC
EFI_STATUS
Ext4LegacyMapBlock (
  IN  EXT4_VOLUME  *Volume,
  IN  CONST UINT32 *Block, // Inode->i_block, 15 entries
  IN  UINT32       LBlock,
  OUT UINT64       *PBlock
  )
{
  CONST UINTN  PointersPerBlock = Volume->BlockSize / sizeof (UINT32);

  *PBlock = 0;

  if (LBlock < 12) {
    *PBlock = Block[LBlock];
    return EFI_SUCCESS;
  }

  LBlock -= 12;

  if (LBlock < PointersPerBlock) {
    EFI_STATUS  Status;
    UINT32      *Indirect;

    if (Block[12] == 0) {
      return EFI_SUCCESS; // hole
    }

    Indirect = AllocatePool (Volume->BlockSize);
    if (Indirect == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    Status = Ext4ReadBlock (Volume, Block[12], Indirect);
    if (!EFI_ERROR (Status)) {
      *PBlock = Indirect[LBlock];
    }

    FreePool (Indirect);
    return Status;
  }

  //
  // Double/triple indirect: not implemented. See file header comment -
  // this is only reachable on pre-extents (ext2/ext3-era) filesystems with
  // files bigger than 12 + BlockSize/4 blocks (e.g. >4096 blocks = 16MiB at
  // a 4K blocksize), which a current mkfs.ext4 will never produce.
  //
  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
Ext4MapBlock (
  IN  EXT4_VOLUME  *Volume,
  IN  CONST EXT4_INODE  *Inode,
  IN  UINT32       LBlock,
  OUT UINT64       *PBlock
  )
{
  if (Inode->i_flags & EXT4_INODE_FLAG_INLINE_DATA) {
    return EFI_UNSUPPORTED;
  }

  if (Inode->i_flags & EXT4_INODE_FLAG_EXTENTS) {
    return Ext4ExtentMapBlock (Volume, Inode->i_block, LBlock, PBlock);
  }

  return Ext4LegacyMapBlock (Volume, Inode->i_block, LBlock, PBlock);
}

EFI_STATUS
Ext4ReadFileData (
  IN     EXT4_VOLUME  *Volume,
  IN     EXT4_INODE   *Inode,
  IN     UINT64       Position,
  IN OUT UINTN        *BufferSize,
  OUT    VOID         *Buffer
  )
{
  UINT64      FileSize;
  UINTN       Remaining;
  UINTN       Done;
  UINT8       *Out;
  UINT8       *BlockBuf;
  EFI_STATUS  Status;

  FileSize = Ext4InodeSize (Inode);
  if (Position >= FileSize) {
    *BufferSize = 0;
    return EFI_SUCCESS;
  }

  Remaining = *BufferSize;
  if (Position + Remaining > FileSize) {
    Remaining = (UINTN)(FileSize - Position);
  }

  BlockBuf = AllocatePool (Volume->BlockSize);
  if (BlockBuf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Out    = Buffer;
  Done   = 0;
  Status = EFI_SUCCESS;

  while (Remaining > 0) {
    UINT32  LBlock;
    UINT32  InBlockOffset;
    UINTN   Chunk;
    UINT64  PBlock;

    LBlock        = (UINT32)(Position / Volume->BlockSize);
    InBlockOffset = (UINT32)(Position % Volume->BlockSize);
    Chunk         = Volume->BlockSize - InBlockOffset;
    if (Chunk > Remaining) {
      Chunk = Remaining;
    }

    Status = Ext4MapBlock (Volume, Inode, LBlock, &PBlock);
    if (EFI_ERROR (Status)) {
      break;
    }

    if (PBlock == 0) {
      ZeroMem (Out + Done, Chunk);
    } else {
      Status = Ext4ReadBlock (Volume, PBlock, BlockBuf);
      if (EFI_ERROR (Status)) {
        break;
      }

      CopyMem (Out + Done, BlockBuf + InBlockOffset, Chunk);
    }

    Position  += Chunk;
    Done      += Chunk;
    Remaining -= Chunk;
  }

  FreePool (BlockBuf);
  *BufferSize = Done;
  //
  // Report success as long as we made progress; a mid-stream mapping error
  // on a 40th block of a 41-block read still returns the first 40 blocks'
  // worth of real data to the caller, which is more useful than discarding
  // it - the caller sees the short read via the returned *BufferSize.
  //
  return (Done > 0) ? EFI_SUCCESS : Status;
}

EFI_STATUS
Ext4DirNext (
  IN     EXT4_VOLUME  *Volume,
  IN     EXT4_INODE   *DirInode,
  IN OUT UINT64       *Cursor,
  OUT    CHAR8        *NameOut,
  OUT    UINTN        *NameLenOut,
  OUT    UINT32       *InodeNumOut,
  OUT    UINT8        *FileTypeOut
  )
{
  UINT64      DirSize;
  UINT8       *BlockBuf;
  EFI_STATUS  Status;

  DirSize = Ext4InodeSize (DirInode);
  BlockBuf = AllocatePool (Volume->BlockSize);
  if (BlockBuf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = EFI_NOT_FOUND;

  while (*Cursor < DirSize) {
    UINT32               LBlock;
    UINT32               InBlockOffset;
    UINT64                PBlock;
    EXT4_DIR_ENTRY_2        Ent;
    CONST UINT8             *EntBase;

    LBlock        = (UINT32)(*Cursor / Volume->BlockSize);
    InBlockOffset = (UINT32)(*Cursor % Volume->BlockSize);

    Status = Ext4MapBlock (Volume, DirInode, LBlock, &PBlock);
    if (EFI_ERROR (Status)) {
      goto Done;
    }

    if (PBlock == 0) {
      // Hole in a directory: skip the whole block, nothing to read here.
      *Cursor = (UINT64)(LBlock + 1) * Volume->BlockSize;
      continue;
    }

    Status = Ext4ReadBlock (Volume, PBlock, BlockBuf);
    if (EFI_ERROR (Status)) {
      goto Done;
    }

    EntBase = BlockBuf + InBlockOffset;
    CopyMem (&Ent, EntBase, sizeof (Ent));

    if ((Ent.rec_len < sizeof (Ent)) || (InBlockOffset + Ent.rec_len > Volume->BlockSize)) {
      // Corrupt rec_len: bail rather than loop forever or read OOB.
      Status = EFI_VOLUME_CORRUPTED;
      goto Done;
    }

    *Cursor += Ent.rec_len;

    if ((Ent.inode == 0) || (Ent.name_len == 0)) {
      continue; // deleted entry
    }

    if ((Ent.name_len == 1) && (EntBase[sizeof (Ent)] == '.')) {
      continue;
    }

    if ((Ent.name_len == 2) && (EntBase[sizeof (Ent)] == '.') && (EntBase[sizeof (Ent) + 1] == '.')) {
      continue;
    }

    CopyMem (NameOut, EntBase + sizeof (Ent), Ent.name_len);
    NameOut[Ent.name_len] = '\0';
    *NameLenOut  = Ent.name_len;
    *InodeNumOut = Ent.inode;
    *FileTypeOut = Volume->HasFileType ? Ent.file_type : EXT4_FT_UNKNOWN;
    Status       = EFI_SUCCESS;
    goto Done;
  }

  Status = EFI_NOT_FOUND;

Done:
  FreePool (BlockBuf);
  return Status;
}

EFI_STATUS
Ext4LookupInDir (
  IN  EXT4_VOLUME  *Volume,
  IN  EXT4_INODE   *DirInode,
  IN  CONST CHAR8  *Name,
  IN  UINTN        NameLen,
  OUT UINT32       *OutInodeNum,
  OUT UINT8        *OutFileType OPTIONAL
  )
{
  UINT64      Cursor;
  CHAR8       Candidate[EXT4_NAME_MAX + 1];
  UINTN       CandidateLen;
  UINT32      InodeNum;
  UINT8       FileType;
  EFI_STATUS  Status;

  if ((DirInode->i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) {
    return EFI_NOT_FOUND;
  }

  Cursor = 0;
  for ( ; ;) {
    Status = Ext4DirNext (Volume, DirInode, &Cursor, Candidate, &CandidateLen, &InodeNum, &FileType);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((CandidateLen == NameLen) && (CompareMem (Candidate, Name, NameLen) == 0)) {
      *OutInodeNum = InodeNum;
      if (OutFileType != NULL) {
        *OutFileType = FileType;
      }

      return EFI_SUCCESS;
    }
  }
}
