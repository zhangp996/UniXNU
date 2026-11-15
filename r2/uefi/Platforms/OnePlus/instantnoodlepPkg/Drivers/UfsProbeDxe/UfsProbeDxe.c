/** @file
  Drive the UFS host far enough to read the device capacity, then stop.

  The layout follows ShengUfs's RootCommands.h so the same sequence can move into the
  kernel driver unchanged:

    buffer + 0x0000  UTRD (32 bytes), OCS in the low byte of word 2
    buffer + 0x1000  command UPIU
    buffer + 0x1200  response UPIU
    buffer + 0x2000  data area

  Words 4/5 of the UTRD carry the command UPIU address, word 6 the response length and
  offset in 4-byte units, word 7 the PRDT count and offset in 4-byte units.
**/
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/ArmLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

#include "UfsPhyTables.h"

#define UFS_HOST_BASE   0x01D84000ULL
#define UFS_PHY_BASE    0x01D87000ULL

#define UFS_IS          0x0020
#define UFS_IE          0x0024
#define UFS_HCS         0x0030
#define UFS_HCE         0x0034
#define UFS_VER         0x0000
#define UFS_CAP         0x0004
#define UFS_UTRLBA      0x0050
#define UFS_UTRLBAU     0x0054
#define UFS_UTRLDBR     0x0058
#define UFS_UTRLRSR     0x0060   // Qualcomm's DWC layout, not the UFSHCI 0x5c
#define UFS_UICCMD      0x0090
#define UFS_UICCMDARG1  0x0094
#define UFS_UICCMDARG2  0x0098

#define GCC_BASE        0x00100000ULL
#define GCC_UFS_BCR     0x00077000
#define GCC_UFS_AXI     0x00077010
#define GCC_UFS_AHB     0x00077018
#define GCC_UFS_TX0     0x0007701C
#define GCC_UFS_RX0     0x00077020
#define GCC_UFS_UNIPRO  0x0007705C
#define GCC_UFS_ICE     0x00077064
#define GCC_UFS_AUX     0x0007709C
#define GCC_UFS_RX1     0x000770B8
#define GCC_UFS_AGGR    0x000770CC

#define UIC_LINK_STARTUP 0x16

#define URD_OFFSET      0x0000
#define UCD_OFFSET      0x1000
#define RSP_OFFSET      0x0200
#define PRDT_OFFSET     0x0400
#define DATA_OFFSET     0x2000
#define BUFFER_PAGES    8

static UINT8  *mBuffer;
static UINT64  mPhys;
static UINT64  mUtRdBase;

