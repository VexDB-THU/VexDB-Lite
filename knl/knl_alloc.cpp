#include "pg_compat.h"

#include <cstdlib>
#include "knl/knl_variable.h"
#include "distance/distance.h"
#include "vector_smgr.h"
#include "guc_config.h"
#include "shared_alloc_set.h"
#include "vecbuf_shared.h"

extern "C" {
#include "storage/shmem.h"
#include "postmaster/bgworker.h"
}

/* PostgreSQL module magic block */
extern "C" {
#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif
}

/* LWLock tranche ID for vector buffer operations */
int pg_vexdb_lock_tranche_id;

/* Track if extension was preloaded */
static bool pg_vexdb_preloaded = false;

/* Shared memory request hook for LWLock registration */
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* Global shared state - in main shmem */
VecBufSharedState *vecbuf_shared_state = NULL;
MemoryContext vecbuf_shared_ctx = NULL;
LWLock *VectorBufferLock = NULL;

/* Calculate shared memory size needed */
static Size
vecbuf_shmem_size(void)
{
    Size size = 0;
    
    /* VecBufSharedState */
    size = add_size(size, sizeof(VecBufSharedState));
    
    /* SharedAllocSet header */
    size = add_size(size, MAXALIGN(sizeof(SharedAllocSetData)));
    
    /* VecBufferManager */
    size = add_size(size, sizeof(VecBufferManager));
    
    /* Vector buffer pages (NVecBuf * 1MB each) */
    Size nvecbuf = NVecBuf(g_instance.diskann_cxt.vector_buffers);
    Size buffer_size = mul_size(nvecbuf, vec_block_size);
    size = add_size(size, buffer_size);

    /*
     * Metadata headroom derived from vector_buffers.
     *
     * Worst-case pool (dim 16, pool_offset=1) has the highest slots per block.
     * Reserve for:
     * 1) VecBufferTag array per slot
     * 2) concurrent_flat_map node/control overhead per slot
     * 3) per-pool block arrays
     */
    Size slots_per_block = vec_block_size / (vector_step_size * sizeof(float));
    Size total_slots = mul_size(nvecbuf, slots_per_block);
    Size tag_overhead = mul_size(total_slots, sizeof(VecBufferTag));
    Size map_overhead = mul_size(total_slots, sizeof(BufferSignature) + sizeof(VecBufferLoc) + 16);
    Size pool_blocks_overhead = mul_size((Size)NVecPool, mul_size(nvecbuf, sizeof(uint32)));
    Size metadata_overhead = add_size(add_size(tag_overhead, map_overhead), pool_blocks_overhead);
    size = add_size(size, metadata_overhead);
    
    /* Alignment padding */
    size = add_size(size, ann_helper::vector_aligned_size);
    
    /* Hashtable overhead is negligible per user direction */
    
    return size;
}

static void
pg_vexdb_shmem_request(void)
{
    if (prev_shmem_request_hook)
        prev_shmem_request_hook();
    
    RequestAddinShmemSpace(vecbuf_shmem_size());
    RequestNamedLWLockTranche("vector_buffer", 1);
}

static void
pg_vexdb_shmem_startup(void)
{
    bool found;
    
    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();
    
    pg_vexdb_preloaded = true;
    
    /* Create or attach to shared state */
    vecbuf_shared_state = (VecBufSharedState *)ShmemInitStruct(
        "VecBufSharedState", sizeof(VecBufSharedState), &found);
    
    if (!found) {
        /* First time initialization */
        vecbuf_shared_state->vector_buffers = g_instance.diskann_cxt.vector_buffers;
        vecbuf_shared_state->enable_buffer_manager = g_instance.diskann_cxt.enable_buffer_manager;
        vecbuf_shared_state->vector_buffer_workers = g_instance.diskann_cxt.vector_buffer_workers;
        vecbuf_shared_state->worker_latch = NULL;
        pg_atomic_init_u32(&vecbuf_shared_state->worker_count, 0);
        pg_atomic_init_u32(&vecbuf_shared_state->pool_offset_to_write, 0xFFFFFFFF);
        
        /* Create the shared memory context */
        vecbuf_shared_ctx = SharedAllocSetCreate("VectorBufferContext",
                                                  8 * 1024,      /* 8KB initial block */
                                                  1024 * 1024);  /* 1MB max block */
        vecbuf_shared_state->vecbuf_ctx = vecbuf_shared_ctx;
        
        /* Initialize VecBufferManager in shared memory */
        init_vector_smgr();
    } else {
        /* Attaching to existing shared memory */
        vecbuf_shared_ctx = vecbuf_shared_state->vecbuf_ctx;
    }
}

