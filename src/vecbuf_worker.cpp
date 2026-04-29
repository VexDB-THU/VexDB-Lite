/**
 * Background worker for vector buffer management
 * Handles buffer expansion, eviction, and redistribution
 */

#include "pg_compat.h"

#include <atomic>

extern "C" {
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/latch.h"
#include "tcop/tcopprot.h"
#include "utils/wait_event.h"
PGDLLEXPORT void vecbuf_worker_main(Datum main_arg);
}

#include "vecbuf_shared.h"
#include "vector_buffer_manager.h"
#include "module/parallel_counter.h"
#include "knl/knl_variable.h"

/* Forward declaration from vector_smgr.cpp */
extern VecBufferManager *VecBufMgr;

/* Record eviction statistics with time-based reset */
static void record_evict(VecBufferPool &pool, size_t nevict)
{
    long sec;
    int usec;
    TimestampTz cur_time = GetCurrentTimestamp();
    TimestampDifference(pool.stats.first_evict_time, cur_time, &sec, &usec);
    if (sec > eviction_time_interval) {
        pool.stats.first_evict_time = cur_time;
        pool.stats.nevict = 0;
    } else {
        pool.stats.nevict.fetch_add(nevict, RELAXED_ORDER);
    }
}

/* Check all pools for eviction needs and return pool_offset that needs work */
static int16 check_and_evict_pools(void)
{
    if (!VecBufMgr || !VecBufMgr->buffer_inited)
        return -1;
    
    uint32 nvecbuf = NVecBuf(g_instance.diskann_cxt.vector_buffers);
    
    for (int16 i = 0; i < NVecPool; ++i) {
        if (!VecBufMgr->pool[i])
            continue;
        auto &cur_pool = *VecBufMgr->pool[i];
        if (cur_pool.stats.nblock == 0)
            continue;
        if (VecBufMgr->nalloced >= nvecbuf && cur_pool.need_evict()) {
            return i;
        }
    }
    return -1;
}

void vecbuf_worker_main(Datum main_arg)
{
    int worker_id = DatumGetInt32(main_arg);
    
    /* Establish signal handlers */
    pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
    pqsignal(SIGHUP, SignalHandlerForConfigReload);
    pqsignal(SIGUSR1, procsignal_sigusr1_handler);
    BackgroundWorkerUnblockSignals();
    
    /* Initialize wait event info */
    InitializeWaitEventSupport();
    
    /* Attach to shared memory - the shared state was created by postmaster */
    if (!vecbuf_shared_state) {
        ereport(ERROR, (errmsg("vecbuf_shared_state is NULL in worker")));
    }
    if (!vecbuf_shared_state->enable_buffer_manager) {
        return;
    }
    vecbuf_shared_ctx = vecbuf_shared_state->vecbuf_ctx;
    if (!vecbuf_shared_ctx) {
        ereport(ERROR, (errmsg("vecbuf_shared_ctx is NULL in worker")));
    }
    
    /* Set our latch for signaling from frontends */
    vecbuf_shared_state->worker_latch = MyLatch;
    pg_atomic_fetch_add_u32(&vecbuf_shared_state->worker_count, 1);
    
    /* Main loop */
    for (;;) {
        if (ShutdownRequestPending)
            break;
        
        /* Handle config reload */
        if (ConfigReloadPending) {
            ConfigReloadPending = false;
            ProcessConfigFile(PGC_SIGHUP);
            if (!vecbuf_shared_state->enable_buffer_manager) {
                break;
            }
        }
        
        if (!VecBufMgr || !VecBufMgr->buffer_inited) {
            WaitLatch(MyLatch,
                      WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                      1000L,
                      PG_WAIT_EXTENSION);
            ResetLatch(MyLatch);
            continue;
        }
        
        int16 pool_offset = (int16)pg_atomic_read_u32(&vecbuf_shared_state->pool_offset_to_write);
        
        if (pool_offset < 0) {
            /* Wait for signal from backend */
            WaitLatch(MyLatch,
                      WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                      1000L,
                      PG_WAIT_EXTENSION);
            ResetLatch(MyLatch);
            
            /* Proactively find work after timeout */
            pool_offset = check_and_evict_pools();
            if (pool_offset < 0)
                continue;
        } else {
            /* Clear the request */
            pg_atomic_write_u32(&vecbuf_shared_state->pool_offset_to_write, -1);
        }
        
        /* Eviction loop - continue while eviction is needed */
        bool first_evict = true;
        size_t nevic = 0;
        bool evicted;
        constexpr int max_loop = 1000;
        int loop_count = max_loop;
        
        for (; loop_count > 0; --loop_count) {
            if (ShutdownRequestPending)
                break;
            
            VecBufMgr->expand_or_recollect_space(pool_offset, evicted);
            
            if (!VecBufMgr->pool[pool_offset] || !evicted)
                break;
            
            auto &cur_pool = *VecBufMgr->pool[pool_offset];
            if (first_evict) {
                first_evict = false;
                if (cur_pool.stats.first_evict_time == 0) {
                    cur_pool.stats.first_evict_time = GetCurrentTimestamp();
                }
            }
            
            ++nevic;
            constexpr size_t step_size = 20ul;
            if (nevic > step_size) {
                record_evict(cur_pool, nevic);
                nevic = 0;
            }
            
            if (!cur_pool.need_evict())
                break;
        }
        
        /* Record remaining evictions */
        if (nevic > 0 && VecBufMgr->pool[pool_offset]) {
            record_evict(*VecBufMgr->pool[pool_offset], nevic);
        }
        
        /* Re-queue work if loop exhausted before eviction completed */
        if (loop_count <= 0) {
            pg_atomic_write_u32(&vecbuf_shared_state->pool_offset_to_write, pool_offset);
        }
        
        /* Try to redistribute blocks between pools */
        VecBufMgr->try_redistribute_block();
    }
    
    pg_atomic_fetch_sub_u32(&vecbuf_shared_state->worker_count, 1);
}
