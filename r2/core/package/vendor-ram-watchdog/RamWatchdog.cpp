// SPDX-License-Identifier: MIT
// RAM bring-up backend for the unchanged 21H461 IOWatchdog/client protocol.
// This is a software deadline observer, not a claim of Snapdragon hardware WDT.
#include "OriginalWatchdogABI.h"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
kern_return_t OSKextLoadKextWithIdentifier(const char *);
void fuxi_watchdog_static_init(void) __asm__("__GLOBAL__sub_I_RamWatchdog.cpp");
bool fuxi_original_watchdog_start(IOWatchdog *, IOService *, unsigned, unsigned);
// stage, accepted clients, polls, original heartbeat sequence, enabled, deadline,
// backend enable count, backend disable count. RAM only, never a disk log.
volatile uint64_t fuxi_ram_watchdog_state[8];
const char *fuxi_ram_watchdog_step = "arguments";
int fuxi_ram_watchdog_detail;
int fuxi_ram_watchdog_prepare(void);
void qcom_usb_debug_get_status(void *status);
unsigned long qcom_usb_debug_enqueue(const void *bytes, unsigned long count);
}

/* Prefix of struct qcom_usb_debug_status (include/QcomXnuUsbDebug.h) up to the transport
 * counters: enough to say whether the console module is active and whether anything is queued. */
struct usb_status_prefix {
    unsigned phase, error;
    int core_result;
    unsigned qcom_result, restore_result, dma_mode, dma_lease_active;
    unsigned vbus_owned, vbus_saved;
    unsigned phy_owned, phy_saved, phy_applied, phy_restoring, stop_failed;
    unsigned dma_address_width, hwparams0;
    unsigned last_trace, trace_a, trace_b, traces;
    unsigned poll_calls, last_poll_events, io_failed;
    uint64_t dma_pa, counter_frequency, cache_line, dropped;
    unsigned domain_result, domain_stage, domain_bank, domain_slot, domain_asid;
    uint64_t tables_pa, dma_iova, domain_last_address, domain_last_expected, domain_last_readback;
    unsigned diag_enabled, diag_checkpoint;
    uint64_t buffer_capacity, buffer_pending, buffer_high_water, buffer_accepted;
    uint64_t buffer_overflow, buffer_contended, buffer_replayed, transport_pending;
};

// Live panel heartbeat: one block per watchdog tick, drawn straight into the frame buffer the
// loader handed over. The panic screen does not fit beside this code, so the panel is the only
// channel that can show the kernel is still scheduling once the service graph starts.
extern "C" void *ml_io_map(vm_offset_t paddr, vm_size_t size, unsigned int flags);

/* The storage stack can flood the console with a retry loop, which buries every early diagnostic.
 * This repeats the block-device list from the watchdog tick so the panel always shows it. */
static void ram_watchdog_block_report(void)
{
    OSDictionary *matching = IOService::serviceMatching("IOMedia");
    OSIterator *iterator = matching ? IOService::getMatchingServices(matching) : nullptr;
    while (iterator) {
        IOService *entry = OSDynamicCast(IOService, iterator->getNextObject());
        if (!entry) {
            break;
        }
        OSString *name = OSDynamicCast(OSString, entry->getProperty("BSD Name"));
        OSNumber *size = OSDynamicCast(OSNumber, entry->getProperty("Size"));
        if (name) {
            extern volatile uint64_t native_ram_block_state[4];
            OSString *uuid = OSDynamicCast(OSString, entry->getProperty("UUID"));
            printf("BLK-TICK: %s size=%llu uuid=%.36s\n", name->getCStringNoCopy(),
                   size ? size->unsigned64BitValue() : 0,
                   uuid ? uuid->getCStringNoCopy() : "-");
            extern volatile int native_owner_data_stage;
            extern unsigned long native_owner_cache_carrier_accepts;
            extern unsigned long native_owner_cache_carrier_rejects;
            extern unsigned long native_sr_map_calls;
            extern unsigned long native_sr_map_failures;
            printf("SR-MAP: calls=%lu failures=%lu\n", native_sr_map_calls, native_sr_map_failures);
            {
                struct usb_status_prefix us = {};
                qcom_usb_debug_get_status(&us);
                printf("USBPHASE: phase=%u poll=%u ioFail=%u pending=%llu accepted=%llu transport=%llu\n",
                       us.phase, us.poll_calls, us.io_failed,
                       (unsigned long long)us.buffer_pending,
                       (unsigned long long)us.buffer_accepted,
                       (unsigned long long)us.transport_pending);
            }
            printf("RWSTATE: online=%llu mem=%llu capacity=%llu big=%llu dataStage=%d carrier=%lu/%lu\n",
                   native_ram_block_state[0], native_ram_block_state[1],
                   native_ram_block_state[2], native_ram_block_state[3],
                   (int)native_owner_data_stage,
                   native_owner_cache_carrier_accepts, native_owner_cache_carrier_rejects);
        }
    }
    OSSafeReleaseNULL(iterator);
    OSSafeReleaseNULL(matching);
}

