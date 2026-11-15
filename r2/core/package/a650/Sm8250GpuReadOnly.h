// SPDX-License-Identifier: BSD-2-Clause-Patent
// IN2020 mode 1 adapter for the UniXNU alioth GpuPowerBoard sequence.
#pragma once

#include <mach/vm_prot.h>
#include "GpuPowerBoard.h"

extern "C" vm_offset_t ml_io_map_with_prot(vm_offset_t, vm_size_t, vm_prot_t);

namespace {
struct GpuReadWindow {
    uint64_t physical;
    uint32_t size;
    vm_offset_t mapped;
};

__attribute__((noinline))
int gpuReadRegister(void *cookie, unsigned window, uint32_t offset, uint32_t *value)
{
    if (window >= GPB_WINDOWS || (offset & 3))
        return -1;
    auto &range = static_cast<GpuReadWindow *>(cookie)[window];
    if (range.size < sizeof(*value) || offset > range.size - sizeof(*value))
        return -1;
    if (!range.mapped)
        range.mapped = ml_io_map_with_prot(range.physical, range.size, VM_PROT_READ);
    if (!range.mapped)
        return -1;
    __asm__ volatile("dmb oshld" ::: "memory");
    *value = *reinterpret_cast<volatile const uint32_t *>(range.mapped + offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return 0;
}

__attribute__((noinline))
void gpuProbeReadOnly()
{
    unsigned enable = 0;
    if (!PE_parse_boot_argn("sm8250-gpu-ro", &enable, sizeof(enable)) || enable != 1)
        return;

    // Only map each window once the upstream prerequisite checks reach it.
    // The XNU I/O mapper retains these mappings for this boot.
    GpuReadWindow windows[GPB_WINDOWS] = {
        {GPB_GCC_GPU_PAGE, 0x1000, 0},
        {GPB_GCC_VOTE_PAGE, 0x1000, 0},
        {GPB_GPUCC_BASE, GPB_GPUCC_SIZE, 0},
        {GPB_SMMU_BASE, GPB_SMMU_GR0_SIZE, 0},
        {GPB_GPU_BASE, 0x1000, 0}, // RBBM_STATUS fits in the first page.
    };
    static_assert(GPB_RBBM_STATUS_BYTES < 0x1000);
    const gpb_io io = {windows, gpuReadRegister, nullptr, nullptr};
    gpb_result result{};
    printf("GPU48 mode1 begin\n");
    gpb_run(&io, 1, (1u << (GPB_WIN_GPU + 1)) - 1, &result);
    printf("GPU48 gcc ahb=%08x memnoc=%08x dvm=%08x misc=%08x vote=%08x\n",
           result.cfg_ahb, result.memnoc, result.dvm, result.misc, result.pll_vote);
    printf("GPU48 gpucc cx=%08x gx=%08x ahb=%08x smmu=%08x gmu=%08x\n",
           result.cx_gdscr, result.gx_gdscr, result.cc_ahb, result.cc_smmu, result.cc_gmu);
    printf("GPU48 smmu id=%08x/%08x/%08x scr0=%08x gfsr=%08x rbbm=%08x\n",
           result.id0, result.id1, result.id2, result.scr0, result.gfsr, result.rbbm_status);
    printf("GPU48 mode1 complete gcc=%u cc=%u smmu=%u gpu=%u io=%d writes=0\n",
           result.gcc, result.cc, result.smmu, result.gpu, result.io_error);
}
}
