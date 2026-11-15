// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

namespace SmUfsBlockRead {
constexpr uint64_t kBlockBytes = 4096;
constexpr uint64_t kMaxBlocks = 16;
enum class Error : unsigned { Success, Argument, WriteProtected, NotReady, NoMemory, Io, Timeout, Unsupported };
struct Attributes {
	uint16_t options = 0;
	uint8_t reserved0024 = 0;
	uint32_t reserved0032 = 0;
	uint64_t reserved0064 = 0;
};
struct Request { uint64_t block = 0, count = 0; Attributes attributes{}; };

static Error attributeStatus(const Attributes &a)
{
	if ((a.options & 0xffceU) || a.reserved0024 || a.reserved0032 || a.reserved0064)
		return Error::Argument;
	if (a.options & 0x0011U) return Error::Unsupported;
	return Error::Success;
}

// Backend owns serialization and persistent transport state. Buffer and completion
// wrappers retain no caller stack pointers after this synchronous call returns.
template <class Backend, class Buffer, class Completion>
static Error execute(Backend &backend, Buffer &buffer, const Request &request, Completion &completion)
{
	if (!buffer.present() || !completion.present()) { backend.reject(); return Error::Argument; }
	if (!buffer.inputOnly()) { backend.reject(); return Error::WriteProtected; }
	const Error attributes = attributeStatus(request.attributes);
	if (attributes != Error::Success) { backend.reject(); return attributes; }
	const uint64_t capacity = backend.capacity();
	if (!request.count || request.count > kMaxBlocks || request.block >= capacity ||
	    request.count > capacity - request.block || request.block > 0xffffffffULL ||
	    request.count - 1 > 0xffffffffULL - request.block ||
	    request.count > ~uint64_t(0) / kBlockBytes || buffer.length() < request.count * kBlockBytes) {
		backend.reject(); return Error::Argument;
	}
	const uint64_t bytes = request.count * kBlockBytes;
	uint8_t *staging = backend.allocate(bytes);
	if (!staging) { backend.reject(); return Error::NoMemory; }
	backend.lock();
	if (!backend.ready()) {
		backend.unlock(); backend.free(staging, bytes); backend.reject(); return Error::NotReady;
	}
	backend.accept(request.count);
	backend.begin();
	Error status = Error::Success;
	uint64_t valid = 0;
	for (uint64_t i = 0; i < request.count; i++) {
		if (backend.expired()) { status = Error::Timeout; backend.close(); break; }
		status = backend.read(uint32_t(request.block + i), staging + valid);
		if (status != Error::Success) { backend.close(); break; }
		valid += kBlockBytes;
	}
	backend.unlock();
	uint64_t actual = valid ? buffer.copy(staging, valid) : 0;
	if (actual > valid) { actual = valid; status = Error::Io; }
	if (actual != valid) status = Error::Io;
	if (actual != valid) { backend.lock(); backend.close(); backend.unlock(); }
	backend.free(staging, bytes);
	backend.completed();
	completion.finish(status, actual);
	backend.retired();
	return Error::Success;
}
}  // namespace SmUfsBlockRead
