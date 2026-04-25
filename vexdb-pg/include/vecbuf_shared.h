/**
 * Shared state for vector buffer manager
 * Stored in main shared memory, accessible from all processes
 */

#ifndef VECBUF_SHARED_H
#define VECBUF_SHARED_H

#include "storage/lwlock.h"
#include "storage/latch.h"
#include "port/atomics.h"
#include "utils/memutils.h"

typedef struct VecBufSharedState {
    /* The shared memory context for all vector buffer allocations */
    MemoryContext vecbuf_ctx;
    
    /* Background worker communication */
    Latch *worker_latch;
    pg_atomic_uint32 worker_count;
    pg_atomic_uint32 pool_offset_to_write;  /* -1 (wrapped) means no pending work */
    
    /* Configuration (set at startup, immutable after) */
    Size vector_buffers;
    bool enable_buffer_manager;
    int vector_buffer_workers;
    
    /* LWLock tranche ID */
    int lwlock_tranche_id;
} VecBufSharedState;

/* Global pointer to shared state - in main shmem */
extern VecBufSharedState *vecbuf_shared_state;

/* Global shared memory context - used by SharedCtxAllocator */
extern MemoryContext vecbuf_shared_ctx;

/* VecBufferManager pointer */
struct VecBufferManager;
extern VecBufferManager *VecBufMgr;

/* LWLock for buffer expansion */
extern LWLock *VectorBufferLock;

/* Initialization functions */
extern void init_vector_smgr(void);

#endif /* VECBUF_SHARED_H */
