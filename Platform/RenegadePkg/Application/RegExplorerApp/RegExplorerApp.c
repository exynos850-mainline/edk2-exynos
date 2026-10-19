/** @file
  A145F read-only register/eFuse dump utility.

  Phone controls do not depend on GenericKeypadDeviceDxe: this app polls the
  A145F volume-key GPIOs directly. VolUp moves up; a VolDown tap moves down;
  holding VolDown for 700 ms selects. USB/UART keyboards also work.

  Dumps are saved as A145F00.TXT, A145F01.TXT, ... to a selected removable
  volume exposed by EFI_SIMPLE_FILE_SYSTEM_PROTOCOL. The app never writes to
  non-removable eMMC. An ext4 card needs a loaded *writable* ext4 filesystem
  driver; the stock FDF in this repository includes FAT only.

  MMIO reads and Samsung SMC reads are diagnostic and can hang on unclocked
  blocks. The ASV SMC call is separately confirmed because the function ID is
  not verified against every installed secure-monitor firmware.
**/

#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/SafeMmioLib.h>
#include <Library/UefiLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/BaseLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>

// Local copy of the standard EFI_BLOCK_IO_PROTOCOL GUID. This avoids an
// unresolved external gEfiBlockIoProtocolGuid in this platform's current
// EDK2 build configuration while retaining the standard protocol value.
STATIC EFI_GUID mRegExplorerBlockIoProtocolGuid = {
  0x964E5B21,
  0x6459,
  0x11D2,
  { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }
};

#ifndef SMC_ARG_SIGN_EXTEND
#define SMC_ARG_SIGN_EXTEND  1
#endif

#define SMC_CMD_REG          ((UINTN)(INTN)(-101))
#define CHIPID_BASE          0x10000000UL
#define PMU_ALIVE_BASE       0x11860000UL
#define TMU_BASE             0x10070000UL
#define ADC_BASE             0x11C40000UL
#define ASV_TABLE_BASE       0x10009000UL
#define ASV_TABLE_WORDS      4

/* From KeypadDeviceImplLib.c: GPA0-7 is VolUp, GPA1-0 is VolDown. */
#define GPIO_BASE            0x11850000UL
#define GPIO_CON_OFFSET      0x0
#define GPIO_DAT_OFFSET      0x4
#define VOL_UP_BANK_OFFSET   0x00
#define VOL_UP_PIN           7
#define VOL_DOWN_BANK_OFFSET 0x20
#define VOL_DOWN_PIN         0
#define INPUT_POLL_US        10000
#define INPUT_POLL_MS        10
#define INPUT_DEBOUNCE_MS    30
#define SELECT_HOLD_MS       700
#define GPIO_KEY_SUPPRESS_MS 400
#define MAX_FS_CANDIDATES    32
#define DUMP_BUFFER_SIZE     32768

STATIC UINT32  mSavedUpCon;
STATIC UINT32  mSavedDownCon;
STATIC BOOLEAN mGpioInitialized;

STATIC CHAR8   mDumpBuffer[DUMP_BUFFER_SIZE];
STATIC UINTN   mDumpLength;

STATIC CONST CHAR16 *mItemText[] = {
  L"CHIPID        (0x10000000, 0x40 words)",
  L"PMU_ALIVE     (0x11860000, first 0x200 bytes)",
  L"TMU           (0x10070000, 0x80 words)",
  L"ADC           (0x11C40000, 0x40 words)",
  L"ASV/eFuse via SMC (0x10009000) [confirmation required]",
  L"Dump ALL four MMIO blocks to a file",
  L"Back to boot menu"
};

#define ITEM_CHIPID  0
#define ITEM_PMU     1
#define ITEM_TMU     2
#define ITEM_ADC     3
#define ITEM_ASV     4
#define ITEM_ALL     5
#define ITEM_BACK    6
#define ITEM_COUNT   7

typedef enum {
  InputNone,
  InputUp,
  InputDown,
  InputSelect,
  InputBack,
  InputKeyboard
} INPUT_ACTION;

