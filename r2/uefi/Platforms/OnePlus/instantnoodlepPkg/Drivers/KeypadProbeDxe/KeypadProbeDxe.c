/** @file
  Keypad debug probe: print the PM8150 GPIO real-time status the keypad library reads.

  The library reports nothing on the device, so this driver shows the SPMI mapping result
  and the raw 0x10 status byte for volume up (GPIO 6) and down (GPIO 7). Pressing a key
  while it runs shows whether the value moves at all.
**/
#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <SpmiLib.h>

EFI_STATUS
EFIAPI
KeypadProbeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINTN       Waited = 0;
  UINTN       Round;
  EFI_STATUS  Status6;
  EFI_STATUS  Status7;

  while ((gST->ConOut == NULL) && (Waited < 6000)) {
    gBS->Stall (10000);
    Waited++;
  }
  if (gST->ConOut == NULL) {
    return EFI_SUCCESS;
  }

  SpmiInitialize ();
  Print (L"\nKEYPADDBG: waiting for the console took %u ms\n", (UINT32)(Waited * 10));
  Print (L"KEYPADDBG: press the volume keys while this runs\n");
  for (Round = 0; Round < 20; Round++) {
    UINT32  Value6 = 0xDEADBEEF;
    UINT32  Value7 = 0xDEADBEEF;

    Status6 = SpmiRead (0, 0xC600, 0x10, 1, &Value6);
    Status7 = SpmiRead (0, 0xC700, 0x10, 1, &Value7);
    Print (L"KEYPADDBG: %u up %r 0x%02x  down %r 0x%02x\n",
           (UINT32)Round,
           Status6, Value6 & 0xFF,
           Status7, Value7 & 0xFF);
    gBS->Stall (500000);
  }
  Print (L"KEYPADDBG: done, returning to BDS\n");
  return EFI_SUCCESS;
}
