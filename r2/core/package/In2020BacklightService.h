// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copyright (c) 2026, UniXNU contributors. All rights reserved.
// OS properties and request coalescing follow UniXNU-sheng AppleARMBacklight.cpp
// at 516a780473a2ac59c8b438e132bb60204dda9e22. Transport is IN2020 DSI0.
#include <libkern/c++/OSNumber.h>
#include <sys/proc.h>

namespace {
constexpr uint32_t kBacklightMaximum = 2047;
const OSMetaClass *backlightAppleClass;

class In2020Backlight : public IOService {
    OSDeclareDefaultStructors(In2020Backlight)

    thread_call_t worker = nullptr;
    IOMemoryMap *clocks = nullptr;
    uint64_t pending = 0;
    uint32_t requests = 0;
    uint32_t level = 0;
    unsigned startAttempts = 0;
    bool published = false;

    bool publishLevel(uint32_t value)
    {
        OSDictionary *brightness = OSDictionary::withCapacity(3);
        OSDictionary *parameters = OSDictionary::withCapacity(1);
        OSNumber *minimum = OSNumber::withNumber(0ULL, 32);
        OSNumber *maximum = OSNumber::withNumber(kBacklightMaximum, 32);
        OSNumber *current = OSNumber::withNumber(value, 32);
        bool ok = brightness && parameters && minimum && maximum && current &&
            brightness->setObject("min", minimum) && brightness->setObject("max", maximum) &&
            brightness->setObject("value", current) && parameters->setObject("brightness", brightness) &&
            setProperty("IODisplayParameters", parameters);
        OSSafeReleaseNULL(minimum);
        OSSafeReleaseNULL(maximum);
        OSSafeReleaseNULL(current);
        OSSafeReleaseNULL(brightness);
        OSSafeReleaseNULL(parameters);
        return ok;
    }

    void schedule(unsigned milliseconds)
    {
        uint64_t deadline;
        clock_interval_to_deadline(milliseconds, NSEC_PER_MSEC, &deadline);
        thread_call_enter_delayed(worker, deadline);
    }

    bool readLevel(uint32_t &value)
    {
        bool issued = false;
        return displayBrightnessTransfer(clocks, true, 0, value, issued) && value <= kBacklightMaximum;
    }

    bool applyLevel(uint32_t value)
    {
        uint32_t observed = 0;
        bool issued = false;
        bool written = displayBrightnessTransfer(clocks, false, value, observed, issued);
        bool read = issued && readLevel(observed);
        if (read) {
            level = observed;
            publishLevel(level);
        }
        if (!written || !read || observed != value) {
            printf("BACKLIGHT failure target=%u issued=%u observed=%u read=%u\n",
                value, unsigned(issued), observed, unsigned(read));
            return false;
        }
        return true;
    }

    void serviceRequests()
    {
        if (!published) {
            if (!readLevel(level)) {
                if (++startAttempts < 10) schedule(1000);
                else printf("BACKLIGHT unavailable=initial-read\n");
                return;
            }
            if (!publishLevel(level) || !setProperty("NativeSandboxGrantProcess", "backboardd") ||
                !setProperty("backlight-control", true)) {
                printf("BACKLIGHT unavailable=properties\n");
                return;
            }
            published = true;
            printf("BACKLIGHT published class=%s level=%u max=%u\n",
                getMetaClass()->getClassName(), level, kBacklightMaximum);
            registerService();
        }

        const uint64_t request = __atomic_exchange_n(&pending, 0ULL, __ATOMIC_ACQUIRE);
        if (!(request >> 63)) return;
        const uint32_t target = uint32_t(request >> 16) & 0xffff;
        const uint32_t milliseconds = uint32_t(request) & 0xffff;
        const uint32_t from = level;
        const uint64_t began = mach_absolute_time();
        uint64_t period;
        nanoseconds_to_absolutetime(uint64_t(milliseconds) * NSEC_PER_MSEC, &period);
        bool success = true;
        for (;;) {
            if (__atomic_load_n(&pending, __ATOMIC_ACQUIRE) >> 63) {
                thread_call_enter(worker);
                return;
            }
            const uint64_t elapsed = mach_absolute_time() - began;
            const uint32_t next = period && elapsed < period ?
                uint32_t(int64_t(from) + (int64_t(target) - from) * int64_t(elapsed) / int64_t(period)) : target;
            if (next != level && !applyLevel(next)) {
                success = false;
                break;
            }
            if (elapsed >= period) break;
            IOSleep(16);
        }
        printf("BACKLIGHT applied target=%u observed=%u verified=%u requests=%u\n",
            target, level, unsigned(success && level == target),
            __atomic_load_n(&requests, __ATOMIC_RELAXED));
    }

