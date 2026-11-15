/** @file
  Volume-key state and SPMI diagnostics on the panel.

  The menu clears the text console, so everything is drawn with GOP BLT. Six rows of
  large hex digits carry the arbiter facts; the colours are fixed so the row order is
  unambiguous even without labels:

    1 white   arbiter version                    (8 digits)
    2 yellow  APID-map base << 8 | entry count   (10 digits)
    3 cyan    raw map entry for APID 0x19        (8 digits)
    4 green   raw map entry for APID 0x1A        (8 digits)
    5 orange  APID for PPID 0xC5 << 16 | 0xC6    (8 digits, FFFF = not mapped)
    6 magenta RT_STS byte for 0xC5 << 8 | 0xC6   (4 digits, EE = read error)

  Below them: left square = volume up, right = volume down (green pressed, dark
  released, magenta no APID, red other failure), with the raw byte as eight cells.
**/
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>

#include <SpmiLib.h>

#define GPIO_UP      0xC500
#define GPIO_DOWN    0xC600
#define GPIO_RT_STS  0x10

#define COLOR_OK     0x00, 0xC8, 0x00
#define COLOR_IDLE   0x1E, 0x1E, 0x1E
#define COLOR_FAIL   0xC8, 0x00, 0x00
#define COLOR_MISS   0xA0, 0x00, 0xA0
#define COLOR_WARN   0xFF, 0xC8, 0x00
#define COLOR_ON     0xFF, 0xFF, 0xFF
#define COLOR_OFF    0x00, 0x20, 0x60

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  *mGop;
STATIC EFI_EVENT                      mTimer;
STATIC EFI_STATUS                     mInit;

/*
 * 5x7 glyphs for 0..F, five bits per row (bit 4 is the leftmost column).
 */
STATIC CONST UINT8  mHexFont[16][7] = {
  { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },  /* 0 */
  { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },  /* 1 */
  { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },  /* 2 */
  { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },  /* 3 */
  { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },  /* 4 */
  { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },  /* 5 */
  { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },  /* 6 */
  { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },  /* 7 */
  { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },  /* 8 */
  { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },  /* 9 */
  { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },  /* A */
  { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },  /* B */
  { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },  /* C */
  { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E },  /* D */
  { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },  /* E */
  { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },  /* F */
};

STATIC
VOID
Fill (
  IN UINTN  X,
  IN UINTN  Y,
  IN UINTN  Width,
  IN UINTN  Height,
  IN UINT8  Red,
  IN UINT8  Green,
  IN UINT8  Blue
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Pixel;

  if ((mGop == NULL) || (Width == 0) || (Height == 0)) {
    return;
  }
  Pixel.Red      = Red;
  Pixel.Green    = Green;
  Pixel.Blue     = Blue;
  Pixel.Reserved = 0;
  mGop->Blt (mGop, &Pixel, EfiBltVideoFill, 0, 0, X, Y, Width, Height, 0);
}

STATIC
VOID
DrawGlyph (
  IN UINTN  X,
  IN UINTN  Y,
  IN UINTN  Scale,
  IN UINTN  Nibble,
  IN UINT8  Red,
  IN UINT8  Green,
  IN UINT8  Blue
  )
{
  UINTN  Row;
  UINTN  Col;
  UINT8  Bits;

  for (Row = 0; Row < 7; Row++) {
    Bits = mHexFont[Nibble & 0xF][Row];
    for (Col = 0; Col < 5; Col++) {
      if ((Bits & (1U << (4 - Col))) != 0) {
        Fill (X + Col * Scale, Y + Row * Scale, Scale, Scale, Red, Green, Blue);
      }
    }
  }
}

STATIC
VOID
DrawHex (
  IN UINTN   X,
  IN UINTN   Y,
  IN UINTN   Scale,
  IN UINT32  Value,
  IN UINTN   Digits,
  IN UINT8   Red,
  IN UINT8   Green,
  IN UINT8   Blue
  )
{
  UINTN  Index;

  for (Index = 0; Index < Digits; Index++) {
    UINTN  Shift = (Digits - 1 - Index) * 4;

    DrawGlyph (
      X + Index * 6 * Scale,
      Y,
      Scale,
      (Shift < 32) ? ((Value >> Shift) & 0xF) : 0,
      Red,
      Green,
      Blue
      );
  }
}

