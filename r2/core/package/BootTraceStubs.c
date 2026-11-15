// SPDX-License-Identifier: MIT
// sheng's ui-trace probes reference kernel hook variables that this port does not define (they belong to
// sheng's own exit/fault/wait instrumentation). The kernel link does not honour weak imports, so the
// symbols must exist; nothing in this port calls them, and the probes only assign their handlers when the
// variables are present.
#include <stdint.h>
#include <stdbool.h>

struct arm_saved_state;

void (*sheng_user_exit_hook)(int, int, uint32_t, uint64_t, const char *, const char *);
void (*sheng_unnest_hook)(uint64_t, uint64_t);
void (*sheng_user_fault_hook)(int, const int64_t *, unsigned);
void (*sheng_sb_wait_hook)(unsigned, uint64_t, unsigned, int, uint32_t);
uint64_t sheng_sb_wait_tid;
bool (*sheng_pmu_fixup)(struct arm_saved_state *);

uint64_t
native_gpu_role_thread(uint32_t pid, uint32_t role)
{
	(void)pid;
	(void)role;
	return 0;
}
