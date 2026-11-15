#ifndef SPMI_LIB_H_
#define SPMI_LIB_H_

#include <Uefi.h>

//
// Detect the PMIC-arbiter version and cache its APID map. Idempotent; returns
// EFI_UNSUPPORTED on the v1 tree-based arbiter this board does not use.
//
EFI_STATUS
SpmiInitialize (VOID);

//
// The raw PMIC-arbiter version register (0xC440000 + 0x00), for diagnostics.
//
UINT32
SpmiVersion (VOID);

//
// Diagnostics: the APID-map window the version selected, the number of entries it
// yielded, one raw entry (indexed by APID) and a PPID lookup. SpmiLookup returns
// EFI_NOT_FOUND when the map has no APID for that PPID.
//
UINT32
SpmiMapBase (VOID);

UINTN
SpmiMapCount (VOID);

UINT32
SpmiMapEntry (
  IN UINT16  Apid
  );

EFI_STATUS
SpmiLookup (
  IN  UINT16  Ppid,
  OUT UINT16  *Apid
  );

//
// Read Count bytes (1..4) from register Offset of the SPMI peripheral at Address.
// Address is the 16-bit peripheral address (for example 0xC500 for PM8150 GPIO6);
// Offset is the register inside it (0x10 is the GPIO real-time status byte).
// Sid is the SPMI slave id (0 for PM8150).
//
EFI_STATUS
SpmiRead (
  IN  UINT8   Sid,
  IN  UINT16  Address,
  IN  UINT8   Offset,
  IN  UINT8   Count,
  OUT UINT32 *Value
  );

#endif