static VOID
SubmitCommand (
  IN  UINT8   Direction,
  IN  UINT32  DataBytes
  )
{
  UINT32  *Words;

  Words = (UINT32 *)(UINTN)mUtRdBase;
  ZeroMem ((VOID *)(UINTN)mUtRdBase, 32);
  ZeroMem (mBuffer + UCD_OFFSET + PRDT_OFFSET, 16);
  /* Bit 24 is part of the command descriptor for a data transfer: without it the
   * controller completes the command with OCS 0 and never runs the PRDT. */
  Words[0] = 0x11000000U | ((UINT32)Direction << 25);
  Words[2] = 0x0FU;
  Words[4] = (UINT32)(mPhys + UCD_OFFSET);
  Words[5] = (UINT32)((mPhys + UCD_OFFSET) >> 32);
  Words[6] = (512U >> 2) | ((RSP_OFFSET >> 2) << 16);
  Words[7] = (DataBytes ? 1U : 0U) | ((PRDT_OFFSET >> 2) << 16);

  if (DataBytes != 0) {
    UINT32  *Prd;
    Prd    = (UINT32 *)(mBuffer + UCD_OFFSET + PRDT_OFFSET);
    Prd[0] = (UINT32)(mPhys + DATA_OFFSET);
    Prd[1] = (UINT32)((mPhys + DATA_OFFSET) >> 32);
    Prd[2] = 0;
    Prd[3] = DataBytes - 1;
    Prd[4] = 0;
    Prd[5] = 0;
  }

  WriteBackDataCacheRange (mBuffer, BUFFER_PAGES * 4096);
  WriteBackDataCacheRange ((VOID *)(UINTN)mUtRdBase, 32);
  ArmDataSynchronizationBarrier ();
  ArmInstructionSynchronizationBarrier ();
  /* Prove the descriptor reached DRAM: invalidate, then read it back. The controller reads
   * memory directly, so a cache-only write leaves it an all-zero UTRD. */
  InvalidateDataCacheRange ((VOID *)(UINTN)mUtRdBase, 32);
  Print (L"UFSPROBE:   dram utrd %08x %08x %08x %08x %08x %08x %08x %08x\n",
         ((volatile UINT32 *)(UINTN)mUtRdBase)[0], ((volatile UINT32 *)(UINTN)mUtRdBase)[1],
         ((volatile UINT32 *)(UINTN)mUtRdBase)[2], ((volatile UINT32 *)(UINTN)mUtRdBase)[3],
         ((volatile UINT32 *)(UINTN)mUtRdBase)[4], ((volatile UINT32 *)(UINTN)mUtRdBase)[5],
         ((volatile UINT32 *)(UINTN)mUtRdBase)[6], ((volatile UINT32 *)(UINTN)mUtRdBase)[7]);
  Print (L"UFSPROBE:   utrd %08x %08x %08x %08x %08x %08x %08x %08x\n",
         Words[0], Words[1], Words[2], Words[3], Words[4], Words[5], Words[6], Words[7]);
  Print (L"UFSPROBE:   prd %08x %08x %08x %08x (dir %u len %u)\n",
         *(UINT32 *)(mBuffer + UCD_OFFSET + PRDT_OFFSET),
         *(UINT32 *)(mBuffer + UCD_OFFSET + PRDT_OFFSET + 4),
         *(UINT32 *)(mBuffer + UCD_OFFSET + PRDT_OFFSET + 8),
         *(UINT32 *)(mBuffer + UCD_OFFSET + PRDT_OFFSET + 12),
         Direction, DataBytes);
  MmioWrite32 ((UINTN)UFS_HOST_BASE + UFS_UTRLDBR, 1);
}

static UINT32
PollOcs (
  IN UINTN  Milliseconds
  )
{
  UINTN   Index;
  UINT32  Ocs;

  for (Index = 0; Index < Milliseconds; Index++) {
    InvalidateDataCacheRange ((VOID *)(UINTN)mUtRdBase, 32);
    Ocs = *(volatile UINT32 *)(UINTN)(mUtRdBase + 8) & 0xFF;
    if (Ocs != 0x0F) {
      return Ocs;
    }
    gBS->Stall (1000);
  }
  return 0x0F;
}


/* QMP UFS PHY block offsets (Linux qmp_ufs_offsets). */
#define PHY_SERDES   0x0000
#define PHY_TX       0x0400
#define PHY_RX       0x0800
#define PHY_PCS      0x0c00

#define GDSC_SW_COLLAPSE  (1U << 31)

STATIC
VOID
WriteTable (
  IN UINTN              Base,
  IN const UFS_PHY_TBL *Table,
  IN UINTN              Count
  )
{
  UINTN  Index;

  for (Index = 0; Index < Count; Index++) {
    MmioWrite32 (Base + Table[Index].offset, Table[Index].value);
  }
}

/* Enable the UFS power domain and its branch clocks, then bring the PHY up. Without this
 * the controller register file returns stale values and control writes are dropped. */
