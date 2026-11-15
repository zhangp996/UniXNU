// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <IOKit/IOKitKeys.h>
#include <IOKit/storage/IOBlockStorageDriver.h>
#include <IOKit/storage/IOMedia.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSBoolean.h>

// Terminal-only qualification: every object, descriptor and callback stays alive
// until the terminal panic, including timeouts and partially completed startup.
struct Probe66Report {
	bool abi = false, limits = false, constructed = false, attached = false, started = false;
	bool media = false, readerOpen = false, writerDenied = false, gpt = false, userdata = false;
	bool aligned = false, unaligned = false, crossBlock = false, read16 = false, read17 = false;
	bool direct17Reject = false, lastBlock = false, lastByte = false, reenter = false;
	bool negatives = false, isolated = false, closed = false, callbacksStable = false, all = false;
	uint32_t stage = 0, negativeCases = 0, driverRegistrations = 0, mediaRegistrations = 0;
	uint32_t driverOptions = 0, mediaOptions = 0;
	uint32_t serviceClassSize = 0, blockClassSize = 0, storageClassSize = 0;
	uint32_t driverClassSize = 0, mediaClassSize = 0;
	uint32_t gptCrc[3] = {}, userdataCrc = 0;
	uint32_t requests16 = 0, requests17 = 0, last17 = 0, maxProviderBlocks = 0, callbackCount = 0;
	const char *reason = "not-run";
};
static Probe66Report gProbe66Report;

class Probe66Media : public IOMedia {
	OSDeclareDefaultStructors(Probe66Media)
public:
	static const OSMetaClass *parentMeta() { return superClass; }
	void registerService(IOOptionBits options = 0) override {
		++gProbe66Report.mediaRegistrations;
		gProbe66Report.mediaOptions |= options;
	}
};
OSDefineMetaClassAndStructors(Probe66Media, IOMedia)

static Sm8250UfsBlock *gProbe66Device = nullptr;
static IOBlockStorageDevice *probe66Device() { return gProbe66Device; }
static Probe66Media *gProbe66Media = nullptr;
class Probe66Driver : public IOBlockStorageDriver {
	OSDeclareDefaultStructors(Probe66Driver)
public:
	static const OSMetaClass *parentMeta() { return superClass; }
	IOMedia *instantiateDesiredMediaObject() override {
		if (gProbe66Media) return nullptr;
		Probe66Media *media = new Probe66Media;
		if (media) {
			media->retain(); // Independent hold even if original init/attach fails.
			gProbe66Media = media;
		}
		return media; // Original instantiateMediaObject owns init, not this factory.
	}
	void registerService(IOOptionBits options = 0) override {
		++gProbe66Report.driverRegistrations;
		gProbe66Report.driverOptions |= options;
	}
};
OSDefineMetaClassAndStructors(Probe66Driver, IOBlockStorageDriver)
static Probe66Driver *gProbe66Driver = nullptr;
static IOService *gProbe66Client = nullptr;
static bool gProbe66ClientAttached = false;
static uint32_t gProbe66CallbackActive = 0, gProbe66DuplicateCallbacks = 0;

struct Probe66Call {
	OSSharedPtr<IOBufferMemoryDescriptor> buffer;
	IOStorageAttributes attributes{};
	IOStorageCompletion completion{};
	UInt64 offset = 0, bytes = 0, actual = 0;
	IOReturn status = kIOReturnError, returned = kIOReturnError;
	uint32_t callbacks = 0, finished = 0;
	bool issued = false, expectsCallback = true;
	Probe66Call *nested = nullptr;
};
enum Probe66CallIndex {
	P66Ref16, P66RefTail, P66RefUser, P66RefLast, P66Gpt, P66User,
	P66Aligned, P66Unaligned, P66Cross, P66Read16, P66Read17, P66Last,
	P66LastByte, P66Outer, P66Inner, P66Direct17, P66BadEnd, P66BadAfter,
	P66BadCross, P66BadOverflow, P66BadOut, P66BadInOut, P66BadWrite,
	P66BadNull, P66BadFua, P66BadEncrypted, P66BadReserved24, P66BadReserved32,
	P66BadReserved64, P66CallCount
};
static Probe66Call gProbe66Calls[P66CallCount];

