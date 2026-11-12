// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef DARWIN_BOOT_VIDEO_H_
#define DARWIN_BOOT_VIDEO_H_

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/GraphicsOutput.h>

STATIC
EFI_STATUS
DarwinBootVideoFromGop (CONST EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop, UINT64 Video[6])
{
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
  UINT64 RowBytes;
  UINT64 Bytes;

  ZeroMem (Video, 6 * sizeof (*Video));
  if ((Gop->Mode == NULL) || (Gop->Mode->Info == NULL) ||
      (Gop->Mode->SizeOfInfo < sizeof (*Info))) { return EFI_COMPROMISED_DATA; }
  Info = Gop->Mode->Info;
  if (Info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor) { return EFI_UNSUPPORTED; }
  RowBytes = (UINT64)Info->PixelsPerScanLine * 4;
  if ((Info->HorizontalResolution == 0) || (Info->VerticalResolution == 0) ||
      (Info->PixelsPerScanLine < Info->HorizontalResolution) ||
      (Info->VerticalResolution > MAX_UINT64 / RowBytes)) { return EFI_COMPROMISED_DATA; }
  Bytes = RowBytes * Info->VerticalResolution;
  if ((Gop->Mode->FrameBufferBase == 0) || (Gop->Mode->FrameBufferBase & 3) ||
      (Gop->Mode->FrameBufferSize < Bytes) ||
      (Gop->Mode->FrameBufferBase > MAX_UINT64 - Gop->Mode->FrameBufferSize)) { return EFI_COMPROMISED_DATA; }
  Video[0] = Gop->Mode->FrameBufferBase;
  Video[1] = 1;
  Video[2] = RowBytes;
  Video[3] = Info->HorizontalResolution;
  Video[4] = Info->VerticalResolution;
  //
  // Bring-up: XNU maps the panel through ml_io_map_unmappable and the kernel map
  // cannot give it the full 10 MiB (1080 x 2376 x 4) at the point the console comes
  // up. Report the top 480 rows so the mapping is 480 x 4320 = 2 MiB; the stride
  // stays the panel's, so the console and the panic screen draw in the top band.
  //
  // Full panel: keep the vertical resolution the GOP reports (1080x2376).  The earlier
  // bring-up clamped this to 480 rows because the kernel could not map the full 10 MiB at
  // console time; a later probe mapped 10.4 MiB successfully, so the clamp is lifted.
  Video[5] = 32;
  return EFI_SUCCESS;
}

#endif