STATIC
EFI_STATUS
UfsPowerAndPhyInit (
  VOID
  )
{
  UINTN   Wait;
  UINT32  Gdscr;
  UINT32  Clocks[] = {
    GCC_UFS_AXI, GCC_UFS_AHB, GCC_UFS_TX0, GCC_UFS_RX0, GCC_UFS_UNIPRO,
    GCC_UFS_ICE, GCC_UFS_AUX, GCC_UFS_RX1, GCC_UFS_AGGR
  };
  UINTN   Index;

  Gdscr = MmioRead32 ((UINTN)GCC_BASE + 0x00077004);
  Print (L"UFSPROBE: GDSCR before 0x%08x\n", Gdscr);
  MmioWrite32 ((UINTN)GCC_BASE + 0x00077004, Gdscr & ~GDSC_SW_COLLAPSE);
  for (Wait = 0; Wait < 200; Wait++) {
    if ((MmioRead32 ((UINTN)GCC_BASE + 0x00077004) & GDSC_SW_COLLAPSE) == 0) {
      break;
    }
    gBS->Stall (1000);
  }
  Print (L"UFSPROBE: GDSCR after 0x%08x (waited %u)\n",
         MmioRead32 ((UINTN)GCC_BASE + 0x00077004), (UINT32)Wait);

  for (Index = 0; Index < ARRAY_SIZE (Clocks); Index++) {
    UINT32  Value = MmioRead32 ((UINTN)GCC_BASE + Clocks[Index]);

    if ((Value & 1U) == 0) {
      MmioWrite32 ((UINTN)GCC_BASE + Clocks[Index], Value | 1U);
      gBS->Stall (1000);
    }
  }
  Print (L"UFSPROBE: clocks AXI 0x%08x AHB 0x%08x UNIPRO 0x%08x\n",
         MmioRead32 ((UINTN)GCC_BASE + GCC_UFS_AXI),
         MmioRead32 ((UINTN)GCC_BASE + GCC_UFS_AHB),
         MmioRead32 ((UINTN)GCC_BASE + GCC_UFS_UNIPRO));

  /* Deassert the block reset, then program the PHY. */
  MmioWrite32 ((UINTN)GCC_BASE + GCC_UFS_BCR, 0);
  gBS->Stall (100);

  WriteTable ((UINTN)UFS_PHY_BASE + PHY_SERDES, SM8150_UFSPHY_SERDES, SM8150_UFSPHY_SERDES_NUM);
  WriteTable ((UINTN)UFS_PHY_BASE + PHY_TX, SM8150_UFSPHY_TX, SM8150_UFSPHY_TX_NUM);
  WriteTable ((UINTN)UFS_PHY_BASE + PHY_RX, SM8150_UFSPHY_RX, SM8150_UFSPHY_RX_NUM);
  WriteTable ((UINTN)UFS_PHY_BASE + PHY_PCS, SM8150_UFSPHY_PCS, SM8150_UFSPHY_PCS_NUM);
  gBS->Stall (1000);

  Print (L"UFSPROBE: PHY written (serdes %u tx %u rx %u pcs %u)\n",
         (UINT32)SM8150_UFSPHY_SERDES_NUM, (UINT32)SM8150_UFSPHY_TX_NUM,
         (UINT32)SM8150_UFSPHY_RX_NUM, (UINT32)SM8150_UFSPHY_PCS_NUM);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
UfsProbeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status = EFI_SUCCESS;
  UINT32      Hcs;
  UINT32      Ocs;
  UINT8       *Ucd;
  UINT8       *Rsp;
  UINT32      LastLba;
  UINT32      BlockSize;

  Hcs = MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_HCS);
  /* The bootloader's own request list is the one address range the controller is proven
   * to reach, so record it before taking the queue over. */
  Print (L"\nUFSPROBE: HCS 0x%08x orig UTRLBA 0x%08x%08x RSR %u IS 0x%08x\n", Hcs,
         MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_UTRLBAU),
         MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_UTRLBA),
         MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_UTRLRSR),
         MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_IS));

  {
    EFI_PHYSICAL_ADDRESS  At = 0x9F600000ULL;
    Status = gBS->AllocatePages (AllocateAddress, EfiBootServicesData, BUFFER_PAGES, &At);
    if (EFI_ERROR (Status)) {
      Print (L"UFSPROBE: allocation at 0x9F600000 failed: %r\n", Status);
      CpuDeadLoop ();
    }
    mBuffer = (UINT8 *)(UINTN)At;
  }
  ZeroMem (mBuffer, BUFFER_PAGES * 4096);
  mPhys   = (UINT64)(UINTN)mBuffer;

  /* GDSCR bit 31 set means the UFS power domain is collapsed: the register file then
   * returns stale values and control writes are dropped, which matches HCE staying 0. */
  Print (L"UFSPROBE: GDSCR 0x%08x BCR 0x%08x\n",
         MmioRead32 ((UINTN)GCC_BASE + 0x00077004),
         MmioRead32 ((UINTN)GCC_BASE + GCC_UFS_BCR));
  Print (L"UFSPROBE: AXI 0x%08x AHB 0x%08x TX0 0x%08x RX0 0x%08x UNIPRO 0x%08x\n",
         MmioRead32 ((UINTN)GCC_BASE + GCC_UFS_AXI),
         MmioRead32 ((UINTN)GCC_BASE + GCC_UFS_AHB),
         MmioRead32 ((UINTN)GCC_BASE + GCC_UFS_TX0),
         MmioRead32 ((UINTN)GCC_BASE + GCC_UFS_RX0),
         MmioRead32 ((UINTN)GCC_BASE + GCC_UFS_UNIPRO));
  Print (L"UFSPROBE: ICE 0x%08x AUX 0x%08x RX1 0x%08x AGGR 0x%08x\n",
         MmioRead32 ((UINTN)GCC_BASE + GCC_UFS_ICE),
         MmioRead32 ((UINTN)GCC_BASE + GCC_UFS_AUX),
         MmioRead32 ((UINTN)GCC_BASE + GCC_UFS_RX1),
         MmioRead32 ((UINTN)GCC_BASE + GCC_UFS_AGGR));

  /* HCE bit 0 enables the host controller. Without it the controller acknowledges a
   * descriptor but performs no transfer, which is what the earlier runs showed. */
  MmioWrite32 ((UINTN)UFS_HOST_BASE + UFS_HCE,
               MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_HCE) | 1U);
  {
    UINTN Wait;
    for (Wait = 0; Wait < 200; Wait++) {
      if ((MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_HCS) & 1U) != 0) {
        break;
      }
      gBS->Stall (1000);
    }
    Print (L"UFSPROBE: HCE 0x%08x HCS 0x%08x after enable\n",
           MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_HCE),
           MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_HCS));
  }

  /* docs/porting/ufs.md: adopt the firmware state or cold-initialise, never both. A usable
   * handoff means HCE enabled, the request list running and a nonzero list base. */
  /* ShengUfs adopts the firmware's existing request list. When the
   * bootloader left one (UTRLBA nonzero) keep that base and only replace its descriptors,
   * so the controller never sees the list move underneath it. */
  {
    UINT64  OrigList = ((UINT64)MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_UTRLBAU) << 32) |
                       MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_UTRLBA);

    Print (L"UFSPROBE: handoff HCE 0x%08x HCS 0x%08x RSR 0x%08x IS 0x%08x list 0x%08x%08x\n",
           MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_HCE),
           MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_HCS),
           MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_UTRLRSR),
           MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_IS),
           (UINT32)(OrigList >> 32), (UINT32)OrigList);

    if ((OrigList != 0) && ((MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_HCE) & 1U) != 0)) {
      mUtRdBase = OrigList;
      Print (L"UFSPROBE: adopting the bootloader list at 0x%08x%08x\n",
             (UINT32)(OrigList >> 32), (UINT32)OrigList);
      if ((MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_UTRLRSR) & 1U) == 0) {
        MmioWrite32 ((UINTN)UFS_HOST_BASE + UFS_UTRLRSR, 1);
      }
    } else {
      Print (L"UFSPROBE: handoff unusable, cold initialising\n");
      UfsPowerAndPhyInit ();
      mUtRdBase = mPhys;
      MmioWrite32 ((UINTN)UFS_HOST_BASE + UFS_IE, 0);
      MmioWrite32 ((UINTN)UFS_HOST_BASE + UFS_UTRLRSR, 0);
      gBS->Stall (1000);
      MmioWrite32 ((UINTN)UFS_HOST_BASE + UFS_UTRLBA, (UINT32)mPhys);
      MmioWrite32 ((UINTN)UFS_HOST_BASE + UFS_UTRLBAU, (UINT32)(mPhys >> 32));
      MmioWrite32 ((UINTN)UFS_HOST_BASE + UFS_IS, 0xFFFFFFFF);
      MmioWrite32 ((UINTN)UFS_HOST_BASE + UFS_UTRLRSR, 1);
    }
  }
  {
    UINTN Wait;
    for (Wait = 0; Wait < 100; Wait++) {
      if (MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_HCS) & 2) {
        break;
      }
      gBS->Stall (1000);
    }
  }
  Print (L"UFSPROBE: our UTRLBA 0x%08x%08x RSR 0x%x HCS 0x%x\n",
         MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_UTRLBAU),
         MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_UTRLBA),
         MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_UTRLRSR),
         MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_HCS));

  Ucd = mBuffer + UCD_OFFSET;
  Rsp = mBuffer + UCD_OFFSET + RSP_OFFSET;

  Ucd[3] = 1;
  SubmitCommand (0, 0);
  Ocs = PollOcs (2000);
  Print (L"UFSPROBE: NOP ocs 0x%x response-type 0x%x tag %u\n", Ocs, Rsp[0], Rsp[3]);

  /* Distinguish "the controller never wrote" from "it wrote elsewhere": the data area is
   * pre-filled with a pattern the device will not return. */
  SetMem (mBuffer + DATA_OFFSET, 64, 0xAA);
  WriteBackDataCacheRange (mBuffer + DATA_OFFSET, 4096);
  ZeroMem (Ucd, 256);
  Ucd[0] = 0x01;
  Ucd[1] = 0x40;
  Ucd[2] = 0;
  Ucd[12] = 0;
  Ucd[13] = 0;
  Ucd[14] = 0;
  Ucd[15] = 8;
  Ucd[16] = 0x25;
  SubmitCommand (2, 8);
  Ocs = PollOcs (2000);
  InvalidateDataCacheRange (mBuffer + DATA_OFFSET, 64);
  Print (L"UFSPROBE:   where prd %08x %08x data %08x %08x\n",
         *(UINT32 *)(mBuffer + 0x1400), *(UINT32 *)(mBuffer + 0x1404),
         *(UINT32 *)(mBuffer + 0x2000), *(UINT32 *)(mBuffer + 0x2004));
  {
    volatile UINT8  *Data;
    UINT32          Word[4];
    UINTN           Index;

    Data = (volatile UINT8 *)(mBuffer + DATA_OFFSET);
    for (Index = 0; Index < 4; Index++) {
      Word[Index] = ((UINT32)Data[Index * 4 + 0] << 24) | ((UINT32)Data[Index * 4 + 1] << 16) |
                    ((UINT32)Data[Index * 4 + 2] << 8) | (UINT32)Data[Index * 4 + 3];
    }
    LastLba   = Word[0];
    BlockSize = Word[1];
    Print (L"UFSPROBE: READ CAPACITY ocs 0x%x status 0x%x residual %u\n",
           Ocs, Rsp[6], ((UINT32)Rsp[12] << 8) | Rsp[13]);
    Print (L"UFSPROBE: data %08x %08x %08x %08x\n", Word[0], Word[1], Word[2], Word[3]);
  }
  Print (L"UFSPROBE: last-lba %lu block-size %lu bytes\n", LastLba, BlockSize);
  if (BlockSize != 0) {
    Print (L"UFSPROBE: capacity %lu MiB\n", ((UINT64)LastLba + 1) * BlockSize / (1024 * 1024));
  }

  /* READ(10) LBA 1, one block, into the data area: the GPT header lives there. */
  ZeroMem (Ucd, 64);
  Ucd[0]  = 0x01;
  Ucd[1]  = 0x40;
  Ucd[2]  = 0;
  Ucd[12] = 0x00;
  Ucd[13] = 0x00;
  Ucd[14] = 0x10;
  Ucd[15] = 0x00;
  Ucd[16] = 0x28;
  Ucd[21] = 0x01;
  Ucd[22] = 0x00;
  Ucd[23] = 0x00;
  Ucd[24] = 0x01;
  SubmitCommand (2, 4096);
  Ocs = PollOcs (2000);
  InvalidateDataCacheRange (mBuffer + DATA_OFFSET, 4096);
  {
    volatile UINT8  *Data;
    CHAR8           Text[9];
    UINTN           Index;

    Data = (volatile UINT8 *)(mBuffer + DATA_OFFSET);
    for (Index = 0; Index < 8; Index++) {
      Text[Index] = (CHAR8)Data[Index];
    }
    Text[8] = 0;
    Print (L"UFSPROBE: READ(10) lba1 ocs 0x%x status 0x%x sig '%a'\n", Ocs, Rsp[6], Text);
    Print (L"UFSPROBE: lba1 %02x %02x %02x %02x %02x %02x %02x %02x\n",
           Data[0], Data[1], Data[2], Data[3], Data[4], Data[5], Data[6], Data[7]);
    Print (L"UFSPROBE: after read prd %08x %08x data %08x %08x\n",
           *(UINT32 *)(mBuffer + 0x1400), *(UINT32 *)(mBuffer + 0x1404),
           *(UINT32 *)(mBuffer + 0x2000), *(UINT32 *)(mBuffer + 0x2004));
  }

  /* Probe every LUN's capacity: the boot LUN and the data LUN differ on Qualcomm parts. */
  for (UINTN Lun = 0; Lun < 4; Lun++) {
    UINT32  CapOcs;

    ZeroMem (Ucd, 256);
    Ucd[0]  = 0x01;
    Ucd[1]  = 0x40;
    Ucd[2]  = (UINT8)Lun;
    Ucd[12] = 0;
    Ucd[13] = 0;
    Ucd[14] = 0;
    Ucd[15] = 8;
    Ucd[16] = 0x25;
    SetMem (mBuffer + DATA_OFFSET, 64, 0xAA);
    WriteBackDataCacheRange (mBuffer + DATA_OFFSET, 4096);
    SubmitCommand (2, 8);
    CapOcs = PollOcs (2000);
    InvalidateDataCacheRange (mBuffer + DATA_OFFSET, 64);
    Print (L"UFSPROBE: LUN %u ocs 0x%x data %08x %08x rsp %02x %02x %02x %02x\n",
           Lun, CapOcs,
           *(UINT32 *)(mBuffer + DATA_OFFSET), *(UINT32 *)(mBuffer + DATA_OFFSET + 4),
           Rsp[0], Rsp[1], Rsp[6], Rsp[7]);
  }

  /* Verdict block: one photograph of the bottom lines settles the state. */
  Print (L"\n=== UFS VERDICT ===\n");
  Print (L"hcs=0x%08x rsr=%u\n", MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_HCS),
         MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_UTRLRSR));
  Print (L"cap_data=%08x %08x  last_lba=%lu blk=%lu\n",
         *(UINT32 *)(mBuffer + DATA_OFFSET), *(UINT32 *)(mBuffer + DATA_OFFSET + 4),
         LastLba, BlockSize);
  Print (L"read_sig=%02x %02x %02x %02x %02x %02x %02x %02x\n",
         *(UINT8 *)(mBuffer + DATA_OFFSET), *(UINT8 *)(mBuffer + DATA_OFFSET + 1),
         *(UINT8 *)(mBuffer + DATA_OFFSET + 2), *(UINT8 *)(mBuffer + DATA_OFFSET + 3),
         *(UINT8 *)(mBuffer + DATA_OFFSET + 4), *(UINT8 *)(mBuffer + DATA_OFFSET + 5),
         *(UINT8 *)(mBuffer + DATA_OFFSET + 6), *(UINT8 *)(mBuffer + DATA_OFFSET + 7));
  {
    UINT32  *Ut = (UINT32 *)(UINTN)mUtRdBase;
    UINT32  *Pr = (UINT32 *)(mBuffer + UCD_OFFSET + PRDT_OFFSET);

    Print (L"utrd=%08x %08x %08x %08x %08x %08x %08x %08x\n",
           Ut[0], Ut[1], Ut[2], Ut[3], Ut[4], Ut[5], Ut[6], Ut[7]);
    Print (L"prdt=%08x %08x %08x %08x\n", Pr[0], Pr[1], Pr[2], Pr[3]);
  }
  Print (L"is=0x%08x ie=0x%08x\n",
         MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_IS), MmioRead32 ((UINTN)UFS_HOST_BASE + UFS_IE));
  Print (L"rsp=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
         Rsp[0], Rsp[1], Rsp[2], Rsp[3], Rsp[4], Rsp[5], Rsp[6], Rsp[7],
         Rsp[8], Rsp[9], Rsp[10], Rsp[11], Rsp[12], Rsp[13], Rsp[14], Rsp[15]);
  Print (L"===================\n");

  CpuDeadLoop ();
  return Status;
}
