// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

// Read-only primary GPT validation over one 4096-byte DMA block at a time.
// Volatile byte inputs prevent merged unaligned loads from the device-mapped block.
namespace SmUfsGpt {
constexpr uint32_t kBlockBytes = 4096;
constexpr uint32_t kMaxEntries = 256;
constexpr uint64_t kMaxTableBytes = 1024 * 1024;

enum class Status : unsigned {
	NotRun, Success, Read, Mbr, Signature, Header, HeaderCrc, HeaderBounds,
	TableShape, TableBounds, EntryBounds, EntryOverlap, TableCrc, Userdata, Changed
};

struct Range { uint64_t first = 0, last = 0; };
struct Report {
	Status status = Status::NotRun;
	bool mbr = false, header = false, table = false, userdata = false, stable = false;
	uint32_t reads = 0, successfulReads = 0, used = 0, entryCount = 0, entrySize = 0, badEntry = 0xffffffffU;
	uint64_t lastSuccessfulLba = ~uint64_t(0);
	uint32_t mbrWords[2] = {}, headerWords[2] = {};
	uint32_t mbrEntries[4][4] = {};
	uint32_t mbrSignature = 0, mbrExpected = 0, mbrReason = 0;
	bool mbrSaturated = false;
	uint32_t mbrCrc = 0, headerBlockCrc = 0, headerStored = 0, headerComputed = 0;
	uint32_t tableStored = 0, tableComputed = 0;
	uint64_t lastRead = 0, alternate = 0, firstUsable = 0, lastUsable = 0, tableLba = 0;
	uint64_t userFirst = 0, userLast = 0, userBlocks = 0;
	unsigned userMatches = 0;
	Range ranges[kMaxEntries];
};

static uint32_t le32(const volatile uint8_t *p)
{
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
static uint64_t le64(const volatile uint8_t *p) { return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32); }
static uint32_t word(const volatile uint8_t *p)
{
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
static uint32_t crcByte(uint32_t crc, uint8_t value)
{
	crc ^= value;
	for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
	return crc;
}
static uint32_t crc32(const volatile uint8_t *p, unsigned bytes)
{
	uint32_t crc = 0xffffffffU;
	for (unsigned i = 0; i < bytes; i++) crc = crcByte(crc, p[i]);
	return crc ^ 0xffffffffU;
}
static bool zero(const volatile uint8_t *p, unsigned bytes)
{
	for (unsigned i = 0; i < bytes; i++) if (p[i]) return false;
	return true;
}
static bool userdataName(const volatile uint8_t *p)
{
	static constexpr char name[] = "userdata";
	for (unsigned i = 0; i < sizeof(name); i++) {
		if (p[i * 2] != uint8_t(name[i]) || p[i * 2 + 1]) return false;
	}
	return true;  // Exact UTF-16LE name, including its terminator.
}

// Reader owns its current block until the next read. A false return is terminal: no retry,
// no fallback GPT, no writes. Report is statically retained by the kernel integration.
template <class Reader>
static void inspect(Reader &reader, uint64_t blocks, Report &r)
{
	auto read = [&](uint64_t lba) {
		r.lastRead = lba;
		if (lba >= blocks || lba > 0xffffffffULL || r.reads >= 260) return false;
		++r.reads;
		if (!reader.read(lba)) return false;
		++r.successfulReads;
		r.lastSuccessfulLba = lba;
		return true;
	};
	r.status = Status::Read;
	if (blocks < 6 || !read(0)) return;
	const volatile uint8_t *b = reader.block();
	r.mbrWords[0] = word(b); r.mbrWords[1] = word(b + 4);
	r.mbrCrc = crc32(b, kBlockBytes);
	r.status = Status::Mbr;
	r.mbrSignature = (uint32_t(b[510]) << 8) | b[511];
	r.mbrExpected = uint32_t(blocks - 1 > 0xffffffffULL ? 0xffffffffULL : blocks - 1);
	for (unsigned i = 0; i < 4; i++) {
		for (unsigned j = 0; j < 4; j++) r.mbrEntries[i][j] = word(b + 446 + i * 16 + j * 4);
	}
	if (r.mbrSignature != 0x55aa) { r.mbrReason = 1; return; }
	unsigned protective = 0;
	for (unsigned i = 0; i < 4; i++) {
		const volatile uint8_t *e = b + 446 + i * 16;
		if (!e[4]) continue;
		if (e[4] != 0xee) { r.mbrReason = 2; return; }  // Reject hybrid MBRs.
		if (le32(e + 8) != 1) { r.mbrReason = 3; return; }
		const uint32_t count = le32(e + 12);
		// Linux also recognizes tools that saturate protective size regardless of capacity.
		if (count != r.mbrExpected && count != 0xffffffffU) { r.mbrReason = 4; return; }
		r.mbrSaturated = count == 0xffffffffU && count != r.mbrExpected;
		++protective;
	}
	if (protective != 1) { r.mbrReason = 5; return; }
	r.mbr = true;
	r.status = Status::Read;
	if (!read(1)) return;
	b = reader.block();
	r.headerWords[0] = word(b); r.headerWords[1] = word(b + 4);
	r.headerBlockCrc = crc32(b, kBlockBytes);
	r.status = Status::Signature;
	static constexpr char signature[] = "EFI PART";
	for (unsigned i = 0; i < 8; i++) if (b[i] != uint8_t(signature[i])) return;
	r.status = Status::Header;
	const uint32_t headerBytes = le32(b + 12);
	if (le32(b + 8) != 0x00010000 || headerBytes < 92 || headerBytes > kBlockBytes || le32(b + 20)) return;
	r.headerStored = le32(b + 16);
	uint32_t crc = 0xffffffffU;
	for (unsigned i = 0; i < headerBytes; i++) crc = crcByte(crc, i >= 16 && i < 20 ? 0 : b[i]);
	r.headerComputed = crc ^ 0xffffffffU;
	r.status = Status::HeaderCrc;
	if (r.headerStored != r.headerComputed) return;
	r.alternate = le64(b + 32); r.firstUsable = le64(b + 40); r.lastUsable = le64(b + 48);
	r.tableLba = le64(b + 72); r.entryCount = le32(b + 80); r.entrySize = le32(b + 84);
	r.tableStored = le32(b + 88);
	r.status = Status::HeaderBounds;
	if (le64(b + 24) != 1 || r.alternate != blocks - 1 || r.firstUsable < 3 ||
	    r.lastUsable < r.firstUsable || r.lastUsable >= r.alternate || zero(b + 56, 16)) return;
	r.status = Status::TableShape;
	if (!r.entryCount || r.entryCount > kMaxEntries || r.entrySize < 128 || r.entrySize > kBlockBytes ||
	    (r.entrySize & (r.entrySize - 1))) return;
	const uint64_t tableBytes = uint64_t(r.entryCount) * r.entrySize;
	if (tableBytes > kMaxTableBytes) return;
	const uint64_t tableBlocks = (tableBytes + kBlockBytes - 1) / kBlockBytes;
	r.status = Status::TableBounds;
	if (r.tableLba < 2 || r.tableLba >= r.firstUsable || tableBlocks > r.firstUsable - r.tableLba ||
	    tableBlocks > r.alternate - r.lastUsable - 1) return;
	r.header = true;
	crc = 0xffffffffU;
	for (uint64_t block = 0; block < tableBlocks; block++) {
		r.status = Status::Read;
		if (!read(r.tableLba + block)) return;
		b = reader.block();
		const uint64_t remaining = tableBytes - block * kBlockBytes;
		const unsigned validBytes = remaining < kBlockBytes ? unsigned(remaining) : kBlockBytes;
		for (unsigned i = 0; i < validBytes; i++) crc = crcByte(crc, b[i]);
		for (unsigned offset = 0; offset < validBytes; offset += r.entrySize) {
			const volatile uint8_t *e = b + offset;
			const uint32_t index = uint32_t((block * kBlockBytes + offset) / r.entrySize);
			if (zero(e, 16)) continue;
			r.badEntry = index;
			const uint64_t first = le64(e + 32), last = le64(e + 40);
			r.status = Status::EntryBounds;
			if (zero(e + 16, 16) || first < r.firstUsable || last > r.lastUsable || first > last) return;
			r.status = Status::EntryOverlap;
			for (unsigned i = 0; i < r.used; i++) {
				if (first <= r.ranges[i].last && r.ranges[i].first <= last) return;
			}
			r.ranges[r.used++] = Range{first, last};
			if (userdataName(e + 56)) {
				++r.userMatches; r.userFirst = first; r.userLast = last; r.userBlocks = last - first + 1;
			}
		}
	}
	r.badEntry = 0xffffffffU;
	r.tableComputed = crc ^ 0xffffffffU;
	r.status = Status::TableCrc;
	if (r.tableComputed != r.tableStored) return;
	r.table = true;
	r.status = Status::Userdata;
	if (r.userMatches != 1) return;
	r.userdata = true;
	// Re-read the two known blocks to witness repeatable data transfer, not just matching magic.
	r.status = Status::Read;
	if (!read(0)) return;
	r.status = Status::Changed;
	if (crc32(reader.block(), kBlockBytes) != r.mbrCrc) return;
	r.status = Status::Read;
	if (!read(1)) return;
	r.status = Status::Changed;
	if (crc32(reader.block(), kBlockBytes) != r.headerBlockCrc) return;
	r.stable = true;
	r.status = Status::Success;
}
}  // namespace SmUfsGpt