typedef struct {
  EFI_HANDLE Handle;
  UINT64     SizeMiB;
} FS_CANDIDATE;

STATIC EFI_INPUT_KEY  mLastKey;
STATIC UINTN          mNowMs;
STATIC UINTN          mLastGpioActivityMs;
STATIC BOOLEAN        mUpCandidate;
STATIC BOOLEAN        mDownCandidate;
STATIC BOOLEAN        mUpStable;
STATIC BOOLEAN        mDownStable;
STATIC UINTN          mUpDebounceMs;
STATIC UINTN          mDownDebounceMs;
STATIC UINTN          mDownPressedAtMs;
STATIC BOOLEAN        mDownLongHandled;
STATIC BOOLEAN        mIgnoreDownUntilRelease;

STATIC
VOID
AppendDump (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Marker;
  UINTN    Remaining;
  UINTN    Written;

  if (mDumpLength >= sizeof (mDumpBuffer) - 1) {
    return;
  }

  Remaining = sizeof (mDumpBuffer) - mDumpLength;
  VA_START (Marker, Format);
  Written = AsciiVSPrint (mDumpBuffer + mDumpLength, Remaining, Format, Marker);
  VA_END (Marker);

  if (Written >= Remaining) {
    mDumpLength = sizeof (mDumpBuffer) - 1;
    mDumpBuffer[mDumpLength] = '\0';
  } else {
    mDumpLength += Written;
  }
}

STATIC
VOID
BeginDump (
  IN CONST CHAR8  *Title
  )
{
  mDumpLength      = 0;
  mDumpBuffer[0]   = '\0';
  AppendDump ("A145F read-only register dump\r\n");
  AppendDump ("%a\r\n\r\n", Title);
}

STATIC
BOOLEAN
ReadVolumeUpPressed (
  VOID
  )
{
  UINT32  Value;

  Value = MmioRead32 (GPIO_BASE + VOL_UP_BANK_OFFSET + GPIO_DAT_OFFSET);
  return ((Value & (1U << VOL_UP_PIN)) == 0);
}

STATIC
BOOLEAN
ReadVolumeDownPressed (
  VOID
  )
{
  UINT32  Value;

  Value = MmioRead32 (GPIO_BASE + VOL_DOWN_BANK_OFFSET + GPIO_DAT_OFFSET);
  return ((Value & (1U << VOL_DOWN_PIN)) == 0);
}

STATIC
VOID
InitializeInput (
  VOID
  )
{
  BOOLEAN  UpPressed;
  BOOLEAN  DownPressed;

  if (mGpioInitialized) {
    return;
  }

  mSavedUpCon = MmioRead32 (GPIO_BASE + VOL_UP_BANK_OFFSET + GPIO_CON_OFFSET);
  mSavedDownCon = MmioRead32 (GPIO_BASE + VOL_DOWN_BANK_OFFSET + GPIO_CON_OFFSET);

  /* Set GPA0-7 and GPA1-0 to GPIO input; don't touch pull configuration. */
  MmioAnd32 (
    GPIO_BASE + VOL_UP_BANK_OFFSET + GPIO_CON_OFFSET,
    (UINT32)~(0xFU << (VOL_UP_PIN * 4))
    );
  MmioAnd32 (
    GPIO_BASE + VOL_DOWN_BANK_OFFSET + GPIO_CON_OFFSET,
    (UINT32)~(0xFU << (VOL_DOWN_PIN * 4))
    );

  UpPressed   = ReadVolumeUpPressed ();
  DownPressed = ReadVolumeDownPressed ();
  mUpCandidate       = UpPressed;
  mDownCandidate     = DownPressed;
  mUpStable          = UpPressed;
  mDownStable        = DownPressed;
  mUpDebounceMs      = INPUT_DEBOUNCE_MS;
  mDownDebounceMs    = INPUT_DEBOUNCE_MS;
  mDownPressedAtMs   = 0;
  mDownLongHandled   = DownPressed;
  mIgnoreDownUntilRelease = DownPressed;
  mNowMs             = 0;
  mLastGpioActivityMs = 0;
  mGpioInitialized   = TRUE;
}

