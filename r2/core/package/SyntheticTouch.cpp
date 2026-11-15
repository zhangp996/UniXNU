// SPDX-License-Identifier: BSD-2-Clause-Patent
// Synthetic touch source for the offline board: it publishes the same HID multitouch device the
// hardware digitizer would, then reports one tap, so the render server has an input event to react
// to.  The component (NativeHidInput.cpp) reads the native_touch_* seqlock globals; this file owns
// them and the callbacks the component expects from the board.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>

extern "C" IOService *native_hid_input_allocate(void);

// The component reads this seqlock.  Odd = writer updating, even = stable.
volatile uint32_t native_touch_seq, native_touch_count, native_touch_hid_reports;
volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];

static IORegistryEntry *
eventDriver(IORegistryEntry *parent, unsigned depth)
{
	if (!depth) return nullptr;
	OSIterator *children = parent->getChildIterator(gIOServicePlane);
	if (!children) return nullptr;
	IORegistryEntry *found = nullptr;
	while (OSObject *child = children->getNextObject()) {
		IORegistryEntry *entry = OSDynamicCast(IORegistryEntry, child);
		if (!entry) continue;
		found = entry->metaCast("IOHIDEventDriver") ? entry : eventDriver(entry, depth - 1);
		if (found) break;
	}
	children->release();
	return found;
}

static void
publishDigitizerProperties(IORegistryEntry *entry)
{
	entry->setProperty("Built-In", kOSBooleanTrue);
	entry->setProperty("DisplayIntegrated", kOSBooleanTrue);
	entry->setProperty("PrimaryUsagePage", 0x0dULL, 32);
	entry->setProperty("PrimaryUsage", 4ULL, 32);
}

extern "C" void
native_hid_live_checkpoint(IOService *device)
{
	static IORegistryEntry *published;
	IORegistryEntry *driver = eventDriver(device, 4);
	if (!driver || driver == published) return;
	publishDigitizerProperties(driver);
	if (IORegistryEntry *interface = driver->getParentEntry(gIOServicePlane)) {
		publishDigitizerProperties(interface);
	}
	published = driver;
	printf("SYNTOUCH HID ready\n");
}

extern "C" void
native_hid_input_observed(unsigned event, int result)
{
	static unsigned reports;
	if (reports++ < 8) printf("SYNTOUCH report event=%u status=%x\n", event, result);
	if (event == 6 && !result) native_touch_hid_reports = native_touch_hid_reports + 1;
}

static bool syntheticWanted;

extern "C" bool
native_hid_input_requested(void)
{
	return syntheticWanted;
}

static void (*boardTouchSink)(void *);
static void *boardTouchSinkContext;

extern "C" void
native_touch_set_sink(void (*callback)(void *), void *context)
{
	boardTouchSink = callback;
	boardTouchSinkContext = context;
}

/* The HID component registers this sink and asks to be told after every published frame; it no
 * longer polls the seqlock on a timer. */
extern "C" void
native_touch_notify(void)
{
	if (boardTouchSink && boardTouchSinkContext) {
		boardTouchSink(boardTouchSinkContext);
	}
}

extern "C" void
native_buttons_set_sink(void (*)(void *, uint32_t), void *) {}

// One finger at the centre of the panel, for the duration of the tap.
static void
setFinger(bool down, int x, int y)
{
	native_touch_seq = native_touch_seq + 1;   // odd: writing
	if (down) {
		native_touch_xs[0] = x;
		native_touch_ys[0] = y;
		native_touch_ids[0] = 1;
		native_touch_count = 1;
	} else {
		native_touch_count = 0;
	}
	native_touch_seq = native_touch_seq + 1;   // even: stable
}

extern "C" void
synthetic_touch_start(void)
{
	IOService *service;
	bool initialized, attached, started;

	syntheticWanted = true;
	service = native_hid_input_allocate();
	if (!service) { printf("SYNTOUCH allocate failed\n"); return; }
	initialized = service->init();
	attached = initialized && service->attach(IOService::getResourceService());
	started = attached && service->start(IOService::getResourceService());
	printf("SYNTOUCH service init=%d attach=%d start=%d\n", initialized, attached, started);
	if (!started) {
		if (attached) service->detach(IOService::getResourceService());
		service->release();
	}
}

extern "C" void
synthetic_touch_tap(int x, int y)
{
	setFinger(true, x, y);
}

extern "C" void
synthetic_touch_release(void)
{
	setFinger(false, 0, 0);
}
