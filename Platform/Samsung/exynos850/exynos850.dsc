## @file
#
#  Copyright (c) 2011-2015, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2016, Intel Corporation. All rights reserved.
#  Copyright (c) 2018 - 2019, Bingxing Wang. All rights reserved.
#  Copyright (c) 2022, Xilin Wu. All rights reserved.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################

[Defines]
  SOC_PLATFORM            = exynos850
  USE_PHYSICAL_TIMER      = FALSE

!include Silicon/Samsung/ExynosPkg/ExynosCommonDsc.inc

[PcdsFixedAtBuild.common]
  # memory
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0xC0000000

  gArmTokenSpaceGuid.PcdCpuVectorBaseAddress|0x80C40000
  gEmbeddedTokenSpaceGuid.PcdPrePiStackBase|0x80C00000
  gEmbeddedTokenSpaceGuid.PcdPrePiStackSize|0x00040000       # 256K stack
  gSamsungTokenSpaceGuid.PcdUefiMemPoolBase|0x80C50000       # DXE Heap base address
  gSamsungTokenSpaceGuid.PcdUefiMemPoolSize|0x0F3B0000       # UefiMemorySize

  # framebuffer
  gSamsungTokenSpaceGuid.PcdMipiFrameBufferAddress|0xfa000000
  gSamsungTokenSpaceGuid.PcdMipiFrameBufferWidth|1080
  gSamsungTokenSpaceGuid.PcdMipiFrameBufferHeight|2408
  gSamsungTokenSpaceGuid.PcdMipiFrameBufferVisibleWidth|1080
  gSamsungTokenSpaceGuid.PcdMipiFrameBufferVisibleHeight|2408

  # timer
  gArmTokenSpaceGuid.PcdArmArchTimerFreqInHz|27300000
  gArmTokenSpaceGuid.PcdArmArchTimerSecIntrNum|19
  gArmTokenSpaceGuid.PcdArmArchTimerIntrNum|20
  gArmTokenSpaceGuid.PcdArmArchTimerVirtIntrNum|27
  gArmTokenSpaceGuid.PcdArmArchTimerHypIntrNum|26

  # interrupts
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x12a01000
  gArmTokenSpaceGuid.PcdGicInterruptInterfaceBase|0x12a02000

  # misc
  gEfiMdeModulePkgTokenSpaceGuid.PcdAcpiDefaultOemRevision|0x00000850

  # cpu stuff
  gArmPlatformTokenSpaceGuid.PcdCoreCount|8
  gArmPlatformTokenSpaceGuid.PcdClusterCount|2

  #
  # SimpleInit
  #
  gSimpleInitTokenSpaceGuid.PcdDeviceTreeStore|0x80000000
  gSimpleInitTokenSpaceGuid.PcdLoggerdUseConsole|TRUE

[LibraryClasses.common]
  # ExynosCommonDsc.inc defaults both of these to ArmPkg's
  # ArmSmcPsciResetSystemLib. On the A145F the secure monitor backs PSCI
  # CPU_ON/OFF/SUSPEND (needed for SMP bring-up) but not SYSTEM_RESET/
  # SYSTEM_OFF, so that lib's SMC calls just return and the boot-menu's
  # Reboot/Power Off entries report "reset system failed". Override with
  # the PMU PS_HOLD/SWRESET implementation instead - see its ResetSystemLib.c
  # for how that was confirmed.
  HwResetSystemLib|Silicon/Samsung/Exynos850Pkg/Library/ResetSystemLib/ResetSystemLib.inf
  ResetSystemLib|Silicon/Samsung/Exynos850Pkg/Library/ResetSystemLib/ResetSystemLib.inf

  # fault-tolerant register probing (RegDump and other debug apps)
  SafeMmioLib|Silicon/Samsung/Exynos850Pkg/Library/SafeMmioLib/SafeMmioLib.inf
  KeypadDeviceImplLib|Silicon/Samsung/Exynos850Pkg/Library/KeypadDeviceImplLib/KeypadDeviceImplLib.inf
  PlatformMemoryMapLib|Silicon/Samsung/Exynos850Pkg/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.inf
  PlatformPeiLib|Silicon/Samsung/Exynos850Pkg/Library/PlatformPeiLib/PlatformPeiLib.inf
  PlatformPrePiLib|Silicon/Samsung/Exynos850Pkg/Library/PlatformPrePiLib/PlatformPrePiLib.inf
  MsPlatformDevicesLib|Silicon/Samsung/Exynos850Pkg/Library/MsPlatformDevicesLib/MsPlatformDevicesLib.inf
  SOCSmbiosInfoLib|Silicon/Samsung/Exynos850Pkg/Library/SOCSmbiosInfoLib/SOCSmbiosInfoLib.inf

[Components.common]
  # stores recovery/bootloader reboot requests in PMU SYSIP_DAT0 for S-Boot
  Silicon/Samsung/Exynos850Pkg/Drivers/ExynosResetNotifyDxe/ExynosResetNotifyDxe.inf
  Silicon/Samsung/Exynos850Pkg/Drivers/Exynos850MmcBlockIoDxe/Exynos850MmcBlockIoDxe.inf
  Silicon/Samsung/Exynos850Pkg/Drivers/Exynos850SdBlockIoDxe/Exynos850SdBlockIoDxe.inf
  Platform/RenegadePkg/Drivers/Ext4Dxe/Ext4Dxe.inf
