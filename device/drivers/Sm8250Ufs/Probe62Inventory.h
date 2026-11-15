// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

// Read-only inventory layered on the validated probe61 parser and transport.
namespace SmUfsInventory {
constexpr unsigned kMaxEntries = SmUfsGpt::kMaxEntries;

struct Entry {
	uint64_t first = 0, last = 0, attributes = 0;
	uint32_t slot = 0;
	uint16_t name[36] = {};
	uint8_t type[16] = {};
};
struct Inventory {
	Entry entries[kMaxEntries];
	unsigned count = 0;
	bool overflow = false, trusted = false;
};

static uint16_t le16(const volatile uint8_t *p)
{
	return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
static bool named(const Entry &entry, const char *name)
{
	unsigned i = 0;
	for (; i < 36 && name[i]; i++) if (entry.name[i] != uint8_t(name[i])) return false;
	return i < 36 && !name[i] && !entry.name[i];
}

template <class Reader>
struct EntryReader {
	Reader &reader;
	const SmUfsGpt::Report &gpt;
	Inventory &inventory;
	bool read(uint64_t lba)
	{
		if (!reader.read(lba)) return false;
		if (!gpt.header || gpt.table || lba < gpt.tableLba) return true;
		const uint64_t bytes = uint64_t(gpt.entryCount) * gpt.entrySize;
		const uint64_t block = lba - gpt.tableLba;
		if (block >= (bytes + 4095) / 4096) return true;
		const uint64_t remaining = bytes - block * 4096;
		const unsigned valid = remaining < 4096 ? unsigned(remaining) : 4096;
		const volatile uint8_t *data = reader.block();
		for (unsigned offset = 0; offset < valid; offset += gpt.entrySize) {
			const volatile uint8_t *e = data + offset;
			if (SmUfsGpt::zero(e, 16)) continue;
			if (inventory.count >= kMaxEntries) { inventory.overflow = true; return false; }
			Entry &out = inventory.entries[inventory.count++];
			out.slot = uint32_t((block * 4096 + offset) / gpt.entrySize) + 1;
			out.first = SmUfsGpt::le64(e + 32); out.last = SmUfsGpt::le64(e + 40);
			out.attributes = SmUfsGpt::le64(e + 48);
			for (unsigned i = 0; i < 16; i++) out.type[i] = e[i];
			for (unsigned i = 0; i < 36; i++) out.name[i] = le16(e + 56 + i * 2);
		}
		return true;
	}
	const volatile uint8_t *block() const { return reader.block(); }
};

static bool trust(Inventory &inventory, const SmUfsGpt::Report &gpt)
{
	inventory.trusted = gpt.status == SmUfsGpt::Status::Success && !inventory.overflow &&
	                    inventory.count == gpt.used;
	return inventory.trusted;
}

enum class Kind : unsigned { Unknown, Ext, F2fs, Apfs };
struct Feature {
	Kind kind = Kind::Unknown;
	bool geometry = false;
	uint32_t blockBytes = 0, compat = 0, incompat = 0, roCompat = 0;
	uint64_t blocks = 0;
};
static Feature feature(const volatile uint8_t *b, uint64_t partitionBlocks)
{
	Feature f;
	if (!partitionBlocks || partitionBlocks > 0xffffffffULL + 1) return f;
	const volatile uint8_t *s = b + 1024;
	const uint64_t bytes = partitionBlocks * 4096;
	if (SmUfsGpt::le32(s) == 0xf2f52010U) {
		f.kind = Kind::F2fs;
		f.blocks = SmUfsGpt::le64(s + 36);
		f.incompat = SmUfsGpt::le32(s + 2180);
		const uint32_t sector = SmUfsGpt::le32(s + 8), perBlock = SmUfsGpt::le32(s + 12);
		f.blockBytes = SmUfsGpt::le32(s + 16) == 12 ? 4096 : 0;
		f.geometry = sector >= 9 && sector <= 12 && perBlock <= 3 && sector + perBlock == 12 &&
		             f.blockBytes == 4096 && SmUfsGpt::le32(s + 20) == 9 &&
		             f.blocks && f.blocks <= partitionBlocks;
	} else if (le16(s + 56) == 0xef53) {
		f.kind = Kind::Ext;
		const uint32_t log = SmUfsGpt::le32(s + 24);
		f.blockBytes = log <= 6 ? 1024U << log : 0;
		f.compat = SmUfsGpt::le32(s + 92); f.incompat = SmUfsGpt::le32(s + 96);
		f.roCompat = SmUfsGpt::le32(s + 100);
		f.blocks = SmUfsGpt::le32(s + 4);
		if (f.incompat & 0x80) f.blocks |= uint64_t(SmUfsGpt::le32(s + 336)) << 32;
		f.geometry = f.blockBytes && f.blocks && f.blocks <= bytes / f.blockBytes;
	} else if (SmUfsGpt::word(b + 32) == 0x4e585342) {
		f.kind = Kind::Apfs;
		f.blockBytes = SmUfsGpt::le32(b + 36); f.blocks = SmUfsGpt::le64(b + 40);
		f.geometry = f.blockBytes >= 4096 && f.blockBytes <= 65536 &&
		             !(f.blockBytes & (f.blockBytes - 1)) && f.blocks && f.blocks <= bytes / f.blockBytes;
	}
	return f;
}

enum class SampleStatus : unsigned { NotRun, Stable, Read, Changed };
struct Sample {
	Feature fs;
	SampleStatus status = SampleStatus::NotRun;
	uint32_t crc = 0, repeatCrc = 0, first = 0, super = 0;
};
struct Volume {
	Sample primary, secondary;
	bool secondaryRequested = false;
};
struct Volumes {
	Volume entries[kMaxEntries];
	unsigned examined = 0, completed = 0, reads = 0, successfulReads = 0;
	uint64_t lastLba = 0;
	bool valid = false;
};

template <class Reader>
static bool sample(Reader &reader, uint64_t lba, uint64_t blocks, Sample &out, Volumes &report)
{
	out.status = SampleStatus::Read;
	report.lastLba = lba; ++report.reads;
	if (!reader.read(lba)) return false;
	++report.successfulReads;
	const volatile uint8_t *b = reader.block();
	out.fs = feature(b, blocks);
	out.first = SmUfsGpt::word(b); out.super = SmUfsGpt::word(b + 1024);
	out.crc = SmUfsGpt::crc32(b, 4096);
	++report.reads;
	if (!reader.read(lba)) { out.fs = Feature{}; return false; }
	++report.successfulReads;
	out.repeatCrc = SmUfsGpt::crc32(reader.block(), 4096);
	if (out.crc != out.repeatCrc) { out.fs = Feature{}; out.status = SampleStatus::Changed; return false; }
	out.status = SampleStatus::Stable;
	return true;
}

template <class Reader>
static void scan(Reader &reader, const Inventory &inventory, uint64_t capacity, Volumes &report)
{
	if (!inventory.trusted || inventory.count > kMaxEntries) return;
	for (unsigned i = 0; i < inventory.count; i++) {
		const Entry &e = inventory.entries[i];
		if (e.first > e.last || e.last >= capacity || e.last > 0xffffffffULL) return;
		Volume &v = report.entries[i];
		++report.examined;
		const uint64_t blocks = e.last - e.first + 1;
		if (!sample(reader, e.first, blocks, v.primary, report)) return;
		v.secondaryRequested = blocks >= 2 && (v.primary.fs.kind == Kind::F2fs || named(e, "userdata") ||
		                       named(e, "unixnu") || named(e, "rootfs") || named(e, "apfs"));
		if (v.secondaryRequested && !sample(reader, e.first + 1, blocks, v.secondary, report)) return;
		++report.completed;
	}
	report.valid = report.completed == inventory.count;
}

template <unsigned Capacity>
struct Text {
	char bytes[Capacity] = {};
	unsigned used = 0;
	bool overflow = false;
	void ch(char value) {
		if (used + 1 >= Capacity) { overflow = true; return; }
		bytes[used++] = value; bytes[used] = 0;
	}
	void str(const char *s) { for (unsigned i = 0; s[i]; i++) ch(s[i]); }
	void num(uint64_t value, unsigned base = 10, unsigned width = 0) {
		char digits[24]; unsigned count = 0;
		do { const unsigned d = unsigned(value % base); digits[count++] = char(d < 10 ? '0' + d : 'a' + d - 10); value /= base; } while (value);
		for (unsigned i = count; i < width; i++) ch('0');
		while (count) ch(digits[--count]);
	}
};
using ReportText = Text<3600>;
using RecordText = Text<384>;

static void name(RecordText &out, const Entry &e)
{
	out.ch('"');
	for (unsigned i = 0; i < 36 && e.name[i]; i++) {
		const uint16_t c = e.name[i];
		if (c >= 32 && c <= 126 && c != '\\' && c != '"') out.ch(char(c));
		else if (c == '\\' || c == '"') { out.ch('\\'); out.ch(char(c)); }
		else { out.str("\\u"); out.num(c, 16, 4); }
	}
	out.ch('"');
}
static void guid(RecordText &out, const uint8_t *g)
{
	const unsigned order[] = {3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
	for (unsigned i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10) out.ch('-');
		out.num(g[order[i]], 16, 2);
	}
}
static void inventoryRecord(RecordText &out, const Entry &e)
{
	out.num(e.slot, 10, 2); out.ch(' '); name(out, e); out.ch(' ');
	out.num(e.first, 16); out.str(".."); out.num(e.last, 16); out.ch('\n');
	out.str(" type "); guid(out, e.type); out.str(" a"); out.num(e.attributes, 16); out.ch('\n');
}
static const char *kind(Kind value)
{
	return value == Kind::Ext ? "ext" : value == Kind::F2fs ? "f2fs" : value == Kind::Apfs ? "apfs" : "unknown";
}
static void sampleRecord(RecordText &out, const char *label, const Sample &s)
{
	out.str(label); out.str(kind(s.fs.kind)); out.str(" g"); out.num(s.fs.geometry ? 1 : 0);
	out.str(" s"); out.num(unsigned(s.status)); out.str(" crc"); out.num(s.crc, 16, 8);
	out.ch('='); out.num(s.repeatCrc, 16, 8);
	if (s.fs.kind == Kind::Unknown) {
		out.str(" raw"); out.num(s.first, 16, 8); out.ch('/'); out.num(s.super, 16, 8); out.ch('\n');
	} else {
		out.str(" bs"); out.num(s.fs.blockBytes); out.str(" n"); out.num(s.fs.blocks, 16); out.ch('\n');
		out.str(" feat "); out.num(s.fs.compat, 16, 8); out.ch('/'); out.num(s.fs.incompat, 16, 8);
		out.ch('/'); out.num(s.fs.roCompat, 16, 8); out.ch('\n');
	}
}
static void volumeRecord(RecordText &out, const Entry &e, const Volume &v)
{
	out.num(e.slot, 10, 2); out.ch(' '); sampleRecord(out, "p0:", v.primary);
	if (v.secondaryRequested) sampleRecord(out, "   p1:", v.secondary);
}
template <unsigned Capacity>
static unsigned rows(const Text<Capacity> &text, unsigned columns)
{
	if (!columns) return text.used ? ~0U : 0;
	unsigned result = 0, col = 0;
	for (unsigned i = 0; i < text.used; i++) {
		if (text.bytes[i] == '\n') { ++result; col = 0; continue; }
		if (col == columns) { ++result; col = 0; }
		++col;
	}
	return result + (col ? 1U : 0U);
}
struct Page {
	unsigned requested = 0, count = 0, first = 0, end = 0;
	bool valid = false;
};

template <class Render>
static Page paginate(unsigned count, unsigned requested, unsigned columns, unsigned rowBudget, Render render,
                     unsigned byteBudget = 3000)
{
	Page p; p.requested = requested;
	if (columns < 20 || rowBudget < 6) return p;
	unsigned page = 0, first = 0, bytes = 0, lines = 0;
	for (unsigned i = 0; i < count; i++) {
		RecordText record; render(record, i);
		const unsigned lineCount = rows(record, columns);
		if (record.overflow || record.used > byteBudget || lineCount > rowBudget) return p;
		if (i > first && (bytes + record.used > byteBudget || lines + lineCount > rowBudget)) {
			if (page == requested) { p.first = first; p.end = i; }
			++page; first = i; bytes = 0; lines = 0;
		}
		bytes += record.used; lines += lineCount;
	}
	if (page == requested) { p.first = first; p.end = count; }
	p.count = page + 1; p.valid = requested < p.count;
	return p;
}

static void pageHeader(ReportText &out, const Page &p)
{
	out.str("page "); out.num(p.requested); out.ch('/'); out.num(p.count);
	out.str(" records "); out.num(p.first); out.str(".."); out.num(p.end); out.str(" (end-exclusive)\n");
	if (!p.valid) out.str("page-invalid: use native-ufs-page=N or larger display\n");
}
struct ReportBudget { unsigned rows = 0, bytes = 0; };
static ReportBudget recordBudget(const ReportText &prefix, unsigned columns, unsigned available,
                                 const char *heading, const char *footer)
{
	RecordText page;
	page.str("page 4294967295/4294967295 records 4294967295..4294967295 (end-exclusive)\n");
	RecordText extra; extra.str(heading); extra.str(footer);
	const unsigned reserved = rows(prefix, columns) + rows(page, columns) + rows(extra, columns);
	const unsigned bytes = prefix.used + page.used + extra.used;
	ReportBudget budget;
	budget.rows = available > reserved ? available - reserved : 0;
	budget.bytes = bytes < sizeof(prefix.bytes) - 1 ? unsigned(sizeof(prefix.bytes) - 1 - bytes) : 0;
	if (budget.bytes > 3000) budget.bytes = 3000;
	return budget;
}
static Page inventoryReport(ReportText &out, const Inventory &inventory, const SmUfsGpt::Report &g,
                            unsigned page, unsigned columns, unsigned rowBudget, const char *summary = nullptr)
{
	out.str("SMUFS capdiag v16b-inventory probe62\n");
	if (summary) out.str(summary);
	out.str("GPT st"); out.num(unsigned(g.status)); out.str(" trusted"); out.num(inventory.trusted ? 1 : 0);
	out.str(" used"); out.num(inventory.count); out.ch('/'); out.num(g.entryCount);
	out.str(" h"); out.num(g.headerStored, 16, 8); out.str(" t"); out.num(g.tableStored, 16, 8); out.ch('\n');
	if (!inventory.trusted) return Page{};
	auto render = [&](RecordText &r, unsigned i) { inventoryRecord(r, inventory.entries[i]); };
	const ReportBudget budget = recordBudget(out, columns, rowBudget,
	    "slot/name start..end LBA inclusive; type=GUID; a=attrs\n",
	    "free-usable blocks 18446744073709551615 tail 18446744073709551615; UTF16 escaped; block4096\n");
	const Page p = paginate(inventory.count, page, columns, budget.rows, render, budget.bytes);
	pageHeader(out, p);
	out.str("slot/name start..end LBA inclusive; type=GUID; a=attrs\n");
	if (p.valid) for (unsigned i = p.first; i < p.end; i++) { RecordText r; render(r, i); out.str(r.bytes); }
	uint64_t used = 0, last = g.firstUsable;
	for (unsigned i = 0; i < inventory.count; i++) {
		used += inventory.entries[i].last - inventory.entries[i].first + 1;
		if (inventory.entries[i].last > last) last = inventory.entries[i].last;
	}
	out.str("free-usable blocks "); out.num(g.lastUsable - g.firstUsable + 1 - used);
	out.str(" tail "); out.num(g.lastUsable - last); out.str("; UTF16 escaped; block4096\n");
	return p;
}
static Page volumeReport(ReportText &out, const Inventory &inventory, const SmUfsGpt::Report &g,
                        const Volumes &volumes, unsigned page, unsigned columns, unsigned rowBudget,
                        const char *summary = nullptr)
{
	out.str("SMUFS capdiag v16b-volumes probe63\n");
	if (summary) out.str(summary);
	out.str("GPT st"); out.num(unsigned(g.status)); out.str(" t"); out.num(g.tableStored, 16, 8);
	out.str(" reads "); out.num(volumes.reads); out.ch('/'); out.num(volumes.successfulReads);
	out.str(" completed "); out.num(volumes.completed); out.ch('/'); out.num(inventory.count);
	out.str(" valid"); out.num(volumes.valid ? 1 : 0); out.ch('\n');
	if (!inventory.trusted) return Page{};
	auto render = [&](RecordText &r, unsigned i) { volumeRecord(r, inventory.entries[i], volumes.entries[i]); };
	const ReportBudget budget = recordBudget(out, columns, rowBudget,
	    "slots follow inventory; p0=start p1=start+1; s1=repeat-stable\n",
	    "g=bounded geometry; fs candidate only; checksums unverified\n"
	    "feat=c/i/r raw masks; ext encrypt i&10000, f2fs i&1\n"
	    "encryption state unknown; no mount/decrypt/write\n");
	const Page p = paginate(inventory.count, page, columns, budget.rows, render, budget.bytes);
	pageHeader(out, p);
	out.str("slots follow inventory; p0=start p1=start+1; s1=repeat-stable\n");
	if (p.valid) for (unsigned i = p.first; i < p.end; i++) { RecordText r; render(r, i); out.str(r.bytes); }
	out.str("g=bounded geometry; fs candidate only; checksums unverified\n");
	out.str("feat=c/i/r raw masks; ext encrypt i&10000, f2fs i&1\n");
	out.str("encryption state unknown; no mount/decrypt/write\n");
	return p;
}
}  // namespace SmUfsInventory
