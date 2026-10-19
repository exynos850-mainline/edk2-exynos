/** @file
  Read-only Exynos850 eMMC Block I/O driver.

  This is a deliberately conservative first-stage driver for the internal eMMC
  on SM-A145F/SM-A145M.  It uses the DesignWare MSHC PIO FIFO (no DMA), exposes
  a standard EFI_BLOCK_IO_PROTOCOL, and refuses all writes to internal storage.

  Hardware assumptions validated against the Exynos850 device tree:
    * eMMC DesignWare MSHC base: 0x12100000
    * CMU_CORE base:             0x12000000
    * eMMC controller clocks:    CMU_CORE + 0x20e8 / + 0x20ec

  The driver intentionally starts in 1-bit legacy mode.  It does not try to
  use HS200/HS400, DMA, or change EXT_CSD configuration bytes.
**/

#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

#define EMMC_BASE                  0x12100000UL
#define CMU_CORE_BASE              0x12000000UL
#define CMU_CORE_MMC_USER_MUX      (CMU_CORE_BASE + 0x0620)
#define CMU_CORE_MMC_ACLK_GATE     (CMU_CORE_BASE + 0x20e8)
#define CMU_CORE_MMC_SDCLK_GATE    (CMU_CORE_BASE + 0x20ec)
#define CMU_GATE_ENABLE_BIT        (1U << 21)

#define MMC_CTRL                   0x000
#define MMC_PWREN                  0x004
#define MMC_CLKDIV                 0x008
#define MMC_CLKSRC                 0x00c
#define MMC_CLKENA                 0x010
#define MMC_TMOUT                  0x014
#define MMC_CTYPE                  0x018
#define MMC_BLKSIZ                 0x01c
#define MMC_BYTCNT                 0x020
#define MMC_INTMASK                0x024
#define MMC_CMDARG                 0x028
#define MMC_CMD                    0x02c
#define MMC_RESP0                  0x030
#define MMC_RINTSTS                0x044
#define MMC_STATUS                 0x048
#define MMC_FIFOTH                 0x04c
#define MMC_VERID                  0x06c
#define MMC_UHS_REG                0x074
#define MMC_BMOD                   0x080
#define MMC_FIFO                   0x100

#define CTRL_RESET                 (1U << 0)
#define CTRL_FIFO_RESET            (1U << 1)
#define CTRL_DMA_RESET             (1U << 2)

#define CMD_START                  (1U << 31)
#define CMD_UPDATE_CLOCK           (1U << 21)
#define CMD_SEND_INIT              (1U << 15)
#define CMD_WAIT_PREV_DATA         (1U << 13)
#define CMD_DATA_EXPECTED          (1U << 9)
#define CMD_RESP_CRC               (1U << 8)
#define CMD_RESP_LONG              (1U << 7)
#define CMD_RESP_EXPECT            (1U << 6)

#define INT_CMD_DONE               (1U << 2)
#define INT_DATA_OVER              (1U << 3)
#define INT_RXDR                   (1U << 5)
#define INT_RESP_CRC_ERROR         (1U << 6)
#define INT_DATA_CRC_ERROR         (1U << 7)
#define INT_RESP_TIMEOUT           (1U << 8)
#define INT_DATA_TIMEOUT           (1U << 9)
#define INT_HOST_TIMEOUT           (1U << 10)
#define INT_FIFO_RUN_ERROR         (1U << 11)
#define INT_HARDWARE_LOCK_ERROR    (1U << 12)
#define INT_START_BIT_ERROR        (1U << 13)
#define INT_END_BIT_ERROR          (1U << 15)
#define INT_COMMAND_ERRORS         (INT_RESP_CRC_ERROR | INT_RESP_TIMEOUT | \
                                   INT_HOST_TIMEOUT | INT_HARDWARE_LOCK_ERROR | \
                                   INT_START_BIT_ERROR | INT_END_BIT_ERROR)
#define INT_DATA_ERRORS            (INT_DATA_CRC_ERROR | INT_DATA_TIMEOUT | \
                                   INT_HOST_TIMEOUT | INT_FIFO_RUN_ERROR | \
                                   INT_HARDWARE_LOCK_ERROR | INT_START_BIT_ERROR | \
                                   INT_END_BIT_ERROR)

