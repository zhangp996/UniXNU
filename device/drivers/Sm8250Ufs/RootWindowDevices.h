// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "RootWindowIo.h"
#include "RootOwnerPolicy.h"
#include "RootSystemPolicy.h"

// Capture shared request state while the gate is held. Emit before the
// completion callback and after unlocking so USB output cannot hold the gate.
struct RootWindowFailureObserver {
	const char *window;
	uint64_t block, count;
	const char *source = nullptr;
	uint64_t lba = 0;
	bool attempted = false, hasTransportState = false;
	unsigned status = 0, budgetStop = 0, submits = 0;
	unsigned transport = 0, issued = 0, complete = 0, poisoned = 0;
	unsigned dbr = 0, isBefore = 0, isAfter = 0, hcs = 0, rsr = 0, ocs = 0, sense = 0;

	void capture(const Probe65Context &context, SmUfsBlockRead::Error error,
	             uint64_t physicalLba, bool sawPhysical, bool requestExpired)
	{
		static bool reported;
		if (__atomic_exchange_n(&reported, true, __ATOMIC_RELAXED)) return;
		lba = physicalLba;
		attempted = sawPhysical;
		using Error = SmUfsBlockRead::Error;
		status = unsigned(error == Error::Success ? Error::Io : error);
		submits = context.submits;
		poisoned = context.transport.poisoned;
		budgetStop = unsigned(context.budget.stop);
		if (requestExpired) source = "request-deadline";
		else if (error == Error::Success) source = "policy";
		else if (budgetStop && error == Error::Timeout && !context.last.issued) source = "budget";
		else if (error == Error::Timeout || error == Error::Io) {
			source = "transport";
			hasTransportState = true;
			const auto &result = context.last;
			transport = unsigned(result.status);
			issued = result.issued;
			complete = result.complete;
			dbr = result.dbr;
			isBefore = result.isBefore;
			isAfter = result.isAfter;
			hcs = result.hcs;
			rsr = result.rsr;
			ocs = result.ocs;
			sense = result.sense;
		} else source = "physical";
	}

	__attribute__((noinline)) void emit() const
	{
		if (!source) return;
		kprintf("UFSFAIL34 window=%s src=%s io=%u block=%llu count=%llu physical=%u lba=%llu submits=%u budget=%u poison=%u\n",
		      window, source, status,
		      static_cast<unsigned long long>(block), static_cast<unsigned long long>(count),
		      static_cast<unsigned>(attempted),
		      static_cast<unsigned long long>(lba), submits, budgetStop, poisoned);
		if (hasTransportState)
			kprintf("UFSFAIL34 transport=%u issued=%u complete=%u dbr=%x is=%x/%x hcs=%x rsr=%x ocs=%x sense=%x\n",
			      transport, issued, complete, dbr, isBefore, isAfter, hcs, rsr, ocs, sense);
	}
};


