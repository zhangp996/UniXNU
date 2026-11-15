// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <IOKit/storage/IOMediaBSDClient.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include "Probe68Native.h"
#include "Probe68ReadProof.h"

// Terminal qualification: every client, task, UIO and data buffer is held until
// the terminal panic. No timeout path snapshots an active task or frees it.
struct Probe68Report {
	bool abi = false, ref = false, init = false, attach = false, start = false;
	bool nodes = false, identity = false, ancestry = false, readOnly = false, isolation = false;
	bool readerOpen = false, geometry = false, writerDenied = false;
	bool gpt = false, userHead = false, userTail = false, repeat17 = false;
	bool alignment = false, eof = false, clip = false, closed = false, workersStable = false, all = false;
	uint32_t stage = 0, failingSlot = 0, clientCount = 0, createCount = 0;
	uint32_t registrations = 0, registrationOptions = 0, readerOpenCount = 0, writerDeniedCount = 0;
	uint32_t geometryCount = 0, rawReadCount = 0, negativeCount = 0, closeCount = 0;
	uint32_t workerCount = 0, workersCompleted = 0, activeWorkers = 0;
	uint32_t requests17 = 0, last17 = 0, maxProviderBlocks = 0;
	uint32_t wholeUnit = 0, userUnit = 0, wholeMinor = 0, userMinor = 0;
	uint32_t wholeBlockDev = 0, wholeCharacterDev = 0, userBlockDev = 0, userCharacterDev = 0;
	uint32_t blockOpenCount = 0, characterOpenCount = 0;
	uint32_t bsdClassSize = 0, storageClassSize = 0, serviceClassSize = 0;
	uint32_t operation = 0, workerOrdinal = 0;
	int lastError = 0;
	uint64_t lastResidual = 0, lastOffset = 0;
	const char *reason = "not-run";
};
static Probe68Report gProbe68Report;
static IOMedia *gProbe68Allowed[2] = {};

class Probe68BSDClient : public IOMediaBSDClient {
	OSDeclareDefaultStructors(Probe68BSDClient)
public:
	uint32_t createCalls = 0, nativeCreateCalls = 0, registrations = 0;
	IOOptionBits registrationOptions = 0;
	static const OSMetaClass *parentMeta() { return superClass; }
	void registerService(IOOptionBits options = 0) override {
		++registrations; registrationOptions |= options;
		++gProbe68Report.registrations; gProbe68Report.registrationOptions |= options;
	}
protected:
	bool createNodes(IOMedia *media) override {
		++createCalls; ++gProbe68Report.createCount;
		if (createCalls != 1 || !media || media != getProvider() ||
		    (media != gProbe68Allowed[0] && media != gProbe68Allowed[1])) return false;
		++nativeCreateCalls;
		return IOMediaBSDClient::createNodes(media);
	}
};
OSDefineMetaClassAndStructors(Probe68BSDClient, IOMediaBSDClient)

// Match spec_vnops: the devsw lock surrounds open/close, while read and
// ioctl are called without that lock. These BSD-private declarations are not
// exported by conf.h unless BSD_KERNEL_PRIVATE is set in the including TU.
extern "C" void devsw_lock(dev_t, int);
extern "C" void devsw_unlock(dev_t, int);

struct Probe68Endpoint {
	Probe68BSDClient *client = nullptr;
	IOMedia *media = nullptr;
	bool initialized = false, attached = false, started = false;
	bool blockOpened = false, characterOpened = false, observed = false, mediaOpen = false;
	int blockFlags = 0, characterFlags = 0;
	SmUfsBsdNative::Snapshot snapshot{};
};
static Probe68Endpoint gProbe68Endpoints[2];
static uint32_t gProbe68Active = 0, gProbe68Completed = 0;

enum Probe68Operation : uint32_t {
	P68Allocate = 1, P68Initialize, P68Attach, P68Start, P68Inspect,
	P68OpenReader, P68OpenWriter, P68Ioctl, P68RawRead, P68Close,
	P68Isolation, P68AlignmentErrno, P68MediaProof
};
struct Probe68Task {
	Probe68Operation operation = P68Allocate;
	Probe68Endpoint *endpoint = nullptr;
	thread_t thread = THREAD_NULL;
	kern_return_t launchResult = KERN_FAILURE;
	uint32_t finished = 0, completions = 0;
	bool launched = false, passed = false, character = false, ancestry = false, readOnly = false;
	int error = ENXIO;
	dev_t dev = 0;
	u_long command = 0;
	uint64_t value = ~uint64_t(0), bytes = 0, offset = 0;
	uint64_t residual = 0, resultingOffset = 0;
	IOBufferMemoryDescriptor *buffer = nullptr;
	uio_t uio = nullptr;
	SmUfsBsdNative::Snapshot snapshot{};
	const SmUfsInventory::Inventory *inventory = nullptr;
};
static Probe68Task gProbe68Tasks[128];
static uint32_t gProbe68TaskCount = 0;
static bool probe68Isolation(const SmUfsInventory::Inventory &);

