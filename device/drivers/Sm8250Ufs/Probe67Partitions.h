// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/storage/IOGUIDPartitionScheme.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSSet.h>
#include <libkern/c++/OSString.h>

// Terminal qualification only. All objects, buffers, attributes and completion
// tuples remain independently held through the terminal panic, even on failure.
struct Probe67Report {
	bool abi = false, ref = false, probe = false, start = false, counts = false;
	bool geometry = false, names = false, guids = false, attrs = false, ancestry = false;
	bool isolation = false, head = false, tail = false, edge = false, reentry = false;
	bool negatives = false, closed = false, callbacksStable = false, all = false;
	uint32_t stage = 0, failingSlot = 0;
	uint32_t factoryCount = 0, setCount = 0, attachedCount = 0, registeredCount = 0;
	uint32_t dtAttachCount = 0, dtDetachCount = 0, schemeRegistrations = 0, schemeOptions = 0;
	uint32_t headCount = 0, tailCount = 0, edgeCount = 0, negativeCount = 0, callbackCount = 0;
	uint32_t readerCount = 0, writerDeniedCount = 0, requests17 = 0, last17 = 0, maxProviderBlocks = 0;
	uint32_t schemeClassSize = 0, guidClassSize = 0, partitionClassSize = 0;
	uint32_t storageClassSize = 0, serviceClassSize = 0, mediaClassSize = 0;
	const char *reason = "not-run";
};
static Probe67Report gProbe67Report;

class Probe67Media : public IOMedia {
	OSDeclareDefaultStructors(Probe67Media)
public:
	UInt32 slot = 0, registrations = 0;
	IOOptionBits registrationOptions = 0;
	static const OSMetaClass *parentMeta() { return superClass; }
	void registerService(IOOptionBits options = 0) override {
		++registrations; registrationOptions |= options;
		++gProbe67Report.registeredCount;
	}
};
OSDefineMetaClassAndStructors(Probe67Media, IOMedia)

static Probe67Media *gProbe67Factory[32] = {};
static Probe67Media *gProbe67BySlot[32] = {};
static bool gProbe67FactoryFault = false;
class Probe67Scheme : public IOGUIDPartitionScheme {
	OSDeclareDefaultStructors(Probe67Scheme)
public:
	static const OSMetaClass *parentMeta() { return superClass; }
	OSSet *partitionSet() const { return _partitions; }
	IOMedia *instantiateDesiredMediaObject(gpt_ent *, UInt32 slot) override {
		Probe67Report &r = gProbe67Report;
		const UInt32 index = r.factoryCount++;
		if (index >= 32 || !slot || slot > 32 || gProbe67BySlot[slot - 1]) {
			gProbe67FactoryFault = true; return nullptr;
		}
		Probe67Media *media = new Probe67Media;
		if (media) {
			media->retain(); // Scan owns init and its original allocation reference.
			media->slot = slot;
			gProbe67Factory[index] = media;
			gProbe67BySlot[slot - 1] = media;
		} else gProbe67FactoryFault = true;
		return media; // Deliberately uninitialized: execute native media factory.
	}
	bool attachMediaObjectToDeviceTree(IOMedia *) override {
		++gProbe67Report.dtAttachCount; return false;
	}
	void detachMediaObjectFromDeviceTree(IOMedia *) override {
		++gProbe67Report.dtDetachCount;
	}
	void registerService(IOOptionBits options = 0) override {
		++gProbe67Report.schemeRegistrations; gProbe67Report.schemeOptions |= options;
	}
};
OSDefineMetaClassAndStructors(Probe67Scheme, IOGUIDPartitionScheme)
static Probe67Scheme *gProbe67Scheme = nullptr;
static bool gProbe67SchemeInitialized = false, gProbe67SchemeAttached = false;
static uint32_t gProbe67CallbackActive = 0, gProbe67DuplicateCallbacks = 0;

// Reuse Probe66's descriptor allocation, byte comparison and completion checks;
// a separate dispatcher carries the child and client instead of targeting whole.
struct Probe67Call {
	Probe66Call io;
	IOMedia *media = nullptr;
	IOService *client = nullptr;
	Probe67Call *nested = nullptr;
};
enum Probe67NegativeIndex { P67BadEnd, P67BadAfter, P67BadCross, P67BadOverflow,
	P67BadOut, P67BadInOut, P67BadWrite, P67NegativeCount };