// Native storage subclasses retain the established API/ABI; only the virtual
// geometry and the bounded 512-to-4096 read mapping differ from the raw adapter.
class RootWindowBlock : public Sm8250UfsBlock {
	OSDeclareDefaultStructors(RootWindowBlock)
	Probe65Context *owner = nullptr; // Retained by the configured base object.
	SmUfsRoot::Layout layout;
	SmUfsRoot::Window window;
	bool ownerVerified = false;
public:
	static const OSMetaClass *parentMeta() { return superClass; }
	bool configureWindow(Probe65Context *context, const SmUfsRoot::Layout &l, const SmUfsRoot::Window &w)
	{
		uint64_t absolute = 0;
		if (!context || context->capacity() > ~uint64_t(0) / 4096 || !l.valid ||
		    !w.size || w.size % 512 || w.blockBytes != 512 || w.flags != 1 ||
		    w.offset > l.imageBytes || w.size > l.imageBytes - w.offset ||
		    !SmUfsRoot::mulAddPhysical(l.firstLba, w.offset, 0, &absolute) ||
		    absolute / 4096 >= context->capacity() ||
		    w.size > context->capacity() * 4096 - absolute || !Sm8250UfsBlock::configure(context)) return false;
		if (SmUfsSystemPolicy::requested() && !strcmp(w.name,"SystemOS") && (!SmUfsSystemPolicy::ready() || !SmUfsSystemRead::windowValid(w))) return false;
		owner = context; layout = l; window = w;
		ownerVerified = SmUfsOwnerPolicy::requested() && !strcmp(w.name, "OS");
		if (ownerVerified && !SmUfsOwnerPolicy::ready()) return false;
		return true;
	}
	bool cacheProbeEligible()
	{
		if (SmUfsSystemPolicy::requested() && !SmUfsSystemPolicy::ready()) return false;
		if (!owner || isInactive() || strcmp(window.name,"SystemOS") || window.flags!=1 || window.blockBytes!=512 || window.size!=3303014400ULL || window.offset!=0x16a600000ULL) return false;
		owner->lock(); const bool live=owner->ready(); owner->unlock(); return live;
	}
	bool ownerAuditEligible()
	{
		if (!ownerVerified || !owner || isInactive() || !SmUfsOwnerPolicy::ready()) return false;
		owner->lock(); const bool live = owner->ready(); owner->unlock(); return live;
	}
	IOReturn reportBlockSize(UInt64 *out) override
	{
		if (!out) return kIOReturnBadArgument; *out = 512; return kIOReturnSuccess;
	}
	IOReturn reportMaxValidBlock(UInt64 *out) override
	{
		if (!out) return kIOReturnBadArgument;
		*out = window.size ? window.size / 512 - 1 : 0;
		return owner && window.size ? kIOReturnSuccess : kIOReturnNotReady;
	}
	char *getProductString() override { return const_cast<char *>("SHNGROOT readonly window"); }
	char *getAdditionalDeviceInfoString() override { return window.name; }
	IOReturn doAsyncReadWrite(IOMemoryDescriptor *descriptor, UInt64 block, UInt64 count,
	                         IOStorageAttributes *attributes, IOStorageCompletion *completion) override
	{
		if (!owner) return kIOReturnNotReady;
		Probe65Buffer buffer{descriptor}; Probe65Completion callback{completion};
		SmUfsBlockRead::Request request{block, count};
		if (attributes) request.attributes = {attributes->options, attributes->reserved0024,
		                                     attributes->reserved0032, attributes->reserved0064};
		RootWindowFailureObserver observer{window.name, block, count};
		if (SmUfsSystemPolicy::requested() && !strcmp(window.name,"SystemOS")) {
			auto reader = SmUfsSystemPolicy::reader();
			return probe65Return(SmUfsRootWindowIo::executeChecked(*owner, buffer, layout, window, request, callback, reader, observer));
		}
		if (ownerVerified) {
			auto reader = SmUfsOwnerPolicy::reader();
			return probe65Return(SmUfsRootWindowIo::executeChecked(*owner, buffer, layout, window, request, callback, reader, observer));
		}
		SmUfsOwnerRead::PlainReader reader;
		return probe65Return(SmUfsRootWindowIo::executeChecked(*owner, buffer, layout, window, request, callback, reader, observer));
	}
};
OSDefineMetaClassAndStructors(RootWindowBlock, Sm8250UfsBlock)

class RootWindowDriver : public RootWholeDriver {
	OSDeclareDefaultStructors(RootWindowDriver)
public:
	RootWholeMedia *media = nullptr; // Independent retain, separate from native ownership.
	static const OSMetaClass *parentMeta() { return superClass; }
	IOMedia *instantiateDesiredMediaObject() override
	{
		if (media) return nullptr;
		media = new RootWholeMedia;
		if (media) media->retain();
		return media;
	}
};
OSDefineMetaClassAndStructors(RootWindowDriver, RootWholeDriver)

