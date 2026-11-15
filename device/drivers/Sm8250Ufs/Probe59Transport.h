// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

// Probe 59 only. The host harness supplies MMIO/cache/time primitives around this same code.
namespace SmUfsProbe59 {
constexpr uint32_t kBytes = 0x4000;
constexpr uint32_t kListBytes = 0x400;
constexpr uint32_t kTaskBytes = 0x400;
constexpr uint32_t kUcd = 0x2000;
constexpr uint32_t kRsp = 0x200;
constexpr uint32_t kPrdt = 0x400;
constexpr uint32_t kData = 0x3000;
constexpr uint32_t kDataBytes = 64;
constexpr uint32_t kUcdBytes = kPrdt + 16;
constexpr uint32_t kUcdSyncBytes = (kUcdBytes + 63) & ~63U;
constexpr uint32_t kRspBytes = 512;
constexpr uint32_t kFatal = 0x71880;
static_assert((kUcd & 127) == 0, "UCD requires 128-byte alignment");
static_assert(kUcd >= 1024, "UCD must not overlap the request list");
static_assert(kRsp >= 32 && kRsp + kRspBytes <= kPrdt, "response layout");
static_assert(kUcd + kUcdSyncBytes <= kData && kData + 4096 <= kBytes, "DMA layout");

// Only these cache-line-complete ranges are owned by this probe. In particular, the
// firmware's task list may live at list+0x1000 and must not be cleared or cleaned.
static bool rangesOverlap(uint64_t a, uint64_t aBytes, uint64_t b, uint64_t bBytes)
{
	return a <= b ? b - a < aBytes : a - b < bBytes;
}

static bool taskListOverlaps(uint64_t list, uint64_t taskList, uint32_t dataSpan = kDataBytes)
{
	return taskList && (rangesOverlap(list, kListBytes, taskList, kTaskBytes) ||
	                   rangesOverlap(list + kUcd, kUcdSyncBytes, taskList, kTaskBytes) ||
	                   rangesOverlap(list + kData, dataSpan, taskList, kTaskBytes));
}

enum class Command : unsigned { Nop, Flag, Power, Tur, Capacity, RequestSense, Wake, ReadBlock, WriteStore, SyncStore };
// OwnerData 3 GiB window. The previous 512 MiB store begins at the exclusive end.
constexpr uint32_t kStoreFirst = 0x039c47dd;
constexpr uint32_t kStoreEnd = 0x03a847f6;
constexpr uint32_t kWriteProofLba = 0x03aa47fa;
static_assert(kStoreEnd - kStoreFirst == 1 + 24 + 786432, "3 GiB OwnerData store geometry");
enum class Status : unsigned {
	NotRun, Success, Timeout, Fatal, Guard, Layout, Ocs, Protocol, Query, Scsi, Geometry, Value
};

struct Result {
	Status status = Status::NotRun;
	bool issued = false, complete = false, valid = false;
	bool valueRead = false, valueKnown = false;
	uint32_t ocs = 15, error = 0, detail = 0, value = 0xffffffffU;
	uint32_t sense = 0xffffffffU, senseKind = 0;
	uint32_t isBefore = 0, isAfter = 0, dbr = 0, hcs = 0, rsr = 0;
	uint8_t request[32] = {};
	uint32_t utrd[8] = {}, prdt[4] = {};
	uint8_t response[64] = {};
	uint8_t data[32] = {};
	uint32_t dataNonzero = 0, dataCrc = 0, dataFirst = 0, dataLast = 0;
	uint32_t dataMbrSignature = 0;
};

static uint32_t dataBytes(Command command)
{
	// REQUEST SENSE uses a DWORD-multiple allocation within the isolated 64-byte data line.
	return (command == Command::ReadBlock || command == Command::WriteStore) ? 4096 :
	       (command == Command::Capacity ? 8 : (command == Command::RequestSense ? 32 : 0));
}

static uint32_t dataSyncBytes(Command command)
{
	return dataBytes(command) == 4096 ? 4096 : kDataBytes;
}

static uint8_t commandLun(Command command)
{
	return command == Command::RequestSense || command == Command::Wake ? 0xd0 : 0;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

static void clearWords(uint8_t *p, unsigned bytes)
{
	volatile uint32_t *words = reinterpret_cast<volatile uint32_t *>(p);
	for (unsigned i = 0; i < bytes / 4; i++) {
		words[i] = 0;
	}
}

static void clearOwned(uint8_t *b)
{
	clearWords(b, kListBytes);
	clearWords(b + kUcd, kUcdSyncBytes);
	clearWords(b + kData, kDataBytes);
}

template <class Port>
static void cleanOwned(Port &port, uint32_t dataSpan = kDataBytes)
{
	port.cleanRange(0, kListBytes);
	port.cleanRange(kUcd, kUcdSyncBytes);
	port.cleanRange(kData, dataSpan);
}

static bool layoutValid(uint64_t phys, const uint8_t *virt)
{
	return phys != 0 && virt != nullptr && !(phys & 4095) && !(reinterpret_cast<uintptr_t>(virt) & 4095) &&
	       phys <= ~uint64_t(0) - kBytes && !((phys + kUcd) & 127);
}

static void build(uint8_t *b, uint64_t phys, Command command, uint32_t lba = 0)
{
	clearWords(b, 32);
	clearWords(b + kUcd, kPrdt + 16);
	clearWords(b + kData, dataSyncBytes(command));
	// Volatile byte stores prevent the compiler merging fields into unaligned wide accesses.
	volatile uint8_t *ucd = b + kUcd;
	if (command == Command::Flag || command == Command::Power) {
		ucd[0] = 0x16;
		ucd[5] = 0x01;
		ucd[12] = command == Command::Flag ? 0x05 : 0x03;
		ucd[13] = command == Command::Flag ? 0x01 : 0x02;
	} else if (command != Command::Nop) {
		ucd[0] = 0x01;
		ucd[2] = commandLun(command);
		if (dataBytes(command)) {
			ucd[1] = command == Command::WriteStore ? 0x20 : 0x40;
			ucd[12] = uint8_t(dataBytes(command) >> 24);
			ucd[13] = uint8_t(dataBytes(command) >> 16);
			ucd[14] = uint8_t(dataBytes(command) >> 8);
			ucd[15] = uint8_t(dataBytes(command));
		}
		if (command == Command::Capacity) {
			ucd[16] = 0x25;
		} else if (command == Command::RequestSense) {
			ucd[16] = 0x03;
			ucd[20] = uint8_t(dataBytes(command));
		} else if (command == Command::Wake) {
			ucd[16] = 0x1b;
			ucd[20] = 0x10;  // START STOP UNIT, power condition ACTIVE
		} else if (command == Command::SyncStore) {
			ucd[16] = 0x35;  // SYNCHRONIZE CACHE(10)
		} else if (command == Command::ReadBlock || command == Command::WriteStore) {
			ucd[16] = command == Command::WriteStore ? 0x2a : 0x28;
			if (command == Command::WriteStore) ucd[17] = 0x08; // FUA
			ucd[18] = uint8_t(lba >> 24);
			ucd[19] = uint8_t(lba >> 16);
			ucd[20] = uint8_t(lba >> 8);
			ucd[21] = uint8_t(lba);
			ucd[24] = 1;  // One 4096-byte logical block
		}
	}
	volatile uint32_t *utrd = reinterpret_cast<volatile uint32_t *>(b);
	const uint32_t length = dataBytes(command);
	const bool data = length != 0;
	const uint64_t ucdPhys = phys + kUcd;
	const unsigned direction = data ? (command == Command::WriteStore ? 1U : 2U) : 0U;
	utrd[0] = 0x10000000U | (direction << 25);
	utrd[2] = 0x0f;
	utrd[4] = uint32_t(ucdPhys);
	utrd[5] = uint32_t(ucdPhys >> 32);
	utrd[6] = (kRspBytes >> 2) | ((kRsp >> 2) << 16);
	utrd[7] = (data ? 1U : 0U) | ((kPrdt >> 2) << 16);
	if (data) {
		volatile uint32_t *prd = reinterpret_cast<volatile uint32_t *>(b + kUcd + kPrdt);
		const uint64_t dataPhys = phys + kData;
		prd[0] = uint32_t(dataPhys);
		prd[1] = uint32_t(dataPhys >> 32);
		prd[2] = 0;
		prd[3] = length - 1;
	}
}

static bool blockRequestValid(const Result &r, uint64_t phys, uint32_t lba, bool write)
{
	const uint8_t *q = r.request;
	if (be32(q) != (write ? 0x01200000U : 0x01400000U) || be32(q + 4) || be32(q + 8) || be32(q + 12) != 4096 ||
	    q[16] != (write ? 0x2a : 0x28) || q[17] != (write ? 0x08 : 0) || be32(q + 18) != lba || q[22] || q[23] || q[24] != 1 ||
	    q[25] || q[26] || q[27] || be32(q + 28)) return false;
	return r.utrd[0] == (write ? 0x12000000U : 0x14000000U) && !r.utrd[1] && r.utrd[2] == 15 && !r.utrd[3] &&
	       r.utrd[4] == uint32_t(phys + kUcd) && r.utrd[5] == uint32_t((phys + kUcd) >> 32) &&
	       r.utrd[6] == ((kRspBytes >> 2) | ((kRsp >> 2) << 16)) &&
	       r.utrd[7] == (1U | ((kPrdt >> 2) << 16)) &&
	       r.prdt[0] == uint32_t(phys + kData) && r.prdt[1] == uint32_t((phys + kData) >> 32) &&
	       !r.prdt[2] && r.prdt[3] == 4095;
}

// Decode bounded fixed/descriptor sense. Unknown formats remain visible as kind 3.
static bool decodeSense(const uint8_t *s, unsigned length, uint32_t &kind, uint32_t &sense)
{
	if (!length) return false;
	const unsigned code = s[0] & 0x7f;
	if (code != 0x70 && code != 0x71 && code != 0x72 && code != 0x73) {
		kind = 3; return true;
	}
	const unsigned needed = code <= 0x71 ? 14 : 8;
	if (length < needed || unsigned(s[7]) + 8 > length ||
	    (code <= 0x71 && unsigned(s[7]) + 8 < 14)) return false;
	kind = code <= 0x71 ? 1 : 2;
	sense = code <= 0x71 ? (uint32_t(s[2] & 15) << 16) | (uint32_t(s[12]) << 8) | s[13] :
	        (uint32_t(s[1] & 15) << 16) | (uint32_t(s[2]) << 8) | s[3];
	return true;
}

// Protocol error codes: 1 type, 2 tag, 3 LUN, 4 length/EHS, 5 query echo,
// 6 residual/flags, 7 NOP result, 8 truncated sense. A device error may be reused only
// after these protocol checks. Sense kind: 0 absent, 1 fixed, 2 descriptor, 3 unsupported.
static void validate(Command command, Result &r)
{
	const uint8_t *rsp = r.response;
	const bool query = command == Command::Flag || command == Command::Power;
	const uint8_t expected = command == Command::Nop ? 0x20 : (query ? 0x36 : 0x21);
	unsigned length = (unsigned(rsp[10]) << 8) | rsp[11];
	r.status = Status::Protocol;
	if (rsp[0] != expected) { r.error = 1; return; }
	if (rsp[3] != 0) { r.error = 2; return; }
	if (rsp[2] != commandLun(command)) { r.error = 3; return; }
	if (rsp[8] || length > kRspBytes - 32) { r.error = 4; return; }
	if (query) {
		const uint8_t opcode = command == Command::Flag ? 5 : 3;
		const uint8_t idn = command == Command::Flag ? 1 : 2;
		if (length || rsp[7]) { r.error = 4; return; }
		if (rsp[12] != opcode || rsp[13] != idn || rsp[14] || rsp[15]) {
			r.error = 5; return;
		}
		if (rsp[6]) { r.status = Status::Query; return; }
		r.value = be32(rsp + 20);
		r.valueRead = true;
		r.valueKnown = command == Command::Flag ? r.value <= 1 :
		               (r.value == 0x11 || r.value == 0x22 || r.value == 0x33);
		if (!r.valueKnown) { r.status = Status::Value; return; }
	} else if (command == Command::Nop) {
		if (length || rsp[6] || rsp[7]) { r.error = 7; return; }
	} else {
		const uint32_t requested = dataBytes(command);
		if ((rsp[1] & 0x40) || be32(rsp + 12) > requested || (!requested && (rsp[1] & 0x60))) {
			r.error = 6; r.detail = 1; return;
		}
		const unsigned senseLength = (unsigned(rsp[32]) << 8) | rsp[33];
		if (length && (length < 2 || senseLength > length - 2 || senseLength > 18)) {
			r.error = 4; return;
		}
		if (rsp[6] || rsp[7]) {
			if (length && senseLength && !decodeSense(rsp + 34, senseLength, r.senseKind, r.sense)) {
				r.error = 8; return;
			}
			r.status = Status::Scsi; return;
		}
		// GOOD may carry an HPB notification in the bounded response segment.
		// Decode SCSI sense only for error status; full-transfer checks still apply.
		if (command == Command::RequestSense) {
			const unsigned residual = be32(rsp + 12);
			if (bool(rsp[1] & 0x20) != bool(residual) || residual >= requested ||
			    !decodeSense(r.data, requested - residual, r.senseKind, r.sense) || r.senseKind == 3) {
				r.error = 8; return;
			}
		} else if ((rsp[1] & 0x60) || be32(rsp + 12)) { r.error = 6; r.detail = 3; return; }
		if (command == Command::Capacity &&
		    (be32(r.data) < 2 || be32(r.data) == 0xffffffffU || be32(r.data + 4) != 4096)) {
			r.status = Status::Geometry; return;
		}
	}
	r.status = Status::Success;
	r.valid = true;
}

struct Transport {
	bool poisoned = false;

	template <class Port>
	Result submit(Port &port, Command command, uint32_t lba = 0, const uint8_t *source = nullptr)
	{
		Result r;
		if (poisoned) return r;
		const bool write = command == Command::WriteStore;
		if (write && (lba < kStoreFirst || lba >= kStoreEnd || !source)) {
			r.status = Status::Guard;
			return r;
		}
		if (!layoutValid(port.phys, port.bytes)) {
			r.status = Status::Layout; poisoned = true; return r;
		}
		r.isBefore = port.read(0x20);
		r.dbr = port.read(0x58);
		r.hcs = port.read(0x30);
		r.rsr = port.read(0x60);
		const uint64_t list = uint64_t(port.read(0x50)) | (uint64_t(port.read(0x54)) << 32);
		const uint64_t taskList = uint64_t(port.read(0x70)) | (uint64_t(port.read(0x74)) << 32);
		if ((r.isBefore & kFatal) || r.dbr || port.read(0x78) || (port.read(0x80) & 1) ||
		    taskList > ~uint64_t(0) - kTaskBytes || taskListOverlaps(port.phys, taskList, dataSyncBytes(command)) ||
		    !(port.read(0x34) & 1) || (r.hcs & 0xb) != 0xb || !(r.rsr & 1) || list != port.phys) {
			r.status = Status::Guard; poisoned = true; return r;
		}
		build(port.bytes, port.phys, command, lba);
		if (write) {
			volatile uint8_t *data = port.bytes + kData;
			for (unsigned i = 0; i < 4096; ++i) data[i] = source[i];
		}
		if (command == Command::ReadBlock || write) {
			const volatile uint8_t *request = port.bytes + kUcd;
			for (unsigned i = 0; i < sizeof(r.request); i++) r.request[i] = request[i];
			const volatile uint32_t *utrd = reinterpret_cast<const volatile uint32_t *>(port.bytes);
			const volatile uint32_t *prd = reinterpret_cast<const volatile uint32_t *>(port.bytes + kUcd + kPrdt);
			for (unsigned i = 0; i < 8; i++) r.utrd[i] = utrd[i];
			for (unsigned i = 0; i < 4; i++) r.prdt[i] = prd[i];
			if (!blockRequestValid(r, port.phys, lba, write)) {
				r.status = Status::Guard; r.error = 10; poisoned = true; return r;
			}
		}
		cleanOwned(port, dataSyncBytes(command));
		if (r.isBefore & 1) port.write(0x20, 1);  // Preserve error evidence; W1C transfer completion only.
		port.begin();
		r.issued = true;
		port.write(0x58, 1);
		for (;;) {
			r.isAfter |= port.read(0x20);
			r.dbr = port.read(0x58);
			if (r.isAfter & kFatal) {
				r.status = Status::Fatal; poisoned = true; break;
			}
			if (!(r.dbr & 1)) {
				port.invalidate(0, 64);
				r.ocs = *reinterpret_cast<volatile uint32_t *>(port.bytes + 8) & 0xff;
				if (r.ocs != 15) {
					r.complete = true;
					port.invalidate(kUcd, kPrdt + 16);
					port.invalidate(kData, dataSyncBytes(command));
					const volatile uint8_t *rsp = port.bytes + kUcd + kRsp;
					for (unsigned i = 0; i < sizeof(r.response); i++) r.response[i] = rsp[i];
					const volatile uint8_t *data = port.bytes + kData;
					for (unsigned i = 0; i < sizeof(r.data); i++) r.data[i] = data[i];
					if (command == Command::ReadBlock) {
						uint32_t crc = 0xffffffffU;
						for (unsigned i = 0; i < 4096; i++) {
							const uint8_t value = data[i];
							r.dataNonzero += value != 0;
							crc ^= value;
							for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
						}
						r.dataCrc = crc ^ 0xffffffffU;
						r.dataFirst = be32(r.data);
						r.dataLast = (uint32_t(data[4092]) << 24) | (uint32_t(data[4093]) << 16) |
						             (uint32_t(data[4094]) << 8) | data[4095];
						r.dataMbrSignature = (uint32_t(data[510]) << 8) | data[511];
					}
					if (r.ocs) {
						r.status = Status::Ocs; poisoned = true;
					} else {
						validate(command, r);
						if (r.status == Status::Protocol || r.status == Status::Geometry) poisoned = true;
					}
					break;
				}
			}
			if (port.expired()) {
				r.status = Status::Timeout; poisoned = true; break;
			}
			port.delay();
		}
		r.hcs = port.read(0x30);
		r.rsr = port.read(0x60);
		if (r.complete && !poisoned && (r.isAfter & 1)) port.write(0x20, 1);
		return r;
	}
};
}  // namespace SmUfsProbe59