struct Probe67Child {
	Probe67Media *media = nullptr;
	IOService *client = nullptr;
	bool attached = false;
	Probe67Call head, tail, reference;
	Probe67Call negative[P67NegativeCount];
};
static Probe67Child gProbe67Children[32];
enum Probe67ExtraIndex { P67Ref16, P67Ref17, P67Unaligned, P67Cross, P67Read17,
	P67Outer, P67Inner, P67ExtraCount };
static Probe67Call gProbe67Extra[P67ExtraCount];

__attribute__((noinline)) static void probe67Issue(Probe67Call &call, bool write = false)
{
	call.io.issued = true;
	IOStorage *storage = call.media;
	if (write) storage->write(call.client, call.io.offset, call.io.buffer.get(), &call.io.attributes, &call.io.completion);
	else storage->read(call.client, call.io.offset, call.io.buffer.get(), &call.io.attributes, &call.io.completion);
}
static void probe67Callback(void *, void *parameter, IOReturn status, UInt64 actual)
{
	__atomic_fetch_add(&gProbe67CallbackActive, 1, __ATOMIC_ACQ_REL);
	Probe67Call *call = static_cast<Probe67Call *>(parameter);
	const uint32_t previous = __atomic_fetch_add(&call->io.callbacks, 1, __ATOMIC_ACQ_REL);
	if (previous == 0) {
		call->io.status = status; call->io.actual = actual;
		if (call->nested) probe67Issue(*call->nested);
		__atomic_store_n(&call->io.finished, 1, __ATOMIC_RELEASE);
	} else __atomic_fetch_add(&gProbe67DuplicateCallbacks, 1, __ATOMIC_RELEASE);
	__atomic_fetch_sub(&gProbe67CallbackActive, 1, __ATOMIC_RELEASE);
}
static bool probe67Buffer(Probe67Call &call, UInt64 bytes, UInt64 offset = 0,
                          IOMedia *media = nullptr, IOService *client = nullptr,
                          IODirection direction = kIODirectionIn)
{
	call.media = media; call.client = client;
	if (!probe66Buffer(call.io, bytes, offset, direction)) return false;
	call.io.completion = IOStorageCompletion{nullptr, probe67Callback, &call};
	return true;
}
static bool probe67Wait(Probe65Context &context, const Probe67Call *call = nullptr)
{
	uint64_t deadline; clock_interval_to_deadline(5000, kMillisecondScale, &deadline);
	while ((call && !__atomic_load_n(&call->io.finished, __ATOMIC_ACQUIRE)) ||
	       __atomic_load_n(&context.inflight, __ATOMIC_ACQUIRE) ||
	       __atomic_load_n(&gProbe67CallbackActive, __ATOMIC_ACQUIRE)) {
		if (mach_absolute_time() >= deadline)
			panic("SMUFS v20 probe67 callback-timeout; persistent resources held, no result snapshot");
		IOSleep(1);
	}
	return true;
}
static bool probe67Read(Probe65Context &context, Probe67Call &call)
{
	probe67Issue(call);
	return probe67Wait(context, &call) && probe66Passed(call.io);
}
static bool probe67Reference(Probe65Context &context, Probe67Call &call, UInt64 block, UInt64 count)
{
	if (!probe67Buffer(call, count * 4096)) return false;
	call.io.attributes.options = kIOStorageOptionIsStatic;
	call.io.attributes.adjustedOffset = 17;
	call.io.issued = true;
	IOBlockStorageDevice *device = gProbe66Device;
	call.io.returned = device->doAsyncReadWrite(call.io.buffer.get(), block, count,
	    &call.io.attributes, &call.io.completion);
	return call.io.returned == kIOReturnSuccess && probe67Wait(context, &call) && probe66Passed(call.io);
}
static bool probe67Number(IOService *service, const char *key, UInt64 expected, unsigned bits = 0)
{
	OSNumber *value = OSDynamicCast(OSNumber, service->getProperty(key));
	return value && value->unsigned64BitValue() == expected && (!bits || value->numberOfBits() == bits);
}
static bool probe67Boolean(IOService *service, const char *key, bool expected)
{
	OSBoolean *value = OSDynamicCast(OSBoolean, service->getProperty(key));
	return value && value->isTrue() == expected;
}
static bool probe67GuidText(const char *text, char out[37])
{
	// XNU uuid_unparse emits uppercase; the immutable GPT reference emits
	// lowercase. Accept only canonical 8-4-4-4-12 hexadecimal spelling, and
	// normalize its case before the reference's exact comparisons.
	if (!text) return false;
	for (unsigned i = 0; i < 36; i++) {
		const char c = text[i];
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (c != '-') return false;
			out[i] = c;
		} else if (c >= '0' && c <= '9') out[i] = c;
		else if (c >= 'a' && c <= 'f') out[i] = c;
		else if (c >= 'A' && c <= 'F') out[i] = char(c - 'A' + 'a');
		else return false;
	}
	if (text[36]) return false;
	out[36] = 0;
	return true;
}
static bool probe67GuidProperty(IOService *service, const char *key, const char *expected)
{
	OSString *value = OSDynamicCast(OSString, service->getProperty(key));
	char canonical[37];
	return value && value->getLength() == 36 &&
	    probe67GuidText(value->getCStringNoCopy(), canonical) &&
	    SmUfsPartitionRef::textEqual(expected, canonical);
}
static bool probe67Private(IOService *service)
{
	return probe66Unpublished(service) && !service->inPlane(gIODTPlane) &&
	    !service->getProperty("BSD Name") && !service->getProperty("BSD Major") &&
	    !service->getProperty("BSD Minor") && !service->getProperty("BSD Unit");
}
static bool probe67WholeClients()
{
	if (!gProbe66ClientAttached || !gProbe67SchemeAttached) return false;
	OSSharedPtr<OSIterator> iterator(gProbe66Media->getClientIterator(), OSNoRetain);
	if (!iterator) return false;
	bool client = false, scheme = false; unsigned count = 0;
	while (OSObject *object = iterator->getNextObject()) {
		++count;
		if (object == gProbe66Client && !client) client = true;
		else if (object == gProbe67Scheme && !scheme) scheme = true;
		else return false;
	}
	return iterator->isValid() && count == 2 && client && scheme;
}
static bool probe67CountChildren(const SmUfsInventory::Inventory &inventory)
{
	Probe67Report &r = gProbe67Report;
	OSSet *set = gProbe67Scheme->partitionSet();
	r.setCount = set ? set->getCount() : 0;
	if (!set || gProbe67FactoryFault || inventory.count != 23 ||
	    r.factoryCount != inventory.count || r.setCount != inventory.count) return false;
	bool expected[32] = {}, setSeen[32] = {}, attachedSeen[32] = {};
	for (unsigned i = 0; i < inventory.count; i++) {
		const UInt32 slot = inventory.entries[i].slot; r.failingSlot = slot;
		if (!slot || slot > 32 || expected[slot - 1]) return false;
		expected[slot - 1] = true;
		Probe67Media *media = gProbe67BySlot[slot - 1];
		if (!media || !set->containsObject(media) || media->slot != slot) return false;
		unsigned factoryMatches = 0;
		for (unsigned j = 0; j < r.factoryCount; j++) if (gProbe67Factory[j] == media) ++factoryMatches;
		if (factoryMatches != 1) return false;
	}
	OSSharedPtr<OSIterator> members(OSCollectionIterator::withCollection(set), OSNoRetain);
	if (!members) return false;
	unsigned membersCount = 0;
	while (OSObject *object = members->getNextObject()) {
		Probe67Media *media = OSDynamicCast(Probe67Media, object);
		if (!media || !media->slot || media->slot > 32) return false;
		const unsigned index = media->slot - 1; r.failingSlot = media->slot;
		if (!expected[index] || setSeen[index] || gProbe67BySlot[index] != media) return false;
		setSeen[index] = true; ++membersCount;
	}
	if (!members->isValid() || membersCount != inventory.count) return false;
	OSSharedPtr<OSIterator> attached(gProbe67Scheme->getClientIterator(), OSNoRetain);
	if (!attached) return false;
	r.attachedCount = 0;
	while (OSObject *object = attached->getNextObject()) {
		Probe67Media *media = OSDynamicCast(Probe67Media, object);
		if (!media || !media->slot || media->slot > 32) return false;
		const unsigned index = media->slot - 1; r.failingSlot = media->slot;
		if (!expected[index] || attachedSeen[index] || !setSeen[index] ||
		    gProbe67BySlot[index] != media || media->getProvider() != gProbe67Scheme ||
		    !probe67Number(media, kIOMediaPartitionIDKey, media->slot, 32)) return false;
		attachedSeen[index] = true; ++r.attachedCount;
	}
	if (!attached->isValid() || r.attachedCount != inventory.count) return false;
	for (unsigned i = 0; i < 32; i++) {
		if (expected[i] != setSeen[i] || expected[i] != attachedSeen[i] ||
		    bool(gProbe67BySlot[i]) != expected[i]) return false;
		if (expected[i] && (gProbe67BySlot[i]->registrations != 1 ||
		    gProbe67BySlot[i]->registrationOptions != 0)) return false;
	}
	return r.registeredCount == inventory.count && r.dtAttachCount == inventory.count &&
	    r.dtDetachCount == 0 && r.schemeRegistrations == 0 && r.schemeOptions == 0;
}
static bool probe67Isolation(const SmUfsInventory::Inventory &inventory)
{
	if (!probe67Private(gProbe66Device) || !probe67Private(gProbe66Driver) ||
	    !probe67Private(gProbe66Media) || !probe67Private(gProbe66Client) || !probe67Private(gProbe67Scheme) ||
	    !probe66OnlyClient(gProbe66Device, gProbe66Driver) ||
	    !probe66OnlyClient(gProbe66Driver, gProbe66Media) || !probe67WholeClients() ||
	    gProbe66Report.driverRegistrations != 1 || gProbe66Report.driverOptions != 0 ||
	    gProbe66Report.mediaRegistrations != 1 || gProbe66Report.mediaOptions != kIOServiceAsynchronous ||
	    !probe67CountChildren(inventory)) return false;
	for (unsigned i = 0; i < inventory.count; i++) {
		const unsigned slot = inventory.entries[i].slot;
		Probe67Child &child = gProbe67Children[slot - 1];
		gProbe67Report.failingSlot = slot;
		if (!probe67Private(child.media) || !probe67Private(child.client) || !child.attached ||
		    child.client->getProvider() != child.media || !probe66OnlyClient(child.media, child.client) ||
		    !probe66OnlyClient(child.client, nullptr)) return false;
	}
	return true;
}
static bool probe67Negative(Probe65Context &context, Probe67Call &call, bool write, IOReturn expected)
{
	const uint32_t submits = context.submits, requests = context.providerRequests;
	probe67Issue(call, write);
	const bool passed = probe67Wait(context, &call) &&
	    __atomic_load_n(&call.io.callbacks, __ATOMIC_ACQUIRE) == 1 &&
	    call.io.status == expected && call.io.actual == 0 &&
	    context.submits == submits && context.providerRequests == requests;
	if (passed) ++gProbe67Report.negativeCount;
	return passed;
}
static bool probe67CallStable(const Probe67Call &call, uint32_t &callbacks)
{
	const uint32_t count = __atomic_load_n(&call.io.callbacks, __ATOMIC_ACQUIRE);
	callbacks += count;
	return call.io.issued ? (__atomic_load_n(&call.io.finished, __ATOMIC_ACQUIRE) && count == 1) : count == 0;
}
static bool probe67Finalize(Probe65Context &context)
{
	Probe67Report &r = gProbe67Report;
	// Safe before init and after partial construction. Incomplete work prevents
	// closing any client; Runner owns context admission and terminal draining.
	if (__atomic_load_n(&context.inflight, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&gProbe67CallbackActive, __ATOMIC_ACQUIRE)) return false;
	bool stable = !__atomic_load_n(&gProbe67DuplicateCallbacks, __ATOMIC_ACQUIRE);
	uint32_t callbacks = 0;
	for (unsigned i = 0; i < 32; i++) {
		const Probe67Child &child = gProbe67Children[i];
		if (!probe67CallStable(child.head, callbacks)) stable = false;
		if (!probe67CallStable(child.tail, callbacks)) stable = false;
		if (!probe67CallStable(child.reference, callbacks)) stable = false;
		for (unsigned j = 0; j < P67NegativeCount; j++)
			if (!probe67CallStable(child.negative[j], callbacks)) stable = false;
	}
	for (unsigned i = 0; i < P67ExtraCount; i++)
		if (!probe67CallStable(gProbe67Extra[i], callbacks)) stable = false;
	r.callbackCount = callbacks; r.callbacksStable = stable;
	if (!stable) { r.all = false; return false; }
	for (unsigned i = 0; i < 32; i++) {
		Probe67Child &child = gProbe67Children[i];
		if (child.attached && child.media->isOpen(child.client)) child.media->close(child.client);
	}
	bool closed = true;
	for (unsigned i = 0; i < 32; i++) {
		const Probe67Child &child = gProbe67Children[i];
		if (child.attached && (child.media->isOpen(child.client) || child.media->isOpen())) closed = false;
	}
	if (gProbe67SchemeInitialized && gProbe67Scheme->isOpen()) closed = false;
	if (gProbe67SchemeAttached && gProbe66Media->isOpen(gProbe67Scheme)) closed = false;
	r.closed = closed;
	if (!closed) r.all = false;
	return closed;
}

