/** @file
  SafeMmioLib - see Library/SafeMmioLib.h.

  Mechanism: a handler for EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS is installed
  through the CPU architectural protocol while a probe is in flight. If the
  abort is a data abort (EC 0x25) at exactly the probed address, the handler
  sets a flag and advances ELR by one instruction (the load, always 4 bytes in
  AArch64), so execution resumes after the faulting load. Any other exception
  uninstalls the handler and returns, so the instruction re-executes and the
  platform's normal exception handler reports it.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/SafeMmioLib.h>
#include <Protocol/Cpu.h>

#define ESR_EC_SHIFT        26
#define ESR_EC_DATA_ABORT   0x25   // data abort without a change in EL

STATIC EFI_CPU_ARCH_PROTOCOL  *mCpu       = NULL;
STATIC BOOLEAN                mInstalled  = FALSE;
STATIC volatile UINTN         mArmedAddr  = 0;
STATIC volatile BOOLEAN       mFaulted    = FALSE;

STATIC
VOID
EFIAPI
AbortHandler (
  IN EFI_EXCEPTION_TYPE  Type,
  IN EFI_SYSTEM_CONTEXT  Context
  )
{
  EFI_SYSTEM_CONTEXT_AARCH64  *C;

  C = Context.SystemContextAArch64;

  if ((mArmedAddr != 0) &&
      ((C->ESR >> ESR_EC_SHIFT) == ESR_EC_DATA_ABORT) &&
      (C->FAR == mArmedAddr))
  {
    mFaulted = TRUE;
    C->ELR  += 4;           // step over the load
    return;
  }

  //
  // Not our abort: get out of the way. Returning without touching ELR makes
  // the CPU re-execute the faulting instruction, which now reaches the
  // default (reporting) handler.
  //
  mCpu->RegisterInterruptHandler (mCpu, Type, NULL);
  mInstalled = FALSE;
}

STATIC
EFI_STATUS
Install (
  VOID
  )
{
  EFI_STATUS  Status;

  if (mInstalled) {
    return EFI_SUCCESS;
  }

  if (mCpu == NULL) {
    Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&mCpu);
    if (EFI_ERROR (Status)) {
      mCpu = NULL;
      return EFI_UNSUPPORTED;
    }
  }

  Status = mCpu->RegisterInterruptHandler (
                   mCpu,
                   EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS,
                   AbortHandler
                   );
  if (EFI_ERROR (Status)) {
    return EFI_UNSUPPORTED;
  }

  mInstalled = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SafeMmioRead32 (
  IN  UINTN   Address,
  OUT UINT32  *Value
  )
{
  EFI_STATUS  Status;
  UINT32      V;

  if ((Value == NULL) || ((Address & 3) != 0) || (Address == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  *Value = 0xFFFFFFFF;

  Status = Install ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  V        = 0xFFFFFFFF;
  mFaulted = FALSE;
  mArmedAddr = Address;

  __asm__ volatile (
    "ldr %w0, [%1]\n\t"
    "dsb sy\n\t"
    : "+r" (V)
    : "r" (Address)
    : "memory"
    );

  mArmedAddr = 0;

  if (mFaulted) {
    return EFI_DEVICE_ERROR;
  }

  *Value = V;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SafeMmioLibDestructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  if (mInstalled && (mCpu != NULL)) {
    mCpu->RegisterInterruptHandler (mCpu, EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS, NULL);
    mInstalled = FALSE;
  }

  return EFI_SUCCESS;
}
