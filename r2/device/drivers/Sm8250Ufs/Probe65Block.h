// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "ReadCommandBudget.h"
#include "SmGateTrace.h"
#include <kern/clock.h>
#include <kern/thread.h>

static IOReturn probe65Return(SmUfsBlockRead::Error value)
{
	using E = SmUfsBlockRead::Error;
	switch (value) {
	case E::Success: return kIOReturnSuccess;
	case E::Argument: return kIOReturnBadArgument;
	case E::WriteProtected: return kIOReturnNotWritable;
	case E::NotReady: return kIOReturnNotReady;
	case E::NoMemory: return kIOReturnNoMemory;
	case E::Timeout: return kIOReturnTimeout;
	case E::Io: return kIOReturnIOError;
	case E::Unsupported: return kIOReturnUnsupported;
	}
	return kIOReturnIOError;
}

class Probe65Context : public OSObject {
	OSDeclareDefaultStructors(Probe65Context)
public:
	bool initialize() { return OSObject::init(); }
	SmUfsMaps maps{};
	SmUfsDma dma{};
	OSSharedPtr<IOBufferMemoryDescriptor> memory;
	SmUfsOwned::State owned;
	Probe59Port port{};
	SmUfsProbe59::Transport transport;
	SmUfsProbe59::Result last;
	IOLock *gate = nullptr;
	uint64_t blocks = 0, requestDeadline = 0;
	bool online = false, stopping = false;
	const uint32_t *cancelled = nullptr;
	uint32_t accepted = 0, rejected = 0, completions = 0, submits = 0;
	uint32_t active = 0, maxActive = 0, inflight = 0;
	uint32_t providerRequests = 0, maxProviderBlocks = 0, lastProviderBlocks = 0;
	uint32_t deniedMutations = 0;
	SmUfsReadBudget::State budget;
	uint64_t capacity() const { return blocks; }
	void reject() { __atomic_fetch_add(&rejected, 1, __ATOMIC_RELAXED); }
	uint8_t *allocate(uint64_t bytes) { return static_cast<uint8_t *>(IOMalloc(vm_size_t(bytes))); }
	void free(uint8_t *p, uint64_t bytes) { IOFree(p, vm_size_t(bytes)); }
	void lock(uint32_t stage = 1) {
		const uint32_t tid = uint32_t(thread_tid(current_thread()));
		smgate_request(uint64_t(this), tid, mach_absolute_time(), stage);
		IOLockLock(gate);
		smgate_acquired(uint64_t(this), tid, mach_absolute_time());
		++active; if (active > maxActive) maxActive = active;
	}
	void unlock() {
		--active;
		smgate_released(uint64_t(this), uint32_t(thread_tid(current_thread())));
		IOLockUnlock(gate);
	}
	void stage(uint32_t value) { smgate_stage(value); }
	bool ready() const { return online && !stopping && !transport.poisoned &&
	                           (!cancelled || !__atomic_load_n(cancelled, __ATOMIC_ACQUIRE)); }
	void accept(uint64_t count) {
		__atomic_fetch_add(&accepted, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&inflight, 1, __ATOMIC_ACQ_REL);
		++providerRequests; lastProviderBlocks = uint32_t(count);
		if (lastProviderBlocks > maxProviderBlocks) maxProviderBlocks = lastProviderBlocks;
	}
	void completed() { __atomic_fetch_add(&completions, 1, __ATOMIC_RELAXED); }
	void retired() { __atomic_fetch_sub(&inflight, 1, __ATOMIC_RELEASE); }
	void begin() { clock_interval_to_deadline(1000000, kMicrosecondScale, &requestDeadline); }
	bool expired() const { return mach_absolute_time() >= requestDeadline; }
	void close() { online = false; }
	SmUfsBlockRead::Error read(uint32_t lba, uint8_t *out) {
		if (!ready()) return SmUfsBlockRead::Error::NotReady;
		if (!budget.allow(mach_absolute_time())) {
			last = SmUfsProbe59::Result{};
			last.status = SmUfsProbe59::Status::Timeout;
			last.error = budget.stop == SmUfsReadBudget::Stop::Count ? 11 : 12;
			online = false;
			return SmUfsBlockRead::Error::Timeout;
		}
		smgate_stage(4); // entering transport.submit; the issued state is not claimed here
		last = transport.submit(port, SmUfsProbe59::Command::ReadBlock, lba);
		if (last.issued) { ++submits; budget.submitted(); }
		smgate_stage(5); // command returned
		if (!last.valid) return last.status == SmUfsProbe59::Status::Timeout ?
		                        SmUfsBlockRead::Error::Timeout : SmUfsBlockRead::Error::Io;
		const volatile uint8_t *in = port.bytes + SmUfsProbe59::kData;
		for (unsigned i = 0; i < 4096; i++) out[i] = in[i];
		return SmUfsBlockRead::Error::Success;
	}
};
OSDefineMetaClassAndStructors(Probe65Context, OSObject)

