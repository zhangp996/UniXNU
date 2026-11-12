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
extern "C" char boot_process_rows[10][120];
extern "C" char boot_ui_trace_rows[14][120];

static void
probe_rows_thread(void *, wait_result_t)
{
	static char lastp[10][120], lastu[14][120];
	for (;;) {
		delay(4000000);
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

extern "C" void
native_legacy_display_late_probe(void)
{
	/* offline-legacy-start runs at bsd_autoconf, before the family is loaded. The load itself is
	 * done by offline-mobile-graphics-load (offline-component-load-probes.patch), which then calls
	 * native_legacy_display_probe(); this entry only records that ordering. */
	printf("DISPLAY: late probe (load handled by offline-mobile-graphics-load)\n");
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
extern "C" bool native_legacy_gpu_offer(void *, IOMemoryDescriptor *, uint64_t, uint32_t, uint32_t, uint32_t) { return true; }
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
extern "C" bool native_adreno_fences(uint32_t *, uint32_t *) { return false; }
extern "C" bool native_adreno_fences_detail(uint32_t *, uint32_t *, uint32_t *) { return false; }