// SPDX-License-Identifier: BSD-2-Clause-Patent
// Mode 6 register allowlist from UniXNU-alioth package/GpuProbe.cpp (aad6aeb).
#pragma once

extern "C" bool sm8250_alioth_gmu_cold_start(unsigned mode, const gpb_io *io);

namespace {
struct AliothGmuIo {
    GpuCxProbe probe;
    vm_offset_t writable[GPB_WIN_GPUCC + 1] = {};
};

int aliothGmuRead(void *cookie, unsigned window, uint32_t offset, uint32_t *value)
{
    auto &probe = static_cast<AliothGmuIo *>(cookie)->probe;
    return gpuReadRegister(probe.windows, window, offset, value);
}

__attribute__((noinline))
int aliothGmuWrite(void *cookie, unsigned window, uint32_t offset, uint32_t value)
{
    bool allowed = false;
    if (window == GPB_WIN_GCC_VOTE)
        allowed = offset == GPB_GCC_PLL_VOTE;
    else if (window == GPB_WIN_GCC_GPU)
        allowed = offset == GPB_GCC_GPU_CFG_AHB || offset == GPB_GCC_DDRSS_GPU_AXI ||
                  offset == GPB_GCC_GPU_MEMNOC_GFX;
    else if (window == GPB_WIN_GPUCC)
        allowed = offset == GPB_CC_CX_GDSCR || offset == GPB_CC_AHB_CLK ||
                  offset == GPB_CC_SMMU_VOTE_CLK || offset == GPB_CC_GMU_RCGR ||
                  offset == GPB_CC_GMU_RCGR + 4 || offset == GPB_CC_CX_GMU_CLK ||
                  offset == GPB_CC_CXO_CLK;
    if (!allowed)
        return -1;

    auto &state = *static_cast<AliothGmuIo *>(cookie);
    const auto &range = state.probe.windows[window];
    auto &mapping = state.writable[window];
    if (!mapping)
        mapping = ml_io_map_with_prot(range.physical, range.size, VM_PROT_READ | VM_PROT_WRITE);
    if (!mapping)
        return -1;
    *reinterpret_cast<volatile uint32_t *>(mapping + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

__attribute__((noinline))
void gpuProbeAliothGmu()
{
    unsigned mode = 0;
    if (!PE_parse_boot_argn("sm8250-gpu-alioth", &mode, sizeof(mode)) || mode < 6 || mode > 12)
        return;

    AliothGmuIo state;
    const gpb_io io = {&state, aliothGmuRead, aliothGmuWrite, gpuCxDelay};
    gpb_result result{};
    gpb_gmu_clock_on clocks{};
    gpb_gmu_state gmu{};
    printf("GPU53 alioth mode6 begin\n");
    constexpr unsigned controlWindows = (1u << (GPB_WIN_GPUCC + 1)) - 1;
    gpb_run(&io, mode, controlWindows, &result);
    // The IN2020 SMMU needs both live GCC branches before its first MMIO read.
    const unsigned smmu = gpuCxSampleSmmu(state.probe, result);
    const unsigned commandDb = smmu == 5 ? gpuCxGmuInputs(state.probe) : 0;
    if (commandDb == 5)
        gpb_gmu_clocks_on(&io, &result, &clocks);
    const bool readable = clocks.step == 5 && !clocks.failed && !result.io_error &&
                          gpb_gmu_regs_readable(&result, clocks.cx_gmu, clocks.ahb);
    if (readable)
        gpb_read_gmu_state(&io, &result, &gmu);
    printf("GPU53 clocks step=%u failed=%d cmd=%08x cfg=%08x gmu=%08x ahb=%08x cxo=%08x axi=%08x memnoc=%08x\n",
           clocks.step, clocks.failed, clocks.gmu_cmd, clocks.gmu_cfg, clocks.cx_gmu,
           clocks.ahb, clocks.cxo, clocks.axi, clocks.memnoc);
    printf("GPU53 alioth mode6 complete cx=%u smmu=%u db=%u io=%d readable=%u reset=%08x fw=%08x hfi=%08x fence=%08x\n",
           result.powered_cx, smmu, commandDb, result.io_error, readable && !result.io_error,
           gmu.sysreset, gmu.fw_init, gmu.hfi_status, gmu.fence);
    if (mode == 6)
        return;
    bool started = false;
    if (readable && !result.io_error && gmu.sysreset == 1 && !gmu.fw_init && !gmu.hfi_status)
        started = sm8250_alioth_gmu_cold_start(mode, &io);
    else
        printf("GPU54 mode7 result GMU not ready for cold start\n");
    if (mode == 11 && !started)
        printf("GPU58 mode11 result prerequisite failed\n");
    if (mode == 10 && !started)
        printf("GPU57 mode10 result prerequisite failed\n");
    if (mode == 9 && !started)
        printf("GPU56 mode9 result bring-up failed\n");
    if (mode == 8 && !started)
        printf("GPU55 mode8 result GMU cold start failed\n");
}
}
