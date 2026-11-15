/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMUFS_RT_CORE_VERSION 1u
#define SMUFS_RT_EVENT_CAPACITY 64u
#define SMUFS_RT_FIRST_EVENT 1u
#define SMUFS_RT_LAST_EVENT (SMUFS_RT_EVENT_CAPACITY - 1u)
#define SMUFS_RT_SNAPSHOT_RETRIES 4u

typedef struct SmUfsRootTraceRecord {
    uint32_t count;
    uint32_t sequence;
    uint32_t arg;
    uint32_t result_kind;
    int32_t result;
    uint32_t dev_valid;
    uint32_t dev;
} SmUfsRootTraceRecord;

typedef struct SmUfsRootTraceState {
    uint32_t enabled;
    uint32_t version;
    uint32_t sequence;
    uint32_t writes;
    uint32_t lost;
    uint32_t writer_lock[SMUFS_RT_EVENT_CAPACITY];
    SmUfsRootTraceRecord records[SMUFS_RT_EVENT_CAPACITY];
} SmUfsRootTraceState;

typedef struct SmUfsRootTraceSnapshot {
    uint32_t enabled;
    uint32_t version;
    uint32_t sequence;
    uint32_t writes;
    uint32_t lost;
    uint32_t partial;
    SmUfsRootTraceRecord records[SMUFS_RT_EVENT_CAPACITY];
} SmUfsRootTraceSnapshot;