namespace SmUfsRootWindows {
struct Entry {
	RootWindowBlock *device = nullptr;
	RootWindowDriver *driver = nullptr;
	bool deviceAttached = false, driverAttached = false, startAttempted = false;
	bool started = false, registered = false, geometry = false;
	uint64_t mediaID = 0, driverID = 0;
};
static Entry entries[2]; // SystemOS, then OS: publish the root candidate last.
static const char *reason = "not-run";
static bool enabled()
{
	uint32_t value = 0;
	return PE_parse_boot_argn("native-ufs-windows", &value, sizeof(value)) && value;
}
static bool registered(IOService *service)
{
	if (!service) return false;
	const IOOptionBits state = service->getState();
	return (state & kIOServiceRegisteredState) && !(state & kIOServiceInactiveState);
}

// Caller has already closed shared admission and drained inflight, while holding
// blockStartupGate. Timeout keeps every reference in this entry for later proof.
static bool stopEntry(Entry &e)
{
	if (e.startAttempted) {
		if (!e.device->isInactive() && !e.device->terminate(kIOServiceRequired)) return false;
		uint64_t deadline; clock_interval_to_deadline(5000, kMillisecondScale, &deadline);
		for (;;) {
			RootWholeMedia *m = e.driver->media;
			const bool driverDone = __atomic_load_n(&e.driver->stopCompleted, __ATOMIC_ACQUIRE) && !e.driver->getProvider();
			const bool mediaDone = !m || (!m->getProvider() &&
			    (!m->attachedOnce || __atomic_load_n(&m->stopCompleted, __ATOMIC_ACQUIRE)));
			if (driverDone && mediaDone && !e.device->getProvider()) break;
			if (mach_absolute_time() >= deadline) return false;
			IOSleep(1);
		}
	} else {
		if (e.driverAttached) e.driver->detach(e.device);
		if (e.deviceAttached) e.device->detach(e.device->getProvider());
	}
	if (e.driver && e.driver->media) { e.driver->media->release(); e.driver->media = nullptr; }
	if (e.driver) { e.driver->release(); e.driver = nullptr; }
	if (e.device) { e.device->release(); e.device = nullptr; }
	e = {};
	return true;
}
static bool stop()
{
	if (!stopEntry(entries[1])) return false;
	return stopEntry(entries[0]);
}

static bool fail(Entry &entry, const char *why)
{
	reason = why;
	// Unpublished, never-started objects have no native asynchronous lifecycle.
	// A start attempt may have initialized PM/media even if it returned false.
	if (!entry.startAttempted) (void)stopEntry(entry);
	return false;
}

static bool publish(IOService *controller, Probe65Context &context, const SmUfsRoot::Layout &layout)
{
	if (!SmUfsOwnerPolicy::initialize())
		panic("SMUFS root-h3c-policy rejected status=%u immutable=%u self=%u trust=%u", SmUfsOwnerPolicy::signatureStatus, SmUfsOwnerPolicy::immutable, SmUfsOwnerPolicy::counts.selftests, SmUfsOwnerPolicy::trustRequested());
	if (!SmUfsSystemPolicy::initialize()) panic("SMUFS cache-h3k policy initialization refused");
	const char *names[2] = {"SystemOS", "OS"};
	for (unsigned i = 0; i < 2; ++i) {
		Entry &e = entries[i];
		context.lock(); const bool ready = context.ready(); context.unlock();
		const SmUfsRoot::Window *w = SmUfsRoot::find(layout, names[i]);
		if (!ready || !w || e.device || e.driver) return fail(e, "window-state");
		e.device = new RootWindowBlock;
		if (!e.device || !e.device->init(nullptr) || !e.device->configureWindow(&context, layout, *w)) {
			return fail(e, "window-device");
		}
		if (!e.device->setProperty(kIOMaximumBlockCountReadKey, 128ULL, 64) ||
		    !e.device->setProperty(kIOMaximumByteCountReadKey, 65536ULL, 64)) return fail(e, "window-limits");
		e.driver = new RootWindowDriver;
		if (!e.driver || !e.driver->init(nullptr)) return fail(e, "window-driver");
		e.deviceAttached = e.device->attach(controller);
		if (!e.deviceAttached) return fail(e, "window-attach-device");
		e.driverAttached = e.driver->attach(e.device);
		if (!e.driverAttached) return fail(e, "window-attach-driver");
		e.startAttempted = true;
		e.started = e.driver->start(e.device);
		if (!e.started || !e.driver->media) return fail(e, "window-start");
		uint64_t deadline; clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
		while (!registered(e.driver) || !registered(e.driver->media)) {
			context.lock(); const bool stillReady = context.ready(); context.unlock();
			if (!stillReady || mach_absolute_time() >= deadline) return fail(e, "window-registration");
			IOSleep(1);
		}
		e.registered = true;
		e.mediaID = e.driver->media->getRegistryEntryID(); e.driverID = e.driver->getRegistryEntryID();
		e.geometry = e.driver->media->getPreferredBlockSize() == 512 && e.driver->media->getSize() == w->size &&
		             e.driver->media->isWhole() && !e.driver->media->isWritable();
		if (!e.geometry) return fail(e, "window-geometry");
	}
	reason = "windows-published";
	return true;
}
} // namespace SmUfsRootWindows
