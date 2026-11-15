// SPDX-License-Identifier: MIT
// Board diagnostic: report the ARM feature registers the kernel sees on this SoC.  Userland (libdispatch
// among others) selects atomic and cache-line paths from these values, so a mismatch with the hardware
// shows up as a different code path rather than as a fault.
#include <IOKit/IOService.h>
#include <pexpert/pexpert.h>

#define READ_SYSREG(reg) ({ unsigned long long v; __asm__ volatile("mrs %0, " #reg : "=r"(v)); v; })

void
sheng_cpu_features_note(void)
{
	unsigned enabled = 0;
	if (!PE_parse_boot_argn("cpu-features", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	unsigned long long midr = READ_SYSREG(midr_el1);
	unsigned long long ctr = READ_SYSREG(ctr_el0);
	unsigned long long isar0 = READ_SYSREG(id_aa64isar0_el1);
	unsigned long long isar1 = READ_SYSREG(id_aa64isar1_el1);
	unsigned long long mmfr0 = READ_SYSREG(id_aa64mmfr0_el1);
	unsigned long long pfr0 = READ_SYSREG(id_aa64pfr0_el1);
	printf("CPUF: midr=%llx ctr=%llx isar0=%llx isar1=%llx mmfr0=%llx pfr0=%llx\n",
	    midr, ctr, isar0, isar1, mmfr0, pfr0);
	printf("CPUF: lse=%llu atomics=%llu dminline=%llu iminline=%llu\n",
	    (isar0 >> 20) & 0xf, (isar0 >> 4) & 0xf, (ctr >> 16) & 0xf, (ctr >> 0) & 0xf);
}
