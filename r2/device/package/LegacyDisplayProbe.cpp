// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board display probe for instantnoodlep: drives the GopFramebuffer legacy provider with the
// firmware GOP console and answers the diagnostics the component asks for.  Bring-up instrument,
// not a desktop claim.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>   // also declares IOMemoryMap
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSerialize.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSKext.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <pexpert/boot.h>
#include <pexpert/arm/protos.h>
#include <machine/machine_routines.h>

extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void native_legacy_display_static_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
extern "C" IOService *native_legacy_display_allocate(void);

extern "C" void native_legacy_display_probe(void);
extern "C" void native_legacy_display_late_probe(void);
extern "C" OSReturn OSKextLoadKextWithIdentifier(const char *);
/* Board diagnostic: the logical index the kernel assigns to each physical MPIDR.  The staged SMP
 * gate selects cores by logical index, so this mapping decides whether all eight physical cores are
 * covered by sheng-smp-max.  Printed once, the first time the probe runs. */
extern int ml_get_cpu_number(uint32_t phys);
#include <sys/vnode.h>
#include "VarExpect.h"

/* Compare the Data volume's /private/var with the checklist the reference seed builder copies
 * from the OS volume (ye3912/apfs scripts/expected-var.json).  A missing entry is a real gap in
 * this port's seed, not a difference of opinion about the layout. */
/* Read the feature registers userland actually sees.  libdispatch picks its atomic path from
 * ID_AA64ISAR0_EL1.Atomic and its cache-line size from CTR_EL0.DminLine, and nothing in this
 * kernel or its firmware traps those registers, so these values are what a userland process reads. */
/* The keybag counters only feed the framebuffer banner; print them to serial as well, because
 * the offline AppleKeyStore state service is what liveactivitiesd needs before chronod can start. */
extern uint32_t native_aks_opens, native_aks_calls;
/* The keybag probes were compiled but never called, so the offline AppleKeyStore state service was
 * never created: the counters read opens=0 calls=0 and the stage variables stayed at zero.  Create
 * it here, before userland daemons ask for keybag state. */
extern "C" void native_keystore_probe(void);
extern "C" void synthetic_touch_start(void);
extern "C" void synthetic_touch_tap(int, int);
extern "C" void hakios_s6sy791_init(void);
extern "C" void MiniAcceleratorPublishStub(IOService *);
extern "C" void OppoBacklightPublish(IOService *);
extern "C" void native_board_backlight_start(void);
extern "C" void synthetic_touch_release(void);

extern volatile uint32_t native_keyevents_stage, native_keystore_stage;
extern volatile int native_keyevents_status, native_keystore_load_status;
extern volatile uint64_t native_keystore_service;

static void report_aks_stage_once(void)
{
	printf("AKSSTAGE keyevents_stage=%u keyevents_status=%d keystore_stage=%u load_status=%d service=%llx\n",
	    native_keyevents_stage, native_keyevents_status, native_keystore_stage,
	    native_keystore_load_status, (unsigned long long)native_keystore_service);
}

static void report_aks_once(void)
{
	printf("AKSCOUNT opens=%u calls=%u\n", native_aks_opens, native_aks_calls);
}

static void report_atomics_once(void)
{
	unsigned long long isar0, ctr;
	__asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	printf("LSEATOM isar0=%llx atomic=%llu lse=%llu ctr=%llx dminline=%llu iminline=%llu\n",
	    isar0, (isar0 >> 4) & 0xf, (isar0 >> 20) & 0xf, ctr, (ctr >> 16) & 0xf, ctr & 0xf);
}

static void check_var_expected(void)
{
	char path[256];
	unsigned present = 0, missing = 0, printed = 0;

	for (unsigned i = 0; i < VAR_EXPECTED_COUNT; i++) {
		vnode_t vp = NULLVP;

		if (snprintf(path, sizeof(path), "/private/var/%s", var_expected[i]) >= (int)sizeof(path)) {
			continue;
		}
		if (vnode_lookup(path, 0, &vp, vfs_context_current()) == 0) {
			present++;
			vnode_put(vp);
		} else {
			missing++;
			if (printed < 40) {
				printf("VAREXP missing: %s\n", var_expected[i]);
				printed++;
			}
		}
	}
	printf("VAREXP summary: present=%u missing=%u total=%u\n", present, missing, VAR_EXPECTED_COUNT);
}
static void print_cpu_map_once(void)
{
	static bool done;
	if (done) {
		return;
	}
	done = true;
	for (unsigned i = 0; i < 8; i++) {
		printf("CPUMAP phys=%#x logical=%d\n", 0x100u * i, ml_get_cpu_number(0x100u * i));
	}
}
static IOService *pendingDisplay;
/* The published display, kept for the swap-completion call. */
static IOService *gDisplay;
static IOService *gSurfaceRoot;
static void publish_display(void);
static void clear_panel(PE_Video *video);
static void publish_thread(void *, wait_result_t);

/* The family completes a swap by calling completeCurrentSwap (vtable slot 312) once the
 * frame is out. Doing it inline races the client's SwapWait, which arms its own wait
 * afterwards; a short delay lets the wait arm first. */
static void
complete_thread(void *arg, wait_result_t)
{
	IOSleep(30);
	IOService *display = gDisplay;
	if (display) {
		typedef void (*complete_fn)(void *);
		complete_fn complete = ((complete_fn *)(*(void ***)display))[312];
		printf("DISPLAY: delayed completion via slot 312 (%p)\n", (void *)complete);
		complete(display);
	}
	(void)arg;
}