STATIC
VOID
RestoreInput (
  VOID
  )
{
  if (!mGpioInitialized) {
    return;
  }

  MmioWrite32 (GPIO_BASE + VOL_UP_BANK_OFFSET + GPIO_CON_OFFSET, mSavedUpCon);
  MmioWrite32 (GPIO_BASE + VOL_DOWN_BANK_OFFSET + GPIO_CON_OFFSET, mSavedDownCon);
  mGpioInitialized = FALSE;
}

STATIC
VOID
DebounceButton (
  IN BOOLEAN  Raw,
  IN OUT BOOLEAN *Candidate,
  IN OUT BOOLEAN *Stable,
  IN OUT UINTN   *StableMs
  )
{
  if (Raw != *Candidate) {
    *Candidate = Raw;
    *StableMs  = 0;
    return;
  }

  if (*StableMs < INPUT_DEBOUNCE_MS) {
    *StableMs += INPUT_POLL_MS;
  }

  if ((*StableMs >= INPUT_DEBOUNCE_MS) && (*Stable != *Candidate)) {
    *Stable = *Candidate;
  }
}

/**
  Return an action without blocking on EFI_SIMPLE_TEXT_INPUT. Volume keys are
  polled directly because the generic keypad DXE has caused synchronous faults
  on this A145F port.
**/
STATIC
INPUT_ACTION
PollInput (
  VOID
  )
{
  BOOLEAN      OldUp;
  BOOLEAN      OldDown;
  BOOLEAN      RawUp;
  BOOLEAN      RawDown;
  EFI_STATUS   Status;
  INPUT_ACTION GpioAction;

  InitializeInput ();
  gBS->Stall (INPUT_POLL_US);
  mNowMs += INPUT_POLL_MS;

  OldUp   = mUpStable;
  OldDown = mDownStable;
  RawUp   = ReadVolumeUpPressed ();
  RawDown = ReadVolumeDownPressed ();

  DebounceButton (RawUp, &mUpCandidate, &mUpStable, &mUpDebounceMs);
  DebounceButton (RawDown, &mDownCandidate, &mDownStable, &mDownDebounceMs);
  GpioAction = InputNone;

  if (mUpStable != OldUp) {
    mLastGpioActivityMs = mNowMs;
    if (mUpStable) {
      GpioAction = InputUp;
    }
  }

  if (mDownStable != OldDown) {
    mLastGpioActivityMs = mNowMs;
    if (mDownStable) {
      mDownPressedAtMs = mNowMs;
      mDownLongHandled = FALSE;
    } else {
      if (mIgnoreDownUntilRelease) {
        mIgnoreDownUntilRelease = FALSE;
        mDownLongHandled = FALSE;
      } else if (!mDownLongHandled) {
        GpioAction = InputDown;
      }
    }
  }

  if (mDownStable && !mDownLongHandled && !mIgnoreDownUntilRelease &&
      ((mNowMs - mDownPressedAtMs) >= SELECT_HOLD_MS)) {
    mDownLongHandled = TRUE;
    mLastGpioActivityMs = mNowMs;
    return InputSelect;
  }

  if (GpioAction != InputNone) {
    return GpioAction;
  }

  Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &mLastKey);
  if (EFI_ERROR (Status)) {
    return InputNone;
  }

  /* Ignore duplicate arrows emitted by a keypad driver for the same GPIO press. */
  if (((mLastKey.ScanCode == SCAN_UP) || (mLastKey.ScanCode == SCAN_DOWN)) &&
      ((mNowMs - mLastGpioActivityMs) < GPIO_KEY_SUPPRESS_MS)) {
    return InputNone;
  }

  return InputKeyboard;
}

STATIC
VOID
PressAnyKey (
  VOID
  )
{
  Print (L"\n  Press a key/button to return...\n");
  while (PollInput () == InputNone) {
    // PollInput services GPIO buttons and keyboard input.
  }
}