static void
register_vecbuf_workers(void)
{
    int nworkers = pg_vexdb_get_vector_buffer_workers();
    
    if (nworkers <= 0)
        return;
    
    for (int i = 0; i < nworkers; i++) {
        BackgroundWorker worker = {0};
        
        worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
        worker.bgw_start_time = BgWorkerStart_ConsistentState;
        worker.bgw_restart_time = BGW_DEFAULT_RESTART_INTERVAL;
        strcpy(worker.bgw_library_name, "pg_vexdb");
        strcpy(worker.bgw_function_name, "vecbuf_worker_main");
        snprintf(worker.bgw_name, BGW_MAXLEN, "vector buffer worker %d", i);
        strcpy(worker.bgw_type, "vector buffer worker");
        worker.bgw_main_arg = Int32GetDatum(i);
        
        RegisterBackgroundWorker(&worker);
    }
}

void* mem_align_alloc(size_t alignment, size_t size)
{
    return palloc_aligned(size, alignment, 0);
}

bool pg_vexdb_is_preloaded(void) { return pg_vexdb_preloaded; }

extern "C" {
void _PG_init(void);
}

void _PG_init(void)
{
    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook = pg_vexdb_shmem_request;
    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = pg_vexdb_shmem_startup;

    pg_vexdb_init_guc();

    /* Initialize distance functions */
    g_instance.annvec_cxt.l2_squared_distance = ann_helper::get_general_distance_func(Metric::L2);
    g_instance.annvec_cxt.negative_inner_product = ann_helper::get_general_distance_func(Metric::INNER_PRODUCT);
    g_instance.annvec_cxt.cosine_distance = ann_helper::get_general_distance_func(Metric::COSINE);

    g_instance.annvec_cxt.half_l2_squared_distance = ann_helper::get_general_half_distance_func(Metric::L2);
    g_instance.annvec_cxt.half_negative_inner_product = ann_helper::get_general_half_distance_func(Metric::INNER_PRODUCT);
    g_instance.annvec_cxt.half_cosine_distance = ann_helper::get_general_half_distance_func(Metric::COSINE);

    g_instance.annvec_cxt.int8_l2_squared_distance = ann_helper::get_general_int8_distance_func(Metric::L2);
    g_instance.annvec_cxt.int8_negative_inner_product = ann_helper::get_general_int8_distance_func(Metric::INNER_PRODUCT);
    g_instance.annvec_cxt.int8_cosine_distance = ann_helper::get_general_int8_distance_func(Metric::COSINE);

    g_instance.annvec_cxt.float_to_half = ann_helper::get_float_to_half_func();
    g_instance.annvec_cxt.half_to_float = ann_helper::get_half_to_float_func();

    g_instance.annvec_cxt.f_flip_sign = nullptr;
    g_instance.annvec_cxt.f_kacs_walk = nullptr;
    g_instance.annvec_cxt.f_warmup_ip_x0_q = nullptr;
    g_instance.annvec_cxt.f_ip_fxi = nullptr;
    g_instance.annvec_cxt.f_mask_ip_x0_q = nullptr;

    g_instance.annvec_cxt.ann_cxt = nullptr;
    g_instance.annvec_cxt.redistrib_elem_tracker = nullptr;
    g_instance.annvec_cxt.qt_update_cxt = nullptr;
    g_instance.annvec_cxt.qt_update_mgr = nullptr;

    g_instance.diskann_cxt.vector_buffers = pg_vexdb_get_vector_buffers();
    g_instance.diskann_cxt.enable_buffer_manager = pg_vexdb_get_enable_vec_buffer_manager();
    g_instance.diskann_cxt.vector_buffer_workers = pg_vexdb_get_vector_buffer_workers();
    
    /* Register background worker if enabled */
    if (pg_vexdb_get_enable_vec_buffer_manager() &&
        process_shared_preload_libraries_in_progress) {
        register_vecbuf_workers();
    }
}
