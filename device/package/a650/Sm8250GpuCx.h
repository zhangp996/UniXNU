// SPDX-License-Identifier: BSD-2-Clause-Patent
// IN2020 adapter for the alioth mode 3 power and clock check.
#pragma once

namespace {
struct GpuCxVote {
    uint32_t offset;
    uint32_t mask;
    uint32_t original;
    bool changed;
};

struct GpuCxProbe {
    GpuReadWindow windows[GPB_WINDOWS] = {
        {GPB_GCC_GPU_PAGE, 0x1000, 0},
        {GPB_GCC_VOTE_PAGE, 0x1000, 0},
        {GPB_GPUCC_BASE, GPB_GPUCC_SIZE, 0},
        {GPB_SMMU_BASE, GPB_SMMU_GR0_SIZE, 0},
        {}, // Mode 6 does not access the GX register window.
        {GPB_GMU_BASE, GPB_GMU_SIZE, 0},
        {GPB_RSCC_BASE, GPB_RSCC_SIZE, 0},
    };
    GpuCxVote votes[3] = {
        {GPB_CC_CX_GDSCR, GPB_GDSC_SW_COLLAPSE, 0, false},
        {GPB_CC_AHB_CLK, GPB_CBCR_ENABLE, 0, false},
        {GPB_CC_SMMU_VOTE_CLK, GPB_CBCR_ENABLE, 0, false},
    };
    vm_offset_t writeMapping = 0;
};

int gpuCxRead(void *cookie, unsigned window, uint32_t offset, uint32_t *value)
{
    if (window > GPB_WIN_GPUCC)
        return -1;
    return gpuReadRegister(static_cast<GpuCxProbe *>(cookie)->windows, window, offset, value);
}

void gpuCxStore(GpuCxProbe &probe, uint32_t offset, uint32_t value)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(probe.writeMapping + offset) = value;
    __asm__ volatile("dsb oshst" ::: "memory");
}

__attribute__((noinline))
int gpuCxWrite(void *cookie, unsigned window, uint32_t offset, uint32_t value)
{
    if (window != GPB_WIN_GPUCC)
        return -1;
    auto &probe = *static_cast<GpuCxProbe *>(cookie);
    for (auto &vote : probe.votes) {
        if (vote.offset != offset)
            continue;
        uint32_t current;
        if (vote.changed || gpuCxRead(cookie, window, offset, &current))
            return -1;
        const uint32_t expected = offset == GPB_CC_CX_GDSCR ? 0 : vote.mask;
        if ((value & vote.mask) != expected || (current & vote.mask) == expected)
            return -1;
        if (!probe.writeMapping) {
            probe.writeMapping = ml_io_map_with_prot(
                GPB_GPUCC_BASE, GPB_GPUCC_SIZE, VM_PROT_READ | VM_PROT_WRITE);
        }
        if (!probe.writeMapping)
            return -1;
        vote.original = current & vote.mask;
        vote.changed = true;
        gpuCxStore(probe, offset, (current & ~vote.mask) | expected);
        printf("GPU49 vote offset=%x bit=%x>%x\n", offset, vote.original, expected);
        return 0;
    }
    return -1;
}

void gpuCxDelay(void *, unsigned us)
{
    IODelay(us);
}

__attribute__((noinline))
unsigned gpuCxSampleSmmu(GpuCxProbe &probe, const gpb_result &result)
{
    if (result.io_error || !result.powered_cx || !result.powered_clocks)
        return 1;

    // The previous diagnostic printed the upstream sequence's pre-CX GCC snapshot.
    // Poll fresh CBCRs after CX is up. Existing enable votes are left in place.
    uint32_t memnoc = 0, dvm = 0;
    bool interconnectReady = false;
    for (unsigned poll = 0; poll < 200; ++poll) {
        if (gpuCxRead(&probe, GPB_WIN_GCC_GPU, GPB_GCC_GPU_MEMNOC_GFX, &memnoc) ||
            gpuCxRead(&probe, GPB_WIN_GCC_GPU, GPB_GCC_GPU_SNOC_DVM_GFX, &dvm))
            return 2;
        if ((memnoc & dvm & GPB_CBCR_ENABLE) && gpb_clock_on(memnoc) && gpb_clock_on(dvm)) {
            interconnectReady = true;
            break;
        }
        IODelay(1);
    }
    printf("GPU50 active GCC memnoc=%08x>%08x dvm=%08x>%08x\n",
           result.memnoc, memnoc, result.dvm, dvm);
    if (!interconnectReady)
        return 3;

    uint32_t cx, ahb, vote;
    if (gpuCxRead(&probe, GPB_WIN_GPUCC, GPB_CC_CX_GDSCR, &cx) ||
        gpuCxRead(&probe, GPB_WIN_GPUCC, GPB_CC_AHB_CLK, &ahb) ||
        gpuCxRead(&probe, GPB_WIN_GPUCC, GPB_CC_SMMU_VOTE_CLK, &vote))
        return 2;
    if (!gpb_gdsc_on(cx) || !(ahb & vote & GPB_CBCR_ENABLE) ||
        !gpb_clock_on(ahb) || !gpb_clock_on(vote))
        return 3;

    uint32_t id0, id1, id2;
    printf("GPU50 GPU SMMU GR0 ID read begin\n");
    if (gpuReadRegister(probe.windows, GPB_WIN_SMMU, GPB_SMMU_ID0, &id0) ||
        gpuReadRegister(probe.windows, GPB_WIN_SMMU, GPB_SMMU_ID1, &id1) ||
        gpuReadRegister(probe.windows, GPB_WIN_SMMU, GPB_SMMU_ID2, &id2))
        return 4;
    printf("GPU50 SMMU IDR0=%08x IDR1=%08x IDR2=%08x\n", id0, id1, id2);
    return id0 != 0 && id0 != UINT32_MAX && id1 != UINT32_MAX && id2 != UINT32_MAX ? 5 : 4;
}

