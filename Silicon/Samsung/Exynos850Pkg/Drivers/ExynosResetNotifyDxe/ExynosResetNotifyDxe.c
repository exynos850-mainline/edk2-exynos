/** @file
  Translates ResetSystem() ResetData into the Samsung reboot-mode value.

  Accepted ResetData (all optional; anything else means "normal"):
    - Qualcomm-style {CHAR16 "RESET_PARAM"[12]; UINT8 Mode} as sent by the
      SimpleInit reboot menu: Mode 1 = recovery, 2 = bootloader.
    - A plain string, ASCII or UCS-2: "recovery", "bootloader"/"bl",
      "fastboot"/"fb", "charge", "sfactory".

  Samsung values (exynos-reboot.c): normal 0, recovery 1, factory 2,
  bootloader 3, charge 4, fastbootd 5. Stored in PMU+0x0810 bits [2:0].
  Whether S-Boot on this exact firmware honours each value is untested.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/ResetNotification.h>

#define PMU_SYSIP_DAT0   0x11860810UL
#define MODE_MASK        0x7U

#define MODE_NORMAL      0
#define MODE_RECOVERY    1
#define MODE_FACTORY     2
#define MODE_BOOTLOADER  3
#define MODE_CHARGE      4
#define MODE_FASTBOOTD   5

#define QC_RECOVERY      1
#define QC_BOOTLOADER    2

typedef struct {
  CHAR16  Tag[12];
  UINT8   Mode;
} RESET_PARAM;

STATIC
BOOLEAN
AsciiIs (
  IN CONST CHAR8  *A,
  IN CONST CHAR8  *Word
  )
{
  return AsciiStriCmp (A, Word) == 0;
}

STATIC
UINT32
ModeFromString (
  IN CONST CHAR8  *S
  )
{
  if (AsciiIs (S, "recovery"))   { return MODE_RECOVERY; }
  if (AsciiIs (S, "bootloader") || AsciiIs (S, "bl")) { return MODE_BOOTLOADER; }
  if (AsciiIs (S, "fastboot") || AsciiIs (S, "fb"))   { return MODE_FASTBOOTD; }
  if (AsciiIs (S, "charge"))     { return MODE_CHARGE; }
  if (AsciiIs (S, "sfactory"))   { return MODE_FACTORY; }
  return MODE_NORMAL;
}

STATIC
UINT32
ModeFromResetData (
  IN UINTN       DataSize,
  IN CONST VOID  *Data
  )
{
  CONST UINT8  *B;
  CHAR8        Tmp[24];
  UINTN        I;

  if ((Data == NULL) || (DataSize < 2)) {
    return MODE_NORMAL;
  }

  B = Data;

  if ((DataSize >= sizeof (RESET_PARAM)) &&
      (StrnCmp ((CONST CHAR16 *)Data, L"RESET_PARAM", 11) == 0))
  {
    switch (((CONST RESET_PARAM *)Data)->Mode) {
      case QC_RECOVERY:   return MODE_RECOVERY;
      case QC_BOOTLOADER: return MODE_BOOTLOADER;
      default:            return MODE_NORMAL;
    }
  }

  //
  // Plain string: UCS-2 if the second byte is zero, else ASCII.
  //
  ZeroMem (Tmp, sizeof (Tmp));
  if (B[1] == 0) {
    CONST CHAR16  *W = Data;
    for (I = 0; (I < sizeof (Tmp) - 1) && (I < DataSize / 2) && (W[I] != 0) && (W[I] < 0x80); I++) {
      Tmp[I] = (CHAR8)W[I];
    }
  } else {
    for (I = 0; (I < sizeof (Tmp) - 1) && (I < DataSize) && (B[I] != 0); I++) {
      Tmp[I] = (CHAR8)B[I];
    }
  }

  return ModeFromString (Tmp);
}

STATIC
VOID
EFIAPI
OnReset (
  IN EFI_RESET_TYPE  ResetType,
  IN EFI_STATUS      ResetStatus,
  IN UINTN           DataSize,
  IN VOID            *ResetData OPTIONAL
  )
{
  UINT32  Mode;

  //
  // Always write the field so a stale mode from an earlier request cannot
  // make the next plain reboot (or power-off + power-on) land in recovery.
  //
  Mode = (ResetType == EfiResetShutdown) ? MODE_NORMAL : ModeFromResetData (DataSize, ResetData);
  MmioAndThenOr32 (PMU_SYSIP_DAT0, ~MODE_MASK, Mode);
}

EFI_STATUS
EFIAPI
ExynosResetNotifyEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_RESET_NOTIFICATION_PROTOCOL  *Notify;
  EFI_STATUS                       Status;

  Status = gBS->LocateProtocol (&gEfiResetNotificationProtocolGuid, NULL, (VOID **)&Notify);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Notify->RegisterResetNotify (Notify, OnReset);
  DEBUG ((DEBUG_INFO, "ExynosResetNotify: registered: %r\n", Status));
  return Status;
}
