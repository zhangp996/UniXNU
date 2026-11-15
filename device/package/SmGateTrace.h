// SPDX-License-Identifier: BSD-2-Clause-Patent
// Observation only. Records, separately, who holds the storage context gate
// (with the bounded region it is in) and who is waiting for it. Publication is
// a seqlock with a serialized writer: an odd epoch marks a write in progress,
// a writer that cannot take the claim records a loss instead of interleaving.
// A newer loss cannot be cleared by an older holder publisher.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct SmGateFields {
 uint64_t holder_gate, requester_gate;
 uint64_t holder_since, requester_since;
 uint32_t holder_tid, last_holder_tid, holder_stage;
 uint32_t requester_tid, requester_stage;
 uint32_t waits, acquires, releases, dropped;
 uint32_t epoch;          // odd while a writer is publishing
};
void smgate_request(uint64_t gate, uint32_t tid, uint64_t now, uint32_t stage);
void smgate_acquired(uint64_t gate, uint32_t tid, uint64_t now);
void smgate_released(uint64_t gate, uint32_t tid);
void smgate_stage(uint32_t stage);
// 1: a complete generation. 0: a write was in progress or moved under the reader.
// -1: a transition was dropped, so the holder record is stale until an acquire or
// release republishes it without a newer loss. Requests/stages cannot recover it.
// Both non-1 values mean the caller must not use the fields.
int smgate_read(struct SmGateFields *out);
#ifdef __cplusplus
}
#endif
