// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

namespace SmUfsBsdProof {
constexpr uint8_t kSentinel = 0xa7;

inline bool deviceMap(uint32_t major, uint32_t minor, uint32_t block, uint32_t character)
{
	return major < 256 && minor <= 0xffffff && block == ((major << 24) | minor) && character == block;
}
inline bool decimalName(const char *name, uint32_t unit, uint32_t slot = 0)
{
	if (!name) return false;
	char expected[32] = {'d', 'i', 's', 'k'};
	unsigned end = 4;
	const uint32_t values[2] = {unit, slot};
	for (unsigned part = 0; part < (slot ? 2U : 1U); part++) {
		if (part) expected[end++] = 's';
		char digits[10]; unsigned count = 0; uint32_t value = values[part];
		do { digits[count++] = char('0' + value % 10); value /= 10; } while (value);
		while (count) expected[end++] = digits[--count];
	}
	expected[end] = 0;
	for (unsigned i = 0; i <= end; i++) if (name[i] != expected[i]) return false;
	return true;
}
inline bool identity(uint32_t wholeMajor, uint32_t wholeUnit, uint32_t wholeMinor, const char *wholeName,
                     uint32_t userMajor, uint32_t userUnit, uint32_t userMinor, const char *userName, uint32_t slot)
{
	return slot && slot <= 32 && wholeMajor < 256 && wholeMajor == userMajor && wholeUnit == userUnit &&
	    wholeMinor != userMinor && wholeMinor <= 0xffffff && userMinor <= 0xffffff &&
	    decimalName(wholeName, wholeUnit) && decimalName(userName, wholeUnit, slot);
}
inline bool workerStable(bool launched, bool launchSucceeded, bool finished, uint32_t completions)
{
	return launched ? launchSucceeded && finished && completions == 1 : !finished && completions == 0;
}
inline bool closedCounts(uint32_t blocks, uint32_t characters, bool blockHeld = false,
                         bool characterHeld = false, bool mediaOpen = false)
{
	return !blocks && !characters && !blockHeld && !characterHeld && !mediaOpen;
}

// Native DIO completion calls uio_setresid only; it does not uio_update.
// The completed byte count therefore comes from residual alone, while the raw
// callback must preserve its starting offset even on a full or clipped read.
inline bool transfer(int error, int expectedError, uint64_t requested, uint64_t start,
                     uint64_t residual, uint64_t end, uint64_t completed)
{
	return error == expectedError && completed <= requested &&
	    residual == requested - completed && end == start;
}
inline bool equal(const void *actual, uint64_t actualSize, uint64_t actualOffset,
                  const void *reference, uint64_t referenceSize, uint64_t referenceOffset, uint64_t length)
{
	if (!actual || !reference || actualOffset > actualSize || referenceOffset > referenceSize ||
	    length > actualSize - actualOffset || length > referenceSize - referenceOffset) return false;
	const uint8_t *a = static_cast<const uint8_t *>(actual) + actualOffset;
	const uint8_t *b = static_cast<const uint8_t *>(reference) + referenceOffset;
	for (uint64_t i = 0; i < length; i++) if (a[i] != b[i]) return false;
	return true;
}
inline bool sentinel(const void *buffer, uint64_t size, uint64_t from)
{
	if (!buffer || from > size) return false;
	const uint8_t *bytes = static_cast<const uint8_t *>(buffer);
	for (uint64_t i = from; i < size; i++) if (bytes[i] != kSentinel) return false;
	return true;
}
}