struct Probe65Buffer {
	IOMemoryDescriptor *descriptor;
	bool present() const { return descriptor != nullptr; }
	bool inputOnly() const { return descriptor->getDirection() == kIODirectionIn; }
	uint64_t length() const { return descriptor->getLength(); }
	uint64_t copy(const uint8_t *source, uint64_t bytes) { return descriptor->writeBytes(0, source, IOByteCount(bytes)); }
};
struct Probe65Completion {
	IOStorageCompletion *completion;
	bool present() const { return completion && completion->action; }
	void finish(SmUfsBlockRead::Error status, uint64_t bytes) {
		const IOStorageCompletion callback = *completion;
		callback.action(callback.target, callback.parameter, probe65Return(status), bytes);
	}
};

class Sm8250UfsBlock : public IOBlockStorageDevice {
	OSDeclareDefaultStructors(Sm8250UfsBlock)
	Probe65Context *context = nullptr;
public:
	static const OSMetaClass *parentMeta() { return superClass; }
	bool configure(Probe65Context *owner) {
		if (!owner || !owner->gate || !owner->blocks) return false;
		context = owner; context->retain(); return true;
	}
	void free() override { OSSafeReleaseNULL(context); IOBlockStorageDevice::free(); }
	IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
	IOReturn doFormatMedia(UInt64) override {
		if (context) __atomic_fetch_add(&context->deniedMutations, 1, __ATOMIC_RELAXED);
		return kIOReturnNotWritable;
	}
	UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
	char *getVendorString() override { return const_cast<char *>("Qualcomm"); }
	char *getProductString() override { return const_cast<char *>("SM8250 readonly LUN0 test"); }
	char *getRevisionString() override { return const_cast<char *>("v18"); }
	char *getAdditionalDeviceInfoString() override { return const_cast<char *>("Unregistered probe65 adapter"); }
	IOReturn reportBlockSize(UInt64 *out) override {
		if (!out) return kIOReturnBadArgument; *out = 4096; return kIOReturnSuccess;
	}
	IOReturn reportMaxValidBlock(UInt64 *out) override {
		if (!out) return kIOReturnBadArgument; *out = context && context->blocks ? context->blocks - 1 : 0;
		return context && context->blocks ? kIOReturnSuccess : kIOReturnNotReady;
	}
	IOReturn reportMediaState(bool *present, bool *changed) override {
		if (!present) return kIOReturnBadArgument;
		*present = false; if (changed) *changed = false;
		if (!context) return kIOReturnNotReady;
		context->lock(); *present = context->ready(); context->unlock(); return kIOReturnSuccess;
	}
	IOReturn reportEjectability(bool *out) override {
		if (!out) return kIOReturnBadArgument; *out = false; return kIOReturnSuccess;
	}
	IOReturn reportRemovability(bool *out) override { return reportEjectability(out); }
	IOReturn reportWriteProtection(bool *out) override {
		if (!out) return kIOReturnBadArgument; *out = true; return kIOReturnSuccess;
	}
	IOReturn doSynchronizeCache() override {
		if (!context) return kIOReturnNotReady;
		context->lock(); const bool ready = context->ready(); context->unlock();
		return ready ? kIOReturnSuccess : kIOReturnNotReady;
	}
	IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache(); }
	IOReturn getWriteCacheState(bool *out) override {
		if (!out) return kIOReturnBadArgument; *out = false; return kIOReturnSuccess;
	}
	IOReturn setWriteCacheState(bool) override {
		if (context) __atomic_fetch_add(&context->deniedMutations, 1, __ATOMIC_RELAXED);
		return kIOReturnNotWritable;
	}
	IOReturn doUnmap(IOBlockStorageDeviceExtent *, UInt32, IOStorageUnmapOptions) override {
		if (context) __atomic_fetch_add(&context->deniedMutations, 1, __ATOMIC_RELAXED);
		return kIOReturnNotWritable;
	}
	IOReturn doAsyncReadWrite(IOMemoryDescriptor *descriptor, UInt64 block, UInt64 count,
	                         IOStorageAttributes *attributes, IOStorageCompletion *completion) override {
		if (!context) return kIOReturnNotReady;
		Probe65Buffer buffer{descriptor}; Probe65Completion callback{completion};
		SmUfsBlockRead::Request request{block, count};
		if (attributes) request.attributes = {attributes->options, attributes->reserved0024,
		                                     attributes->reserved0032, attributes->reserved0064};
		return probe65Return(SmUfsBlockRead::execute(*context, buffer, request, callback));
	}
};
OSDefineMetaClassAndStructors(Sm8250UfsBlock, IOBlockStorageDevice)

// Separate compilation boundary keeps the hardware tests on the base virtual ABI.
__attribute__((noinline)) static IOReturn probe65Invoke(IOBlockStorageDevice *device, IOMemoryDescriptor *buffer,
                                                      UInt64 block, UInt64 count, IOStorageCompletion *completion)
{
	IOStorageAttributes attributes{};
	return device->doAsyncReadWrite(buffer, block, count, &attributes, completion);
}
