// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <sys/buf.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/ucred.h>
#include "Probe69Iobuf.h"
#include "Probe69Checks.h"

struct Probe69Report {
	enum class RootState : uint8_t { Unknown, Ready, NotReady };
	enum class DevfsState : uint8_t { Unknown, Ready, NotReady };
	bool block = false, vfs = false, blocked = false, closed = false, stable = false, all = false;
	bool isolation = false, rootReady = false, devfsReady = false;
	uint32_t stage = 0, caseIndex = 0, failureSlot = 0, operation = 0;
	uint32_t callbackCount = 0, submitCount = 0, callbackActive = 0;
	uint32_t workerCount = 0, workersCompleted = 0, activeWorkers = 0;
	uint32_t openCount = 0, closeCount = 0, blockOpenCount = 0, characterOpenCount = 0;
	uint32_t strategyCases = 0, completedCases = 0, vfsReads = 0, vfsOpens = 0, vfsCloses = 0;
	uint32_t blockBudgetLimit = 0, blockBudgetUsed = 0, vfsBudgetLimit = 0, vfsBudgetUsed = 0;
	int opError = 0, readinessError = 0;
	bool diagnosticFailed = false;
	uint64_t lastResidual = 0, lastCount = 0, lastSize = 0;
	RootState root = RootState::Unknown;
	DevfsState devfs = DevfsState::Unknown;
	const char *reason = "not-run";
};
static Probe69Report gProbe69Report;

enum Probe69Operation : uint32_t { P69Open = 1, P69Close, P69Observe, P69Strategy, P69Readiness,
	P69PathRead, P69Scope, P69ReleaseReadiness };
struct Probe69Task {
	Probe69Operation operation = P69Observe;
	Probe68Endpoint *endpoint = nullptr;
	thread_t thread = THREAD_NULL;
	uint32_t finished = 0, callbacks = 0, callbackFinished = 0;
	bool launched = false, returned = false, submitted = false, passed = false;
	int error = 0, closeError = 0, callbackError = 0;
	int64_t residual = 0;
	uint32_t completionCount = 0;
	uint64_t block = 0, bytes = 0, offset = 0;
	buf_t bp = nullptr;
	IOBufferMemoryDescriptor *memory = nullptr;
	SmUfsBsdNative::Snapshot snapshot{};
	vnode_t opened = nullptr;
	bool vnodeOpened = false, vnodeClosed = false;
	char path[96] = {};
	const SmUfsInventory::Inventory *inventory = nullptr;
};
static Probe69Task gProbe69Tasks[48];
static uint32_t gProbe69TaskCount = 0, gProbe69Active = 0, gProbe69Completed = 0;
static uint32_t gProbe69CallbackActive = 0, gProbe69Duplicate = 0;
static bool gProbe69BlockOpened[2] = {};
static vnode_t gProbe69DevDirectory = nullptr;
static mount_t gProbe69DevMount = nullptr;
class Probe69MountGuard {
	mount_t mount;
public:
	int error;
	explicit Probe69MountGuard(mount_t value) : mount(value), error(value ? vfs_busy(value, LK_NOWAIT) : ENOENT) {}
	~Probe69MountGuard() { if (!error) vfs_unbusy(mount); }
	Probe69MountGuard(const Probe69MountGuard &) = delete;
	Probe69MountGuard &operator=(const Probe69MountGuard &) = delete;
};
static vfs_context_t gProbe69VfsContext = nullptr;
static proc_t gProbe69Proc = nullptr;
static kauth_cred_t gProbe69Credential = nullptr;

