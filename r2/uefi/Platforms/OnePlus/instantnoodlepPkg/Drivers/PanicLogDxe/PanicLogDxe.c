/** @file
  Print the embedded panic log XNU leaves behind.

  XNU's DT panic patch writes struct embedded_panic_header plus the panic text into the
  region the board device tree names in pram/reg, and records the maximum size in
  /chosen/embedded-panic-log-size. This driver reads that region early, prints whatever
  it holds, and stops, so a panic from a previous boot is readable on a board without a
  serial console.
**/
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

#define PANIC_LOG_BASE   0x9F400000ULL
#define PANIC_LOG_SIZE   0x00080000U
#define PANIC_LOG_MAGIC  0x46554E4B  /* "FUNK" */

typedef struct {
  UINT32  Magic;
  UINT32  Crc;
  UINT32  Version;
  UINT32  Flags;
  UINT32  LogOffset;
  UINT32  LogLen;
  UINT32  StackshotOffset;
  UINT32  StackshotLen;
  UINT32  OtherLogOffset;
  UINT32  OtherLogLen;
} PANIC_LOG_HEADER;

EFI_STATUS
EFIAPI
PanicLogEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  CONST PANIC_LOG_HEADER  *Header;
  CONST CHAR8             *Text;
  UINTN                   Index;
  UINTN                   Length;

  Header = (CONST PANIC_LOG_HEADER *)(UINTN)PANIC_LOG_BASE;
  if (Header->Magic != PANIC_LOG_MAGIC) {
    return EFI_SUCCESS;
  }

  Print (L"\nPANICLOG: kernel panic from the previous boot\n");
  Print (L"PANICLOG: version %u flags 0x%x log 0x%x+0x%x stackshot 0x%x+0x%x\n",
         Header->Version, Header->Flags, Header->LogOffset, Header->LogLen,
         Header->StackshotOffset, Header->StackshotLen);

  if ((Header->LogOffset < sizeof (*Header)) || (Header->LogOffset >= PANIC_LOG_SIZE)) {
    Print (L"PANICLOG: log offset out of range\n");
    CpuDeadLoop ();
  }

  Length = Header->LogLen;
  if (Length > PANIC_LOG_SIZE - Header->LogOffset) {
    Length = PANIC_LOG_SIZE - Header->LogOffset;
  }
  if (Length > 8192) {
    Length = 8192;
  }

  Text = (CONST CHAR8 *)(UINTN)PANIC_LOG_BASE + Header->LogOffset;
  for (Index = 0; Index < Length; Index++) {
    Print (L"%c", Text[Index]);
  }
  Print (L"\nPANICLOG: end\n");

  /* Stop here so the text survives the boot menu redrawing the screen. */
  CpuDeadLoop ();
  return EFI_SUCCESS;
}