    static void run(thread_call_param_t owner, thread_call_param_t)
    {
        static_cast<In2020Backlight *>(owner)->serviceRequests();
    }

public:
    bool start(IOService *provider) override
    {
        if (!IOService::start(provider)) return false;
        clocks = displayProbeMap(kDisplayClockPage, 0x1000);
        worker = thread_call_allocate_with_options(run, this,
            THREAD_CALL_PRIORITY_LOW, THREAD_CALL_OPTIONS_ONCE);
        if (!clocks || !worker) return false;
        schedule(1000);
        return true;
    }

    void free() override
    {
        if (worker) thread_call_free(worker);
        OSSafeReleaseNULL(clocks);
        IOService::free();
    }

    IOReturn setProperties(OSObject *object) override
    {
        char process[17] = {};
        proc_selfname(process, sizeof(process));
        if (strcmp(process, "backboardd")) return kIOReturnNotPermitted;
        OSDictionary *properties = OSDynamicCast(OSDictionary, object);
        if (!properties) return kIOReturnBadArgument;
        OSNumber *brightness = OSDynamicCast(OSNumber, properties->getObject("brightness"));
        if (!brightness) return kIOReturnSuccess;
        OSNumber *fade = OSDynamicCast(OSNumber, properties->getObject("FadePeriod"));
        const int32_t requested = int32_t(brightness->unsigned32BitValue());
        const uint32_t target = requested < 0 ? 0 :
            uint32_t(requested) > kBacklightMaximum ? kBacklightMaximum : uint32_t(requested);
        const uint64_t fadeTicks = fade ? fade->unsigned64BitValue() : 0;
        const uint64_t milliseconds = fadeTicks > (5ULL << 16) ? 5000 : (fadeTicks * 1000) >> 16;
        const uint32_t count = __atomic_add_fetch(&requests, 1U, __ATOMIC_RELAXED);
        __atomic_store_n(&pending, (1ULL << 63) | (uint64_t(target) << 16) | milliseconds, __ATOMIC_RELEASE);
        thread_call_enter(worker);
        if (count <= 16 || !(count % 32))
            printf("BACKLIGHT request=%u level=%u fade_ms=%llu\n", count, target, milliseconds);
        return kIOReturnSuccess;
    }
};

// Standard XNU registration, with the same AppleARMBacklight conformance
// used by the upstream service, expressed directly in getMetaClass().
In2020Backlight::MetaClass In2020Backlight::gMetaClass;
const OSMetaClass * const In2020Backlight::metaClass = &In2020Backlight::gMetaClass;
const OSMetaClass * const In2020Backlight::superClass = &IOService::gMetaClass;
In2020Backlight::In2020Backlight(const OSMetaClass *meta) : IOService(meta) {}
In2020Backlight::~In2020Backlight() {}
const OSMetaClass *In2020Backlight::getMetaClass() const
{
    return backlightAppleClass ? backlightAppleClass : &gMetaClass;
}
OSMetaClassConstructorInit(In2020Backlight, IOService, )
OSDefineDefaultStructors(In2020Backlight, IOService)

void startIn2020Backlight()
{
    const OSSymbol *name = OSSymbol::withCStringNoCopy("AppleARMBacklight");
    const OSMetaClass *apple = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (!apple) {
        printf("BACKLIGHT unavailable=AppleARMBacklight-metaclass\n");
        return;
    }
    IOService *provider = (IOService *)IOService::getPlatform();
    In2020Backlight *service = new In2020Backlight;
    if (!provider || !service || !service->init() || !service->attach(provider) || !service->start(provider)) {
        if (service) {
            if (service->getProvider()) service->detach(provider);
            service->release();
        }
        printf("BACKLIGHT unavailable=service-start\n");
        return;
    }
    backlightAppleClass = apple;
    // The built-in service remains attached for the lifetime of this kernel.
    service->release();
}
}
