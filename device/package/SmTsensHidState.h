// SPDX-License-Identifier: MIT
// Endpoint state for the panic note. Plain scalars only: the reader formats
// them, and nothing here allocates, takes a lock or touches hardware.
#pragma once
#include <stdint.h>
enum SmTsensHidQueryReason : uint32_t {
 SmTsensQueryAnswered=0, SmTsensQueryOffline=1, SmTsensQueryType=2,
 SmTsensQueryMatching=3, SmTsensQueryOptions=4, SmTsensQueryBackend=5,
 SmTsensQueryPending=6
};
struct SmTsensHidQuery {
 uint64_t seen,temperature,answered,rejected,caller;
 uint32_t type,options,matching,reason;
};
struct SmTsensHidState {
 uint32_t class_error;              // register_class() errno, 0 when it succeeded
 uint32_t created;                  // endpoints whose start() returned true
 uint32_t endpoint_error[2];        // live_copy errno at create time, or ENXIO
 uint32_t requested[2];             // original base start asked to publish
 uint32_t event_probe[2];           // kernel-only: 0 untested, 1 event returned/released, 2 NULL, 3 no endpoint
 uint32_t published[2];             // this service called registerService
 uint32_t matched[2];               // lookup found the instance in MatchedState
 uint32_t group[2],channel[2],location[2],product_ok[2];
 uint32_t last_error;               // last conversion/live_read errno
 // Query counters are monotonic atomics written by client threads; the accessor
 // fills them individually after the snapshot copy, not through the seqlock.
 uint64_t queries,answered,offline,refused;
 // Last request shape and completion per physical endpoint. Protected by the
 // same lock as the state snapshot; matching is only a boolean, never dereferenced.
 SmTsensHidQuery query[2];
 const char *step;
};
