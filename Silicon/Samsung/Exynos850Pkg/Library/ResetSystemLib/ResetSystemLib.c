/** @file
  ResetSystemLib implementation for Exynos850 (S5E3830), SM-A145F.

  Why this exists: the ExynosCommonDsc.inc default wires ResetSystemLib to
  ArmPkg's ArmSmcPsciResetSystemLib, which issues PSCI SYSTEM_RESET /
  SYSTEM_OFF over SMC and simply RETURNS if the secure monitor does not
  implement them (per the PSCI spec, those calls are documented to fall
  through on an unimplemented function). On this device they do fall
  through: the secure monitor here only backs the PSCI calls Linux's SMP
  bring-up needs (CPU_ON/OFF/SUSPEND) - confirmed by the live DTB
  (psci node is present, "enable-method = psci" on every CPU) - but not
  SYSTEM_RESET/SYSTEM_OFF. Simple-init's boot-menu "Reboot"/"Power Off"
  entries call gRT->ResetSystem(), see it return instead of resetting, and
  print "reset system failed" / "... failed with Invalid argument" - which
  is exactly what showed up on screen.

  Samsung's own Android kernel never relies on PSCI for this either: it
  power-cycles the AP by toggling PS_HOLD in the PMU_ALIVE block, and
  triggers a full warm reset via the PMU's SWRESET register. Both live in
  the same memory-mapped PMU_ALIVE syscon the stock DTB exposes at
  /system-controller@11860000 (compatible "samsung,exynos3830-pmu").

  PS_HOLD_CONTROL offset (0x30C) is not a guess: it is read directly out of
  the stock DTB's own "exynos-debug-test" node
  (ps_hold_control_offset = <0x30c>;), captured from the running device via
  a145f-capture-live-dtb.sh. Bit 8 (0x100) is the PS_HOLD output enable bit;
  clearing it is the same mechanism Samsung's sec_reboot/pm_power_off path
  and this project's own Exynos7420Pkg ResetSystemLib already use for the
  equivalent register on an older SoC in the same family.

  SWRESET is SYSTEM_CONFIGURATION (PMU_ALIVE + 0x3A00), bit 1
  (SWRESET_SYSTEM). This is not carried over from the Exynos7420 (0x400): it
  is taken from the stock kernel's exynos-reboot.c (drv_data_v1:
  swreset_reg = SYSTEM_CONFIGURATION, swreset_bit = 1) and matches the
  syscon-reboot node of the upstream Linux exynos850.dtsi
  (offset 0x3a00, mask/value 0x2).

  Both writes are bounded (a few hundred iterations of a 1us-ish spin, not
  Exynos7420Pkg's unbounded "while (...)"), so a wrong guess here hangs for
  a moment and then returns cleanly to the caller (which logs it) instead
  of wedging the whole firmware the way an infinite poll loop would.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/ResetSystemLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

//
// PMU_ALIVE / "system-controller@11860000" (samsung,exynos3830-pmu, syscon).
// Base confirmed from the stock live DTB.
//
#define PMU_ALIVE_BASE          0x11860000UL

//
// Offset confirmed from the stock live DTB's exynos-debug-test node
// (ps_hold_control_offset = <0x30c>;). Bit 8 is PS_HOLD.
//
#define PS_HOLD_CONTROL_OFFSET  0x030CUL
#define PS_HOLD_EN_BIT          (1UL << 8)

//
// Offset carried over from this repo's Exynos7420Pkg ResetSystemLib; same
// register name/position across the whole Exynos4/5/7 PMU_ALIVE family.
// Not yet independently confirmed on this exact SoC.
//
#define SWRESET_OFFSET          0x3A00UL
#define SWRESET_TRIGGER         0x2UL   // SWRESET_SYSTEM = bit 1

#define POLL_ITERATIONS         2000
#define POLL_DELAY_US           100

STATIC
VOID
PollForBitOrTimeout (
  IN UINTN   RegisterAddress,
  IN UINT32  ExpectedValue,
  IN CHAR8   *WhichReset
  )
{
  UINTN  Count;

  for (Count = 0; Count < POLL_ITERATIONS; Count++) {
    if (MmioRead32 (RegisterAddress) == ExpectedValue) {
      return;
    }

    MicroSecondDelay (POLL_DELAY_US);
  }

  DEBUG ((
    DEBUG_WARN,
    "ResetSystemLib(a145f): %a did not take within %d us - PMU SFR at 0x%lx "
    "never latched 0x%08x. Returning to caller instead of hanging.\n",
    WhichReset,
    POLL_ITERATIONS * POLL_DELAY_US,
    RegisterAddress,
    ExpectedValue
    ));
}

/**
  Cold/warm reset: full-chip reset via PMU SWRESET.
  If this function returns, the write did not take within the poll window.
**/
VOID
EFIAPI
ResetCold (
  VOID
  )
{
  UINTN  SwResetAddress;

  SwResetAddress = PMU_ALIVE_BASE + SWRESET_OFFSET;

  DEBUG ((DEBUG_ERROR, "ResetSystemLib(a145f): cold reset (PMU SWRESET @0x%lx)\n", SwResetAddress));

  MmioWrite32 (SwResetAddress, SWRESET_TRIGGER);
  PollForBitOrTimeout (SwResetAddress, SWRESET_TRIGGER, "SWRESET cold reset");
}

