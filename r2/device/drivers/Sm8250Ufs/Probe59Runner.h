// Probe 59 keeps its DMA mapping on this thread's stack until the terminal panic.
struct Probe59Port {
	uintptr_t hc;
	uint8_t *bytes;
	uint64_t phys;
	uint64_t deadline = 0;
	uint32_t timeoutUs = 300000;
	uint32_t read(uint32_t offset) { return readReg(hc, offset); }
	void write(uint32_t offset, uint32_t value) { writeReg(hc, offset, value); }
	void cleanRange(unsigned offset, unsigned length) { cacheCleanInvalidate(bytes + offset, length); }
	void invalidate(unsigned offset, unsigned length) { cacheInvalidate(bytes + offset, length); }
	void begin() { clock_interval_to_deadline(timeoutUs, kMicrosecondScale, &deadline); }
	bool expired() { return mach_absolute_time() >= deadline; }
	void delay() { IODelay(10); }
};

struct Probe61Reader {
	Probe59Port &port;
	SmUfsProbe59::Transport &transport;
	uint64_t blocks;
	SmUfsProbe59::Result last;
	uint64_t deadline = 0;
	bool budgetExpired = false;
	const uint32_t *cancelled = nullptr;
	bool read(uint64_t lba)
	{
		last = SmUfsProbe59::Result{};
		if (cancelled && __atomic_load_n(cancelled, __ATOMIC_ACQUIRE)) {
			last.status = SmUfsProbe59::Status::Guard;
			return false;
		}
		if (mach_absolute_time() >= deadline) {
			budgetExpired = true;
			last.status = SmUfsProbe59::Status::Timeout;
			last.error = 9;  // Overall read budget expired before this command was issued.
			return false;
		}
		if (lba >= blocks || lba > 0xffffffffULL || transport.poisoned) {
			last.status = SmUfsProbe59::Status::Guard;
			return false;
		}
		last = transport.submit(port, SmUfsProbe59::Command::ReadBlock, uint32_t(lba));
		return last.valid;
	}
	const volatile uint8_t *block() const { return port.bytes + SmUfsProbe59::kData; }
};

#include "Probe65Block.h"
#include "Probe65Harness.h"
#include "Probe66Media.h"
#include "Probe67Reference.h"
#include "Probe67Partitions.h"
#include "Probe68Native.h"
#include "Probe68BSD.h"
#include "Probe69BlockVfs.h"
#include "RootProduction.h"

// Keep the 4-KiB range inventory out of the kernel thread's limited stack.
static SmUfsGpt::Report gProbe61Gpt;
static SmUfsInventory::Inventory gProbe62Inventory;
static SmUfsInventory::Volumes gProbe63Volumes;
static SmUfsPartitionRef::Reference gProbe67Reference;
static SmUfsInventory::ReportText gProbe62Text;
// Terminal probe64 keeps this wired allocation prepared, including every failure path.
static OSSharedPtr<IOBufferMemoryDescriptor> gProbe64Memory;
static SmUfsOwned::State gProbe64Owned;

static bool probeAllocateOwned(uint32_t cap, uint64_t oldList, uint64_t oldTask, SmUfsDma &dma,
                               SmUfsOwned::State &s, OSSharedPtr<IOBufferMemoryDescriptor> &memory)
{
	s.cap = cap;
	if (memory) { s.error = SmUfsOwned::Error::Allocation; return false; }
	const IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) |
	                             static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous) |
	                             static_cast<IOOptionBits>(kIOMemoryMapperNone);
	if (cap & SmUfsOwned::kAddress64) {
		memory = OSSharedPtr<IOBufferMemoryDescriptor>(
		    IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, options, SmUfsOwned::kBytes, 4096), OSNoRetain);
	} else {
		memory = OSSharedPtr<IOBufferMemoryDescriptor>(
		    IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options, SmUfsOwned::kBytes, 0xfffff000ULL), OSNoRetain);
	}
	s.allocated = bool(memory);
	if (!s.allocated) { s.error = SmUfsOwned::Error::Allocation; return false; }
	s.prepareResult = memory->prepare(kIODirectionInOut);
	s.prepared = s.prepareResult == kIOReturnSuccess;
	if (!s.prepared) { s.error = SmUfsOwned::Error::Prepare; return false; }
	IOByteCount contiguous = 0;
	s.phys = memory->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
	s.contiguous = contiguous;
	dma.phys = s.phys;
	dma.virt = static_cast<uint8_t *>(memory->getBytesNoCopy());
	return SmUfsOwned::validate(s, dma.virt, memory->getLength(), oldList, oldTask);
}

struct Probe59Text {
	char bytes[1800] = {};
	size_t used = 0;
	void add(const char *format, ...) __printflike(2, 3)
	{
		if (used >= sizeof(bytes) - 1) return;
		va_list args;
		va_start(args, format);
		int count = vsnprintf(bytes + used, sizeof(bytes) - used, format, args);
		va_end(args);
		if (count <= 0) return;
		size_t remaining = sizeof(bytes) - 1 - used;
		used += size_t(count) < remaining ? size_t(count) : remaining;
	}
};

