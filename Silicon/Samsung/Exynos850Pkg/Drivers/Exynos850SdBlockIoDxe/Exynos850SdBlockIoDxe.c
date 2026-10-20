/** @file
  Exynos850 SD-card-slot Block I/O driver (PIO read/write).

  Same DesignWare MSHC IP block as the internal eMMC driver
  (Exynos850MmcBlockIoDxe), just the second instance, and the SD protocol
  instead of the eMMC one: CMD8 + ACMD41 (not CMD1), card-assigned RCA from
  CMD3 (not host-assigned), capacity from the CSD register (not EXT_CSD).

  Like the eMMC driver, this now issues real CMD24 (WRITE_BLOCK) writes
  alongside CMD17 reads - same single-block PIO FIFO path, same post-write
  DAT0 busy-wait before the next command. One thing this driver deliberately
  does NOT do: read the SD card's mechanical write-protect notch. That notch
  is read by the host's own GPIO/switch wiring on most designs, not by any
  register in this DesignWare MSHC block, and this driver doesn't have a
  confirmed GPIO for it on this board - so a physically write-protected card
  will fail the CMD24 itself (R1 card status / CRC/timeout error) rather
  than being refused up front. That failure is surfaced as EFI_DEVICE_ERROR,
  same as any other write failure.

  Hardware facts below came from the user's own captured live device tree
  (tools/samsung-a145f/live-dtb/stock_live.dtb in the uniLoader repo), not
  from guessing:
    * SD slot DesignWare MSHC base: 0x13500000 ("dwmmc2@13500000")
    * slot is 4-bit, supports-highspeed/sd-uhs-sdr50/sdr104 upstream, but
      this driver only ever negotiates 1-bit legacy/default-speed, same
      conservative choice the eMMC driver makes
    * card-detect-gpio is wired to gpa0-3 or an external IRQ line,  and the
      controller also has its own CDETECT register (offset 0x50) - this
      driver reads CDETECT for an informational log line and nothing else,
      since the DT marks this slot "card-detect-invert" and guessing the
      polarity wrong would make the driver refuse a card that IS present
    * vmmc (LDO24) / vqmmc (LDO10) are regulators on the PMIC, reached over
      I2C, not simple GPIOs - this driver does NOT attempt to power them on
    * the SD CIU clock (clocks = <&clock CLK_..199> <&clock CLK_..192>) is
      gated from clock-controller@0x120e0000 (CMU_TOP), not CMU_CORE where
      the eMMC gates live - the DTB only gives abstract clock IDs (0x199 /
      0x192), not a confirmed ID-to-bit mapping, so this driver does not
      attempt to gate it either

  Net effect: unlike the eMMC driver, this one does NOT self-power the SD
  slot. It only succeeds if a previous boot stage already left the rail and
  CIU clock running (a card the user left inserted across a reboot is the
  common case this covers). If the slot is cold, VERID / CMD0 will fail or
  time out and the driver logs why and returns EFI_SUCCESS without
  publishing Block I/O, exactly like the eMMC driver's "never block boot"
  policy. Getting real power-on support later needs the PMIC's I2C address
  and regulator register map (S2MPUxx family, unconfirmed here) plus the
  CMU_TOP clock-ID table from the Samsung clk driver - guessing either one
  risks hitting the wrong I2C target or the wrong clock gate on real
  hardware, so this driver deliberately does not try.

  This driver only supports CSD-structure-1 (SDHC/SDXC, CCS=1) cards, i.e.
  essentially everything made since ~2010. Older standard-capacity cards are
  refused rather than guessed at.
**/

#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

#define SD_BASE                    0x13500000UL

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
#define MMC_RESP1                  0x034
#define MMC_RESP2                  0x038
#define MMC_RESP3                  0x03c
#define MMC_RINTSTS                0x044
#define MMC_STATUS                 0x048
#define MMC_FIFOTH                 0x04c
#define MMC_CDETECT                0x050
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
#define CMD_DATA_WRITE             (1U << 10)
#define CMD_DATA_EXPECTED          (1U << 9)
#define CMD_RESP_CRC               (1U << 8)
#define CMD_RESP_LONG              (1U << 7)
#define CMD_RESP_EXPECT            (1U << 6)

#define INT_CMD_DONE               (1U << 2)
#define INT_DATA_OVER              (1U << 3)
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
#define STATUS_FIFO_FULL           (1U << 3)
#define STATUS_DATA_BUSY            (1U << 9)

