// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <IOKit/storage/IOBlockStorageDriver.h>
#include <IOKit/storage/IOMedia.h>
#include <libkern/crypto/sha2.h>

#include "RootContainer.h"
#include "RootWindowRead.h"

// H1 production gate: one registered, read-only whole-LUN media plus a single
// bounded verification of the flashed SHNGROOT container. Nothing here retries,
// writes, resets the controller or frees DMA while a transfer may be outstanding.

class RootWholeMedia;
class RootWholeDriver;

static IOService *gRootWholeController = nullptr;
static Probe65Context *gRootWholeContext = nullptr;
static Sm8250UfsBlock *gRootWholeDevice = nullptr;
static RootWholeMedia *gRootWholeMedia = nullptr;
static RootWholeDriver *gRootWholeDriver = nullptr;

class RootWholeMedia : public IOMedia {
	OSDeclareDefaultStructors(RootWholeMedia)
public:
	static const OSMetaClass *parentMeta() { return superClass; }
	uint32_t stopCompleted = 0;
	bool attachedOnce = false;
	bool attach(IOService *provider) override
	{
		const bool result = IOMedia::attach(provider);
		if (result) attachedOnce = true;
		return result;
	}
	void stop(IOService *provider) override
	{
		IOMedia::stop(provider);
		__atomic_store_n(&stopCompleted, 1, __ATOMIC_RELEASE);
	}
};
OSDefineMetaClassAndStructors(RootWholeMedia, IOMedia)

class RootWholeDriver : public IOBlockStorageDriver {
	OSDeclareDefaultStructors(RootWholeDriver)
public:
	static const OSMetaClass *parentMeta() { return superClass; }
	uint32_t stopCompleted = 0;
	void stop(IOService *provider) override
	{
		IOBlockStorageDriver::stop(provider);
		__atomic_store_n(&stopCompleted, 1, __ATOMIC_RELEASE);
	}
	IOMedia *instantiateDesiredMediaObject() override
	{
		if (gRootWholeMedia) return nullptr;
		RootWholeMedia *media = new RootWholeMedia;
		if (media) {
			media->retain(); // Independent owner even when native init/attach fails.
			gRootWholeMedia = media;
		}
		return media;  // The original instantiateMediaObject owns init/attach/registration.
	}
};
OSDefineMetaClassAndStructors(RootWholeDriver, IOBlockStorageDriver)

#include "RootWindowDevices.h"
#include "RootOwnerBinding.h"
#include "RootBootSet.h"

