[Defines]
  PLATFORM_NAME                  = a13
  PLATFORM_GUID                  = 4bff5247-a89a-4a4b-91ea-80d21fa7ff16
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010019
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = Platform/Samsung/exynos850/exynos850.fdf
  DEVICE_DXE_FV_COMPONENTS       = Platform/Samsung/exynos850/exynos850.fdf.inc

!include Platform/Samsung/exynos850/exynos850.dsc

[BuildOptions.common]
  GCC:*_*_AARCH64_CC_FLAGS = -DENABLE_SIMPLE_INIT

[PcdsFixedAtBuild.common]
  gSamsungTokenSpaceGuid.PcdMipiFrameBufferWidth|1080
  gSamsungTokenSpaceGuid.PcdMipiFrameBufferHeight|2408

  # Simple Init
  gSimpleInitTokenSpaceGuid.PcdGuiDefaultDPI|300

  gRenegadePkgTokenSpaceGuid.PcdDeviceVendor|"Samsung"
  gRenegadePkgTokenSpaceGuid.PcdDeviceProduct|"Galaxy A13"
  gRenegadePkgTokenSpaceGuid.PcdDeviceCodeName|"a13"
