/** @file
  Fault-tolerant 32-bit MMIO read for probing registers that the non-secure
  world may not be allowed to touch (TrustZone-protected SFRs, gated blocks).

  On the Exynos850 such a read raises a *synchronous external abort* (ESR EC
  0x25, DFSC 0x10) instead of returning data, which kills a UEFI application
  with the exception dump. SafeMmioRead32() catches exactly that abort for the
  one address being probed, skips the load, and reports failure.

  Limits: it cannot help if the bus hangs or the abort arrives asynchronously
  (SError). Reads only; do not use it for normal driver register access.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef SAFE_MMIO_LIB_H_
#define SAFE_MMIO_LIB_H_

#include <Uefi.h>

/**
  Read a 32-bit register without dying on an external abort.

  @param[in]  Address  Physical address, 4-byte aligned.
  @param[out] Value    Receives the value on success; set to 0xFFFFFFFF on failure.

  @retval EFI_SUCCESS            The register was read.
  @retval EFI_DEVICE_ERROR       The access aborted (not accessible from here).
  @retval EFI_INVALID_PARAMETER  Value is NULL or Address is unaligned.
  @retval EFI_UNSUPPORTED        The CPU exception handler could not be hooked.
**/
EFI_STATUS
EFIAPI
SafeMmioRead32 (
  IN  UINTN   Address,
  OUT UINT32  *Value
  );

#endif