namespace SmUfsRootProduction {

using namespace SmUfsRoot;

struct Sha256Digest {
	bool operator()(const uint8_t *bytes, uint64_t length, uint8_t out[32]) const
	{
		if (!bytes || length > ~size_t(0)) return false;
		SHA256_CTX context;
		SHA256_Init(&context);
		SHA256_Update(&context, bytes, size_t(length));
		SHA256_Final(out, &context);
		return true;
	}
};

struct Backend {
	Probe65Context &context;
	bool readPhysical(uint64_t lba, uint8_t block[4096])
	{
		if (lba >= context.capacity()) return false;
		context.lock();
		if (!context.ready()) { context.unlock(); context.reject(); return false; }
		context.accept(1);
		context.begin();
		const SmUfsBlockRead::Error status = context.read(uint32_t(lba), block);
		if (status != SmUfsBlockRead::Error::Success) context.close();
		context.unlock();
		context.completed();
		context.retired();
		return status == SmUfsBlockRead::Error::Success;
	}
};

struct Report {
	bool attempted = false, deviceAttached = false, driverAttached = false, started = false;
	bool registered = false, online = false, readOnly = false, geometry = false, whole = false;
	bool manifestValid = false, windowsPresent = false, osFirst = false, osApfs = false;
	bool osTail = false, systemFirst = false, probeComplete = false, lateStable = false;
	uint32_t parseError = 0xffffffffU, version = 0, count = 0;
	uint64_t imageBytes = 0, userFirst = 0, userLast = 0;
	uint64_t mediaRegistryID = 0, driverRegistryID = 0;
	uint32_t blockBytes = 0;
	uint64_t blocks = 0, windowOffsets[4] = {}, windowSizes[4] = {};
	char windowNames[4][32] = {};
	uint32_t osWords[2] = {}, osApfsWord = 0, systemWords[2] = {}, osTailWord = 0;
	uint32_t accepted = 0, completed = 0, rejected = 0, submits = 0, inflight = 0;
	uint32_t stage = 0;
	bool lateViaMedia = false; // Direct context reads; never claim media I/O proof.
	bool teardownComplete = false, teardownHeld = false;
	const char *reason = "not-run";
};
static Report gReport;
static bool gDeviceAttached = false, gDriverAttached = false, gDriverStartAttempted = false;
static uint8_t gInitialOs[512] = {};
// Only start/lateRead use this workspace, both under blockStartupGate.
// Native storage requests retain their own buffers and never touch it.
static uint8_t gManifest[kManifestBytes] = {};
static uint8_t gSector[512] = {};
static uint8_t gPhysicalScratch[kPhysicalBlockBytes] = {};
static Layout gLayout;


static void snapshot()
{
	if (!gRootWholeContext) return;
	Probe65Context &c = *gRootWholeContext;
	c.lock();
	gReport.accepted = __atomic_load_n(&c.accepted, __ATOMIC_RELAXED);
	gReport.completed = __atomic_load_n(&c.completions, __ATOMIC_RELAXED);
	gReport.rejected = __atomic_load_n(&c.rejected, __ATOMIC_RELAXED);
	gReport.submits = c.submits;
	gReport.inflight = __atomic_load_n(&c.inflight, __ATOMIC_ACQUIRE);
	gReport.online = c.ready();
	c.unlock();
}

static bool registered(IOService *service)
{
	if (!service) return false;
	const IOOptionBits state = service->getState();
	return (state & kIOServiceRegisteredState) && !(state & kIOServiceInactiveState);
}

static bool stop(); // All lifecycle entry points require blockStartupGate.
static void discardFailure() { (void)stop(); }

static uint32_t loadLe32(const uint8_t *p)
{
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static bool windowProbe(Backend &backend, const Layout &layout, const char *name, uint64_t offset,
                        uint8_t out[512])
{
	const Window *window = find(layout, name);
	if (!window) return false;
	return readWindowWithScratch(backend, layout, *window, ReadRequest{offset, 512, out}, gPhysicalScratch) == ReadError::Success;
}

// The board boots one production controller. A retained reference keeps callbacks
// safe if the provider stops. Context::ready() rejects I/O after stop.
static Probe65Context *gStoreContext;
static bool gStoreOpen;

static bool storeLayoutMatches(const Layout &layout)
{
	return layout.valid && layout.firstLba == 0x003f13a8 && layout.lastLba == 0x03ae47fa &&
	       layout.imageBytes == 0x22f400000ULL;
}

// A proof image may use exactly the last payload block of the verified candidate.
static void proveDataWrite(Backend &backend, const Layout &layout, uint64_t lba,
                           unsigned reads, unsigned zeroMetadata, bool targetBlank)
{
	uint32_t enabled = 0;
	if (!PE_parse_boot_argn("sm8250-data-fua-proof", &enabled, sizeof(enabled)) || enabled != 1)
		return;

	using namespace SmUfsProbe59;
	if (layout.firstLba != 0x003f13a8 || layout.lastLba != 0x03ae47fa ||
	    layout.imageBytes != 0x22f400000ULL || lba != kWriteProofLba ||
	    reads != 6 || zeroMetadata != 5) {
		printf("UFS-DATA fua refused=layout-or-metadata\n");
		return;
	}

	static constexpr char marker[] = "UNIXNU-IN2020-FUA-PROOF-V1";
	const auto matches = [](const uint8_t *block) {
		if (memcmp(block, marker, sizeof(marker))) return false;
		for (unsigned i = sizeof(marker); i < kPhysicalBlockBytes; ++i)
			if (block[i]) return false;
		return true;
	};
	if (matches(gPhysicalScratch)) {
		printf("UFS-DATA fua persisted=1 lba=%llx\n", lba);
		return;
	}
	if (!targetBlank) {
		printf("UFS-DATA fua refused=target-nonzero\n");
		return;
	}

	memcpy(gPhysicalScratch, marker, sizeof(marker));
	auto &context = backend.context;
	context.lock();
	if (!context.ready() || !context.budget.allow(mach_absolute_time())) {
		context.unlock();
		printf("UFS-DATA fua refused=controller\n");
		return;
	}
	context.accept(1);
	const uint32_t oldTimeout = context.port.timeoutUs;
	context.port.timeoutUs = 2000000;
	const Result result = context.transport.submit(context.port, Command::WriteStore,
	                                               uint32_t(lba), gPhysicalScratch);
	context.port.timeoutUs = oldTimeout;
	if (result.issued) { ++context.submits; context.budget.submitted(); }
	if (context.transport.poisoned) context.close();
	context.unlock();
	context.completed();
	context.retired();

	const bool verified = result.valid && backend.readPhysical(lba, gPhysicalScratch) && matches(gPhysicalScratch);
	printf("UFS-DATA fua write=%u verify=%u lba=%llx status=%u ocs=%u sense=%x\n",
	       unsigned(result.valid), unsigned(verified), lba, unsigned(result.status), result.ocs, result.sense);
}

// One read-only survey of the candidate 512 MiB overlay for the validated IN2020 layout.
static void reportDataCandidate(Backend &backend, const Layout &layout)
{
	// The 512 MiB seed uses one header block, four bitmap blocks, then 131072 page blocks.
	constexpr uint64_t metadataBlocks = 5;
	constexpr uint64_t storeBlocks = metadataBlocks + 131072;
	constexpr uint64_t reserveBlocks = (1ULL << 30) / kPhysicalBlockBytes;
	constexpr uint64_t guardBlocks = (4ULL << 20) / kPhysicalBlockBytes;
	const uint64_t partitionBlocks = layout.userdataBytes / kPhysicalBlockBytes;
	const uint64_t imageBlocks = layout.imageBytes / kPhysicalBlockBytes +
	                             (layout.imageBytes % kPhysicalBlockBytes != 0);
	if (partitionBlocks <= reserveBlocks + storeBlocks ||
	    partitionBlocks - reserveBlocks - storeBlocks < imageBlocks + guardBlocks) {
		printf("UFS-DATA candidate rejected=geometry\n");
		return;
	}

	const uint64_t end = layout.lastLba + 1 - reserveBlocks;
	const uint64_t first = end - storeBlocks;
	const uint64_t offsets[] = {0, 1, 2, 3, metadataBlocks - 1, storeBlocks - 1};
	const char *header = "unread";
	unsigned read = 0, zeros = 0, zeroMetadata = 0;
	bool targetBlank = false;
	for (const uint64_t offset : offsets) {
		if (!backend.readPhysical(first + offset, gPhysicalScratch))
			break;
		++read;
		bool blank = true;
		for (uint8_t byte : gPhysicalScratch)
			blank &= byte == 0;
		if (blank) {
			++zeros;
			if (offset < metadataBlocks) ++zeroMetadata;
		}
		if (offset == storeBlocks - 1) targetBlank = blank;
		if (offset == 0)
			header = blank ? "zero" : (memcmp(gPhysicalScratch, "SHNGDATA", 8) == 0 ? "shngdata" : "other");
	}
	printf("UFS-DATA candidate first=%llx end_exclusive=%llx blocks=%llx samples=%u/%u sample_zero=%u header=%s\n",
	       first, end, storeBlocks, read, unsigned(sizeof(offsets) / sizeof(offsets[0])), zeros, header);
	proveDataWrite(backend, layout, end - 1, read, zeroMetadata, targetBlank);
}

static void start(IOService *controller, Probe65Context &context, uint64_t userFirst, uint64_t userLast)
{
	gReport = {};
	gReport.attempted = true;
	gReport.userFirst = userFirst;
	gReport.userLast = userLast;
	if (!controller || !context.gate || !context.blocks ||
	    gRootWholeDevice || gRootWholeDriver || gRootWholeMedia) {
		gReport.reason = "state";
		return;
	}
	gRootWholeController = controller; gRootWholeController->retain();
	gRootWholeContext = &context; gRootWholeContext->retain();
	gRootWholeDevice = new Sm8250UfsBlock;
	if (!gRootWholeDevice || !gRootWholeDevice->init(nullptr) || !gRootWholeDevice->configure(&context)) {
		gReport.reason = "device";
		discardFailure();
		return;
	}
	if (!gRootWholeDevice->setProperty(kIOMaximumBlockCountReadKey, 16ULL, 64) ||
	    !gRootWholeDevice->setProperty(kIOMaximumByteCountReadKey, 65536ULL, 64)) {
		gReport.reason = "read-limits"; discardFailure(); return;
	}
	gRootWholeDriver = new RootWholeDriver;
	if (!gRootWholeDriver || !gRootWholeDriver->init(nullptr)) { gReport.reason = "driver"; discardFailure(); return; }
	gReport.deviceAttached = gDeviceAttached = gRootWholeDevice->attach(controller);
	if (!gReport.deviceAttached) { gReport.reason = "attach-device"; discardFailure(); return; }
	gReport.driverAttached = gDriverAttached = gRootWholeDriver->attach(gRootWholeDevice);
	if (!gReport.driverAttached) { gReport.reason = "attach-driver"; discardFailure(); return; }
	gDriverStartAttempted = true;
	gReport.started = gRootWholeDriver->start(gRootWholeDevice);
	if (!gReport.started || !gRootWholeMedia) { gReport.reason = "start"; discardFailure(); return; }
	gReport.mediaRegistryID = gRootWholeMedia->getRegistryEntryID();
	gReport.driverRegistryID = gRootWholeDriver->getRegistryEntryID();
	// registerService queues matching; a registry ID alone is not registration.
	uint64_t registrationDeadline;
	clock_interval_to_deadline(1000, kMillisecondScale, &registrationDeadline);
	while (!registered(gRootWholeMedia) || !registered(gRootWholeDriver)) {
		context.lock(); const bool ready = context.ready(); context.unlock();
		if (!ready || mach_absolute_time() >= registrationDeadline) {
			gReport.reason = "registration-pending"; return;
		}
		IOSleep(1);
	}
	gReport.registered = registered(gRootWholeMedia) && registered(gRootWholeDriver);
	gReport.blockBytes = uint32_t(gRootWholeMedia->getPreferredBlockSize());
	gReport.blocks = gRootWholeMedia->getSize() / 4096;
	gReport.whole = gRootWholeMedia->isWhole();
	gReport.readOnly = !gRootWholeMedia->isWritable();
	gReport.geometry = gReport.blockBytes == 4096 && gReport.blocks == context.blocks && gReport.whole;
	context.lock(); gReport.online = context.ready(); context.unlock();
	gReport.reason = "media";
	if (!gReport.registered || !gReport.geometry || !gReport.online) return;

	// One bounded, read-only pass over the flashed container.
	Backend backend{context};
	uint8_t *manifest = gManifest;
	if (!backend.readPhysical(userFirst, manifest)) { gReport.reason = "manifest-read"; return; }
	gLayout = parse(manifest, (userLast - userFirst + 1) * kPhysicalBlockBytes,
	                            userFirst, userLast, Sha256Digest{});
	const Layout &layout = gLayout;
	gReport.parseError = uint32_t(layout.error);
	gReport.manifestValid = layout.valid;
	if (!layout.valid) { gReport.reason = "manifest"; return; }
	gReport.version = layout.version;
	gReport.count = layout.count;
	gReport.imageBytes = layout.imageBytes;
	gReport.windowsPresent = layout.count >= 2;
	printf("UFS-LAYOUT manifest version=%u image_bytes=%llx windows=%u\n",
	       layout.version, layout.imageBytes, layout.count);
	for (uint32_t i = 0; i < layout.count; ++i) {
		const auto &window = layout.windows[i];
		printf("UFS-LAYOUT window name=%s offset=%llx bytes=%llx block_bytes=%u flags=%x\n",
		       window.name, window.offset, window.size, window.blockBytes, window.flags);
	}
	for (uint32_t i = 0; i < layout.count && i < 4; ++i) {
		for (unsigned c = 0; c < 32; ++c) gReport.windowNames[i][c] = layout.windows[i].name[c];
		gReport.windowOffsets[i] = layout.windows[i].offset;
		gReport.windowSizes[i] = layout.windows[i].size;
	}
	uint8_t *sector = gSector;
	if (windowProbe(backend, layout, "OS", 0, sector)) {
		gReport.osFirst = true;
		gReport.osWords[0] = loadLe32(sector);
		gReport.osWords[1] = loadLe32(sector + 4);
		memcpy(gInitialOs, sector, sizeof(gInitialOs));
	}
	if (windowProbe(backend, layout, "OS", 17408, sector)) {
		gReport.osApfs = sector[32] == 'N' && sector[33] == 'X' && sector[34] == 'S' && sector[35] == 'B';
		gReport.osApfsWord = loadLe32(sector + 32);
	}
	const Window *os = find(layout, "OS");
	if (os && os->size >= 512 && windowProbe(backend, layout, "OS", os->size - 512, sector)) {
		gReport.osTail = true;
		gReport.osTailWord = loadLe32(sector);
	}
	if (windowProbe(backend, layout, "SystemOS", 0, sector)) {
		gReport.systemFirst = true;
		gReport.systemWords[0] = loadLe32(sector);
		gReport.systemWords[1] = loadLe32(sector + 4);
	}
	snapshot();
	gReport.probeComplete = gReport.osFirst && gReport.osApfs && gReport.osTail && gReport.systemFirst;
	gReport.reason = gReport.probeComplete ? "probe-complete" : "probe";
	if (gReport.probeComplete) {
		reportDataCandidate(backend, layout);
		if (storeLayoutMatches(layout) && !__atomic_load_n(&gStoreContext, __ATOMIC_ACQUIRE)) {
			context.retain();
			__atomic_store_n(&gStoreContext, &context, __ATOMIC_RELEASE);
		}
	}
	if (gReport.probeComplete && SmUfsRootWindows::enabled()) {
		(void)SmUfsRootWindows::publish(controller, context, layout);
		gReport.reason = SmUfsRootWindows::reason;
	}
}

// One repeat via the context backend. This compares the entire sampled OS sector;
// it does not prove media-path I/O or hash either full window payload.
static bool lateRead()
{
	gReport.registered = registered(gRootWholeMedia) && registered(gRootWholeDriver);
	if (!gRootWholeContext || !gReport.registered)
		return false;
	Backend backend{*gRootWholeContext};
	uint8_t *manifest = gManifest;
	if (!backend.readPhysical(gReport.userFirst, manifest)) return false;
	gLayout = parse(manifest, (gReport.userLast - gReport.userFirst + 1) * kPhysicalBlockBytes,
	                            gReport.userFirst, gReport.userLast, Sha256Digest{});
	const Layout &layout = gLayout;
	if (!layout.valid) return false;
	uint8_t *sector = gSector;
	if (!windowProbe(backend, layout, "OS", 0, sector)) return false;
	gReport.lateStable = gReport.osFirst && memcmp(sector, gInitialOs, sizeof(gInitialOs)) == 0;
	return gReport.lateStable;
}

static bool stop()
{
	if (gRootWholeContext) {
		gRootWholeContext->lock();
		gRootWholeContext->stopping = true;
		gRootWholeContext->online = false;
		gRootWholeContext->unlock();
	}
	uint64_t deadline;
	clock_interval_to_deadline(5000, kMillisecondScale, &deadline);
	while (gRootWholeContext && __atomic_load_n(&gRootWholeContext->inflight, __ATOMIC_ACQUIRE)) {
		if (mach_absolute_time() >= deadline) { gReport.teardownHeld = true; return false; }
		IOSleep(1);
	}
	if (!SmUfsRootWindows::stop()) { gReport.teardownHeld = true; return false; }
	if (gDriverStartAttempted) {
		// Native termination owns stop, provider close and detach. Never replace
		// its asynchronous completion with isInactive/finalize or a manual detach.
		if (!gRootWholeDevice->isInactive() && !gRootWholeDevice->terminate(kIOServiceRequired)) {
			gReport.teardownHeld = true; return false;
		}
		clock_interval_to_deadline(5000, kMillisecondScale, &deadline);
		for (;;) {
			const bool driverDone = __atomic_load_n(&gRootWholeDriver->stopCompleted, __ATOMIC_ACQUIRE) &&
			                        !gRootWholeDriver->getProvider();
			// Native media init may fail before attach; that allocation never needs stop.
			const bool mediaDone = !gRootWholeMedia || (!gRootWholeMedia->getProvider() &&
			    (!gRootWholeMedia->attachedOnce || __atomic_load_n(&gRootWholeMedia->stopCompleted, __ATOMIC_ACQUIRE)));
			if (driverDone && mediaDone && !gRootWholeDevice->getProvider() &&
			    !__atomic_load_n(&gRootWholeContext->inflight, __ATOMIC_ACQUIRE)) break;
			if (mach_absolute_time() >= deadline) { gReport.teardownHeld = true; return false; }
			IOSleep(1);
		}
	} else {
		// No native start, media publication or PMinit has occurred.
		if (gDriverAttached) gRootWholeDriver->detach(gRootWholeDevice);
		if (gDeviceAttached) gRootWholeDevice->detach(gRootWholeController);
	}
	if (gRootWholeDriver) { gRootWholeDriver->release(); gRootWholeDriver = nullptr; }
	if (gRootWholeMedia) { gRootWholeMedia->release(); gRootWholeMedia = nullptr; }
	if (gRootWholeDevice) { gRootWholeDevice->release(); gRootWholeDevice = nullptr; }
	// The context and controller stay retained: NativeUfs still holds blockContext
	// for this boot, and releasing DMA while hardware state is unqualified is worse.
	gDeviceAttached = gDriverAttached = gDriverStartAttempted = false;
	gReport.teardownComplete = true;
	return true;
}

} // namespace SmUfsRootProduction

extern "C" unsigned native_ufs_store_mode(void)
{
	uint32_t mode = 0;
	return PE_parse_boot_argn("sheng-data-persist", &mode, sizeof(mode)) && mode == 2 ? 2 : 0;
}

extern "C" unsigned native_ufs_store_gpt_check(void)
{
	using namespace SmUfsRootProduction;
	auto *context = __atomic_load_n(&gStoreContext, __ATOMIC_ACQUIRE);
	if (!context || context->capacity() != 0x03ae4800) return 1;
	struct Survey {
		Backend backend;
		SmUfsGpt::Report report{};
		uint8_t data[kPhysicalBlockBytes];
		explicit Survey(Probe65Context &c) : backend{c} {}
		bool read(uint64_t lba) { return backend.readPhysical(lba, data); }
		const uint8_t *block() const { return data; }
	};
	auto *survey = new Survey(*context);
	if (!survey) return 2;
	SmUfsGpt::inspect(*survey, context->capacity(), survey->report);
	const auto &gpt = survey->report;
	const bool match = gpt.status == SmUfsGpt::Status::Success && gpt.userFirst == 0x003f13a8 &&
	                   gpt.userLast == 0x03ae47fa && gpt.headerComputed == 0xbf6cf8b6 &&
	                   gpt.tableComputed == 0x361af821;
	delete survey;
	return match ? 0 : 3;
}

extern "C" int native_ufs_store_open(uint64_t blocks, uint64_t *firstLba, const char **why)
{
	using namespace SmUfsProbe59;
	using namespace SmUfsRootProduction;
	if (native_ufs_store_mode() != 2 || blocks != kStoreEnd - kStoreFirst ||
	    native_ufs_store_gpt_check() ||
	    __atomic_exchange_n(&gStoreOpen, true, __ATOMIC_ACQ_REL)) {
		if (why) *why = "unverified or already open";
		return 0;
	}
	if (firstLba) *firstLba = kStoreFirst;
	printf("UFS-DATA store first=%x end_exclusive=%x blocks=%llx\n", kStoreFirst, kStoreEnd, blocks);
	return 1;
}

static bool sm8250StoreRange(uint64_t relative, uint32_t blocks)
{
	using namespace SmUfsProbe59;
	constexpr uint64_t capacity = kStoreEnd - kStoreFirst;
	return __atomic_load_n(&SmUfsRootProduction::gStoreOpen, __ATOMIC_ACQUIRE) &&
	       blocks && relative < capacity && blocks <= capacity - relative;
}

extern "C" int native_ufs_store_read(uint64_t relative, uint32_t blocks, void *destination)
{
	using namespace SmUfsProbe59;
	using namespace SmUfsRootProduction;
	if (!destination || !sm8250StoreRange(relative, blocks)) return 0;
	Backend backend{*__atomic_load_n(&gStoreContext, __ATOMIC_ACQUIRE)};
	auto *data = static_cast<uint8_t *>(destination);
	for (uint32_t i = 0; i < blocks; ++i)
		if (!backend.readPhysical(kStoreFirst + relative + i, data + size_t(i) * kPhysicalBlockBytes))
			return 0;
	return 1;
}

static bool sm8250StoreCommand(Probe65Context &context, SmUfsProbe59::Command command,
                               uint32_t lba, const uint8_t *source)
{
	context.lock();
	if (!context.ready() || !context.budget.allow(mach_absolute_time())) {
		context.unlock();
		return false;
	}
	context.accept(1);
	const auto timeout = context.port.timeoutUs;
	context.port.timeoutUs = command == SmUfsProbe59::Command::SyncStore ? 10000000 : 2000000;
	const auto result = context.transport.submit(context.port, command, lba, source);
	context.port.timeoutUs = timeout;
	if (result.issued) { ++context.submits; context.budget.submitted(); }
	if (context.transport.poisoned) context.close();
	context.unlock();
	context.completed();
	context.retired();
	if (!result.valid)
		printf("UFS-DATA store error cmd=%u lba=%x status=%u ocs=%u sense=%x\n",
		       unsigned(command), lba, unsigned(result.status), result.ocs, result.sense);
	return result.valid;
}

extern "C" int native_ufs_store_write(uint64_t relative, uint32_t blocks, const void *source)
{
	using namespace SmUfsProbe59;
	using namespace SmUfsRootProduction;
	if (!source || !sm8250StoreRange(relative, blocks)) return 0;
	auto &context = *__atomic_load_n(&gStoreContext, __ATOMIC_ACQUIRE);
	const auto *data = static_cast<const uint8_t *>(source);
	for (uint32_t i = 0; i < blocks; ++i)
		if (!sm8250StoreCommand(context, Command::WriteStore, uint32_t(kStoreFirst + relative + i),
		                        data + size_t(i) * kPhysicalBlockBytes))
			return 0;
	return 1;
}

extern "C" int native_ufs_store_sync(void)
{
	using namespace SmUfsRootProduction;
	if (!sm8250StoreRange(0, 1)) return 0;
	return sm8250StoreCommand(*__atomic_load_n(&gStoreContext, __ATOMIC_ACQUIRE),
	                         SmUfsProbe59::Command::SyncStore, 0, nullptr);
}