static bool probe68Callbacks(const Probe68Endpoint &endpoint)
{
	const SmUfsBsdNative::Snapshot &s = endpoint.snapshot;
	if (!endpoint.observed || !s.available || !s.valid || !s.callbacksValid || !s.nodesValid) return false;
	const struct bdevsw &block = bdevsw[s.major];
	const struct cdevsw &character = cdevsw[s.major];
	return reinterpret_cast<uintptr_t>(block.d_open) == s.blockOpen &&
	    reinterpret_cast<uintptr_t>(block.d_close) == s.blockClose &&
	    reinterpret_cast<uintptr_t>(block.d_strategy) == s.blockStrategy &&
	    reinterpret_cast<uintptr_t>(block.d_ioctl) == s.blockIoctl &&
	    reinterpret_cast<uintptr_t>(character.d_open) == s.characterOpen &&
	    reinterpret_cast<uintptr_t>(character.d_close) == s.characterClose &&
	    reinterpret_cast<uintptr_t>(character.d_read) == s.characterRead &&
	    reinterpret_cast<uintptr_t>(character.d_write) == s.characterWrite &&
	    reinterpret_cast<uintptr_t>(character.d_ioctl) == s.characterIoctl;
}

static void probe68Worker(void *parameter, wait_result_t)
{
	Probe68Task &task = *static_cast<Probe68Task *>(parameter);
	Probe68Endpoint *endpoint = task.endpoint;
	task.error = 0;
	switch (task.operation) {
	case P68Allocate:
		endpoint->client = new Probe68BSDClient;
		task.passed = endpoint->client != nullptr;
		break;
	case P68Initialize:
		endpoint->initialized = endpoint->client->init(nullptr);
		task.passed = endpoint->initialized;
		break;
	case P68Attach:
		endpoint->attached = endpoint->client->attach(endpoint->media);
		task.passed = endpoint->attached;
		break;
	case P68Start: {
		IOService *client = endpoint->client;
		endpoint->started = client->start(endpoint->media);
		task.passed = endpoint->started;
		break;
	}
	case P68Inspect:
		task.passed = SmUfsBsdNative::inspect(endpoint->client, endpoint->media, task.snapshot) &&
		    task.snapshot.available && task.snapshot.valid && task.snapshot.nodesValid && task.snapshot.callbacksValid;
		if (task.passed) endpoint->mediaOpen = endpoint->media->isOpen(endpoint->client);
		else task.error = int(task.snapshot.reason);
		break;
	case P68OpenReader:
	case P68OpenWriter:
	case P68Close: {
		const int type = task.character ? S_IFCHR : S_IFBLK;
		const int flags = task.operation == P68Close ? (task.character ? endpoint->characterFlags : endpoint->blockFlags) :
		    (task.operation == P68OpenWriter ? FWRITE : FREAD);
		devsw_lock(task.dev, type);
		if (!probe68Callbacks(*endpoint) || !kernproc) task.error = ENXIO;
		else if (task.operation == P68Close) {
			task.error = task.character ? cdevsw[major(task.dev)].d_close(task.dev, flags, type, kernproc) :
			    bdevsw[major(task.dev)].d_close(task.dev, flags, type, kernproc);
			if (!task.error) {
				if (task.character) { endpoint->characterOpened = false; endpoint->characterFlags = 0; }
				else { endpoint->blockOpened = false; endpoint->blockFlags = 0; }
			}
		} else {
			task.error = task.character ? cdevsw[major(task.dev)].d_open(task.dev, flags, type, kernproc) :
			    bdevsw[major(task.dev)].d_open(task.dev, flags, type, kernproc);
			// Even an unexpectedly accepted writer open is tracked for close.
			if (!task.error) {
				if (task.character) { endpoint->characterOpened = true; endpoint->characterFlags = flags; }
				else { endpoint->blockOpened = true; endpoint->blockFlags = flags; }
			}
		}
		devsw_unlock(task.dev, type);
		task.passed = task.operation == P68OpenWriter ? task.error == EACCES : task.error == 0;
		break;
	}
	case P68Ioctl:
		if (!probe68Callbacks(*endpoint) || !kernproc) task.error = ENXIO;
		else task.error = task.character ? cdevsw[major(task.dev)].d_ioctl(task.dev, task.command,
		    reinterpret_cast<caddr_t>(&task.value), FREAD, kernproc) :
		    bdevsw[major(task.dev)].d_ioctl(task.dev, task.command, reinterpret_cast<caddr_t>(&task.value), FREAD, kernproc);
		task.passed = task.error == 0;
		break;
	case P68RawRead:
		task.buffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionIn,
		    vm_size_t(task.bytes), 4096);
		if (!task.buffer) { task.error = ENOMEM; break; }
		memset(task.buffer->getBytesNoCopy(), 0xa7, size_t(task.bytes));
		task.uio = uio_create(1, off_t(task.offset), UIO_SYSSPACE, UIO_READ);
		if (!task.uio) { task.error = ENOMEM; break; }
		task.error = uio_addiov(task.uio, reinterpret_cast<user_addr_t>(task.buffer->getBytesNoCopy()), user_size_t(task.bytes));
		if (task.error) break;
		if (!probe68Callbacks(*endpoint)) task.error = ENXIO;
		else task.error = cdevsw[major(task.dev)].d_read(task.dev, task.uio, 0);
		task.residual = uint64_t(uio_resid(task.uio));
		task.resultingOffset = uint64_t(uio_offset(task.uio));
		task.passed = true; // The caller checks the expected errno/residual tuple.
		break;
	case P68Isolation:
		task.passed = task.inventory && probe68Isolation(*task.inventory);
		break;
	case P68AlignmentErrno:
		task.error = endpoint->media->errnoFromReturn(kIOReturnNotAligned);
		task.passed = task.error != 0;
		break;
	case P68MediaProof: {
		IOMedia *user = gProbe68Endpoints[1].media;
		task.ancestry = gProbe68Endpoints[0].client->getProvider() == gProbe66Media &&
		    gProbe68Endpoints[1].client->getProvider() == user && user->getProvider() == gProbe67Scheme &&
		    gProbe67Scheme->getProvider() == gProbe66Media && gProbe66Media->getProvider() == gProbe66Driver &&
		    gProbe66Driver->getProvider() == gProbe66Device;
		task.readOnly = gProbe66Media->isWhole() && !user->isWhole() &&
		    !gProbe66Media->isWritable() && !user->isWritable() &&
		    probe67Boolean(gProbe66Media, kIOMediaWritableKey, false) && probe67Boolean(user, kIOMediaWritableKey, false);
		task.passed = task.ancestry && task.readOnly;
		break;
	}
	}
	__atomic_fetch_add(&task.completions, 1, __ATOMIC_RELEASE);
	__atomic_fetch_add(&gProbe68Completed, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&task.finished, 1, __ATOMIC_RELEASE);
	__atomic_fetch_sub(&gProbe68Active, 1, __ATOMIC_RELEASE);
	// The thread reference and task remain held even after its execution ends.
	thread_terminate(current_thread());
}