//
// Some words (e.g. CHIPID+0x14) raise a synchronous external abort when read
// from the non-secure world, which used to take the whole firmware down.
// SafeMmioRead32() reports that as an error; unreadable words show "--------".
// A read that hangs the bus (no abort) cannot be caught, so the address is
// printed BEFORE the words are read: the last line on screen is where it hung.
//
STATIC
VOID
ReadWordStr (
  IN  UINTN  Addr,
  OUT CHAR8  Out[9]
  )
{
  UINT32  V;

  if (EFI_ERROR (SafeMmioRead32 (Addr, &V))) {
    AsciiStrCpyS (Out, 9, "--------");
  } else {
    AsciiSPrint (Out, 9, "%08x", V);
  }
}

STATIC
VOID
AppendMmioRegion (
  IN CONST CHAR8  *Title,
  IN UINTN        Base,
  IN UINTN        Count
  )
{
  UINTN   Index;
  CHAR8   W[4][9];
  UINTN   Addr;

  AppendDump ("==== %a @ 0x%010lx ====\r\n", Title, (UINT64)Base);
  for (Index = 0; Index < Count; Index += 4) {
    Addr = Base + Index * sizeof (UINT32);
    ReadWordStr (Addr + 0, W[0]);
    ReadWordStr (Addr + 4, W[1]);
    ReadWordStr (Addr + 8, W[2]);
    ReadWordStr (Addr + 12, W[3]);
    AppendDump (
      "0x%010lx: %a %a %a %a\r\n",
      (UINT64)Addr, W[0], W[1], W[2], W[3]
      );
  }
  AppendDump ("\r\n");
}

STATIC
VOID
DumpMmioRegion (
  IN CONST CHAR16  *Title,
  IN CONST CHAR8   *AsciiTitle,
  IN UINTN         Base,
  IN UINTN         Count
  )
{
  UINTN   Index;
  CHAR8   W[4][9];
  UINTN   Addr;

  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"==== %s ====\n\n", Title);
  AppendDump ("==== %a @ 0x%010lx ====\r\n", AsciiTitle, (UINT64)Base);

  for (Index = 0; Index < Count; Index += 4) {
    Addr = Base + Index * sizeof (UINT32);
    Print (L"  0x%010lx:", (UINT64)Addr);   // before the reads: marks where a hang happens
    ReadWordStr (Addr + 0, W[0]);
    ReadWordStr (Addr + 4, W[1]);
    ReadWordStr (Addr + 8, W[2]);
    ReadWordStr (Addr + 12, W[3]);
    Print (L" %a %a %a %a\n", W[0], W[1], W[2], W[3]);
    AppendDump (
      "0x%010lx: %a %a %a %a\r\n",
      (UINT64)Addr, W[0], W[1], W[2], W[3]
      );
  }
  AppendDump ("\r\n");
}

STATIC
UINTN
SmcReadSfr (
  IN  UINTN   Addr,
  OUT UINT32  *Value
  )
{
  ARM_SMC_ARGS  SmcArgs = { 0 };
  UINT32        Id;

  Id = (0x3U << 30) | (UINT32)(Addr >> 2);
  SmcArgs.Arg0 = SMC_CMD_REG;
#if SMC_ARG_SIGN_EXTEND
  SmcArgs.Arg1 = (UINTN)(INTN)(INT32)Id;
#else
  SmcArgs.Arg1 = (UINTN)Id;
#endif

  ArmCallSmc (&SmcArgs);
  if (SmcArgs.Arg0 == 0) {
    *Value = (UINT32)SmcArgs.Arg2;
  }
  return SmcArgs.Arg0;
}

