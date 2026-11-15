// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

struct Probe65Call {
	IOBlockStorageDevice *device = nullptr;
	OSSharedPtr<IOBufferMemoryDescriptor> buffer;
	uint64_t block = 0, count = 0, actual = 0;
	IOReturn returned = kIOReturnError, status = kIOReturnError;
	uint32_t callbacks = 0, done = 0;
	uint32_t crc[3] = {};
};
static bool probe65Buffer(Probe65Call &call, uint64_t bytes, IODirection direction = kIODirectionIn)
{
	call.buffer = OSSharedPtr<IOBufferMemoryDescriptor>(
	    IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, direction, vm_size_t(bytes), 4096), OSNoRetain);
	return bool(call.buffer);
}
static void probe65Callback(void *, void *parameter, IOReturn status, UInt64 actual)
{
	Probe65Call *call = static_cast<Probe65Call *>(parameter);
	++call->callbacks; call->status = status; call->actual = actual;
}
static void probe65Perform(Probe65Call &call)
{
	IOStorageCompletion completion{nullptr, probe65Callback, &call};
	call.returned = probe65Invoke(call.device, call.buffer.get(), call.block, call.count, &completion);
	if (call.returned == kIOReturnSuccess && call.callbacks == 1 && call.status == kIOReturnSuccess &&
	    call.actual == call.count * 4096) {
		const uint8_t *data = static_cast<const uint8_t *>(call.buffer->getBytesNoCopy());
		for (unsigned i = 0; i < 3 && i < call.count; i++) call.crc[i] = SmUfsGpt::crc32(data + i * 4096, 4096);
	}
}
static bool probe65Passed(const Probe65Call &call)
{
	return call.returned == kIOReturnSuccess && call.callbacks == 1 && call.status == kIOReturnSuccess &&
	       call.actual == call.count * 4096;
}
static void probe65Worker(void *parameter, wait_result_t)
{
	Probe65Call *call = static_cast<Probe65Call *>(parameter);
	probe65Perform(*call);
	__atomic_store_n(&call->done, 1, __ATOMIC_RELEASE);
	thread_terminate(current_thread());
}
struct Probe65Reenter {
	Probe65Call outer, inner;
	uint64_t geometry = 0;
	bool geometryOk = false;
};
static void probe65ReenterCallback(void *, void *parameter, IOReturn status, UInt64 actual)
{
	Probe65Reenter *test = static_cast<Probe65Reenter *>(parameter);
	probe65Callback(nullptr, &test->outer, status, actual);
	test->geometryOk = test->inner.device->reportBlockSize(&test->geometry) == kIOReturnSuccess && test->geometry == 4096;
	probe65Perform(test->inner);
}
struct Probe65TestReport {
	bool constructed = false, abi = false, geometry = false, single = false, multi = false;
	uint32_t blockClassSize = 0, serviceClassSize = 0;
	bool userdata = false, lastBlock = false, concurrent = false, reenter = false, negatives = false;
	bool all = false;
	unsigned negativeCases = 0;
	uint32_t singleCrc = 0, multiCrc[3] = {}, userdataCrc = 0;
	uint32_t callerCrc[2] = {};
};
static Probe65Call gProbe65Calls[8];
static Probe65Reenter gProbe65Reenter;
static Probe65TestReport gProbe65Tests;
static OSSharedPtr<Sm8250UfsBlock> gProbe65Device;