STATIC
VOID
DrawKey (
  IN UINTN       X,
  IN UINTN       Y,
  IN UINTN       Size,
  IN EFI_STATUS  Status,
  IN UINT32      Value
  )
{
  UINTN  Bit;
  UINTN  Cell = Size / 7;
  UINTN  Step = Cell + Cell / 4;

  if (EFI_ERROR (Status)) {
    if (Status == EFI_NOT_FOUND) {
      Fill (X, Y, Size, Size, COLOR_MISS);
    } else {
      Fill (X, Y, Size, Size, COLOR_FAIL);
    }
    for (Bit = 0; Bit < 8; Bit++) {
      Fill (X + Bit * Step, Y + Size + Step / 2, Cell, Cell, COLOR_FAIL);
    }
    return;
  }

  if ((Value & 1) == 0) {
    Fill (X, Y, Size, Size, COLOR_OK);
  } else {
    Fill (X, Y, Size, Size, COLOR_IDLE);
  }

  for (Bit = 0; Bit < 8; Bit++) {
    if (((Value >> Bit) & 1) != 0) {
      Fill (X + Bit * Step, Y + Size + Step / 2, Cell, Cell, COLOR_ON);
    } else {
      Fill (X + Bit * Step, Y + Size + Step / 2, Cell, Cell, COLOR_OFF);
    }
  }
}

STATIC
VOID
EFIAPI
Repaint (
  IN EFI_EVENT  Event,
  IN VOID      *Context
  )
{
  EFI_STATUS  UpStatus;
  EFI_STATUS  DownStatus;
  UINT32      UpValue   = 0;
  UINT32      DownValue = 0;
  UINT16      UpApid    = 0xFFFF;
  UINT16      DownApid  = 0xFFFF;
  UINT32      Apids;
  UINT32      Sts;
  UINTN       Width;
  UINTN       Height;
  UINTN       Scale;
  UINTN       Step;
  UINTN       X8;
  UINTN       X10;
  UINTN       Top;
  UINTN       Size;
  UINTN       Left;

  if ((mGop == NULL) || (mGop->Mode == NULL) || (mGop->Mode->Info == NULL)) {
    return;
  }
  Width  = mGop->Mode->Info->HorizontalResolution;
  Height = mGop->Mode->Info->VerticalResolution;

  /* Top bar: green = map loaded, magenta = no APID entries, yellow = v1 arbiter,
     red = anything else. */
  if (mInit == EFI_SUCCESS) {
    Fill (0, 0, Width, Height / 40, COLOR_OK);
  } else if (mInit == EFI_NOT_FOUND) {
    Fill (0, 0, Width, Height / 40, COLOR_MISS);
  } else if (mInit == EFI_UNSUPPORTED) {
    Fill (0, 0, Width, Height / 40, COLOR_WARN);
  } else {
    Fill (0, 0, Width, Height / 40, COLOR_FAIL);
  }

  UpStatus   = SpmiRead (0, GPIO_UP, GPIO_RT_STS, 1, &UpValue);
  DownStatus = SpmiRead (0, GPIO_DOWN, GPIO_RT_STS, 1, &DownValue);
  SpmiLookup (0xC5, &UpApid);
  SpmiLookup (0xC6, &DownApid);

  Apids = ((UINT32)UpApid << 16) | DownApid;
  Sts   = ((UINT32)(EFI_ERROR (UpStatus) ? 0xEE : (UpValue & 0xFF)) << 8) |
          (UINT32)(EFI_ERROR (DownStatus) ? 0xEE : (DownValue & 0xFF));

  Scale = Width / 72;
  if (Scale == 0) {
    Scale = 1;
  }
  Step = 9 * Scale;
  X8   = (Width - 8 * 6 * Scale) / 2;
  X10  = (Width - 10 * 6 * Scale) / 2;

  Fill (0, Height / 40, Width, 6 * Step + 10, 0, 0, 0);
  DrawHex (X8,  Step / 2,      Scale, SpmiVersion (), 8, 0xFF, 0xFF, 0xFF);
  DrawHex (X10, Step + Step / 2, Scale,
           ((SpmiMapBase () >> 8) << 8) | (SpmiMapCount () & 0xFF), 10,
           0xFF, 0xC8, 0x00);
  DrawHex (X8,  2 * Step + Step / 2, Scale, SpmiMapEntry (0x19), 8, 0x00, 0xC8, 0xFF);
  DrawHex (X8,  3 * Step + Step / 2, Scale, SpmiMapEntry (0x1A), 8, 0x00, 0xC8, 0x00);
  DrawHex (X8,  4 * Step + Step / 2, Scale, Apids, 8, 0xFF, 0x78, 0x00);
  DrawHex (X8,  5 * Step + Step / 2, Scale, Sts, 4, 0xFF, 0x00, 0xFF);

  Size = Width / 4;
  Left = Width / 12;
  Top  = 6 * Step + Height / 20;

  DrawKey (Left, Top, Size, UpStatus, UpValue);
  DrawKey (Left + (Width / 2), Top, Size, DownStatus, DownValue);
}

EFI_STATUS
EFIAPI
KeypadHudEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mGop);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mInit = SpmiInitialize ();

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Repaint, NULL, &mTimer);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = gBS->SetTimer (mTimer, TimerPeriodic, 2000000);  /* 200 ms */
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Repaint (NULL, NULL);
  return EFI_SUCCESS;
}