STATIC
BOOLEAN
ConfirmAsvRead (
  VOID
  )
{
  INPUT_ACTION  Action;

  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"==== ASV/eFuse table via SMC_CMD_REG (read class) ====\n\n");
  Print (L"This calls the secure monitor. The function ID is taken from Samsung's\n");
  Print (L"kernel source but is not verified against every stock firmware.\n");
  Print (L"A bad function ID can hang or reset the phone (no registers are written).\n\n");
  Print (L"Hold Volume Down for 700 ms (or press Enter) to continue.\n");
  Print (L"Volume Up / Esc / Q cancels.\n");

  for (;;) {
    Action = PollInput ();
    if (Action == InputSelect) {
      return TRUE;
    }
    if ((Action == InputUp) || (Action == InputDown) || (Action == InputBack)) {
      return FALSE;
    }
    if (Action == InputKeyboard) {
      if ((mLastKey.UnicodeChar == CHAR_CARRIAGE_RETURN) ||
          (mLastKey.UnicodeChar == L' ')) {
        return TRUE;
      }
      return FALSE;
    }
  }
}

STATIC
VOID
SaveDumpToRemovableFs (
  VOID
  );

STATIC
VOID
DumpAsvTable (
  VOID
  )
{
  UINTN   Index;
  UINT32  Value;
  UINTN   Status;
  UINTN   Addr;

  if (!ConfirmAsvRead ()) {
    return;
  }

  BeginDump ("ASV/eFuse table (read-only SMC path)");
  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"==== ASV/eFuse table via SMC_CMD_REG (read class) ====\n\n");
  Print (L"  Address       Value / status\n");

  for (Index = 0; Index < ASV_TABLE_WORDS; Index++) {
    Addr  = ASV_TABLE_BASE + Index * sizeof (UINT32);
    Value = 0;
    Status = SmcReadSfr (Addr, &Value);
    if (Status == 0) {
      Print (L"  0x%010lx: %08x  (a0=0 ok)\n", (UINT64)Addr, Value);
      AppendDump ("0x%010lx: %08x (a0=0 ok)\r\n", (UINT64)Addr, Value);
    } else {
      Print (L"  0x%010lx: <failed, a0=0x%lx>\n", (UINT64)Addr, (UINT64)Status);
      AppendDump ("0x%010lx: <failed, a0=0x%lx>\r\n", (UINT64)Addr, (UINT64)Status);
    }
  }
  AppendDump ("\r\nWord 0 bitfields (asv_s5e3830.h):\r\n");
  AppendDump ("[11:8] littlecpu_asv_group [15:12] g3d_asv_group\r\n");
  AppendDump ("[19:16] mif_asv_group [23:20] int_asv_group\r\n");
  AppendDump ("[27:24] cam_disp_asv_group [31:28] cp_asv_group\r\n");
  Print (L"\nWord 0 fields: littlecpu, g3d, mif, int, cam/disp, cp ASV groups.\n");
  SaveDumpToRemovableFs ();
}

STATIC
VOID
DrawMenu (
  IN UINTN  Selected
  )
{
  UINTN  Index;

  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"==== A145F Register/eFuse Explorer (read-only) ====\n\n");
  for (Index = 0; Index < ITEM_COUNT; Index++) {
    Print (L"  %s %s\n", (Index == Selected) ? L">" : L" ", mItemText[Index]);
  }
  Print (L"\n  VolUp = up; VolDown tap = down; hold VolDown 700 ms = select.\n");
  Print (L"  Keyboard: arrows/W-S, Enter/Space, 1-6, Q/Esc.\n");
  Print (L"  Dumps are saved to a selected removable writable filesystem.\n");
}

STATIC
VOID
DrawVolumeMenu (
  IN FS_CANDIDATE  *Candidates,
  IN UINTN         CandidateCount,
  IN UINTN         Selected
  )
{
  UINTN  Index;

  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"==== Choose removable SD volume for dump ====\n\n");
  Print (L"Only removable media are shown; internal eMMC is excluded.\n");
  Print (L"A writable filesystem driver is required.\n\n");
  for (Index = 0; Index < CandidateCount; Index++) {
    Print (
      L"  %s %u. removable volume (%lu MiB)\n",
      (Index == Selected) ? L">" : L" ",
      (UINT32)(Index + 1),
      Candidates[Index].SizeMiB
      );
  }
  Print (L"\n  VolUp = up; VolDown tap = down; hold VolDown = select. Q/Esc cancels.\n");
}