void NativeUfs::probeState59(SmUfsMaps &map)
{
	using namespace SmUfsProbe59;
	const uintptr_t hc = map.hc;
	const uint64_t oldList = uint64_t(readReg(hc, 0x50)) | (uint64_t(readReg(hc, 0x54)) << 32);
	const uint64_t oldTask = uint64_t(readReg(hc, 0x70)) | (uint64_t(readReg(hc, 0x74)) << 32);
	uint64_t list = oldList, taskList = oldTask;
	const uint32_t preHce = readReg(hc, 0x34), preHcs = readReg(hc, 0x30);
	const uint32_t preIs = readReg(hc, 0x20), preDbr = readReg(hc, 0x58);
	const uint32_t preRsr = readReg(hc, 0x60), preTmDbr = readReg(hc, 0x78);
	const uint32_t preTmRsr = readReg(hc, 0x80);
	const bool productionMode = gSmUfs.level == 71;
	const bool rootTraceMode = gSmUfs.level == 70;
	const bool blockVfsMode = gSmUfs.level == 69 || rootTraceMode;
	const bool bsdMode = gSmUfs.level == 68 || blockVfsMode;
	const bool partitionMode = gSmUfs.level == 67 || bsdMode;
	const bool mediaMode = gSmUfs.level == 66 || partitionMode;
	const bool blockMode = gSmUfs.level == 65 || mediaMode || productionMode;
	const bool ownedMode = gSmUfs.level == 64 || blockMode;
	Probe65Context *context = nullptr;
	bool startupLocked = false;
	if (blockMode) {
		IOLockLock(blockStartupGate); startupLocked = true;
		context = new Probe65Context;
		if (!context || !context->initialize()) panic("SMUFS block context-allocation probe%u", gSmUfs.level);
		context->maps = map;
		context->cancelled = &blockCancelled;
		context->gate = IOLockAlloc();
		if (!context->gate) panic("SMUFS v18 lock-allocation");
		blockContext = context;
	}
	SmUfsOwned::State &owned = context ? context->owned : gProbe64Owned;
	OSSharedPtr<IOBufferMemoryDescriptor> &memory = context ? context->memory : gProbe64Memory;
	const bool inventoryMode = (gSmUfs.level == 62 || gSmUfs.level == 63 || ownedMode) && !productionMode;
	const bool volumeMode = (gSmUfs.level == 63 || ownedMode) && !productionMode;
	const bool gptMode = gSmUfs.level == 61 || inventoryMode || productionMode;
	const bool wakeMode = gSmUfs.level == 60 || gptMode;
	SmUfsDma localDma = {};
	SmUfsDma &dma = context ? context->dma : localDma;
	Result results[5];
	Result requestSense, wake, powerAfter, turAfter, capAfter, blockAfter;
	unsigned powerTries = 0, turTries = 0;
	bool wakeNeeded = false;
	Transport localTransport;
	Transport &transport = context ? context->transport : localTransport;
	unsigned stage = 0;
	uint32_t sid0 = 0xffffffffU, sid1 = 0xffffffffU;
	uint32_t lastHcs = preHcs, lastIs = preIs, lastDbr = preDbr, lastRsr = preRsr;
	const char *failure = "none";

	auto capture = [&]() {
		lastHcs = readReg(hc, 0x30);
		lastIs = readReg(hc, 0x20);
		lastDbr = readReg(hc, 0x58);
		lastRsr = readReg(hc, 0x60);
	};
	auto cancelled = [&]() {
		if (blockMode && __atomic_load_n(&blockCancelled, __ATOMIC_ACQUIRE)) {
			failure = "cancelled"; return true;
		}
		return false;
	};
	auto run = [&]() {
		stage = 1;
		if (cancelled()) return;
		if (!list || (list & 4095) || list > ~uint64_t(0) - kBytes ||
		    taskList > ~uint64_t(0) - 1024) {
			failure = "list-address"; return;
		}
		if (!ownedMode && taskListOverlaps(list, taskList, gptMode ? 4096 : kDataBytes)) {
			failure = "task-list-overlap"; return;
		}
		if (!(preHce & 1) || (preHcs & 0xb) != 0xb || preDbr || preTmDbr || (preIs & kFatal)) {
			failure = "inherited-state"; return;
		}
		stage = 2;
		if (cancelled()) return;
		writeReg(hc, 0x24, 0);  // Disable firmware interrupt handling before exclusive polled use.
		writeReg(hc, 0x4c, 0);
		writeReg(hc, 0x60, 0);
		writeReg(hc, 0x80, 0);
		bool stopped = false;
		for (unsigned i = 0; i < 10000; i++) {
			if (!(readReg(hc, 0x60) & 1) && !(readReg(hc, 0x80) & 1)) {
				stopped = true; break;
			}
			IODelay(10);
		}
		capture();
		if (!stopped || lastDbr || readReg(hc, 0x78) || (lastIs & kFatal)) {
			failure = "queue-stop"; return;
		}
		stage = 3;
		if (cancelled()) return;
		if (ownedMode) {
			if (!probeAllocateOwned(readReg(hc, 0), oldList, oldTask, dma, owned, memory)) { failure = "owned-allocation"; return; }
			list = dma.phys; taskList = list + SmUfsOwned::kTask;
		} else {
			dma.phys = list;
			dma.virt = mapDma(list, kBytes, dma.map);
		}
		if (!dma.virt || !layoutValid(list, dma.virt)) {
			failure = "dma-layout"; return;
		}
		// The queue is quiescent before either stream is configured or the old page is changed.
		if (cancelled()) return;
		sid0 = smmuAllowStream(0x0e0, list, kBytes);
		if (sid0 != 1) { failure = "sid-e0"; return; }
		if (cancelled()) return;
		sid1 = smmuAllowStream(0x4e0, list, kBytes);
		if (sid1 != 1) { failure = "sid-4e0"; return; }
		Probe59Port localPort{hc, dma.virt, list};
		if (context) context->port = localPort;
		Probe59Port &port = context ? context->port : localPort;
		if (ownedMode) {
			clearWords(dma.virt, kBytes);
			port.cleanRange(0, kBytes);
		} else {
			// Preserve the inherited task page; the transfer ranges alone are probe-owned.
			clearOwned(dma.virt);
			cleanOwned(port);
		}
		stage = 4;
		if (cancelled()) return;
		if (ownedMode) {
			if (!SmUfsOwned::select(owned, port)) { failure = "owned-select"; return; }
		} else {
			writeReg(hc, 0x50, uint32_t(list));
			writeReg(hc, 0x54, uint32_t(list >> 32));
			if (readReg(hc, 0x50) != uint32_t(list) || readReg(hc, 0x54) != uint32_t(list >> 32)) {
				failure = "list-readback"; return;
			}
		}
		if (cancelled()) return;
		if (preIs & 1) writeReg(hc, 0x20, 1);
		writeReg(hc, 0x60, 1);
		bool ready = false;
		for (unsigned i = 0; i < 10000; i++) {
			if ((readReg(hc, 0x60) & 1) && (readReg(hc, 0x30) & 2)) {
				ready = true; break;
			}
			IODelay(10);
		}
		capture();
		if (!ready) { failure = "queue-start"; return; }
		fbMark2(3);
		const Command commands[] = {Command::Nop, Command::Flag, Command::Power, Command::Tur, Command::Capacity};
		for (unsigned i = 0; i < 5; i++) {
			if (cancelled()) return;
			stage = 10 + i;
			fbFill(gFb, 16 + i * 32, 896, 24, 24, 0xffffff00U);
			results[i] = transport.submit(port, commands[i]);
			lastHcs = results[i].hcs; lastIs = results[i].isAfter;
			lastDbr = results[i].dbr; lastRsr = results[i].rsr;
			fbFill(gFb, 16 + i * 32, 896, 24, 24,
			       results[i].valid ? 0xff00ff00U : 0xffff0000U);
			if (transport.poisoned || (i == 0 && !results[i].valid)) {
				failure = "command-stop"; return;
			}
			if (wakeMode && i == 3) break;  // Retain the pre-wake TUR, defer capacity until after waking.
		}
		auto wakeSubmit = [&](Command command) {
			if (cancelled()) { Result r; r.status = Status::Guard; return r; }
			Result r = transport.submit(port, command);
			lastHcs = r.hcs; lastIs = r.isAfter; lastDbr = r.dbr; lastRsr = r.rsr;
			return r;
		};
		if (!wakeMode) { stage = 15; return; }
		if (!results[2].valid) { failure = "power-before-invalid"; return; }
		wakeNeeded = results[2].value != 0x11;
		if (wakeNeeded) {
			stage = 20;
			port.timeoutUs = 1000000;
			requestSense = wakeSubmit(Command::RequestSense);
			if (!requestSense.valid) { failure = "request-sense"; return; }
			stage = 21;
			wake = wakeSubmit(Command::Wake);
			if (!wake.valid) { failure = "ssu-active"; return; }
			IOSleep(50);
		}
		port.timeoutUs = 300000;
		stage = 22;
		for (unsigned i = 0; i < 20; i++) {
			++powerTries;
			powerAfter = wakeSubmit(Command::Power);
			if (transport.poisoned || !powerAfter.valid) { failure = "power-after-invalid"; return; }
			if (powerAfter.value == 0x11) break;
			IOSleep(20);
		}
		if (powerAfter.value != 0x11) { failure = "still-non-active"; return; }
		stage = 23;
		for (unsigned i = 0; i < 20; i++) {
			++turTries;
			turAfter = wakeSubmit(Command::Tur);
			if (transport.poisoned) { failure = "tur-after-transport"; return; }
			if (turAfter.valid) break;
			if (turAfter.status != Status::Scsi ||
			    (turAfter.sense != 0x020401 && turAfter.sense != 0x020402 &&
			     (turAfter.sense & 0xff0000U) != 0x060000U)) break;
			IOSleep(20);
		}
		stage = 24;
		capAfter = wakeSubmit(Command::Capacity);
		if (transport.poisoned) { failure = "cap-after-transport"; return; }
		stage = 25;
		if (!gptMode) return;
		if (!turAfter.valid || !capAfter.valid) { failure = "block-prerequisite"; return; }
		stage = 30;
		port.timeoutUs = 300000;
		Probe61Reader reader{port, transport, uint64_t(be32(capAfter.data)) + 1, {}};
		if (blockMode) reader.cancelled = &blockCancelled;
		clock_interval_to_deadline(productionMode ? 10000000 : 1500000, kMicrosecondScale, &reader.deadline);
			if (partitionMode) {
				SmUfsPartitionRef::Observer<Probe61Reader> capture{reader, gProbe61Gpt, gProbe67Reference};
				SmUfsInventory::EntryReader<SmUfsPartitionRef::Observer<Probe61Reader>> observer{capture, gProbe61Gpt, gProbe62Inventory};
				SmUfsGpt::inspect(observer, reader.blocks, gProbe61Gpt);
				SmUfsInventory::trust(gProbe62Inventory, gProbe61Gpt);
			} else if (inventoryMode) {
				SmUfsInventory::EntryReader<Probe61Reader> observer{reader, gProbe61Gpt, gProbe62Inventory};
			SmUfsGpt::inspect(observer, reader.blocks, gProbe61Gpt);
			SmUfsInventory::trust(gProbe62Inventory, gProbe61Gpt);
		} else {
			SmUfsGpt::inspect(reader, reader.blocks, gProbe61Gpt);
		}
		blockAfter = reader.last;
		if (blockAfter.issued) {
			lastHcs = blockAfter.hcs; lastIs = blockAfter.isAfter;
			lastDbr = blockAfter.dbr; lastRsr = blockAfter.rsr;
		}
		if (reader.budgetExpired) { failure = "gpt-budget"; return; }
		if (gProbe61Gpt.status != SmUfsGpt::Status::Success) { failure = "gpt-check"; return; }
		stage = 31;
			// H1 production gate: same validated transport, then a persistent
			// read-only whole-LUN media. No diagnostic budget, tests or panic.
			if (productionMode) {
				stage = 90;
				context->lock();
				context->blocks = uint64_t(be32(capAfter.data)) + 1;
				context->last = blockAfter;
				context->online = !__atomic_load_n(&blockCancelled, __ATOMIC_ACQUIRE);
				context->unlock();
				if (!context->ready() || gProbe61Gpt.userMatches != 1 ||
				    gProbe61Gpt.userFirst > gProbe61Gpt.userLast) {
					failure = "production-state";
					return;
				}
				printf("PLATFORM-DMA ufs pa=%llx bytes=%x utrl=%llx task=%llx\n",
				       owned.phys, kBytes, owned.listReadback, owned.taskReadback);
				const auto &gpt = gProbe61Gpt;
				printf("UFS-LAYOUT gpt lun=0 block_bytes=4096 blocks=%llx userdata=%llx..%llx primary_crc=%x entries_crc=%x\n",
				       context->blocks, gpt.userFirst, gpt.userLast, gpt.headerComputed, gpt.tableComputed);
				SmUfsRootProduction::start(this, *context, gpt.userFirst, gpt.userLast);
				stage = 91;
				return;
			}
			if (inventoryMode && !gProbe62Inventory.trusted) { failure = "inventory-untrusted"; return; }
			if (partitionMode && !SmUfsPartitionRef::validate(gProbe67Reference, gProbe61Gpt, gProbe62Inventory, reader.blocks)) {
				failure = "partition-reference"; return;
			}
		if (volumeMode) {
			stage = 32;
			clock_interval_to_deadline(10000000, kMicrosecondScale, &reader.deadline);
			SmUfsInventory::scan(reader, gProbe62Inventory, reader.blocks, gProbe63Volumes);
			blockAfter = reader.last;
			if (blockAfter.issued) {
				lastHcs = blockAfter.hcs; lastIs = blockAfter.isAfter;
				lastDbr = blockAfter.dbr; lastRsr = blockAfter.rsr;
			}
			if (reader.budgetExpired) { failure = "volume-budget"; return; }
			if (!gProbe63Volumes.valid) { failure = "volume-check"; return; }
			stage = 33;
		}
		if (blockMode) {
			stage = 40;
			context->lock();
			context->blocks = uint64_t(be32(capAfter.data)) + 1;
			context->last = blockAfter;
			context->online = !__atomic_load_n(&blockCancelled, __ATOMIC_ACQUIRE);
			context->unlock();
			IOLockUnlock(blockStartupGate); startupLocked = false;
			if (__atomic_load_n(&blockCancelled, __ATOMIC_ACQUIRE)) { failure = "cancelled"; return; }
			if (mediaMode) {
				stage = 50;
				probe66Tests(*this, *context, reader, gProbe61Gpt, gProbe62Inventory, gProbe63Volumes);
				context->lock();
				blockAfter = context->last;
				const bool ready = context->ready();
				context->unlock();
				if (blockAfter.issued) {
					lastHcs = blockAfter.hcs; lastIs = blockAfter.isAfter;
					lastDbr = blockAfter.dbr; lastRsr = blockAfter.rsr;
				}
				if (!gProbe66Report.all || !ready) { failure = "media-api"; return; }
				if (partitionMode) {
					stage = 60;
					context->lock();
					context->budget.enabled = true; context->budget.limit = 256;
					clock_interval_to_deadline(20000, kMillisecondScale, &context->budget.deadline);
					context->unlock();
					probe67Tests(*context, gProbe61Gpt, gProbe62Inventory, gProbe63Volumes, gProbe67Reference);
					context->lock();
					const bool partitionsReady = context->ready() && context->budget.stop == SmUfsReadBudget::Stop::None;
					blockAfter = context->last;
					context->unlock();
					if (!gProbe67Report.all || !partitionsReady) { failure = "partition-api"; return; }
					if (bsdMode) {
						stage = 70;
						context->lock();
						context->budget = SmUfsReadBudget::State{};
						context->budget.enabled = true; context->budget.limit = 256;
						clock_interval_to_deadline(20000, kMillisecondScale, &context->budget.deadline);
						context->unlock();
						probe68Tests(*context, gProbe61Gpt, gProbe62Inventory, gProbe63Volumes);
						context->lock();
						const bool bsdReady = context->ready() && context->budget.stop == SmUfsReadBudget::Stop::None;
						blockAfter = context->last;
						context->unlock();
						if (!gProbe68Report.all || !bsdReady) { failure = "bsd-api"; return; }
						if (blockVfsMode) {
							stage = 80;
							context->lock();
							context->budget = SmUfsReadBudget::State{};
							context->budget.enabled = true; context->budget.limit = 256;
							clock_interval_to_deadline(20000, kMillisecondScale, &context->budget.deadline);
							context->unlock();
							probe69Tests(*context, gProbe61Gpt, gProbe62Inventory, gProbe63Volumes);
							context->lock();
							const bool layerReady = context->ready() && context->budget.stop == SmUfsReadBudget::Stop::None;
							blockAfter = context->last;
							context->unlock();
							const Probe69Report &r = gProbe69Report;
							if (!r.block || (!r.vfs && !r.blocked) || !layerReady) { failure = "block-vfs-api"; return; }
							stage = 83;
							return;
						}
						stage = 74;
						return;
					}
					stage = 64;
					return;
				}
				stage = 54;
				return;
			}
			probe65Tests(*context, gProbe61Gpt, gProbe62Inventory, gProbe63Volumes);
			context->lock();
			blockAfter = context->last;
			const bool ready = context->ready();
			context->unlock();
			if (blockAfter.issued) {
				lastHcs = blockAfter.hcs; lastIs = blockAfter.isAfter;
				lastDbr = blockAfter.dbr; lastRsr = blockAfter.rsr;
			}
			if (!gProbe65Tests.all || !ready) { failure = "block-api"; return; }
			stage = 45;
		}
	};
	run();
	if (rootTraceMode) probe70Capture(1);
	if (productionMode) SmUfsRootProduction::gReport.stage = stage;
	if (startupLocked) IOLockUnlock(blockStartupGate);
	// Production never closes admission or panics here: the service stays online
	// until the delayed H1 report in NativeUfs::productionReport().
	if (productionMode) return;
	// Only saved CPU memory is used below: a timeout must never trigger another DMA write/ring.
	if (blockVfsMode) {
		const bool finalized = probe69Finalize(*context);
		context->lock();
		const bool beforeClose = context->ready();
		context->online = false; context->stopping = true;
		context->unlock();
		uint64_t deadline; clock_interval_to_deadline(5000, kMillisecondScale, &deadline);
		while (__atomic_load_n(&context->inflight, __ATOMIC_ACQUIRE)) {
			if (mach_absolute_time() >= deadline) panic("SMUFS v22 drain-timeout; pending resources held");
			IOSleep(1);
		}
		bool refused = false;
		if (probe66Device()) {
			static Probe65Call terminal;
			terminal.device = probe66Device(); terminal.count = 1;
			if (probe65Buffer(terminal, 4096)) {
				const uint32_t submits = context->submits;
				probe65Perform(terminal);
				refused = terminal.returned == kIOReturnNotReady && !terminal.callbacks && context->submits == submits;
			}
		}
		context->lock();
		const Probe69Report &r = gProbe69Report;
		const bool drained = !__atomic_load_n(&context->inflight, __ATOMIC_ACQUIRE);
		const bool clean = stage == 83 && r.block && finalized && r.closed && r.stable && r.isolation &&
		                   beforeClose && drained && refused && !transport.poisoned &&
		                   context->budget.stop == SmUfsReadBudget::Stop::None && context->accepted == context->completions;
		const bool success = clean && r.vfs && r.all && !r.blocked;
		const bool blockOnly = clean && !r.vfs && r.blocked;
		if (success) stage = 85;
		else if (blockOnly) stage = 84;
		else if (stage == 83) failure = "block-vfs-terminal";
		blockAfter = context->last;
		if (rootTraceMode) {
			probe70Capture(2);
			const bool traceDone = clean && probe70Valid();
			if (traceDone) stage = success ? 95 : 94;
			Probe59Text rootText;
			rootText.add("SMUFS capdiag v23-root-trace probe70\n");
			rootText.add("stage%u stop %s poison%u diag %s\n", stage, failure, transport.poisoned ? 1u : 0u,
			             traceDone ? "done" : "incomplete");
			rootText.add("storage%u block%u vfs%u blocked%u reason %s\n", clean ? 1u : 0u,
			             r.block ? 1u : 0u, r.vfs ? 1u : 0u, r.blocked ? 1u : 0u, r.reason);
			probe70Report(rootText);
			rootText.add("ready root%u devfs%u paths%u/%u/%u callbacks%u/%u\n", unsigned(r.root), unsigned(r.devfs),
			             r.vfsOpens, r.vfsCloses, r.vfsReads, r.callbackCount, r.submitCount);
			rootText.add("budget%u/%u vfs%u/%u workers%u/%u active%u\n", r.blockBudgetUsed, r.blockBudgetLimit,
			             r.vfsBudgetUsed, r.vfsBudgetLimit, r.workersCompleted, r.workerCount, r.activeWorkers);
			rootText.add("terminal close%u stable%u drain%u refuse%u inflight%u\n", finalized ? 1u : 0u,
			             r.stable ? 1u : 0u, drained ? 1u : 0u, refused ? 1u : 0u, context->inflight);
			rootText.add("requests%u/%u rejects%u submits%u last%u e%x", context->accepted, context->completions,
			             context->rejected, context->submits, unsigned(blockAfter.status), blockAfter.error);
			context->unlock();
			panic("%s", rootText.bytes);
		}
		Probe59Text text;
		text.add("SMUFS capdiag v22-block-vfs probe69\n");
		text.add("stage %u stop %s poison%u\n", stage, failure, transport.poisoned ? 1u : 0u);
		text.add("outcome %s test%u case%u slot%u\n", success ? "all-success" : blockOnly ? "block-only" : "stopped",
		         r.stage, r.caseIndex, r.failureSlot);
		text.add("block%u vfs%u blocked%u reason %s\n", r.block ? 1u : 0u, r.vfs ? 1u : 0u, r.blocked ? 1u : 0u, r.reason);
		text.add("prefix bsd%u parts%u count%u callbacks%u/%u\n", gProbe68Report.all ? 1u : 0u,
		         gProbe67Report.all ? 1u : 0u, gProbe67Report.attachedCount, r.callbackCount, r.submitCount);
		text.add("strategy cases%u/%u cb-active%u isolation%u\n", r.completedCases, r.strategyCases,
		         r.callbackActive, r.isolation ? 1u : 0u);
		text.add("block-open%u close%u native-opens%u/%u\n", r.openCount, r.closeCount, r.blockOpenCount, r.characterOpenCount);
		text.add("root%u devfs%u ready%u/%u paths-open%u close%u read%u\n", unsigned(r.root), unsigned(r.devfs),
		         r.rootReady ? 1u : 0u, r.devfsReady ? 1u : 0u, r.vfsOpens, r.vfsCloses, r.vfsReads);
		text.add("block-budget%u/%u vfs-budget%u/%u stop%u\n", r.blockBudgetUsed, r.blockBudgetLimit,
		         r.vfsBudgetUsed, r.vfsBudgetLimit, unsigned(context->budget.stop));
		text.add("workers%u/%u active%u\n", r.workersCompleted, r.workerCount, r.activeWorkers);
		text.add("op%u errno%d readiness%d residual%llu count%llu size%llu\n", r.operation, r.opError,
		         r.readinessError, r.lastResidual, r.lastCount, r.lastSize);
		text.add("accepted%u completed%u rejected%u submits%u\n", context->accepted, context->completions, context->rejected, context->submits);
		text.add("terminal closed%u stable%u drain%u refuse%u inflight%u\n", finalized ? 1u : 0u, r.stable ? 1u : 0u,
		         drained ? 1u : 0u, refused ? 1u : 0u, context->inflight);
		text.add("owned valid%u held%u selected%u pool%llx\n", owned.validated ? 1u : 0u, memory ? 1u : 0u,
		         owned.selected ? 1u : 0u, owned.phys);
		text.add("last st%u i%u c%u v%u o%x e%x\n", unsigned(blockAfter.status), blockAfter.issued ? 1u : 0u,
		         blockAfter.complete ? 1u : 0u, blockAfter.valid ? 1u : 0u, blockAfter.ocs, blockAfter.error);
		text.add("last is%x db%x hc%x rs%x", blockAfter.isAfter, blockAfter.dbr, blockAfter.hcs, blockAfter.rsr);
		context->unlock();
		panic("%s", text.bytes);
	}
	if (bsdMode) {
		const bool finalized = probe68Finalize(*context);
		context->lock();
		const bool beforeClose = context->ready();
		context->online = false; context->stopping = true;
		context->unlock();
		uint64_t deadline; clock_interval_to_deadline(5000, kMillisecondScale, &deadline);
		while (__atomic_load_n(&context->inflight, __ATOMIC_ACQUIRE)) {
			if (mach_absolute_time() >= deadline) panic("SMUFS v21 drain-timeout; resources held");
			IOSleep(1);
		}
		bool refused = false;
		if (probe66Device()) {
			static Probe65Call terminal;
			terminal.device = probe66Device(); terminal.count = 1;
			if (probe65Buffer(terminal, 4096)) {
				const uint32_t submits = context->submits;
				probe65Perform(terminal);
				refused = terminal.returned == kIOReturnNotReady && !terminal.callbacks && context->submits == submits;
			}
		}
		context->lock();
		const Probe68Report &r = gProbe68Report;
		const bool drained = !__atomic_load_n(&context->inflight, __ATOMIC_ACQUIRE);
		const bool success = stage == 74 && r.all && finalized && beforeClose && drained && refused &&
		                     !transport.poisoned && context->budget.stop == SmUfsReadBudget::Stop::None &&
		                     context->accepted == context->completions;
		if (success) stage = 75;
		else if (stage == 74) failure = "bsd-terminal";
		blockAfter = context->last;
		Probe59Text text;
		text.add("SMUFS capdiag v21-bsd-read probe68\n");
		text.add("stage %u stop %s poison%u\n", stage, failure, transport.poisoned ? 1u : 0u);
		text.add("outcome %s test%u slot%u %s\n", success ? "all-success" : "stopped", r.stage, r.failingSlot, r.reason);
		text.add("prefix media%u parts%u count%u ref%u abi%u bsd%x\n", gProbe66Report.all ? 1u : 0u,
		         gProbe67Report.all ? 1u : 0u, gProbe67Report.attachedCount, gProbe67Reference.trusted ? 1u : 0u,
		         r.abi ? 1u : 0u, r.bsdClassSize);
		text.add("clients%u init%u attach%u start%u create%u reg-held%u\n", r.clientCount, r.init ? 1u : 0u,
		         r.attach ? 1u : 0u, r.start ? 1u : 0u, r.createCount, r.registrations);
		text.add("nodes%u identity%u chain%u ro%u isolated%u\n", r.nodes ? 1u : 0u, r.identity ? 1u : 0u,
		         r.ancestry ? 1u : 0u, r.readOnly ? 1u : 0u, r.isolation ? 1u : 0u);
		text.add("whole unit%u minor%u dev%x/%x\n", r.wholeUnit, r.wholeMinor, r.wholeBlockDev, r.wholeCharacterDev);
		text.add("user unit%u minor%u dev%x/%x\n", r.userUnit, r.userMinor, r.userBlockDev, r.userCharacterDev);
		text.add("reader%u geom%u writer-deny%u raw%u negatives%u\n", r.readerOpenCount, r.geometryCount,
		         r.writerDeniedCount, r.rawReadCount, r.negativeCount);
		text.add("reads gpt%u head%u tail%u repeat17%u align%u eof%u clip%u\n", r.gpt ? 1u : 0u,
		         r.userHead ? 1u : 0u, r.userTail ? 1u : 0u, r.repeat17 ? 1u : 0u,
		         r.alignment ? 1u : 0u, r.eof ? 1u : 0u, r.clip ? 1u : 0u);
		text.add("split17 req%u last%u max%u budget%u/%u stop%u\n", r.requests17, r.last17, r.maxProviderBlocks,
		         context->budget.issued, context->budget.limit, unsigned(context->budget.stop));
		text.add("workers%u/%u active%u close%u opens%u/%u\n", r.workersCompleted, r.workerCount, r.activeWorkers,
		         r.closeCount, r.blockOpenCount, r.characterOpenCount);
		text.add("op%u errno%d resid%llu offset%llx\n", r.operation, r.lastError, r.lastResidual, r.lastOffset);
		text.add("accepted%u completed%u rejected%u submits%u\n", context->accepted, context->completions, context->rejected, context->submits);
		text.add("terminal closed%u drain%u refused%u inflight%u\n", finalized ? 1u : 0u, drained ? 1u : 0u,
		         refused ? 1u : 0u, context->inflight);
		text.add("last st%u i%u c%u v%u o%x e%x\n", unsigned(blockAfter.status), blockAfter.issued ? 1u : 0u,
		         blockAfter.complete ? 1u : 0u, blockAfter.valid ? 1u : 0u, blockAfter.ocs, blockAfter.error);
		text.add("last is%x db%x hc%x rs%x", blockAfter.isAfter, blockAfter.dbr, blockAfter.hcs, blockAfter.rsr);
		context->unlock();
		panic("%s", text.bytes);
	}
	if (partitionMode) {
		const bool finalized = probe67Finalize(*context);
		context->lock();
		const bool beforeClose = context->ready();
		context->online = false; context->stopping = true;
		context->unlock();
		uint64_t deadline; clock_interval_to_deadline(5000, kMillisecondScale, &deadline);
		while (__atomic_load_n(&context->inflight, __ATOMIC_ACQUIRE)) {
			if (mach_absolute_time() >= deadline) panic("SMUFS v20 drain-timeout; resources held");
			IOSleep(1);
		}
		bool refused = false;
		if (probe66Device()) {
			static Probe65Call terminal;
			terminal.device = probe66Device(); terminal.count = 1;
			if (probe65Buffer(terminal, 4096)) {
				const uint32_t submits = context->submits;
				probe65Perform(terminal);
				refused = terminal.returned == kIOReturnNotReady && !terminal.callbacks && context->submits == submits;
			}
		}
		context->lock();
		const Probe67Report &r = gProbe67Report;
		const bool drained = !__atomic_load_n(&context->inflight, __ATOMIC_ACQUIRE);
		const bool success = stage == 64 && r.all && finalized && beforeClose && drained && refused &&
		                     !transport.poisoned && context->budget.stop == SmUfsReadBudget::Stop::None &&
		                     context->accepted == context->completions;
		if (success) stage = 65;
		else if (stage == 64) failure = "partition-terminal";
		blockAfter = context->last;
		Probe59Text text;
		text.add("SMUFS capdiag v20-gpt-media probe67\n");
		text.add("stage %u stop %s poison%u\n", stage, failure, transport.poisoned ? 1u : 0u);
		text.add("outcome %s test%u slot%u %s\n", success ? "all-success" : "stopped", r.stage, r.failingSlot, r.reason);
		text.add("prefix media%u gpt%u %u/%u samples%u/%u ref%u\n", gProbe66Report.all ? 1u : 0u,
		         unsigned(gProbe61Gpt.status), gProbe61Gpt.successfulReads, gProbe61Gpt.reads,
		         gProbe63Volumes.successfulReads, gProbe63Volumes.reads, gProbe67Reference.trusted ? 1u : 0u);
		text.add("abi%u guid%x part%x storage%x media%x\n", r.abi ? 1u : 0u, r.schemeClassSize,
		         r.partitionClassSize, r.storageClassSize, r.mediaClassSize);
		text.add("probe%u start%u counts%u factory%u set%u attach%u\n", r.probe ? 1u : 0u, r.start ? 1u : 0u,
		         r.counts ? 1u : 0u, r.factoryCount, r.setCount, r.attachedCount);
		text.add("fields geom%u names%u guid%u attrs%u chain%u\n", r.geometry ? 1u : 0u, r.names ? 1u : 0u,
		         r.guids ? 1u : 0u, r.attrs ? 1u : 0u, r.ancestry ? 1u : 0u);
		text.add("isolate%u reg-held%u scheme%u dt-held%u/%u\n", r.isolation ? 1u : 0u, r.registeredCount,
		         r.schemeRegistrations, r.dtAttachCount, r.dtDetachCount);
		text.add("readers%u writer-deny%u head%u tail%u edge%u\n", r.readerCount, r.writerDeniedCount,
		         r.headCount, r.tailCount, r.edgeCount);
		text.add("reenter%u negative%u cases%u callbacks%u\n", r.reentry ? 1u : 0u,
		         r.negatives ? 1u : 0u, r.negativeCount, r.callbackCount);
		text.add("split17 req%u last%u max%u budget%u/%u stop%u\n", r.requests17, r.last17, r.maxProviderBlocks,
		         context->budget.issued, context->budget.limit, unsigned(context->budget.stop));
		text.add("accepted%u completed%u rejected%u submits%u\n", context->accepted, context->completions, context->rejected, context->submits);
		text.add("terminal closed%u drain%u refused%u inflight%u\n", finalized ? 1u : 0u, drained ? 1u : 0u,
		         refused ? 1u : 0u, context->inflight);
		text.add("owned valid%u held%u select%u pool%llx\n", owned.validated ? 1u : 0u, memory ? 1u : 0u,
		         owned.selected ? 1u : 0u, owned.phys);
		text.add("last st%u i%u c%u v%u o%x e%x\n", unsigned(blockAfter.status), blockAfter.issued ? 1u : 0u,
		         blockAfter.complete ? 1u : 0u, blockAfter.valid ? 1u : 0u, blockAfter.ocs, blockAfter.error);
		text.add("last is%x db%x hc%x rs%x", blockAfter.isAfter, blockAfter.dbr, blockAfter.hcs, blockAfter.rsr);
		context->unlock();
		panic("%s", text.bytes);
	}
	if (mediaMode) {
		const bool finalized = probe66Finalize(*context);
		context->lock();
		const bool beforeClose = context->ready();
		context->online = false; context->stopping = true;
		context->unlock();
		uint64_t drainDeadline;
		clock_interval_to_deadline(5000, kMillisecondScale, &drainDeadline);
		while (__atomic_load_n(&context->inflight, __ATOMIC_ACQUIRE)) {
			if (mach_absolute_time() >= drainDeadline)
				panic("SMUFS v19 drain-timeout; admission closed resources held");
			IOSleep(1);
		}
		bool refused = false;
		IOBlockStorageDevice *device = probe66Device();
		if (device) {
			static Probe65Call terminal;
			terminal.device = device; terminal.count = 1;
			if (probe65Buffer(terminal, 4096)) {
				const uint32_t previous = context->submits;
				probe65Perform(terminal);
				refused = terminal.returned == kIOReturnNotReady && !terminal.callbacks && context->submits == previous;
			}
		}
		context->lock();
		const Probe66Report &r = gProbe66Report;
		const bool drained = !__atomic_load_n(&context->inflight, __ATOMIC_ACQUIRE);
		const bool success = stage == 54 && r.all && finalized && beforeClose && drained && refused &&
		                     !transport.poisoned && context->accepted == context->completions;
		if (success) stage = 55;
		else if (stage == 54) failure = "media-terminal";
		blockAfter = context->last;
		Probe59Text text;
		text.add("SMUFS capdiag v19-media probe66\n");
		text.add("stage %u stop %s poison%u\n", stage, failure, transport.poisoned ? 1u : 0u);
		text.add("outcome %s test%u reason %s\n", success ? "all-success" : "stopped", r.stage, r.reason);
		text.add("owned valid%u held%u select%u pool%llx\n", owned.validated ? 1u : 0u, memory ? 1u : 0u,
		         owned.selected ? 1u : 0u, owned.phys);
		text.add("prefix gpt%u %u/%u samples%u/%u\n", unsigned(gProbe61Gpt.status), gProbe61Gpt.successfulReads,
		         gProbe61Gpt.reads, gProbe63Volumes.successfulReads, gProbe63Volumes.reads);
		text.add("abi%u sizes %x/%x/%x/%x/%x\n", r.abi ? 1u : 0u, r.serviceClassSize, r.blockClassSize,
		         r.storageClassSize, r.driverClassSize, r.mediaClassSize);
		text.add("stack obj%u attach%u start%u media%u limits%u\n", r.constructed ? 1u : 0u, r.attached ? 1u : 0u,
		         r.started ? 1u : 0u, r.media ? 1u : 0u, r.limits ? 1u : 0u);
		text.add("reader%u writer-deny%u isolated%u reg-held%u/%u\n", r.readerOpen ? 1u : 0u, r.writerDenied ? 1u : 0u,
		         r.isolated ? 1u : 0u, r.driverRegistrations, r.mediaRegistrations);
		text.add("reads gpt%u user%u aligned%u unalign%u cross%u\n", r.gpt ? 1u : 0u, r.userdata ? 1u : 0u,
		         r.aligned ? 1u : 0u, r.unaligned ? 1u : 0u, r.crossBlock ? 1u : 0u);
		text.add("split16 %u req%u split17 %u req%u last%u max%u\n", r.read16 ? 1u : 0u, r.requests16,
		         r.read17 ? 1u : 0u, r.requests17, r.last17, r.maxProviderBlocks);
		text.add("edge block%u byte%u reenter%u negative%u cases%u\n", r.lastBlock ? 1u : 0u, r.lastByte ? 1u : 0u,
		         r.reenter ? 1u : 0u, r.negatives ? 1u : 0u, r.negativeCases);
		text.add("crc %08x %08x %08x user%08x\n", r.gptCrc[0], r.gptCrc[1], r.gptCrc[2], r.userdataCrc);
		text.add("accepted%u completed%u rejected%u submits%u\n", context->accepted, context->completions, context->rejected, context->submits);
		text.add("terminal closed%u drain%u refused%u inflight%u\n", finalized ? 1u : 0u, drained ? 1u : 0u,
		         refused ? 1u : 0u, context->inflight);
		text.add("last st%u i%u c%u v%u o%x e%x\n", unsigned(blockAfter.status), blockAfter.issued ? 1u : 0u,
		         blockAfter.complete ? 1u : 0u, blockAfter.valid ? 1u : 0u, blockAfter.ocs, blockAfter.error);
		text.add("last is%x db%x hc%x rs%x", blockAfter.isAfter, blockAfter.dbr, blockAfter.hcs, blockAfter.rsr);
		context->unlock();
		panic("%s", text.bytes);
	}
	if (blockMode) {
		context->lock();
		const Probe65TestReport &r = gProbe65Tests;
		Probe59Text text;
		text.add("SMUFS capdiag v18-block-api probe65\n");
		text.add("stage %u stop %s poison%u\n", stage, failure, transport.poisoned ? 1u : 0u);
		text.add("outcome %s registered0 attached0\n", stage == 45 && r.all ? "all-success" : "stopped");
		text.add("owned alloc%u prep%u valid%u held%u select%u err%u\n", owned.allocated ? 1u : 0u,
		         owned.prepared ? 1u : 0u, owned.validated ? 1u : 0u, memory ? 1u : 0u,
		         owned.selected ? 1u : 0u, unsigned(owned.error));
		text.add("pool %llx contig%llx list%llx task%llx\n", owned.phys, owned.contiguous, owned.listReadback, owned.taskReadback);
		text.add("capacity %08x %08x sid%x/%x\n", be32(capAfter.data), be32(capAfter.data + 4), sid0, sid1);
		text.add("preflight gpt%u try%u ok%u samples%u/%u valid%u\n", unsigned(gProbe61Gpt.status), gProbe61Gpt.reads,
		         gProbe61Gpt.successfulReads, gProbe63Volumes.reads, gProbe63Volumes.successfulReads, gProbe63Volumes.valid ? 1u : 0u);
		text.add("abi%u sizes block%x service%x registered0\n", r.abi ? 1u : 0u, r.blockClassSize, r.serviceClassSize);
		text.add("api object%u geom%u single%u multi%u user%u edge%u\n", r.constructed ? 1u : 0u,
		         r.geometry ? 1u : 0u, r.single ? 1u : 0u, r.multi ? 1u : 0u, r.userdata ? 1u : 0u, r.lastBlock ? 1u : 0u);
		text.add("parallel%u reentry%u negatives%u cases%u\n", r.concurrent ? 1u : 0u, r.reenter ? 1u : 0u,
		         r.negatives ? 1u : 0u, r.negativeCases);
		text.add("single crc%08x multi %08x %08x %08x\n", r.singleCrc, r.multiCrc[0], r.multiCrc[1], r.multiCrc[2]);
		text.add("userdata crc%08x callers %08x %08x\n", r.userdataCrc, r.callerCrc[0], r.callerCrc[1]);
		text.add("requests accepted%u completed%u rejected%u\n", context->accepted, context->completions, context->rejected);
		text.add("submits%u max-active%u active%u denied-mutations%u\n", context->submits, context->maxActive,
		         context->active - 1, context->deniedMutations);
		text.add("last st%u i%u c%u v%u o%x e%x\n", unsigned(blockAfter.status), blockAfter.issued ? 1u : 0u,
		         blockAfter.complete ? 1u : 0u, blockAfter.valid ? 1u : 0u, blockAfter.ocs, blockAfter.error);
		text.add("last is%x db%x hc%x rs%x", lastIs, lastDbr, lastHcs, lastRsr);
		context->unlock();
		panic("%s", text.bytes);
	}
	if (ownedMode) {
		const SmUfsOwned::State &s = gProbe64Owned;
		const SmUfsGpt::Report &g = gProbe61Gpt;
		const SmUfsInventory::Volumes &v = gProbe63Volumes;
		Probe59Text text;
		text.add("SMUFS capdiag v17-owned-dma probe64\n");
		text.add("stage %u stop %s poison %u\n", stage, failure, transport.poisoned ? 1u : 0u);
		const bool success = stage == 33 && s.selected && gProbe62Inventory.trusted && v.valid && !transport.poisoned;
		text.add("outcome %s\n", success ? "all-success" : "stopped");
		text.add("owned alloc%u prep%u valid%u held%u err%u\n", s.allocated ? 1u : 0u, s.prepared ? 1u : 0u,
		         s.validated ? 1u : 0u, gProbe64Memory ? 1u : 0u, unsigned(s.error));
		text.add("cap %08x addr64%u prepare%08x\n", s.cap, (s.cap & SmUfsOwned::kAddress64) ? 1u : 0u, s.prepareResult);
		text.add("pool %llx bytes%x contig%llx\n", s.phys, kBytes, s.contiguous);
		text.add("list old%llx new%llx got%llx\n", oldList, list, s.listReadback);
		text.add("task old%llx new%llx got%llx\n", oldTask, taskList, s.taskReadback);
		text.add("select tried%u valid%u task-rs%x db%x\n", s.selectAttempted ? 1u : 0u, s.selected ? 1u : 0u,
		         s.taskRsrReadback, s.taskDbrReadback);
		text.add("sid e0 %x 4e0 %x\n", sid0, sid1);
		text.add("power %08x>%08x ssu%u tur%u cap%u\n", results[2].value, powerAfter.value,
		         wake.valid ? 1u : 0u, turAfter.valid ? 1u : 0u, capAfter.valid ? 1u : 0u);
		text.add("capacity %08x %08x\n", be32(capAfter.data), be32(capAfter.data + 4));
		text.add("gpt st%u try%u ok%u m%u h%u t%u u%u stable%u\n", unsigned(g.status), g.reads, g.successfulReads,
		         g.mbr ? 1u : 0u, g.header ? 1u : 0u, g.table ? 1u : 0u, g.userdata ? 1u : 0u, g.stable ? 1u : 0u);
		text.add("hdrcrc %08x=%08x tblcrc %08x=%08x\n", g.headerStored, g.headerComputed, g.tableStored, g.tableComputed);
		text.add("entries %u used%u trusted%u\n", g.entryCount, g.used, gProbe62Inventory.trusted ? 1u : 0u);
		text.add("samples %u/%u complete%u/%u valid%u\n", v.reads, v.successfulReads, v.completed,
		         gProbe62Inventory.count, v.valid ? 1u : 0u);
		text.add("userdata %llx..%llx blocks%llu\n", g.userFirst, g.userLast, g.userBlocks);
		text.add("repeat mbr%08x hdr%08x last-lba%llx\n", g.mbrCrc, g.headerBlockCrc, v.lastLba);
		text.add("last st%u i%u c%u v%u o%x e%x\n", unsigned(blockAfter.status), blockAfter.issued ? 1u : 0u,
		         blockAfter.complete ? 1u : 0u, blockAfter.valid ? 1u : 0u, blockAfter.ocs, blockAfter.error);
		text.add("last is%x db%x hc%x rs%x", lastIs, lastDbr, lastHcs, lastRsr);
		panic("%s", text.bytes);
	}
	if (inventoryMode) {
		uint32_t page = 0;
		PE_parse_boot_argn("native-ufs-page", &page, sizeof(page));
		const unsigned width = PE_state.video.v_width ? unsigned(PE_state.video.v_width) : 1440;
		const unsigned height = PE_state.video.v_height ? unsigned(PE_state.video.v_height) : 3168;
		const unsigned columns = width / 18;
		const unsigned rows = height > height / 8 + 8 ? (height - height / 8 - 8) / 42 : 0;
		const unsigned rowBudget = rows > 8 ? rows - 8 : 0;
		SmUfsInventory::RecordText summary;
		summary.str("stage "); summary.num(stage); summary.str(" stop "); summary.str(failure);
		summary.str(" poison"); summary.num(transport.poisoned ? 1 : 0);
		summary.str(" st"); summary.num(unsigned(blockAfter.status));
		summary.str(" i"); summary.num(blockAfter.issued ? 1 : 0); summary.str(" o"); summary.num(blockAfter.ocs, 16);
		summary.str(" e"); summary.num(blockAfter.error, 16); summary.ch('\n');
		summary.str("last-lba "); summary.num(volumeMode ? gProbe63Volumes.lastLba : gProbe61Gpt.lastRead, 16);
		summary.str(" is"); summary.num(lastIs, 16); summary.str(" db"); summary.num(lastDbr, 16);
		summary.str(" hc"); summary.num(lastHcs, 16); summary.str(" rs"); summary.num(lastRsr, 16);
		summary.ch('\n');
		SmUfsInventory::ReportText &out = gProbe62Text;
		if (volumeMode) SmUfsInventory::volumeReport(out, gProbe62Inventory, gProbe61Gpt, gProbe63Volumes,
		                                             page, columns, rowBudget, summary.bytes);
		else SmUfsInventory::inventoryReport(out, gProbe62Inventory, gProbe61Gpt, page, columns,
		                                    rowBudget, summary.bytes);
		const unsigned textRows = SmUfsInventory::rows(out, columns ? columns : 1);
		if (out.overflow || summary.overflow || textRows > rowBudget) {
			panic("SMUFS v16b report-capacity p%u chars%u rows%u/%u\n%s", page, out.used,
			      textRows, rowBudget, summary.bytes);
		}
		panic("%s", out.bytes);
	}
	Probe59Text text;
	if (gptMode) {
		const SmUfsGpt::Report &g = gProbe61Gpt;
		text.add("SMUFS capdiag v15-gpt probe61\n");
		text.add("stage %u stop %s poison %u\n", stage, failure, transport.poisoned ? 1u : 0u);
		text.add("outcome %s\n", stage == 31 && g.status == SmUfsGpt::Status::Success ? "all-success" : "stopped");
		text.add("list %llx ucd %llx sid %x %x\n", list, list + kUcd, sid0, sid1);
		text.add("power %08x>%08x ssu-v%u tur-v%u cap-v%u\n", results[2].value, powerAfter.value,
		         wake.valid ? 1u : 0u, turAfter.valid ? 1u : 0u, capAfter.valid ? 1u : 0u);
		text.add("capacity %08x %08x\n", be32(capAfter.data), be32(capAfter.data + 4));
		text.add("gpt st%u try%u ok%u last-lba %llx\n", unsigned(g.status), g.reads, g.successfulReads, g.lastRead);
		text.add("last-ok-lba %llx\n", g.lastSuccessfulLba);
		text.add("mbr-v%u hdr-v%u table-v%u user-v%u stable%u\n", g.mbr ? 1u : 0u, g.header ? 1u : 0u,
		         g.table ? 1u : 0u, g.userdata ? 1u : 0u, g.stable ? 1u : 0u);
		text.add("block st%u i%u c%u v%u o%02x e%x r%08x\n", unsigned(blockAfter.status),
		         blockAfter.issued ? 1u : 0u, blockAfter.complete ? 1u : 0u, blockAfter.valid ? 1u : 0u,
		         blockAfter.ocs, blockAfter.error, be32(blockAfter.response + 4));
		text.add("block sense %06x detail%u\n", blockAfter.sense, blockAfter.detail);
		const uint8_t *rsp = blockAfter.response;
		text.add("rsp %08x %08x %08x %08x\n", be32(rsp), be32(rsp + 4), be32(rsp + 8), be32(rsp + 12));
		if (!blockAfter.valid) {
			text.add("rsp+ %08x %08x %08x %08x\n", be32(rsp + 16), be32(rsp + 20), be32(rsp + 24), be32(rsp + 28));
			text.add("sense-len %u raw %08x %08x %08x %08x %08x\n", (unsigned(rsp[32]) << 8) | rsp[33],
			         be32(rsp + 32), be32(rsp + 36), be32(rsp + 40), be32(rsp + 44), be32(rsp + 48));
			text.add("flags %02x seg %u residual %u req %u\n", rsp[1], (unsigned(rsp[10]) << 8) | rsp[11],
			         be32(rsp + 12), be32(blockAfter.request + 12));
			text.add("req %08x %08x %08x %08x\n", be32(blockAfter.request), be32(blockAfter.request + 4),
			         be32(blockAfter.request + 8), be32(blockAfter.request + 12));
			text.add("cdb %08x %08x %08x %08x\n", be32(blockAfter.request + 16), be32(blockAfter.request + 20),
			         be32(blockAfter.request + 24), be32(blockAfter.request + 28));
			text.add("utrd %08x %08x %08x %08x\n", blockAfter.utrd[0], blockAfter.utrd[4], blockAfter.utrd[6], blockAfter.utrd[7]);
			text.add("prd %08x %08x %08x %08x dbc%u want4095\n", blockAfter.prdt[0], blockAfter.prdt[1],
			         blockAfter.prdt[2], blockAfter.prdt[3], blockAfter.prdt[3]);
		} else {
			text.add("pmbr sig%04x why%u expect%08x sat%u\n", g.mbrSignature, g.mbrReason, g.mbrExpected,
			         g.mbrSaturated ? 1u : 0u);
			for (unsigned i = 0; i < 4; i++) text.add("mbr%u %08x %08x %08x %08x\n", i,
			         g.mbrEntries[i][0], g.mbrEntries[i][1], g.mbrEntries[i][2], g.mbrEntries[i][3]);
		}
		text.add("data nz%u crc%08x first%08x end%08x mbr%04x\n", blockAfter.dataNonzero,
		         blockAfter.dataCrc, blockAfter.dataFirst, blockAfter.dataLast, blockAfter.dataMbrSignature);
		text.add("hdrmagic %08x %08x\n", g.headerWords[0], g.headerWords[1]);
		text.add("hdrcrc %08x=%08x\n", g.headerStored, g.headerComputed);
		text.add("tblcrc %08x=%08x\n", g.tableStored, g.tableComputed);
		text.add("entries lba%llx count%u size%u used%u\n", g.tableLba, g.entryCount, g.entrySize, g.used);
		text.add("usable %llx..%llx backup %llx\n", g.firstUsable, g.lastUsable, g.alternate);
		text.add("userdata matches%u bad-entry%x\n", g.userMatches, g.badEntry);
		text.add("userdata lba %llx..%llx\n", g.userFirst, g.userLast);
		text.add("userdata blocks %llu bytes %llu\n", g.userBlocks, g.userBlocks * 4096);
		text.add("repeat crc mbr %08x hdr %08x\n", g.mbrCrc, g.headerBlockCrc);
		text.add("last is %x dbr %x hcs %x rsr %x", lastIs, lastDbr, lastHcs, lastRsr);
		panic("%s", text.bytes);
	}
	text.add(wakeMode ? "SMUFS capdiag v9-wake probe60\n" : "SMUFS capdiag v8b-state probe59\n");
	text.add("stage %u stop %s poison %u\n", stage, failure, transport.poisoned ? 1u : 0u);
	const bool allGood = wakeMode ? (results[0].valid && results[1].valid && results[2].valid &&
	                                (!wakeNeeded || (requestSense.valid && wake.valid)) &&
	                                powerAfter.valid && powerAfter.value == 0x11 && turAfter.valid && capAfter.valid) :
	                               (results[0].valid && results[1].valid && results[2].valid &&
	                                results[3].valid && results[4].valid);
	text.add("outcome %s\n", stage != (wakeMode ? 25U : 15U) ? "stopped" : (allGood ? "all-success" : "partial"));
	text.add("list %llx ucd %llx\n", list, list + kUcd);
	text.add("sid e0 %x 4e0 %x rsp+%x prd+%x data+%x\n", sid0, sid1, kUcd + kRsp, kUcd + kPrdt, kData);
	text.add("pre hce %x hcs %x is %x db %x rs %x\n", preHce, preHcs, preIs, preDbr, preRsr);
	text.add("tm %llx db %x rs %x\n", taskList, preTmDbr, preTmRsr);
	const char *names[] = {"nop", "flag", "power", "tur", "cap"};
	auto commandLine = [&](const char *name, const Result &r) {
		text.add("%s st%u i%u c%u v%u o%02x e%x r%08x\n", name, unsigned(r.status),
		         r.issued ? 1u : 0u, r.complete ? 1u : 0u, r.valid ? 1u : 0u,
		         r.ocs, r.error, be32(r.response + 4));
	};
	for (unsigned i = 0; i < (wakeMode ? 4U : 5U); i++) commandLine(names[i], results[i]);
	text.add("flag read%u known%u val %08x\n", results[1].valueRead ? 1u : 0u,
	         results[1].valueKnown ? 1u : 0u, results[1].value);
	text.add("power read%u known%u val %08x\n", results[2].valueRead ? 1u : 0u,
	         results[2].valueKnown ? 1u : 0u, results[2].value);
	if (wakeMode) {
		text.add("wake-needed %u powertries %u turtries %u\n", wakeNeeded ? 1u : 0u, powerTries, turTries);
		commandLine("rsense", requestSense);
		commandLine("ssu", wake);
		commandLine("p-after", powerAfter);
		commandLine("t-after", turAfter);
		commandLine("c-after", capAfter);
		text.add("power %08x>%08x read%u known%u\n", results[2].value, powerAfter.value,
		         powerAfter.valueRead ? 1u : 0u, powerAfter.valueKnown ? 1u : 0u);
		text.add("sense before %06x rs %06x ssu %06x\n", results[3].sense, requestSense.sense, wake.sense);
		text.add("sense after tur %06x cap %06x\n", turAfter.sense, capAfter.sense);
		text.add("rsdata %08x %08x %08x %08x %08x\n", be32(requestSense.data),
		         be32(requestSense.data + 4), be32(requestSense.data + 8),
		         be32(requestSense.data + 12), be32(requestSense.data + 16));
		text.add("capdata %08x %08x\n", be32(capAfter.data), be32(capAfter.data + 4));
	} else {
		text.add("capdata %08x %08x\n", be32(results[4].data), be32(results[4].data + 4));
		for (unsigned i = 3; i < 5; i++) {
			const uint8_t *rsp = results[i].response;
			text.add("%s sk%u sense %06x\n", names[i], results[i].senseKind, results[i].sense);
			text.add("raw %08x %08x %08x %08x %08x\n", be32(rsp + 32),
			         be32(rsp + 36), be32(rsp + 40), be32(rsp + 44), be32(rsp + 48));
		}
	}
	text.add("last is %x dbr %x hcs %x rsr %x", lastIs, lastDbr, lastHcs, lastRsr);
	// Full command snapshots remain in this scope and can be printed before the terminal screen.
	for (unsigned i = 0; i < 5; i++) {
		const Result &r = results[i];
		kprintf("SMUFS59 %s is %08x>%08x db %x hc %x rs %x rsp %08x %08x %08x %08x echo %08x\n",
		        names[i], r.isBefore, r.isAfter, r.dbr, r.hcs, r.rsr, be32(r.response),
		        be32(r.response + 4), be32(r.response + 8), be32(r.response + 12), be32(r.response + 20));
	}
	panic("%s", text.bytes);
}