static Probe68Task *probe68Task(Probe68Operation operation, Probe68Endpoint *endpoint = nullptr,
                               bool character = false)
{
	if (gProbe68TaskCount >= 128 || __atomic_load_n(&gProbe68Active, __ATOMIC_ACQUIRE)) return nullptr;
	Probe68Task &task = gProbe68Tasks[gProbe68TaskCount++];
	task.operation = operation; task.endpoint = endpoint; task.character = character;
	if (endpoint && endpoint->observed)
		task.dev = dev_t(character ? endpoint->snapshot.characterDev : endpoint->snapshot.blockDev);
	return &task;
}
static bool probe68Run(Probe65Context &context, Probe68Task *task, bool startup = false)
{
	if (!task || task->launched || __atomic_load_n(&gProbe68Active, __ATOMIC_ACQUIRE)) return false;
	gProbe68Report.operation = uint32_t(task->operation);
	gProbe68Report.workerOrdinal = uint32_t(task - gProbe68Tasks) + 1;
	task->launched = true;
	__atomic_fetch_add(&gProbe68Active, 1, __ATOMIC_ACQ_REL);
	task->launchResult = kernel_thread_start(probe68Worker, task, &task->thread);
	if (task->launchResult != KERN_SUCCESS) {
		__atomic_fetch_sub(&gProbe68Active, 1, __ATOMIC_RELEASE);
		gProbe68Report.lastError = int(task->launchResult);
		return false;
	}
	uint64_t deadline; clock_interval_to_deadline(startup ? 10000 : 5000, kMillisecondScale, &deadline);
	while (!__atomic_load_n(&task->finished, __ATOMIC_ACQUIRE) ||
	       __atomic_load_n(&gProbe68Active, __ATOMIC_ACQUIRE) ||
	       __atomic_load_n(&context.inflight, __ATOMIC_ACQUIRE)) {
		if (mach_absolute_time() >= deadline)
			panic("SMUFS v21 probe68 native-timeout operation=%u task=%u; persistent resources held, no result snapshot",
			    uint32_t(task->operation), gProbe68Report.workerOrdinal);
		IOSleep(1);
	}
	gProbe68Report.workerCount = gProbe68TaskCount;
	gProbe68Report.workersCompleted = __atomic_load_n(&gProbe68Completed, __ATOMIC_ACQUIRE);
	gProbe68Report.activeWorkers = 0;
	gProbe68Report.lastError = task->error;
	gProbe68Report.lastResidual = task->residual;
	gProbe68Report.lastOffset = task->resultingOffset;
	return task->passed && __atomic_load_n(&task->completions, __ATOMIC_ACQUIRE) == 1;
}

static bool probe68Observe(Probe65Context &context, Probe68Endpoint &endpoint)
{
	Probe68Task *task = probe68Task(P68Inspect, &endpoint);
	if (!probe68Run(context, task)) return false;
	endpoint.snapshot = task->snapshot; endpoint.observed = true;
	return true;
}

