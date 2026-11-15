// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "RootOwnerRead.h"
#include "BlockReadCore.h"

namespace SmUfsRootWindowIo {
constexpr uint64_t kSector = 512, kMaxSectors = 128;
using SmUfsBlockRead::Error;

struct IgnoreReadFailure {
	template <class Backend> void capture(const Backend &, Error, uint64_t, bool, bool) {}
	void emit() {}
};

// Caller holds the context gate. Scratch belongs to this request, never shared
// with a completion or a concurrent window/media request.
template <class Backend> struct Physical {
	Backend &backend;
	Error status = Error::Success;
	uint64_t lastLba = 0;
	bool attempted = false, requestExpired = false;
	bool readPhysical(uint64_t lba, uint8_t *out)
	{
		lastLba = lba;
		attempted = true;
		if (backend.expired()) {
			requestExpired = true;
			status = Error::Timeout;
			return false;
		}
		if (lba >= backend.capacity() || lba > 0xffffffffULL) { status = Error::Argument; return false; }
		status = backend.read(uint32_t(lba), out);
		// A completed block is forward progress. Per-command UFS limits remain
		// in backend.read, and the window and policy bound the block count.
		if (status == Error::Success) backend.begin();
		return status == Error::Success;
	}
};

template <class Backend, class Buffer, class Completion, class Policy, class Observer = IgnoreReadFailure>
static Error executeChecked(Backend &backend, Buffer &buffer, const SmUfsRoot::Layout &layout,
                     const SmUfsRoot::Window &window, const SmUfsBlockRead::Request &request,
                     Completion &completion, Policy &policy, Observer observer = {})
{
	if (!buffer.present() || !completion.present()) { backend.reject(); return Error::Argument; }
	if (!buffer.inputOnly()) { backend.reject(); return Error::WriteProtected; }
	const Error attributes = SmUfsBlockRead::attributeStatus(request.attributes);
	if (attributes != Error::Success) { backend.reject(); return attributes; }
	const uint64_t sectors = window.size / kSector;
	if (!layout.valid || window.size % kSector || window.blockBytes != kSector ||
	    !request.count || request.count > kMaxSectors || request.block >= sectors ||
	    request.count > sectors - request.block || buffer.length() < request.count * kSector) {
		backend.reject(); return Error::Argument;
	}
	const uint64_t bytes = request.count * kSector;
	const uint64_t allocated = bytes + Policy::scratchBytes;
	uint8_t *staging = backend.allocate(allocated);
	if (!staging) { backend.reject(); return Error::NoMemory; }
	backend.lock();
	if (!backend.ready()) {
		backend.unlock(); backend.free(staging, allocated); backend.reject(); return Error::NotReady;
	}
	backend.accept(request.count);
	backend.begin();
	backend.stage(3); // policy validation and physical read
	Physical<Backend> physical{backend};
	const auto result = policy.read(physical, layout, window,
	    SmUfsRoot::ReadRequest{request.block * kSector, bytes, staging}, staging + bytes);
	Error status = physical.status;
	if (result != SmUfsRoot::ReadError::Success && status == Error::Success) status = Error::Io;
	if (status != Error::Success) {
		observer.capture(backend, physical.status, physical.lastLba,
		                 physical.attempted, physical.requestExpired);
		// The request deadline is checked before issuing the next command.
		// The timed-out request still fails, but no DMA needs quarantine.
		if (!physical.requestExpired) backend.close();
	}
	backend.stage(6); // leaving with the gate still held
	backend.unlock();
	observer.emit();
	uint64_t actual = status == Error::Success ? buffer.copy(staging, bytes) : 0;
	if (actual > bytes) { actual = bytes; status = Error::Io; backend.lock(); backend.close(); backend.unlock(); }
	if (status == Error::Success && actual != bytes) {
		status = Error::Io; backend.lock(); backend.close(); backend.unlock();
	}
	backend.free(staging, allocated);
	backend.completed();
	completion.finish(status, actual); // No context gate held, reentry is permitted.
	backend.retired();
	return Error::Success;
}
template <class Backend, class Buffer, class Completion>
static Error execute(Backend &backend, Buffer &buffer, const SmUfsRoot::Layout &layout,
                    const SmUfsRoot::Window &window, const SmUfsBlockRead::Request &request,
                    Completion &completion)
{
 SmUfsOwnerRead::PlainReader plain;
 return executeChecked(backend, buffer, layout, window, request, completion, plain);
}
} // namespace SmUfsRootWindowIo