#define SD_CMD_GO_IDLE              0
#define SD_CMD_ALL_SEND_CID         2
#define SD_CMD_SEND_RELATIVE_ADDR   3
#define SD_CMD_SELECT_CARD          7
#define SD_CMD_SEND_IF_COND         8
#define SD_CMD_SEND_CSD             9
#define SD_CMD_WRITE_BLOCK          24
#define SD_CMD_APP_CMD              55
#define SD_ACMD_SD_SEND_OP_COND     41

#define SD_IF_COND_VOLTAGE_27_36    (1U << 8)
#define SD_IF_COND_CHECK_PATTERN    0xaaU
#define SD_OCR_BUSY                 (1U << 31)
#define SD_OCR_CCS                  (1U << 30)
#define SD_OCR_HCS_ARG              (1U << 30)
#define SD_OCR_VOLTAGE_WINDOW       0x00FF8000U
#define SD_BLOCK_SIZE                512U
#define SD_POLL_LIMIT                1000000U

STATIC EFI_BLOCK_IO_MEDIA        mMedia;
STATIC EFI_BLOCK_IO_PROTOCOL     mBlockIo;
STATIC EFI_HANDLE                mBlockIoHandle;
STATIC BOOLEAN                   mCardReady;
STATIC UINT32                    mMediaId = 0x145FCA4DU;

STATIC CONST EFI_GUID mExynos850SdDevicePathGuid = {
  0xa1458501, 0x01c0, 0x4e4d, { 0x53, 0x44, 0x43, 0x41, 0x52, 0x44, 0x00, 0x01 }
};

typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} EXYNOS850_SD_DEVICE_PATH;

STATIC EXYNOS850_SD_DEVICE_PATH mDevicePath;

STATIC
UINT32
SdRegRead (
  IN UINTN Offset
  )
{
  return MmioRead32 (SD_BASE + Offset);
}

STATIC
VOID
SdRegWrite (
  IN UINTN  Offset,
  IN UINT32 Value
  )
{
  MmioWrite32 (SD_BASE + Offset, Value);
}

