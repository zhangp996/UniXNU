#include <Library/MemoryMapLib.h>

//
// IN2020 memory map. Every DDR entry comes from this board's live device tree
// (docs/evidence/device-fdt.dts in the device repository) and every register
// window from the same tree. The UEFI regions are placed inside the free window
// between dfps_data_region (0x9E400000) and kboot_uboot_logmem (0x9FE00000).
//
// Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute
//
STATIC EFI_MEMORY_REGION_DESCRIPTOR gMemoryDescriptor[] = {
  // DDR regions the bootloader hands over.
  {"HYP",                  0x80000000, 0x00600000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"XBL AOP",              0x80700000, 0x00160000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"AOP CMD DB",           0x80860000, 0x00020000, AddMem, MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"SMEM",                 0x80900000, 0x00200000, AddMem, MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"Removed Region",       0x80B00000, 0x0CD00000, AddMem, MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"PIL Camera",           0x8DC00000, 0x00500000, AddMem, MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"PIL WLAN FW",          0x8E100000, 0x00100000, AddMem, MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"PIL IPA FW",           0x8E200000, 0x00010000, AddMem, MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"PIL IPA GSI",          0x8E210000, 0x0000A000, AddMem, MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"PIL GPU",              0x8E21A000, 0x00002000, AddMem, MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"PIL NPU",              0x8E300000, 0x00500000, AddMem, MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"PIL Video",            0x8E800000, 0x00500000, AddMem, MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"PIL CVP",              0x8ED00000, 0x00500000, AddMem, MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"PIL CDSP",             0x8F200000, 0x01400000, AddMem, MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"PIL SLPI",             0x90600000, 0x01500000, AddMem, MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"PIL ADSP",             0x91B00000, 0x02500000, AddMem, MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"PIL SPSS",             0x94000000, 0x00100000, AddMem, MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"CDSP Secure Heap",     0x94100000, 0x04600000, AddMem, MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},

  // The ABL leaves the panel powered through cont_splash_region, and this is the
  // region SimpleFbDxe publishes as the GOP framebuffer.
  {"Display Reserved",     0x9C000000, 0x02300000, AddMem, MEM_RES, SYS_MEM_CAP,   Reserv, WRITE_THROUGH_XN},
  {"DFPS Data",            0x9E300000, 0x00100000, AddMem, MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},

  // UEFI regions, inside the free window measured on this board.
  {"UEFI FD",              0x9E400000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP,   BsData, WRITE_BACK},
  // Loaded DXE images run from these regions, so they must stay executable: an XN
  // mapping makes the first jump into a driver fault and the dispatcher then reports
  // every architectural protocol as missing.
  {"DXE Heap",             0x9E800000, 0x00800000, AddMem, SYS_MEM, SYS_MEM_CAP,   Conv,   WRITE_BACK},

  // Free memory. DxeCore sizes its initial pools from the conventional regions the
  // memory map declares, and 8 MiB of heap is below the minimum it needs, which trips
  // "ASSERT [DxeCore] (Code.c:2518): Length >= MinimalMemorySizeNeeded". Each span
  // below is the gap between measured carveouts inside the first DRAM bank, so none
  // of them overlap a region listed above or below.
  {"Free Low",             0x98700000, 0x03900000, AddMem, SYS_MEM, SYS_MEM_CAP,   Conv,   WRITE_BACK},
  //
  // The embedded panic log lives at 0x9F400000 (see dts/instantnoodlep.dts, node
  // pram@9f400000). It is carved out of the Free Mid span so the firmware never
  // allocates over the buffer XNU writes its panic into.
  //
  {"Panic Log",            0x9F400000, 0x00080000, AddMem, SYS_MEM, SYS_MEM_CAP,   Reserv, WRITE_BACK},
  {"Free Mid Low",         0x9F040000, 0x003C0000, AddMem, SYS_MEM, SYS_MEM_CAP,   Conv,   WRITE_BACK},
  {"Free Mid High",        0x9F480000, 0x00980000, AddMem, SYS_MEM, SYS_MEM_CAP,   Conv,   WRITE_BACK},
  // The ramoops node at 0xB0000000 (0x400000) sits inside the tail of the first bank,
  // so the span is split around it instead of straddling it.
  {"Free High",            0xA0000000, 0x010000000, AddMem, SYS_MEM, SYS_MEM_CAP,  Conv,   WRITE_BACK},
  {"Free Tail",            0xB0400000, 0x00B700000, AddMem, SYS_MEM, SYS_MEM_CAP,  Conv,   WRITE_BACK},

  // The two upper DRAM banks the live device tree reports. DarwinBoot needs one
  // contiguous window as large as payload.adt.owned_memory_size (4 GiB here), and the
  // first bank alone cannot provide it; the third bank is exactly 4 GiB and sits at
  // 0x180000000.
  {"RAM Bank 2",           0xC0000000, 0x0C0000000, AddMem, SYS_MEM, SYS_MEM_CAP,  Conv,   WRITE_BACK},
  {"RAM Bank 3",           0x180000000, 0x100000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"UEFI Stack",           0x9F000000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP,   BsData, WRITE_BACK_XN},

  // Reserved by the bootloader above the UEFI window.
  {"UBoot Log",            0x9FE00000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP,   Reserv, WRITE_BACK_XN},
  {"Ram Oops",             0xB0000000, 0x00400000, AddMem, MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},

  // Component register windows, from the live device tree.
  {"GIC",                  0x17A00000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"UFS",                  0x01D84000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  //
  // GCC, for the UFS clock branches and the UFS PHY block reset. The UFS registers the
  // clock driver touches are at 0x1770xx, inside this window.
  //
  {"GCC",                  0x00100000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"QUPV3",                0x009C0000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"GPU SMMU",             0x03DA0000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"SECURITY CONTROL",     0x00780000, 0x00007000, AddDev, MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"PRNG CFG",             0x010D0000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"TCSR",                 0x01FC0000, 0x00030000, AddDev, MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"PERIPH SS",            0x08800000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"IPC ROUTER",           0x00400000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"USB",                  0x0A600000, 0x04000000, AddDev, MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  //
  // SPMI PMIC arbiter. The core, channel and configuration windows fall inside the
  // USB span above, but the observer window starts exactly at its end (the USB
  // region ends at 0xE600000) and the interrupt block follows it. Without these the
  // first observer read (channel 0x80*APID) faults at 0xE600C80.
  //
  {"PMIC ARB OBS",         0x0E600000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"PMIC ARB INTR",        0x0E700000, 0x000A0000, AddDev, MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
};

VOID
EFIAPI
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount)
{
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = (UINT8)(sizeof (gMemoryDescriptor) / sizeof (gMemoryDescriptor[0]));
}
