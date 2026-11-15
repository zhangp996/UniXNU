#include <Uefi.h>
#include <Library/ConfigurationMapLib.h>

//
// Board configuration consumed by Mu-Silicium drivers through
// LocateConfigurationEntry32/64. This port selects no vendor DXE driver, so the
// table carries only entries of the generic modules it builds; each one is a value
// measured on this board rather than copied from another device.
//
// Name, Value
//
STATIC EFI_CONFIGURATION_ENTRY_DESCRIPTOR gConfigurationDescriptor[] = {
  {"Panel Width",  1440},
  {"Panel Height", 3168},
  {"Touch IRQ GPIO", 39},
  {"Touch Reset GPIO", 38},
};

VOID
EFIAPI
GetConfigurationMap (
  OUT EFI_CONFIGURATION_ENTRY_DESCRIPTOR **ConfigurationDescriptor,
  OUT UINT8                               *ConfigurationDescriptorCount)
{
  *ConfigurationDescriptor      = gConfigurationDescriptor;
  *ConfigurationDescriptorCount = (UINT8)(sizeof (gConfigurationDescriptor) / sizeof (gConfigurationDescriptor[0]));
}