static void probe67Tests(Probe65Context &context, const SmUfsGpt::Report &gpt,
                         const SmUfsInventory::Inventory &inventory, const SmUfsInventory::Volumes &volumes,
                         const SmUfsPartitionRef::Reference &reference)
{
	Probe67Report &r = gProbe67Report;
	r.stage = 1; r.reason = "abi";
	const OSMetaClass *guid = Probe67Scheme::parentMeta(), *media = Probe67Media::parentMeta();
	if (!guid || !media) return;
	const OSMetaClass *partition = guid->getSuperClass();
	const OSMetaClass *storage = partition ? partition->getSuperClass() : nullptr;
	const OSMetaClass *service = storage ? storage->getSuperClass() : nullptr;
	if (!partition || !storage || !service) return;
	r.schemeClassSize = r.guidClassSize = guid->getClassSize();
	r.partitionClassSize = partition->getClassSize(); r.storageClassSize = storage->getClassSize();
	r.serviceClassSize = service->getClassSize(); r.mediaClassSize = media->getClassSize();
	r.abi = r.schemeClassSize == sizeof(IOGUIDPartitionScheme) &&
	    r.partitionClassSize == sizeof(IOPartitionScheme) && r.storageClassSize == sizeof(IOStorage) &&
	    r.serviceClassSize == sizeof(IOService) && r.mediaClassSize == sizeof(IOMedia) &&
	    !strcmp(guid->getClassName(), "IOGUIDPartitionScheme") &&
	    !strcmp(partition->getClassName(), "IOPartitionScheme") &&
	    !strcmp(storage->getClassName(), "IOStorage") && !strcmp(service->getClassName(), "IOService") &&
	    !strcmp(media->getClassName(), "IOMedia") &&
	    service == IOService::metaClass && media->getSuperClass() == storage;
	if (!r.abi) return;
	r.stage = 2; r.reason = "reference";
	r.ref = reference.trusted && inventory.trusted && volumes.valid && inventory.count == 23 &&
	    volumes.completed == inventory.count && gpt.status == SmUfsGpt::Status::Success &&
	    gpt.used == inventory.count && gpt.entryCount == 32 && gpt.entrySize == sizeof(gpt_ent) &&
	    gpt.tableLba == 2 && context.blocks >= 17 && context.blocks <= ~UInt64(0) / 4096;
	if (!r.ref || !gProbe66Report.all || !gProbe66Report.closed || !gProbe66Device ||
	    !gProbe66Driver || !gProbe66Media || !gProbe66Client || gProbe67Scheme) return;
	r.stage = 3; r.reason = "init-attach";
	gProbe67Scheme = new Probe67Scheme;
	if (!gProbe67Scheme || !gProbe67Scheme->init(nullptr)) return;
	gProbe67SchemeInitialized = true;
	gProbe67SchemeAttached = gProbe67Scheme->attach(gProbe66Media);
	if (!gProbe67SchemeAttached) return;
	r.stage = 4; r.reason = "native-probe";
	SInt32 score = 0;
	IOService *schemeService = gProbe67Scheme;
	r.probe = schemeService->probe(gProbe66Media, &score) == gProbe67Scheme;
	if (!r.probe || !probe67Wait(context)) return;
	r.stage = 5; r.reason = "native-start";
	r.start = schemeService->start(gProbe66Media);
	if (!r.start) return;
	r.stage = 6; r.reason = "counts";
	r.counts = probe67CountChildren(inventory);
	if (!r.counts) return;
	r.stage = 7; r.reason = "disk-guid";
	char diskGuid[37]; SmUfsPartitionRef::guid(reference.disk, diskGuid);
	if (!probe67GuidProperty(gProbe67Scheme, kIOGUIDPartitionSchemeUUIDKey, diskGuid)) return;
	for (unsigned i = 0; i < inventory.count; i++) {
		const SmUfsInventory::Entry &entry = inventory.entries[i];
		r.failingSlot = entry.slot;
		SmUfsPartitionRef::Expected expected;
		uint64_t base = 0, size = 0;
		r.reason = "expected-reference";
		if (!SmUfsPartitionRef::expected(inventory, reference, i, expected) ||
		    !SmUfsPartitionRef::byteRange(expected.first, expected.last, context.blocks, base, size)) return;
		Probe67Child &child = gProbe67Children[entry.slot - 1];
		child.media = gProbe67BySlot[entry.slot - 1];
		IOMedia *media = child.media;
		r.reason = "geometry";
		if (!media->isFormatted() || media->isWritable() || media->isWhole() ||
		    media->getPreferredBlockSize() != 4096 || media->getBase() != base || media->getSize() != size ||
		    media->getAttributes() != gProbe66Media->getAttributes() ||
		    !probe67Number(media, kIOMediaPreferredBlockSizeKey, 4096) ||
		    !probe67Number(media, kIOMediaBaseKey, base, 64) || !probe67Number(media, kIOMediaSizeKey, size) ||
		    !probe67Boolean(media, kIOMediaWholeKey, false) || !probe67Boolean(media, kIOMediaWritableKey, false) ||
		    !probe67Boolean(media, kIOMediaRemovableKey, false) ||
		    !probe67Boolean(media, kIOMediaEjectableKey, false)) return;
		r.reason = "name-location-slot";
		char location[12];
		snprintf(location, sizeof(location), "%u", entry.slot);
		if (!SmUfsPartitionRef::textEqual(expected.name, media->getName()) || !media->getLocation() ||
		    strcmp(media->getLocation(), location) || !probe67Number(media, kIOMediaPartitionIDKey, entry.slot, 32)) return;
		r.reason = "type-unique-guid";
		char canonicalType[37], canonicalUuid[37];
		OSString *hint = OSDynamicCast(OSString, media->getProperty(kIOMediaContentHintKey));
		OSString *uuid = OSDynamicCast(OSString, media->getProperty(kIOMediaUUIDKey));
		if (!hint || hint->getLength() != 36 ||
		    !probe67GuidText(media->getContentHint(), canonicalType) ||
		    !probe67GuidProperty(media, kIOMediaContentHintKey, expected.type) ||
		    !probe67GuidProperty(media, kIOMediaContentKey, expected.type) ||
		    !uuid || uuid->getLength() != 36 || !probe67GuidText(uuid->getCStringNoCopy(), canonicalUuid) ||
		    !SmUfsPartitionRef::textEqual(expected.type, canonicalType) ||
		    !SmUfsPartitionRef::textEqual(expected.uuid, canonicalUuid)) return;
		r.reason = "gpt-attributes";
		if (!probe67Number(media, kIOMediaGPTPartitionAttributesKey, expected.attributes, 64)) return;
		OSNumber *gptAttributes = OSDynamicCast(OSNumber, media->getProperty(kIOMediaGPTPartitionAttributesKey));
		r.reason = "reference-row";
		if (!gptAttributes || !SmUfsPartitionRef::matches(expected, child.media->slot, media->getBase(), media->getSize(),
		    gptAttributes->unsigned64BitValue(), canonicalType, canonicalUuid, media->getName())) return;
	}
	r.geometry = r.names = r.guids = r.attrs = true;
	r.stage = 8; r.reason = "ancestry";
	r.ancestry = gProbe67Scheme->getProvider() == gProbe66Media &&
	    gProbe66Media->getProvider() == gProbe66Driver && gProbe66Driver->getProvider() == gProbe66Device &&
	    probe67WholeClients();
	if (!r.ancestry) return;
	r.stage = 9; r.reason = "child-clients";
	for (unsigned i = 0; i < inventory.count; i++) {
		const unsigned slot = inventory.entries[i].slot; r.failingSlot = slot;
		Probe67Child &child = gProbe67Children[slot - 1];
		child.client = new IOService;
		if (!child.client || !child.client->init(nullptr)) return;
		child.attached = child.client->attach(child.media);
		if (!child.attached || !child.media->open(child.client, 0, kIOStorageAccessReader) ||
		    !child.media->isOpen(child.client)) return;
		++r.readerCount;
		if (child.media->open(child.client, 0, kIOStorageAccessReaderWriter) || !child.media->isOpen(child.client)) return;
		++r.writerDeniedCount;
	}
	r.stage = 10; r.reason = "isolation";
	r.isolation = probe67Isolation(inventory);
	if (!r.isolation) return;
	r.stage = 11; r.reason = "head-tail";
	for (unsigned i = 0; i < inventory.count; i++) {
		const SmUfsInventory::Entry &entry = inventory.entries[i]; r.failingSlot = entry.slot;
		Probe67Child &child = gProbe67Children[entry.slot - 1];
		r.reason = "head-crc";
		if (volumes.entries[i].primary.status != SmUfsInventory::SampleStatus::Stable ||
		    !probe67Buffer(child.head, 4096, 0, child.media, child.client) || !probe67Read(context, child.head)) return;
		const uint32_t crc = SmUfsGpt::crc32(static_cast<const uint8_t *>(child.head.io.buffer->getBytesNoCopy()), 4096);
		if (crc != volumes.entries[i].primary.crc || crc != volumes.entries[i].primary.repeatCrc) return;
		++r.headCount;
		r.reason = "tail-absolute";
		if (!probe67Reference(context, child.reference, entry.last, 1) ||
		    !probe67Buffer(child.tail, 4096, child.media->getSize() - 4096, child.media, child.client) ||
		    !probe67Read(context, child.tail) || !probe66Equal(child.tail.io, child.reference.io, 0, 4096)) return;
		++r.tailCount;
	}
	r.head = r.headCount == inventory.count; r.tail = r.tailCount == inventory.count;
	if (!r.head || !r.tail) return;
	r.stage = 12; r.reason = "userdata-reference";
	const SmUfsInventory::Entry *user = nullptr;
	for (unsigned i = 0; i < inventory.count; i++) if (SmUfsInventory::named(inventory.entries[i], "userdata")) {
		if (user) return;
		user = &inventory.entries[i];
	}
	if (!user || user->first != gpt.userFirst || user->last - user->first + 1 < 17) return;
	r.failingSlot = user->slot;
	Probe67Child &userChild = gProbe67Children[user->slot - 1];
	Probe67Call &ref16 = gProbe67Extra[P67Ref16], &ref17 = gProbe67Extra[P67Ref17];
	if (!probe67Reference(context, ref16, user->first, 16) || !probe67Reference(context, ref17, user->first + 16, 1)) return;
	r.stage = 13; r.reason = "userdata-unaligned";
	Probe67Call &unaligned = gProbe67Extra[P67Unaligned];
	if (!probe67Buffer(unaligned, 1000, 17, userChild.media, userChild.client) ||
	    !probe67Read(context, unaligned) || !probe66Equal(unaligned.io, ref16.io, 17, 1000)) return;
	++r.edgeCount;
	r.reason = "userdata-cross-block";
	Probe67Call &cross = gProbe67Extra[P67Cross];
	if (!probe67Buffer(cross, 31, 4096 - 13, userChild.media, userChild.client) ||
	    !probe67Read(context, cross) || !probe66Equal(cross.io, ref16.io, 4096 - 13, 31)) return;
	++r.edgeCount;
	r.stage = 14; r.reason = "userdata-17-blocks";
	Probe67Call &read17 = gProbe67Extra[P67Read17];
	const uint32_t requests = context.providerRequests;
	if (!probe67Buffer(read17, 17 * 4096, 0, userChild.media, userChild.client) ||
	    !probe67Read(context, read17) || !probe66Equal(read17.io, ref16.io, 0, 16 * 4096) ||
	    !probe66Equal(read17.io, ref17.io, 0, 4096, 16 * 4096)) return;
	r.requests17 = context.providerRequests - requests; r.last17 = context.lastProviderBlocks;
	r.maxProviderBlocks = context.maxProviderBlocks;
	if (r.requests17 < 2 || r.requests17 > 17 || !r.last17 || r.last17 > 16 || r.maxProviderBlocks > 16) return;
	++r.edgeCount; r.edge = r.edgeCount == 3;
	r.stage = 15; r.reason = "completion-reentry";
	Probe67Call &outer = gProbe67Extra[P67Outer], &inner = gProbe67Extra[P67Inner];
	if (!probe67Buffer(outer, 4096, 0, userChild.media, userChild.client) ||
	    !probe67Buffer(inner, 4096, 4096, userChild.media, userChild.client)) return;
	outer.nested = &inner;
	probe67Issue(outer);
	r.reentry = probe67Wait(context, &outer) && probe67Wait(context, &inner) &&
	    probe66Passed(outer.io) && probe66Passed(inner.io) &&
	    probe66Equal(outer.io, ref16.io, 0, 4096) && probe66Equal(inner.io, ref16.io, 4096, 4096);
	if (!r.reentry) return;
	r.stage = 16; r.reason = "child-negatives";
	for (unsigned i = 0; i < inventory.count; i++) {
		const unsigned slot = inventory.entries[i].slot; r.failingSlot = slot;
		Probe67Child &child = gProbe67Children[slot - 1];
		const UInt64 size = child.media->getSize();
		for (unsigned j = 0; j < P67NegativeCount; j++) {
			UInt64 bytes = 1, offset = size; IODirection direction = kIODirectionIn;
			if (j == P67BadAfter) offset = size + 1;
			if (j == P67BadCross) { offset = size - 1; bytes = 2; }
			if (j == P67BadOverflow) offset = ~UInt64(0);
			if (j == P67BadOut || j == P67BadInOut || j == P67BadWrite) {
				offset = 0; bytes = 4096; direction = j == P67BadInOut ? kIODirectionInOut : kIODirectionOut;
			}
			Probe67Call &bad = child.negative[j];
			if (!probe67Buffer(bad, bytes, offset, child.media, child.client, direction)) return;
			const IOReturn expected = j == P67BadWrite ? kIOReturnLockedWrite : kIOReturnBadArgument;
			if (!probe67Negative(context, bad, j == P67BadWrite, expected)) return;
		}
	}
	r.negatives = r.negativeCount == inventory.count * P67NegativeCount;
	if (!r.negatives) return;
	r.stage = 17; r.reason = "finalize";
	if (!probe67Finalize(context)) return;
	r.isolation = r.isolation && probe67Isolation(inventory);
	r.all = r.abi && r.ref && r.probe && r.start && r.counts && r.geometry && r.names && r.guids &&
	    r.attrs && r.ancestry && r.isolation && r.head && r.tail && r.edge && r.reentry && r.negatives &&
	    r.closed && r.callbacksStable && r.readerCount == inventory.count && r.writerDeniedCount == inventory.count;
	if (r.all) { r.stage = 18; r.failingSlot = 0; r.reason = "ok"; }
}