static void probe69Callback(buf_t bp, void *parameter)
{
	__atomic_fetch_add(&gProbe69CallbackActive, 1, __ATOMIC_ACQ_REL);
	Probe69Task &task = *static_cast<Probe69Task *>(parameter);
	const uint32_t previous = __atomic_fetch_add(&task.callbacks, 1, __ATOMIC_ACQ_REL);
	if (!previous) {
		task.callbackError = buf_error(bp);
		task.residual = buf_resid(bp);
		task.completionCount = buf_count(bp);
		__atomic_store_n(&task.callbackFinished, 1, __ATOMIC_RELEASE);
	} else __atomic_fetch_add(&gProbe69Duplicate, 1, __ATOMIC_RELEASE);
	// The callback owns this iobuf. Retain it to terminal panic: biodone may
	// still access bp after this function returns, and timeout never recycles it.
	__atomic_fetch_sub(&gProbe69CallbackActive, 1, __ATOMIC_RELEASE);
}
static unsigned probe69EndpointIndex(const Probe68Endpoint *endpoint)
{
	return endpoint == &gProbe68Endpoints[0] ? 0u : 1u;
}
static bool probe69Observe(Probe69Task &task)
{
	Probe68Endpoint &endpoint = *task.endpoint;
	if (!SmUfsBsdNative::inspect(endpoint.client, endpoint.media, task.snapshot) ||
	    !task.snapshot.valid || !task.snapshot.nodesValid || !task.snapshot.callbacksValid) return false;
	const auto &before = endpoint.snapshot;
	return task.snapshot.major == before.major && task.snapshot.minor == before.minor &&
	       task.snapshot.unit == before.unit && task.snapshot.blockDev == before.blockDev &&
	       task.snapshot.characterDev == before.characterDev && task.snapshot.blockSize == 4096 &&
	       task.snapshot.characterOpenCount == 0 && task.snapshot.blockOpenCount ==
	           (gProbe69BlockOpened[probe69EndpointIndex(&endpoint)] ? 1u : 0u);
}
static void probe69Readiness(Probe69Task &task)
{
	Probe69Report &r = gProbe69Report;
	vnode_t root = vfs_rootvnode();
	if (!root) {
		r.root = Probe69Report::RootState::NotReady; r.blocked = true;
		r.reason = "root-not-ready"; task.passed = true; return;
	}
	r.root = Probe69Report::RootState::Ready; r.rootReady = true;
	vnode_put(root);
	gProbe69VfsContext = vfs_context_kernel();
	if (!gProbe69VfsContext || !kernproc) { r.blocked = true; r.reason = "context-not-ready"; task.passed = true; return; }
	gProbe69Proc = vfs_context_proc(gProbe69VfsContext);
	gProbe69Credential = vfs_context_ucred(gProbe69VfsContext);
	if (gProbe69Proc != kernproc || !IS_VALID_CRED(gProbe69Credential)) {
		r.blocked = true; r.reason = "context-not-ready"; task.passed = true; return;
	}
	task.error = vnode_lookup("/dev", VNODE_LOOKUP_NOFOLLOW | VNODE_LOOKUP_CROSSMOUNTNOWAIT,
	                         &gProbe69DevDirectory, gProbe69VfsContext);
	if (task.error) {
		if (task.error == ENOENT || task.error == EBUSY) {
			r.devfs = Probe69Report::DevfsState::NotReady; r.blocked = true;
			r.reason = task.error == EBUSY ? "mount-transition" : "dev-lookup-not-ready";
			task.passed = true;
		} else r.reason = "dev-lookup-error";
		return;
	}
	if (vnode_vtype(gProbe69DevDirectory) != VDIR) { task.error = ENOTDIR; r.reason = "dev-not-directory"; return; }
	gProbe69DevMount = vnode_mount(gProbe69DevDirectory);
	if (!gProbe69DevMount) {
		r.devfs = Probe69Report::DevfsState::NotReady; r.blocked = true;
		r.reason = "devfs-not-mounted"; task.passed = true; return;
	}
	Probe69MountGuard mountGuard(gProbe69DevMount);
	task.error = mountGuard.error;
	if (task.error) {
		if (task.error == ENOENT || task.error == EBUSY) {
			r.devfs = Probe69Report::DevfsState::NotReady; r.blocked = true;
			r.reason = "mount-transition"; task.passed = true;
		} else r.reason = "mount-busy-error";
		return;
	}
	struct vfsstatfs *stat = vfs_statfs(gProbe69DevMount);
	if (!stat || strcmp(stat->f_fstypename, "devfs")) {
		r.devfs = Probe69Report::DevfsState::NotReady; r.blocked = true;
		r.reason = "mount-not-devfs"; task.passed = true; return;
	}
	r.devfs = Probe69Report::DevfsState::Ready; r.devfsReady = true;
	r.reason = "devfs-ready"; task.passed = true;
}
static void probe69PathRead(Probe69Task &task)
{
	Probe69Report &r = gProbe69Report;
	if (!r.rootReady || !r.devfsReady || !gProbe69VfsContext || gProbe69Proc != kernproc ||
	    !IS_VALID_CRED(gProbe69Credential)) { task.error = ENXIO; return; }
	Probe69MountGuard mountGuard(gProbe69DevMount);
	if (mountGuard.error) { task.error = mountGuard.error; return; }
	vnode_t lookup = nullptr;
	const int flags = VNODE_LOOKUP_NOFOLLOW | VNODE_LOOKUP_CROSSMOUNTNOWAIT;
	task.error = vnode_lookup(task.path, flags, &lookup, gProbe69VfsContext);
	if (task.error) return;
	const dev_t expected = dev_t(task.endpoint->snapshot.characterDev);
	const bool identity = vnode_vtype(lookup) == VCHR && vnode_specrdev(lookup) == expected &&
	                      vnode_mount(lookup) == gProbe69DevMount;
	vnode_put(lookup);
	if (!identity) { task.error = ENXIO; return; }
	task.error = vnode_open(task.path, FREAD, 0, flags, &task.opened, gProbe69VfsContext);
	if (task.error) return;
	task.vnodeOpened = true; ++r.vfsOpens;
	const bool openedIdentity = vnode_vtype(task.opened) == VCHR && vnode_specrdev(task.opened) == expected &&
	                            vnode_mount(task.opened) == gProbe69DevMount;
	if (!openedIdentity) task.error = ENXIO;
	else {
		task.memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionIn, vm_size_t(task.bytes), 4096);
		if (!task.memory) task.error = ENOMEM;
		else {
			memset(task.memory->getBytesNoCopy(), SmUfsBsdProof::kSentinel, size_t(task.bytes));
			int residual = int(task.bytes);
			task.error = vn_rdwr(UIO_READ, task.opened, static_cast<caddr_t>(task.memory->getBytesNoCopy()),
			                    int(task.bytes), off_t(task.offset), UIO_SYSSPACE, 0,
			                    gProbe69Credential, &residual, gProbe69Proc);
			task.residual = residual;
		}
	}
	task.closeError = vnode_close(task.opened, FREAD, gProbe69VfsContext);
	task.opened = nullptr; task.vnodeClosed = true; ++r.vfsCloses;
	if (!task.error && task.closeError) task.error = task.closeError;
	task.passed = SmUfsBlockVfsProof::pathname(r.rootReady, r.devfsReady, true, openedIdentity,
	                                         0, task.error, task.closeError, task.residual);
	if (task.passed) ++r.vfsReads;
}
static void probe69Worker(void *parameter, wait_result_t)
{
	Probe69Task &task = *static_cast<Probe69Task *>(parameter);
	Probe68Endpoint *endpoint = task.endpoint;
	switch (task.operation) {
	case P69Open:
	case P69Close: {
		const dev_t dev = dev_t(endpoint->snapshot.blockDev);
		devsw_lock(dev, S_IFBLK);
		if (!probe68Callbacks(*endpoint)) task.error = ENXIO;
		else if (task.operation == P69Open) task.error = bdevsw[major(dev)].d_open(dev, FREAD, S_IFBLK, kernproc);
		else task.error = bdevsw[major(dev)].d_close(dev, FREAD, S_IFBLK, kernproc);
		devsw_unlock(dev, S_IFBLK);
		if (!task.error) {
			const bool opening = task.operation == P69Open;
			gProbe69BlockOpened[probe69EndpointIndex(endpoint)] = opening;
			if (opening) ++gProbe69Report.openCount; else ++gProbe69Report.closeCount;
			uint32_t bytes = 0;
			task.error = bdevsw[major(dev)].d_ioctl(dev, DKIOCGETBLOCKSIZE, reinterpret_cast<caddr_t>(&bytes), FREAD, kernproc);
			task.passed = !task.error && bytes == 4096;
		}
		break;
	}
	case P69Observe:
		task.passed = probe69Observe(task);
		if (!task.passed) task.error = ENXIO;
		break;
	case P69Strategy: {
		if (!probe68Callbacks(*endpoint) || !gProbe69BlockOpened[probe69EndpointIndex(endpoint)]) { task.error = ENXIO; break; }
		const dev_t dev = dev_t(endpoint->snapshot.blockDev);
		uint32_t bytes = 0;
		task.error = bdevsw[major(dev)].d_ioctl(dev, DKIOCGETBLOCKSIZE, reinterpret_cast<caddr_t>(&bytes), FREAD, kernproc);
		if (task.error || bytes != 4096) { if (!task.error) task.error = EINVAL; break; }
		task.memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionIn, vm_size_t(task.bytes), 4096);
		if (!task.memory) { task.error = ENOMEM; break; }
		memset(task.memory->getBytesNoCopy(), SmUfsBsdProof::kSentinel, size_t(task.bytes));
		task.bp = buf_alloc(nullptr);
		if (!task.bp) { task.error = ENOMEM; break; }
		task.error = probe69_bind_iobuf(task.bp, dev);
		if (task.error) break;
		buf_setdataptr(task.bp, reinterpret_cast<uintptr_t>(task.memory->getBytesNoCopy()));
		buf_setsize(task.bp, int(task.bytes)); buf_setcount(task.bp, int(task.bytes));
		buf_setblkno(task.bp, daddr64_t(task.block)); buf_setresid(task.bp, int(task.bytes));
		buf_seterror(task.bp, 0); buf_setflags(task.bp, B_READ);
		task.error = buf_setcallback(task.bp, probe69Callback, &task);
		if (task.error) break;
		task.submitted = true;
		bdevsw[major(dev)].d_strategy(task.bp);
		bytes = 0;
		task.error = bdevsw[major(dev)].d_ioctl(dev, DKIOCGETBLOCKSIZE, reinterpret_cast<caddr_t>(&bytes), FREAD, kernproc);
		task.passed = !task.error && bytes == 4096;
		break;
	}
	case P69Readiness: probe69Readiness(task); break;
	case P69PathRead: probe69PathRead(task); break;
	case P69Scope: task.passed = task.inventory && probe68Isolation(*task.inventory); break;
	case P69ReleaseReadiness:
		if (gProbe69DevDirectory) { vnode_put(gProbe69DevDirectory); gProbe69DevDirectory = nullptr; }
		gProbe69DevMount = nullptr; task.passed = true; break;
	}
	task.returned = true;
	__atomic_fetch_add(&gProbe69Completed, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&task.finished, 1, __ATOMIC_RELEASE);
	__atomic_fetch_sub(&gProbe69Active, 1, __ATOMIC_RELEASE);
	thread_terminate(current_thread());
}
static Probe69Task *probe69Run(Probe65Context &context, Probe69Operation operation, Probe68Endpoint *endpoint = nullptr,
	uint64_t block = 0, uint64_t bytes = 0, uint64_t offset = 0, const SmUfsInventory::Inventory *inventory = nullptr)
{
	if (gProbe69TaskCount >= 48 || __atomic_load_n(&gProbe69Active, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&gProbe69CallbackActive, __ATOMIC_ACQUIRE)) return nullptr;
	Probe69Task &task = gProbe69Tasks[gProbe69TaskCount++];
	task.operation = operation; task.endpoint = endpoint; task.block = block; task.bytes = bytes;
	task.offset = offset; task.inventory = inventory;
	if (operation == P69PathRead) snprintf(task.path, sizeof(task.path), "/dev/r%s", endpoint->snapshot.name);
	if (!gProbe69Report.diagnosticFailed) gProbe69Report.operation = uint32_t(operation);
	task.launched = true;
	__atomic_fetch_add(&gProbe69Active, 1, __ATOMIC_ACQ_REL);
	const kern_return_t rc = kernel_thread_start(probe69Worker, &task, &task.thread);
	if (rc != KERN_SUCCESS) {
		__atomic_fetch_sub(&gProbe69Active, 1, __ATOMIC_RELEASE);
		gProbe69Report.opError = int(rc); return nullptr;
	}
	uint64_t deadline; clock_interval_to_deadline(10000, kMillisecondScale, &deadline);
	for (;;) {
		const bool returned = __atomic_load_n(&task.finished, __ATOMIC_ACQUIRE) &&
		                      !__atomic_load_n(&gProbe69Active, __ATOMIC_ACQUIRE);
		const bool callbackDone = returned && (!task.submitted || __atomic_load_n(&task.callbackFinished, __ATOMIC_ACQUIRE));
		if (callbackDone && !__atomic_load_n(&gProbe69CallbackActive, __ATOMIC_ACQUIRE) &&
		    !__atomic_load_n(&context.inflight, __ATOMIC_ACQUIRE)) break;
		if (mach_absolute_time() >= deadline)
			panic("SMUFS v22 native-timeout op%u task%u; buf/vnode/DMA held, no active snapshot", uint32_t(operation), gProbe69TaskCount);
		IOSleep(1);
	}
	Probe69Report &r = gProbe69Report;
	r.workerCount = gProbe69TaskCount; r.workersCompleted = __atomic_load_n(&gProbe69Completed, __ATOMIC_ACQUIRE);
	r.activeWorkers = 0; r.callbackActive = 0;
	if (!r.diagnosticFailed) {
		r.opError = task.error ? task.error : task.submitted ? task.callbackError : 0;
		r.lastResidual = task.residual < 0 ? ~uint64_t(0) : uint64_t(task.residual);
		r.lastCount = task.completionCount; r.lastSize = task.bytes;
		if (!task.passed || r.opError) r.diagnosticFailed = !r.blocked || !task.passed;
	}
	if (operation == P69Readiness) r.readinessError = task.error;
	return task.passed ? &task : nullptr;
}
static bool probe69Idle(Probe65Context &context)
{
	if (__atomic_load_n(&gProbe69Active, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&gProbe69CallbackActive, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&context.inflight, __ATOMIC_ACQUIRE) || __atomic_load_n(&gProbe69Duplicate, __ATOMIC_ACQUIRE)) return false;
	uint32_t callbacks = 0, submits = 0;
	for (uint32_t i = 0; i < gProbe69TaskCount; ++i) {
		const Probe69Task &task = gProbe69Tasks[i];
		const bool returned = __atomic_load_n(&task.finished, __ATOMIC_ACQUIRE) && task.returned;
		const uint32_t calls = __atomic_load_n(&task.callbacks, __ATOMIC_ACQUIRE);
		if (!SmUfsBlockVfsProof::settled(returned, task.submitted, calls, 0, 0)) return false;
		if (task.submitted) { ++submits; callbacks += calls; }
	}
	Probe69Report &r = gProbe69Report;
	r.callbackCount = callbacks; r.submitCount = submits; r.callbackActive = 0;
	r.workerCount = gProbe69TaskCount; r.workersCompleted = __atomic_load_n(&gProbe69Completed, __ATOMIC_ACQUIRE);
	r.activeWorkers = 0; r.stable = r.workerCount == r.workersCompleted;
	return r.stable;
}
static bool probe69Finalize(Probe65Context &context)
{
	if (!probe69Idle(context)) return false;
	bool closed = true;
	for (unsigned i = 0; i < 2; ++i) if (gProbe69BlockOpened[i])
		if (!probe69Run(context, P69Close, &gProbe68Endpoints[i])) closed = false;
	if (gProbe69DevDirectory)
		if (!probe69Run(context, P69ReleaseReadiness)) closed = false;
	uint32_t blocks = 0, characters = 0;
	for (unsigned i = 0; i < 2; ++i) if (gProbe68Endpoints[i].started) {
		Probe69Task *task = probe69Run(context, P69Observe, &gProbe68Endpoints[i]);
		if (!task) { closed = false; continue; }
		blocks += task->snapshot.blockOpenCount; characters += task->snapshot.characterOpenCount;
		if (gProbe69BlockOpened[i]) closed = false;
	}
	Probe69Report &r = gProbe69Report;
	r.blockOpenCount = blocks; r.characterOpenCount = characters;
	r.closed = closed && !blocks && !characters;
	if (!r.closed) r.all = false;
	return r.closed && probe69Idle(context);
}
static bool probe69Compare(const Probe69Task *task, const Probe66Call &reference, uint64_t referenceOffset,
	uint64_t bytes, uint64_t actualOffset = 0)
{
	return task && task->memory && reference.buffer && SmUfsBsdProof::equal(task->memory->getBytesNoCopy(), task->bytes,
	    actualOffset, reference.buffer->getBytesNoCopy(), reference.buffer->getLength(), referenceOffset, bytes);
}
static bool probe69BlockCase(Probe65Context &context, unsigned index, Probe68Endpoint &endpoint,
	uint64_t block, uint64_t bytes, uint64_t actual, const Probe66Call *reference, uint64_t referenceOffset = 0)
{
	Probe69Report &r = gProbe69Report; r.caseIndex = index; ++r.strategyCases;
	const uint32_t before = context.submits;
	Probe69Task *task = probe69Run(context, P69Strategy, &endpoint, block, bytes);
	if (!task || !task->submitted || task->completionCount != bytes ||
	    !SmUfsBlockVfsProof::completion(bytes, task->callbackError, 0, task->residual, actual, task->callbacks)) return false;
	if (actual && (!reference || !probe69Compare(task, *reference, referenceOffset, actual))) return false;
	if (actual < bytes && !SmUfsBsdProof::sentinel(task->memory->getBytesNoCopy(), bytes, actual)) return false;
	if (!actual && context.submits != before) return false;
	if (!probe69Run(context, P69Observe, &endpoint)) return false;
	++r.completedCases;
	return true;
}
static void probe69Tests(Probe65Context &context, const SmUfsGpt::Report &gpt,
	const SmUfsInventory::Inventory &inventory, const SmUfsInventory::Volumes &volumes)
{
	(void)gpt; (void)volumes;
	Probe69Report &r = gProbe69Report;
	r.stage = 1; r.reason = "prefix";
	if (!gProbe68Report.all || !gProbe68Report.closed || !gProbe67Report.all || !inventory.trusted ||
	    !context.ready() || gProbe69TaskCount) return;
	const SmUfsInventory::Entry *user = nullptr;
	for (unsigned i = 0; i < inventory.count; ++i) if (SmUfsInventory::named(inventory.entries[i], "userdata")) {
		if (user) return; user = &inventory.entries[i];
	}
	if (!user || !user->slot || user->slot > 32) return;
	Probe67Child &child = gProbe67Children[user->slot - 1];
	Probe68Endpoint &whole = gProbe68Endpoints[0], &slice = gProbe68Endpoints[1];
	if (slice.media != child.media || !child.media) return;
	r.stage = 2; r.reason = "block-open";
	r.blockBudgetLimit = context.budget.limit;
	for (unsigned i = 0; i < 2; ++i) if (!probe69Run(context, P69Open, &gProbe68Endpoints[i])) return;
	r.stage = 3; r.reason = "block-cases";
	if (!probe69BlockCase(context, 1, whole, 1, 4096, 4096, &gProbe66Calls[P66Gpt], 4096) ||
	    !probe69BlockCase(context, 2, whole, 0, 12288, 12288, &gProbe66Calls[P66Gpt]) ||
	    !probe69BlockCase(context, 3, slice, 0, 4096, 4096, &child.head.io) ||
	    !probe69BlockCase(context, 4, slice, child.media->getSize() / 4096 - 1, 4096, 4096, &child.tail.io)) return;
	const uint32_t requests = context.providerRequests;
	Probe69Task *large = probe69Run(context, P69Strategy, &slice, 0, 17 * 4096);
	++r.strategyCases; r.caseIndex = 5;
	if (!large || large->completionCount != 17 * 4096 ||
	    !SmUfsBlockVfsProof::completion(17 * 4096, large->callbackError, 0, large->residual, 17 * 4096, large->callbacks) ||
	    !probe69Compare(large, gProbe67Extra[P67Ref16].io, 0, 16 * 4096) ||
	    !probe69Compare(large, gProbe67Extra[P67Ref17].io, 0, 4096, 16 * 4096) ||
	    context.providerRequests - requests < 2 || context.maxProviderBlocks > 16 ||
	    !probe69Run(context, P69Observe, &slice)) return;
	++r.completedCases;
	const uint64_t end = child.media->getSize() / 4096;
	if (!probe69BlockCase(context, 6, slice, end, 4096, 0, nullptr) ||
	    !probe69BlockCase(context, 7, slice, end + 1, 4096, 0, nullptr) ||
	    !probe69BlockCase(context, 8, slice, end - 1, 8192, 4096, &child.tail.io)) return;
	r.stage = 22; r.reason = "block-close";
	for (unsigned i = 0; i < 2; ++i) if (!probe69Run(context, P69Close, &gProbe68Endpoints[i])) return;
	r.blockBudgetUsed = context.budget.issued;
	r.block = r.strategyCases == 8 && r.completedCases == 8 && r.openCount == 2 && r.closeCount == 2 && probe69Idle(context);
	if (!r.block) return;
	r.stage = 23; r.reason = "readiness";
	if (!probe69Run(context, P69Readiness)) return;
	if (!r.blocked) {
		context.lock(); context.budget = SmUfsReadBudget::State{};
		context.budget.enabled = true; context.budget.limit = 256;
		clock_interval_to_deadline(20000, kMillisecondScale, &context.budget.deadline); context.unlock();
		r.vfsBudgetLimit = 256; r.stage = 24; r.reason = "vfs-read";
		Probe69Task *a = probe69Run(context, P69PathRead, &whole, 0, 12288, 0);
		if (!probe69Compare(a, gProbe66Calls[P66Gpt], 0, 12288)) return;
		Probe69Task *b = probe69Run(context, P69PathRead, &slice, 0, 4096, 0);
		if (!probe69Compare(b, child.head.io, 0, 4096)) return;
		Probe69Task *c = probe69Run(context, P69PathRead, &slice, 0, 4096, child.media->getSize() - 4096);
		if (!probe69Compare(c, child.tail.io, 0, 4096)) return;
		r.vfsBudgetUsed = context.budget.issued;
		r.vfs = r.vfsReads == 3 && r.vfsOpens == 3 && r.vfsCloses == 3;
		if (!r.vfs) return;
	}
	if (!probe69Finalize(context)) return;
	r.isolation = probe69Run(context, P69Scope, nullptr, 0, 0, 0, &inventory) != nullptr;
	r.stable = probe69Idle(context);
	r.all = SmUfsBlockVfsProof::result(r.block, r.vfs, r.blocked, r.closed, r.stable, r.isolation);
	if (r.all) { r.stage = 25; r.reason = "ok"; }
}
