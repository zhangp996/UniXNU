/** @file
  KeypadDeviceLib for instantnoodlep.

  The volume keys are PM8150 SPMI GPIO 6 (up, peripheral 0xC500) and GPIO 7 (down,
  0xC600), active low. The stock kernel's /proc/interrupts decodes volume_up/down to
  SPMI peripheral 0xC5/0xC6 and APID 25/26, which fixes the addresses; the device
  tree's gpio_keys node supplies the polarity and the 15 ms debounce. Each poll reads the
  GPIO real-time status byte (offset 0x10, bit 0) and reports a scan code on a
  transition. KonaKeysDxe is the driver currently built into the firmware.
**/
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/KeypadDeviceLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <SpmiLib.h>

#define PM8150_SID        0
#define GPIO6_ADDRESS     0xC500
#define GPIO7_ADDRESS     0xC600
#define GPIO_RT_STS       0x10
#define POLL_MS           15   /* device-tree debounce hint; KonaKeysDxe polls every 10 ms */

STATIC BOOLEAN  mVolumeUp;
STATIC BOOLEAN  mVolumeDown;

STATIC
BOOLEAN
KeyDown (
  IN UINT16  Address,
  IN UINT32 *Raw
  )
{
  UINT32      Value = 0xFFFFFFFF;
  EFI_STATUS  Status;

  Status = SpmiRead (PM8150_SID, Address, GPIO_RT_STS, 1, &Value);
  if (Raw != NULL) {
    *Raw = Value;
  }
  if (EFI_ERROR (Status)) {
    return FALSE;
  }
  /* Active low: the key pulls the pin to ground. */
  return (Value & 1) == 0;
}

STATIC
VOID
PushKey (
  IN KEYPAD_RETURN_API *KeypadReturnApi,
  IN UINT16             ScanCode
  )
{
  EFI_KEY_DATA  KeyData;

  ZeroMem (&KeyData, sizeof (KeyData));
  KeyData.Key.ScanCode    = ScanCode;
  KeyData.Key.UnicodeChar = CHAR_NULL;
  KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &KeyData);
}

EFI_STATUS
EFIAPI
KeypadDeviceReset (
  IN KEYPAD_DEVICE_PROTOCOL *This
  )
{
  UINT32  Raw;

  SpmiInitialize ();
  /* Seed the state so a key already held at boot is not reported as a fresh press. */
  mVolumeUp   = KeyDown (GPIO6_ADDRESS, &Raw);
  mVolumeDown = KeyDown (GPIO7_ADDRESS, &Raw);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
KeypadDeviceGetKeys (
  IN KEYPAD_DEVICE_PROTOCOL *This,
  IN KEYPAD_RETURN_API      *KeypadReturnApi,
  IN UINT64                  Delta
  )
{
  BOOLEAN  Up;
  BOOLEAN  Down;

  if (KeypadReturnApi == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Up   = KeyDown (GPIO6_ADDRESS, NULL);
  Down = KeyDown (GPIO7_ADDRESS, NULL);

  if (Up && !mVolumeUp) {
    PushKey (KeypadReturnApi, SCAN_UP);
  }
  if (Down && !mVolumeDown) {
    PushKey (KeypadReturnApi, SCAN_DOWN);
  }
  mVolumeUp   = Up;
  mVolumeDown = Down;
  return EFI_SUCCESS;
}
