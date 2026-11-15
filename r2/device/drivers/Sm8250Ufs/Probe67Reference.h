// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include "Probe61Gpt.h"
#include "Probe62Inventory.h"

// Bounded reference for probe67's exact primary-GPT case. The caller retains
// Reference, Report and Inventory in static storage; no block or inventory copy
// is retained here. GUID bytes always have the GPT on-disk byte order.
namespace SmUfsPartitionRef {
constexpr unsigned kSlots = 32;
constexpr unsigned kUsed = 23;

struct Reference {
	uint8_t disk[16] = {}, unique[kSlots][16] = {};
	bool captured[kSlots] = {};
	bool headerCaptured = false, tableCaptured = false, trusted = false, overflow = false;
	unsigned used = 0;
};

// Nest below EntryReader, so every byte is captured from the same successful
// read that the existing GPT parser and inventory consume. There are no reads,
// retries, fallback parsing or mutations of the underlying block in this layer.
template <class Reader>
struct Observer {
	Reader &reader;
	const SmUfsGpt::Report &gpt;
	Reference &reference;
	bool read(uint64_t lba)
	{
		if (!reader.read(lba)) return false;
		const volatile uint8_t *data = reader.block();
		if (lba == 1 && !gpt.header && !reference.headerCaptured) {
			for (unsigned i = 0; i < 16; ++i) reference.disk[i] = data[56 + i];
			reference.headerCaptured = true;
		}
		if (!gpt.header || gpt.table || lba < gpt.tableLba) return true;
		if (gpt.entryCount != kSlots || gpt.entrySize != 128 || gpt.tableLba != 2) {
			reference.overflow = true;
			return true; // The inherited parser decides whether the read is valid.
		}
		if (lba != 2) return true;
		if (reference.tableCaptured) { reference.overflow = true; return true; }
		for (unsigned slot = 0; slot < kSlots; ++slot) {
			const volatile uint8_t *entry = data + slot * 128;
			if (SmUfsGpt::zero(entry, 16)) continue;
			for (unsigned i = 0; i < 16; ++i) reference.unique[slot][i] = entry[16 + i];
			reference.captured[slot] = true;
			++reference.used;
		}
		reference.tableCaptured = true;
		return true;
	}
	const volatile uint8_t *block() const { return reader.block(); }
};

// Standard lowercase GUID text, including the mixed-endian GPT fields. This is
// also usable with a volatile byte source; do not cast a device buffer to words.
static void guid(const volatile uint8_t *raw, char out[37])
{
	static const unsigned order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
	static const char hex[] = "0123456789abcdef";
	unsigned n = 0;
	for (unsigned i = 0; i < 16; ++i) {
		if (i == 4 || i == 6 || i == 8 || i == 10) out[n++] = '-';
		const uint8_t value = raw[order[i]];
		out[n++] = hex[value >> 4]; out[n++] = hex[value & 15];
	}
	out[n] = 0;
}
static void guid(const uint8_t *raw, char out[37])
{
	guid(static_cast<const volatile uint8_t *>(raw), out);
}

// On this device unsupported Unicode, control bytes and unterminated names
// stop reference validation before any native partition probe is attempted.
static bool asciiName(const uint16_t name[36], char out[37])
{
	out[0] = 0;
	for (unsigned i = 0; i < 36; ++i) {
		const uint16_t value = name[i];
		if (!value) { out[i] = 0; return true; }
		if (value < 32 || value > 126) { out[0] = 0; return false; }
		out[i] = char(value);
	}
	out[0] = 0;
	return false;
}

// Subtract before adding or multiplying, and bound both the inclusive end and
// byte conversion. Length >= 2 is needed by the native harness's start+1 case.
static bool byteRange(uint64_t first, uint64_t last, uint64_t blocks,
                      uint64_t &offset, uint64_t &length)
{
	offset = 0; length = 0;
	const uint64_t maxBlock = ~uint64_t(0) / SmUfsGpt::kBlockBytes;
	if (first >= last || last >= blocks || first > maxBlock || last >= maxBlock) return false;
	offset = first * SmUfsGpt::kBlockBytes;
	length = (last - first + 1) * SmUfsGpt::kBlockBytes;
	return true;
}

static bool validate(Reference &reference, const SmUfsGpt::Report &gpt,
                     const SmUfsInventory::Inventory &inventory, uint64_t blocks)
{
	reference.trusted = false;
	if (gpt.status != SmUfsGpt::Status::Success || !inventory.trusted || inventory.overflow ||
	    reference.overflow || !reference.headerCaptured || !reference.tableCaptured ||
	    gpt.entryCount != kSlots || gpt.entrySize != 128 || gpt.tableLba != 2 ||
	    gpt.used != kUsed || inventory.count != kUsed || reference.used != kUsed ||
	    blocks < 6 || gpt.firstUsable < 3 || gpt.firstUsable > gpt.lastUsable ||
	    gpt.alternate != blocks - 1 || gpt.lastUsable >= blocks - 1 ||
	    SmUfsGpt::zero(reference.disk, 16)) return false;
	uint32_t seen = 0;
	char name[37];
	for (unsigned i = 0; i < inventory.count; ++i) {
		const SmUfsInventory::Entry &entry = inventory.entries[i];
		if (!entry.slot || entry.slot > kSlots) return false;
		const unsigned slot = entry.slot - 1;
		const uint32_t bit = uint32_t(1) << slot;
		if ((seen & bit) || !reference.captured[slot] ||
		    SmUfsGpt::zero(reference.unique[slot], 16) || SmUfsGpt::zero(entry.type, 16) ||
		    entry.first < gpt.firstUsable || entry.last > gpt.lastUsable ||
		    !asciiName(entry.name, name)) return false;
		uint64_t offset, length;
		if (!byteRange(entry.first, entry.last, blocks, offset, length)) return false;
		seen |= bit;
	}
	for (unsigned slot = 0; slot < kSlots; ++slot) {
		if (reference.captured[slot] != bool(seen & (uint32_t(1) << slot))) return false;
	}
	reference.trusted = true;
	return true;
}

// One immutable expected row can be constructed at a time, avoiding another
// retained inventory. Treat a successfully built row as const in the harness.
struct Expected {
	uint64_t first = 0, last = 0, attributes = 0;
	uint32_t slot = 0;
	char type[37] = {}, uuid[37] = {}, name[37] = {};
};
static bool expected(const SmUfsInventory::Inventory &inventory, const Reference &reference,
                     unsigned index, Expected &out)
{
	out = Expected{};
	if (!reference.trusted || !inventory.trusted || inventory.overflow ||
	    inventory.count != kUsed || index >= inventory.count) return false;
	const SmUfsInventory::Entry &entry = inventory.entries[index];
	if (!entry.slot || entry.slot > kSlots || !reference.captured[entry.slot - 1]) return false;
	if (!asciiName(entry.name, out.name)) return false;
	guid(entry.type, out.type); guid(reference.unique[entry.slot - 1], out.uuid);
	out.slot = entry.slot; out.first = entry.first; out.last = entry.last; out.attributes = entry.attributes;
	return true;
}

// Inputs must name accessible NUL-terminated strings of at most 36 bytes. Read
// no more than 37 bytes from either side, even if a native value is malformed.
static bool textEqual(const char *expectedText, const char *actualText)
{
	if (!expectedText || !actualText) return false;
	for (unsigned i = 0; i < 37; ++i) {
		if (expectedText[i] != actualText[i]) return false;
		if (!expectedText[i]) return true;
	}
	return false;
}
static bool matches(const Expected &expectedRow, uint32_t slot, uint64_t offset, uint64_t length,
                    uint64_t attributes, const char *type, const char *uuid, const char *name)
{
	uint64_t expectedOffset, expectedLength;
	if (expectedRow.last == ~uint64_t(0) ||
	    !byteRange(expectedRow.first, expectedRow.last, expectedRow.last + 1,
	               expectedOffset, expectedLength)) return false;
	return slot == expectedRow.slot && offset == expectedOffset && length == expectedLength &&
	       attributes == expectedRow.attributes && textEqual(expectedRow.type, type) &&
	       textEqual(expectedRow.uuid, uuid) && textEqual(expectedRow.name, name);
}
} // namespace SmUfsPartitionRef
