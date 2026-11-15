// SPDX-License-Identifier: BSD-2-Clause-Patent
// Minimal panel-brightness service for the in2020 board, following the reference AppleARMBacklight
// recipe in docs/porting/display (steps 3-5): the board class keeps its own name but answers
// getMetaClass() with AppleARMPlatform's AppleARMBacklight metaclass, publishes backlight-control and
// IODisplayParameters.brightness, and accepts the brightness requests.
//
// The reference comments record why the service matters beyond brightness: CoreBrightness only takes a
// backlight for the built-in display when IOObjectConformsTo(service, "AppleARMBacklight") holds, and
// only then starts the libEDR server that QuartzCore's IOMFBServer sends to synchronously.  Publishing
// backlight-control without that conformance made the display server hold its lock and the screen stop
// updating (their int-236).  This board still has no working backlight transport, so the level is kept
// in software; the service and its properties are what the display side needs.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>

#define BL_MAX_LEVEL 0x7ffu
#define BL_MIN_NITS  2
#define BL_MID_NITS  160
#define BL_MAX_NITS  800

class OppoBacklight : public IOService
{
	OSDeclareDefaultStructors(OppoBacklight)

	static const OSMetaClass *gAppleClass;
	static bool gImpersonating;
	uint32_t level;

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;

	// AppleARMPlatform already defines AppleARMBacklight, so this class keeps its own name and answers
	// with that metaclass instead: IOObjectConformsTo and the sandbox both walk getMetaClass.  This
	// kernel is arm64 (not arm64e), where the reference copies the vtable and points slot 7 at its own
	// function; on arm64e a copied vtable would not authenticate and the flag path is used instead.

	bool impersonate()
	{
		const OSSymbol *name = OSSymbol::withCString("AppleARMBacklight");
		gAppleClass = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
		OSSafeReleaseNULL(name);
		if (!gAppleClass) {
			printf("BACKLIGHT: no AppleARMBacklight metaclass in this kernel\n");
			return false;
		}
		__atomic_store_n(&gImpersonating, true, __ATOMIC_RELEASE);
#if __has_feature(ptrauth_calls)
		return true;
#else
		enum { kSlots = 320, kGetMetaClass = 7 };
		uintptr_t *copy = static_cast<uintptr_t *>(IOMalloc(kSlots * sizeof(uintptr_t)));

		if (!copy) {
			return false;
		}
		memcpy(copy, *reinterpret_cast<uintptr_t **>(this), kSlots * sizeof(uintptr_t));
		copy[kGetMetaClass] = reinterpret_cast<uintptr_t>(&OppoBacklight::appleMetaClass);
		__asm__ volatile("dmb ish" ::: "memory");
		*reinterpret_cast<uintptr_t **>(this) = copy;
		return true;
#endif
	}

	static const OSMetaClass *appleMetaClass(const OSMetaClassBase *)
	{
		return gAppleClass;
	}

	void publishLevel(uint32_t value)
	{
		OSDictionary *brightness = OSDictionary::withCapacity(3);
		OSDictionary *milliNits = OSDictionary::withCapacity(3);
		OSDictionary *parameters = OSDictionary::withCapacity(2);
		OSNumber *min = OSNumber::withNumber(0ULL, 32);
		OSNumber *max = OSNumber::withNumber((uint64_t)BL_MAX_LEVEL, 32);
		OSNumber *current = OSNumber::withNumber((uint64_t)value, 32);
		OSNumber *minMilli = OSNumber::withNumber((uint64_t)BL_MIN_NITS * 1000, 32);
		OSNumber *maxMilli = OSNumber::withNumber((uint64_t)BL_MAX_NITS * 1000, 32);
		OSNumber *currentMilli = OSNumber::withNumber((uint64_t)value * BL_MAX_NITS * 1000 / BL_MAX_LEVEL, 32);

		if (brightness && milliNits && parameters && min && max && current && minMilli && maxMilli && currentMilli) {
			brightness->setObject("min", min);
			brightness->setObject("max", max);
			brightness->setObject("value", current);
			milliNits->setObject("min", minMilli);
			milliNits->setObject("max", maxMilli);
			milliNits->setObject("value", currentMilli);
			parameters->setObject("brightness", brightness);
			parameters->setObject("BrightnessMilliNits", milliNits);
			setProperty("IODisplayParameters", parameters);
		}
		OSSafeReleaseNULL(min);
		OSSafeReleaseNULL(max);
		OSSafeReleaseNULL(current);
		OSSafeReleaseNULL(minMilli);
		OSSafeReleaseNULL(maxMilli);
		OSSafeReleaseNULL(currentMilli);
		OSSafeReleaseNULL(brightness);
		OSSafeReleaseNULL(milliNits);
		OSSafeReleaseNULL(parameters);
	}

	void publishCalibration()
	{
		const int32_t table[] = {
			(int32_t)(BL_MIN_NITS << 16), (int32_t)(BL_MID_NITS << 16), (int32_t)(BL_MAX_NITS << 16)
		};
		if (OSData *marketing = OSData::withBytes(table, sizeof(table))) {
			setProperty("backlight-marketing-table", marketing);
			marketing->release();
		}
		OSDictionary *calibration = OSDictionary::withCapacity(6);
		if (!calibration) {
			return;
		}
		const struct { const char *key; int nits; } fields[] = {
			{ "LminProduct", BL_MIN_NITS }, { "LmidProduct", BL_MID_NITS }, { "LmaxProduct", BL_MAX_NITS },
			{ "LminPanel", BL_MIN_NITS }, { "LmidPanel", BL_MID_NITS }, { "LmaxPanel", BL_MAX_NITS },
		};
		for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
			OSNumber *number = OSNumber::withNumber((uint64_t)fields[i].nits << 16, 32);
			if (number) {
				calibration->setObject(fields[i].key, number);
				number->release();
			}
		}
		setProperty("backlight-calibration-parameters", calibration);
		calibration->release();
	}
};

const OSMetaClass *OppoBacklight::gAppleClass = nullptr;
bool OppoBacklight::gImpersonating = false;

#define super IOService
OSDefineMetaClassAndStructors(OppoBacklight, IOService)
#undef super

bool
OppoBacklight::start(IOService *provider)
{
	uint32_t control = 1;

	if (!IOService::start(provider)) {
		return false;
	}
	level = BL_MAX_LEVEL / 2;
	PE_parse_boot_argn("backlight-control", &control, sizeof(control));
	if (control == 1 && impersonate()) {
		setProperty("NativeSandboxGrantProcess", "backboardd");
		setProperty("backlight-control", kOSBooleanTrue);
	}
	publishCalibration();
	publishLevel(level);
	registerService();
	printf("BACKLIGHT: AppleARMBacklight published, control=%u conforms=%d\n",
	    control, (int)impersonate());
	return true;
}

extern "C" void
OppoBacklightPublish(IOService *provider)
{
	static bool published;
	uint32_t enabled = 0;

	if (published || provider == nullptr) {
		return;
	}
	if (!PE_parse_boot_argn("backlight", &enabled, sizeof(enabled)) || enabled != 1) {
		return;
	}
	OppoBacklight *backlight = OSTypeAlloc(OppoBacklight);

	published = true;
	if (backlight == nullptr || !backlight->init(nullptr)) {
		OSSafeReleaseNULL(backlight);
		return;
	}
	if (!backlight->attach(provider) || !backlight->start(provider)) {
		backlight->detach(provider);
		backlight->release();
		return;
	}
	printf("BACKLIGHT: service attached to the resource service\n");
}
