/** @file
  Read-only ext4 EFI_SIMPLE_FILE_SYSTEM_PROTOCOL driver.

  Why: the a145f build has a read-only eMMC EFI_BLOCK_IO_PROTOCOL driver and
  PartitionDxe/DiskIoDxe already in the FV, but the only filesystem driver
  wired up is FatPkg's FAT driver. The Linux-root partitions this whole
  project boots from (uniLoader, U-Boot, and the Fedora-on-SD-card work)
  are ext4, not FAT, so nothing in EDK2 itself could look inside them -
  RegDump/MmcDiagDxe could see the raw disk, but not list or read a single
  file on it. This driver binds EFI_SIMPLE_FILE_SYSTEM_PROTOCOL onto any
  partition whose superblock says ext4, read-only, so the Shell / boot
  menu / a future bootloader entry can browse and read files on it the
  same way FatPkg does for a FAT partition - e.g. `fs1:` in the UEFI Shell
  on an ext4 partition, or reading /boot/vmlinuz straight out of an ext4
  root without needing a separate FAT /boot.

  Everything that touches the on-disk format lives in Ext4Disk.c; this file
  is the EDK2 driver-model/file-protocol plumbing around it.

  Read-only is a hard design choice, not a missing feature: Write/Delete/
  SetInfo/create-on-Open all return EFI_WRITE_PROTECTED. A bug in a write
  path on a filesystem this complex is how you turn "can't boot Fedora"
  into "ext4 corruption on the only copy of the rootfs" - not worth it for
  what this driver is for (reading a kernel/initramfs/config at boot time).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Ext4Dxe.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Guid/FileInfo.h>
#include <Guid/FileSystemInfo.h>
#include <Guid/FileSystemVolumeLabelInfo.h>

STATIC EFI_FILE_PROTOCOL  mExt4FileProtocolTemplate;

//
// ---------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------
//

STATIC
EFI_STATUS
Ext4Utf16ToAscii (
  IN  CONST CHAR16  *Src,
  OUT CHAR8         **AsciiOut
  )
{
  UINTN   Len;
  CHAR8   *Out;
  UINTN   Index;

  Len = StrLen (Src);
  Out = AllocatePool (Len + 1);
  if (Out == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  for (Index = 0; Index < Len; Index++) {
    CHAR16  C;

    C = Src[Index];
    //
    // ASCII-only path component matching. Filenames on the boot-relevant
    // partitions here (kernel, initramfs, /boot/*, bootloader configs) are
    // ASCII; a non-ASCII byte in a path component just won't match
    // anything and the lookup fails with EFI_NOT_FOUND, same as a typo.
    //
    Out[Index] = (C < 0x80) ? (CHAR8)C : '?';
  }

  Out[Len]  = '\0';
  *AsciiOut = Out;
  return EFI_SUCCESS;
}

//
// Resolve an ASCII path (components separated by '\\' or '/', possibly
// with "." and ".." components) starting at StartInode, to a final inode.
// Fills OutInode with that inode's on-disk contents.
//
STATIC
EFI_STATUS
Ext4ResolvePath (
  IN  EXT4_VOLUME  *Volume,
  IN  UINT32       StartInodeNum,
  IN  CONST CHAR8  *Path,
  OUT UINT32       *OutInodeNum,
  OUT EXT4_INODE   *OutInode
  )
{
  UINT32      CurInodeNum;
  EXT4_INODE   CurInode;
  EFI_STATUS   Status;
  CONST CHAR8  *P;

  if ((Path[0] == '\\') || (Path[0] == '/')) {
    CurInodeNum = EXT4_ROOT_INO;
  } else {
    CurInodeNum = StartInodeNum;
  }

  Status = Ext4ReadInode (Volume, CurInodeNum, &CurInode);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  P = Path;
  while (*P != '\0') {
    CONST CHAR8  *CompStart;
    UINTN        CompLen;
    UINT32       NextInodeNum;
    EXT4_INODE    NextInode;

    while ((*P == '\\') || (*P == '/')) {
      P++;
    }

    if (*P == '\0') {
      break;
    }

    CompStart = P;
    while ((*P != '\0') && (*P != '\\') && (*P != '/')) {
      P++;
    }

    CompLen = (UINTN)(P - CompStart);

    if ((CompLen == 1) && (CompStart[0] == '.')) {
      continue;
    }

    if ((CurInode.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) {
      return EFI_NOT_FOUND;
    }

    if ((CompLen == 2) && (CompStart[0] == '.') && (CompStart[1] == '.')) {
      Status = Ext4LookupInDir (Volume, &CurInode, "..", 2, &NextInodeNum, NULL);
    } else {
      Status = Ext4LookupInDir (Volume, &CurInode, CompStart, CompLen, &NextInodeNum, NULL);
    }

    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = Ext4ReadInode (Volume, NextInodeNum, &NextInode);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    CurInodeNum = NextInodeNum;
    CurInode    = NextInode;
  }

  *OutInodeNum = CurInodeNum;
  CopyMem (OutInode, &CurInode, sizeof (CurInode));
  return EFI_SUCCESS;
}

//
// ---------------------------------------------------------------------
// EFI_FILE_PROTOCOL
// ---------------------------------------------------------------------
//

STATIC
EFI_STATUS
Ext4NewOpenFile (
  IN  EXT4_VOLUME        *Volume,
  IN  UINT32             InodeNum,
  IN  CONST EXT4_INODE   *Inode,
  IN  CONST CHAR16       *Name OPTIONAL,
  OUT EXT4_OPEN_FILE     **OutFile
  )
{
  EXT4_OPEN_FILE  *F;

  F = AllocateZeroPool (sizeof (*F));
  if (F == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  F->Signature   = EXT4_OPEN_FILE_SIGNATURE;
  F->Volume      = Volume;
  F->InodeNum    = InodeNum;
  CopyMem (&F->Inode, Inode, sizeof (F->Inode));
  F->IsDirectory = ((Inode->i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR);
  F->File        = mExt4FileProtocolTemplate;

  if (Name != NULL) {
    UINTN  Len;

    Len     = StrLen (Name);
    F->Name = AllocatePool ((Len + 1) * sizeof (CHAR16));
    if (F->Name == NULL) {
      FreePool (F);
      return EFI_OUT_OF_RESOURCES;
    }

    CopyMem (F->Name, Name, (Len + 1) * sizeof (CHAR16));
  } else {
    F->Name = AllocateZeroPool (sizeof (CHAR16));
  }

  Volume->OpenFiles++;
  *OutFile = F;
  return EFI_SUCCESS;
}

STATIC
CHAR16 *
Ext4LastPathComponent (
  IN CONST CHAR16  *Path
  )
{
  CONST CHAR16  *Last;
  CONST CHAR16  *P;

  Last = Path;
  for (P = Path; *P != L'\0'; P++) {
    if ((*P == L'\\') || (*P == L'/')) {
      Last = P + 1;
    }
  }

  return (CHAR16 *)((*Last == L'\0') ? Path : Last);
}

EFI_STATUS
EFIAPI
Ext4FileOpen (
  IN  EFI_FILE_PROTOCOL  *This,
  OUT EFI_FILE_PROTOCOL  **NewHandle,
  IN  CHAR16             *FileName,
  IN  UINT64             OpenMode,
  IN  UINT64             Attributes
  )
{
  EXT4_OPEN_FILE  *Parent;
  CHAR8           *AsciiPath;
  UINT32          InodeNum;
  EXT4_INODE      Inode;
  EFI_STATUS      Status;
  EXT4_OPEN_FILE  *NewFile;

  if ((This == NULL) || (NewHandle == NULL) || (FileName == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (OpenMode != EFI_FILE_MODE_READ) {
    return EFI_WRITE_PROTECTED;
  }

  Parent = EXT4_OPEN_FILE_FROM_THIS (This);

  if (FileName[0] == L'\0') {
    Status = Ext4NewOpenFile (Parent->Volume, Parent->InodeNum, &Parent->Inode, L"", &NewFile);
    if (!EFI_ERROR (Status)) {
      *NewHandle = &NewFile->File;
    }

    return Status;
  }

  Status = Ext4Utf16ToAscii (FileName, &AsciiPath);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Ext4ResolvePath (Parent->Volume, Parent->InodeNum, AsciiPath, &InodeNum, &Inode);
  FreePool (AsciiPath);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Inode.i_mode & EXT4_S_IFMT) == EXT4_S_IFLNK) {
    //
    // Symlinks are not resolved by this driver (no loop/visited-set
    // protection implemented, and the boot files this exists for are not
    // symlinked). Report them as not-found rather than returning raw link
    // target bytes as if they were file content.
    //
    return EFI_NOT_FOUND;
  }

  Status = Ext4NewOpenFile (Parent->Volume, InodeNum, &Inode, Ext4LastPathComponent (FileName), &NewFile);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *NewHandle = &NewFile->File;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Ext4FileClose (
  IN EFI_FILE_PROTOCOL  *This
  )
{
  EXT4_OPEN_FILE  *F;

  F = EXT4_OPEN_FILE_FROM_THIS (This);
  F->Volume->OpenFiles--;
  if (F->Name != NULL) {
    FreePool (F->Name);
  }

  FreePool (F);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Ext4FileDelete (
  IN EFI_FILE_PROTOCOL  *This
  )
{
  Ext4FileClose (This);
  return EFI_WARN_DELETE_FAILURE;
}

EFI_STATUS
EFIAPI
Ext4FileRead (
  IN     EFI_FILE_PROTOCOL  *This,
  IN OUT UINTN              *BufferSize,
  OUT    VOID                *Buffer
  )
{
  EXT4_OPEN_FILE  *F;
  EFI_STATUS      Status;

  if ((This == NULL) || (BufferSize == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  F = EXT4_OPEN_FILE_FROM_THIS (This);

  if (F->IsDirectory) {
    CHAR8                NameA[EXT4_NAME_MAX + 1];
    UINTN                 NameLen;
    UINT32                ChildInodeNum;
    UINT8                 ChildFileType;
    EXT4_INODE             ChildInode;
    EFI_FILE_INFO          *Info;
    UINTN                   NeededSize;
    UINTN                   NameChars;

    Status = Ext4DirNext (F->Volume, &F->Inode, &F->DirCursor, NameA, &NameLen, &ChildInodeNum, &ChildFileType);
    if (Status == EFI_NOT_FOUND) {
      *BufferSize = 0; // end of directory
      return EFI_SUCCESS;
    }

    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = Ext4ReadInode (F->Volume, ChildInodeNum, &ChildInode);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    NameChars  = NameLen;
    NeededSize = SIZE_OF_EFI_FILE_INFO + ((NameChars + 1) * sizeof (CHAR16));
    if (*BufferSize < NeededSize) {
      *BufferSize = NeededSize;
      return EFI_BUFFER_TOO_SMALL;
    }

    Info = (EFI_FILE_INFO *)Buffer;
    ZeroMem (Info, NeededSize);
    Info->Size         = NeededSize;
    Info->FileSize     = Ext4InodeSize (&ChildInode);
    Info->PhysicalSize = (UINT64)ChildInode.i_blocks_lo * 512;
    Info->Attribute    = EFI_FILE_READ_ONLY;
    if ((ChildInode.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) {
      Info->Attribute |= EFI_FILE_DIRECTORY;
    }

    Ext4EpochToEfiTime (ChildInode.i_atime, &Info->LastAccessTime);
    Ext4EpochToEfiTime (ChildInode.i_mtime, &Info->ModificationTime);
    Ext4EpochToEfiTime (ChildInode.i_ctime, &Info->CreateTime);

    {
      UINTN  I;

      for (I = 0; I < NameChars; I++) {
        Info->FileName[I] = (CHAR16)NameA[I];
      }

      Info->FileName[NameChars] = L'\0';
    }

    *BufferSize = NeededSize;
    return EFI_SUCCESS;
  }

  Status = Ext4ReadFileData (F->Volume, &F->Inode, F->Position, BufferSize, Buffer);
  if (!EFI_ERROR (Status)) {
    F->Position += *BufferSize;
  }

  return Status;
}

EFI_STATUS
EFIAPI
Ext4FileWrite (
  IN     EFI_FILE_PROTOCOL  *This,
  IN OUT UINTN              *BufferSize,
  IN     VOID                *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

EFI_STATUS
EFIAPI
Ext4FileSetPosition (
  IN EFI_FILE_PROTOCOL  *This,
  IN UINT64             Position
  )
{
  EXT4_OPEN_FILE  *F;

  F = EXT4_OPEN_FILE_FROM_THIS (This);

  if (F->IsDirectory) {
    if (Position != 0) {
      return EFI_UNSUPPORTED;
    }

    F->DirCursor = 0;
    return EFI_SUCCESS;
  }

  if (Position == MAX_UINT64) {
    Position = Ext4InodeSize (&F->Inode);
  }

  F->Position = Position;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Ext4FileGetPosition (
  IN  EFI_FILE_PROTOCOL  *This,
  OUT UINT64             *Position
  )
{
  EXT4_OPEN_FILE  *F;

  F = EXT4_OPEN_FILE_FROM_THIS (This);
  if (F->IsDirectory) {
    return EFI_UNSUPPORTED;
  }

  *Position = F->Position;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
Ext4FillFileInfo (
  IN  EXT4_OPEN_FILE  *F,
  IN OUT UINTN        *BufferSize,
  OUT VOID             *Buffer
  )
{
  UINTN          NameChars;
  UINTN          NeededSize;
  EFI_FILE_INFO  *Info;

  NameChars  = (F->Name != NULL) ? StrLen (F->Name) : 0;
  NeededSize = SIZE_OF_EFI_FILE_INFO + ((NameChars + 1) * sizeof (CHAR16));
  if (*BufferSize < NeededSize) {
    *BufferSize = NeededSize;
    return EFI_BUFFER_TOO_SMALL;
  }

  Info = (EFI_FILE_INFO *)Buffer;
  ZeroMem (Info, NeededSize);
  Info->Size         = NeededSize;
  Info->FileSize     = Ext4InodeSize (&F->Inode);
  Info->PhysicalSize = (UINT64)F->Inode.i_blocks_lo * 512;
  Info->Attribute    = EFI_FILE_READ_ONLY;
  if (F->IsDirectory) {
    Info->Attribute |= EFI_FILE_DIRECTORY;
  }

  Ext4EpochToEfiTime (F->Inode.i_atime, &Info->LastAccessTime);
  Ext4EpochToEfiTime (F->Inode.i_mtime, &Info->ModificationTime);
  Ext4EpochToEfiTime (F->Inode.i_ctime, &Info->CreateTime);

  if (NameChars != 0) {
    CopyMem (Info->FileName, F->Name, (NameChars + 1) * sizeof (CHAR16));
  }

  *BufferSize = NeededSize;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Ext4FileGetInfo (
  IN     EFI_FILE_PROTOCOL  *This,
  IN     EFI_GUID            *InformationType,
  IN OUT UINTN                *BufferSize,
  OUT    VOID                  *Buffer
  )
{
  EXT4_OPEN_FILE  *F;

  F = EXT4_OPEN_FILE_FROM_THIS (This);

  if (CompareGuid (InformationType, &gEfiFileInfoGuid)) {
    return Ext4FillFileInfo (F, BufferSize, Buffer);
  }

  if (CompareGuid (InformationType, &gEfiFileSystemInfoGuid)) {
    EFI_FILE_SYSTEM_INFO  *FsInfo;
    UINTN                   NameChars;
    UINTN                   NeededSize;
    UINTN                   I;

    NameChars  = AsciiStrLen (F->Volume->VolumeLabel);
    NeededSize = SIZE_OF_EFI_FILE_SYSTEM_INFO + ((NameChars + 1) * sizeof (CHAR16));
    if (*BufferSize < NeededSize) {
      *BufferSize = NeededSize;
      return EFI_BUFFER_TOO_SMALL;
    }

    FsInfo = (EFI_FILE_SYSTEM_INFO *)Buffer;
    ZeroMem (FsInfo, NeededSize);
    FsInfo->Size        = NeededSize;
    FsInfo->ReadOnly     = TRUE;
    FsInfo->VolumeSize   = F->Volume->BlocksCount * F->Volume->BlockSize;
    FsInfo->FreeSpace    = F->Volume->FreeBlocksCount * F->Volume->BlockSize;
    FsInfo->BlockSize    = F->Volume->BlockSize;

    for (I = 0; I < NameChars; I++) {
      FsInfo->VolumeLabel[I] = (CHAR16)F->Volume->VolumeLabel[I];
    }

    FsInfo->VolumeLabel[NameChars] = L'\0';
    *BufferSize = NeededSize;
    return EFI_SUCCESS;
  }

  if (CompareGuid (InformationType, &gEfiFileSystemVolumeLabelInfoIdGuid)) {
    UINTN   NameChars;
    UINTN   NeededSize;
    UINTN   I;
    EFI_FILE_SYSTEM_VOLUME_LABEL  *Lbl;

    NameChars  = AsciiStrLen (F->Volume->VolumeLabel);
    NeededSize = sizeof (EFI_FILE_SYSTEM_VOLUME_LABEL) + (NameChars * sizeof (CHAR16));
    if (*BufferSize < NeededSize) {
      *BufferSize = NeededSize;
      return EFI_BUFFER_TOO_SMALL;
    }

    Lbl = (EFI_FILE_SYSTEM_VOLUME_LABEL *)Buffer;
    for (I = 0; I < NameChars; I++) {
      Lbl->VolumeLabel[I] = (CHAR16)F->Volume->VolumeLabel[I];
    }

    Lbl->VolumeLabel[NameChars] = L'\0';
    *BufferSize                 = NeededSize;
    return EFI_SUCCESS;
  }

  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
Ext4FileSetInfo (
  IN EFI_FILE_PROTOCOL  *This,
  IN EFI_GUID            *InformationType,
  IN UINTN                BufferSize,
  IN VOID                 *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

EFI_STATUS
EFIAPI
Ext4FileFlush (
  IN EFI_FILE_PROTOCOL  *This
  )
{
  return EFI_SUCCESS; // nothing is ever dirtied; trivially "flushed"
}

STATIC EFI_FILE_PROTOCOL  mExt4FileProtocolTemplate = {
  EFI_FILE_PROTOCOL_REVISION,
  Ext4FileOpen,
  Ext4FileClose,
  Ext4FileDelete,
  Ext4FileRead,
  Ext4FileWrite,
  Ext4FileGetPosition,
  Ext4FileSetPosition,
  Ext4FileGetInfo,
  Ext4FileSetInfo,
  Ext4FileFlush,
  NULL, // OpenEx
  NULL, // ReadEx
  NULL, // WriteEx
  NULL  // FlushEx
};

//
// ---------------------------------------------------------------------
// EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
// ---------------------------------------------------------------------
//

EFI_STATUS
EFIAPI
Ext4OpenVolume (
  IN  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *This,
  OUT EFI_FILE_PROTOCOL                **Root
  )
{
  EXT4_VOLUME     *Volume;
  EXT4_INODE       RootInode;
  EFI_STATUS       Status;
  EXT4_OPEN_FILE  *F;

  Volume = EXT4_VOLUME_FROM_FS (This);

  Status = Ext4ReadInode (Volume, EXT4_ROOT_INO, &RootInode);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((RootInode.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) {
    return EFI_VOLUME_CORRUPTED;
  }

  Status = Ext4NewOpenFile (Volume, EXT4_ROOT_INO, &RootInode, L"\\", &F);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Root = &F->File;
  return EFI_SUCCESS;
}

//
// ---------------------------------------------------------------------
// EFI_DRIVER_BINDING_PROTOCOL
// ---------------------------------------------------------------------
//

EFI_STATUS
EFIAPI
Ext4DriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath OPTIONAL
  )
{
  EFI_STATUS              Status;
  EFI_DISK_IO_PROTOCOL     *DiskIo;
  EFI_BLOCK_IO_PROTOCOL    *BlockIo;
  UINT16                   Magic;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiDiskIoProtocolGuid,
                  (VOID **)&DiskIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **)&BlockIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    goto CloseDiskIo;
  }

  if (BlockIo->Media->RemovableMedia && !BlockIo->Media->MediaPresent) {
    Status = EFI_NO_MEDIA;
    goto CloseDiskIo;
  }

  Status = DiskIo->ReadDisk (
                      DiskIo,
                      BlockIo->Media->MediaId,
                      EXT4_SUPERBLOCK_OFFSET + OFFSET_OF (EXT4_SUPERBLOCK, s_magic),
                      sizeof (Magic),
                      &Magic
                      );
  if (!EFI_ERROR (Status) && (Magic != EXT4_SUPER_MAGIC)) {
    Status = EFI_UNSUPPORTED;
  }

CloseDiskIo:
  gBS->CloseProtocol (ControllerHandle, &gEfiDiskIoProtocolGuid, This->DriverBindingHandle, ControllerHandle);
  return Status;
}

EFI_STATUS
EFIAPI
Ext4DriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath OPTIONAL
  )
{
  EFI_STATUS    Status;
  EXT4_VOLUME  *Volume;

  Volume = AllocateZeroPool (sizeof (*Volume));
  if (Volume == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiDiskIoProtocolGuid,
                  (VOID **)&Volume->DiskIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    goto FreeVolume;
  }

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **)&Volume->BlockIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    goto CloseDiskIo;
  }

  Volume->Handle = ControllerHandle;

  Status = Ext4MountVolume (Volume);
  if (EFI_ERROR (Status)) {
    goto CloseDiskIo;
  }

  Volume->Fs.Revision    = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
  Volume->Fs.OpenVolume  = Ext4OpenVolume;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ControllerHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  &Volume->Fs,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    if (Volume->GroupDescs != NULL) {
      FreePool (Volume->GroupDescs);
    }

    goto CloseDiskIo;
  }

  DEBUG ((
    DEBUG_INFO,
    "Ext4Dxe: mounted ext4 volume \"%a\" (blocksize=%u, %Lu blocks)\n",
    Volume->VolumeLabel,
    Volume->BlockSize,
    Volume->BlocksCount
    ));

  return EFI_SUCCESS;

CloseDiskIo:
  gBS->CloseProtocol (ControllerHandle, &gEfiDiskIoProtocolGuid, This->DriverBindingHandle, ControllerHandle);
FreeVolume:
  FreePool (Volume);
  return Status;
}

EFI_STATUS
EFIAPI
Ext4DriverBindingStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer OPTIONAL
  )
{
  EFI_STATUS                         Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EXT4_VOLUME                        *Volume;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&Fs,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Volume = EXT4_VOLUME_FROM_FS (Fs);
  if (Volume->OpenFiles != 0) {
    return EFI_ACCESS_DENIED;
  }

  Status = gBS->UninstallMultipleProtocolInterfaces (
                  ControllerHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  &Volume->Fs,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  gBS->CloseProtocol (ControllerHandle, &gEfiDiskIoProtocolGuid, This->DriverBindingHandle, ControllerHandle);

  if (Volume->GroupDescs != NULL) {
    FreePool (Volume->GroupDescs);
  }

  FreePool (Volume);
  return EFI_SUCCESS;
}

EFI_DRIVER_BINDING_PROTOCOL  gExt4DriverBinding = {
  Ext4DriverBindingSupported,
  Ext4DriverBindingStart,
  Ext4DriverBindingStop,
  0x10,
  NULL,
  NULL
};

EFI_STATUS
EFIAPI
Ext4DriverEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EfiLibInstallDriverBindingComponentName2 (
           ImageHandle,
           SystemTable,
           &gExt4DriverBinding,
           ImageHandle,
           NULL,
           NULL
           );
}
