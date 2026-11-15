// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <stdint.h>
#include <string.h>

namespace SmUfsRoot {

constexpr uint32_t kManifestBytes = 4096;
constexpr uint32_t kManifestChecksumBytes = 4064;
constexpr uint64_t kWindowBase = 1ULL << 20;
constexpr uint64_t kWindowAlignment = 1ULL << 20;
constexpr uint32_t kMaxWindows = 4;
constexpr uint32_t kWindowEntryBytes = 128;
constexpr uint32_t kWindowBlockBytes = 512;
constexpr uint32_t kPhysicalBlockBytes = 4096;
constexpr uint32_t kReadOnlyFlag = 1;

// The parser is deliberately independent of SHA-256 implementation details. The
// production caller supplies a bounded digest routine from the kernel crypto layer.
typedef bool (*Hash32)(const uint8_t *bytes, uint64_t length, uint8_t digest[32]);

enum class ParseError : uint32_t {
	None,
	Argument,
	Signature,
	Version,
	Count,
	Checksum,
	Total,
	Name,
	Shape,
	Alignment,
	Bounds,
	Overlap,
	Duplicate,
};

struct Window {
	char name[32] = {};
	uint64_t offset = 0;
	uint64_t size = 0;
	uint32_t blockBytes = 0;
	uint32_t flags = 0;
	uint8_t digest[32] = {};
};

struct Layout {
	bool valid = false;
	ParseError error = ParseError::None;
	uint32_t version = 0;
	uint32_t count = 0;
	uint64_t imageBytes = 0;
	uint64_t userdataBytes = 0;
	uint64_t firstLba = 0;
	uint64_t lastLba = 0;
	Window windows[kMaxWindows] = {};
};

static inline uint32_t le32(const uint8_t *p)
{
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static inline uint64_t le64(const uint8_t *p)
{
	return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32);
}

static inline bool addOverflow(uint64_t a, uint64_t b, uint64_t *out)
{
	if (b > ~uint64_t(0) - a) return true;
	*out = a + b;
	return false;
}

static inline bool alignUp(uint64_t value, uint64_t alignment, uint64_t *out)
{
	if (!alignment || value > ~uint64_t(0) - (alignment - 1)) return true;
	*out = (value + alignment - 1) & ~(alignment - 1);
	return false;
}

static inline bool validName(const uint8_t *raw, char out[32])
{
	unsigned length = 0;
	while (length < 32 && raw[length]) {
		const uint8_t c = raw[length];
		if (c < 0x21 || c > 0x7e) return false;
		++length;
	}
	if (!length || length == 32) return false;
	for (unsigned i = length; i < 32; ++i) if (raw[i]) return false;
	memset(out, 0, 32);
	memcpy(out, raw, length);
	return true;
}

static inline bool sameName(const char a[32], const char b[32])
{
	return memcmp(a, b, 32) == 0;
}

static inline bool rangeOverlap(const Window &a, const Window &b)
{
	return a.offset < b.offset + b.size && b.offset < a.offset + a.size;
}

template <typename Digest>
static Layout parse(const uint8_t *manifest, uint64_t userdataBytes,
                    uint64_t firstLba, uint64_t lastLba, Digest digest)
{
	Layout result{};
	result.userdataBytes = userdataBytes;
	result.firstLba = firstLba;
	result.lastLba = lastLba;
	if (!manifest || userdataBytes == 0 || firstLba > lastLba) {
		result.error = ParseError::Argument;
		return result;
	}
	const uint64_t capacityBlocks = lastLba - firstLba + 1;
	if (capacityBlocks > ~uint64_t(0) / kPhysicalBlockBytes ||
	    capacityBlocks * kPhysicalBlockBytes != userdataBytes) {
		result.error = ParseError::Argument;
		return result;
	}
	static constexpr uint8_t signature[8] = {'S', 'H', 'N', 'G', 'R', 'O', 'O', 'T'};
	if (memcmp(manifest, signature, sizeof(signature)) != 0) {
		result.error = ParseError::Signature;
		return result;
	}
	result.version = le32(manifest + 8);
	result.count = le32(manifest + 12);
	result.imageBytes = le64(manifest + 16);
	if (result.version != 1) { result.error = ParseError::Version; return result; }
	if (!result.count || result.count > kMaxWindows) { result.error = ParseError::Count; return result; }
	if (result.imageBytes < kWindowBase || result.imageBytes > userdataBytes) {
		result.error = ParseError::Total;
		return result;
	}
	uint8_t computed[32] = {};
	if (!digest(manifest, kManifestChecksumBytes, computed) ||
	    memcmp(computed, manifest + kManifestChecksumBytes, sizeof(computed)) != 0) {
		result.error = ParseError::Checksum;
		return result;
	}
	uint64_t expectedOffset = kWindowBase;
	for (uint32_t i = 0; i < result.count; ++i) {
		const uint8_t *entry = manifest + 24 + uint64_t(i) * kWindowEntryBytes;
		Window &window = result.windows[i];
		if (!validName(entry, window.name)) { result.error = ParseError::Name; return result; }
		window.offset = le64(entry + 32);
		window.size = le64(entry + 40);
		window.blockBytes = le32(entry + 48);
		window.flags = le32(entry + 52);
		for (unsigned byte = 0; byte < sizeof(window.digest); ++byte) window.digest[byte] = entry[56 + byte];
		if (window.blockBytes != kWindowBlockBytes || window.flags != kReadOnlyFlag || !window.size) {
			result.error = ParseError::Shape;
			return result;
		}
		if (window.offset != expectedOffset || window.offset % kWindowAlignment) {
			result.error = ParseError::Alignment;
			return result;
		}
		uint64_t end = 0;
		if (addOverflow(window.offset, window.size, &end) || end > result.imageBytes) {
			result.error = ParseError::Bounds;
			return result;
		}
		uint64_t alignedEnd = 0;
		if (alignUp(end, kWindowAlignment, &alignedEnd) || alignedEnd > result.imageBytes) {
			result.error = ParseError::Bounds;
			return result;
		}
		for (uint32_t j = 0; j < i; ++j) {
			if (sameName(window.name, result.windows[j].name)) {
				result.error = ParseError::Duplicate;
				return result;
			}
			if (rangeOverlap(window, result.windows[j])) {
				result.error = ParseError::Overlap;
				return result;
			}
		}
		expectedOffset = alignedEnd;
	}
	if (expectedOffset != result.imageBytes) {
		result.error = ParseError::Total;
		return result;
	}
	result.valid = true;
	result.error = ParseError::None;
	return result;
}

static inline const Window *find(const Layout &layout, const char *query)
{
	if (!layout.valid || !query) return nullptr;
	char name[32] = {};
	unsigned length = 0;
	while (length < 31 && query[length]) {
		const unsigned char c = static_cast<unsigned char>(query[length]);
		if (c < 0x21 || c > 0x7e) return nullptr;
		name[length] = char(c);
		++length;
	}
	if (!length || query[length] != '\0') return nullptr;
	for (uint32_t i = 0; i < layout.count; ++i) if (sameName(layout.windows[i].name, name)) return &layout.windows[i];
	return nullptr;
}

} // namespace SmUfsRoot