static void
complete_swap_later(void)
{
	unsigned enabled = 0;
	PE_parse_boot_argn("offline-legacy-complete", &enabled, sizeof(enabled));
	if (enabled != 1 || !gDisplay) {
		return;
	}
	thread_t thread = nullptr;
	if (kernel_thread_start(complete_thread, nullptr, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* The component's static initialiser is globalised in the object (see globalize_symbol.py), so the symbol the
 * reference implementation calls is the real one; nothing is defined here. */


/* PROBE-ROWS: mirror the diagnostic page's process and ui-trace rows to the serial console. The page
 * renderer is not reached on this port, so the probe starts its own printer. */
static void gpu_metal_report_once(void);
static void gpu_metal_diag_report(unsigned tick);
extern "C" char boot_process_rows[10][120];
extern "C" char boot_ui_trace_rows[14][120];
/* Both row arrays are only refreshed by these snapshot calls, which the diagnostic page makes; the page is
 * not rendered here, so the printer performs them itself. */
extern "C" void boot_process_snapshot(void);
extern "C" void boot_ui_trace_snapshot(unsigned seconds);
extern "C" int native_owner_setup_skip(void);
extern "C" int native_probe_iosurface_root(void);
extern "C" int native_create_iosurface(uint32_t, uint32_t, uint32_t, uint32_t);

/* Board bring-up: signal one process by name, for the experiment in which the connection a blocked client
 * waits on is torn down and has to be made again.  The process ABI here is the one BootProcessProbe.c uses:
 * opaque proc_t plus accessors, so no BSD private header is pulled into this translation unit. */
struct proc;
typedef struct proc *probe_proc_t;
extern "C" char *proc_best_name(probe_proc_t);
extern "C" int proc_pid(probe_proc_t);
extern "C" void proc_iterate(unsigned, int (*)(probe_proc_t, void *), void *,
    int (*)(probe_proc_t, void *), void *);
extern "C" void psignal(struct proc *, int);
#define PROBE_ALLPROCLIST (1U << 0)
#define PROBE_NOWAITTRANS (1U << 2)
struct kill_ctx { const char *name; int hits; };
static int
kill_one_proc(probe_proc_t p, void *ctx)
{
	struct kill_ctx *k = (struct kill_ctx *)ctx;
	const char *name = p ? proc_best_name(p) : nullptr;
	if (!k || !name) {
		return 0;
	}
	if (__builtin_strcmp(name, k->name) != 0) {
		/* Board bring-up: the names proc_best_name reports, so the experiment targets one that exists. */
		static int shown;
		if (shown < 12) {
			shown++;
			printf("DISPLAY: proc name '%s'\n", name);
		}
		return 0;
	}
	psignal((struct proc *)p, 9);
	k->hits++;
	return 1;
}
extern "C" int
native_kill_named(const char *name)
{
	struct kill_ctx k = { name, 0 };
	proc_iterate(PROBE_ALLPROCLIST | PROBE_NOWAITTRANS, kill_one_proc, &k, nullptr, nullptr);
	return k.hits;
}

static void
probe_rows_thread(void *, wait_result_t)
{
	static char lastp[10][120], lastu[14][120];
	for (;;) {
		delay(4000000);
		{
			static unsigned tick;
			boot_process_snapshot();
			boot_ui_trace_snapshot(tick += 4);
			/* The container tree only exists once the Data volume is up, so the LaunchServices store is
			 * dropped from here as well, a minute into the boot. */
			if (tick == 8) {
				{
					IOService *resource = IOService::getResourceService();
					printf("ACCEL: publishing the empty IOAcceleratorES, resource=%p\n", (void *)resource);
					MiniAcceleratorPublishStub(resource);
				{
					uint32_t bl = 0;
					if (PE_parse_boot_argn("backlight", &bl, sizeof(bl)) && bl == 1) {
						printf("BACKLIGHT: starting the IN2020 DSI service\n");
						native_board_backlight_start();
					}
				}
				}
				hakios_s6sy791_init();
				native_probe_iosurface_root();
			}
			if (tick == 12) {
				unsigned create = 0;
				/* Off by default: an IOUserClient2022 rejects the base-class externalMethod entry with
				 * "wrong externalMethod" and panics in a development kernel. */
				PE_parse_boot_argn("iosurface-create", &create, sizeof(create));
				if (create == 1) {
					native_create_iosurface(1080, 2376, 4352, 10342400);
				}
			}
			static unsigned realTouchScreen = 0;
			if (tick == 20) {
				/* With the real S6SY791 driver running, the synthetic taps would fight it for the
				 * native_touch_* seqlock; keep them only when the panel driver is off. */
				if (PE_parse_boot_argn("touchscreen", &realTouchScreen, sizeof(realTouchScreen))) {
					realTouchScreen = (realTouchScreen == 1);
				}
			}
			if (!realTouchScreen && tick >= 24 && (tick % 4) == 0) {
				static unsigned phase;
				if ((phase++ & 1) == 0) {
					synthetic_touch_tap(540, 1188);
					printf("SYNTOUCH down\n");
				} else {
					synthetic_touch_release();
					printf("SYNTOUCH up\n");
				}
			}
			if (tick % 4 == 0) {
				/* Board bring-up experiment: end lsd once, so the connection SpringBoard is waiting on is
				 * torn down and has to be made again.  kill-lsd=<tick> selects the sample (0 = never). */
				static unsigned kill_tick, killed;
				if (!kill_tick) {
					PE_parse_boot_argn("kill-lsd", &kill_tick, sizeof(kill_tick));
				}
				if (kill_tick && !killed && tick >= kill_tick) {
					killed = 1;
					printf("DISPLAY: killing lsd at tick %u: %d\n", tick, native_kill_named("lsd"));
				}
			}
			if (tick >= 40 && (tick % 20) == 0) {
				gpu_metal_diag_report(tick);
			}
			if (tick == 24) {
				gpu_metal_report_once();
			}
			if (tick == 24) {
				synthetic_touch_tap(16384, 16384);
				printf("SYNTOUCH tap sent\n");
				/* After the tick-20 AKS attempt: report where the service creation stopped. */
				report_aks_stage_once();
				report_aks_once();
			}
			if (tick == 20) {
				synthetic_touch_start();
				/* Re-run the Setup skip here: the early call runs before the Data volume covers
				 * /private/var, so the container tree it creates is hidden by the mount. */
				printf("DISPLAY: re-run the Setup skip on the mounted Data volume: %d\n",
				    native_owner_setup_skip());
				/* Now that /private/var is the Data volume, compare it with the reference checklist. */
				check_var_expected();
				report_atomics_once();
				report_aks_once();
				native_keystore_probe();
			}
		}
		for (unsigned i = 0; i < 10; i++) {
			const char *row = boot_process_rows[i];
			if (!row[0] || row[0] == ' ') continue;
			if (__builtin_memcmp(row, lastp[i], sizeof(lastp[i])) == 0) continue;
			__builtin_memcpy(lastp[i], row, sizeof(lastp[i]));
			printf("PROC[%u] %s\n", i, row);
		}
		for (unsigned i = 0; i < 14; i++) {
			const char *row = boot_ui_trace_rows[i];
			if (!row[0] || row[0] == ' ') continue;
			if (__builtin_memcmp(row, lastu[i], sizeof(lastu[i])) == 0) continue;
			__builtin_memcpy(lastu[i], row, sizeof(lastu[i]));
			printf("UIT[%u] %s\n", i, row);
		}
	}
}

static void
probe_rows_start(void)
{
	unsigned enabled = 0;
	thread_t thread;
	if (!PE_parse_boot_argn("boot-ui-trace", &enabled, sizeof(enabled)) || enabled != 1) return;
	if (kernel_thread_start(probe_rows_thread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}


/* The component-load probe (patches/xnu/offline-component-load-probes.patch) records the result of loading the
 * prelinked IOMobileGraphicsFamily; the display class's superclass comes from that family. */
extern "C" volatile int native_mobile_graphics_load_status;

/* Waits for the prelinked mobile-graphics family and then registers the display. */
static void
deferred_probe_thread(void *, wait_result_t)
{
	for (unsigned i = 0; i < 100; i++) {
		delay(100000);
		if (native_mobile_graphics_load_status == 0) {
			printf("DISPLAY: family ready after %u ms; registering\n", (i + 1) * 100);
			native_legacy_display_probe();
			return;
		}
	}
	printf("DISPLAY: family never became ready (status=%d)\n", (int)native_mobile_graphics_load_status);
}

/* --- functional entries ------------------------------------------------- */

extern "C" void
native_legacy_display_probe(void)
{
	static bool registered;

	printf("DISPLAY: probe entered (registered=%d) mobile-graphics-load=%d\n", registered ? 1 : 0,
	    (int)native_mobile_graphics_load_status);
	if (native_mobile_graphics_load_status != 0) {
		/* The display class's superclass lives in the prelinked IOMobileGraphicsFamily. Registering before
		 * that family is loaded leaves the superclass unresolved and postModLoad faults, so wait for it. */
		printf("DISPLAY: family not loaded yet (%d); deferring registration\n",
		    (int)native_mobile_graphics_load_status);
		static bool retrying;
		if (!retrying) {
			retrying = true;
			thread_t thread = nullptr;
			if (kernel_thread_start(deferred_probe_thread, nullptr, &thread) == KERN_SUCCESS) {
				thread_deallocate(thread);
			}
		}
		return;
	}
	if (registered) {
		return;
	}
	/* An embedded class registers itself: the kernel runs the object's __mod_init_func entry at startup and
	 * finalises it in its own module, exactly like the framework's other build_stage = "embedded" drivers.
	 * So there is nothing to preModLoad here - only a check that the class is present. */
	{
		const OSSymbol *className = OSSymbol::withCString("NativeLegacyDisplay");
		const OSMetaClass *meta = className ? OSMetaClass::getMetaClassWithName(className) : nullptr;
		printf("DISPLAY: embedded class NativeLegacyDisplay meta=%p size=0x%x\n", (void *)meta,
		    meta ? (unsigned)meta->getClassSize() : 0);
		if (className) className->release();
		if (!meta) {
			printf("DISPLAY: the embedded class is not registered\n");
			return;
		}
	}
	{
		extern void boot_process_start(void);
		boot_process_start();
	}
	probe_rows_start();
	/* postModLoad() is the kext-load finaliser and faults for a class that is statically linked into the
	 * kernel (OSMetaClass::postModLoad + 0x448).  The metaclass is already registered by the static
	 * initialiser above, so check that the class can be allocated instead of finalising a fake module. */
	/* Whether the class really became allocatable shows in the component's own factory below:
	 * native_legacy_display_allocate() news the class and prints what it got. */
	registered = true;

	PE_Video video = {};
	int console_rc = PE_current_console(&video);
	if (PE_state.bootArgs) {
		boot_args *ba = (boot_args *)PE_state.bootArgs;
		printf("DISPLAY: bootargs video base=0x%llx w=%lu h=%lu rb=%lu depth=%u display=%u\n",
		    (unsigned long long)ba->Video.v_baseAddr, (unsigned long)ba->Video.v_width,
		    (unsigned long)ba->Video.v_height, (unsigned long)ba->Video.v_rowBytes,
		    (unsigned)ba->Video.v_depth, (unsigned)ba->Video.v_display);
		{
			const uint64_t *raw = (const uint64_t *)ba;
			printf("DISPLAY: bootargs raw %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx\n",
			    (unsigned long long)raw[0], (unsigned long long)raw[1], (unsigned long long)raw[2],
			    (unsigned long long)raw[3], (unsigned long long)raw[4], (unsigned long long)raw[5],
			    (unsigned long long)raw[6], (unsigned long long)raw[7], (unsigned long long)raw[8],
			    (unsigned long long)raw[9]);
		}
	}
	if (console_rc || !video.v_baseAddr || !video.v_width || !video.v_height ||
	    video.v_depth != 32 || video.v_rotate || video.v_rowBytes < video.v_width * 4) {
		printf("DISPLAY: no usable GOP console (base=0x%llx %lux%lu depth=%lu rot=%lu stride=%llu)\n",
		    (unsigned long long)video.v_baseAddr, (unsigned long)video.v_width, (unsigned long)video.v_height,
		    (unsigned long)video.v_depth, (unsigned long)video.v_rotate, (unsigned long long)video.v_rowBytes);
		return;
	}
	printf("DISPLAY: GOP console 0x%llx %lux%lu depth=%lu stride=%llu\n",
	    (unsigned long long)video.v_baseAddr, (unsigned long)video.v_width, (unsigned long)video.v_height,
	    (unsigned long)video.v_depth, (unsigned long long)video.v_rowBytes);

	IOService *display = native_legacy_display_allocate();
	if (!display) {
		printf("DISPLAY: allocate failed\n");
		return;
	}
	printf("DISPLAY: allocated %s size=0x%x legacy=%d mobilefb=%d\n",
	    display->getMetaClass()->getClassName(),
	    (unsigned)display->getMetaClass()->getClassSize(),
	    display->metaCast("IOMobileFramebufferLegacy") ? 1 : 0,
	    display->metaCast("IOMobileFramebuffer") ? 1 : 0);
	{
		extern char boundParentMeta[] __asm__("__ZN25IOMobileFramebufferLegacy10gMetaClassE");
		const uint64_t *raw = (const uint64_t *)(void *)boundParentMeta;
		printf("DISPLAY: bound parent meta=%p raw=%llx %llx %llx %llx\n", (void *)raw,
		    (unsigned long long)raw[0], (unsigned long long)raw[1],
		    (unsigned long long)raw[2], (unsigned long long)raw[3]);
		const OSSymbol *name = OSSymbol::withCString("IOMobileFramebufferLegacy");
		const OSMetaClass *registered = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
		printf("DISPLAY: registered family meta=%p name=%s size=0x%x\n", (void *)registered,
		    registered ? registered->getClassName() : "-",
		    registered ? (unsigned)registered->getClassSize() : 0);
		if (name) name->release();
	}
	{
		const OSMetaClass *mc = display->getMetaClass();
		for (int depth = 0; mc && depth < 8; ++depth) {
			printf("DISPLAY: meta[%d] %s size=0x%x super=%s\n", depth,
			    mc->getClassName(), (unsigned)mc->getClassSize(),
			    mc->getSuperClass() ? mc->getSuperClass()->getClassName() : "-");
			mc = mc->getSuperClass();
		}
	}
	if (display->init()) {
		thread_t thread = nullptr;
		if (kernel_thread_start(publish_thread, nullptr, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
		unsigned phase = 0;
		PE_parse_boot_argn("offline-legacy-phase", &phase, sizeof(phase));
		printf("DISPLAY: init ok phase=%u\n", phase);
		/* The kernel's offline-mobile-graphics-load probe calls this entry from bsd_init, right after
		 * vfsinit; publishing here would block the boot thread (the IOSurface match waits), which the
		 * storage root cannot afford.  Hand the publication to a kernel thread instead. */
		pendingDisplay = display;
	} else {
		printf("DISPLAY: init failed\n");
		display->release();
	}
}

/* Map the fields the family keeps in the display object: every 8-byte slot that holds a kernel
 * pointer is printed with its offset, so the default-surface field can be recognised once anything
 * writes it.  A fault here panics with the offset already on the console. */
static void
scan_display_fields(IOService *object)
{
	unsigned char *base = (unsigned char *)object;
	unsigned found = 0;
	printf("DISPLAY: field scan of the 0x18a0 object\n");
	for (unsigned off = 0; off + 8 <= 0x18a0 && found < 48; off += 8) {
		uintptr_t value = *(uintptr_t *)(base + off);
		/* The family stores a 0xffffffff00000000 pattern in some slots; it is not a pointer. */
		if (value < 0xfffffff000000000ULL || value >= 0xffffffff00000000ULL) {
			continue;
		}
		uintptr_t vtable = *(uintptr_t *)value;
		printf("  +0x%03x = 0x%llx vtable=0x%llx\n", off, (unsigned long long)value,
		    (unsigned long long)vtable);
		found++;
	}
	printf("DISPLAY: field scan done (%u pointer slots)\n", found);
}

/* Create one BGRA IOSurface through the prelinked IOSurfaceRoot factory (vtable slot 141, the method
 * that keeps the surface count and the tail queue).  Returns null on any failure; every step is logged. */
static OSObject *__attribute__((unused))
create_surface(uint32_t width, uint32_t height, uint32_t stride)
{
	if (!gSurfaceRoot) {
		printf("DISPLAY: no IOSurfaceRoot to create a surface on\n");
		return nullptr;
	}
	OSDictionary *props = OSDictionary::withCapacity(8);
	if (!props) {
		return nullptr;
	}
	uint32_t pixel_format = 0x42475241; /* 'BGRA' */
	uint32_t alloc = (uint32_t)(stride * height);
	struct { const char *key; uint32_t value; } words[] = {
		{"IOSurfaceWidth", width},
		{"IOSurfaceHeight", height},
		{"IOSurfaceBytesPerRow", stride},
		{"IOSurfacePixelFormat", pixel_format},
		{"IOSurfaceAllocSize", alloc},
		{"IOSurfacePixelSizeCastingAllowed", 0},
	};
	for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
		OSData *data = OSData::withBytes(&words[i].value, sizeof(uint32_t));
		if (data) {
			props->setObject(words[i].key, data);
			data->release();
		}
	}
	typedef OSObject *(*create_fn)(void *, void *, OSDictionary *, uint32_t, OSObject **);
	create_fn fn = (create_fn)(*(uintptr_t **)gSurfaceRoot)[141];
	OSObject *out = nullptr;
	printf("DISPLAY: create_surface %ux%u stride %u via vt[141]=%p task=%p\n", width, height, stride,
	    (void *)fn, (void *)current_task());
	OSObject *result = fn(gSurfaceRoot, current_task(), props, 0, &out);
	printf("DISPLAY: create_surface result=%p out=%p\n", (void *)result, (void *)out);
	props->release();
	if (out && out->getMetaClass()) {
		printf("DISPLAY: create_surface class=%s size=0x%x\n", out->getMetaClass()->getClassName(),
		    (unsigned)out->getMetaClass()->getClassSize());
	}
	IOUserClient *client = OSDynamicCast(IOUserClient, out);
	if (client) {
		/* Selector 0 creates a surface: scalarInput carries the parent id, structureInput the
		 * serialized property dictionary, structureOutput the lock result (0xa68 bytes) that tells
		 * the caller where the pixels and the shared state page are. */
		/* Hand-written XML plist: the user client unserializes either XML or binary. */
		char blob[768];
		int blob_len = snprintf(blob, sizeof(blob),
		    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
		    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
		    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
		    "<plist version=\"1.0\"><dict>"
		    "<key>IOSurfaceWidth</key><integer>%u</integer>"
		    "<key>IOSurfaceHeight</key><integer>%u</integer>"
		    "<key>IOSurfaceBytesPerRow</key><integer>%u</integer>"
		    "<key>IOSurfacePixelFormat</key><integer>%u</integer>"
		    "<key>IOSurfaceAllocSize</key><integer>%u</integer>"
		    "</dict></plist>",
		    width, height, stride, pixel_format, alloc);
		bool serialized = blob_len > 0 && blob_len < (int)sizeof(blob);
		uint64_t parent_id = 0;
		uint8_t lock_result[0xa68] = {};
		IOExternalMethodArguments args = {};
		args.selector = 0;
		args.scalarInput = &parent_id;
		args.scalarInputCount = 1;
		args.structureInput = serialized ? blob : nullptr;
		args.structureInputSize = serialized ? (uint32_t)blob_len : 0;
		args.structureOutput = lock_result;
		args.structureOutputSize = sizeof(lock_result);
		IOUserClient2022 *client2022 = (IOUserClient2022 *)client;
		IOReturn rc = client2022->externalMethod(0, (IOExternalMethodArgumentsOpaque *)&args);
		uint32_t *words = (uint32_t *)lock_result;
		printf("DISPLAY: create sel0 rc=0x%x serialized=%d len=%u out=%08x %08x %08x %08x %08x %08x\n",
		    (unsigned)rc, serialized ? 1 : 0, (unsigned)blob_len,
		    words[0], words[1], words[2], words[3], words[4], words[5]);
		if (rc == kIOReturnSuccess) {
			for (unsigned i = 0; i < 16; i++) {
				printf("DISPLAY:   lock[%02u]=0x%llx\n", i, (unsigned long long)((uint64_t *)lock_result)[i]);
			}
		}

	}
	return out;
}

/* Bring-up: an abstract slot the owned backend does not implement yet.  */
/* Clear the whole panel, not just the 480-row window the kernel is handed: the loader draws on the same
 * memory at panel height, so the tail rows keep its old image otherwise. */
static void __attribute__((unused))
clear_panel(PE_Video *video)
{
	if (!video || !video->v_baseAddr || !video->v_rowBytes) {
		return;
	}
	uint64_t base = (uint64_t)video->v_baseAddr & ~0xFFFULL;
	IOByteCount bytes = (IOByteCount)((uint64_t)video->v_rowBytes * 2400);
	IOMemoryDescriptor *descriptor = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress)base,
	    bytes, kIODirectionOutIn);
	IOMemoryMap *map = descriptor ? descriptor->map(kIOMapDefaultCache | kIOMapAnywhere) : nullptr;
	if (!map) {
		printf("DISPLAY: panel clear mapping failed (%llu bytes at 0x%llx)\n",
		    (unsigned long long)bytes, (unsigned long long)base);
		if (descriptor) {
			descriptor->release();
		}
		return;
	}
	/* bzero uses dc zva, which is illegal on the device mapping this framebuffer gets; write the zeros
	 * with explicit volatile stores instead. */
	{
		volatile uint64_t *pixels = (volatile uint64_t *)map->getVirtualAddress();
		for (IOByteCount i = 0; i < bytes / 8; i++) {
			pixels[i] = 0;
		}
	}
	printf("DISPLAY: cleared panel %llu bytes at 0x%llx\n", (unsigned long long)bytes,
	    (unsigned long long)base);
	map->release();
	descriptor->release();
}

static void
publish_display(void)
{
	IOService *display = pendingDisplay;
	if (!display) {
		printf("DISPLAY: nothing pending to publish\n");
		return;
	}
	gDisplay = display;
	pendingDisplay = nullptr;
	OSReturn rc = OSKextLoadKextWithIdentifier("com.apple.iokit.IOSurface");
	printf("DISPLAY: IOSurface kext rc=0x%x\n", (unsigned)rc);
	OSDictionary *matching = OSDictionary::withCapacity(1);
	const OSObject *names[2] = {OSString::withCString("IOCoreSurfaceRoot"), OSString::withCString("IOSurfaceRoot")};
	OSArray *list = (names[0] && names[1]) ? OSArray::withObjects(names, 2) : nullptr;
	if (matching && list) {
		matching->setObject(gIONameMatchKey, list);
	}
	IOService *surface = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
	printf("DISPLAY: IOSurface root found=%d\n", surface ? 1 : 0);
	if (surface && !gSurfaceRoot) {
		surface->retain();
		gSurfaceRoot = surface;
	}
	if (surface) {
		const uintptr_t *vtable = *(const uintptr_t **)surface;
		const OSMetaClass *meta = surface->getMetaClass();
		printf("DISPLAY: IOSurfaceRoot %p class=%s size=0x%x vtable=%p\n", surface,
		    meta->getClassName(), (unsigned)meta->getClassSize(), (void *)vtable);
		for (unsigned i = 0; i < 8; i++) {
			printf("DISPLAY:   vt[%u]=0x%llx\n", i, (unsigned long long)vtable[i]);
		}
	}
	if (surface) surface->release();
	if (list) list->release();
	for (int i = 0; i < 2; ++i) if (names[i]) ((OSObject *)names[i])->release();
	if (matching) matching->release();

	PE_Video video = {};
	if (PE_current_console(&video) || !video.v_baseAddr || !video.v_depth) {
		printf("DISPLAY: no GOP console at publication\n");
		return;
	}
	IOService *parent = IOService::getResourceService();
	/* The family's own start wants an AppleARMIODevice as provider ("provider was not
	 * AppleARMIODevice"); use one when the prelinked AppleARMPlatform offers it, and keep the plain
	 * IOService as the fallback. */
	IOService *nub = OSDynamicCast(IOService, OSMetaClass::allocClassWithName("AppleARMIODevice"));
	printf("DISPLAY: nub class %s\n", nub ? "AppleARMIODevice" : "IOService (fallback)");
	if (!nub) {
		nub = new IOService;
	}
	/* Exactly stride*height: the component's startHardware() accepts the device memory only when the
	 * mapping length equals v_rowBytes * v_height, so any slack makes start() fail. */
	IOByteCount nubBytes = (IOByteCount)((uint64_t)video.v_rowBytes * video.v_height);
	IODeviceMemory *memory = IODeviceMemory::withRange(video.v_baseAddr, nubBytes);
	OSArray *ranges = OSArray::withCapacity(1);
	if (nub && memory && ranges && parent && nub->init()) {
		ranges->setObject(memory);
		nub->setDeviceMemory(ranges);
		nub->setName("NativeGOPFramebuffer");
		bool attached = nub->attach(parent);
		bool started = attached ? nub->start(parent) : false;
		printf("DISPLAY: nub %p attach=%d start=%d mem=0x%llx+0x%llx\n", nub, attached, started,
		    (unsigned long long)video.v_baseAddr,
		    (unsigned long long)nubBytes);
		IORegistryEntry *dtParent = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
		const char type[] = "display-subsystem";
		OSData *data = OSData::withBytes(type, sizeof(type));
		if (dtParent && data && nub->setProperty("device_type", data)) {
			nub->setName("gop-display", gIODTPlane);
			printf("DISPLAY: dt attach=%d\n", nub->attachToParent(dtParent, gIODTPlane) ? 1 : 0);
		}
		if (data) data->release();
		if (dtParent) dtParent->release();
		/* The family's create_default_fb_surface asks IOSurfaceRoot for "PurpleGfxMem", which does not
		 * exist on this board; seed its own region cache at +0x17c0 with system memory and set the
		 * enable byte at +0x1780 so slot 299 builds the temporary surface instead (uke's recipe). */
		IOBufferMemoryDescriptor *surface_region = IOBufferMemoryDescriptor::withCapacity(0x2000000,
		    kIODirectionOutIn);
		if (surface_region) {
			uint32_t format = 0x42475241 /* 'BGRA' */, planes = 1, bpp = 4;
			memset((unsigned char *)display + 0x1700, 0, 0x70);
			memcpy((unsigned char *)display + 0x1700, &format, sizeof(format));
			memcpy((unsigned char *)display + 0x1704, &planes, sizeof(planes));
			memcpy((unsigned char *)display + 0x1708, &bpp, sizeof(bpp));
			((unsigned char *)display)[0x1748] = 0;
			surface_region->retain();
			memcpy((unsigned char *)display + 0x17c0, &surface_region, sizeof(surface_region));
			((unsigned char *)display)[0x1780] |= 1;
			printf("DISPLAY: seeded surface region %p bytes=%llu\n", surface_region,
			    (unsigned long long)surface_region->getLength());
		} else {
			printf("DISPLAY: surface region allocation failed\n");
		}
		bool dispAttached = display->attach(nub);
		bool dispStarted = dispAttached ? display->start(nub) : false;
		printf("DISPLAY: display attach=%d start=%d\n", dispAttached, dispStarted);
		if (dispStarted) {
			scan_display_fields(display);
			/* Adopt the temporary surface the family created at +0x1620 as the default one at +0x1618,
			 * the way the uke port does; retain the region and descriptor so the compositor's mapping
			 * never points at freed pages when the display's stop clears the field. */
			void *temp = nullptr, *def = nullptr;
			memcpy(&temp, (unsigned char *)display + 0x1620, sizeof(temp));
			memcpy(&def, (unsigned char *)display + 0x1618, sizeof(def));
			printf("DISPLAY: surfaces default=%p temp=%p\n", def, temp);
			unsigned adopt = 0;
			PE_parse_boot_argn("offline-legacy-adopt", &adopt, sizeof(adopt));
			if (adopt == 1 && !def && temp) {
				void *surface_region = nullptr, *surface_descriptor = nullptr;
				uint32_t sid = 0;
				memcpy(&sid, (unsigned char *)temp + 0x10, sizeof(sid));
				memcpy(&surface_region, (unsigned char *)temp + 0x28, sizeof(surface_region));
				memcpy(&surface_descriptor, (unsigned char *)temp + 0x30, sizeof(surface_descriptor));
				if (surface_region) {
					((OSObject *)surface_region)->retain();
				}
				if (surface_descriptor) {
					((OSObject *)surface_descriptor)->retain();
				}
				((OSObject *)temp)->retain();
				memcpy((unsigned char *)display + 0x1618, &temp, sizeof(temp));
				/* Clear the family's temporary slot as well: surface_complete frees whatever it finds
				 * there, and that free is what ended presentation after the first few swaps. */
				{
					void *none = nullptr;
					memcpy((unsigned char *)display + 0x1620, &none, sizeof(none));
				}
				printf("DISPLAY: clear the family's temporary slot\n");
				printf("DISPLAY: adopted default surface %p sid=%u region=%p descriptor=%p\n",
				    temp, (unsigned)sid, surface_region, surface_descriptor);
			}
		}
	}
	if (memory) memory->release();
	if (ranges) ranges->release();
}

static void
publish_thread(void *, wait_result_t)
{
	IOSleep(2000);
	publish_display();
}

/* Board backlight: the sibling IN2020 port publishes AppleARMBacklight-conforming brightness properties over
 * the DSI0 transport (In2020BacklightService.h).  Started once, after the display family is loaded. */
static bool gBacklightStarted __attribute__((unused));
/* Backlight: In2020BacklightService.h is kept in the package; wiring waits for a kext-clean C++ translation unit. */

extern "C" void
native_legacy_display_late_probe(void)
{
	/* offline-legacy-start runs at bsd_autoconf, before the family is loaded. The load itself is
	 * done by offline-mobile-graphics-load (offline-component-load-probes.patch), which then calls
	 * native_legacy_display_probe(); this entry only records that ordering. */
	printf("DISPLAY: late probe (load handled by offline-mobile-graphics-load)\n");
	print_cpu_map_once();
}


/* --- diagnostics the component asks for ---------------------------------- */

extern "C" bool native_legacy_prepare_scanout(void) { return true; }

/* The component's surface accessors are bound to the original IOSurface functions; call the same
 * symbols here so the log shows what the copy will see. */
extern "C" uint32_t native_legacy_surface_width(void *);
extern "C" uint32_t native_legacy_surface_allocation(void *);
extern "C" uint32_t native_legacy_surface_height(void *);
extern "C" void *native_legacy_surface_descriptor(void *);
extern "C" uint64_t native_legacy_surface_stride(void *, uint32_t);
static bool
publish_word(IORegistryEntry *entry, const char *name, uint32_t value)
{
	OSData *existing = OSDynamicCast(OSData, entry->getProperty(name));
	if (existing) {
		uint32_t current = 0;
		if (existing->getLength() == sizeof(current)) {
			memcpy(&current, existing->getBytesNoCopy(), sizeof(current));
		}
		return current == value;
	}
	OSData *data = OSData::withBytes(&value, sizeof(value));
	bool ok = data && entry->setProperty(name, data);
	if (data) {
		data->release();
	}
	return ok;
}

/* The component calls this when it maps the provider's framebuffer.  The firmware GOP mode and the
 * UI scale become the published display geometry, as the reference board does at phase 4. */
extern "C" bool
native_legacy_gop_mapping_observed(IOService *object, IOMemoryMap *mapping, uint32_t width, uint32_t height)
{
	unsigned scale = 0;
	PE_parse_boot_argn("offline-legacy-unit-scale", &scale, sizeof(scale));
	if (!scale) {
		return true;
	}
	unsigned phase = 0;
	PE_parse_boot_argn("offline-legacy-phase", &phase, sizeof(phase));
	PE_Video video = {};
	if (scale < 1 || scale > 3 || phase != 4 || PE_current_console(&video) ||
	    video.v_width != width || video.v_height != height || video.v_rotate ||
	    !mapping || mapping->getPhysicalAddress() != video.v_baseAddr) {
		printf("DISPLAY: mapping check refused scale=%u phase=%u video=%lux%lu mapping=%p\n",
		    scale, phase, (unsigned long)video.v_width, (unsigned long)video.v_height, (void *)mapping);
		return false;
	}
	IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
	if (!chosen) {
		printf("DISPLAY: no /chosen node\n");
		return false;
	}
	bool ok = publish_word(chosen, "display-scale", scale) &&
	    publish_word(chosen, "display-rotation", video.v_rotate);
	printf("DISPLAY: chosen display-scale=%u rotation=%lu ok=%d\n", scale,
	    (unsigned long)video.v_rotate, ok ? 1 : 0);
	chosen->release();
	return ok;
}
/* Report the frame as taken by the presentation path: the component then retires the swap early,
 * inside the family's work loop, which is the only place completeCurrentSwap() wakes the waiter.
 * The probe copies the frame itself in the swap-validation hook. */
/* This board has no GPU that can write the scanout memory, so the offer must be declined:
 * returning true takes the driver GPU hand-off branch, which retires the swap without ever
 * copying the client frame to the framebuffer - the screen stays on the last console frame
 * while every userland swap is silently dropped. */
extern "C" bool native_legacy_gpu_offer(void *, IOMemoryDescriptor *, uint64_t, uint32_t, uint32_t, uint32_t) { return false; }
extern "C" void native_legacy_gpu_fallback_observed(int) {}
extern "C" void native_legacy_pm_observed(unsigned, IOReturn, unsigned long) {}
extern "C" void native_legacy_registration_observed(IOService *) {}
extern "C" void native_legacy_retirement_observed(IOService *) {}
extern "C" void native_legacy_client_enter(task_t, uint32_t) {}
extern "C" void native_legacy_client_observed(IOReturn, IOUserClient *) {}
extern "C" void native_legacy_swap_stage(unsigned stage)
{
	static unsigned n;
	if (n++ < 8) {
		printf("DISPLAY: swap stage %u\n", stage);
	}
}
/* Present one surface on the firmware framebuffer: map the surface's pixel descriptor and copy the rows
 * into the GOP memory, the way the sibling port does on the compositor's own thread. */
static void
present_surface(void *surface)
{
	static unsigned presents;
	if (!surface) {
		return;
	}
	uint32_t width = 0, height = 0, stride = 0, format = 0;
	uint64_t allocation = 0;
	void *descriptor = nullptr;
	/* Measured layout: +0x60 holds the height (480 for this panel's compositor surface), +0x68 the
	 * stride (4352 = 1088 * 4) and +0x90 the allocation (4352 * 480).  The width is derived from the
	 * stride; the words around the block are logged so the exact field can be pinned next time. */
	uint32_t words[8] = {};
	for (unsigned i = 0; i < 8; i++) {
		memcpy(&words[i], (unsigned char *)surface + 0x50 + 4 * i, 4);
	}
	memcpy(&width, (unsigned char *)surface + 0x58, 4);
	memcpy(&height, (unsigned char *)surface + 0x60, 4);
	memcpy(&stride, (unsigned char *)surface + 0x68, 4);
	printf("DISPLAY: surface words +50 %08x %08x %08x %08x %08x %08x %08x %08x\n",
	    words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
	if (!width && stride >= 4) {
		width = stride / 4;
	}
	memcpy(&format, (unsigned char *)surface + 0x80, 4);
	memcpy(&allocation, (unsigned char *)surface + 0x90, 8);
	memcpy(&descriptor, (unsigned char *)surface + 0x30, 8);
	PE_Video video = {};
	IOReturn console_rc = PE_current_console(&video);
	printf("DISPLAY: present surface=%p %ux%u stride=%u fmt=%08x alloc=%llu desc=%p console=%d base=0x%llx %lux%lu rb=%lu\n",
	    surface, width, height, stride, format, (unsigned long long)allocation, descriptor,
	    (int)console_rc, (unsigned long long)video.v_baseAddr, (unsigned long)video.v_width,
	    (unsigned long)video.v_height, (unsigned long)video.v_rowBytes);
	if (console_rc || !video.v_baseAddr || !width || !height || !stride || !descriptor) {
		return;
	}
	IOMemoryDescriptor *source = (IOMemoryDescriptor *)descriptor;
	IOMemoryMap *source_map = source->map(kIOMapDefaultCache | kIOMapAnywhere);
	if (!source_map) {
		printf("DISPLAY: present: surface map failed\n");
		return;
	}
	/* v_baseAddr carries a flag in its low bits (0x9c000001 for this panel); the framebuffer itself
	 * starts at the page base, and an unaligned destination aborts the copy. */
	uint64_t fb_phys = (uint64_t)video.v_baseAddr & ~0xFFFULL;
	uint64_t fb_off = 0;
	IOByteCount fb_bytes = (IOByteCount)((uint64_t)video.v_rowBytes * video.v_height);
	IOMemoryDescriptor *fb_desc = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress)fb_phys,
	    fb_bytes, kIODirectionOutIn);
	IOMemoryMap *fb_map = fb_desc ? fb_desc->map(kIOMapDefaultCache | kIOMapAnywhere) : nullptr;
	if (!fb_map) {
		printf("DISPLAY: present: framebuffer map failed\n");
		source_map->release();
		if (fb_desc) {
			fb_desc->release();
		}
		return;
	}
	unsigned char *src = (unsigned char *)source_map->getVirtualAddress();
	unsigned char *dst = (unsigned char *)fb_map->getVirtualAddress() + fb_off;
	uint32_t rows = height < video.v_height ? height : (uint32_t)video.v_height;
	uint32_t cols = stride < video.v_rowBytes ? stride : (uint32_t)video.v_rowBytes;
	for (uint32_t row = 0; row < rows; row++) {
		memcpy(dst + (uint64_t)row * video.v_rowBytes, src + (uint64_t)row * stride, cols);
	}
	if (presents++ < 3) {
		printf("DISPLAY: present copied %u rows x %u bytes (frame %u)\n", rows, cols, presents);
	}
	fb_map->release();
	fb_desc->release();
	source_map->release();
}

extern "C" void native_legacy_swap_validation_observed(bool accepted, unsigned reason, const void *request,
    const void *surface)
{
	static unsigned logged;
	if (logged < 6) {
		logged++;
		uint64_t surfaces[4] = {};
		if (request) {
			memcpy(surfaces, (const unsigned char *)request + 2624, sizeof(surfaces));
		}
		uint32_t sw = 0, sh = 0, sstride = 0, sfmt = 0;
		uint64_t salloc = 0;
		void *sdesc = nullptr, *sregion = nullptr;
		if (surface) {
			memcpy(&sw, (unsigned char *)surface + 0x60, 4);
			memcpy(&sh, (unsigned char *)surface + 0x64, 4);
			memcpy(&sstride, (unsigned char *)surface + 0x68, 4);
			memcpy(&sfmt, (unsigned char *)surface + 0x80, 4);
			memcpy(&salloc, (unsigned char *)surface + 0x90, 8);
			memcpy(&sdesc, (unsigned char *)surface + 0x30, 8);
			memcpy(&sregion, (unsigned char *)surface + 0x28, 8);
		}
		printf("DISPLAY: swap accepted=%d reason=%u request=%p surface=%p req_surfaces=%llx,%llx,%llx,%llx\n",
		    accepted ? 1 : 0, reason, request, surface,
		    (unsigned long long)surfaces[0], (unsigned long long)surfaces[1],
		    (unsigned long long)surfaces[2], (unsigned long long)surfaces[3]);
		printf("DISPLAY: surface geom %ux%u stride=%u fmt=%08x alloc=%llu desc=%p region=%p\n",
		    sw, sh, sstride, sfmt, (unsigned long long)salloc, sdesc, sregion);
		if (accepted && surface) {
			{
				void *desc = native_legacy_surface_descriptor((void *)surface);
				uint64_t descLen = desc ? ((IOMemoryDescriptor *)desc)->getLength() : 0;
				uint64_t mapLen = 0;
				if (desc) {
					IOMemoryMap *m = ((IOMemoryDescriptor *)desc)->map(kIOMapAnywhere | kIOMapReadOnly);
					if (m) { mapLen = m->getLength(); m->release(); }
				}
				printf("DISPLAY: accessor width=%u height=%u alloc=%u descLen=%llu mapLen=%llu\n",
				    (unsigned)native_legacy_surface_width((void *)surface),
				    (unsigned)native_legacy_surface_height((void *)surface),
				    (unsigned)native_legacy_surface_allocation((void *)surface),
				    (unsigned long long)descLen, (unsigned long long)mapLen);
			}
			present_surface((void *)surface);
			/* The family completes a swap by calling its own completeCurrentSwap (vtable slot 312)
			 * once the frame is out; do the same now that this probe has copied the frame. */
			complete_swap_later();
		}
	}
}

/* Diagnostic hooks the display component calls but this board does not use yet. */
extern "C" void native_legacy_copy_timing_observed(uint64_t ticks, uint64_t bytes)
{
	static unsigned n;
	if (n++ < 4) {
		printf("DISPLAY: copy timing ticks=%llu bytes=%llu\n", (unsigned long long)ticks, (unsigned long long)bytes);
	}
}
extern "C" void native_legacy_frame_colors_observed(const uint32_t *, const uint64_t *, uint64_t) {}
extern "C" void native_legacy_frame_observed(unsigned, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {}
extern "C" void native_legacy_surface_rejected(uint64_t, uint64_t, uint32_t, uint32_t, uint64_t, uint64_t, uint64_t) {}
extern "C" void native_legacy_transfer_observed(IOReturn rc, const void *source, uint64_t bytes)
{
	static unsigned n;
	if (n++ < 6) {
		printf("DISPLAY: transfer rc=0x%x source=%p bytes=%llu\n", (unsigned)rc, source, (unsigned long long)bytes);
	}
}
extern "C" void native_legacy_vsync_observed(unsigned a, unsigned b)
{
	static unsigned n;
	if (n++ < 4) {
		printf("DISPLAY: vsync %u %u\n", a, b);
	}
}
/* Abstract-slot guard: the component declares it noreturn, so it must not return. */
/* Bring-up: slot 345 is reached when the family abandons a swap after its wait times out.  Panicking here
 * restarts the machine right after the frame was copied to the framebuffer, so log it and return instead. */
extern "C" __attribute__((noinline)) void
native_legacy_backend_guard(unsigned slot, void *object, void *caller)
{
	static unsigned seen;
	if (seen++ < 8) {
		printf("DISPLAY: abstract slot %u (object=%p caller=%p); returning\n", slot, object, caller);
	}
}

extern "C" int native_gpu_trace_enabled(void) { return 0; }
extern "C" void native_gpu_trace_record(uint32_t, uint32_t, const uint64_t *, uint32_t) {}
/* Pixel capture is part of the merged display's GPU trace; tracing is disabled on this board
 * (native_gpu_trace_enabled returns 0), so the sink is an inert stub that satisfies the link. */
extern "C" int native_gpu_pixel_put(const struct NativeGpuPixelPiece *, uint32_t) { return 0; }
/* docs/zh/guide/skip-setup.md: a fresh Data seed boots into Setup, and Setup is the state in which
 * launchd never starts SpringBoard.  The documented tool does two things on macOS: it writes these
 * preferences and it creates the /private/var/containers skeleton lsd needs before its database can
 * be seeded.  The port needs both without a Mac, so both are done here after the Data mount. */
#include <sys/attr.h>
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>

static int
native_write_preference(const char *path, const char *body, size_t length)
{
	vfs_context_t ctx = vfs_context_current();
	vnode_t vp = NULLVP;
	int error = vnode_open(path, FWRITE | O_CREAT | O_TRUNC, 0644, 0, &vp, ctx);
	if (error) {
		return error;
	}
	error = vn_rdwr(UIO_WRITE, vp, (caddr_t)(uintptr_t)body, (int)length, 0, UIO_SYSSPACE, IO_NODELOCKED,
	    vfs_context_ucred(ctx), NULL, current_proc());
	if (!error) {
		(void)VNOP_FSYNC(vp, MNT_WAIT, ctx);
	}
	(void)vnode_close(vp, FWRITE, ctx);
	return error;
}

/* The pattern XNU itself uses for a kernel-made directory (bsd/vfs/vfs_xattr.c): a componentname
 * naming the entry and a vnode_attr whose type is VDIR, handed to the parent vnode's VNOP_MKDIR. */
static int
native_make_dir(const char *parent_path, const char *leaf, vfs_context_t ctx)
{
	struct componentname cn;
	struct vnode_attr va;
	vnode_t dvp = NULLVP, sdvp = NULLVP;
	int error = vnode_lookup(parent_path, 0, &dvp, ctx);
	if (error) {
		return error;
	}
	bzero(&cn, sizeof(cn));
	cn.cn_nameiop = LOOKUP;
	cn.cn_flags = ISLASTCN;
	cn.cn_pnbuf = (char *)(uintptr_t)leaf;
	cn.cn_pnlen = (int)strlen(leaf);
	cn.cn_nameptr = cn.cn_pnbuf;
	cn.cn_namelen = (int)strlen(leaf);
	VATTR_INIT(&va);
	VATTR_SET(&va, va_uid, 0);
	VATTR_SET(&va, va_gid, 0);
	VATTR_SET(&va, va_mode, 0755);
	VATTR_SET(&va, va_type, VDIR);
	error = VNOP_MKDIR(dvp, &sdvp, &cn, &va, ctx);
	if (error == 0) {
		vnode_put(sdvp);
	}
	vnode_put(dvp);
	return error;
}

extern "C" int
native_owner_setup_skip(void)
{
	/* setup-skip=0 writes the setup state back to "not done" so the Hello screen can be reached; the
	 * persistent store otherwise keeps whatever an earlier boot wrote. */
	unsigned skip = 1;
	PE_parse_boot_argn("setup-skip", &skip, sizeof(skip));
	static const char purple[] =
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<plist version=\"1.0\"><dict>"
	    "<key>SetupDone</key><true/><key>SetupFinishedAllSteps</key><true/>"
	    "<key>SetupVersion</key><integer>11</integer><key>daemonizeNeedsToRun</key><false/>"
	    "</dict></plist>\n";
	static const char springboard[] =
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<plist version=\"1.0\"><dict>"
	    "<key>BKNoWatchdogs</key><true/><key>MCForceShowCloudConfigurationUI</key><false/>"
	    "<key>SBDisableAutoDim</key><true/><key>SBDontDimOrLockOnAC</key><true/><key>SBDisableIdleSleep</key><true/>"
	    "<key>FBLaunchWatchdogExceptions</key><dict><key>com.apple.purplebuddy</key><integer>100</integer></dict>"
	    "</dict></plist>\n";
	static const char migration[] =
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<plist version=\"1.0\"><dict><key>LastSystemVersion</key><string>21H461</string></dict></plist>\n";
	static const struct { const char *parent, *leaf; } dirs[] = {
	    { "/private/var", "containers" },
	    { "/private/var/containers", "Bundle" },
	    { "/private/var/containers", "Data" },
	    { "/private/var/containers/Data", "System" },
	    { "/private/var/containers", "Shared" },
	    { "/private/var/containers/Shared", "SystemGroup" },
	    { "/private/var/containers", "Temp" },
	    { "/private/var/db", "launchservices" },
	};
	vfs_context_t ctx = vfs_context_current();
	unsigned made = 0, present = 0, failed = 0;
	for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		int e = native_make_dir(dirs[i].parent, dirs[i].leaf, ctx);
		if (e == 0) {
			made++;
		} else if (e == EEXIST) {
			present++;
		} else {
			failed++;
		}
	}
	/* The job the system ships starts on demand and never does.  A second copy of the same label
	 * on the writable Data volume, with RunAtLoad, is the least invasive way to force it. */
	{
		native_make_dir("/", "Library", ctx);
		native_make_dir("/Library", "LaunchDaemons", ctx);
		static const char chronod_job[] =
		    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
		    "<plist version=\"1.0\"><dict>"
		    "<key>Label</key><string>com.apple.chronod</string>"
		    "<key>ProgramArguments</key><array><string>/System/Library/PrivateFrameworks/ChronoCore.framework/Support/chronod</string></array>"
		    "<key>MachServices</key><dict><key>com.apple.chronoservices</key><true/>"
		    "<key>com.apple.chrono.widgetcenterconnection</key><true/></dict>"
		    "<key>UserName</key><string>mobile</string>"
		    "<key>RunAtLoad</key><true/>"
		    "<key>KeepAlive</key><dict><key>Crashed</key><true/></dict>"
		    "<key>POSIXSpawnType</key><string>Interactive</string>"
		    "</dict></plist>\n";
		int job_error = native_write_preference(
		    "/Library/LaunchDaemons/com.apple.chronod.plist", chronod_job, sizeof(chronod_job) - 1);
		printf("DISPLAY: chronod job on the Data volume: %d\n", job_error);
	}
	/* SpringBoard blocks in a synchronous XPC to com.apple.chronoservices and the on-demand job
	 * com.apple.chronod (MachServices = com.apple.chronoservices, RunAtLoad = false) never starts.
	 * Force it on at boot through launchd's overrides database. */
	static const char overrides[] =
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<plist version=\"1.0\"><dict>"
	    "<key>com.apple.chronod</key><dict>"
	    "<key>Disabled</key><false/>"
	    "<key>OnDemand</key><false/>"
	    "</dict>"
	    "</dict></plist>\n";
	int overrides_error = native_write_preference(
	    "/private/var/db/com.apple.xpc.launchd/overrides.plist", overrides, sizeof(overrides) - 1);
	printf("DISPLAY: chronod override written: %d\n", overrides_error);
	static const char purple_off[] =
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<plist version=\"1.0\"><dict>"
	    "<key>SetupDone</key><false/><key>SetupFinishedAllSteps</key><false/>"
	    "<key>SetupVersion</key><integer>11</integer><key>daemonizeNeedsToRun</key><true/>"
	    "</dict></plist>\n";
	int purple_error = native_write_preference("/private/var/mobile/Library/Preferences/com.apple.purplebuddy.plist",
	    (skip == 0) ? purple_off : purple, (skip == 0) ? sizeof(purple_off) - 1 : sizeof(purple) - 1);
	int spring_error = native_write_preference("/private/var/mobile/Library/Preferences/com.apple.springboard.plist",
	    springboard, sizeof(springboard) - 1);
	int migration_error = native_write_preference("/private/var/mobile/Library/Preferences/com.apple.migration.plist",
	    migration, sizeof(migration) - 1);
	printf("DISPLAY: setup-skip containers made=%u present=%u failed=%u prefs=%d/%d/%d\n",
	    made, present, failed, purple_error, spring_error, migration_error);
	return (failed || purple_error || spring_error || migration_error) ? 1 : 0;
}

/* Checklist step 4: supply the default surface through the original IOSurfaceRoot factory.  Its creation
 * entry point is an IOUserClient external method, but both calls needed to reach it - IOService::newUserClient
 * and IOUserClient::externalMethod - are virtual, so the kernel can dispatch them without linking any kext
 * symbol.  First question: does IOSurfaceRoot hand a user client to the kernel task at all? */
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>

extern "C" int
native_probe_iosurface_root(void)
{
	IOService *root = IOService::waitForMatchingService(IOService::serviceMatching("IOSurfaceRoot"), 1000000000ULL);
	if (!root) {
		printf("DISPLAY: IOSurfaceRoot not found\n");
		return -1;
	}
	IOUserClient *client = nullptr;
	IOReturn status = root->newUserClient(current_task(), nullptr, 0, nullptr, &client);
	printf("DISPLAY: IOSurfaceRoot %p newUserClient=%d client=%p\n", root, (int)status, client);
	if (client) {
		client->release();
	}
	root->release();
	return (int)status;
}

/* Second half of the factory path: ask the client to create the surface with the GOP geometry.  The
 * descriptor is the serialized property dictionary IOSurfaceRootUserClient takes in structureInput; the
 * reply carries the surface number.  A wrong descriptor shows up as a specific error, which is itself the
 * answer we need from the first attempt. */
extern "C" int
native_create_iosurface(uint32_t width, uint32_t height, uint32_t stride, uint32_t alloc)
{
	static const char plist_head[] =
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
	    "<plist version=\"1.0\"><dict>"
	    "<key>IOSurfaceWidth</key><integer>";
	static const char plist_mid1[] = "</integer><key>IOSurfaceHeight</key><integer>";
	static const char plist_mid2[] = "</integer><key>IOSurfaceBytesPerElement</key><integer>4</integer>"
	    "<key>IOSurfaceBytesPerRow</key><integer>";
	static const char plist_mid3[] = "</integer><key>IOSurfaceAllocSize</key><integer>";
	static const char plist_tail[] = "</integer><key>IOSurfacePixelFormat</key><integer>1111970369</integer>"
	    "<key>IOSurfaceName</key><string>PurpleGfxMem</string>"
	    "<key>IOSurfaceMemoryRegion</key><string>PurpleGfxMem</string>"
	    "</dict></plist>";
	char plist[768];
	int len = snprintf(plist, sizeof(plist), "%s%u%s%u%s%u%s%u%s",
	    plist_head, (unsigned)width, plist_mid1, (unsigned)height, plist_mid2,
	    (unsigned)stride, plist_mid3, (unsigned)alloc, plist_tail);
	if (len <= 0 || (size_t)len >= sizeof(plist)) {
		printf("DISPLAY: iosurface descriptor too long\n");
		return -1;
	}

	IOService *root = IOService::waitForMatchingService(IOService::serviceMatching("IOSurfaceRoot"), 1000000000ULL);
	if (!root) {
		printf("DISPLAY: IOSurfaceRoot not found\n");
		return -1;
	}
	IOUserClient *client = nullptr;
	IOReturn status = root->newUserClient(current_task(), nullptr, 0, nullptr, &client);
	if (status || !client) {
		printf("DISPLAY: newUserClient failed %d\n", (int)status);
		root->release();
		return (int)status;
	}
	char reply[512];
	IOExternalMethodArguments args = {};
	args.version = kIOExternalMethodArgumentsCurrentVersion;
	args.selector = 0;	/* kIOSurfaceRootUserClientCreateSurface */
	args.structureInput = plist;
	args.structureInputSize = (IOByteCount)len;
	args.structureOutput = reply;
	args.structureOutputSize = sizeof(reply);
	/* IOSurfaceRootUserClient is an IOUserClient2022, which answers on its own two-argument virtual
	 * instead of the legacy externalMethod(selector, args, dispatch, ...) entry. */
#if defined(KERNEL_PRIVATE) && KERNEL_PRIVATE
	IOReturn rc = kIOReturnError;
	for (uint32_t sel = 0; sel < 4; sel++) {
		IOExternalMethodArguments probe = {};
		probe.version = kIOExternalMethodArgumentsCurrentVersion;
		probe.selector = sel;
		probe.structureInput = plist;
		probe.structureInputSize = (IOByteCount)len;
		probe.structureOutput = reply;
		probe.structureOutputSize = sizeof(reply);
		IOReturn r = ((IOUserClient2022 *)client)->externalMethod(sel,
		    (IOExternalMethodArgumentsOpaque *)(void *)&probe);
		printf("DISPLAY: iosurf selector %u rc=0x%x out=%llu\n", (unsigned)sel, (unsigned)r,
		    (unsigned long long)probe.structureOutputSize);
		if (sel == 0) {
			rc = r;
		}
	}
#else
	IOReturn rc = client->externalMethod(0, &args, nullptr, nullptr);
#endif
	printf("DISPLAY: createSurface selector 0 rc=0x%x in=%u out=%llu\n", (unsigned)rc, (unsigned)len,
	    (unsigned long long)args.structureOutputSize);
	if (rc == kIOReturnSuccess && args.structureOutputSize > 0) {
		reply[sizeof(reply) - 1] = 0;
		printf("DISPLAY: createSurface reply: %.160s\n", reply);
	}
	client->release();
	root->release();
	return (int)rc;
}

/* --- A650 GPU archive link stubs ------------------------------------------------------------
 * drivers/Adreno650 (libAdreno650.a) references three symbols that the reference board gets from
 * its own GOP handoff and boot-log export.  This board has neither yet, so provide weak defaults:
 * a real implementation added later wins without touching the archive.  They are only reached
 * once the GPU publish hook runs (see the GPU plan in docs/evidence). */
extern "C" __attribute__((weak)) IOReturn
native_gop_handoff_call(uint32_t op, void *task, const void *input, size_t input_size, void *output,
    size_t output_size)
{
	(void)op; (void)task; (void)input; (void)input_size; (void)output; (void)output_size;
	return kIOReturnUnsupported;
}

extern "C" __attribute__((weak)) void *
native_gop_handoff_memory(void)
{
	return nullptr;
}

extern "C" __attribute__((weak)) int
boot_log_export_put(uint32_t generation, uint32_t total, uint32_t offset, const void *data, uint32_t size)
{
	(void)generation; (void)total; (void)offset; (void)data; (void)size;
	return -1;
}

/* The display probe still calls the empty-accelerator publisher when accel_stub=1.  With the A650
 * archive linked MiniAcceleratorES.o stays out of the display archive, so provide a weak no-op here:
 * a strong definition (the stub rebuilt, or a real accelerator) still wins at link time. */
extern "C" __attribute__((weak)) void
MiniAcceleratorPublishStub(IOService * provider)
{
	(void)provider;
}

/* --- GPU stage: one diagnostic line per boot ------------------------------------------------
 * Nothing calls native_adreno_publish() yet (the board power/GMU glue is still to be ported from
 * the reference port), so this reports the three facts the port needs before that code is written:
 * how many addon files landed, whether the GPU extensions union mounted, and whether the GMU/ZAP
 * firmware images are actually linked in (they are weak defaults until a builder embeds them). */
extern "C" {
extern volatile int native_gpu_addon_files, native_gpu_addon_written, native_gpu_addon_failed;
extern volatile int native_gpu_addon_stage, native_gpu_addon_status;
uint32_t native_adreno_publish_stats(uint32_t *closed);
IOService *native_adreno_published_service();
int gpu_metal_start(void);
}

/* The accelerator publish counters the Adreno driver keeps (NativeAdrenoPublish.cpp): enough to tell whether
 * any process reached the published IOAcceleratorES and which one read MetalPluginName. */
extern "C" {
struct NativeAdrenoDiagView { uint32_t words[56]; };
extern volatile struct NativeAdrenoDiagView native_adreno_metal_diag;
extern char native_adreno_metal_rows[48][96];
extern volatile uint32_t native_adreno_metal_rows_seq;
}
static void
gpu_metal_diag_report(unsigned tick)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("gpu-metal", &enabled, sizeof(enabled)) || enabled != 1) {
        return;
    }
    const unsigned char *b = (const unsigned char *)&native_adreno_metal_diag;
    uint32_t w[56];
    for (int k = 0; k < 56; ++k) {
        memcpy(&w[k], b + k * 4, 4);
    }
    uint32_t closed = 0;
    const uint32_t opened = native_adreno_publish_stats(&closed);
    IOService *svc = native_adreno_published_service();
    const char *opener = (const char *)(b + 220);
    const char *reader = (const char *)(b + 271);
    const char *name_reader = (const char *)(b + 322);
    const char *starter = (const char *)(b + 339);
    printf("GPUDIAG tick=%u pub=%u closed=%u svc=%p opens=%u prop=%u name=%u calls=%u last_sel=%u last_status=%x\n",
        tick, opened, closed, (void *)svc, w[0], w[5], w[8], w[9], w[10], w[11]);
    printf("GPUDIAG opener=[%s|%s|%s] reader=[%s|%s|%s] name_reader=[%s] starter=[%s|%s|%s]\n",
        opener, opener + 17, opener + 34, reader, reader + 17, reader + 34,
        name_reader, starter, starter + 17, starter + 34);
    printf("GPUDIAG ext entries=%u sel=%u st=%x in=%u out=%u tunnel=%u probes=%u mb_maps=%u mb_calls=%u marks=%u inflight=%u forced=%u\n",
        w[12], w[13], w[14], w[15], w[16], w[17], w[18], w[27], w[28], w[29], w[52], w[54]);
    printf("GPUDIAG selbits=%08x %08x\n", w[19], w[20]);
    printf("GPUDIAG report=[%s] report2=[%s]\n", (const char *)(b + 390), (const char *)(b + 454));
    for (unsigned r = 0; r < 6; ++r) {
        const char *row = (const char *)&native_adreno_metal_rows[r];
        if (row[0]) printf("GPUDIAG row%u=[%s]\n", r, row);
    }
}

static void
gpu_metal_report_once(void)
{
	unsigned enabled = 0;
	if (!PE_parse_boot_argn("gpu-metal", &enabled, sizeof(enabled)) || enabled != 1) {
		return;
	}
	uint32_t closed = 0;
	const uint32_t opened = native_adreno_publish_stats(&closed);
	printf("GPU: addon files=%d written=%d failed=%d; extensions stage=%d status=%d; accelerator=%u open=%u service=%p\n",
	    (int)native_gpu_addon_files, (int)native_gpu_addon_written, (int)native_gpu_addon_failed,
	    (int)native_gpu_addon_stage, (int)native_gpu_addon_status,
	    opened, closed,
	    (void *)native_adreno_published_service());
	printf("GPU: bring-up mode 12\n");
	gpu_metal_start();
}
