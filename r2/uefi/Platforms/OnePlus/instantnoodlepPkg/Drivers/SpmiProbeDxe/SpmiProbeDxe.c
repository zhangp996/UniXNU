/** @file
  Minimal SPMI PMIC arbiter probe for instantnoodlep.

  The volume keys are PM8150 SPMI GPIO 6 and 7 (device DTS: gpio_keys pinctrl lives under
  /soc/qcom,spmi@c440000). Mu-Silicium carries no SPMI driver, so the firmware cannot read
  the panel's input yet. This probe reads the arbiter version, resolves the GPIO peripheral
  APID from the configuration mapping table and dumps its registers, which is the evidence
  needed before a keypad device can be written.
**/
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/IoLib.h>

#define SPMI_CORE_BASE   0x0C440000ULL
#define SPMI_OBSVR_BASE  0x0E600000ULL
#define SPMI_CNFG_BASE   0x0C40A000ULL

#define ARB_VERSION      0x0000
#define CH_CMD           0x0000
#define CH_STATUS        0x0008
#define CH_DATA          0x0010

#define APID_MAP_OFFSET  0x0B00
#define APID_VALID       0x8000
#define STATUS_DONE      0x1
#define STATUS_FAIL      0x2
#define EE               0

STATIC
UINT16
PpidToApid (
  IN UINT16  Ppid,
  OUT UINT32 *Raw
  )
{
  UINT32  Entry;

  Entry = MmioRead32 ((UINTN)(SPMI_CNFG_BASE + APID_MAP_OFFSET + 4 * Ppid));
  *Raw  = Entry;
  if ((Entry & APID_VALID) == 0) {
    return 0xFFFF;
  }
  return (UINT16)(Entry & ~APID_VALID);
}

STATIC
EFI_STATUS
SpmiRead (
  IN  UINT16  Apid,
  IN  UINT8   Offset,
  IN  UINT8   Count,
  OUT UINT32 *Value
  )
{
  UINTN   Channel = (UINTN)(0x8000ULL * Apid);
  UINT32  Command;
  UINT32  Status;
  UINT32  Spins;

  Command = ((UINT32)(Count - 1) & 0x7) | ((UINT32)Offset << 4);
  MmioWrite32 ((UINTN)(SPMI_OBSVR_BASE + Channel + CH_CMD), Command);
  for (Spins = 0; Spins < 1000; Spins++) {
    Status = MmioRead32 ((UINTN)(SPMI_OBSVR_BASE + Channel + CH_STATUS));
    if ((Status & STATUS_DONE) != 0) {
      break;
    }
    gBS->Stall (10);
  }
  if ((Status & STATUS_DONE) == 0) {
    return EFI_TIMEOUT;
  }
  if ((Status & STATUS_FAIL) != 0) {
    return EFI_DEVICE_ERROR;
  }
  *Value = MmioRead32 ((UINTN)(SPMI_OBSVR_BASE + Channel + CH_DATA));
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SpmiProbeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32  Version;
  UINT32  Raw6;
  UINT32  Raw7;
  UINT16  Apid6;
  UINT16  Apid7;
  UINTN   Index;

  /* TRUE depex dispatches this driver before the console exists, and Print asserts on a
   * NULL ConOut. Wait for the console instead of returning: the probe then runs early
   * enough to own the panel before the menu draws. */
  {
    UINTN  Waited = 0;

    while ((gST->ConOut == NULL) && (Waited < 3000)) {
      gBS->Stall (10000);
      Waited++;
    }
    if (gST->ConOut == NULL) {
      return EFI_SUCCESS;
    }
  }

  Version = MmioRead32 ((UINTN)(SPMI_CORE_BASE + ARB_VERSION));
  Print (L"\nSPMIPROBE: arbiter version 0x%08x\n", Version);

  /* PM8150 GPIO tile starts at 0xC000; each GPIO peripheral is 0x100 apart. */
  Apid6 = PpidToApid ((UINT16)((0 << 8) | 0xC6), &Raw6);
  Apid7 = PpidToApid ((UINT16)((0 << 8) | 0xC7), &Raw7);
  Print (L"SPMIPROBE: GPIO6 map 0x%08x apid 0x%04x, GPIO7 map 0x%08x apid 0x%04x\n",
         Raw6, Apid6, Raw7, Apid7);
  if ((Apid6 == 0xFFFF) || (Apid7 == 0xFFFF)) {
    Print (L"SPMIPROBE: mapping table miss, needs another offset base\n");
    return EFI_SUCCESS;
  }

  /* Dump every candidate register once, then watch for changes. A single photograph then
   * carries the whole state, and pressing a volume key shows which register tracks it. */
  {
    UINT8   Regs[] = { 0x00, 0x04, 0x08, 0x0C, 0x10, 0x14 };
    UINT32  Before6[6];
    UINT32  Before7[6];
    UINTN   R;

    Print (L"SPMIPROBE: dump GPIO6/GPIO7 registers\n");
    for (R = 0; R < 6; R++) {
      Before6[R] = 0xFFFFFFFF;
      Before7[R] = 0xFFFFFFFF;
      SpmiRead (Apid6, Regs[R], 2, &Before6[R]);
      SpmiRead (Apid7, Regs[R], 2, &Before7[R]);
      Print (L"SPMIPROBE: [0x%02x] GPIO6=0x%08x GPIO7=0x%08x\n", Regs[R], Before6[R], Before7[R]);
    }

    Print (L"SPMIPROBE: watch 60 s, press the volume keys now\n");
    for (Index = 0; Index < 120; Index++) {
      for (R = 0; R < 6; R++) {
        UINT32  Now = 0;

        if (!EFI_ERROR (SpmiRead (Apid6, Regs[R], 2, &Now)) && (Now != Before6[R])) {
          Print (L"SPMIPROBE: GPIO6[0x%02x] 0x%08x -> 0x%08x\n", Regs[R], Before6[R], Now);
          Before6[R] = Now;
        }
        if (!EFI_ERROR (SpmiRead (Apid7, Regs[R], 2, &Now)) && (Now != Before7[R])) {
          Print (L"SPMIPROBE: GPIO7[0x%02x] 0x%08x -> 0x%08x\n", Regs[R], Before7[R], Now);
          Before7[R] = Now;
        }
      }
      gBS->Stall (500000);
    }
    Print (L"SPMIPROBE: done, holding\n");
  }
  CpuDeadLoop ();
  return EFI_SUCCESS;
}
