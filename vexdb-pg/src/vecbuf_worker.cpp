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

/* Forward declaration from vector_smgr.cpp */
extern VecBufferManager *VecBufMgr;

/* Check all pools for eviction needs */
static void check_and_evict_pools(void)
{
    return;
}

void vecbuf_worker_main(Datum main_arg)
{
    int worker_id = DatumGetInt32(main_arg);
    
    /* Establish signal handlers */
    pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
    pqsignal(SIGHUP, SignalHandlerForConfigReload);
    pqsignal(SIGUSR1, procsignal_sigusr1_handler);
    BackgroundWorkerUnblockSignals();
    
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
    while (!ShutdownRequestPending) {
        int16 pool_offset;
        
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
        
        /* Get pool that needs work */
        pool_offset = (int16)pg_atomic_read_u32(&vecbuf_shared_state->pool_offset_to_write);
        
        if (pool_offset >= 0 && pool_offset < NVecPool) {
            /* Clear the request */
            pg_atomic_write_u32(&vecbuf_shared_state->pool_offset_to_write, 0xFFFFFFFF);
        }
        
        /* Check all pools for eviction needs periodically */
        check_and_evict_pools();
        
        /* Wait for work or timeout */
        WaitLatch(MyLatch,
                  WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                  1000L,  /* 1 second timeout */
                  PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);
    }
    
    pg_atomic_fetch_sub_u32(&vecbuf_shared_state->worker_count, 1);
}