STATIC
EFI_STATUS
CreateDumpFile (
  IN  EFI_HANDLE  FsHandle,
  OUT CHAR16      *SavedName,
  IN  UINTN       SavedNameChars
  )
{
  EFI_STATUS                     Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *SimpleFs;
  EFI_FILE_PROTOCOL              *Root;
  EFI_FILE_PROTOCOL              *File;
  CHAR16                         Name[16];
  UINTN                          Slot;
  UINTN                          BufferOffset;
  UINTN                          ChunkSize;

  if (SavedNameChars < 12) {
    return EFI_BUFFER_TOO_SMALL;
  }

  Status = gBS->HandleProtocol (
                  FsHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&SimpleFs
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SimpleFs->OpenVolume (SimpleFs, &Root);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  File = NULL;
  for (Slot = 0; Slot < 100; Slot++) {
    EFI_FILE_PROTOCOL  *Existing;
    EFI_STATUS         ProbeStatus;

    Name[0] = L'\\';
    Name[1] = L'A'; Name[2] = L'1'; Name[3] = L'4'; Name[4] = L'5'; Name[5] = L'F';
    Name[6] = (CHAR16)(L'0' + (Slot / 10));
    Name[7] = (CHAR16)(L'0' + (Slot % 10));
    Name[8] = L'.'; Name[9] = L'T'; Name[10] = L'X'; Name[11] = L'T'; Name[12] = L'\0';

    Existing = NULL;
    ProbeStatus = Root->Open (Root, &Existing, Name, EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR (ProbeStatus)) {
      Existing->Close (Existing);
      continue;
    }
    if (ProbeStatus != EFI_NOT_FOUND) {
      Root->Close (Root);
      return ProbeStatus;
    }

    Status = Root->Open (
                    Root,
                    &File,
                    Name,
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                    EFI_FILE_ARCHIVE
                    );
    if (EFI_ERROR (Status)) {
      Root->Close (Root);
      return Status;
    }
    break;
  }

  if (File == NULL) {
    Root->Close (Root);
    return EFI_OUT_OF_RESOURCES;
  }

  BufferOffset = 0;
  while (BufferOffset < mDumpLength) {
    ChunkSize = mDumpLength - BufferOffset;
    Status = File->Write (File, &ChunkSize, mDumpBuffer + BufferOffset);
    if (EFI_ERROR (Status) || (ChunkSize == 0)) {
      if (!EFI_ERROR (Status)) {
        Status = EFI_DEVICE_ERROR;
      }
      File->Close (File);
      Root->Close (Root);
      return Status;
    }
    BufferOffset += ChunkSize;
  }

  Status = File->Flush (File);
  File->Close (File);
  Root->Close (Root);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* Copy the filename without its initial slash for the completion message. */
  CopyMem (SavedName, Name + 1, 12 * sizeof (CHAR16));
  return EFI_SUCCESS;
}

STATIC
VOID
SaveDumpToRemovableFs (
  VOID
  )
{
  EFI_STATUS       Status;
  EFI_HANDLE       *Handles;
  UINTN            HandleCount;
  UINTN            Index;
  UINTN            CandidateCount;
  UINTN            Selected;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  FS_CANDIDATE     Candidates[MAX_FS_CANDIDATES];
  INPUT_ACTION     Action;
  BOOLEAN          Redraw;
  CHAR16           SavedName[16];

  Handles = NULL;
  HandleCount = 0;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status) || (HandleCount == 0)) {
    gST->ConOut->ClearScreen (gST->ConOut);
    Print (L"No UEFI filesystem volumes are available.\n\n");
    Print (L"This firmware currently bundles FAT support only. An ext4 SD needs\n");
    Print (L"a loaded writable ext4 driver before this app can create a file.\n");
    PressAnyKey ();
    if (!EFI_ERROR (Status) && (Handles != NULL)) {
      FreePool (Handles);
    }
    return;
  }

  CandidateCount = 0;
  for (Index = 0; Index < HandleCount; Index++) {
    BlockIo = NULL;
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &mRegExplorerBlockIoProtocolGuid,
                    (VOID **)&BlockIo
                    );
    if (EFI_ERROR (Status) || (BlockIo == NULL) || (BlockIo->Media == NULL) ||
        !BlockIo->Media->MediaPresent || !BlockIo->Media->RemovableMedia ||
        (BlockIo->Media->BlockSize == 0)) {
      continue;
    }
    if (CandidateCount >= MAX_FS_CANDIDATES) {
      break;
    }
    Candidates[CandidateCount].Handle = Handles[Index];
    Candidates[CandidateCount].SizeMiB =
      (((UINT64)BlockIo->Media->LastBlock + 1) * BlockIo->Media->BlockSize) / (1024 * 1024);
    CandidateCount++;
  }
  FreePool (Handles);

  if (CandidateCount == 0) {
    gST->ConOut->ClearScreen (gST->ConOut);
    Print (L"No removable filesystem is exposed to UEFI.\n\n");
    Print (L"The app will not write to internal eMMC.\n");
    Print (L"For ext4, a writable ext4 driver must be built into the firmware.\n");
    PressAnyKey ();
    return;
  }

  Selected = 0;
  Redraw = TRUE;
  for (;;) {
    if (Redraw) {
      DrawVolumeMenu (Candidates, CandidateCount, Selected);
      Redraw = FALSE;
    }
    Action = PollInput ();
    if (Action == InputUp) {
      Selected = (Selected == 0) ? CandidateCount - 1 : Selected - 1;
      Redraw = TRUE;
    } else if (Action == InputDown) {
      Selected = (Selected + 1) % CandidateCount;
      Redraw = TRUE;
    } else if (Action == InputSelect) {
      break;
    } else if (Action == InputBack) {
      return;
    } else if (Action == InputKeyboard) {
      if ((mLastKey.ScanCode == SCAN_ESC) || (mLastKey.UnicodeChar == L'q') ||
          (mLastKey.UnicodeChar == L'Q')) {
        return;
      } else if ((mLastKey.ScanCode == SCAN_UP) || (mLastKey.UnicodeChar == L'w') ||
                 (mLastKey.UnicodeChar == L'W')) {
        Selected = (Selected == 0) ? CandidateCount - 1 : Selected - 1;
        Redraw = TRUE;
      } else if ((mLastKey.ScanCode == SCAN_DOWN) || (mLastKey.UnicodeChar == L's') ||
                 (mLastKey.UnicodeChar == L'S')) {
        Selected = (Selected + 1) % CandidateCount;
        Redraw = TRUE;
      } else if ((mLastKey.UnicodeChar >= L'1') && (mLastKey.UnicodeChar <= L'9')) {
        UINTN  Number;
        Number = (UINTN)(mLastKey.UnicodeChar - L'1');
        if (Number < CandidateCount) {
          Selected = Number;
          break;
        }
      } else if (mLastKey.UnicodeChar == CHAR_CARRIAGE_RETURN) {
        break;
      }
    }
  }

  SavedName[0] = L'\0';
  Status = CreateDumpFile (Candidates[Selected].Handle, SavedName, sizeof (SavedName) / sizeof (SavedName[0]));
  gST->ConOut->ClearScreen (gST->ConOut);
  if (EFI_ERROR (Status)) {
    Print (L"Could not write dump to the selected volume.\n\n");
    Print (L"EFI_STATUS: %r\n\n", Status);
    Print (L"If this is ext4, the driver may be missing or read-only.\n");
  } else {
    Print (L"Dump saved successfully: %s\n\n", SavedName);
    Print (L"Bytes written: %lu\n", (UINT64)mDumpLength);
  }
  PressAnyKey ();
}

