/** @file
  Polled SPMI PMIC-arbiter reader for instantnoodlep.

  The arbiter is /soc/qcom,spmi@c440000: core 0xC440000, channel window 0xC600000,
  observer window 0xE600000 and configuration window 0xC40A000. The register layout
  is version dependent (Linux drivers/spmi/spmi-pmic-arb.c):

    v2/v3  APID map 0x800 + 4*APID   observer 0x1000*EE  + 0x8000*APID
    v5     APID map 0x900 + 4*APID   observer 0x10000*EE + 0x80*APID
    v7     APID map 0x2000 + 4*APID  observer 0x8000*EE  + 0x20*APID

  The APID map lives in the core window (0xC440000, 0x1100 bytes), not in the
  configuration window: Linux reads it through pmic_arb->core. cnfg (0xC40A000)
  holds only the v1 PPID decision tree at 0xB00, which this reader does not use.
  Reading 0x900 from cnfg returns an unrelated owner/status table (all 0x04).

  The map is indexed by APID and stores the PPID in bits 8..19. It is not a
  PPID-indexed array: the previous implementation read 0xB00 + 4*PPID, which is
  the v1 decision-tree root and meaningless on this arbiter. A read command also
  carries the read opcode (15) in bits 27..31 and completes into RDATA0 (0x18),
  not into the write-data register at 0x10.
**/
#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "SpmiLib.h"

#define SPMI_CORE_BASE   0x0C440000ULL
#define SPMI_OBSVR_BASE  0x0E600000ULL

#define SPMI_OBSVR_SIZE  0x00100000ULL

#define ARB_VERSION      0x0000
#define ARB_FEATURES     0x0004
#define ARB_APID_COUNT   0x000007FF

#define CH_CMD           0x0000
#define CH_STATUS        0x0008
#define CH_RDATA0        0x0018

#define SPMI_OP_READ     15
#define SPMI_MAP_PPID(V) (((V) >> 8) & 0xFFF)

#define STATUS_DONE      0x1
#define STATUS_FAILURE   0x2
#define STATUS_DENIED    0x4
#define STATUS_DROPPED   0x8

#define EE               0
#define MAX_APIDS        128

typedef struct {
  UINT16  Ppid;
  UINT16  Apid;
} SPMI_MAP_ENTRY;

STATIC CONST UINT32        mMapBases[] = { 0x0800, 0x0900, 0x2000 };

STATIC UINT32              mVersion;
STATIC UINT32              mMapBase;
STATIC UINT32              mObsEeStep;
STATIC UINT32              mObsApidStep;
STATIC UINTN               mApidCount;
STATIC UINTN               mMapCount;
STATIC SPMI_MAP_ENTRY      mMap[MAX_APIDS];
STATIC BOOLEAN             mInitialized;
STATIC EFI_STATUS          mInitStatus;

STATIC
VOID
LoadMap (
  IN UINT32  Base
  )
{
  UINTN   Index;
  UINT32  Entry;

  mMapCount = 0;
  for (Index = 0; (Index < mApidCount) && (mMapCount < MAX_APIDS); Index++) {
    Entry = MmioRead32 ((UINTN)(SPMI_CORE_BASE + Base + 4 * Index));
    if (Entry == 0) {
      continue;
    }
    /* A dead bus reads back 0xFFFFFFFF, which would otherwise look like PPID 0xFFF. */
    if (SPMI_MAP_PPID (Entry) == 0xFFF) {
      continue;
    }
    mMap[mMapCount].Ppid = (UINT16)SPMI_MAP_PPID (Entry);
    mMap[mMapCount].Apid = (UINT16)Index;
    mMapCount++;
  }
}