#define STATUS_FIFO_EMPTY          (1U << 2)
#define STATUS_DATA_BUSY            (1U << 9)

#define MMC_CMD_GO_IDLE             0
#define MMC_CMD_SEND_OP_COND        1
#define MMC_CMD_ALL_SEND_CID        2
#define MMC_CMD_SET_RELATIVE_ADDR   3
#define MMC_CMD_SELECT_CARD         7
#define MMC_CMD_SEND_EXT_CSD        8

#define MMC_OCR_BUSY                (1U << 31)
#define MMC_OCR_SECTOR_MODE         (1U << 30)
#define MMC_OCR_VOLTAGE_WINDOW       0x00FF8000U
#define MMC_OCR_ARGUMENT            (MMC_OCR_SECTOR_MODE | MMC_OCR_VOLTAGE_WINDOW)
#define MMC_RCA                     1U
#define MMC_BLOCK_SIZE              512U
#define MMC_EXT_CSD_SECTOR_COUNT    212U
#define MMC_POLL_LIMIT              1000000U

STATIC EFI_BLOCK_IO_MEDIA        mMedia;
STATIC EFI_BLOCK_IO_PROTOCOL     mBlockIo;
STATIC EFI_HANDLE                mBlockIoHandle;
STATIC BOOLEAN                   mCardReady;
STATIC UINT32                    mMediaId = 0x145F8500U;
STATIC UINT8                     mExtCsd[MMC_BLOCK_SIZE];

STATIC CONST EFI_GUID mExynos850MmcDevicePathGuid = {
  0xa1458500, 0x01c0, 0x4e4d, { 0x4d, 0x43, 0x44, 0x50, 0x00, 0x00, 0x00, 0x01 }
};

typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} EXYNOS850_MMC_DEVICE_PATH;

STATIC EXYNOS850_MMC_DEVICE_PATH mDevicePath;

STATIC
UINT32
MmcRegRead (
  IN UINTN Offset
  )
{
  return MmioRead32 (EMMC_BASE + Offset);
}

STATIC
VOID
MmcRegWrite (
  IN UINTN  Offset,
  IN UINT32 Value
  )
{
  MmioWrite32 (EMMC_BASE + Offset, Value);
}