STATIC
BOOLEAN
RunItem (
  IN UINTN  Item
  )
{
  switch (Item) {
    case ITEM_CHIPID:
      BeginDump ("CHIPID");
      DumpMmioRegion (L"CHIPID @ 0x10000000", "CHIPID", CHIPID_BASE, 0x40);
      break;
    case ITEM_PMU:
      BeginDump ("PMU_ALIVE");
      DumpMmioRegion (L"PMU_ALIVE (first 0x200) @ 0x11860000", "PMU_ALIVE", PMU_ALIVE_BASE, 0x80);
      break;
    case ITEM_TMU:
      BeginDump ("TMU");
      DumpMmioRegion (L"TMU @ 0x10070000", "TMU", TMU_BASE, 0x80);
      break;
    case ITEM_ADC:
      BeginDump ("ADC");
      DumpMmioRegion (L"ADC @ 0x11C40000", "ADC", ADC_BASE, 0x40);
      break;
    case ITEM_ASV:
      DumpAsvTable ();
      return FALSE;
    case ITEM_ALL:
      BeginDump ("CHIPID + PMU_ALIVE + TMU + ADC");
      gST->ConOut->ClearScreen (gST->ConOut);
      Print (L"Dumping CHIPID, PMU_ALIVE, TMU and ADC...\n");
      AppendMmioRegion ("CHIPID", CHIPID_BASE, 0x40);
      AppendMmioRegion ("PMU_ALIVE (first 0x200 bytes)", PMU_ALIVE_BASE, 0x80);
      AppendMmioRegion ("TMU", TMU_BASE, 0x80);
      AppendMmioRegion ("ADC", ADC_BASE, 0x40);
      Print (L"Finished reading MMIO blocks.\n");
      break;
    default:
      return TRUE;
  }

  SaveDumpToRemovableFs ();
  return FALSE;
}

