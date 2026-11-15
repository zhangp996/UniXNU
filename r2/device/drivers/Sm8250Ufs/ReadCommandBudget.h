// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

namespace SmUfsReadBudget {
enum class Stop : unsigned { None, Count, Deadline };
struct State {
	bool enabled = false;
	uint32_t issued = 0, limit = 0;
	uint64_t deadline = 0;
	Stop stop = Stop::None;
	bool allow(uint64_t now) {
		if (!enabled) return true;
		if (stop != Stop::None) return false;
		if (now >= deadline) { stop = Stop::Deadline; return false; }
		if (issued >= limit) { stop = Stop::Count; return false; }
		return true;
	}
	void submitted() { if (enabled) ++issued; }
};
}
