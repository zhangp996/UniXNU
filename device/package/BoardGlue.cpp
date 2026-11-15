/*
 * Board glue for the first fresh-core build.
 *
 * These symbols are provided by the board layer:
 *  - the GPU addon counters the display probe reads (the mount implementation lives in the
 *    kernel's gpu-addon patch; its counters are not linked in this build yet),
 *  - the DPU map/state hooks the official GopFramebuffer driver treats as optional,
 *  - the HID allocator entry (real NativeHidInput is wired in a later step; this is a null stub),
 *  - the UFS root-trace snapshot (the trace backend is a later step).
 * They are device-repository code, not framework imports.
 */
#include <IOKit/IOService.h>
#include <stdint.h>
#include "../drivers/Sm8250Ufs/RootTrace.h"

extern "C" {
volatile int native_gpu_addon_files = 0;
volatile int native_gpu_addon_written = 0;
volatile int native_gpu_addon_failed = 0;
volatile int native_gpu_addon_stage = 0;
volatile int native_gpu_addon_status = -1;

volatile uint32_t native_ufs_dpu_state = 0;

uint32_t
native_ufs_dpu_map(unsigned slot, const uint64_t *pages, unsigned count)
{
	(void)slot; (void)pages; (void)count;
	return 0;
}

IOService *
native_hid_input_allocate(void)
{
	return nullptr;
}
}

uint32_t
native_smufs_root_trace_snapshot(SmUfsRootTraceSnapshot *out)
{
	if (out) {
		*out = SmUfsRootTraceSnapshot{};
	}
	return 0;
}

/* Bring-up diagnostics and GOP handoff helpers that the board-layer probe references.
 * They are empty placeholders in this first fresh-core build; the device records keep the
 * exact behaviour as a later bring-up step. */
extern "C" {
char boot_ui_trace_rows[14][120];
char boot_process_rows[10][120];
char boot_progress_ext[8][48];

void
boot_ui_trace_snapshot(unsigned seconds)
{
	(void)seconds;
}

void
boot_process_snapshot(void)
{
}

void
boot_process_start(void)
{
}

int
gpu_metal_start(void)
{
	return 0;
}

volatile uint32_t native_adreno_tunnel_methods[64];
volatile uint32_t native_adreno_unknown_abi[64];
volatile uint32_t native_adreno_method_fail[64];
volatile uint32_t native_adreno_vm_stats[40];

void
native_gop_handoff_close(void)
{
}
}
