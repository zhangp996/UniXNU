/*
 * Weak definitions for every board-local name UniXNU-sheng's diagnostic renderer reports but
 * this port does not provide: the Virtio transport, the verified-block chain, the Synaptics
 * touch rows, the legacy display, Adreno, the sheng PMGR cpu-frequency poll, the UI-trace and
 * boot-process probes, the SMP/IRQ counters and the fault/exit/breakpoint hooks that ship in
 * sheng's companion probes. The kernel link does not honour weak_import, so a name the
 * renderer mentions must exist somewhere.
 *
 * They are weak on purpose: a strong definition from the kernel or a component still wins, so
 * the KonaUfs counters keep reporting and only the rows this board cannot fill read zero.
 * native_ufs_progress_text is deliberately absent - it proves the KonaUfs archive was linked,
 * and a stub would hide its absence. The kernel API (vm_page_*, proc_self*, host_statistics64,
 * max_mem, nprocs) is left to the kernel. Generated from the extern declarations of
 * package/BootProgress.c.
 */
#include <stdint.h>
#include <stddef.h>

volatile uint64_t native_ufs_block_reads __attribute__((weak)) = 0;
volatile uint64_t native_ufs_verified_blocks __attribute__((weak)) = 0;
volatile uint64_t native_ufs_verify_ticks __attribute__((weak)) = 0;
volatile uint64_t native_ufs_read_ticks __attribute__((weak)) = 0;
volatile uint64_t native_ufs_block_failures __attribute__((weak)) = 0;
volatile uint32_t native_ufs_integrity_failures __attribute__((weak)) = 0;
volatile uint32_t native_ufs_blocks_published __attribute__((weak)) = 0;
volatile uint32_t native_ufs_probe_calls __attribute__((weak)) = 0;
volatile uint32_t native_ufs_matched __attribute__((weak)) = 0;
volatile uint32_t native_ufs_root_step __attribute__((weak)) = 0;
const char *volatile native_ufs_root_failure __attribute__((weak)) = 0;
volatile uint32_t native_virtio_ready __attribute__((weak)) = 0;
volatile uint32_t native_virtio_read_requests __attribute__((weak)) = 0;
volatile uint32_t native_virtio_read_failures __attribute__((weak)) = 0;
volatile uint64_t native_virtio_verified_blocks __attribute__((weak)) = 0;
volatile uint32_t native_ram_block_sparse __attribute__((weak)) = 0;
volatile uint64_t native_ram_block_bytes __attribute__((weak)) = 0;
volatile uint64_t native_ram_block_resident_bytes __attribute__((weak)) = 0;
volatile uint64_t native_ram_block_reads __attribute__((weak)) = 0;
volatile uint64_t native_ram_block_failures __attribute__((weak)) = 0;
__attribute__((weak)) void boot_progress_exit_fault(int boot_progress_exit_fault0, unsigned boot_progress_exit_fault1, char *boot_progress_exit_fault2, unsigned long boot_progress_exit_fault3) { (void)boot_progress_exit_fault0; (void)boot_progress_exit_fault1; (void)boot_progress_exit_fault2; (void)boot_progress_exit_fault3; }
__attribute__((weak)) void boot_progress_exit_reason(int boot_progress_exit_reason0, unsigned boot_progress_exit_reason1, char *boot_progress_exit_reason2, unsigned long boot_progress_exit_reason3) { (void)boot_progress_exit_reason0; (void)boot_progress_exit_reason1; (void)boot_progress_exit_reason2; (void)boot_progress_exit_reason3; }
char boot_progress_throw_text[330] __attribute__((weak));
__attribute__((weak)) void boot_ui_trace_snapshot(unsigned boot_ui_trace_snapshot0) { (void)boot_ui_trace_snapshot0; }
char boot_ui_trace_rows[14][120] __attribute__((weak));
char boot_ui_trace_label[120] __attribute__((weak));
unsigned boot_ui_trace_ready __attribute__((weak)) = 0;
volatile unsigned boot_ui_trace_phase __attribute__((weak)) = 0;
volatile unsigned boot_ui_trace_index __attribute__((weak)) = 0;
__attribute__((weak)) void boot_process_capture() { }
__attribute__((weak)) void boot_process_snapshot() { }
__attribute__((weak)) void boot_log_tick(unsigned boot_log_tick0) { (void)boot_log_tick0; }
__attribute__((weak)) void sheng_cpufreq_poll() { }
char boot_process_rows[10][120] __attribute__((weak));
__attribute__((weak)) unsigned boot_ui_trace_role_rows(unsigned boot_ui_trace_role_rows0, char boot_ui_trace_role_rows1[][120], unsigned boot_ui_trace_role_rows2, unsigned boot_ui_trace_role_rows3) { (void)boot_ui_trace_role_rows0; (void)boot_ui_trace_role_rows1; (void)boot_ui_trace_role_rows2; (void)boot_ui_trace_role_rows3; return 0; }
char native_touch_rows[4][120] __attribute__((weak));
char native_cpufreq_row[120] __attribute__((weak));
volatile uint32_t native_hid_input_events[8] __attribute__((weak));
volatile int native_hid_input_results[8] __attribute__((weak));
volatile uint32_t native_touch_loop_step __attribute__((weak)) = 0;
volatile uint32_t native_touch_seq __attribute__((weak)) = 0;
volatile uint32_t native_touch_count __attribute__((weak)) = 0;
volatile int native_touch_xs[12] __attribute__((weak));
volatile int native_touch_ys[12] __attribute__((weak));
volatile int native_touch_ids[12] __attribute__((weak));
volatile uint32_t native_cpufreq_stage __attribute__((weak)) = 0;
volatile unsigned native_touch_stage_req __attribute__((weak)) = 0;
volatile unsigned boot_process_passes __attribute__((weak)) = 0;
volatile unsigned boot_process_seen __attribute__((weak)) = 0;
volatile unsigned boot_process_attempts __attribute__((weak)) = 0;
volatile unsigned boot_process_busy __attribute__((weak)) = 0;
volatile uint32_t armcpu_smp_requested __attribute__((weak)) = 0;
volatile uint32_t armcpu_smp_running __attribute__((weak)) = 0;
volatile uint32_t armcpu_smp_conduit __attribute__((weak)) = 0;
volatile uint32_t armcpu_boot_mpidr __attribute__((weak)) = 0;
volatile uint32_t armcpu_smp_step[8] __attribute__((weak));
volatile uint32_t armcpu_smp_seen_mpidr[8] __attribute__((weak));
volatile uint32_t armcpu_smp_fail[8] __attribute__((weak));
volatile uint32_t armcpu_smp_sys[8][5] __attribute__((weak));
volatile uint32_t armcpu_boot_sys[5] __attribute__((weak));
volatile uint32_t armcpu_gic_stride __attribute__((weak)) = 0;
volatile uint32_t armcpu_gic_typer_probe[2] __attribute__((weak));
volatile int32_t armcpu_smp_psci_rc[8] __attribute__((weak));
volatile uint32_t armcpu_irq_phase __attribute__((weak)) = 0;
volatile uint32_t armcpu_irq_id __attribute__((weak)) = 0;
volatile uint32_t armcpu_quiesce_calls __attribute__((weak)) = 0;
volatile uint32_t armcpu_halt_calls __attribute__((weak)) = 0;
volatile int native_owner_root_accepted __attribute__((weak)) = 0;
volatile uint32_t native_legacy_stage __attribute__((weak)) = 0;
volatile uint32_t native_legacy_pm_stage __attribute__((weak)) = 0;
volatile uint32_t native_legacy_pm_calls __attribute__((weak)) = 0;
volatile uint32_t native_legacy_pm_callbacks __attribute__((weak)) = 0;
volatile uint32_t native_legacy_swap_validation_calls __attribute__((weak)) = 0;
volatile uint32_t native_legacy_swap_validation_result __attribute__((weak)) = 0;
volatile uint32_t native_legacy_swap_validation_reason __attribute__((weak)) = 0;
volatile uint32_t native_legacy_swap_accepted_count __attribute__((weak)) = 0;
volatile uint32_t native_legacy_swap_rejected_count __attribute__((weak)) = 0;
volatile uint64_t native_legacy_rej_surface[7] __attribute__((weak));
volatile uint32_t native_legacy_rej_reason __attribute__((weak)) = 0;
volatile uint64_t native_legacy_rej_surfaces[4] __attribute__((weak));
volatile uint32_t native_legacy_rej_state[3] __attribute__((weak));
volatile uint32_t native_legacy_rej_options[3] __attribute__((weak));
volatile uint32_t native_legacy_rej_size[2] __attribute__((weak));
volatile uint32_t native_legacy_rej_rects[2][4] __attribute__((weak));
char native_legacy_rej_class[32] __attribute__((weak));
volatile uint32_t native_legacy_frame_count __attribute__((weak)) = 0;
volatile uint32_t native_legacy_frame_kind[32] __attribute__((weak));
volatile uint32_t native_legacy_frame_time[32] __attribute__((weak));
volatile uint64_t native_legacy_frame_stats[32][5] __attribute__((weak));
volatile uint32_t native_legacy_frame_colors[8][4] __attribute__((weak));
volatile uint64_t native_legacy_frame_color_counts[8][4] __attribute__((weak));
volatile uint64_t native_legacy_frame_color_other[8] __attribute__((weak));
volatile uint32_t native_legacy_transfer_calls __attribute__((weak)) = 0;
volatile uint32_t native_legacy_retirement_calls __attribute__((weak)) = 0;
volatile uint32_t native_legacy_guard_slot __attribute__((weak)) = 0;
volatile uint32_t native_legacy_prepare_calls __attribute__((weak)) = 0;
volatile uint32_t native_legacy_prepare_reason __attribute__((weak)) = 0;
volatile uint32_t native_legacy_prepare_attempts __attribute__((weak)) = 0;
volatile uint32_t native_legacy_client_calls __attribute__((weak)) = 0;
volatile uint32_t native_legacy_client_type __attribute__((weak)) = 0;
volatile int native_legacy_client_pid __attribute__((weak)) = 0;
volatile int native_legacy_client_status __attribute__((weak)) = 0;
char native_legacy_client_class[64] __attribute__((weak));
volatile uint64_t native_legacy_gop_virtual __attribute__((weak)) = 0;
volatile uint64_t native_legacy_gop_physical __attribute__((weak)) = 0;
volatile uint64_t native_legacy_gop_bytes __attribute__((weak)) = 0;
volatile int native_legacy_transfer_status __attribute__((weak)) = 0;
volatile uint64_t native_legacy_transfer_bytes __attribute__((weak)) = 0;
volatile uint32_t native_data_persist_state __attribute__((weak)) = 0;
volatile uint32_t native_data_persist_header __attribute__((weak)) = 0;
volatile uint32_t native_data_persist_gpt __attribute__((weak)) = 0;
volatile uint32_t native_data_persist_gpt_after __attribute__((weak)) = 0;
volatile uint64_t native_data_persist_pages __attribute__((weak)) = 0;
volatile uint64_t native_data_persist_loaded __attribute__((weak)) = 0;
volatile uint64_t native_data_persist_writes __attribute__((weak)) = 0;
volatile uint64_t native_data_persist_syncs __attribute__((weak)) = 0;
volatile uint64_t native_data_persist_errors __attribute__((weak)) = 0;
volatile uint64_t native_data_persist_first_lba __attribute__((weak)) = 0;
volatile uint64_t native_data_persist_present __attribute__((weak)) = 0;
volatile uint64_t native_legacy_copy_n __attribute__((weak)) = 0;
volatile uint64_t native_gop_gpu_offers __attribute__((weak)) = 0;
volatile uint64_t native_gop_gpu_completions __attribute__((weak)) = 0;
volatile uint64_t native_gop_gpu_failures __attribute__((weak)) = 0;
volatile uint64_t native_gop_gpu_abandoned __attribute__((weak)) = 0;
volatile uint32_t native_legacy_gpu_fallbacks __attribute__((weak)) = 0;
volatile uint32_t native_legacy_gpu_last_status __attribute__((weak)) = 0;
volatile uint64_t native_legacy_copy_last __attribute__((weak)) = 0;
volatile uint64_t native_legacy_early_retires __attribute__((weak)) = 0;
volatile uint64_t native_legacy_early_failures __attribute__((weak)) = 0;
volatile uint64_t native_legacy_copy_max __attribute__((weak)) = 0;
volatile uint64_t native_legacy_copy_sum __attribute__((weak)) = 0;
volatile uint64_t native_legacy_copy_bytes_last __attribute__((weak)) = 0;
volatile uint64_t native_legacy_copy_bytes_sum __attribute__((weak)) = 0;
char native_wi_log[4][100] __attribute__((weak));
volatile unsigned native_wi_count __attribute__((weak)) = 0;
volatile uint64_t armcpu_timer_interrupts __attribute__((weak)) = 0;
volatile uint64_t native_adreno_cache_stats[5] __attribute__((weak));
volatile uint32_t native_adreno_coherence[8] __attribute__((weak));
volatile uint32_t native_adreno_wc_distrusted __attribute__((weak)) = 0;
volatile uint32_t native_adreno_wc_audit[6] __attribute__((weak));
volatile uint64_t native_adreno_reassert_ticks __attribute__((weak)) = 0;
volatile uint64_t native_adreno_audit_ticks __attribute__((weak)) = 0;
volatile uint32_t native_adreno_audit_attr __attribute__((weak)) = 0;
volatile uint64_t armcpu_ipi_interrupts __attribute__((weak)) = 0;
__attribute__((weak)) uint64_t boot_diag_scan(uint64_t boot_diag_scan0) { (void)boot_diag_scan0; return 0; }
__attribute__((weak)) int boot_diag_stream(int (*boot_diag_stream0)(const void *, uint32_t, void *), void *boot_diag_stream1) { (void)boot_diag_stream0; (void)boot_diag_stream1; return 0; }
__attribute__((weak)) void boot_progress_critical_exit(int a0, char *a1, unsigned long a2) { (void)a0; (void)a1; (void)a2; }
__attribute__((weak)) void boot_progress_user_abort(void *a0, unsigned a1, uint64_t a2) { (void)a0; (void)a1; (void)a2; }
__attribute__((weak)) void boot_progress_user_breakpoint(void *a0, unsigned a1) { (void)a0; (void)a1; }
/*
 * The Data mount path in zzz-boot-progress.patch calls sheng's watchdog launch policy.
 * Kona has no sheng PMGR or watchdog policy, so the call is a no-op here; this weak
 * definition satisfies the link without pretending a policy was applied.
 */
__attribute__((weak)) int sheng_boot_watchdog_policy(void *process, uint64_t scratch) { (void)process; (void)scratch; return 0; }
/*
 * VirtioMmioBlock declares this weak hook for a UFS window provider. This board's KonaUfs
 * is built without NATIVE_VERIFIED_BLOCK during bring-up, so no provider defines it and the
 * kernel link still needs the symbol. A weak no-op leaves the UFS window unauthenticated while
 * the Virtio archive provides the owner-root policy functions.
 */
__attribute__((weak)) int native_ufs_block_authenticates(void *provider, const char *uuid, uint8_t *digest) { (void)provider; (void)uuid; (void)digest; return 0; }
