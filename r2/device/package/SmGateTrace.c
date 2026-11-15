#include <stdbool.h>
// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SmGateTrace.h"
static struct SmGateFields fields;
// At most one bounded publisher. A loss advances a generation; only a complete
// holder event can synchronize the record to that generation.
static uint32_t writing;
static uint64_t loss_generation;
static uint64_t synchronized_loss;
static int claim(void) {
 for (unsigned attempt = 0; attempt < 64; ++attempt) {
  uint32_t expected = 0;
  if (__atomic_compare_exchange_n(&writing, &expected, 1U, false,
                                  __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return 1;
 }
 return 0;
}
static void release(void) { __atomic_store_n(&writing, 0U, __ATOMIC_RELEASE); }
static void begin(void) {
 __atomic_add_fetch(&fields.epoch, 1U, __ATOMIC_RELAXED); // odd: publishing
 __atomic_thread_fence(__ATOMIC_RELEASE);
}
static void end(void) {
 __atomic_thread_fence(__ATOMIC_RELEASE);
 __atomic_add_fetch(&fields.epoch, 1U, __ATOMIC_RELAXED); // even: complete
}
#define PUT64(field, value) __atomic_store_n(&fields.field, (uint64_t)(value), __ATOMIC_RELAXED)
#define PUT32(field, value) __atomic_store_n(&fields.field, (uint32_t)(value), __ATOMIC_RELAXED)
#define ADD32(field) __atomic_add_fetch(&fields.field, 1U, __ATOMIC_RELAXED)
static void mark_lost(void) {
 ADD32(dropped);
 __atomic_add_fetch(&loss_generation, 1ULL, __ATOMIC_SEQ_CST);
}
// Called with the claim held and epoch odd. A newer loss stays visible: no
// publisher clears a shared invalidation flag after releasing its claim.
static void synchronize_holder(uint64_t generation) {
 if (__atomic_load_n(&synchronized_loss, __ATOMIC_RELAXED) != generation) {
  PUT64(requester_gate, 0); PUT64(requester_since, 0);
  PUT32(requester_tid, 0); PUT32(requester_stage, 0);
 }
 __atomic_store_n(&synchronized_loss, generation, __ATOMIC_RELAXED);
}
void smgate_request(uint64_t gate, uint32_t tid, uint64_t now, uint32_t stage) {
 if (!claim()) { mark_lost(); return; }
 begin();
 PUT64(requester_gate, gate);
 PUT64(requester_since, now);
 PUT32(requester_tid, tid);
 PUT32(requester_stage, stage);
 ADD32(waits);
 end();
 release();
}
void smgate_acquired(uint64_t gate, uint32_t tid, uint64_t now) {
 if (!claim()) { mark_lost(); return; }
 const uint64_t generation = __atomic_load_n(&loss_generation, __ATOMIC_SEQ_CST);
 begin();
 PUT64(holder_gate, gate);
 PUT64(holder_since, now);
 PUT32(holder_tid, tid);
 PUT32(last_holder_tid, tid);
 PUT32(holder_stage, 2U);
 ADD32(acquires);
 synchronize_holder(generation);
 end();
 release();
}
void smgate_released(uint64_t gate, uint32_t tid) {
 if (!claim()) { mark_lost(); return; }
 const uint64_t generation = __atomic_load_n(&loss_generation, __ATOMIC_SEQ_CST);
 begin();
 PUT64(holder_gate, gate);
 PUT32(holder_tid, 0U);
 PUT32(holder_stage, 0U);
 PUT32(last_holder_tid, tid);
 ADD32(releases);
 synchronize_holder(generation);
 end();
 release();
}
void smgate_stage(uint32_t stage) {
 if (!claim()) { mark_lost(); return; }
 begin();
 // Only the holder's own path calls this, so it cannot rewrite a waiter state.
 PUT32(holder_stage, stage);
 end();
 release();
}
int smgate_read(struct SmGateFields *out) {
 if (!out) return 0;
 for (unsigned attempt = 0; attempt < 3; ++attempt) {
  const uint64_t lost = __atomic_load_n(&loss_generation, __ATOMIC_SEQ_CST);
  uint32_t before = __atomic_load_n(&fields.epoch, __ATOMIC_ACQUIRE);
  if (before & 1U) continue;
  out->holder_gate = __atomic_load_n(&fields.holder_gate, __ATOMIC_RELAXED);
  out->requester_gate = __atomic_load_n(&fields.requester_gate, __ATOMIC_RELAXED);
  out->holder_since = __atomic_load_n(&fields.holder_since, __ATOMIC_RELAXED);
  out->requester_since = __atomic_load_n(&fields.requester_since, __ATOMIC_RELAXED);
  out->holder_tid = __atomic_load_n(&fields.holder_tid, __ATOMIC_RELAXED);
  out->last_holder_tid = __atomic_load_n(&fields.last_holder_tid, __ATOMIC_RELAXED);
  out->holder_stage = __atomic_load_n(&fields.holder_stage, __ATOMIC_RELAXED);
  out->requester_tid = __atomic_load_n(&fields.requester_tid, __ATOMIC_RELAXED);
  out->requester_stage = __atomic_load_n(&fields.requester_stage, __ATOMIC_RELAXED);
  out->waits = __atomic_load_n(&fields.waits, __ATOMIC_RELAXED);
  out->acquires = __atomic_load_n(&fields.acquires, __ATOMIC_RELAXED);
  out->releases = __atomic_load_n(&fields.releases, __ATOMIC_RELAXED);
  out->dropped = __atomic_load_n(&fields.dropped, __ATOMIC_RELAXED);
  const uint64_t synchronized = __atomic_load_n(&synchronized_loss, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  if (before == __atomic_load_n(&fields.epoch, __ATOMIC_ACQUIRE)) {
   if (lost != synchronized ||
       lost != __atomic_load_n(&loss_generation, __ATOMIC_SEQ_CST)) return -1;
   out->epoch = before;
   return 1;
  }
 }
 return 0;
}

#ifdef SMGATE_TEST
// Host-only hooks: hold the writer claim so a transition is forced to be dropped.
void smgate_test_hold(void) { while (!claim()) { } }
void smgate_test_release(void) { release(); }
#endif