VOID
EFIAPI
ResetWarm (
  VOID
  )
{
  DEBUG ((DEBUG_ERROR, "ResetSystemLib(a145f): warm reset -> cold reset (same SWRESET path)\n"));
  ResetCold ();
}

/**
  Shutdown: drop PS_HOLD so the PMIC cuts AP power, same mechanism Samsung's
  own pm_power_off/sec_reboot path uses on this register.
  If this function returns, the write did not take within the poll window.
**/
VOID
EFIAPI
ResetShutdown (
  VOID
  )
{
  UINTN   PsHoldAddress;
  UINT32  Value;

  PsHoldAddress = PMU_ALIVE_BASE + PS_HOLD_CONTROL_OFFSET;

  DEBUG ((DEBUG_ERROR, "ResetSystemLib(a145f): shutdown (PMU PS_HOLD @0x%lx)\n", PsHoldAddress));

  Value = MmioRead32 (PsHoldAddress);
  MmioWrite32 (PsHoldAddress, Value & ~PS_HOLD_EN_BIT);

  //
  // Dropping PS_HOLD removes the AP's own power rail, so most of the time
  // this call never gets to check its own result. The poll below only
  // matters on the (debug-only / bench-power) case where the rail stays up.
  //
  PollForBitOrTimeout (PsHoldAddress, Value & ~PS_HOLD_EN_BIT, "PS_HOLD shutdown");
}

/**
  Platform-specific reset: a cold reset. The reboot reason (recovery /
  bootloader / ...) is stored in PMU SYSIP_DAT0 by ExynosResetNotifyDxe,
  which receives ResetData through EFI_RESET_NOTIFICATION_PROTOCOL.
**/
VOID
EFIAPI
ResetPlatformSpecific (
  IN UINTN  DataSize,
  IN VOID   *ResetData OPTIONAL
  )
{
  DEBUG ((
    DEBUG_WARN,
    "ResetSystemLib(a145f): platform-specific reset -> cold reset\n"
    ));
  ResetCold ();
}

VOID
EFIAPI
ResetSystem (
  IN EFI_RESET_TYPE  ResetType,
  IN EFI_STATUS      ResetStatus,
  IN UINTN           DataSize,
  IN VOID            *ResetData OPTIONAL
  )
{
  switch (ResetType) {
    case EfiResetWarm:
      ResetWarm ();
      return;

    case EfiResetCold:
      ResetCold ();
      return;

    case EfiResetShutdown:
      ResetShutdown ();
      return;

    case EfiResetPlatformSpecific:
      ResetPlatformSpecific (DataSize, ResetData);
      return;

    default:
      return;
  }
}