__attribute__((noinline))
unsigned gpuCxGmuInputs(GpuCxProbe &probe)
{
    // Stock IN2020 gpucc-kona.c and vdd-level.h: 200 MHz needs MIN_SVS.
    constexpr uint16_t minSvs = 48;
    const vm_offset_t mapping = ml_io_map_with_prot(GPB_CMD_DB_BASE, GPB_CMD_DB_BYTES, VM_PROT_READ);
    if (!mapping)
        return 1;
    const auto *db = reinterpret_cast<const uint8_t *>(mapping);
    gpb_cmd_db_entry cx{}, mx{}, gfx{};
    if (gpb_cmd_db_find(db, GPB_CMD_DB_BYTES, "cx.lvl", &cx) ||
        gpb_cmd_db_find(db, GPB_CMD_DB_BYTES, "mx.lvl", &mx) ||
        gpb_cmd_db_find(db, GPB_CMD_DB_BYTES, "gfx.lvl", &gfx))
        return 2;
    if (cx.slave != 3 || mx.slave != 3 || gfx.slave != 3 ||
        cx.aux_bytes < 2 || cx.aux_bytes > 32 || (cx.aux_bytes & 1))
        return 2;
    unsigned index = 0;
    uint16_t level = 0;
    for (; index < cx.aux_bytes / 2; ++index) {
        level = gpb_le16(cx.aux + index * 2);
        if (level >= minSvs)
            break;
    }
    if (index == cx.aux_bytes / 2)
        return 3;
    printf("GPU51 command-db cx=%x mx=%x gfx=%x min=%u level=%u hlvl=%u bytes=%u/%u/%u\n",
           cx.address, mx.address, gfx.address, minSvs, level, index,
           cx.aux_bytes, mx.aux_bytes, gfx.aux_bytes);

    uint32_t cmd, cfg, gmu, cxo, pllVote, axi;
    if (gpuCxRead(&probe, GPB_WIN_GPUCC, GPB_CC_GMU_RCGR, &cmd) ||
        gpuCxRead(&probe, GPB_WIN_GPUCC, GPB_CC_GMU_RCGR + 4, &cfg) ||
        gpuCxRead(&probe, GPB_WIN_GPUCC, GPB_CC_CX_GMU_CLK, &gmu) ||
        gpuCxRead(&probe, GPB_WIN_GPUCC, GPB_CC_CXO_CLK, &cxo) ||
        gpuCxRead(&probe, GPB_WIN_GCC_VOTE, GPB_GCC_PLL_VOTE, &pllVote) ||
        gpuCxRead(&probe, GPB_WIN_GCC_GPU, GPB_GCC_DDRSS_GPU_AXI, &axi))
        return 4;
    printf("GPU51 GMU clocks cmd=%08x cfg=%08x gmu=%08x cxo=%08x pll=%08x axi=%08x\n",
           cmd, cfg, gmu, cxo, pllVote, axi);
    return 5;
}

__attribute__((noinline))
void gpuProbeCx()
{
    unsigned enable = 0;
    if (!PE_parse_boot_argn("sm8250-gpu-cx", &enable, sizeof(enable)) || enable < 1 || enable > 3)
        return;

    GpuCxProbe probe;
    constexpr unsigned clocks = (1u << (GPB_WIN_GPUCC + 1)) - 1;
    const gpb_io io = {&probe, gpuCxRead, gpuCxWrite, gpuCxDelay};
    gpb_result result{};
    printf("GPU49 mode3 begin\n");
    gpb_run(&io, 3, clocks, &result);

    // Report the active window before undoing the temporary diagnostic votes.
    printf("GPU49 active cfg=%08x cx=%08x hw=%08x gx=%08x ahb=%08x smmu=%08x memnoc=%08x dvm=%08x\n",
           result.cfg_ahb, result.cx_gdscr, result.cx_hwctrl, result.gx_gdscr,
           result.cc_ahb, result.cc_smmu, result.memnoc, result.dvm);
    const unsigned smmu = enable >= 2 ? gpuCxSampleSmmu(probe, result) : 0;
    const unsigned gmu = enable == 3 && smmu == 5 ? gpuCxGmuInputs(probe) : 0;
    unsigned writes = 0, restored = 0;
    for (int i = 2; i >= 0; --i) {
        const auto &vote = probe.votes[i];
        if (!vote.changed)
            continue;
        writes |= 1u << i;
        uint32_t current, after;
        if (gpuCxRead(&probe, GPB_WIN_GPUCC, vote.offset, &current))
            continue;
        gpuCxStore(probe, vote.offset, (current & ~vote.mask) | vote.original);
        if (!gpuCxRead(&probe, GPB_WIN_GPUCC, vote.offset, &after) &&
            (after & vote.mask) == vote.original)
            restored |= 1u << i;
    }
    printf("GPU49 mode3 complete gcc=%u cc=%u cx=%u clocks=%u io=%d writes=%x restored=%x\n",
           result.gcc, result.cc, result.powered_cx, result.powered_clocks,
           result.io_error, writes, restored);
    if (enable >= 2)
        printf("GPU50 smmu complete stage=%u restored=%x/%x\n", smmu, restored, writes);
    if (enable == 3)
        printf("GPU51 gmu inputs complete stage=%u restored=%x/%x\n", gmu, restored, writes);
}
}
