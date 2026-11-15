// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

// Probe64 owns this entire pool; no inherited descriptor memory is initialized.
namespace SmUfsOwned {
constexpr uint32_t kBytes = SmUfsProbe59::kBytes;
constexpr uint32_t kTask = 0x1000;
constexpr uint32_t kAddress64 = 1U << 24;
enum class Error : unsigned { None, Allocation, Prepare, Layout, Length, Address, Overlap, Busy, Readback };
struct State {
	uint64_t phys = 0, contiguous = 0;
	uint64_t listReadback = 0, taskReadback = 0;
	uint32_t cap = 0, prepareResult = 0xffffffffU;
	uint32_t taskRsrReadback = 0xffffffffU, taskDbrReadback = 0xffffffffU;
	Error error = Error::None;
	bool allocated = false, prepared = false, validated = false;
	bool selectAttempted = false, selected = false;
};
static bool validate(State &s, const uint8_t *virt, uint64_t length,
                     uint64_t oldList, uint64_t oldTask)
{
	using namespace SmUfsProbe59;
	s.validated = false;
	if (!s.allocated) { s.error = Error::Allocation; return false; }
	if (!s.prepared) { s.error = Error::Prepare; return false; }
	if (!layoutValid(s.phys, virt)) { s.error = Error::Layout; return false; }
	if (length < kBytes || s.contiguous < kBytes) { s.error = Error::Length; return false; }
	if (!(s.cap & kAddress64) && (s.phys > 0xffffffffULL || kBytes - 1 > 0xffffffffULL - s.phys)) {
		s.error = Error::Address; return false;
	}
	if (!oldList || oldList > ~uint64_t(0) - kBytes || oldTask > ~uint64_t(0) - kTaskBytes ||
	    rangesOverlap(s.phys, kBytes, oldList, kBytes) ||
	    (oldTask && rangesOverlap(s.phys, kBytes, oldTask, kTaskBytes))) {
		s.error = Error::Overlap; return false;
	}
	s.validated = true; s.error = Error::None;
	return true;
}

template <class Port>
static bool select(State &s, Port &port)
{
	if (!s.validated || s.selectAttempted) return false;
	if (port.read(0x58) || port.read(0x78) || (port.read(0x60) & 1) || (port.read(0x80) & 1) ||
	    (port.read(0x20) & SmUfsProbe59::kFatal) || !(port.read(0x34) & 1)) {
		s.error = Error::Busy; return false;
	}
	// From this point the controller may reference the allocation, even if a readback fails.
	s.selectAttempted = true;
	port.write(0x50, uint32_t(s.phys)); port.write(0x54, uint32_t(s.phys >> 32));
	port.write(0x70, uint32_t(s.phys + kTask)); port.write(0x74, uint32_t((s.phys + kTask) >> 32));
	s.listReadback = uint64_t(port.read(0x50)) | (uint64_t(port.read(0x54)) << 32);
	s.taskReadback = uint64_t(port.read(0x70)) | (uint64_t(port.read(0x74)) << 32);
	s.taskDbrReadback = port.read(0x78); s.taskRsrReadback = port.read(0x80);
	if (s.listReadback != s.phys || s.taskReadback != s.phys + kTask || port.read(0x58) ||
	    s.taskDbrReadback || (port.read(0x60) & 1) || (s.taskRsrReadback & 1) ||
	    (port.read(0x20) & SmUfsProbe59::kFatal)) {
		s.error = Error::Readback; return false;
	}
	s.selected = true; s.error = Error::None;
	return true;
}
}  // namespace SmUfsOwned
