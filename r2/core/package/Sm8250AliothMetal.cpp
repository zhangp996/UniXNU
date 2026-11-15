// SPDX-License-Identifier: BSD-2-Clause-Patent
// IN2020 registration of the staged UniXNU-alioth Metal accelerator.
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern "C" {
int OSKextLoadKextWithIdentifier(const char *);
extern kmod_info_t g_kernel_kmod_info;
void native_adreno_accelerator_static_init() __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp");
IOService *native_adreno_accelerator_allocate();
unsigned native_adreno_accelerator_instance_size();
}

namespace {
bool gBackendReady;
}

extern "C" void sm8250_alioth_metal_backend_ready()
{
    __atomic_store_n(&gBackendReady, true, __ATOMIC_RELEASE);
}

extern "C" int sm8250_alioth_metal_register()
{
    if (!__atomic_load_n(&gBackendReady, __ATOMIC_ACQUIRE))
        return kIOReturnNotReady;
    const int loaded = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
    if (loaded)
        return loaded;
    const OSSymbol *name = OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (!parent || parent->getClassSize() != 136)
        return kIOReturnNotFound;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle)
        return kIOReturnNoResources;
    native_adreno_accelerator_static_init();
    const auto result = OSMetaClass::postModLoad(handle);
    if (result)
        return result;
    if (native_adreno_accelerator_instance_size() != 136)
        return kIOReturnInvalid;

    auto *accelerator = native_adreno_accelerator_allocate();
    if (!accelerator)
        return kIOReturnNoMemory;
    if (!accelerator->init() || !accelerator->attach(IOService::getPlatform())) {
        accelerator->release();
        return kIOReturnNotAttached;
    }
    accelerator->setProperty("MetalPluginName", "NativeAdrenoMetal");
    accelerator->setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
    accelerator->setProperty("NativeTurnipDebug", "nocb");
    accelerator->setProperty("NativeMetalWithhold", "apple-families");
    accelerator->setProperty("NativeSandboxGrantProcess",
        "backboardd,iconservicesagent,Preferences,CollectionsPoster,AegirPoster,KaleidoscopePoster,"
        "PhotosPosterProvider,PosterBoard,WidgetRenderer_Default,mediaserverd,SpringBoard");
    accelerator->registerService();
    return kIOReturnSuccess;
}