// Base-pointer calls deliberately exercise the native storage virtual ABI.
__attribute__((noinline)) static void probe66Issue(Probe66Call &call, bool write = false)
{
	call.issued = true;
	IOStorage *storage = gProbe66Media;
	if (write) storage->write(gProbe66Client, call.offset, call.buffer.get(), &call.attributes, &call.completion);
	else storage->read(gProbe66Client, call.offset, call.buffer.get(), &call.attributes, &call.completion);
}
static void probe66Callback(void *, void *parameter, IOReturn status, UInt64 actual)
{
	__atomic_fetch_add(&gProbe66CallbackActive, 1, __ATOMIC_ACQ_REL);
	Probe66Call *call = static_cast<Probe66Call *>(parameter);
	const uint32_t previous = __atomic_fetch_add(&call->callbacks, 1, __ATOMIC_ACQ_REL);
	if (previous == 0) {
		call->status = status; call->actual = actual;
		if (call->nested) probe66Issue(*call->nested);
		__atomic_store_n(&call->finished, 1, __ATOMIC_RELEASE);
	} else {
		__atomic_fetch_add(&gProbe66DuplicateCallbacks, 1, __ATOMIC_RELEASE);
	}
	__atomic_fetch_sub(&gProbe66CallbackActive, 1, __ATOMIC_RELEASE);
}
static bool probe66Buffer(Probe66Call &call, UInt64 bytes, UInt64 offset = 0,
                          IODirection direction = kIODirectionIn)
{
	call.bytes = bytes; call.offset = offset;
	call.completion = IOStorageCompletion{nullptr, probe66Callback, &call};
	call.buffer = OSSharedPtr<IOBufferMemoryDescriptor>(
	    IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, direction, vm_size_t(bytes), 4096), OSNoRetain);
	return bool(call.buffer);
}
static bool probe66Wait(Probe65Context &context, const Probe66Call *call = nullptr)
{
	uint64_t deadline; clock_interval_to_deadline(5000, kMillisecondScale, &deadline);
	while ((call && !__atomic_load_n(&call->finished, __ATOMIC_ACQUIRE)) ||
	       __atomic_load_n(&context.inflight, __ATOMIC_ACQUIRE) ||
	       __atomic_load_n(&gProbe66CallbackActive, __ATOMIC_ACQUIRE)) {
		if (mach_absolute_time() >= deadline)
			panic("SMUFS v19 callback-timeout; persistent resources held, no result snapshot");
		IOSleep(1);
	}
	return true;
}
static bool probe66Passed(const Probe66Call &call)
{
	return __atomic_load_n(&call.finished, __ATOMIC_ACQUIRE) &&
	       __atomic_load_n(&call.callbacks, __ATOMIC_ACQUIRE) == 1 &&
	       call.status == kIOReturnSuccess && call.actual == call.bytes;
}
static bool probe66ReadCall(Probe65Context &context, Probe66Call &call)
{
	probe66Issue(call);
	return probe66Wait(context, &call) && probe66Passed(call);
}
static bool probe66Reference(Probe65Context &context, Probe66Call &call, UInt64 block, UInt64 count,
                             bool nullAttributes = false)
{
	if (!probe66Buffer(call, count * 4096)) return false;
	call.issued = true;
	if (!nullAttributes) {
		call.attributes.options = kIOStorageOptionIsStatic;
		call.attributes.adjustedOffset = 17; // Native advisory field must not change addressing.
	}
	IOBlockStorageDevice *device = gProbe66Device;
	call.returned = device->doAsyncReadWrite(call.buffer.get(), block, count,
	    nullAttributes ? nullptr : &call.attributes, &call.completion);
	return call.returned == kIOReturnSuccess && probe66Wait(context, &call) && probe66Passed(call);
}
static bool probe66Equal(const Probe66Call &call, const Probe66Call &reference,
                         UInt64 referenceOffset, UInt64 bytes, UInt64 callOffset = 0)
{
	if (!call.buffer || !reference.buffer || callOffset > call.buffer->getLength() ||
	    bytes > call.buffer->getLength() - callOffset || referenceOffset > reference.buffer->getLength() ||
	    bytes > reference.buffer->getLength() - referenceOffset) return false;
	const uint8_t *a = static_cast<const uint8_t *>(call.buffer->getBytesNoCopy()) + callOffset;
	const uint8_t *b = static_cast<const uint8_t *>(reference.buffer->getBytesNoCopy()) + referenceOffset;
	for (UInt64 i = 0; i < bytes; i++) if (a[i] != b[i]) return false;
	return true;
}
static bool probe66Unpublished(IOService *service)
{
	return service && !(service->getState() & (kIOServiceRegisteredState | kIOServiceMatchedState));
}
static bool probe66OnlyClient(IOService *service, IOService *expected)
{
	OSSharedPtr<OSIterator> clients(service->getClientIterator(), OSNoRetain);
	if (!clients) return false;
	unsigned count = 0;
	while (OSObject *object = clients->getNextObject()) {
		if (object != expected || ++count > 1) return false;
	}
	return clients->isValid() && count == (expected ? 1u : 0u);
}
static bool probe66Isolation()
{
	return probe66Unpublished(gProbe66Device) && probe66Unpublished(gProbe66Driver) &&
	       probe66Unpublished(gProbe66Media) && probe66Unpublished(gProbe66Client) &&
	       probe66OnlyClient(gProbe66Device, gProbe66Driver) &&
	       probe66OnlyClient(gProbe66Driver, gProbe66Media) &&
	       probe66OnlyClient(gProbe66Media, gProbe66ClientAttached ? gProbe66Client : nullptr) &&
	       gProbe66Report.driverRegistrations == 1 && gProbe66Report.driverOptions == 0 &&
	       gProbe66Report.mediaRegistrations == 1 && gProbe66Report.mediaOptions == kIOServiceAsynchronous;
}
static bool probe66Number(IOService *object, const char *key, UInt64 value)
{
	if (!object->setProperty(key, value, 64)) return false;
	OSSharedPtr<OSObject> property(object->copyProperty(key, gIOServicePlane), OSNoRetain);
	OSNumber *number = OSDynamicCast(OSNumber, property.get());
	return number && number->unsigned64BitValue() == value;
}
static bool probe66MediaNumber(const char *key, UInt64 value)
{
	OSNumber *number = OSDynamicCast(OSNumber, gProbe66Media->getProperty(key));
	return number && number->unsigned64BitValue() == value;
}
static bool probe66MediaBoolean(const char *key, bool value)
{
	OSBoolean *boolean = OSDynamicCast(OSBoolean, gProbe66Media->getProperty(key));
	return boolean && boolean->isTrue() == value;
}
static bool probe66Negative(Probe65Context &context, Probe66Call &call, bool write, IOReturn expected)
{
	const uint32_t submits = context.submits, requests = context.providerRequests;
	probe66Issue(call, write);
	const bool passed = probe66Wait(context, &call) &&
	    __atomic_load_n(&call.callbacks, __ATOMIC_ACQUIRE) == 1 && call.status == expected && call.actual == 0 &&
	    context.submits == submits && context.providerRequests == requests;
	if (passed) ++gProbe66Report.negativeCases;
	return passed;
}
static bool probe66Finalize(Probe65Context &context)
{
	Probe66Report &r = gProbe66Report;
	// Runner may call this after an early failure. Never close an active client.
	if (__atomic_load_n(&context.inflight, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&gProbe66CallbackActive, __ATOMIC_ACQUIRE)) return false;
	for (unsigned i = 0; i < P66CallCount; i++) {
		const Probe66Call &call = gProbe66Calls[i];
		if (call.issued && call.expectsCallback &&
		    !__atomic_load_n(&call.finished, __ATOMIC_ACQUIRE)) return false;
	}
	// Do not change context admission here: Runner owns terminal close/drain.
	if (gProbe66ClientAttached && gProbe66Media->isOpen(gProbe66Client))
		gProbe66Media->close(gProbe66Client);
	r.closed = !gProbe66ClientAttached || !gProbe66Media->isOpen(gProbe66Client);
	bool stable = !__atomic_load_n(&gProbe66DuplicateCallbacks, __ATOMIC_ACQUIRE);
	uint32_t callbacks = 0;
	for (unsigned i = 0; i < P66CallCount; i++) {
		const Probe66Call &call = gProbe66Calls[i];
		const uint32_t count = __atomic_load_n(&call.callbacks, __ATOMIC_ACQUIRE);
		callbacks += count;
		if (call.issued && count != (call.expectsCallback ? 1u : 0u)) stable = false;
	}
	r.callbackCount = callbacks; r.callbacksStable = stable;
	if (!r.closed || !stable) r.all = false;
	return r.closed && stable;
}

