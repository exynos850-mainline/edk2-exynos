/** @file
  On-disk ext2/3/4 structures and private driver types for Ext4Dxe.

  Field layouts below are taken from the stable, long-frozen ext4 on-disk
  format (fs/ext4/ext4.h in the Linux kernel; this layout has not changed
  since ext4 was merged, and most of it goes back to ext2). AArch64 is
  little-endian, same as every field here, so no byte-swapping is done.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef EXT4_DXE_H_
#define EXT4_DXE_H_

#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DiskIo.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/SimpleFileSystem.h>

#define EXT4_SUPERBLOCK_OFFSET  1024
#define EXT4_SUPER_MAGIC        0xEF53

#pragma pack(1)

typedef struct {
  UINT32    s_inodes_count;
  UINT32    s_blocks_count_lo;
  UINT32    s_r_blocks_count_lo;
  UINT32    s_free_blocks_count_lo;
  UINT32    s_free_inodes_count;
  UINT32    s_first_data_block;
  UINT32    s_log_block_size;
  UINT32    s_log_cluster_size;
  UINT32    s_blocks_per_group;
  UINT32    s_clusters_per_group;
  UINT32    s_inodes_per_group;
  UINT32    s_mtime;
  UINT32    s_wtime;
  UINT16    s_mnt_count;
  UINT16    s_max_mnt_count;
  UINT16    s_magic;
  UINT16    s_state;
  UINT16    s_errors;
  UINT16    s_minor_rev_level;
  UINT32    s_lastcheck;
  UINT32    s_checkinterval;
  UINT32    s_creator_os;
  UINT32    s_rev_level;
  UINT16    s_def_resuid;
  UINT16    s_def_resgid;
  UINT32    s_first_ino;
  UINT16    s_inode_size;
  UINT16    s_block_group_nr;
  UINT32    s_feature_compat;
  UINT32    s_feature_incompat;
  UINT32    s_feature_ro_compat;
  UINT8     s_uuid[16];
  CHAR8     s_volume_name[16];
  CHAR8     s_last_mounted[64];
  UINT32    s_algorithm_usage_bitmap;
  UINT8     s_prealloc_blocks;
  UINT8     s_prealloc_dir_blocks;
  UINT16    s_reserved_gdt_blocks;
  UINT8     s_journal_uuid[16];
  UINT32    s_journal_inum;
  UINT32    s_journal_dev;
  UINT32    s_last_orphan;
  UINT32    s_hash_seed[4];
  UINT8     s_def_hash_version;
  UINT8     s_jnl_backup_type;
  UINT16    s_desc_size;
  UINT32    s_default_mount_opts;
  UINT32    s_first_meta_bg;
  UINT32    s_mkfs_time;
  UINT32    s_jnl_blocks[17];
  UINT32    s_blocks_count_hi;
  UINT32    s_r_blocks_count_hi;
  UINT32    s_free_blocks_count_hi;
  UINT8     s_pad_to_1024[1024 - 344];
} EXT4_SUPERBLOCK;

#define EXT4_FEATURE_INCOMPAT_FILETYPE  0x0002
#define EXT4_FEATURE_INCOMPAT_EXTENTS   0x0040
#define EXT4_FEATURE_INCOMPAT_64BIT     0x0080
#define EXT4_FEATURE_INCOMPAT_FLEX_BG   0x0200
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA 0x8000

typedef struct {
  UINT32    bg_block_bitmap_lo;
  UINT32    bg_inode_bitmap_lo;
  UINT32    bg_inode_table_lo;
  UINT16    bg_free_blocks_count_lo;
  UINT16    bg_free_inodes_count_lo;
  UINT16    bg_used_dirs_count_lo;
  UINT16    bg_flags;
  UINT32    bg_exclude_bitmap_lo;
  UINT16    bg_block_bitmap_csum_lo;
  UINT16    bg_inode_bitmap_csum_lo;
  UINT16    bg_itable_unused_lo;
  UINT16    bg_checksum;
  UINT32    bg_block_bitmap_hi;
  UINT32    bg_inode_bitmap_hi;
  UINT32    bg_inode_table_hi;
  UINT16    bg_free_blocks_count_hi;
  UINT16    bg_free_inodes_count_hi;
  UINT16    bg_used_dirs_count_hi;
  UINT16    bg_itable_unused_hi;
  UINT32    bg_exclude_bitmap_hi;
  UINT16    bg_block_bitmap_csum_hi;
  UINT16    bg_inode_bitmap_csum_hi;
  UINT32    bg_reserved;
} EXT4_GROUP_DESC; // 64 bytes; only the first 32 are valid without the 64BIT feature

#define EXT4_GOOD_OLD_INODE_SIZE  128

typedef struct {
  UINT16    i_mode;
  UINT16    i_uid;
  UINT32    i_size_lo;
  UINT32    i_atime;
  UINT32    i_ctime;
  UINT32    i_mtime;
  UINT32    i_dtime;
  UINT16    i_gid;
  UINT16    i_links_count;
  UINT32    i_blocks_lo;
  UINT32    i_flags;
  UINT32    osd1;
  UINT32    i_block[15];
  UINT32    i_generation;
  UINT32    i_file_acl_lo;
  UINT32    i_size_high;
  UINT32    i_obso_faddr;
  UINT8     osd2[12];
  UINT16    i_extra_isize;
  UINT16    i_checksum_hi;
  UINT32    i_ctime_extra;
  UINT32    i_mtime_extra;
  UINT32    i_atime_extra;
  UINT32    i_crtime;
  UINT32    i_crtime_extra;
  UINT32    i_version_hi;
  UINT32    i_projid;
} EXT4_INODE; // 160 bytes of the (at least) 128-byte on-disk inode we care about

#define EXT4_S_IFMT   0xF000
#define EXT4_S_IFREG  0x8000
#define EXT4_S_IFDIR  0x4000
#define EXT4_S_IFLNK  0xA000

#define EXT4_INODE_FLAG_EXTENTS      0x00080000
#define EXT4_INODE_FLAG_INLINE_DATA  0x10000000

#define EXT4_ROOT_INO  2

typedef struct {
  UINT16    eh_magic;
  UINT16    eh_entries;
  UINT16    eh_max;
  UINT16    eh_depth;
  UINT32    eh_generation;
} EXT4_EXTENT_HEADER; // 12 bytes

#define EXT4_EXT_MAGIC  0xF30A

typedef struct {
  UINT32    ei_block;
  UINT32    ei_leaf_lo;
  UINT16    ei_leaf_hi;
  UINT16    ei_unused;
} EXT4_EXTENT_IDX; // 12 bytes

typedef struct {
  UINT32    ee_block;
  UINT16    ee_len;       // top bit set => uninitialized extent
  UINT16    ee_start_hi;
  UINT32    ee_start_lo;
} EXT4_EXTENT; // 12 bytes

typedef struct {
  UINT32    inode;
  UINT16    rec_len;
  UINT8     name_len;
  UINT8     file_type;
  // CHAR8 name[name_len] follows, not NUL terminated
} EXT4_DIR_ENTRY_2;

#define EXT4_FT_UNKNOWN   0
#define EXT4_FT_REG_FILE  1
#define EXT4_FT_DIR       2
#define EXT4_FT_SYMLINK   7

#pragma pack()

//
// Driver-private runtime state, not on-disk formats.
//

typedef struct {
  UINT32    BlockSize;
  UINT32    InodeSize;
  UINT32    InodesPerGroup;
  UINT32    BlocksPerGroup;
  UINT32    GroupDescSize;
  UINT32    GroupCount;
  UINT64    BlocksCount;
  UINT64    FreeBlocksCount;
  BOOLEAN   Has64Bit;
  BOOLEAN   HasFileType;
  CHAR8     VolumeLabel[17];

  EFI_HANDLE                       Handle;
  EFI_BLOCK_IO_PROTOCOL             *BlockIo;
  EFI_DISK_IO_PROTOCOL              *DiskIo;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL   Fs;

  EXT4_GROUP_DESC    *GroupDescs; // GroupCount entries, raw 64-byte stride regardless of GroupDescSize
  UINTN              OpenFiles;
} EXT4_VOLUME;

#define EXT4_VOLUME_FROM_FS(a)  BASE_CR (a, EXT4_VOLUME, Fs)

typedef struct {
  UINT32    Signature;
  EXT4_VOLUME    *Volume;
  EFI_FILE_PROTOCOL    File;
  UINT32    InodeNum;
  EXT4_INODE    Inode;
  UINT64    Position;
  BOOLEAN   IsDirectory;
  //
  // Directory read cursor: byte offset from the start of the directory's
  // logical data stream. Advanced one dirent at a time by Read().
  //
  UINT64    DirCursor;
  CHAR16    *Name; // leaf name this handle was opened with, for GetInfo()
} EXT4_OPEN_FILE;

#define EXT4_OPEN_FILE_SIGNATURE  SIGNATURE_32 ('e', '4', 'o', 'f')
#define EXT4_OPEN_FILE_FROM_THIS(a)  CR (a, EXT4_OPEN_FILE, File, EXT4_OPEN_FILE_SIGNATURE)

extern EFI_DRIVER_BINDING_PROTOCOL   gExt4DriverBinding;

EFI_STATUS
Ext4MountVolume (
  IN OUT EXT4_VOLUME  *Volume
  );

EFI_STATUS
Ext4ReadInode (
  IN  EXT4_VOLUME  *Volume,
  IN  UINT32       InodeNum,
  OUT EXT4_INODE   *Inode
  );

EFI_STATUS
Ext4ReadFileData (
  IN     EXT4_VOLUME  *Volume,
  IN     EXT4_INODE   *Inode,
  IN     UINT64       Position,
  IN OUT UINTN        *BufferSize,
  OUT    VOID         *Buffer
  );

EFI_STATUS
Ext4LookupInDir (
  IN  EXT4_VOLUME  *Volume,
  IN  EXT4_INODE   *DirInode,
  IN  CONST CHAR8  *Name,
  IN  UINTN        NameLen,
  OUT UINT32       *OutInodeNum,
  OUT UINT8        *OutFileType OPTIONAL
  );

EFI_STATUS
Ext4DirNext (
  IN     EXT4_VOLUME      *Volume,
  IN     EXT4_INODE       *DirInode,
  IN OUT UINT64           *Cursor,
  OUT    CHAR8            *NameOut,     // caller buffer, EXT4_NAME_MAX+1 bytes
  OUT    UINTN            *NameLenOut,
  OUT    UINT32           *InodeNumOut,
  OUT    UINT8            *FileTypeOut
  );

EFI_STATUS
Ext4FileOpen (
  IN  EFI_FILE_PROTOCOL  *This,
  OUT EFI_FILE_PROTOCOL  **NewHandle,
  IN  CHAR16             *FileName,
  IN  UINT64             OpenMode,
  IN  UINT64             Attributes
  );

#define EXT4_NAME_MAX  255

UINT64
Ext4InodeSize (
  IN CONST EXT4_INODE  *Inode
  );

VOID
Ext4EpochToEfiTime (
  IN  UINT32       Epoch,
  OUT EFI_TIME     *Time
  );

#endif