EFI_STATUS
EFIAPI
RegExplorerAppEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  INPUT_ACTION  Action;
  UINTN         Selected;
  BOOLEAN       Done;
  BOOLEAN       Redraw;

  Selected = 0;
  Done = FALSE;
  Redraw = TRUE;
  InitializeInput ();

  while (!Done) {
    if (Redraw) {
      DrawMenu (Selected);
      Redraw = FALSE;
    }
    Action = PollInput ();
    if (Action == InputUp) {
      Selected = (Selected == 0) ? ITEM_COUNT - 1 : Selected - 1;
      Redraw = TRUE;
    } else if (Action == InputDown) {
      Selected = (Selected + 1) % ITEM_COUNT;
      Redraw = TRUE;
    } else if (Action == InputSelect) {
      Done = RunItem (Selected);
      Redraw = TRUE;
    } else if (Action == InputBack) {
      Done = TRUE;
    } else if (Action == InputKeyboard) {
      if ((mLastKey.ScanCode == SCAN_UP) || (mLastKey.UnicodeChar == L'w') ||
          (mLastKey.UnicodeChar == L'W') || (mLastKey.UnicodeChar == L'k') ||
          (mLastKey.UnicodeChar == L'K')) {
        Selected = (Selected == 0) ? ITEM_COUNT - 1 : Selected - 1;
        Redraw = TRUE;
      } else if ((mLastKey.ScanCode == SCAN_DOWN) || (mLastKey.UnicodeChar == L's') ||
                 (mLastKey.UnicodeChar == L'S') || (mLastKey.UnicodeChar == L'j') ||
                 (mLastKey.UnicodeChar == L'J')) {
        Selected = (Selected + 1) % ITEM_COUNT;
        Redraw = TRUE;
      } else if (mLastKey.UnicodeChar == CHAR_CARRIAGE_RETURN || mLastKey.UnicodeChar == L' ') {
        Done = RunItem (Selected);
        Redraw = TRUE;
      } else if ((mLastKey.UnicodeChar >= L'1') && (mLastKey.UnicodeChar <= L'6')) {
        Done = RunItem ((UINTN)(mLastKey.UnicodeChar - L'1'));
        Redraw = TRUE;
      } else if ((mLastKey.ScanCode == SCAN_ESC) || (mLastKey.UnicodeChar == L'q') ||
                 (mLastKey.UnicodeChar == L'Q')) {
        Done = TRUE;
      }
    }
  }

  RestoreInput ();
  gST->ConOut->ClearScreen (gST->ConOut);
  return EFI_SUCCESS;
}
