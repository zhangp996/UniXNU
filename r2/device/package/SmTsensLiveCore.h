// SPDX-License-Identifier: MIT
// Measurement state only. Hardware decoder is the already-qualified H4A code.
#pragma once
#include "SmTsensReadOnly.h"
enum SmTsensReadResult { SmTsensOk, SmTsensInvalid, SmTsensStale, SmTsensBadArgument };
struct SmTsensLiveSnapshot {
 SmTsensGroup groups[2];
 uint64_t timestamp;
 uint64_t generation;
};
// Caller serializes publication and reading. A failed acquisition replaces the
// valid mask too; no previous sample is silently reused as current data.
static inline void sm_tsens_publish(SmTsensLiveSnapshot *state,const SmTsensGroup groups[2],uint64_t now) {
 state->groups[0]=groups[0];state->groups[1]=groups[1];state->timestamp=now;++state->generation;
}
static inline SmTsensReadResult sm_tsens_live_read(const SmTsensLiveSnapshot *state,
 unsigned group,unsigned channel,uint64_t now,uint64_t maximum_age,
 int32_t *millicelsius,uint64_t *timestamp) {
 if(!state || !millicelsius || !timestamp || group>=2 || channel>=16 || !maximum_age)return SmTsensBadArgument;
 if(!state->generation || !(state->groups[group].valid&(1U<<channel)))return SmTsensInvalid;
 if(now<state->timestamp || now-state->timestamp>maximum_age)return SmTsensStale;
 *millicelsius=state->groups[group].millicelsius[channel];*timestamp=state->timestamp;
 return SmTsensOk;
}