static bool probe68Attributes(IOService *service, const SmUfsBsdNative::Snapshot *snapshot)
{
	if (!service || !probe66Unpublished(service) || service->inPlane(gIODTPlane)) return false;
	OSString *name = OSDynamicCast(OSString, service->getProperty("BSD Name"));
	if (!snapshot) return !service->getProperty("BSD Name") && !service->getProperty("BSD Major") &&
	    !service->getProperty("BSD Minor") && !service->getProperty("BSD Unit");
	return name && !strcmp(name->getCStringNoCopy(), snapshot->name) &&
	    probe67Number(service, "BSD Major", snapshot->major, 32) &&
	    probe67Number(service, "BSD Minor", snapshot->minor, 32) &&
	    probe67Number(service, "BSD Unit", snapshot->unit, 32);
}
static bool probe68ClientSet(IOService *provider, IOService *a, IOService *b = nullptr, IOService *c = nullptr)
{
	OSSharedPtr<OSIterator> iterator(provider->getClientIterator(), OSNoRetain);
	if (!iterator) return false;
	bool seen[3] = {}; IOService *expected[3] = {a, b, c}; unsigned count = 0;
	while (OSObject *object = iterator->getNextObject()) {
		bool found = false;
		for (unsigned i = 0; i < 3; i++) if (expected[i] && object == expected[i] && !seen[i]) {
			seen[i] = true; found = true; ++count; break;
		}
		if (!found) return false;
	}
	if (!iterator->isValid()) return false;
	for (unsigned i = 0; i < 3; i++) if (seen[i] != bool(expected[i])) return false;
	return count == unsigned(bool(a)) + unsigned(bool(b)) + unsigned(bool(c));
}
static bool probe68Isolation(const SmUfsInventory::Inventory &inventory)
{
	Probe68Endpoint &whole = gProbe68Endpoints[0], &user = gProbe68Endpoints[1];
	if (!whole.observed || !user.observed || whole.media != gProbe66Media || !whole.attached || !user.attached ||
	    whole.client->getProvider() != whole.media || user.client->getProvider() != user.media ||
	    !probe68Attributes(gProbe66Device, nullptr) || !probe68Attributes(gProbe66Driver, nullptr) ||
	    !probe68Attributes(gProbe66Client, nullptr) || !probe68Attributes(gProbe67Scheme, nullptr) ||
	    !probe68Attributes(whole.media, &whole.snapshot) || !probe68Attributes(user.media, &user.snapshot) ||
	    !probe68Attributes(whole.client, nullptr) || !probe68Attributes(user.client, nullptr) ||
	    !probe68ClientSet(gProbe66Device, gProbe66Driver) || !probe68ClientSet(gProbe66Driver, gProbe66Media) ||
	    !probe68ClientSet(gProbe66Media, gProbe66Client, gProbe67Scheme, whole.client) ||
	    !probe68ClientSet(gProbe66Client, nullptr) || !probe68ClientSet(whole.client, nullptr) ||
	    !probe68ClientSet(user.client, nullptr) || !probe67CountChildren(inventory) ||
	    gProbe66Report.driverRegistrations != 1 || gProbe66Report.driverOptions != 0 ||
	    gProbe66Report.mediaRegistrations != 1 || gProbe66Report.mediaOptions != kIOServiceAsynchronous ||
	    gProbe68Report.registrations != 2 || gProbe68Report.registrationOptions != 0) return false;
	unsigned bsdClients = 0, bsdMedia = 1;
	for (unsigned i = 0; i < 2; i++) {
		const Probe68Endpoint &endpoint = gProbe68Endpoints[i];
		if (endpoint.client->registrations != 1 || endpoint.client->registrationOptions != 0 ||
		    endpoint.client->createCalls != 1 || endpoint.client->nativeCreateCalls != 1 ||
		    !endpoint.client->metaCast("IOMediaBSDClient")) return false;
		++bsdClients;
	}
	for (unsigned i = 0; i < inventory.count; i++) {
		const unsigned slot = inventory.entries[i].slot;
		Probe67Child &child = gProbe67Children[slot - 1];
		const bool isUser = child.media == user.media;
		if (!probe68Attributes(child.media, isUser ? &user.snapshot : nullptr) ||
		    !probe68Attributes(child.client, nullptr) || child.client->getProvider() != child.media ||
		    !probe68ClientSet(child.media, child.client, isUser ? user.client : nullptr) ||
		    !probe68ClientSet(child.client, nullptr)) return false;
		if (isUser) ++bsdMedia;
	}
	return bsdClients == 2 && bsdMedia == 2;
}
static bool probe68Scope(Probe65Context &context, const SmUfsInventory::Inventory &inventory)
{
	Probe68Task *task = probe68Task(P68Isolation);
	if (!task) return false;
	task->inventory = &inventory;
	return probe68Run(context, task);
}
static bool probe68Nodes(const Probe68Endpoint &endpoint, const char *name)
{
	const SmUfsBsdNative::Snapshot &s = endpoint.snapshot;
	char raw[64]; snprintf(raw, sizeof(raw), "r%s", name);
	return s.available && s.valid && s.callbacksValid && s.nodesValid &&
	    s.client == reinterpret_cast<uintptr_t>(endpoint.client) && s.media == reinterpret_cast<uintptr_t>(endpoint.media) &&
	    s.matchingCount == 1 && s.blockNode && s.characterNode && s.blockNode != s.characterNode &&
	    s.blockDevnode && s.characterDevnode && s.blockDevnode != s.characterDevnode &&
	    s.blockType == 1 && s.characterType == 2 && s.blockUid == 0 && s.blockGid == 5 && (s.blockMode & 07777) == 0640 &&
	    s.characterUid == 0 && s.characterGid == 5 && (s.characterMode & 07777) == 0640 &&
	    !strcmp(s.name, name) && !strcmp(s.blockName, name) && !strcmp(s.characterName, raw) &&
	    SmUfsBsdProof::deviceMap(s.major, s.minor, s.blockDev, s.characterDev);
}
static bool probe68OpenCounts(Probe65Context &context, uint32_t block, uint32_t character)
{
	uint32_t blockTotal = 0, characterTotal = 0;
	for (unsigned i = 0; i < 2; i++) {
		Probe68Endpoint &endpoint = gProbe68Endpoints[i];
		if (!probe68Observe(context, endpoint)) return false;
		blockTotal += endpoint.snapshot.blockOpenCount;
		characterTotal += endpoint.snapshot.characterOpenCount;
		if (endpoint.snapshot.blockOpenCount != block || endpoint.snapshot.characterOpenCount != character) return false;
	}
	gProbe68Report.blockOpenCount = blockTotal; gProbe68Report.characterOpenCount = characterTotal;
	return true;
}
static bool probe68Geometry(Probe65Context &context, Probe68Endpoint &endpoint, bool character)
{
	const u_long commands[5] = { DKIOCGETBLOCKSIZE, DKIOCGETBLOCKCOUNT, DKIOCISWRITABLE,
	    DKIOCGETMAXBLOCKCOUNTREAD, DKIOCGETMAXBYTECOUNTREAD };
	const uint64_t expected[5] = {4096, endpoint.media->getSize() / 4096, 0, 16, 65536};
	for (unsigned i = 0; i < 5; i++) {
		const uint32_t submits = context.submits, requests = context.providerRequests;
		Probe68Task *task = probe68Task(P68Ioctl, &endpoint, character);
		if (!task) return false;
		task->command = commands[i];
		if (!probe68Run(context, task) || context.submits != submits || context.providerRequests != requests ||
		    (i == 0 || i == 2 ? uint64_t(uint32_t(task->value)) : task->value) != expected[i]) return false;
		++gProbe68Report.geometryCount;
	}
	return true;
}
static Probe68Task *probe68Raw(Probe65Context &context, Probe68Endpoint &endpoint, uint64_t offset, uint64_t bytes,
                              int expectedError = 0, uint64_t completed = ~uint64_t(0))
{
	Probe68Task *task = probe68Task(P68RawRead, &endpoint, true);
	if (!task) return nullptr;
	task->offset = offset; task->bytes = bytes;
	if (completed == ~uint64_t(0)) completed = bytes;
	if (!probe68Run(context, task) || !SmUfsBsdProof::transfer(task->error, expectedError, bytes, offset,
	    task->residual, task->resultingOffset, completed)) return nullptr;
	++gProbe68Report.rawReadCount;
	return task;
}
static bool probe68Equal(const Probe68Task *task, const Probe66Call &reference,
                         uint64_t referenceOffset, uint64_t bytes, uint64_t offset = 0)
{
	return task && task->buffer && reference.buffer && SmUfsBsdProof::equal(task->buffer->getBytesNoCopy(),
	    task->bytes, offset, reference.buffer->getBytesNoCopy(), reference.buffer->getLength(), referenceOffset, bytes);
}
static bool probe68Untouched(const Probe68Task *task, uint64_t from = 0)
{
	return task && task->buffer && SmUfsBsdProof::sentinel(task->buffer->getBytesNoCopy(), task->bytes, from);
}
static bool probe68Idle(Probe65Context &context)
{
	if (__atomic_load_n(&gProbe68Active, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&context.inflight, __ATOMIC_ACQUIRE)) return false;
	bool stable = true; uint32_t completed = 0;
	for (uint32_t i = 0; i < gProbe68TaskCount; i++) {
		const Probe68Task &task = gProbe68Tasks[i];
		if (!SmUfsBsdProof::workerStable(task.launched, task.launchResult == KERN_SUCCESS,
		    __atomic_load_n(&task.finished, __ATOMIC_ACQUIRE) != 0,
		    __atomic_load_n(&task.completions, __ATOMIC_ACQUIRE))) stable = false;
		if (task.launched && task.launchResult == KERN_SUCCESS &&
		    __atomic_load_n(&task.finished, __ATOMIC_ACQUIRE) &&
		    __atomic_load_n(&task.completions, __ATOMIC_ACQUIRE) == 1) ++completed;
	}
	stable = stable && completed == __atomic_load_n(&gProbe68Completed, __ATOMIC_ACQUIRE);
	gProbe68Report.workerCount = gProbe68TaskCount; gProbe68Report.workersCompleted = completed;
	gProbe68Report.activeWorkers = 0; gProbe68Report.workersStable = stable;
	return stable;
}
static bool probe68Finalize(Probe65Context &context)
{
	// Safe before client allocation, and after partial init/start. Active tasks
	// prohibit snapshots and prohibit entering a native close.
	if (!probe68Idle(context)) return false;
	bool closed = true;
	for (unsigned i = 0; i < 2; i++) {
		Probe68Endpoint &endpoint = gProbe68Endpoints[i];
		for (unsigned kind = 0; kind < 2; kind++) {
			const bool character = kind == 0;
			if (!(character ? endpoint.characterOpened : endpoint.blockOpened)) continue;
			if (!probe68Run(context, probe68Task(P68Close, &endpoint, character))) closed = false;
			else ++gProbe68Report.closeCount;
		}
	}
	uint32_t blocks = 0, characters = 0;
	for (unsigned i = 0; i < 2; i++) {
		Probe68Endpoint &endpoint = gProbe68Endpoints[i];
		if (endpoint.started) {
			if (!probe68Observe(context, endpoint)) { closed = false; continue; }
			blocks += endpoint.snapshot.blockOpenCount; characters += endpoint.snapshot.characterOpenCount;
			if (!SmUfsBsdProof::closedCounts(endpoint.snapshot.blockOpenCount, endpoint.snapshot.characterOpenCount,
			    endpoint.blockOpened, endpoint.characterOpened, endpoint.mediaOpen)) closed = false;
		}
		if (endpoint.blockOpened || endpoint.characterOpened) closed = false;
	}
	gProbe68Report.blockOpenCount = blocks; gProbe68Report.characterOpenCount = characters;
	gProbe68Report.closed = closed && SmUfsBsdProof::closedCounts(blocks, characters);
	if (!gProbe68Report.closed) gProbe68Report.all = false;
	return probe68Idle(context) && gProbe68Report.closed;
}

