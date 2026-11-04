## @file
# BootLoaderSelector clean EDK2 platform file.
# Credits: ©DNMTechLabs / dnmtechlabs@gmail.com
##

[Defines]
  PLATFORM_NAME                  = BootLoaderSelector
  PLATFORM_GUID                  = 3F71A792-6E03-4D84-9F1D-F3C81EC80F44
  PLATFORM_VERSION = 2.33
  DSC_SPECIFICATION              = 0x00010006
  OUTPUT_DIRECTORY               = Build/BootLoaderSelector
  SUPPORTED_ARCHITECTURES        = X64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT

[LibraryClasses]
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf

[Components]
  BootLoaderSelectorPkg/BootLoaderSelector/BootLoaderSelector.inf
  AuditTests/UefiSmokeChild/SmokeChild.inf

[BuildOptions]
  # BaseLib carries its own narrowly scoped GCC 10.2 DSE workaround. Keep
  # dead-store elimination enabled for the application and all other libraries.
  GCC:*_*_X64_CC_FLAGS = -ffile-prefix-map=$(WORKSPACE)=.
