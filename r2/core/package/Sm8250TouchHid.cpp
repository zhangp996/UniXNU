/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSMetaClass.h>
#include <kern/clock.h>
#include <mach/kmod.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>

#include "Sm8250TouchFrame.h"

extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_hid_static_init(void) __asm__("__GLOBAL__sub_I_NativeHidInput.cpp");
extern "C" IOService *native_hid_input_allocate(void);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" void sm8250_keys_start(void);

namespace {
IOLock *sinkLock;
using TouchSink = void (*)(void *);
TouchSink sink;
IOService *sinkDevice;
using ButtonSink = void (*)(void *, uint32_t);
ButtonSink buttonSink;
IOService *buttonDevice;
unsigned successfulButtonReports;
IOService *activeDigitizer;
unsigned successfulReports;

IORegistryEntry *eventDriver(IORegistryEntry *parent, unsigned depth)
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

void publishDigitizerProperties(IORegistryEntry *entry)
{
    entry->setProperty("Built-In", kOSBooleanTrue);
    entry->setProperty("DisplayIntegrated", kOSBooleanTrue);
    entry->setProperty("PrimaryUsagePage", 0x0dULL, 32);
    entry->setProperty("PrimaryUsage", 4ULL, 32);
}
}

extern "C" unsigned native_sm_gop_status(unsigned field);

extern "C" bool native_hid_input_requested(void)
{
    static bool checked, enabled, sent;
    static uint64_t deadline;
    if (!checked) {
        unsigned mode = 0;
        enabled = PE_parse_boot_argn("sm8250-home-once", &mode, sizeof(mode)) && mode == 1;
        checked = true;
    }
    if (!enabled || sent || native_sm_gop_status(4) < 2) return false;
    if (!deadline) {
        clock_interval_to_deadline(3, NSEC_PER_SEC, &deadline);
        printf("HOME45 armed after two display retirements\n");
    }
    if (mach_absolute_time() < deadline) return false;
    sent = true;
    return true;
}

extern "C" void native_hid_live_checkpoint(IOService *device)
{
    static IORegistryEntry *published;
    IORegistryEntry *driver = eventDriver(device, 4);
    if (!driver || driver == published) return;
    publishDigitizerProperties(driver);
    if (IORegistryEntry *interface = driver->getParentEntry(gIOServicePlane)) {
        publishDigitizerProperties(interface);
    }
    published = driver;
    printf("TOUCH44 HID ready\n");
    sm8250_keys_start();
}

extern "C" void native_hid_buttons_observed(uint32_t mask, int result)
{
    if (!result) __atomic_add_fetch(&successfulButtonReports, 1u, __ATOMIC_RELAXED);
    static unsigned failures;
    if (!result || failures++ < 4) printf("KEY46 report mask=%x status=%x\n", mask, result);
}

extern "C" void native_buttons_set_sink(ButtonSink callback, void *context)
{
    IOLockLock(sinkLock);
    buttonSink = callback;
    buttonDevice = static_cast<IOService *>(context);
    IOLockUnlock(sinkLock);
}

extern "C" bool sm8250_buttons_publish(uint32_t mask)
{
    const unsigned prior = __atomic_load_n(&successfulButtonReports, __ATOMIC_RELAXED);
    IOLockLock(sinkLock);
    ButtonSink callback = buttonSink;
    IOService *device = buttonDevice;
    if (device) device->retain();
    IOLockUnlock(sinkLock);
    if (callback && device) callback(device, mask);
    OSSafeReleaseNULL(device);
    return __atomic_load_n(&successfulButtonReports, __ATOMIC_RELAXED) != prior;
}

extern "C" void native_hid_input_observed(unsigned event, int result)
{
    if (event == 4 || event == 5) {
        printf("HOME45 report action=%s status=%x\n", event == 4 ? "down" : "up", result);
        return;
    }
    if (event != 6) return;
    if (!result) __atomic_add_fetch(&successfulReports, 1u, __ATOMIC_RELAXED);
    static unsigned samples;
    if (samples++ < 8) printf("TOUCH44 HID report status=%x\n", result);
}

extern "C" unsigned sm8250_touch_hid_reports_ok(void)
{
    return __atomic_load_n(&successfulReports, __ATOMIC_RELAXED);
}

extern "C" void native_touch_set_sink(TouchSink callback, void *context)
{
    IOLockLock(sinkLock);
    sink = callback;
    sinkDevice = static_cast<IOService *>(context);
    IOLockUnlock(sinkLock);
}

extern "C" void sm8250_touch_notify(void)
{
    IOLockLock(sinkLock);
    TouchSink callback = sink;
    IOService *device = sinkDevice;
    if (device) device->retain();
    IOLockUnlock(sinkLock);
    if (callback && device) callback(device);
    OSSafeReleaseNULL(device);
}

extern "C" bool sm8250_touch_hid_start(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("sheng-touch-hid", &enabled, sizeof(enabled)) || enabled != 1) return false;
    PE_Video video = {};
    if (PE_current_console(&video) || !video.v_width || !video.v_height || video.v_rotate ||
        uint64_t(video.v_width) * SmTouchMaxY != uint64_t(video.v_height) * (SmTouchMaxX + 1)) {
        printf("TOUCH44 geometry refused %lux%lu rotation=%lu\n",
               (unsigned long)video.v_width, (unsigned long)video.v_height, (unsigned long)video.v_rotate);
        return false;
    }
    printf("TOUCH44 geometry panel-max=%ux%u display=%lux%lu rotation=0\n",
           SmTouchMaxX, SmTouchMaxY, (unsigned long)video.v_width, (unsigned long)video.v_height);

    const auto family = OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
    if (family) {
        printf("TOUCH44 HID family load status=%x\n", family);
        return false;
    }
    const auto matching = OSKext::loadKextWithIdentifier("com.apple.iokit.IOHIDEventDriver",
        true, false, kOSKextExcludeNone, kOSKextExcludeNone, nullptr);
    printf("TOUCH44 HID matching load status=%x\n", matching);
    const OSSymbol *name = OSSymbol::withCString("IOHIDDevice");
    const OSMetaClass *base = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (!base || base->getClassSize() != 208) return false;

    OSDictionary *match = IOService::serviceMatching("IOHIDResource");
    IOService *resource = match ? IOService::waitForMatchingService(match, 5000000000ULL) : nullptr;
    OSSafeReleaseNULL(match);
    if (!resource) return false;

    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (handle) {
        native_hid_static_init();
        const auto status = OSMetaClass::postModLoad(handle);
        if (!status && (sinkLock = IOLockAlloc())) {
            IOService *device = native_hid_input_allocate();
            const bool initialized = device && device->metaCast("IOHIDDevice") && device->init();
            const bool attached = initialized && device->attach(resource);
            if (attached && device->start(resource)) {
                activeDigitizer = device;
            } else {
                if (attached) device->detach(resource);
                OSSafeReleaseNULL(device);
            }
        }
    }
    resource->release();
    printf("TOUCH44 HID device started=%u\n", activeDigitizer != nullptr);
    return activeDigitizer != nullptr;
}
