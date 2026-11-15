// SPDX-License-Identifier: MIT
// Board-side PMGR bring-up, minimal form: load the prelinked ApplePMGR kext so the platform has a PMGR provider,
// without declaring its ABI (the reference port publishes a nub through a reconstructed class layout; that needs
// OriginalPMGRABI.h, which no public repository carries).  sheng-pmgr=<n> selects it; 0 or absent leaves it off.
#include <IOKit/IOService.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>

#ifdef __cplusplus
extern "C" {
#endif

extern kern_return_t OSKextLoadKextWithIdentifier(const char *);

void
sheng_pmgr_run(void)
{
	unsigned mode = 0;
	if (!PE_parse_boot_argn("sheng-pmgr", &mode, sizeof(mode)) || mode == 0)
		return;
	kern_return_t result = OSKextLoadKextWithIdentifier("com.apple.driver.ApplePMGR");
	printf("PMGR: load result=0x%x mode=%u\n", result, mode);
}

#ifdef __cplusplus
}
#endif
