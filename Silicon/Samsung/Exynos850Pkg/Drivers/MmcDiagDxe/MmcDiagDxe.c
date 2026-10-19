/** @file
  eMMC bring-up, STAGE 1: read-only diagnostic. Opt-in; not part of the default build.

  Prints the DesignWare MMC (dw_mshc) registers of the internal eMMC controller on the
  console at ReadyToBoot. It only READS, writes nothing, and does not touch clocks,
  power domains or pins.

  dwmmc0 0x12100000  eMMC (8-bit, HS400 enhanced strobe, non-removable), from the stock DTB.
  The SD card slot (dwmmc2 @ 0x13500000) is NOT read: it is probably powered down.
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/IoLib.h>
#include <Guid/EventGroup.h>

#define EMMC_BASE  0x12100000UL

typedef struct {
  UINT32        Offset;
  CONST CHAR16  *Name;
} MMC_REG;

STATIC CONST MMC_REG  mRegs[] = {
  { 0x00, L"CTRL"    },
  { 0x04, L"PWREN"   },
  { 0x08, L"CLKDIV"  },
  { 0x0C, L"CLKSRC"  },
  { 0x10, L"CLKENA"  },
  { 0x14, L"TMOUT"   },
  { 0x18, L"CTYPE"   },
  { 0x1C, L"BLKSIZ"  },
  { 0x24, L"INTMASK" },
  { 0x2C, L"CMD"     },
  { 0x44, L"RINTSTS" },
  { 0x48, L"STATUS"  },
  { 0x4C, L"FIFOTH"  },
  { 0x50, L"CDETECT" },
  { 0x6C, L"VERID"   },
  { 0x70, L"HCON"    },
  { 0x74, L"UHS_REG" },
  { 0x78, L"RST_N"   },
  { 0x80, L"BMOD"    },
  { 0x8C, L"IDSTS"   },
};

STATIC
VOID
EFIAPI
MmcDiagNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINTN   Index;
  UINT32  Verid;
  UINT32  Clkena;
  UINT32  Pwren;

  gBS->CloseEvent (Event);   // run once

  Print (L"\n[mmc-diag] eMMC controller 0x%08x (read-only)\n", EMMC_BASE);
  for (Index = 0; Index < ARRAY_SIZE (mRegs); Index++) {
    Print (L"[mmc-diag] +0x%02x %-8s = 0x%08x\n",
           mRegs[Index].Offset, mRegs[Index].Name,
           MmioRead32 (EMMC_BASE + mRegs[Index].Offset));
  }

  Verid  = MmioRead32 (EMMC_BASE + 0x6C);
  Clkena = MmioRead32 (EMMC_BASE + 0x10);
  Pwren  = MmioRead32 (EMMC_BASE + 0x04);

  Print (L"[mmc-diag] VERID %a (a DesignWare MSHC reads 0x5342xxxx)\n",
         ((Verid >> 16) == 0x5342) ? "looks valid" : "UNEXPECTED, controller not answering");
  Print (L"[mmc-diag] card power %a, card clock %a\n",
         (Pwren  & 1) ? "ON" : "off",
         (Clkena & 1) ? "ON" : "off");
  Print (L"[mmc-diag] done. Photograph this screen.\n\n");
}

EFI_STATUS
EFIAPI
MmcDiagEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT  Event;

  return gBS->CreateEventEx (
                EVT_NOTIFY_SIGNAL,
                TPL_CALLBACK,
                MmcDiagNotify,
                NULL,
                &gEfiEventReadyToBootGuid,
                &Event
                );
}
