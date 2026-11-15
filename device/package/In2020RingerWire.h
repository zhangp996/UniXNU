/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
/* Existing filename retained. Board policy only; native IOHIDEvent factory
 * now owns the entire event representation. Upper/middle silent, lower ring.
 * The separate vibrate preference and motor driver remain unfinished. */
static inline int in2020_ringer_state(unsigned position) {
 return position>=1 && position<=3 ? int(position==3) : -1;
}