EFI_STATUS
SpmiInitialize (VOID)
{
  UINT32  Features;
  UINTN   Index;
  UINT32  Version;

  if (mInitialized) {
    return mInitStatus;
  }
  mInitialized = TRUE;

  Version = MmioRead32 ((UINTN)(SPMI_CORE_BASE + ARB_VERSION));
  mVersion = Version;
  if ((Version == 0) || (Version == 0xFFFFFFFFU)) {
    mInitStatus = EFI_DEVICE_ERROR;
    return mInitStatus;
  }
  if (Version >= 0x70000000) {
    mMapBase     = 0x2000;
    mObsEeStep   = 0x8000;
    mObsApidStep = 0x20;
  } else if (Version >= 0x50000000) {
    mMapBase     = 0x0900;
    mObsEeStep   = 0x10000;
    mObsApidStep = 0x80;
  } else if (Version >= 0x20010000) {
    mMapBase     = 0x0800;
    mObsEeStep   = 0x1000;
    mObsApidStep = 0x8000;
  } else {
    /* v1 uses a PPID decision tree, not the APID map this reader implements. */
    mInitStatus = EFI_UNSUPPORTED;
    return mInitStatus;
  }

  Features = MmioRead32 ((UINTN)(SPMI_CORE_BASE + ARB_FEATURES));
  mApidCount = Features & ARB_APID_COUNT;
  if ((mApidCount == 0) || (mApidCount > MAX_APIDS)) {
    mApidCount = MAX_APIDS;
  }

  LoadMap (mMapBase);
  if (mMapCount == 0) {
    /* A misdetected version would also explain an empty chosen window: try the
       other two layouts before giving up, but keep the observer strides above. */
    for (Index = 0; Index < ARRAY_SIZE (mMapBases); Index++) {
      if (mMapBases[Index] == mMapBase) {
        continue;
      }
      LoadMap (mMapBases[Index]);
      if (mMapCount != 0) {
        mMapBase = mMapBases[Index];
        break;
      }
    }
  }

  mInitStatus = (mMapCount != 0) ? EFI_SUCCESS : EFI_NOT_FOUND;
  return mInitStatus;
}

UINT32
SpmiVersion (VOID)
{
  return mVersion;
}

STATIC
BOOLEAN
LookupApid (
  IN  UINT16  Ppid,
  OUT UINT16  *Apid
  );

UINT32
SpmiMapBase (VOID)
{
  SpmiInitialize ();
  return mMapBase;
}

UINTN
SpmiMapCount (VOID)
{
  SpmiInitialize ();
  return mMapCount;
}

UINT32
SpmiMapEntry (
  IN UINT16  Apid
  )
{
  SpmiInitialize ();
  if (Apid >= mApidCount) {
    return 0;
  }
  return MmioRead32 ((UINTN)(SPMI_CORE_BASE + mMapBase + 4 * (UINTN)Apid));
}

EFI_STATUS
SpmiLookup (
  IN  UINT16  Ppid,
  OUT UINT16  *Apid
  )
{
  EFI_STATUS  Status;

  if (Apid == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Status = SpmiInitialize ();
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return LookupApid (Ppid, Apid) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

STATIC
BOOLEAN
LookupApid (
  IN  UINT16  Ppid,
  OUT UINT16  *Apid
  )
{
  UINTN  Index;

  for (Index = 0; Index < mMapCount; Index++) {
    if (mMap[Index].Ppid == Ppid) {
      *Apid = mMap[Index].Apid;
      return TRUE;
    }
  }
  return FALSE;
}

EFI_STATUS
SpmiRead (
  IN  UINT8   Sid,
  IN  UINT16  Address,
  IN  UINT8   Offset,
  IN  UINT8   Count,
  OUT UINT32 *Value
  )
{
  EFI_STATUS  Status;
  UINT16      Ppid;
  UINT16      Apid;
  UINTN       Channel;
  UINT32      Command;
  UINT32      State = 0;
  UINTN       Spins;

  if (Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if ((Count == 0) || (Count > 4)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = SpmiInitialize ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* Address is the 16-bit peripheral address; its high byte is the PPID. */
  Ppid = (UINT16)(((UINT16)Sid << 8) | ((Address >> 8) & 0xFF));
  if (!LookupApid (Ppid, &Apid)) {
    return EFI_NOT_FOUND;
  }

  Channel = (UINTN)(mObsEeStep * EE + mObsApidStep * Apid);
  if ((Channel + CH_RDATA0 + sizeof (UINT32)) > SPMI_OBSVR_SIZE) {
    return EFI_DEVICE_ERROR;
  }

  Command = ((UINT32)SPMI_OP_READ << 27) | ((UINT32)Offset << 4) | (UINT32)(Count - 1);
  MmioWrite32 ((UINTN)(SPMI_OBSVR_BASE + Channel + CH_CMD), Command);
  for (Spins = 0; Spins < 1000; Spins++) {
    State = MmioRead32 ((UINTN)(SPMI_OBSVR_BASE + Channel + CH_STATUS));
    if ((State & STATUS_DONE) != 0) {
      break;
    }
    gBS->Stall (10);
  }
  if ((State & STATUS_DONE) == 0) {
    return EFI_TIMEOUT;
  }
  if ((State & (STATUS_FAILURE | STATUS_DENIED | STATUS_DROPPED)) != 0) {
    return EFI_DEVICE_ERROR;
  }

  *Value = MmioRead32 ((UINTN)(SPMI_OBSVR_BASE + Channel + CH_RDATA0));
  if (Count < 4) {
    *Value &= (1U << (Count * 8)) - 1;
  }
  return EFI_SUCCESS;
}
