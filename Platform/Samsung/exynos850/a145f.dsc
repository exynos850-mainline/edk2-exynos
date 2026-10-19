[Defines]
  PLATFORM_NAME                  = a145f
  PLATFORM_GUID                  = 8c1f4a52-1b7e-4d0a-9b6e-a145f0850a14
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010019
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = Platform/Samsung/exynos850/exynos850.fdf
  DEVICE_DXE_FV_COMPONENTS       = Platform/Samsung/exynos850/a145f.fdf.inc
  BROKEN_CNTFRQ_EL0              = 1

  # Private, read-only register/eFuse dump menu (see
  # Platform/RenegadePkg/Application/RegExplorerApp). Only built for this
  # device's own private image - drop these two lines before sharing a build.
  REG_EXPLORER_APP_SUPPORT       = TRUE

!include Platform/Samsung/exynos850/exynos850.dsc

[BuildOptions.common]
  GCC:*_*_AARCH64_CC_FLAGS = -std=gnu11 -DENABLE_SIMPLE_INIT -DREG_EXPLORER_APP_SUPPORT -DBROKEN_CNTFRQ_EL0=$(BROKEN_CNTFRQ_EL0)

[LibraryClasses.common]
  # A145F-specific map: UEFI lives at 0xa1000000 and must stay out of the
  # regions the stock bootloader / TrustZone reserve (see the library source).
  PlatformMemoryMapLib|Silicon/Samsung/Exynos850Pkg/Library/PlatformMemoryMapLibA145f/PlatformMemoryMapLibA145f.inf

  # Exception handler that validates pointers and prints ESR/FAR/registers first
  # (the stock one faults while unwinding a corrupt frame chain and hides the cause).
  DefaultExceptionHandlerLib|Silicon/Samsung/Exynos850Pkg/Library/DefaultExceptionHandlerLibSafe/DefaultExceptionHandlerLibSafe.inf

[PcdsFixedAtBuild.common]
  # DXE heap must end before ramoops (0x8fe00000)
  gSamsungTokenSpaceGuid.PcdUefiMemPoolBase|0x80C50000
  gSamsungTokenSpaceGuid.PcdUefiMemPoolSize|0x0F1B0000

  # system counter of this SoC runs at 26 MHz (DTB: clock-frequency = 0x18cba80)
  gArmTokenSpaceGuid.PcdArmArchTimerFreqInHz|26000000

  gSamsungTokenSpaceGuid.PcdMipiFrameBufferWidth|1080
  gSamsungTokenSpaceGuid.PcdMipiFrameBufferHeight|2408

  # Simple Init
  gSimpleInitTokenSpaceGuid.PcdGuiDefaultDPI|300

  gRenegadePkgTokenSpaceGuid.PcdDeviceVendor|"Samsung"
  gRenegadePkgTokenSpaceGuid.PcdDeviceProduct|"Galaxy A14 4G"
  gRenegadePkgTokenSpaceGuid.PcdDeviceCodeName|"a145f"

[Components.common]
  Silicon/Samsung/ExynosPkg/Drivers/ExynosUsbHostDxe/ExynosUsbHostDxe.inf
  # standalone tool: built to Build/.../RegDump.efi, not packed into the FV
  Silicon/Samsung/Exynos850Pkg/Application/RegDump/RegDump.inf
  MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf
  MdeModulePkg/Bus/Pci/NonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceDxe.inf
