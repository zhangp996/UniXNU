##
# OnePlus 8 Pro (instantnoodlep) platform definition.
#
# Board values come from this unit's live device tree and bootloader variables.
# The free window between dfps_data_region and kboot_uboot_logmem is
# 0x9E400000..0x9FE00000, so the firmware volume, the DXE heap and the stack sit
# inside it.
##
[Defines]
  PLATFORM_NAME                  = instantnoodlep
  PLATFORM_GUID                  = 7A1E5C64-9B2D-4F8A-B6E1-3C5D7E9F1A2B
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/instantnoodlepPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = instantnoodlepPkg/instantnoodlep.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

  #
  # 0 = SM8250. The board reports kona / SM8250.
  #
  SOC_TYPE                       = 0

!include KonaPkg/KonaPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # Firmware volume: 4 MiB at 0x9E400000, matching Resources/Configs/instantnoodlep.toml.
  #
  gArmTokenSpaceGuid.PcdFdBaseAddress|0x9E400000
  gArmTokenSpaceGuid.PcdFdSize|0x00400000
  gArmTokenSpaceGuid.PcdFvBaseAddress|0x9E400000
  gArmTokenSpaceGuid.PcdFvSize|0x00400000

  #
  # DDR: the first measured bank, which is where the firmware runs.
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x3BB00000

  #
  # UEFI stack, inside the free window.
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x9F000000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x00040000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"OnePlus"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"8 Pro"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"instantnoodlep"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"8Pro_instantnoodlep"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"IN2020"

  #
  # Panel: 1440x3168 AMOLED, driven at 32 bpp by the framebuffer the ABL leaves.
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1080
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2376
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"

  #
  # Bring-up: keep the console to errors so the missing architectural protocols and
  # the assert that follows are the only lines on screen. Raise this once the DXE
  # dispatch order is healthy.
  #
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000000

  #
  # BdsDxe repairs driver health through FormBrowser2, which no module in this
  # package publishes; BmRepairAllControllers asserts when the protocol is missing.
  # A zero GUID is the documented way to disable the health check.
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdDriverHealthConfigureForm|{0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0}


##
# GenFv rebases the images it packs and rejects any PE whose section and file
# alignment differ. The link needs both values, and the module-type section takes
# precedence over a generic entry, so the runtime-driver alignment is pinned here.
##
[BuildOptions.common.EDKII.DXE_RUNTIME_DRIVER]
  *_CLANGPDB_*_DLINK_FLAGS = /ALIGN:0x1000 /FILEALIGN:0x1000

[LibraryClasses]
  MemoryMapLib|instantnoodlepPkg/Library/MemoryMapLib/MemoryMapLib.inf
  ConfigurationMapLib|instantnoodlepPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

  #
  # UniXNU package library, consumed by the menu application and the drivers.
  #
  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf

  #
  # Volume keys: PM8150 SPMI GPIO 6/7 through the local SPMI reader, so the menu has input
  # without a serial port.
  #
  SpmiLib|instantnoodlepPkg/Library/SpmiLib/SpmiLib.inf
  KeypadDeviceLib|instantnoodlepPkg/Library/KeypadDeviceLib/KeypadDeviceLib.inf

  #
  # Advanced Logger: the QR exporter pages the firmware log onto the panel, which is this
  # board's only log transport until a UART or USB debug link exists
  # (docs/porting/log-capture#qr-logs).
  #


[Components]
  #
  # Core and services
  #
  MdeModulePkg/Core/Dxe/DxeMain.inf
  MdeModulePkg/Universal/PCD/Dxe/Pcd.inf
  ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf
  MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf
  EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
  MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
  MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf

  #
  # Console, input and device paths
  #
  EmbeddedPkg/SimpleTextInOutSerial/SimpleTextInOutSerial.inf
  MdeModulePkg/Universal/PrintDxe/PrintDxe.inf
  MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf
  MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf

  #
  # Interrupts and timer
  #
  ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  ArmPkg/Drivers/TimerDxe/TimerDxe.inf

  #
  # Block, partition and file system: the payload arrives as a FAT ramdisk.
  #
  MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  FatPkg/EnhancedFatDxe/Fat.inf
  MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf

  #
  # Display, memory and SMBIOS from the shared Silicium package.
  #
  SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf
  # SiliciumPkg/Drivers/KeypadDeviceDxe/KeypadDeviceDxe.inf
  # instantnoodlepPkg/Drivers/KeypadProbeDxe/KeypadProbeDxe.inf  (text is cleared by the menu)
  # instantnoodlepPkg/Drivers/KeypadHudDxe/KeypadHudDxe.inf  (keys verified; disabled so the menu stays readable)
  # instantnoodlepPkg/Drivers/KeypadPixelDxe/KeypadPixelDxe.inf
  instantnoodlepPkg/Drivers/KonaKeysDxe/KonaKeysDxe.inf
  # Vendor SPMI/PMIC binaries abort without their full dependency set (data abort at FAR 0x20).
  # instantnoodlepPkg/Binaries/SPMIDxe/SPMIDxe.inf  (also aborts alone: needs the vendor platform)
  # instantnoodlepPkg/Binaries/PmicDxe/PmicDxeLa.inf
  # SiliciumPkg/Drivers/KeypadDxe/KeypadDxe.inf
  # UniXNUPkg/Driver/AdvancedLogDxe/AdvancedLogDxe.inf  (changes the GOP mode; keep the menu layout)
  # instantnoodlepPkg/Drivers/SpmiProbeDxe/SpmiProbeDxe.inf  (probe: it holds the panel)
  instantnoodlepPkg/Drivers/PanicLogDxe/PanicLogDxe.inf
  # instantnoodlepPkg/Drivers/UfsProbeDxe/UfsProbeDxe.inf  (probe: it holds the panel)
  SiliciumPkg/Drivers/RamManagerDxe/RamManagerDxe.inf
  SiliciumPkg/Drivers/SmBiosTableDxe/SmBiosTableDxe.inf
  MdeModulePkg/Universal/SmbiosDxe/SmbiosDxe.inf
  MdeModulePkg/Universal/Acpi/AcpiTableDxe/AcpiTableDxe.inf
  MdeModulePkg/Universal/Acpi/AcpiPlatformDxe/AcpiPlatformDxe.inf

  #
  # UniXNU: menu, boot option registration, FDT initrd block device and the loader.
  #
  UniXNUPkg/Application/UniXNU/UniXNU.inf
  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
  UniXNUPkg/Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.inf
  DarwinBootPkg/Driver/DarwinBoot.inf
