// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include "RootContainer.h"

namespace SmUfsRoot {

enum class ReadError : uint32_t {
	Success,
	Argument,
	NotReady,
	Timeout,
	Io,
	WriteProtected,
};

struct ReadRequest {
	uint64_t offset = 0;
	uint64_t bytes = 0;
	uint8_t *output = nullptr;
};

static inline bool mulAddPhysical(uint64_t firstLba, uint64_t windowOffset,
                                  uint64_t offset, uint64_t *absolute)
{
	if (firstLba > ~uint64_t(0) / kPhysicalBlockBytes) return false;
	uint64_t base = firstLba * kPhysicalBlockBytes;
	if (addOverflow(base, windowOffset, &base) || addOverflow(base, offset, absolute)) return false;
	return true;
}

static inline bool rangeWithin(const Window &window, uint64_t offset, uint64_t bytes)
{
	return offset <= window.size && bytes <= window.size - offset;
}

// Backend must provide:
//   bool readPhysical(uint64_t lba, uint8_t block[4096]);
// It owns the transport lock and retains the physical DMA/context. The helper
// never retains caller pointers after it returns and never writes to storage.
template <typename Backend>
static ReadError readWindowWithScratch(Backend &backend, const Layout &layout, const Window &window,
                            const ReadRequest &request, uint8_t *block)
{
	if (!block) return ReadError::Argument;
	if (!layout.valid || !request.output || !request.bytes || !rangeWithin(window, request.offset, request.bytes))
		return ReadError::Argument;
	if (!window.blockBytes || window.blockBytes != kWindowBlockBytes)
		return ReadError::Argument;
	uint64_t firstByte = 0;
	if (!mulAddPhysical(layout.firstLba, window.offset, request.offset, &firstByte))
		return ReadError::Argument;
	uint64_t endByte = 0;
	if (addOverflow(firstByte, request.bytes, &endByte)) return ReadError::Argument;
	const uint64_t firstPhysical = firstByte / kPhysicalBlockBytes;
	const uint64_t lastPhysical = (endByte - 1) / kPhysicalBlockBytes;
	uint64_t physicalCount = lastPhysical - firstPhysical + 1;
	if (!physicalCount || physicalCount > 17) return ReadError::Argument;
	uint64_t copied = 0;
	uint64_t source = firstByte % kPhysicalBlockBytes;
	for (uint64_t physical = firstPhysical; physical <= lastPhysical; ++physical) {
		if (!backend.readPhysical(physical, block)) return ReadError::Io;
		const uint64_t available = kPhysicalBlockBytes - source;
		const uint64_t amount = (request.bytes - copied < available) ? request.bytes - copied : available;
		for (uint64_t i = 0; i < amount; ++i) request.output[copied + i] = block[source + i];
		copied += amount;
		source = 0;
	}
	return copied == request.bytes ? ReadError::Success : ReadError::Io;
}

// Compatibility wrapper for callers that have an independent stack budget.
template <typename Backend>
static ReadError readWindow(Backend &backend, const Layout &layout, const Window &window,
                            const ReadRequest &request)
{
 uint8_t block[kPhysicalBlockBytes] = {};
 return readWindowWithScratch(backend, layout, window, request, block);
}

} // namespace SmUfsRoot