STATIC
EFI_STATUS
SdWaitCommandStartClear (
  VOID
  )
{
  UINTN  Count;

  for (Count = 0; Count < SD_POLL_LIMIT; Count++) {
    if ((SdRegRead (MMC_CMD) & CMD_START) == 0) {
      if ((SdRegRead (MMC_RINTSTS) & INT_HARDWARE_LOCK_ERROR) != 0) {
        return EFI_DEVICE_ERROR;
      }
      return EFI_SUCCESS;
    }
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
SdUpdateClock (
  VOID
  )
{
  SdRegWrite (MMC_RINTSTS, 0xffffffffU);
  SdRegWrite (MMC_CMD, CMD_START | CMD_UPDATE_CLOCK | CMD_WAIT_PREV_DATA);
  return SdWaitCommandStartClear ();
}

STATIC
EFI_STATUS
SdSetClock (
  IN UINT32 Divisor
  )
{
  EFI_STATUS Status;

  SdRegWrite (MMC_CLKENA, 0);
  Status = SdUpdateClock ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  SdRegWrite (MMC_CLKDIV, Divisor);
  Status = SdUpdateClock ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  SdRegWrite (MMC_CLKENA, 1);
  return SdUpdateClock ();
}

STATIC
EFI_STATUS
SdSendCommand (
  IN  UINT32 Index,
  IN  UINT32 Argument,
  IN  UINT32 Flags,
  OUT UINT32 *Response OPTIONAL
  )
{
  UINT32     Interrupts;
  UINTN      Count;
  EFI_STATUS Status;

  SdRegWrite (MMC_RINTSTS, 0xffffffffU);
  SdRegWrite (MMC_CMDARG, Argument);
  SdRegWrite (MMC_CMD, CMD_START | (Index & 0x3fU) | Flags);

  for (Count = 0; Count < SD_POLL_LIMIT; Count++) {
    Interrupts = SdRegRead (MMC_RINTSTS);
    if ((Interrupts & INT_COMMAND_ERRORS) != 0) {
      SdRegWrite (MMC_RINTSTS, Interrupts);
      return EFI_DEVICE_ERROR;
    }

    if ((Interrupts & INT_CMD_DONE) != 0) {
      if (Response != NULL) {
        *Response = SdRegRead (MMC_RESP0);
      }
      SdRegWrite (MMC_RINTSTS, Interrupts);
      return EFI_SUCCESS;
    }
  }

  Status = SdWaitCommandStartClear ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Interrupts = SdRegRead (MMC_RINTSTS);
  if ((Interrupts & INT_CMD_DONE) != 0) {
    if (Response != NULL) {
      *Response = SdRegRead (MMC_RESP0);
    }
    SdRegWrite (MMC_RINTSTS, Interrupts);
    return EFI_SUCCESS;
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
SdAppCommand (
  IN  UINT32 Rca,
  IN  UINT32 AcmdIndex,
  IN  UINT32 AcmdArgument,
  IN  UINT32 AcmdFlags,
  OUT UINT32 *Response OPTIONAL
  )
{
  EFI_STATUS Status;

  // CMD55 must immediately precede the ACMD it is escaping; a failure here
  // means the card does not support application commands at all.
  Status = SdSendCommand (
             SD_CMD_APP_CMD,
             Rca << 16,
             CMD_RESP_EXPECT | CMD_RESP_CRC,
             NULL
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return SdSendCommand (AcmdIndex, AcmdArgument, AcmdFlags, Response);
}

STATIC
EFI_STATUS
SdSendInitClocks (
  VOID
  )
{
  SdRegWrite (MMC_RINTSTS, 0xffffffffU);
  SdRegWrite (MMC_CMD, CMD_START | CMD_SEND_INIT);
  return SdWaitCommandStartClear ();
}

STATIC
EFI_STATUS
SdReadDataCommand (
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

  SdRegWrite (MMC_BLKSIZ, (UINT32)BufferSize);
  SdRegWrite (MMC_BYTCNT, (UINT32)BufferSize);
  SdRegWrite (MMC_RINTSTS, 0xffffffffU);
  SdRegWrite (MMC_CMDARG, Argument);
  SdRegWrite (MMC_CMD, CMD_START | (Index & 0x3fU) | Flags | CMD_DATA_EXPECTED);

  // Same FIFO-drain-while-receiving approach as the eMMC driver: the FIFO is
  // only 64 words deep, so wait for CMD_DONE first can overrun it.

  Words = (UINT32 *)Buffer;
  WordCount = BufferSize / sizeof (UINT32);
  for (WordIndex = 0; WordIndex < WordCount; WordIndex++) {
    for (Count = 0; Count < SD_POLL_LIMIT; Count++) {
      Interrupts = SdRegRead (MMC_RINTSTS);
      if ((Interrupts & (INT_COMMAND_ERRORS | INT_DATA_ERRORS)) != 0) {
        SdRegWrite (MMC_RINTSTS, Interrupts);
        return EFI_DEVICE_ERROR;
      }
      if ((SdRegRead (MMC_STATUS) & STATUS_FIFO_EMPTY) == 0) {
        break;
      }
      if ((Interrupts & INT_DATA_OVER) != 0) {
        return EFI_DEVICE_ERROR;
      }
    }
    if (Count == SD_POLL_LIMIT) {
      return EFI_TIMEOUT;
    }

    Words[WordIndex] = SdRegRead (MMC_FIFO);
  }

  for (Count = 0; Count < SD_POLL_LIMIT; Count++) {
    Interrupts = SdRegRead (MMC_RINTSTS);
    if ((Interrupts & (INT_COMMAND_ERRORS | INT_DATA_ERRORS)) != 0) {
      SdRegWrite (MMC_RINTSTS, Interrupts);
      return EFI_DEVICE_ERROR;
    }
    if ((Interrupts & (INT_DATA_OVER | INT_CMD_DONE)) ==
        (INT_DATA_OVER | INT_CMD_DONE)) {
      SdRegWrite (MMC_RINTSTS, Interrupts);
      return EFI_SUCCESS;
    }
  }

  Status = SdWaitCommandStartClear ();
  return EFI_ERROR (Status) ? Status : EFI_TIMEOUT;
}

STATIC
EFI_STATUS
SdReadSector (
  IN  EFI_LBA Lba,
  OUT VOID    *Buffer
  )
{
  if (Lba > 0xffffffffULL) {
    return EFI_UNSUPPORTED;
  }

  // Block addressing is only correct for CCS=1 (SDHC/SDXC) cards, which is
  // the only kind this driver ever publishes Block I/O for.
  return SdReadDataCommand (
           17,
           (UINT32)Lba,
           CMD_RESP_EXPECT | CMD_RESP_CRC,
           Buffer,
           SD_BLOCK_SIZE
           );
}

STATIC
EFI_STATUS
SdWriteDataCommand (
  IN  UINT32      Index,
  IN  UINT32      Argument,
  IN  UINT32      Flags,
  IN  CONST VOID  *Buffer,
  IN  UINTN       BufferSize
  )
{
  EFI_STATUS    Status;
  UINT32        Interrupts;
  UINTN         WordIndex;
  UINTN         WordCount;
  UINTN         Count;
  CONST UINT32  *Words;

  if ((Buffer == NULL) || (BufferSize == 0) || ((BufferSize & 3U) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  SdRegWrite (MMC_BLKSIZ, (UINT32)BufferSize);
  SdRegWrite (MMC_BYTCNT, (UINT32)BufferSize);
  SdRegWrite (MMC_RINTSTS, 0xffffffffU);
  SdRegWrite (MMC_CMDARG, Argument);
  SdRegWrite (MMC_CMD, CMD_START | (Index & 0x3fU) | Flags |
              CMD_DATA_EXPECTED | CMD_DATA_WRITE);

  // Same FIFO-drain-while-sending approach as the eMMC driver's write path.

  Words     = (CONST UINT32 *)Buffer;
  WordCount = BufferSize / sizeof (UINT32);
  for (WordIndex = 0; WordIndex < WordCount; WordIndex++) {
    for (Count = 0; Count < SD_POLL_LIMIT; Count++) {
      Interrupts = SdRegRead (MMC_RINTSTS);
      if ((Interrupts & (INT_COMMAND_ERRORS | INT_DATA_ERRORS)) != 0) {
        SdRegWrite (MMC_RINTSTS, Interrupts);
        return EFI_DEVICE_ERROR;
      }
      if ((SdRegRead (MMC_STATUS) & STATUS_FIFO_FULL) == 0) {
        break;
      }
    }
    if (Count == SD_POLL_LIMIT) {
      return EFI_TIMEOUT;
    }

    SdRegWrite (MMC_FIFO, Words[WordIndex]);
  }

  for (Count = 0; Count < SD_POLL_LIMIT; Count++) {
    Interrupts = SdRegRead (MMC_RINTSTS);
    if ((Interrupts & (INT_COMMAND_ERRORS | INT_DATA_ERRORS)) != 0) {
      SdRegWrite (MMC_RINTSTS, Interrupts);
      return EFI_DEVICE_ERROR;
    }
    if ((Interrupts & (INT_DATA_OVER | INT_CMD_DONE)) ==
        (INT_DATA_OVER | INT_CMD_DONE)) {
      SdRegWrite (MMC_RINTSTS, Interrupts);
      break;
    }
  }
  if (Count == SD_POLL_LIMIT) {
    Status = SdWaitCommandStartClear ();
    return EFI_ERROR (Status) ? Status : EFI_TIMEOUT;
  }

  // Wait for the card to release DAT0 (done programming flash) before this
  // function returns, same reasoning as the eMMC driver's write path - a
  // write-protected or write-failing card is far more likely on a removable
  // SD card than on the internal eMMC, so this wait (and its timeout) is the
  // only signal this driver has that something went wrong post-transfer.
  for (Count = 0; Count < SD_POLL_LIMIT; Count++) {
    if ((SdRegRead (MMC_STATUS) & STATUS_DATA_BUSY) == 0) {
      return EFI_SUCCESS;
    }
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
SdWriteSector (
  IN  EFI_LBA     Lba,
  IN  CONST VOID  *Buffer
  )
{
  if (Lba > 0xffffffffULL) {
    return EFI_UNSUPPORTED;
  }

  return SdWriteDataCommand (
           SD_CMD_WRITE_BLOCK,
           (UINT32)Lba,
           CMD_RESP_EXPECT | CMD_RESP_CRC,
           Buffer,
           SD_BLOCK_SIZE
           );
}

STATIC
EFI_STATUS
EFIAPI
SdBlockIoReset (
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
SdBlockIoReadBlocks (
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
  if ((BufferSize % SD_BLOCK_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }
  if (((UINTN)Buffer & 3U) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  BlockCount = BufferSize / SD_BLOCK_SIZE;
  if ((Lba > This->Media->LastBlock) ||
      (BlockCount > (UINTN)(This->Media->LastBlock - Lba + 1))) {
    return EFI_INVALID_PARAMETER;
  }

  Bytes = (UINT8 *)Buffer;
  for (BlockIndex = 0; BlockIndex < BlockCount; BlockIndex++) {
    Status = SdReadSector (Lba + BlockIndex, Bytes + BlockIndex * SD_BLOCK_SIZE);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SdBlockIoWriteBlocks (
  IN EFI_BLOCK_IO_PROTOCOL *This,
  IN UINT32                MediaId,
  IN EFI_LBA               Lba,
  IN UINTN                 BufferSize,
  IN VOID                  *Buffer
  )
{
  UINTN       BlockCount;
  UINTN       BlockIndex;
  EFI_STATUS  Status;
  CONST UINT8 *Bytes;

  if ((This == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (MediaId != This->Media->MediaId) {
    return EFI_MEDIA_CHANGED;
  }
  if (!This->Media->MediaPresent || !mCardReady) {
    return EFI_NO_MEDIA;
  }
  if (This->Media->ReadOnly) {
    return EFI_WRITE_PROTECTED;
  }
  if ((BufferSize % SD_BLOCK_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }
  if (((UINTN)Buffer & 3U) != 0) {
    return EFI_INVALID_PARAMETER;
  }

  BlockCount = BufferSize / SD_BLOCK_SIZE;
  if ((Lba > This->Media->LastBlock) ||
      (BlockCount > (UINTN)(This->Media->LastBlock - Lba + 1))) {
    return EFI_INVALID_PARAMETER;
  }

  Bytes = (CONST UINT8 *)Buffer;
  for (BlockIndex = 0; BlockIndex < BlockCount; BlockIndex++) {
    Status = SdWriteSector (Lba + BlockIndex, Bytes + BlockIndex * SD_BLOCK_SIZE);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SdBlockIoFlushBlocks (
  IN EFI_BLOCK_IO_PROTOCOL *This
  )
{
  (VOID)This;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SdInitializeCard (
  OUT UINT64 *SectorCount
  )
{
  EFI_STATUS  Status;
  UINT32      Response;
  UINT32      Rca;
  UINTN       Attempt;
  UINT32      Version;
  UINT32      Cdetect;
  UINT32      HwResp0;
  UINT32      HwResp1;
  UINT32      HwResp2;
  UINT32      CsdStructure;
  UINT32      CSize;

  Version = SdRegRead (MMC_VERID);
  if ((Version >> 16) != 0x5342U) {
    Print (L"[sdcard] no DesignWare MSHC signature at 0x%lx (VERID=0x%08x); "
           L"slot is probably clock-gated/unpowered right now\n",
           (UINT64)SD_BASE, Version);
    return EFI_NOT_FOUND;
  }

  Print (L"[sdcard] DesignWare MSHC found, VERID=0x%08x\n", Version);

  Cdetect = SdRegRead (MMC_CDETECT);
  Print (L"[sdcard] CDETECT=0x%08x (raw controller bit; DT marks this slot "
         L"card-detect-invert, polarity not assumed here)\n", Cdetect);

  // Deliberately NOT touching the CMU_TOP CIU clock gate or the vmmc/vqmmc
  // PMIC regulators here - see the file header. Only the controller-local
  // clock divider (safe, does not reach outside this IP block) is driven.

  SdRegWrite (MMC_CTRL, CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET);
  for (Attempt = 0; Attempt < SD_POLL_LIMIT; Attempt++) {
    if ((SdRegRead (MMC_CTRL) & (CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET)) == 0) {
      break;
    }
  }
  if (Attempt == SD_POLL_LIMIT) {
    Print (L"[sdcard] controller reset timed out\n");
    return EFI_TIMEOUT;
  }

  SdRegWrite (MMC_PWREN, 1);
  SdRegWrite (MMC_TMOUT, 0xffffffffU);
  SdRegWrite (MMC_INTMASK, 0);
  SdRegWrite (MMC_CTYPE, 0);        // 1-bit legacy bus, same conservative choice as eMMC.
  SdRegWrite (MMC_UHS_REG, SdRegRead (MMC_UHS_REG) & ~(1U << 16));
  SdRegWrite (MMC_FIFOTH, 0x00070008U);
  SdRegWrite (MMC_BMOD, 0);         // DMA disabled.
  SdRegWrite (MMC_CLKSRC, 0);

  Status = SdSetClock (33);         // same ~394 kHz identification clock as eMMC.
  if (EFI_ERROR (Status)) {
    Print (L"[sdcard] could not configure identification clock (%r)\n", Status);
    return Status;
  }

  Status = SdSendInitClocks ();
  if (EFI_ERROR (Status)) {
    Print (L"[sdcard] initial clock sequence failed (%r)\n", Status);
    return Status;
  }

  Status = SdSendCommand (SD_CMD_GO_IDLE, 0, 0, NULL);
  if (EFI_ERROR (Status)) {
    Print (L"[sdcard] CMD0 failed (%r); no card, or slot unpowered\n", Status);
    return Status;
  }

  // CMD8 both probes for "SD 2.0 or later" and the 2.7-3.6V window. A card
  // that does not echo the pattern is either absent, non-SD, or an SDSC
  // (pre-2.0) card - this driver deliberately does not fall back further.
  Status = SdSendCommand (
             SD_CMD_SEND_IF_COND,
             SD_IF_COND_VOLTAGE_27_36 | SD_IF_COND_CHECK_PATTERN,
             CMD_RESP_EXPECT | CMD_RESP_CRC,
             &Response
             );
  if (EFI_ERROR (Status) ||
      ((Response & 0xffU) != SD_IF_COND_CHECK_PATTERN)) {
    Print (L"[sdcard] CMD8 failed or pattern mismatch (%r, resp=0x%08x); "
           L"refusing pre-2.0/non-SD cards rather than guessing\n",
           Status, Response);
    return EFI_UNSUPPORTED;
  }

  // ACMD41 until the card reports ready; require CCS (SDHC/SDXC) since that
  // is the only capacity addressing this driver decodes.
  Status = EFI_TIMEOUT;
  Response = 0;
  for (Attempt = 0; Attempt < 1000; Attempt++) {
    Status = SdAppCommand (
               0,
               SD_ACMD_SD_SEND_OP_COND,
               SD_OCR_HCS_ARG | SD_OCR_VOLTAGE_WINDOW,
               CMD_RESP_EXPECT,
               &Response
               );
    if (EFI_ERROR (Status)) {
      Print (L"[sdcard] ACMD41 failed (%r)\n", Status);
      return Status;
    }
    if ((Response & SD_OCR_BUSY) != 0) {
      break;
    }
    MicroSecondDelay (1000);
  }
  if ((Response & SD_OCR_BUSY) == 0) {
    Print (L"[sdcard] ACMD41 timed out; check SD slot power and CIU clock\n");
    return EFI_TIMEOUT;
  }
  if ((Response & SD_OCR_CCS) == 0) {
    Print (L"[sdcard] card reported standard capacity (no CCS); refusing - "
           L"this driver only decodes SDHC/SDXC block addressing\n");
    return EFI_UNSUPPORTED;
  }

  Status = SdSendCommand (
             SD_CMD_ALL_SEND_CID,
             0,
             CMD_RESP_EXPECT | CMD_RESP_LONG | CMD_RESP_CRC,
             NULL
             );
  if (EFI_ERROR (Status)) {
    Print (L"[sdcard] CMD2/CID failed (%r)\n", Status);
    return Status;
  }

  // CMD3: unlike eMMC's CMD3, the SD card assigns its own RCA and returns it
  // in response bits [31:16].
  Status = SdSendCommand (
             SD_CMD_SEND_RELATIVE_ADDR,
             0,
             CMD_RESP_EXPECT | CMD_RESP_CRC,
             &Response
             );
  if (EFI_ERROR (Status)) {
    Print (L"[sdcard] CMD3/RCA failed (%r)\n", Status);
    return Status;
  }
  Rca = Response >> 16;

  Status = SdSendCommand (
             SD_CMD_SEND_CSD,
             Rca << 16,
             CMD_RESP_EXPECT | CMD_RESP_LONG | CMD_RESP_CRC,
             NULL
             );
  if (EFI_ERROR (Status)) {
    Print (L"[sdcard] CMD9/CSD failed (%r)\n", Status);
    return Status;
  }

  // Response word mapping for a 136-bit (R2) response on this IP: hardware
  // RESP3 holds register bits [127:96] (MSB), RESP2 holds [95:64], RESP1
  // holds [63:32], RESP0 holds [31:0] - same convention the Linux dw_mmc
  // driver uses. CSD_STRUCTURE is register bits [127:126]; C_SIZE (CSD
  // version 1/SDHC-SDXC only) is register bits [69:48], which straddles
  // RESP2 (bits [69:64], the low 6 bits of C_SIZE) and RESP1 (bits [63:48],
  // the high 16 bits... counted from C_SIZE's own LSB, i.e. RESP1[31:16]).
  HwResp0 = SdRegRead (MMC_RESP3);
  HwResp1 = SdRegRead (MMC_RESP2);
  HwResp2 = SdRegRead (MMC_RESP1);

  CsdStructure = (HwResp0 >> 30) & 0x3U;
  if (CsdStructure != 1) {
    Print (L"[sdcard] CSD_STRUCTURE=%u, not the v2.0 (SDHC/SDXC) layout this "
           L"driver decodes; refusing rather than guessing C_SIZE bits\n",
           CsdStructure);
    return EFI_UNSUPPORTED;
  }

  CSize = ((HwResp1 & 0x3fU) << 16) | (HwResp2 >> 16);
  *SectorCount = ((UINT64)CSize + 1) * 1024ULL;
  if (*SectorCount == 0) {
    Print (L"[sdcard] CSD C_SIZE decoded to zero sectors; refusing to publish Block I/O\n");
    return EFI_UNSUPPORTED;
  }

  Status = SdSendCommand (
             SD_CMD_SELECT_CARD,
             Rca << 16,
             CMD_RESP_EXPECT | CMD_RESP_CRC | CMD_WAIT_PREV_DATA,
             NULL
             );
  if (EFI_ERROR (Status)) {
    Print (L"[sdcard] CMD7/select failed (%r)\n", Status);
    return Status;
  }

  // Stay at a conservative clock after init, same choice as the eMMC driver.
  Status = SdSetClock (1);
  if (EFI_ERROR (Status)) {
    Print (L"[sdcard] warning: keeping identification clock (%r)\n", Status);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
Exynos850SdBlockIoEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINT64      SectorCount;

  (VOID)ImageHandle;
  (VOID)SystemTable;

  Status = SdInitializeCard (&SectorCount);
  if (EFI_ERROR (Status)) {
    // Never block boot over an absent/unpowered SD slot.
    return EFI_SUCCESS;
  }

  mMedia.MediaId = mMediaId;
  mMedia.RemovableMedia = TRUE;
  mMedia.MediaPresent = TRUE;
  mMedia.LogicalPartition = FALSE;
  mMedia.ReadOnly = FALSE;
  mMedia.WriteCaching = FALSE;
  mMedia.BlockSize = SD_BLOCK_SIZE;
  mMedia.IoAlign = 4;
  mMedia.LastBlock = (EFI_LBA)(SectorCount - 1);
  mMedia.LowestAlignedLba = 0;
  mMedia.LogicalBlocksPerPhysicalBlock = 1;

  mBlockIo.Revision = EFI_BLOCK_IO_PROTOCOL_REVISION2;
  mBlockIo.Media = &mMedia;
  mBlockIo.Reset = SdBlockIoReset;
  mBlockIo.ReadBlocks = SdBlockIoReadBlocks;
  mBlockIo.WriteBlocks = SdBlockIoWriteBlocks;
  mBlockIo.FlushBlocks = SdBlockIoFlushBlocks;

  mDevicePath.Vendor.Header.Type = HARDWARE_DEVICE_PATH;
  mDevicePath.Vendor.Header.SubType = HW_VENDOR_DP;
  mDevicePath.Vendor.Header.Length[0] = (UINT8)sizeof (VENDOR_DEVICE_PATH);
  mDevicePath.Vendor.Header.Length[1] = (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8);
  mDevicePath.Vendor.Guid = mExynos850SdDevicePathGuid;
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
    Print (L"[sdcard] failed to publish Block I/O (%r)\n", Status);
    return EFI_SUCCESS;
  }

  Print (L"[sdcard] Exynos850 SD card online: %lu sectors, %lu MiB; READ/WRITE "
         L"(single-block PIO CMD17/CMD24, no DMA, no WP-notch check)\n",
         SectorCount, (SectorCount / 2048));
  Print (L"[sdcard] DiskIoDxe/PartitionDxe can now scan its partition table.\n");
  return EFI_SUCCESS;
}