static void ram_watchdog_panel_tick(uint64_t tick)
{
    static volatile uint32_t *panel;
    if (!panel) {
        panel = (volatile uint32_t *)ml_io_map(0x9C000000, 1080u * 480u * 4u, 0x00000040u);
    }
    if (!panel) {
        return;
    }
    /* One moving marker: clear the row and draw the block for this tick, so the panel shows a
     * travelling dot rather than a row that fills up and then looks frozen. */
    for (unsigned y = 0; y < 16; y++) {
        for (unsigned x = 0; x < 28u * 20u; x++) {
            panel[(y + 96) * 1080u + 8u + x] = 0x00000000u;
        }
    }
    unsigned slot = (unsigned)(tick % 28u);
    for (unsigned y = 0; y < 16; y++) {
        for (unsigned x = 0; x < 16; x++) {
            panel[(y + 96) * 1080u + slot * 20u + 8u + x] = 0x0000FFFFu;
        }
    }
}

class RamWatchdog : public IOWatchdog {
    OSDeclareDefaultStructors(RamWatchdog)
    thread_call_t timer = nullptr;
    unsigned armed = 0;
    static void expired(thread_call_param_t parameter, thread_call_param_t) {
        auto *self = static_cast<RamWatchdog *>(parameter);
        if (!__atomic_load_n(&self->armed, __ATOMIC_ACQUIRE)) return;
        // These fields and their lock are qualified from the original methods
        // that enable/disable/pet the watchdog. Never write original state here.
        auto *lock = *reinterpret_cast<IOSimpleLock **>(reinterpret_cast<char *>(self) + 0xa0);
        if (!lock) panic("RAM_WATCHDOG missing original heartbeat lock");
        bool interrupts = ml_set_interrupts_enabled(false);
        IOSimpleLockLock(lock);
        bool enabled = *reinterpret_cast<unsigned char *>(reinterpret_cast<char *>(self) + 0xa8);
        uint64_t sequence = *reinterpret_cast<uint64_t *>(reinterpret_cast<char *>(self) + 0xb8);
        uint64_t deadline = *reinterpret_cast<uint64_t *>(reinterpret_cast<char *>(self) + 0xc8);
        uint64_t now = mach_absolute_time();
        IOSimpleLockUnlock(lock);
        ml_set_interrupts_enabled(interrupts);
        uint64_t beat = __atomic_add_fetch(&fuxi_ram_watchdog_state[2], 1, __ATOMIC_RELAXED);
        ram_watchdog_panel_tick(beat);
        if ((beat % 20u) == 1u) {
            ram_watchdog_block_report();
        }
        fuxi_ram_watchdog_state[3] = sequence;
        fuxi_ram_watchdog_state[4] = enabled;
        fuxi_ram_watchdog_state[5] = deadline;
        if (enabled && (!deadline || now >= deadline)) {
            /* Bring-up: the service must exist so watchdogd's open succeeds, but nothing pets it yet
             * (watchdogd exits early on this port), so the deadline is reported instead of fatal. */
            static unsigned reported;
            if (!reported) {
                reported = 1;
                printf("RAM_WATCHDOG deadline passed (not fatal): beats=%llu now=%llx deadline=%llx\n",
                    sequence, now, deadline);
            }
            self->armed = 0;
            return;
        }
        /* Re-arming from inside the callback is what keeps the heartbeat alive, but it must stop: the
         * service graph runs for minutes and the repeated thread_call_invoke path is where this
         * board crashed (pc = thread_call_invoke + 0x94).  Sixty beats are enough for the panel
         * heartbeat; after that the backend stays registered and silent. */
        if (beat < 60 && __atomic_load_n(&self->armed, __ATOMIC_ACQUIRE)) self->schedule();
    }
    void schedule() {
        uint64_t next;
        clock_interval_to_deadline(1, NSEC_PER_SEC, &next);
        thread_call_enter_delayed(timer, next);
    }
public:
    void registerService(IOOptionBits options = 0) override {
        // The original start calls this virtual with options=0. Its asynchronous
        // default does not guarantee the MatchedState required by our lookup.
        IOService::registerService((options & ~IOOptionBits(kIOServiceAsynchronous)) | kIOServiceSynchronous);
    }
    bool prepareTimer() {
        timer = thread_call_allocate(expired, this);
        return timer != nullptr;
    }
    bool start(IOService *provider) override {
        // Same option arguments as the original hardware subclass's zero-flag
        // path; no boot-arg change to the original monitoring/permission policy.
        return timer && fuxi_original_watchdog_start(this, provider, 0, 0);
    }
    void stop(IOService *provider) override {
        IOWatchdog::stop(provider);
    }
    void free() override {
        __atomic_store_n(&armed, 0, __ATOMIC_RELEASE);
        if (timer) { thread_call_cancel_wait(timer); thread_call_free(timer); timer = nullptr; }
        IOService::free();
    }
    IOReturn newUserClient(task_t task, void *security, UInt32 type,
                          OSDictionary *properties, IOUserClient **client) override {
        IOReturn result = IOWatchdog::newUserClient(task, security, type, properties, client);
        if (result == kIOReturnSuccess && client && *client)
            __atomic_add_fetch(&fuxi_ram_watchdog_state[1], 1, __ATOMIC_RELAXED);
        return result;
    }
    // Exact three abstract slots of the original IOWatchdog, after IOService.
    // The deadline is owned by its original heartbeat/enable/disable handlers.
    void enableBackend() override {
        __atomic_store_n(&armed, 1, __ATOMIC_RELEASE);
        __atomic_add_fetch(&fuxi_ram_watchdog_state[6], 1, __ATOMIC_RELAXED);
        schedule();
    }
    void disableBackend() override {
        __atomic_store_n(&armed, 0, __ATOMIC_RELEASE);
        __atomic_add_fetch(&fuxi_ram_watchdog_state[7], 1, __ATOMIC_RELAXED);
        if (timer) thread_call_cancel(timer);
    }
    void restartBackend() override {
        // Re-evaluate the actual original deadline; do not manufacture a pet.
        if (__atomic_load_n(&armed, __ATOMIC_ACQUIRE)) schedule();
    }
};
OSDefineMetaClassAndStructors(RamWatchdog, IOWatchdog)

