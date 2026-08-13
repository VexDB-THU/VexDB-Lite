/**
 * Copyright (c) 2026 VexDB-THU
 */

#ifndef GRAPH_INDEX_STATE_H
#define GRAPH_INDEX_STATE_H

#include "c.h"
#include "utils/relcache.h"

enum class GIStateOper {
    GET_UNDER_VACUUM,
    SET_UNDER_VACUUM,
};

union GIStateInput {
    struct {
        bool val;
        bool set_result;
    } bool_val;
};

/* Entry stored in the shared hash table.  index_oid is the key (first field).
 * LWLock on the whole table provides all synchronisation, so plain uint8
 * flags are sufficient — no atomics needed. */
typedef struct GIStateEntry {
    Oid index_oid;
    uint8 under_vacuum;
    uint8 under_qt_update;
    uint8 under_async_insert;
} GIStateEntry;

/* Query or mutate the per-index state map */
void graph_index_get_state(Relation index, GIStateOper op, GIStateInput &input);

/* One-time initialisation — creates the shared hash table and acquires the
 * LWLock.  Must be called inside shmem_startup_hook, after shared memory is
 * available. */
void graph_index_state_init(void);

#endif /* GRAPH_INDEX_STATE_H */
