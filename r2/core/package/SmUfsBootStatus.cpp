// SPDX-License-Identifier: MIT
// Board boot-status compatibility interface; RAM only, no PMIC/PON backend.
// IOPMUPrimary is the original watchdogd's discovery contract. It does not
// advertise a Qualcomm power/reset/RTC operation or persistent boot-stage store.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSIterator.h>
#include <mach/kmod.h>
#include <kern/task.h>
#include <sys/proc.h>
#include <sys/errno.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void smufs_boot_status_static_init(void) __asm__("__GLOBAL__sub_I_SmUfsBootStatus.cpp");
}
static unsigned ready, writes, attempts, lastStage, lastResult;
static int lastPid = -1;

class SmUfsBootStatus : public IOService {
    OSDeclareDefaultStructors(SmUfsBootStatus)
public:
    IOReturn setProperties(OSObject *object) override {
        __atomic_add_fetch(&attempts, 1U, __ATOMIC_RELAXED);
        IOReturn result = IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator);
        if (result == kIOReturnSuccess) {
            OSDictionary *properties = OSDynamicCast(OSDictionary, object);
            OSNumber *stage = properties ? OSDynamicCast(OSNumber, properties->getObject("IOPMUBootStage")) : nullptr;
            // A single immutable numeric value. Reject unrelated and mixed
            // keys rather than forwarding them into the registry or hardware.
            if (!properties || properties->getCount() != 1 || !stage || stage->unsigned64BitValue() > 255)
                result = kIOReturnBadArgument;
            else if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE))
                result = kIOReturnNotReady;
            else if (!setProperty("IOPMUBootStage", stage))
                result = kIOReturnNoMemory;
            else {
                __atomic_store_n(&lastStage, stage->unsigned32BitValue(), __ATOMIC_RELAXED);
                __atomic_add_fetch(&writes, 1U, __ATOMIC_RELAXED);
            }
        }
        __atomic_store_n(&lastPid, proc_selfpid(), __ATOMIC_RELAXED);
        __atomic_store_n(&lastResult, unsigned(result), __ATOMIC_RELEASE);
        return result;
    }
};
OSDefineMetaClassAndStructors(SmUfsBootStatus, IOService)
static SmUfsBootStatus *bootStatus; // Owned and retained for this RAM boot.

static OSDictionary *matching() {
    OSDictionary *match = IOService::serviceMatching("IOService");
    OSDictionary *properties = OSDictionary::withCapacity(1);
    OSNumber *one = OSNumber::withNumber(1, 32);
    if (!match || !properties || !one || !properties->setObject("IOPMUPrimary", one) ||
        !match->setObject("IOPropertyMatch", properties)) {
        OSSafeReleaseNULL(match);
    }
    OSSafeReleaseNULL(one);
    OSSafeReleaseNULL(properties);
    return match;
}
static int matches(IOService *expected) {
    OSDictionary *match = matching();
    if (!match) return -ENOMEM;
    OSIterator *iterator = IOService::getMatchingServices(match);
    match->release();
    // This XNU returns NULL for an empty matching set, not an empty iterator.
    if (!iterator) return expected ? -ENXIO : 0;
    int count = 0;
    bool found = !expected;
    while (OSObject *object = iterator->getNextObject()) {
        ++count;
        if (object == expected) found = true;
    }
    bool valid = iterator->isValid();
    iterator->release();
    return valid && found ? count : -EAGAIN;
}
extern "C" int native_smufs_boot_status_prepare(void) {
    if (bootStatus) return EALREADY;
    int before = matches(nullptr);
    if (before) return before < 0 ? -before : EEXIST;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return ENOMEM;
    smufs_boot_status_static_init();
    if (OSMetaClass::postModLoad(handle)) return ENOEXEC;
    IOService *provider = IOService::getResourceService();
    if (!provider) return ENXIO;
    SmUfsBootStatus *service = new SmUfsBootStatus;
    if (!service) return ENOMEM;
    bool attached = false, started = false;
    int error = ENOMEM;
    do {
        if (!service->init()) break;
        if (!service->setProperty("UniXNUBootStatusBackend", "volatile-ram-only") ||
            !service->setProperty("HardwarePMUImplemented", false) ||
            !service->setProperty("BootStagePersistent", false)) break;
        attached = service->attach(provider);
        if (!attached) { error = ENXIO; break; }
        started = service->start(provider);
        if (!started) { error = ENXIO; break; }
        // Set the matching identity only after start; no fabricated hardware ID.
        if (!service->setProperty("IOPMUPrimary", uint64_t(1), 32)) break;
        bootStatus = service;
        __atomic_store_n(&ready, 1U, __ATOMIC_RELEASE);
        service->registerService(kIOServiceSynchronous);
        int count = matches(service);
        if (count != 1 || service->isInactive() || !(service->getState() & kIOServiceRegisteredState))
            panic("SMUFS boot-status publication refused matches%d", count);
        IOLog("SMUFS boot-status matched IOPMUPrimary=1 software RAM only\n");
        return 0;
    } while (false);
    if (started) service->stop(provider);
    if (attached) service->detach(provider);
    service->release();
    return error;
}
extern "C" const char *native_smufs_boot_status_note(void) {
    static char note[224];
    // Independent atomic observations, not an event-consistent tuple.
    snprintf(note, sizeof(note), "boot-status ready%u software1 hardware0 volatile1 attempts%u writes%u stage%u pid%d result%x\n",
        __atomic_load_n(&ready, __ATOMIC_ACQUIRE), __atomic_load_n(&attempts, __ATOMIC_RELAXED),
        __atomic_load_n(&writes, __ATOMIC_RELAXED), __atomic_load_n(&lastStage, __ATOMIC_RELAXED),
        __atomic_load_n(&lastPid, __ATOMIC_RELAXED), __atomic_load_n(&lastResult, __ATOMIC_ACQUIRE));
    return note;
}