static inline uint32_t smufs_rt_core_increment(uint32_t *counter)
{
    uint32_t value = __atomic_load_n(counter, __ATOMIC_RELAXED);
    for (uint32_t attempt = 0u; attempt < 4u; ++attempt) {
        if (value == UINT32_MAX) return 0u;
        const uint32_t next = value + 1u;
        if (__atomic_compare_exchange_n(counter, &value, next, 1,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return next;
    }
    return 0u;
}

static inline void smufs_rt_core_lost(SmUfsRootTraceState *state)
{
    // UINT32_MAX means a saturated count or contention prevented exact accounting.
    if (!smufs_rt_core_increment(&state->lost)) {
        __atomic_store_n(&state->lost, UINT32_MAX, __ATOMIC_RELEASE);
    }
}

static inline void smufs_rt_core_emit(SmUfsRootTraceState *state,
                                       uint32_t event_id,
                                       uint32_t arg,
                                       uint32_t result_kind,
                                       int32_t result,
                                       uint32_t dev_valid,
                                       uint32_t dev)
{
    if (state == (SmUfsRootTraceState *)0 ||
        __atomic_load_n(&state->enabled, __ATOMIC_ACQUIRE) == 0u) {
        return;
    }
    if (event_id < SMUFS_RT_FIRST_EVENT || event_id > SMUFS_RT_LAST_EVENT) {
        smufs_rt_core_lost(state);
        return;
    }

    uint32_t expected = 0u;
    if (!__atomic_compare_exchange_n(&state->writer_lock[event_id], &expected, 1u,
                                     1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        smufs_rt_core_lost(state);
        return;
    }

    SmUfsRootTraceRecord *record = &state->records[event_id];
    const uint32_t old_count = __atomic_load_n(&record->count, __ATOMIC_RELAXED);
    const uint32_t sequence = smufs_rt_core_increment(&state->sequence);
    if (!sequence || old_count == UINT32_MAX) {
        smufs_rt_core_lost(state);
        __atomic_store_n(&state->writer_lock[event_id], 0u, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&record->count, old_count + 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&record->arg, arg, __ATOMIC_RELAXED);
    __atomic_store_n(&record->result_kind, result_kind, __ATOMIC_RELAXED);
    __atomic_store_n(&record->result, result, __ATOMIC_RELAXED);
    __atomic_store_n(&record->dev_valid, dev_valid != 0u ? 1u : 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&record->dev, dev_valid != 0u ? dev : 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&record->sequence, sequence, __ATOMIC_RELEASE);
    if (!smufs_rt_core_increment(&state->writes)) smufs_rt_core_lost(state);
    __atomic_store_n(&state->writer_lock[event_id], 0u, __ATOMIC_RELEASE);
}

static inline void smufs_rt_core_snapshot_record(const SmUfsRootTraceRecord *source,
                                                  SmUfsRootTraceRecord *destination)
{
    __atomic_store_n(&destination->sequence,
                     __atomic_load_n(&source->sequence, __ATOMIC_ACQUIRE), __ATOMIC_RELAXED);
    __atomic_store_n(&destination->count,
                     __atomic_load_n(&source->count, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    __atomic_store_n(&destination->arg,
                     __atomic_load_n(&source->arg, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    __atomic_store_n(&destination->result_kind,
                     __atomic_load_n(&source->result_kind, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    __atomic_store_n(&destination->result,
                     __atomic_load_n(&source->result, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    __atomic_store_n(&destination->dev_valid,
                     __atomic_load_n(&source->dev_valid, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
    __atomic_store_n(&destination->dev,
                     __atomic_load_n(&source->dev, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
}

static inline uint32_t smufs_rt_core_snapshot(const SmUfsRootTraceState *state,
                                                SmUfsRootTraceSnapshot *snapshot)
{
    if (state == (const SmUfsRootTraceState *)0 ||
        snapshot == (SmUfsRootTraceSnapshot *)0) {
        return 0u;
    }

    for (uint32_t attempt = 0u; attempt < SMUFS_RT_SNAPSHOT_RETRIES; ++attempt) {
        const uint32_t start_sequence = __atomic_load_n(&state->sequence, __ATOMIC_ACQUIRE);
        const uint32_t start_writes = __atomic_load_n(&state->writes, __ATOMIC_ACQUIRE);
        const uint32_t start_lost = __atomic_load_n(&state->lost, __ATOMIC_ACQUIRE);
        const uint32_t start_enabled = __atomic_load_n(&state->enabled, __ATOMIC_ACQUIRE);
        const uint32_t start_version = __atomic_load_n(&state->version, __ATOMIC_ACQUIRE);
        uint32_t active = 0u;

        for (uint32_t event_id = 0u; event_id < SMUFS_RT_EVENT_CAPACITY; ++event_id) {
            if (__atomic_load_n(&state->writer_lock[event_id], __ATOMIC_ACQUIRE) != 0u) {
                active = 1u;
            }
            smufs_rt_core_snapshot_record(&state->records[event_id],
                                          &snapshot->records[event_id]);
        }

        snapshot->enabled = start_enabled;
        snapshot->version = start_version;
        snapshot->sequence = start_sequence;
        snapshot->writes = start_writes;
        snapshot->lost = start_lost;
        snapshot->partial = 0u;

        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        const uint32_t end_sequence = __atomic_load_n(&state->sequence, __ATOMIC_ACQUIRE);
        const uint32_t end_writes = __atomic_load_n(&state->writes, __ATOMIC_ACQUIRE);
        const uint32_t end_lost = __atomic_load_n(&state->lost, __ATOMIC_ACQUIRE);
        const uint32_t end_enabled = __atomic_load_n(&state->enabled, __ATOMIC_ACQUIRE);
        const uint32_t end_version = __atomic_load_n(&state->version, __ATOMIC_ACQUIRE);
        for (uint32_t event_id = 0u; event_id < SMUFS_RT_EVENT_CAPACITY; ++event_id) {
            if (__atomic_load_n(&state->writer_lock[event_id], __ATOMIC_ACQUIRE) != 0u) {
                active = 1u;
            }
        }
        if (active == 0u && start_sequence == end_sequence &&
            start_writes == end_writes && start_lost == end_lost &&
            start_enabled == end_enabled && start_version == end_version) {
            return 1u;
        }
    }

    snapshot->partial = 1u;
    snapshot->enabled = __atomic_load_n(&state->enabled, __ATOMIC_ACQUIRE);
    snapshot->version = __atomic_load_n(&state->version, __ATOMIC_ACQUIRE);
    snapshot->sequence = __atomic_load_n(&state->sequence, __ATOMIC_ACQUIRE);
    snapshot->writes = __atomic_load_n(&state->writes, __ATOMIC_ACQUIRE);
    snapshot->lost = __atomic_load_n(&state->lost, __ATOMIC_ACQUIRE);
    return 0u;
}

#ifdef __cplusplus
} /* extern "C" */
#endif
