/** @file
  Read-only register dump for the Samsung Galaxy A14 4G (SM-A145F, Exynos850).

  Port of the U-Boot "regdump" bootmenu entry (0004-a145f-regdump). Addresses
  come from the stock live DTB. Only MmioRead32 is used: it cannot change
  silicon state.

  Usage (UEFI Shell):
    RegDump.efi                 dump all blocks
    RegDump.efi chipid pmu      dump only the named blocks
    RegDump.efi -h              help
    RegDump.efi > fs0:\regdump.txt     save the output to the USB stick

  Notes
  - Each block's header is printed BEFORE it is read. If a block's clock/power
    domain is gated, the bus access can hang the phone; the last header on
    screen then tells you which block it was (reboot and skip it by name).
  - This is NOT the eFUSE/OTP controller. On Exynos850 that is TrustZone-only
    and not mapped to the non-secure world.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/SafeMmioLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/ShellParameters.h>

typedef struct {
  CONST CHAR16  *Key;
  CONST CHAR16  *Title;
  CONST CHAR16  *Note;
  UINT32        Base;
  UINT32        Words;
} REG_BLOCK;

STATIC CONST REG_BLOCK  mBlocks[] = {
  { L"chipid", L"CHIPID",
    L"product/package ID and revision (samsung,s5e3830-chipid)",
    0x10000000, 0x40 },
  { L"pmu",    L"PMU_ALIVE (first 0x200 bytes)",
    L"reset-reason / status registers; the rest of the 64 KiB block is power-domain control",
    0x11860000, 0x80 },
  { L"tmu",    L"TMU (shared by the three thermal probes)",
    L"includes TRIMINFO; DTB has samsung,tmu_efuse_value = 0x37 for all three probes",
    0x10070000, 0x80 },
  { L"adc",    L"ADC (exynos-adc-v3)",
    L"controller registers only, nothing is sampled",
    0x11C40000, 0x40 },
};

STATIC
VOID
DumpBlock (
  IN CONST REG_BLOCK  *B
  )
{
  UINT32  Index;
  UINT32  Value;

  //
  // Header first: if the read below hangs the bus, this is the last thing seen.
  //
  Print (L"===== %s @ 0x%08X =====\n", B->Title, B->Base);
  Print (L"  (%s)\n", B->Note);

  for (Index = 0; Index < B->Words; Index++) {
    if ((Index % 4) == 0) {
      Print (L"%08X:", B->Base + (Index * 4));
    }

    //
    // Some words (e.g. CHIPID+0x14) raise a synchronous external abort when
    // read from the non-secure world. SafeMmioRead32 turns that into a status
    // so the dump continues instead of taking the firmware down.
    //
    if (EFI_ERROR (SafeMmioRead32 ((UINTN)B->Base + (Index * 4), &Value))) {
      Print (L"   FAULT  ");
    } else {
      Print (L" %08X", Value);
    }
    if ((Index % 4) == 3) {
      Print (L"\n");
    }
  }

  if ((B->Words % 4) != 0) {
    Print (L"\n");
  }
}

STATIC
VOID
Usage (
  VOID
  )
{
  UINTN  I;

  Print (L"RegDump - read-only SM-A145F register dump\n");
  Print (L"  RegDump.efi [all | block ...]\n  blocks:");
  for (I = 0; I < ARRAY_SIZE (mBlocks); I++) {
    Print (L" %s", mBlocks[I].Key);
  }

  Print (L"\n  Save output:  RegDump.efi > fs0:\\regdump.txt\n");
}

EFI_STATUS
EFIAPI
RegDumpMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_SHELL_PARAMETERS_PROTOCOL  *Params;
  EFI_STATUS                     Status;
  BOOLEAN                        Selected[ARRAY_SIZE (mBlocks)];
  BOOLEAN                        Any;
  UINTN                          Arg;
  UINTN                          I;

  Params = NULL;
  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiShellParametersProtocolGuid,
                  (VOID **)&Params
                  );

  ZeroMem (Selected, sizeof (Selected));
  Any = FALSE;

  if (!EFI_ERROR (Status) && (Params != NULL)) {
    for (Arg = 1; Arg < Params->Argc; Arg++) {
      CONST CHAR16  *A = Params->Argv[Arg];

      if ((StrCmp (A, L"-h") == 0) || (StrCmp (A, L"--help") == 0) || (StrCmp (A, L"-?") == 0)) {
        Usage ();
        return EFI_SUCCESS;
      }

      if (StrCmp (A, L"all") == 0) {
        for (I = 0; I < ARRAY_SIZE (mBlocks); I++) {
          Selected[I] = TRUE;
        }

        Any = TRUE;
        continue;
      }

      for (I = 0; I < ARRAY_SIZE (mBlocks); I++) {
        if (StrCmp (A, mBlocks[I].Key) == 0) {
          Selected[I] = TRUE;
          Any         = TRUE;
          break;
        }
      }

      if (I == ARRAY_SIZE (mBlocks)) {
        Print (L"unknown block '%s'\n", A);
        Usage ();
        return EFI_INVALID_PARAMETER;
      }
    }
  }

  if (!Any) {
    for (I = 0; I < ARRAY_SIZE (mBlocks); I++) {
      Selected[I] = TRUE;
    }
  }

  for (I = 0; I < ARRAY_SIZE (mBlocks); I++) {
    if (Selected[I]) {
      DumpBlock (&mBlocks[I]);
    }
  }

  Print (L"done. True eFUSE/OTP controller is TrustZone-only, not shown.\n");
  return EFI_SUCCESS;
}
