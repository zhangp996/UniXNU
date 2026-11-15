// SPDX-License-Identifier: MIT
// Adapted from mouse233/unixnu-fuxi commit9644d94e6bb050e4bde5f50f94312dd10cd3f416 package/RamWatchdog.cpp (MIT).
// Board changes: names, guarded scheduling/drain, atomic diagnostic snapshots and strict H3V caller.
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
#include <sys/proc.h>
#include <libkern/libkern.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
kern_return_t OSKextLoadKextWithIdentifier(const char *);
void smufs_watchdog_static_init(void) __asm__("__GLOBAL__sub_I_SmUfsWatchdog.cpp");
bool smufs_original_watchdog_start(IOWatchdog *, IOService *, unsigned, unsigned);
// stage, accepted clients, polls, original heartbeat sequence, enabled, deadline,
// backend enable count, backend disable count. RAM only, never a disk log.
volatile uint64_t native_smufs_watchdog_state[8];
const char *native_smufs_watchdog_step = "arguments";
int native_smufs_watchdog_detail;
int native_smufs_watchdog_prepare(void);
void native_smufs_watchdog_client_result(unsigned type, IOReturn result);
}

class SmUfsPlatformWatchdog : public IOWatchdog {
    OSDeclareDefaultStructors(SmUfsPlatformWatchdog)
    thread_call_t timer = nullptr;
    unsigned armed = 0;
    unsigned stopping = 0;
    IOSimpleLock *scheduleGate = nullptr;
    void quiesce() {
        if (scheduleGate) {
            bool interrupts = ml_set_interrupts_enabled(false);
            IOSimpleLockLock(scheduleGate);
            __atomic_store_n(&stopping, 1U, __ATOMIC_RELEASE);
            __atomic_store_n(&armed, 0U, __ATOMIC_RELEASE);
            if (timer) thread_call_cancel(timer);
            IOSimpleLockUnlock(scheduleGate);
            ml_set_interrupts_enabled(interrupts);
        }
        // No scheduling caller can queue after stopping is set under the gate.
        // Drain before the original stop releases its heartbeat lock/state.
        if (timer) thread_call_cancel_wait(timer);
    }
    static void expired(thread_call_param_t parameter, thread_call_param_t) {
        auto *self = static_cast<SmUfsPlatformWatchdog *>(parameter);
        if (!__atomic_load_n(&self->armed, __ATOMIC_ACQUIRE) || __atomic_load_n(&self->stopping, __ATOMIC_ACQUIRE)) return;
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
        __atomic_add_fetch(&native_smufs_watchdog_state[2], 1, __ATOMIC_RELAXED);
        __atomic_store_n(&native_smufs_watchdog_state[3], sequence, __ATOMIC_RELAXED);
        __atomic_store_n(&native_smufs_watchdog_state[4], enabled, __ATOMIC_RELAXED);
        __atomic_store_n(&native_smufs_watchdog_state[5], deadline, __ATOMIC_RELAXED);
        if (enabled && (!deadline || now >= deadline) && __atomic_load_n(&self->armed, __ATOMIC_ACQUIRE) && !__atomic_load_n(&self->stopping, __ATOMIC_ACQUIRE))
            panic("RAM_WATCHDOG software deadline expired: beats=%llu now=%llx deadline=%llx",
                sequence, now, deadline);
        if (__atomic_load_n(&self->armed, __ATOMIC_ACQUIRE)) self->schedule();
    }
    void schedule() {
        if (!scheduleGate) return;
        bool interrupts = ml_set_interrupts_enabled(false);
        IOSimpleLockLock(scheduleGate);
        if (!__atomic_load_n(&stopping, __ATOMIC_ACQUIRE) && __atomic_load_n(&armed, __ATOMIC_ACQUIRE) && timer) {
            uint64_t next;
            clock_interval_to_deadline(1, NSEC_PER_SEC, &next);
            thread_call_enter_delayed(timer, next);
        }
        IOSimpleLockUnlock(scheduleGate);
        ml_set_interrupts_enabled(interrupts);
    }
public:
    void registerService(IOOptionBits options = 0) override {
        // The original start calls this virtual with options=0. Its asynchronous
        // default does not guarantee the MatchedState required by our lookup.
        IOService::registerService((options & ~IOOptionBits(kIOServiceAsynchronous)) | kIOServiceSynchronous);
    }
    bool prepareTimer() {
        scheduleGate = IOSimpleLockAlloc();
        if (!scheduleGate) return false;
        timer = thread_call_allocate(expired, this);
        return timer != nullptr;
    }
    bool start(IOService *provider) override {
        // Same option arguments as the original hardware subclass's zero-flag
        // path; no boot-arg change to the original monitoring/permission policy.
        return timer && smufs_original_watchdog_start(this, provider, 0, 0);
    }
    void stop(IOService *provider) override {
        quiesce();
        IOWatchdog::stop(provider);
    }
    void free() override {
        quiesce();
        if (timer) { thread_call_free(timer); timer = nullptr; }
        if (scheduleGate) { IOSimpleLockFree(scheduleGate); scheduleGate = nullptr; }
        IOService::free();
    }
    IOReturn newUserClient(task_t task, void *security, UInt32 type,
                          OSDictionary *properties, IOUserClient **client) override {
        IOReturn result = IOWatchdog::newUserClient(task, security, type, properties, client);
        native_smufs_watchdog_client_result(type, result);
        if (result == kIOReturnSuccess && client && *client)
            __atomic_add_fetch(&native_smufs_watchdog_state[1], 1, __ATOMIC_RELAXED);
        return result;
    }
    // Exact three abstract slots of the original IOWatchdog, after IOService.
    // The deadline is owned by its original heartbeat/enable/disable handlers.
    void enableBackend() override {
        if (!scheduleGate) panic("SMUFS watchdog missing schedule gate");
        bool interrupts = ml_set_interrupts_enabled(false);
        IOSimpleLockLock(scheduleGate);
        if (!__atomic_load_n(&stopping, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&armed, 1U, __ATOMIC_RELEASE);
            __atomic_add_fetch(&native_smufs_watchdog_state[6], 1, __ATOMIC_RELAXED);
        }
        IOSimpleLockUnlock(scheduleGate);
        ml_set_interrupts_enabled(interrupts);
        schedule();
    }
    void disableBackend() override {
        if (!scheduleGate) return;
        bool interrupts = ml_set_interrupts_enabled(false);
        IOSimpleLockLock(scheduleGate);
        __atomic_store_n(&armed, 0U, __ATOMIC_RELEASE);
        __atomic_add_fetch(&native_smufs_watchdog_state[7], 1, __ATOMIC_RELAXED);
        if (timer) thread_call_cancel(timer);
        IOSimpleLockUnlock(scheduleGate);
        ml_set_interrupts_enabled(interrupts);
    }
    void restartBackend() override {
        // Re-evaluate the actual original deadline; do not manufacture a pet.
        if (__atomic_load_n(&armed, __ATOMIC_ACQUIRE)) schedule();
    }
};
OSDefineMetaClassAndStructors(SmUfsPlatformWatchdog, IOWatchdog)