static void probe66Tests(NativeUfs &controller, Probe65Context &context, Probe61Reader &reader,
                         const SmUfsGpt::Report &gpt, const SmUfsInventory::Inventory &inventory,
                         const SmUfsInventory::Volumes &volumes)
{
	(void)reader; // Preflight has ended; all subsequent transport goes through context.
	Probe66Report &r = gProbe66Report;
	r.stage = 1; r.reason = "abi";
	const OSMetaClass *block = Sm8250UfsBlock::parentMeta();
	const OSMetaClass *driver = Probe66Driver::parentMeta(), *media = Probe66Media::parentMeta();
	if (!block || !driver || !media) return;
	const OSMetaClass *storage = driver->getSuperClass();
	if (!storage) return;
	r.serviceClassSize = IOService::metaClass->getClassSize();
	r.blockClassSize = block->getClassSize(); r.storageClassSize = storage->getClassSize();
	r.driverClassSize = driver->getClassSize(); r.mediaClassSize = media->getClassSize();
	r.abi = r.serviceClassSize == sizeof(IOService) && r.blockClassSize == sizeof(IOBlockStorageDevice) &&
	    r.storageClassSize == sizeof(IOStorage) && r.driverClassSize == sizeof(IOBlockStorageDriver) &&
	    r.mediaClassSize == sizeof(IOMedia) && !strcmp(block->getClassName(), "IOBlockStorageDevice") &&
	    !strcmp(driver->getClassName(), "IOBlockStorageDriver") && !strcmp(media->getClassName(), "IOMedia") &&
	    !strcmp(storage->getClassName(), "IOStorage") && block->getSuperClass() == IOService::metaClass &&
	    storage->getSuperClass() == IOService::metaClass && media->getSuperClass() == storage;
	if (!r.abi) return;
	r.stage = 2; r.reason = "construct";
	if (gProbe66Device || gProbe66Driver || gProbe66Client || context.blocks < 17 || context.blocks > (~UInt64(0) / 4096)) return;
	gProbe66Device = new Sm8250UfsBlock;
	if (!gProbe66Device || !gProbe66Device->init(nullptr) || !gProbe66Device->configure(&context)) return;
	gProbe66Driver = new Probe66Driver;
	gProbe66Client = new IOService;
	if (!gProbe66Driver || !gProbe66Client || !gProbe66Driver->init(nullptr) || !gProbe66Client->init(nullptr)) return;
	r.constructed = true;
	r.stage = 3; r.reason = "limits";
	r.limits = probe66Number(gProbe66Device, kIOMaximumBlockCountReadKey, 16) &&
	    probe66Number(gProbe66Device, kIOMaximumByteCountReadKey, 65536) &&
	    probe66Number(gProbe66Device, kIOMaximumBlockCountWriteKey, 16) &&
	    probe66Number(gProbe66Device, kIOMaximumByteCountWriteKey, 65536);
	if (!r.limits) return;
	r.stage = 4; r.reason = "attach";
	r.attached = gProbe66Device->attach(&controller) && gProbe66Driver->attach(gProbe66Device);
	if (!r.attached) return;
	r.stage = 5; r.reason = "start";
	r.started = gProbe66Driver->start(gProbe66Device);
	if (!r.started) return;
	r.stage = 6; r.reason = "media";
	const UInt64 mediaBytes = context.blocks * 4096;
	r.media = gProbe66Media && gProbe66Media->getProvider() == gProbe66Driver &&
	    gProbe66Media->isWhole() && gProbe66Media->getBase() == 0 &&
	    gProbe66Media->getPreferredBlockSize() == 4096 && gProbe66Media->getSize() == mediaBytes &&
	    !gProbe66Media->isWritable() && !(gProbe66Media->getAttributes() &
	    (kIOMediaAttributeRemovableMask | kIOMediaAttributeEjectableMask)) &&
	    probe66MediaNumber(kIOMediaPreferredBlockSizeKey, 4096) && probe66MediaNumber(kIOMediaSizeKey, mediaBytes) &&
	    probe66MediaBoolean(kIOMediaWholeKey, true) && probe66MediaBoolean(kIOMediaWritableKey, false) &&
	    probe66MediaBoolean(kIOMediaRemovableKey, false) && probe66MediaBoolean(kIOMediaEjectableKey, false);
	if (!r.media) return;
	r.stage = 7; r.reason = "open";
	gProbe66ClientAttached = gProbe66Client->attach(gProbe66Media);
	if (!gProbe66ClientAttached) return;
	r.readerOpen = gProbe66Media->open(gProbe66Client, 0, kIOStorageAccessReader) && gProbe66Media->isOpen(gProbe66Client);
	if (!r.readerOpen) return;
	r.writerDenied = !gProbe66Media->open(gProbe66Client, 0, kIOStorageAccessReaderWriter) && gProbe66Media->isOpen(gProbe66Client);
	if (!r.writerDenied) return;
	r.stage = 8; r.reason = "isolation";
	r.isolated = probe66Isolation();
	if (!r.isolated) return;
	r.stage = 9; r.reason = "reference";
	Probe66Call &reference = gProbe66Calls[P66Ref16], &tail = gProbe66Calls[P66RefTail];
	Probe66Call &userReference = gProbe66Calls[P66RefUser], &lastReference = gProbe66Calls[P66RefLast];
	if (!probe66Reference(context, reference, 0, 16) || !probe66Reference(context, tail, 16, 1, true) ||
	    !probe66Reference(context, userReference, gpt.userFirst, 1) ||
	    !probe66Reference(context, lastReference, context.blocks - 1, 1)) return;
	r.stage = 10; r.reason = "gpt";
	if (gpt.status != SmUfsGpt::Status::Success || gpt.tableLba != 2 || UInt64(gpt.entryCount) * gpt.entrySize != 4096) return;
	Probe66Call &gptCall = gProbe66Calls[P66Gpt];
	if (!probe66Buffer(gptCall, 12288) || !probe66ReadCall(context, gptCall)) return;
	const uint8_t *bytes = static_cast<const uint8_t *>(gptCall.buffer->getBytesNoCopy());
	for (unsigned i = 0; i < 3; i++) r.gptCrc[i] = SmUfsGpt::crc32(bytes + i * 4096, 4096);
	r.gpt = r.gptCrc[0] == gpt.mbrCrc && r.gptCrc[1] == gpt.headerBlockCrc && r.gptCrc[2] == gpt.tableStored &&
	    probe66Equal(gptCall, reference, 0, 12288);
	if (!r.gpt) return;
	r.stage = 11; r.reason = "userdata";
	bool foundUser = false; uint32_t userCrc = 0;
	for (unsigned i = 0; i < inventory.count; i++) if (SmUfsInventory::named(inventory.entries[i], "userdata")) {
		if (foundUser) return;
		foundUser = true; userCrc = volumes.entries[i].primary.crc;
	}
	Probe66Call &user = gProbe66Calls[P66User];
	if (!foundUser || !inventory.trusted || !volumes.valid || !probe66Buffer(user, 4096, gpt.userFirst * 4096) ||
	    !probe66ReadCall(context, user)) return;
	r.userdataCrc = SmUfsGpt::crc32(static_cast<const uint8_t *>(user.buffer->getBytesNoCopy()), 4096);
	r.userdata = r.userdataCrc == userCrc && probe66Equal(user, userReference, 0, 4096);
	if (!r.userdata) return;
	r.stage = 12; r.reason = "aligned";
	Probe66Call &aligned = gProbe66Calls[P66Aligned];
	r.aligned = probe66Buffer(aligned, 8192, 4096) && probe66ReadCall(context, aligned) && probe66Equal(aligned, reference, 4096, 8192);
	if (!r.aligned) return;
	r.stage = 13; r.reason = "unaligned";
	Probe66Call &unaligned = gProbe66Calls[P66Unaligned];
	r.unaligned = probe66Buffer(unaligned, 1000, 17) && probe66ReadCall(context, unaligned) && probe66Equal(unaligned, reference, 17, 1000);
	if (!r.unaligned) return;
	r.stage = 14; r.reason = "cross-block";
	Probe66Call &cross = gProbe66Calls[P66Cross];
	r.crossBlock = probe66Buffer(cross, 31, 4096 - 13) && probe66ReadCall(context, cross) && probe66Equal(cross, reference, 4096 - 13, 31);
	if (!r.crossBlock) return;
	r.stage = 15; r.reason = "read16";
	Probe66Call &read16 = gProbe66Calls[P66Read16];
	uint32_t requests = context.providerRequests;
	r.read16 = probe66Buffer(read16, 65536) && probe66ReadCall(context, read16) && probe66Equal(read16, reference, 0, 65536);
	r.requests16 = context.providerRequests - requests;
	r.read16 = r.read16 && r.requests16 >= 1 && context.maxProviderBlocks <= 16 && context.lastProviderBlocks <= 16;
	if (!r.read16) return;
	r.stage = 16; r.reason = "read17-breakup";
	Probe66Call &read17 = gProbe66Calls[P66Read17]; requests = context.providerRequests;
	r.read17 = probe66Buffer(read17, 17 * 4096) && probe66ReadCall(context, read17) &&
	    probe66Equal(read17, reference, 0, 65536) && probe66Equal(read17, tail, 0, 4096, 65536);
	r.requests17 = context.providerRequests - requests; r.last17 = context.lastProviderBlocks;
	r.maxProviderBlocks = context.maxProviderBlocks;
	r.read17 = r.read17 && r.requests17 >= 2 && r.requests17 <= 17 && r.last17 >= 1 && r.last17 <= 16 && r.maxProviderBlocks <= 16;
	if (!r.read17) return;
	r.stage = 17; r.reason = "direct17";
	Probe66Call &direct = gProbe66Calls[P66Direct17];
	direct.buffer = read17.buffer; direct.bytes = read17.bytes; direct.issued = true; direct.expectsCallback = false;
	direct.completion = IOStorageCompletion{nullptr, probe66Callback, &direct};
	const uint32_t submits = context.submits; requests = context.providerRequests;
	IOBlockStorageDevice *device = gProbe66Device;
	direct.returned = device->doAsyncReadWrite(direct.buffer.get(), 0, 17, &direct.attributes, &direct.completion);
	r.direct17Reject = direct.returned != kIOReturnSuccess && !__atomic_load_n(&direct.callbacks, __ATOMIC_ACQUIRE) &&
	    context.submits == submits && context.providerRequests == requests;
	if (!r.direct17Reject) return;
	r.stage = 18; r.reason = "last-block-byte";
	Probe66Call &last = gProbe66Calls[P66Last], &lastByte = gProbe66Calls[P66LastByte];
	r.lastBlock = probe66Buffer(last, 4096, mediaBytes - 4096) && probe66ReadCall(context, last) && probe66Equal(last, lastReference, 0, 4096);
	if (!r.lastBlock) return;
	r.lastByte = probe66Buffer(lastByte, 1, mediaBytes - 1) && probe66ReadCall(context, lastByte) && probe66Equal(lastByte, lastReference, 4095, 1);
	if (!r.lastByte) return;
	r.stage = 19; r.reason = "completion-reentry";
	Probe66Call &outer = gProbe66Calls[P66Outer], &inner = gProbe66Calls[P66Inner];
	if (!probe66Buffer(outer, 4096) || !probe66Buffer(inner, 4096, 4096)) return;
	outer.nested = &inner;
	probe66Issue(outer);
	r.reenter = probe66Wait(context, &outer) && probe66Wait(context, &inner) && probe66Passed(outer) && probe66Passed(inner) &&
	    probe66Equal(outer, reference, 0, 4096) && probe66Equal(inner, reference, 4096, 4096);
	if (!r.reenter) return;
	r.stage = 20; r.reason = "media-negatives";
	for (unsigned i = P66BadEnd; i <= P66BadReserved64; i++) {
		Probe66Call &bad = gProbe66Calls[i];
		UInt64 length = 1, offset = mediaBytes; IODirection direction = kIODirectionIn;
		if (i == P66BadAfter) offset = mediaBytes + 1;
		if (i == P66BadCross) { offset = mediaBytes - 1; length = 2; }
		if (i == P66BadOverflow) offset = ~UInt64(0);
		if (i == P66BadOut || i == P66BadWrite) { offset = 0; length = 4096; direction = kIODirectionOut; }
		if (i == P66BadInOut) { offset = 0; length = 4096; direction = kIODirectionInOut; }
		if (i >= P66BadFua) { offset = 0; length = 4096; }
		if (i == P66BadFua) bad.attributes.options = kIOStorageOptionForceUnitAccess;
		if (i == P66BadEncrypted) bad.attributes.options = kIOStorageOptionIsEncrypted;
		if (i == P66BadReserved24) bad.attributes.reserved0024 = 1;
		if (i == P66BadReserved32) bad.attributes.reserved0032 = 1;
		if (i == P66BadReserved64) bad.attributes.reserved0064 = 1;
		if (i == P66BadNull) {
			bad.completion = IOStorageCompletion{nullptr, probe66Callback, &bad}; bad.offset = 0;
		} else if (!probe66Buffer(bad, length, offset, direction)) return;
		if (i == P66BadFua || i == P66BadEncrypted) {
			bad.issued = true; bad.expectsCallback = false;
			const uint32_t beforeSubmits = context.submits, beforeRequests = context.providerRequests;
			bad.returned = device->doAsyncReadWrite(bad.buffer.get(), 0, 1, &bad.attributes, &bad.completion);
			if (bad.returned != kIOReturnUnsupported || __atomic_load_n(&bad.callbacks, __ATOMIC_ACQUIRE) ||
			    context.submits != beforeSubmits || context.providerRequests != beforeRequests) return;
			++r.negativeCases;
		} else {
			const IOReturn expected = i == P66BadWrite ? kIOReturnLockedWrite : kIOReturnBadArgument;
			if (!probe66Negative(context, bad, i == P66BadWrite, expected)) return;
		}
	}
	r.negatives = r.negativeCases == 13;
	if (!r.negatives) return;
	r.stage = 21; r.reason = "finalize";
	if (!probe66Finalize(context)) return;
	r.isolated = r.isolated && probe66Isolation();
	r.all = r.abi && r.limits && r.constructed && r.attached && r.started && r.media && r.readerOpen && r.writerDenied &&
	    r.gpt && r.userdata && r.aligned && r.unaligned && r.crossBlock && r.read16 && r.read17 && r.direct17Reject &&
	    r.lastBlock && r.lastByte && r.reenter && r.negatives && r.isolated && r.closed && r.callbacksStable;
	if (r.all) { r.stage = 22; r.reason = "ok"; }
}
