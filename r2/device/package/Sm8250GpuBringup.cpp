// SPDX-License-Identifier: BSD-2-Clause-Patent
// IN2020 GPU stage: map the register windows, run the staged power/clock bring-up (CX/GDSC, SMMU vote clocks,
// GMU clocks), then the A650 cold start (SMMU routes, GMU firmware through HFI, GX, ZAP, CP init, publish) and
// finally register the accelerator the Metal plugin binds to. Reached from LegacyDisplayProbe only with
// boot-arg gpu-metal=1. Everything is gated: a stage that does not show its prerequisite is reported and the
// sequence stops before touching a block whose clock is off (reading one can hang the bus).
#include <IOKit/IOLib.h>
#include <mach/vm_prot.h>
#include <stdint.h>
#include "GpuPowerBoardAlioth.h"

extern "C" {
vm_offset_t ml_io_map_with_prot(vm_offset_t, vm_size_t, vm_prot_t);
bool sm8250_alioth_gmu_cold_start(unsigned mode, const gpb_io *io);
int sm8250_alioth_metal_register(void);
}

namespace {
vm_offset_t gWindows[GPB_WINDOWS];

int windowRead(void *cookie, unsigned window, uint32_t offset, uint32_t *out)
{
	(void)cookie;
	if (window >= GPB_WINDOWS || !gWindows[window] || !out) {
		return -1;
	}
	*out = *reinterpret_cast<volatile const uint32_t *>(gWindows[window] + offset);
	return 0;
}

int windowWrite(void *cookie, unsigned window, uint32_t offset, uint32_t value)
{
	(void)cookie;
	if (window >= GPB_WINDOWS || !gWindows[window]) {
		return -1;
	}
	*reinterpret_cast<volatile uint32_t *>(gWindows[window] + offset) = value;
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

void windowDelay(void *cookie, unsigned microseconds)
{
	(void)cookie;
	IODelay(microseconds);
}

const uint64_t kWindowBase[GPB_WINDOWS] = {
    GPB_GCC_GPU_PAGE, GPB_GCC_VOTE_PAGE, GPB_GPUCC_BASE, GPB_SMMU_BASE, GPB_GPU_BASE, GPB_GMU_BASE, GPB_RSCC_BASE};
const vm_size_t kWindowBytes[GPB_WINDOWS] = {
    0x1000, 0x1000, (vm_size_t)GPB_GPUCC_SIZE, (vm_size_t)GPB_SMMU_GR0_SIZE, (vm_size_t)GPB_GPU_WINDOW,
    (vm_size_t)GPB_GMU_SIZE, (vm_size_t)GPB_RSCC_SIZE};
}

extern "C" int
gpu_metal_start(void)
{
	for (unsigned i = 0; i < GPB_WINDOWS; ++i) {
		gWindows[i] = ml_io_map_with_prot(kWindowBase[i], kWindowBytes[i], VM_PROT_READ | VM_PROT_WRITE);
		if (!gWindows[i]) {
			printf("GPU: window %u (%llx) map failed\n", i, (unsigned long long)kWindowBase[i]);
			return -1;
		}
	}
	printf("GPU: seven windows mapped\n");
	gpb_io io = {nullptr, windowRead, windowWrite, windowDelay};
	const unsigned have = (1u << GPB_WINDOWS) - 1u;

	/* Stage 1-3: GCC AHB clock, GPUCC reads, gpu_cx_gdsc on, and the SMMU vote clocks. Nothing past this point may
	 * touch the SMMU or GMU unless CX is powered. */
	gpb_result power = {};
	gpb_run(&io, 3, have, &power);
	printf("GPU: power gcc=%u cc=%u smmu=%u cfg_ahb=%08x>%08x cx_gdscr=%08x hwctrl=%08x cx=%u clocks=%u err=%d\n",
	    power.gcc, power.cc, power.smmu, power.cfg_ahb, power.cfg_ahb_after, power.cx_gdscr, power.cx_hwctrl,
	    power.powered_cx, power.powered_clocks, power.io_error);
	if (power.io_error || power.cc != GPB_DONE || !power.powered_cx) {
		printf("GPU: CX power did not come up; stopping before the SMMU\n");
		return -2;
	}

	/* Stage 6: the GMU clocks a6xx_gmu_resume enables, at the 200 MHz OPP the HFI startup table wants. */
	gpb_gmu_clock_on clocks = {};
	gpb_gmu_clocks_on(&io, &power, &clocks);
	printf("GPU: gmu clocks step=%u failed=%d rcg_polls=%u branch_polls=%u vote=%08x gmu_cmd=%08x gmu_cfg=%08x cx_gmu=%08x ahb=%08x cxo=%08x axi=%08x memnoc=%08x pll1=%08x\n",
	    clocks.step, clocks.failed, clocks.rcg_polls, clocks.branch_polls, clocks.vote, clocks.gmu_cmd, clocks.gmu_cfg,
	    clocks.cx_gmu, clocks.ahb, clocks.cxo, clocks.axi, clocks.memnoc, clocks.pll1_mode);
	if (clocks.failed) {
		printf("GPU: GMU clocks did not come up; stopping before the cold start\n");
		return -3;
	}

	printf("GPU: A650 cold start mode 12\n");
	if (!sm8250_alioth_gmu_cold_start(12, &io)) {
		printf("GPU: cold start reported failure\n");
		return -4;
	}
	const int registered = sm8250_alioth_metal_register();
	printf("GPU: metal register rc=%d\n", registered);
	return registered;
}