static SmUfsPlatformWatchdog *watchdog; // Published service retained for this RAM boot.

// Observe exact real service identity. No process-name grants or return changes.
struct SmUfsWatchdogPolicyObservation {
    unsigned state;
    int pid;
    unsigned type;
    int target;
    int control;
};
static SmUfsWatchdogPolicyObservation policyObservation;
static SmUfsWatchdogPolicyObservation clientObservation;
static volatile unsigned policyEvents;
extern "C" bool native_smufs_watchdog_is_service(IOService *service) {
    return __atomic_load_n(&native_smufs_watchdog_state[0], __ATOMIC_ACQUIRE) == 3 && service == watchdog;
}
extern "C" void native_smufs_watchdog_open_policy_observe(IOService *service, unsigned type, int target, int control) {
    if (!native_smufs_watchdog_is_service(service)) return;
    __atomic_add_fetch(&policyEvents, 1U, __ATOMIC_RELAXED);
    unsigned expected = 0;
    if (!__atomic_compare_exchange_n(&policyObservation.state, &expected, 1U, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    policyObservation.pid = proc_selfpid();
    policyObservation.type = type;
    policyObservation.target = target;
    policyObservation.control = control;
    __atomic_store_n(&policyObservation.state, 2U, __ATOMIC_RELEASE);
    IOLog("SMUFS watchdog policy pid%d type%u target%d control%d (original checks)\n", policyObservation.pid, type, target, control);
}
extern "C" void native_smufs_watchdog_client_result(unsigned type, IOReturn result) {
    unsigned expected = 0;
    if (!__atomic_compare_exchange_n(&clientObservation.state, &expected, 1U, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    clientObservation.pid = proc_selfpid();
    clientObservation.type = type;
    clientObservation.target = result;
    __atomic_store_n(&clientObservation.state, 2U, __ATOMIC_RELEASE);
}

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
extern "C" int native_smufs_watchdog_prepare(void) {
    native_smufs_watchdog_step = "arguments";
    native_smufs_watchdog_detail = 0;
    // Board-specific caller runs exactly once after strict root/SystemOS/Data
    // preparation and before original launchd. No extra boot argument is needed.
    if (watchdog) return EALREADY;
    native_smufs_watchdog_step = "load";
    native_smufs_watchdog_detail = OSKextLoadKextWithIdentifier("com.apple.driver.AppleARMWatchdogTimer");
    if (native_smufs_watchdog_detail) return ENXIO;
    __atomic_store_n(&native_smufs_watchdog_state[0], 1U, __ATOMIC_RELEASE);
    native_smufs_watchdog_step = "metaclass";
    const OSMetaClass *base = lookup("IOWatchdog"), *client = lookup("IOWatchdogUserClient");
    if (!base || base != &IOWatchdog::gMetaClass || base->getClassSize() != 0xf8 ||
        !client || client->getClassSize() != 0xe8) return ENOEXEC;
    native_smufs_watchdog_step = "preexisting";
    int before = countServices(nullptr);
    native_smufs_watchdog_detail = before;
    if (before != 0) return before < 0 ? -before : EEXIST;
    native_smufs_watchdog_step = "pre-mod";
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return ENOMEM;
    smufs_watchdog_static_init();
    native_smufs_watchdog_step = "post-mod";
    native_smufs_watchdog_detail = OSMetaClass::postModLoad(handle);
    if (native_smufs_watchdog_detail) return ENOEXEC;
    native_smufs_watchdog_step = "provider";
    IOService *provider = IOService::getResourceService();
    if (!provider) return ENXIO;
    native_smufs_watchdog_step = "allocate";
    watchdog = new SmUfsPlatformWatchdog;
    if (!watchdog) return ENOMEM;
    native_smufs_watchdog_step = "init";
    if (!watchdog->init()) return ENOMEM;
    native_smufs_watchdog_step = "timer";
    if (!watchdog->prepareTimer()) return ENOMEM;
    native_smufs_watchdog_step = "properties";
    if (!watchdog->setProperty("UniXNUWatchdogBackend", "software-original-deadline") ||
        !watchdog->setProperty("HardwareWatchdogImplemented", false)) return ENOMEM;
    native_smufs_watchdog_step = "attach";
    if (!watchdog->attach(provider)) return ENXIO;
    native_smufs_watchdog_step = "start";
    if (!watchdog->start(provider)) return ENXIO;
    __atomic_store_n(&native_smufs_watchdog_state[0], 2U, __ATOMIC_RELEASE);
    native_smufs_watchdog_step = "match";
    native_smufs_watchdog_detail = countServices(watchdog);
    if (native_smufs_watchdog_detail != 1)
        return native_smufs_watchdog_detail < 0 ? -native_smufs_watchdog_detail :
            native_smufs_watchdog_detail > 1 ? EEXIST : ENXIO;
    __atomic_store_n(&native_smufs_watchdog_state[0], 3U, __ATOMIC_RELEASE);
    native_smufs_watchdog_step = "ready";
    IOLog("RAM_WATCHDOG registered matched started: original IOWatchdog/client; software deadline backend\n");
    return 0;
}

extern "C" const char *native_smufs_watchdog_note(void) {
    static char note[768];
    bool policyReady = __atomic_load_n(&policyObservation.state, __ATOMIC_ACQUIRE) == 2;
    bool clientReady = __atomic_load_n(&clientObservation.state, __ATOMIC_ACQUIRE) == 2;
    // Individual atomic observations, not a coherent tuple of original fields.
    snprintf(note, sizeof(note), "watchdog stage%llu step=%s raw=%d software1 hardware0\nclients%llu polls%llu beats%llu enabled%llu deadline%llx backend%llu/%llu\npolicy seen%u events%u pid%d type%u target%d control%d\nclient seen%u pid%d type%u result%x\n",
        (unsigned long long)__atomic_load_n(&native_smufs_watchdog_state[0], __ATOMIC_ACQUIRE), native_smufs_watchdog_step, native_smufs_watchdog_detail,
        (unsigned long long)__atomic_load_n(&native_smufs_watchdog_state[1], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&native_smufs_watchdog_state[2], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&native_smufs_watchdog_state[3], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&native_smufs_watchdog_state[4], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&native_smufs_watchdog_state[5], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&native_smufs_watchdog_state[6], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&native_smufs_watchdog_state[7], __ATOMIC_RELAXED),
        policyReady ? 1U : 0U, __atomic_load_n(&policyEvents, __ATOMIC_RELAXED),
        policyReady ? policyObservation.pid : -1, policyReady ? policyObservation.type : 0U,
        policyReady ? policyObservation.target : -1, policyReady ? policyObservation.control : -1,
        clientReady ? 1U : 0U, clientReady ? clientObservation.pid : -1,
        clientReady ? clientObservation.type : 0U, clientReady ? unsigned(clientObservation.target) : 0U);
    return note;
}
