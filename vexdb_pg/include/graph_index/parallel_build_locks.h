/**
 * Copyright (c) 2026 VexDB-THU
 *
 * Cross-process locks used by PostgreSQL parallel graph builds.
 *
 * PostgreSQL parallel workers join the leader's heavyweight-lock group, so
 * LockPage() and LockRelationForExtension() alone do not serialize sibling
 * workers.  These named LWLocks provide the missing process-to-process
 * exclusion without putting the complete graph insertion behind one lock.
 */
#ifndef VEX_GRAPH_PARALLEL_BUILD_LOCKS_H
#define VEX_GRAPH_PARALLEL_BUILD_LOCKS_H

#include "postgres.h"
#include "storage/lwlock.h"
#include "utils/rel.h"

#define VEX_GRAPH_BUILD_ENTRY_LOCK_STRIPES 128
#define VEX_GRAPH_BUILD_ENTRY_WAIT_LOCK_STRIPES 128
#define VEX_GRAPH_BUILD_STORAGE_LOCK_STRIPES 128
#define VEX_GRAPH_BUILD_EXTENSION_LOCK_STRIPES 128
#define VEX_GRAPH_BUILD_POINT_LOCK_STRIPES 4096

extern LWLockPadded *VexGraphBuildEntryLocks;
extern LWLockPadded *VexGraphBuildEntryWaitLocks;
extern LWLockPadded *VexGraphBuildStorageLocks;
extern LWLockPadded *VexGraphBuildExtensionLocks;
extern LWLockPadded *VexGraphBuildPointLocks;

static inline uint64
vex_graph_build_mix64(uint64 value)
{
    value ^= value >> 30;
    value *= UINT64CONST(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64CONST(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}

static inline LWLock *
vex_graph_build_relation_lock(LWLockPadded *locks, uint32 stripes, Relation relation,
                              uint64 salt)
{
    if (locks == NULL) {
        return NULL;
    }
    uint64 key = ((uint64)RelationGetRelid(relation) << 32) ^ salt;
    return &locks[vex_graph_build_mix64(key) % stripes].lock;
}

static inline LWLock *
vex_graph_build_entry_lock(Relation relation)
{
    return vex_graph_build_relation_lock(VexGraphBuildEntryLocks,
        VEX_GRAPH_BUILD_ENTRY_LOCK_STRIPES, relation, UINT64CONST(0x656e747279));
}

static inline LWLock *
vex_graph_build_entry_wait_lock(Relation relation)
{
    return vex_graph_build_relation_lock(VexGraphBuildEntryWaitLocks,
        VEX_GRAPH_BUILD_ENTRY_WAIT_LOCK_STRIPES, relation, UINT64CONST(0x656e7477616974));
}

static inline LWLock *
vex_graph_build_storage_lock(Relation relation)
{
    return vex_graph_build_relation_lock(VexGraphBuildStorageLocks,
        VEX_GRAPH_BUILD_STORAGE_LOCK_STRIPES, relation, UINT64CONST(0x73746f72616765));
}

static inline LWLock *
vex_graph_build_extension_lock(Relation relation)
{
    return vex_graph_build_relation_lock(VexGraphBuildExtensionLocks,
        VEX_GRAPH_BUILD_EXTENSION_LOCK_STRIPES, relation, UINT64CONST(0x657874656e64));
}

static inline LWLock *
vex_graph_build_point_lock(Relation relation, uint64 point_id, bool base_layer)
{
    if (VexGraphBuildPointLocks == NULL) {
        return NULL;
    }
    uint64 key = ((uint64)RelationGetRelid(relation) << 32) ^ point_id;
    key ^= base_layer ? UINT64CONST(0x62617365) : UINT64CONST(0x7570706572);
    return &VexGraphBuildPointLocks[
        vex_graph_build_mix64(key) % VEX_GRAPH_BUILD_POINT_LOCK_STRIPES].lock;
}

static inline void
vex_graph_build_lock_acquire(LWLock *lock, LWLockMode mode)
{
    if (lock != NULL) {
        LWLockAcquire(lock, mode);
    }
}

static inline void
vex_graph_build_lock_release(LWLock *lock)
{
    if (lock != NULL) {
        LWLockRelease(lock);
    }
}

#endif /* VEX_GRAPH_PARALLEL_BUILD_LOCKS_H */