static void probe65Tests(Probe65Context &context, const SmUfsGpt::Report &g, const SmUfsInventory::Inventory &inventory,
                         const SmUfsInventory::Volumes &volumes)
{
	Probe65TestReport &r = gProbe65Tests;
	const OSMetaClass *parent = Sm8250UfsBlock::parentMeta();
	r.blockClassSize = parent->getClassSize();
	r.serviceClassSize = IOService::metaClass->getClassSize();
	r.abi = r.blockClassSize == sizeof(IOBlockStorageDevice) && r.serviceClassSize == sizeof(IOService) &&
	        parent->getSuperClass() == IOService::metaClass;
	if (!r.abi) return;
	Sm8250UfsBlock *object = new Sm8250UfsBlock;
	if (!object) return;
	gProbe65Device = OSSharedPtr<Sm8250UfsBlock>(object, OSNoRetain);
	if (!object->init(nullptr) || !object->configure(&context)) return;
	r.constructed = true;
	IOBlockStorageDevice *device = object;
	UInt64 blockBytes = 0, last = 0; bool protectedMedia = false, present = false, changed = true, removable = true;
	r.geometry = device->reportBlockSize(&blockBytes) == kIOReturnSuccess && blockBytes == 4096 &&
	             device->reportMaxValidBlock(&last) == kIOReturnSuccess && last + 1 == context.blocks &&
	             device->reportWriteProtection(&protectedMedia) == kIOReturnSuccess && protectedMedia &&
	             device->reportMediaState(&present, &changed) == kIOReturnSuccess && present && !changed &&
	             device->reportRemovability(&removable) == kIOReturnSuccess && !removable;
	if (!r.geometry) return;
	for (unsigned i = 0; i < 8; i++) {
		Probe65Call &c = gProbe65Calls[i]; c.device = device; c.count = 1;
		if (!probe65Buffer(c, i == 1 ? 12288 : 4096)) return;
	}
	Probe65Call &single = gProbe65Calls[0]; single.block = 1; probe65Perform(single);
	r.singleCrc = single.crc[0]; r.single = probe65Passed(single) && single.crc[0] == g.headerBlockCrc;
	if (!r.single) return;
	if (g.tableLba != 2 || uint64_t(g.entryCount) * g.entrySize != 4096) return;
	Probe65Call &multi = gProbe65Calls[1]; multi.block = 0; multi.count = 3; probe65Perform(multi);
	for (unsigned i = 0; i < 3; i++) r.multiCrc[i] = multi.crc[i];
	r.multi = probe65Passed(multi) && multi.crc[0] == g.mbrCrc && multi.crc[1] == g.headerBlockCrc && multi.crc[2] == g.tableStored;
	if (!r.multi) return;
	uint32_t userCrc = 0; bool foundUser = false;
	for (unsigned i = 0; i < inventory.count; i++) if (SmUfsInventory::named(inventory.entries[i], "userdata")) {
		userCrc = volumes.entries[i].primary.crc; foundUser = true;
	}
	Probe65Call &user = gProbe65Calls[2]; user.block = g.userFirst; probe65Perform(user);
	r.userdataCrc = user.crc[0]; r.userdata = foundUser && probe65Passed(user) && user.crc[0] == userCrc;
	if (!r.userdata) return;
	Probe65Call &edge = gProbe65Calls[3]; edge.block = context.blocks - 1; probe65Perform(edge);
	Probe65Call &repeat = gProbe65Calls[4]; repeat.block = edge.block; probe65Perform(repeat);
	r.lastBlock = probe65Passed(edge) && probe65Passed(repeat) && edge.crc[0] == repeat.crc[0];
	if (!r.lastBlock) return;
	Probe65Call &a = gProbe65Calls[5], &b = gProbe65Calls[6]; a.block = 0; b.block = 1;
	thread_t workerA = nullptr, workerB = nullptr;
	const kern_return_t startA = kernel_thread_start(probe65Worker, &a, &workerA);
	const kern_return_t startB = kernel_thread_start(probe65Worker, &b, &workerB);
	if (workerA) thread_deallocate(workerA);
	if (workerB) thread_deallocate(workerB);
	uint64_t deadline; clock_interval_to_deadline(5000, kMillisecondScale, &deadline);
	while ((startA == KERN_SUCCESS && !__atomic_load_n(&a.done, __ATOMIC_ACQUIRE)) ||
	       (startB == KERN_SUCCESS && !__atomic_load_n(&b.done, __ATOMIC_ACQUIRE))) {
		if (mach_absolute_time() >= deadline) panic("SMUFS v18 worker-timeout; resources held, no result snapshot");
		IOSleep(1);
	}
	if (startA != KERN_SUCCESS || startB != KERN_SUCCESS) return;
	r.callerCrc[0] = a.crc[0]; r.callerCrc[1] = b.crc[0];
	r.concurrent = probe65Passed(a) && probe65Passed(b) && a.crc[0] == g.mbrCrc && b.crc[0] == g.headerBlockCrc && context.maxActive == 1;
	if (!r.concurrent) return;
	Probe65Reenter &nested = gProbe65Reenter;
	nested.outer.device = nested.inner.device = device; nested.outer.block = 0; nested.inner.block = 1;
	nested.outer.count = nested.inner.count = 1;
	if (!probe65Buffer(nested.outer, 4096) || !probe65Buffer(nested.inner, 4096)) return;
	IOStorageCompletion reentry{nullptr, probe65ReenterCallback, &nested};
	nested.outer.returned = probe65Invoke(device, nested.outer.buffer.get(), 0, 1, &reentry);
	r.reenter = probe65Passed(nested.outer) && nested.geometryOk && probe65Passed(nested.inner) && nested.inner.crc[0] == g.headerBlockCrc;
	if (!r.reenter) return;
	Probe65Call &bad = gProbe65Calls[7];
	const uint32_t submits = context.submits;
	for (unsigned i = 0; i < 8; i++) {
		bad.callbacks = 0; bad.actual = 0; bad.block = 0; bad.count = 1;
		IODirection direction = kIODirectionIn; uint64_t bytes = 4096;
		if (i == 0) direction = kIODirectionOut;
		if (i == 1) direction = kIODirectionInOut;
		if (i == 2) bad.count = 0;
		if (i == 3) bad.block = context.blocks;
		if (i == 4) { bad.block = context.blocks - 1; bad.count = 2; }
		if (i == 5) bytes = 4095;
		if (i == 6) bad.count = 17;
		if (i == 7) bad.block = ~uint64_t(0);
		if (!probe65Buffer(bad, bytes, direction)) return;
		IOStorageCompletion completion{nullptr, probe65Callback, &bad};
		bad.returned = probe65Invoke(device, bad.buffer.get(), bad.block, bad.count, &completion);
		if (bad.returned == kIOReturnSuccess || bad.callbacks || context.submits != submits) return;
		++r.negativeCases;
	}
	IOBlockStorageDeviceExtent extent{}; extent.blockStart = 0; extent.blockCount = 1;
	if (device->doFormatMedia(4096) != kIOReturnNotWritable || device->doUnmap(&extent, 1, 0) != kIOReturnNotWritable ||
	    device->setWriteCacheState(true) != kIOReturnNotWritable || context.submits != submits) return;
	r.negativeCases += 3; r.negatives = true;
	context.lock();
	r.all = context.maxActive == 1 && context.active == 1 && context.accepted == context.completions &&
	        context.ready();
	context.unlock();
}