STATIC
EFI_STATUS
MmcWaitCommandStartClear (
  VOID
  )
{
  UINTN  Count;

  for (Count = 0; Count < MMC_POLL_LIMIT; Count++) {
    if ((MmcRegRead (MMC_CMD) & CMD_START) == 0) {
      if ((MmcRegRead (MMC_RINTSTS) & INT_HARDWARE_LOCK_ERROR) != 0) {
        return EFI_DEVICE_ERROR;
      }
      return EFI_SUCCESS;
    }
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
MmcUpdateClock (
  VOID
  )
{
  MmcRegWrite (MMC_RINTSTS, 0xffffffffU);
  MmcRegWrite (MMC_CMD, CMD_START | CMD_UPDATE_CLOCK | CMD_WAIT_PREV_DATA);
  return MmcWaitCommandStartClear ();
}

STATIC
EFI_STATUS
MmcSetClock (
  IN UINT32 Divisor
  )
{
  EFI_STATUS Status;

  MmcRegWrite (MMC_CLKENA, 0);
  Status = MmcUpdateClock ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmcRegWrite (MMC_CLKDIV, Divisor);
  Status = MmcUpdateClock ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmcRegWrite (MMC_CLKENA, 1);
  return MmcUpdateClock ();
}

STATIC
EFI_STATUS
MmcSendCommand (
  IN  UINT32 Index,
  IN  UINT32 Argument,
  IN  UINT32 Flags,
  OUT UINT32 *Response OPTIONAL
  )
{
  UINT32     Interrupts;
  UINTN      Count;
  EFI_STATUS Status;

  MmcRegWrite (MMC_RINTSTS, 0xffffffffU);
  MmcRegWrite (MMC_CMDARG, Argument);
  MmcRegWrite (MMC_CMD, CMD_START | (Index & 0x3fU) | Flags);

  for (Count = 0; Count < MMC_POLL_LIMIT; Count++) {
    Interrupts = MmcRegRead (MMC_RINTSTS);
    if ((Interrupts & INT_COMMAND_ERRORS) != 0) {
      MmcRegWrite (MMC_RINTSTS, Interrupts);
      return EFI_DEVICE_ERROR;
    }

    if ((Interrupts & INT_CMD_DONE) != 0) {
      if (Response != NULL) {
        *Response = MmcRegRead (MMC_RESP0);
      }
      MmcRegWrite (MMC_RINTSTS, Interrupts);
      return EFI_SUCCESS;
    }
  }

  Status = MmcWaitCommandStartClear ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Interrupts = MmcRegRead (MMC_RINTSTS);
  if ((Interrupts & INT_CMD_DONE) != 0) {
    if (Response != NULL) {
      *Response = MmcRegRead (MMC_RESP0);
    }
    MmcRegWrite (MMC_RINTSTS, Interrupts);
    return EFI_SUCCESS;
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
MmcSendInitClocks (
  VOID
  )
{
  MmcRegWrite (MMC_RINTSTS, 0xffffffffU);
  MmcRegWrite (MMC_CMD, CMD_START | CMD_SEND_INIT);
  return MmcWaitCommandStartClear ();
}

STATIC
EFI_STATUS
MmcReadDataCommand (
  IN  UINT32 Index,
  IN  UINT32 Argument,
  IN  UINT32 Flags,
  OUT VOID   *Buffer,
  IN  UINTN  BufferSize
  )
{
  EFI_STATUS  Status;
  UINT32      Interrupts;
  UINTN       WordIndex;
  UINTN       WordCount;
  UINTN       Count;
  UINT32      *Words;

  if ((Buffer == NULL) || (BufferSize == 0) || ((BufferSize & 3U) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  MmcRegWrite (MMC_BLKSIZ, (UINT32)BufferSize);
  MmcRegWrite (MMC_BYTCNT, (UINT32)BufferSize);
  MmcRegWrite (MMC_RINTSTS, 0xffffffffU);
  MmcRegWrite (MMC_CMDARG, Argument);
  MmcRegWrite (MMC_CMD, CMD_START | (Index & 0x3fU) | Flags | CMD_DATA_EXPECTED);

  // Drain the FIFO while the card is sending data.  The Exynos850 MSHC FIFO
  // is only 64 words deep; waiting for CMD_DONE first can overrun it on a
  // 512-byte transfer.

  Words = (UINT32 *)Buffer;
  WordCount = BufferSize / sizeof (UINT32);
  for (WordIndex = 0; WordIndex < WordCount; WordIndex++) {
    for (Count = 0; Count < MMC_POLL_LIMIT; Count++) {
      Interrupts = MmcRegRead (MMC_RINTSTS);
      if ((Interrupts & (INT_COMMAND_ERRORS | INT_DATA_ERRORS)) != 0) {
        MmcRegWrite (MMC_RINTSTS, Interrupts);
        return EFI_DEVICE_ERROR;
      }
      if ((MmcRegRead (MMC_STATUS) & STATUS_FIFO_EMPTY) == 0) {
        break;
      }
      if ((Interrupts & INT_DATA_OVER) != 0) {
        return EFI_DEVICE_ERROR;
      }
    }
    if (Count == MMC_POLL_LIMIT) {
      return EFI_TIMEOUT;
    }

    Words[WordIndex] = MmcRegRead (MMC_FIFO);
  }

  for (Count = 0; Count < MMC_POLL_LIMIT; Count++) {
    Interrupts = MmcRegRead (MMC_RINTSTS);
    if ((Interrupts & (INT_COMMAND_ERRORS | INT_DATA_ERRORS)) != 0) {
      MmcRegWrite (MMC_RINTSTS, Interrupts);
      return EFI_DEVICE_ERROR;
    }
    if ((Interrupts & (INT_DATA_OVER | INT_CMD_DONE)) ==
        (INT_DATA_OVER | INT_CMD_DONE)) {
      MmcRegWrite (MMC_RINTSTS, Interrupts);
      return EFI_SUCCESS;
    }
  }

  Status = MmcWaitCommandStartClear ();
  return EFI_ERROR (Status) ? Status : EFI_TIMEOUT;
}

STATIC
EFI_STATUS
MmcReadSector (
  IN  EFI_LBA Lba,
  OUT VOID    *Buffer
  )
{
  UINT32 Argument;

  if (Lba > 0xffffffffULL) {
    return EFI_UNSUPPORTED;
  }

  Argument = (UINT32)Lba;  // CMD1 must have reported sector-addressed eMMC.
  return MmcReadDataCommand (
           17,
           Argument,
           CMD_RESP_EXPECT | CMD_RESP_CRC,
           Buffer,
           MMC_BLOCK_SIZE
           );
}

STATIC
EFI_STATUS
EFIAPI
MmcBlockIoReset (
  IN EFI_BLOCK_IO_PROTOCOL *This,
  IN BOOLEAN               ExtendedVerification
  )
{
  (VOID)This;
  (VOID)ExtendedVerification;
  return mCardReady ? EFI_SUCCESS : EFI_NO_MEDIA;
}

STATIC
EFI_STATUS
EFIAPI
MmcBlockIoReadBlocks (
  IN EFI_BLOCK_IO_PROTOCOL *This,
  IN UINT32                MediaId,
  IN EFI_LBA               Lba,
  IN UINTN                 BufferSize,
  OUT VOID                 *Buffer
  )
{
  UINTN      BlockCount;
  UINTN      BlockIndex;
  EFI_STATUS Status;
  UINT8      *Bytes;

  if ((This == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (MediaId != This->Media->MediaId) {
    return EFI_MEDIA_CHANGED;
  }
  if (!This->Media->MediaPresent || !mCardReady) {
    return EFI_NO_MEDIA;
  }
  if ((BufferSize % MMC_BLOCK_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }
  if (((UINTN)Buffer & 3U) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  BlockCount = BufferSize / MMC_BLOCK_SIZE;
  if ((Lba > This->Media->LastBlock) ||
      (BlockCount > (UINTN)(This->Media->LastBlock - Lba + 1))) {
    return EFI_INVALID_PARAMETER;
  }

  Bytes = (UINT8 *)Buffer;
  for (BlockIndex = 0; BlockIndex < BlockCount; BlockIndex++) {
    Status = MmcReadSector (Lba + BlockIndex, Bytes + BlockIndex * MMC_BLOCK_SIZE);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MmcBlockIoWriteBlocks (
  IN EFI_BLOCK_IO_PROTOCOL *This,
  IN UINT32                MediaId,
  IN EFI_LBA               Lba,
  IN UINTN                 BufferSize,
  IN VOID                  *Buffer
  )
{
  (VOID)This;
  (VOID)MediaId;
  (VOID)Lba;
  (VOID)BufferSize;
  (VOID)Buffer;
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
MmcBlockIoFlushBlocks (
  IN EFI_BLOCK_IO_PROTOCOL *This
  )
{
  (VOID)This;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MmcInitializeCard (
  OUT UINT64 *SectorCount
  )
{
  EFI_STATUS  Status;
  UINT32      Response;
  UINTN       Attempt;
  UINT32      Version;

  Version = MmcRegRead (MMC_VERID);
  if ((Version >> 16) != 0x5342U) {
    Print (L"[emmc] no DesignWare MSHC signature at 0x%lx (VERID=0x%08x)\n",
           (UINT64)EMMC_BASE, Version);
    return EFI_NOT_FOUND;
  }

  Print (L"[emmc] DesignWare MSHC found, VERID=0x%08x\n", Version);

  // Enable the two CMU_CORE gates used by the embedded MMC host.  Select the
  // 26-MHz oscillator as the controller input so divisor 33 is below 400 kHz
  // during card identification.  The OS is expected to re-program CMU on boot.
  MmioWrite32 (CMU_CORE_MMC_ACLK_GATE,
               MmioRead32 (CMU_CORE_MMC_ACLK_GATE) | CMU_GATE_ENABLE_BIT);
  MmioWrite32 (CMU_CORE_MMC_SDCLK_GATE,
               MmioRead32 (CMU_CORE_MMC_SDCLK_GATE) | CMU_GATE_ENABLE_BIT);
  MmioWrite32 (CMU_CORE_MMC_USER_MUX,
               MmioRead32 (CMU_CORE_MMC_USER_MUX) & ~1U);
  MicroSecondDelay (10);

  // Reset controller and FIFO; stay in PIO mode (CTRL DMA bit remains clear).
  MmcRegWrite (MMC_CTRL, CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET);
  for (Attempt = 0; Attempt < MMC_POLL_LIMIT; Attempt++) {
    if ((MmcRegRead (MMC_CTRL) & (CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET)) == 0) {
      break;
    }
  }
  if (Attempt == MMC_POLL_LIMIT) {
    Print (L"[emmc] controller reset timed out\n");
    return EFI_TIMEOUT;
  }

  MmcRegWrite (MMC_PWREN, 1);
  MmcRegWrite (MMC_TMOUT, 0xffffffffU);
  MmcRegWrite (MMC_INTMASK, 0);
  MmcRegWrite (MMC_CTYPE, 0);       // 1-bit legacy bus: no EXT_CSD writes.
  // Preserve the inherited I/O-voltage bit, but turn off DDR timing if the
  // previous boot stage left the controller configured for HS400.
  MmcRegWrite (MMC_UHS_REG, MmcRegRead (MMC_UHS_REG) & ~(1U << 16));
  MmcRegWrite (MMC_FIFOTH, 0x00070008U);
  MmcRegWrite (MMC_BMOD, 0);        // DMA disabled.
  MmcRegWrite (MMC_CLKSRC, 0);

  Status = MmcSetClock (33);        // 26 MHz / (2 * 33) = about 394 kHz.
  if (EFI_ERROR (Status)) {
    Print (L"[emmc] could not configure identification clock (%r)\n", Status);
    return Status;
  }

  Status = MmcSendInitClocks ();
  if (EFI_ERROR (Status)) {
    Print (L"[emmc] initial clock sequence failed (%r)\n", Status);
    return Status;
  }

  Status = MmcSendCommand (MMC_CMD_GO_IDLE, 0, 0, NULL);
  if (EFI_ERROR (Status)) {
    Print (L"[emmc] CMD0 failed (%r)\n", Status);
    return Status;
  }

  // eMMC OCR: request sector mode plus the standard 2.7-3.6 V window.  CMD1
  // is retried until the card's busy/ready bit becomes set.
  Status = EFI_TIMEOUT;
  for (Attempt = 0; Attempt < 1000; Attempt++) {
    Status = MmcSendCommand (
               MMC_CMD_SEND_OP_COND,
               MMC_OCR_ARGUMENT,
               CMD_RESP_EXPECT,
               &Response
               );
    if (EFI_ERROR (Status)) {
      Print (L"[emmc] CMD1 failed (%r)\n", Status);
      return Status;
    }
    if ((Response & MMC_OCR_BUSY) != 0) {
      break;
    }
    MicroSecondDelay (1000);
  }
  if ((Response & MMC_OCR_BUSY) == 0) {
    Print (L"[emmc] CMD1 timed out; check eMMC power, pins and CMU clocks\n");
    return EFI_TIMEOUT;
  }
  if ((Response & MMC_OCR_SECTOR_MODE) == 0) {
    Print (L"[emmc] card did not accept sector mode; refusing unsafe addressing\n");
    return EFI_UNSUPPORTED;
  }

  Status = MmcSendCommand (
             MMC_CMD_ALL_SEND_CID,
             0,
             CMD_RESP_EXPECT | CMD_RESP_LONG | CMD_RESP_CRC,
             NULL
             );
  if (EFI_ERROR (Status)) {
    Print (L"[emmc] CMD2/CID failed (%r)\n", Status);
    return Status;
  }

  Status = MmcSendCommand (
             MMC_CMD_SET_RELATIVE_ADDR,
             MMC_RCA << 16,
             CMD_RESP_EXPECT | CMD_RESP_CRC,
             NULL
             );
  if (EFI_ERROR (Status)) {
    Print (L"[emmc] CMD3/RCA failed (%r)\n", Status);
    return Status;
  }

  Status = MmcSendCommand (
             MMC_CMD_SELECT_CARD,
             MMC_RCA << 16,
             CMD_RESP_EXPECT | CMD_RESP_CRC | CMD_WAIT_PREV_DATA,
             NULL
             );
  if (EFI_ERROR (Status)) {
    Print (L"[emmc] CMD7/select failed (%r)\n", Status);
    return Status;
  }

  // CMD7 has an R1b response.  Wait for DAT0/card-busy to release before
  // issuing CMD8 and reading the 512-byte EXT_CSD register block.
  for (Attempt = 0; Attempt < MMC_POLL_LIMIT; Attempt++) {
    if ((MmcRegRead (MMC_STATUS) & STATUS_DATA_BUSY) == 0) {
      break;
    }
  }
  if (Attempt == MMC_POLL_LIMIT) {
    Print (L"[emmc] card remained busy after CMD7\n");
    return EFI_TIMEOUT;
  }

  Status = MmcReadDataCommand (
             MMC_CMD_SEND_EXT_CSD,
             0,
             CMD_RESP_EXPECT | CMD_RESP_CRC,
             mExtCsd,
             sizeof (mExtCsd)
             );
  if (EFI_ERROR (Status)) {
    Print (L"[emmc] CMD8/EXT_CSD failed (%r)\n", Status);
    return Status;
  }

  *SectorCount = (UINT64)mExtCsd[MMC_EXT_CSD_SECTOR_COUNT] |
                 ((UINT64)mExtCsd[MMC_EXT_CSD_SECTOR_COUNT + 1] << 8) |
                 ((UINT64)mExtCsd[MMC_EXT_CSD_SECTOR_COUNT + 2] << 16) |
                 ((UINT64)mExtCsd[MMC_EXT_CSD_SECTOR_COUNT + 3] << 24);
  if (*SectorCount == 0) {
    Print (L"[emmc] EXT_CSD SEC_COUNT is zero; refusing to publish Block I/O\n");
    return EFI_UNSUPPORTED;
  }

  // Stay at a conservative 13 MHz oscillator-derived clock after init.
  Status = MmcSetClock (1);
  if (EFI_ERROR (Status)) {
    Print (L"[emmc] warning: keeping identification clock (%r)\n", Status);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Exynos850MmcBlockIoEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT64      SectorCount;

  (VOID)ImageHandle;
  (VOID)SystemTable;

  Status = MmcInitializeCard (&SectorCount);
  if (EFI_ERROR (Status)) {
    // Do not block boot if this board revision or its boot chain left MMC
    // clocks/power in a state this first-stage driver cannot initialize.
    return EFI_SUCCESS;
  }

  mMedia.MediaId = mMediaId;
  mMedia.RemovableMedia = FALSE;
  mMedia.MediaPresent = TRUE;
  mMedia.LogicalPartition = FALSE;
  mMedia.ReadOnly = TRUE;
  mMedia.WriteCaching = FALSE;
  mMedia.BlockSize = MMC_BLOCK_SIZE;
  mMedia.IoAlign = 4;
  mMedia.LastBlock = (EFI_LBA)(SectorCount - 1);
  mMedia.LowestAlignedLba = 0;
  mMedia.LogicalBlocksPerPhysicalBlock = 1;

  mBlockIo.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION2;
  mBlockIo.Media = &mMedia;
  mBlockIo.Reset = MmcBlockIoReset;
  mBlockIo.ReadBlocks = MmcBlockIoReadBlocks;
  mBlockIo.WriteBlocks = MmcBlockIoWriteBlocks;
  mBlockIo.FlushBlocks = MmcBlockIoFlushBlocks;

  mDevicePath.Vendor.Header.Type = HARDWARE_DEVICE_PATH;
  mDevicePath.Vendor.Header.SubType = HW_VENDOR_DP;
  mDevicePath.Vendor.Header.Length[0] = (UINT8)sizeof (VENDOR_DEVICE_PATH);
  mDevicePath.Vendor.Header.Length[1] = (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8);
  mDevicePath.Vendor.Guid = mExynos850MmcDevicePathGuid;
  mDevicePath.End.Type = END_DEVICE_PATH_TYPE;
  mDevicePath.End.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  mDevicePath.End.Length[0] = (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL);
  mDevicePath.End.Length[1] = (UINT8)(sizeof (EFI_DEVICE_PATH_PROTOCOL) >> 8);

  mCardReady = TRUE;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mBlockIoHandle,
                  &gEfiDevicePathProtocolGuid, &mDevicePath,
                  &gEfiBlockIoProtocolGuid, &mBlockIo,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    mCardReady = FALSE;
    Print (L"[emmc] failed to publish Block I/O (%r)\n", Status);
    return EFI_SUCCESS;
  }

  Print (L"[emmc] Exynos850 eMMC online: %lu sectors, %lu MiB; READ-ONLY\n",
         SectorCount, (SectorCount / 2048));
  Print (L"[emmc] DiskIoDxe/PartitionDxe can now scan its partition table.\n");
  return EFI_SUCCESS;
}
