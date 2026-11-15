// SPDX-License-Identifier: BSD-2-Clause-Patent
// Expose no-passcode state after the verified fixture Data mount and board policy gate.
// The service supplies neither key material nor password operations.
#include <IOKit/IOService.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
#include <sys/proc.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_keyevents_static_init(void) __asm__("__GLOBAL__sub_I_NativeKeyStoreEvents.cpp");
extern "C" IOService *native_keyevents_allocate(void);
extern "C" int smufs_aks_data_ready(void);

extern "C" {
static unsigned started;
volatile uint32_t native_keyevents_stage, native_keyevents_refused_opens;
volatile int native_keyevents_status = -1;
volatile uint64_t native_keyevents_service;

void native_keyevents_open_refused(void) {
    native_keyevents_refused_opens = native_keyevents_refused_opens + 1;
}
bool native_aks_state_enabled(void) { return smufs_aks_data_ready() != 0; }
uint32_t native_aks_lock_state(void) { return 3; }
void native_aks_call(uint32_t selector, int32_t status) {
    static uint32_t samples[2];
    char caller[17] = {};
    proc_selfname(caller, sizeof(caller));
    const unsigned group = !strcmp(caller, "SpringBoard");
    if (__atomic_fetch_add(&samples[group], 1, __ATOMIC_RELAXED) < 16)
        printf("KEYSTATE36 pid%d %s selector%u status%x\n", proc_selfpid(), caller, selector, status);
}
void native_aks_opened(void) {}

void native_keyevents_late_probe(void) {
    if (started++) return;
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("offline-key-events", &enabled, sizeof(enabled)) || enabled != 1) return;
    native_keyevents_stage = 1;

    const OSSymbol *name = OSSymbol::withCString("AppleKeyStore");
    bool absent = name && !OSMetaClass::getMetaClassWithName(name);
    OSSafeReleaseNULL(name);
    if (!absent) {
        native_keyevents_status = kIOReturnExclusiveAccess;
        printf("KEYEVENT29 class-present status%x\n", native_keyevents_status);
        return;
    }

    IOService *provider = IOService::getResourceService();
    if (!provider) {
        native_keyevents_status = kIOReturnNoDevice;
        printf("KEYEVENT29 no-provider status%x\n", native_keyevents_status);
        return;
    }
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        native_keyevents_status = kIOReturnNoResources;
        printf("KEYEVENT29 premod status%x\n", native_keyevents_status);
        return;
    }
    native_keyevents_static_init();
    native_keyevents_status = OSMetaClass::postModLoad(handle);
    if (native_keyevents_status) {
        printf("KEYEVENT29 postmod status%x\n", native_keyevents_status);
        return;
    }
    native_keyevents_stage = 2;

    IOService *service = native_keyevents_allocate();
    bool initialized = service && service->init();
    bool attached = initialized && service->attach(provider);
    bool running = attached && service->start(provider);
    if (!running) {
        if (attached) service->detach(provider);
        OSSafeReleaseNULL(service);
        native_keyevents_status = kIOReturnError;
        printf("KEYEVENT29 start init%u attach%u status%x\n", initialized, attached, native_keyevents_status);
        return;
    }
    native_keyevents_service = reinterpret_cast<uintptr_t>(service);
    native_keyevents_status = kIOReturnSuccess;
    native_keyevents_stage = 3;
    printf("KEYSTATE36 registered service%p; state gated by verified Data and no-passcode policy\n", service);
}
}