static RamWatchdog *watchdog; // Published service retained for this RAM boot.
static const OSMetaClass *lookup(const char *name) {
    const OSSymbol *key = OSSymbol::withCString(name);
    const OSMetaClass *meta = key ? OSMetaClass::getMetaClassWithName(key) : nullptr;
    OSSafeReleaseNULL(key);
    return meta;
}
static int countServices(IOService *expected) {
    OSDictionary *match = IOService::serviceMatching("IOWatchdog");
    if (!match) return -ENOMEM;
    OSIterator *iterator = IOService::getMatchingServices(match);
    match->release();
    if (!iterator) return 0;
    int count = 0;
    bool owns = expected == nullptr;
    while (OSObject *object = iterator->getNextObject()) {
        ++count;
        if (object == expected) owns = true;
    }
    bool valid = iterator->isValid();
    iterator->release();
    if (!valid) return -EAGAIN;
    return owns ? count : -ENXIO;
}
extern "C" int fuxi_ram_watchdog_prepare(void) {
    fuxi_ram_watchdog_step = "arguments";
    fuxi_ram_watchdog_detail = 0;
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("ram-system-watchdog", &enabled, sizeof(enabled)) || enabled != 1 || watchdog)
        return EINVAL;
    fuxi_ram_watchdog_step = "load";
    fuxi_ram_watchdog_detail = OSKextLoadKextWithIdentifier("com.apple.driver.AppleARMWatchdogTimer");
    if (fuxi_ram_watchdog_detail) return ENXIO;
    fuxi_ram_watchdog_state[0] = 1;
    fuxi_ram_watchdog_step = "metaclass";
    const OSMetaClass *base = lookup("IOWatchdog"), *client = lookup("IOWatchdogUserClient");
    if (!base || base != &IOWatchdog::gMetaClass || base->getClassSize() != 0xf8 ||
        !client || client->getClassSize() != 0xe8) return ENOEXEC;
    fuxi_ram_watchdog_step = "preexisting";
    int before = countServices(nullptr);
    fuxi_ram_watchdog_detail = before;
    if (before != 0) return before < 0 ? -before : EEXIST;
    fuxi_ram_watchdog_step = "pre-mod";
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return ENOMEM;
    fuxi_watchdog_static_init();
    fuxi_ram_watchdog_step = "post-mod";
    fuxi_ram_watchdog_detail = OSMetaClass::postModLoad(handle);
    if (fuxi_ram_watchdog_detail) return ENOEXEC;
    fuxi_ram_watchdog_step = "provider";
    IOService *provider = IOService::getResourceService();
    if (!provider) return ENXIO;
    fuxi_ram_watchdog_step = "allocate";
    watchdog = new RamWatchdog;
    if (!watchdog) return ENOMEM;
    fuxi_ram_watchdog_step = "init";
    if (!watchdog->init()) return ENOMEM;
    fuxi_ram_watchdog_step = "timer";
    if (!watchdog->prepareTimer()) return ENOMEM;
    watchdog->setProperty("UniXNUWatchdogBackend", "software-original-deadline");
    watchdog->setProperty("HardwareWatchdogImplemented", false);
    fuxi_ram_watchdog_step = "attach";
    if (!watchdog->attach(provider)) return ENXIO;
    fuxi_ram_watchdog_step = "start";
    if (!watchdog->start(provider)) return ENXIO;
    fuxi_ram_watchdog_state[0] = 2;
    fuxi_ram_watchdog_step = "match";
    fuxi_ram_watchdog_detail = countServices(watchdog);
    if (fuxi_ram_watchdog_detail != 1)
        return fuxi_ram_watchdog_detail < 0 ? -fuxi_ram_watchdog_detail :
            fuxi_ram_watchdog_detail > 1 ? EEXIST : ENXIO;
    fuxi_ram_watchdog_state[0] = 3;
    fuxi_ram_watchdog_step = "ready";
    IOLog("RAM_WATCHDOG registered matched started: original IOWatchdog/client; software deadline backend\n");
    return 0;
}