static void probe68Tests(Probe65Context &context, const SmUfsGpt::Report &gpt,
                         const SmUfsInventory::Inventory &inventory, const SmUfsInventory::Volumes &volumes)
{
	Probe68Report &r = gProbe68Report;
	r.stage = 1; r.reason = "abi";
	const OSMetaClass *bsd = Probe68BSDClient::parentMeta();
	const OSMetaClass *service = bsd ? bsd->getSuperClass() : nullptr;
	const OSMetaClass *media = Probe67Media::parentMeta();
	const OSMetaClass *storage = media ? media->getSuperClass() : nullptr;
	if (!bsd || !service || !storage) return;
	r.bsdClassSize = bsd->getClassSize(); r.serviceClassSize = service->getClassSize();
	r.storageClassSize = storage->getClassSize();
	r.abi = !strcmp(bsd->getClassName(), "IOMediaBSDClient") && r.bsdClassSize == sizeof(IOMediaBSDClient) &&
	    !strcmp(service->getClassName(), "IOService") && r.serviceClassSize == sizeof(IOService) &&
	    service == IOService::metaClass && !strcmp(storage->getClassName(), "IOStorage") &&
	    r.storageClassSize == sizeof(IOStorage) && storage->getSuperClass() == service;
	if (!r.abi) return;
	r.stage = 2; r.reason = "v20-reference";
	unsigned userIndex = inventory.count;
	for (unsigned i = 0; i < inventory.count; i++) if (SmUfsInventory::named(inventory.entries[i], "userdata")) {
		if (userIndex != inventory.count) return;
		userIndex = i;
	}
	if (userIndex == inventory.count) return;
	const SmUfsInventory::Entry &user = inventory.entries[userIndex];
	r.failingSlot = user.slot;
	r.ref = gProbe67Report.all && gProbe67Report.closed && gProbe67Report.callbacksStable &&
	    gProbe66Report.all && gProbe66Report.closed && inventory.trusted && inventory.count == 23 &&
	    volumes.valid && volumes.completed == inventory.count && gpt.status == SmUfsGpt::Status::Success &&
	    user.slot && user.slot <= 32 && user.first == gpt.userFirst && user.last == gpt.userLast &&
	    user.last - user.first + 1 >= 17 && volumes.entries[userIndex].primary.status == SmUfsInventory::SampleStatus::Stable &&
	    context.ready() && !context.transport.poisoned && context.blocks <= ~uint64_t(0) / 4096 &&
	    probe66Passed(gProbe66Calls[P66Gpt]) && probe66Passed(gProbe67Extra[P67Ref16].io) &&
	    probe66Passed(gProbe67Extra[P67Ref17].io);
	if (!r.ref || gProbe68TaskCount || gProbe68Endpoints[0].client || gProbe68Endpoints[1].client) return;
	Probe67Child &userChild = gProbe67Children[user.slot - 1];
	if (!userChild.media || userChild.media != gProbe67BySlot[user.slot - 1] ||
	    !probe66Passed(userChild.head.io) || !probe66Passed(userChild.tail.io) || !probe66Passed(userChild.reference.io)) return;
	gProbe68Allowed[0] = gProbe66Media; gProbe68Allowed[1] = userChild.media;
	r.stage = 3; r.reason = "allocate";
	for (unsigned i = 0; i < 2; i++) {
		gProbe68Endpoints[i].media = gProbe68Allowed[i];
		if (!probe68Run(context, probe68Task(P68Allocate, &gProbe68Endpoints[i]), true)) return;
		++r.clientCount;
	}
	(void)SmUfsBsdNative::configureWhitelist(gProbe68Endpoints[0].client, gProbe68Endpoints[0].media,
	    gProbe68Endpoints[1].client, gProbe68Endpoints[1].media);
	r.stage = 4; r.reason = "native-init";
	for (unsigned i = 0; i < 2; i++)
		if (!probe68Run(context, probe68Task(P68Initialize, &gProbe68Endpoints[i]), true)) return;
	r.init = true;
	r.stage = 5; r.reason = "native-attach";
	for (unsigned i = 0; i < 2; i++)
		if (!probe68Run(context, probe68Task(P68Attach, &gProbe68Endpoints[i]), true)) return;
	r.attach = true;
	r.stage = 6; r.reason = "native-start";
	for (unsigned i = 0; i < 2; i++)
		if (!probe68Run(context, probe68Task(P68Start, &gProbe68Endpoints[i]), true)) return;
	r.start = true;
	r.stage = 7; r.reason = "native-nodes";
	for (unsigned i = 0; i < 2; i++) if (!probe68Observe(context, gProbe68Endpoints[i])) return;
	const SmUfsBsdNative::Snapshot &whole = gProbe68Endpoints[0].snapshot, &slice = gProbe68Endpoints[1].snapshot;
	char wholeName[64], userName[64];
	snprintf(wholeName, sizeof(wholeName), "disk%u", whole.unit);
	snprintf(userName, sizeof(userName), "disk%us%u", whole.unit, user.slot);
	r.nodes = probe68Nodes(gProbe68Endpoints[0], wholeName) && probe68Nodes(gProbe68Endpoints[1], userName);
	if (!r.nodes) return;
	r.wholeUnit = whole.unit; r.userUnit = slice.unit; r.wholeMinor = whole.minor; r.userMinor = slice.minor;
	r.wholeBlockDev = whole.blockDev; r.wholeCharacterDev = whole.characterDev;
	r.userBlockDev = slice.blockDev; r.userCharacterDev = slice.characterDev;
	r.stage = 8; r.reason = "same-unit-distinct-minor";
	r.identity = whole.globals == slice.globals && whole.anchorTable == slice.anchorTable && whole.minorTable == slice.minorTable &&
	    whole.stateLock == slice.stateLock && whole.openLock == slice.openLock &&
	    SmUfsBsdProof::identity(whole.major, whole.unit, whole.minor, whole.name,
	        slice.major, slice.unit, slice.minor, slice.name, user.slot) && whole.slot != slice.slot &&
	    whole.blockDev != slice.blockDev && whole.characterDev != slice.characterDev &&
	    whole.blockNode != slice.blockNode && whole.characterNode != slice.characterNode;
	if (!r.identity) return;
	r.stage = 9; r.reason = "ancestry-readonly";
	Probe68Task *mediaProof = probe68Task(P68MediaProof);
	if (!probe68Run(context, mediaProof)) return;
	r.ancestry = mediaProof->ancestry; r.readOnly = mediaProof->readOnly;
	if (!r.ancestry || !r.readOnly) return;
	r.stage = 10; r.reason = "scope";
	r.isolation = probe68Scope(context, inventory);
	if (!r.isolation || !probe68OpenCounts(context, 0, 0)) return;
	r.stage = 11; r.reason = "writer-denied";
	for (unsigned i = 0; i < 2; i++) for (unsigned kind = 0; kind < 2; kind++) {
		const uint32_t submits = context.submits, requests = context.providerRequests;
		if (!probe68Run(context, probe68Task(P68OpenWriter, &gProbe68Endpoints[i], kind == 1)) ||
		    context.submits != submits || context.providerRequests != requests) return;
		++r.writerDeniedCount;
	}
	r.writerDenied = r.writerDeniedCount == 4 && probe68OpenCounts(context, 0, 0);
	if (!r.writerDenied) return;
	r.stage = 12; r.reason = "reader-open";
	for (unsigned i = 0; i < 2; i++) for (unsigned kind = 0; kind < 2; kind++) {
		if (!probe68Run(context, probe68Task(P68OpenReader, &gProbe68Endpoints[i], kind == 1))) return;
		++r.readerOpenCount;
	}
	r.readerOpen = r.readerOpenCount == 4 && probe68OpenCounts(context, 1, 1);
	if (!r.readerOpen) return;
	r.stage = 13; r.reason = "readonly-geometry-ioctls";
	for (unsigned i = 0; i < 2; i++) for (unsigned kind = 0; kind < 2; kind++)
		if (!probe68Geometry(context, gProbe68Endpoints[i], kind == 1)) return;
	r.geometry = r.geometryCount == 20;
	if (!r.geometry) return;
	r.stage = 14; r.reason = "raw-gpt-three";
	Probe68Task *rawGpt = probe68Raw(context, gProbe68Endpoints[0], 0, 3 * 4096);
	if (!rawGpt || !probe68Equal(rawGpt, gProbe66Calls[P66Gpt], 0, 3 * 4096)) return;
	const uint8_t *gptBytes = static_cast<const uint8_t *>(rawGpt->buffer->getBytesNoCopy());
	r.gpt = SmUfsGpt::crc32(gptBytes, 4096) == gpt.mbrCrc &&
	    SmUfsGpt::crc32(gptBytes + 4096, 4096) == gpt.headerBlockCrc &&
	    SmUfsGpt::crc32(gptBytes + 8192, 4096) == gpt.tableStored;
	if (!r.gpt) return;
	r.stage = 15; r.reason = "raw-user-head";
	Probe68Task *head = probe68Raw(context, gProbe68Endpoints[1], 0, 4096);
	if (!head || !probe68Equal(head, userChild.head.io, 0, 4096)) return;
	const uint32_t headCrc = SmUfsGpt::crc32(static_cast<const uint8_t *>(head->buffer->getBytesNoCopy()), 4096);
	r.userHead = headCrc == volumes.entries[userIndex].primary.crc && headCrc == volumes.entries[userIndex].primary.repeatCrc;
	if (!r.userHead) return;
	r.stage = 16; r.reason = "raw-user-tail";
	const uint64_t userSize = userChild.media->getSize();
	Probe68Task *tail = probe68Raw(context, gProbe68Endpoints[1], userSize - 4096, 4096);
	r.userTail = probe68Equal(tail, userChild.tail.io, 0, 4096) && probe68Equal(tail, userChild.reference.io, 0, 4096);
	if (!r.userTail) return;
	r.stage = 17; r.reason = "raw-user-repeat17";
	for (unsigned repeat = 0; repeat < 2; repeat++) {
		const uint32_t requests = context.providerRequests;
		Probe68Task *read = probe68Raw(context, gProbe68Endpoints[1], 0, 17 * 4096);
		if (!probe68Equal(read, gProbe67Extra[P67Ref16].io, 0, 16 * 4096) ||
		    !probe68Equal(read, gProbe67Extra[P67Ref17].io, 0, 4096, 16 * 4096)) return;
		r.requests17 = context.providerRequests - requests; r.last17 = context.lastProviderBlocks;
		r.maxProviderBlocks = context.maxProviderBlocks;
		if (r.requests17 < 2 || r.requests17 > 17 || !r.last17 || r.last17 > 16 || r.maxProviderBlocks > 16) return;
	}
	r.repeat17 = true;
	r.stage = 18; r.reason = "raw-alignment";
	Probe68Task *mapping = probe68Task(P68AlignmentErrno, &gProbe68Endpoints[1]);
	if (!probe68Run(context, mapping)) return;
	const uint64_t badOffsets[2] = {17, 0}, badLengths[2] = {4096, 1000};
	for (unsigned i = 0; i < 2; i++) {
		const uint32_t submits = context.submits, requests = context.providerRequests;
		Probe68Task *bad = probe68Raw(context, gProbe68Endpoints[1], badOffsets[i], badLengths[i], mapping->error, 0);
		if (!probe68Untouched(bad) || context.submits != submits || context.providerRequests != requests) return;
		++r.negativeCount;
	}
	r.alignment = true;
	r.stage = 19; r.reason = "raw-eof";
	const uint64_t eofOffsets[2] = {userSize, userSize + 4096};
	for (unsigned i = 0; i < 2; i++) {
		const uint32_t submits = context.submits, requests = context.providerRequests;
		Probe68Task *eof = probe68Raw(context, gProbe68Endpoints[1], eofOffsets[i], 4096, 0, 0);
		if (!probe68Untouched(eof) || context.submits != submits || context.providerRequests != requests) return;
		++r.negativeCount;
	}
	r.eof = true;
	r.stage = 20; r.reason = "raw-aligned-clip";
	Probe68Task *clip = probe68Raw(context, gProbe68Endpoints[1], userSize - 4096, 8192, 0, 4096);
	r.clip = probe68Equal(clip, userChild.tail.io, 0, 4096) && probe68Untouched(clip, 4096);
	if (!r.clip) return;
	r.stage = 21; r.reason = "close-and-final-observe";
	if (!probe68Finalize(context)) return;
	r.isolation = r.isolation && probe68Scope(context, inventory);
	if (!probe68Idle(context)) return;
	r.all = r.abi && r.ref && r.init && r.attach && r.start && r.nodes && r.identity && r.ancestry && r.readOnly &&
	    r.isolation && r.readerOpen && r.geometry && r.writerDenied && r.gpt && r.userHead && r.userTail &&
	    r.repeat17 && r.alignment && r.eof && r.clip && r.closed && r.workersStable && r.clientCount == 2 &&
	    r.createCount == 2 && r.readerOpenCount == 4 && r.writerDeniedCount == 4 && r.closeCount == 4 &&
	    r.negativeCount == 4 && r.rawReadCount == 10 && !r.blockOpenCount && !r.characterOpenCount;
	if (r.all) { r.stage = 22; r.failingSlot = 0; r.reason = "ok"; }
}
