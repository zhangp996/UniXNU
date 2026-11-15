/** @file
  Minimal SPMI keypad indicator: no console, no text, only colour.

  Four squares are drawn straight into the frame buffer and refreshed forever:

    square 0  green when the SPMI read of PM8150 GPIO 6 succeeded, red when it failed
    square 1  the same for GPIO 7
    square 2  white when GPIO 6 reads low (volume up pressed), black otherwise
    square 3  white when GPIO 7 reads low (volume down pressed), black otherwise

  Colours are unambiguous without a font or a working console.
**/
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>

#include <SpmiLib.h>

#define SQUARE  180

STATIC
VOID
Fill (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
  IN UINT32                        X,
  IN UINT8                         R,
  IN UINT8                         G,
  IN UINT8                         B
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Pixel;
  UINT32                         Y;

  Pixel.Red = R; Pixel.Green = G; Pixel.Blue = B; Pixel.Reserved = 0;
  for (Y = 0; Y < SQUARE; Y++) {
    Gop->Blt (Gop, &Pixel, EfiBltVideoFill, 0, 0, X, 20, SQUARE, SQUARE, 0);
  }
}

EFI_STATUS
EFIAPI
KeypadPixelEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop = NULL;
  EFI_STATUS                     Status;

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status) || (Gop == NULL)) {
    return EFI_SUCCESS;
  }

  SpmiInitialize ();
  for (;;) {
    UINT32  Up = 0xFFFFFFFF;
    UINT32  Down = 0xFFFFFFFF;
    EFI_STATUS  UpStatus = SpmiRead (0, 0xC600, 0x10, 1, &Up);
    EFI_STATUS  DownStatus = SpmiRead (0, 0xC700, 0x10, 1, &Down);

    Fill (Gop, 20,  (UINT8)(EFI_ERROR (UpStatus) ? 255 : 0), (UINT8)(EFI_ERROR (UpStatus) ? 0 : 255), 0);
    Fill (Gop, 220, (UINT8)(EFI_ERROR (DownStatus) ? 255 : 0), (UINT8)(EFI_ERROR (DownStatus) ? 0 : 255), 0);
    Fill (Gop, 420, (UINT8)((Up & 1) ? 0 : 255), (UINT8)((Up & 1) ? 0 : 255), (UINT8)((Up & 1) ? 0 : 255));
    Fill (Gop, 620, (UINT8)((Down & 1) ? 0 : 255), (UINT8)((Down & 1) ? 0 : 255), (UINT8)((Down & 1) ? 0 : 255));
    gBS->Stall (200000);
  }
  return EFI_SUCCESS;
}
