#include "pg_compat.h"

#include <cstdlib>
#include "global_instance.h"
#include "distance/core/distance.h"
#include "vector_buffer/vector_smgr.h"
#include "graph_index/parallel_build_locks.h"
#include "guc_config.h"
#include "graph_index/graph_index_state.h"
#include "vector_buffer/shared_alloc_set.h"
#include "vector_buffer/vecbuf_shared.h"

extern "C" {
#include "storage/shmem.h"
#include "postmaster/bgworker.h"
#include "utils/hsearch.h"
}

/* PostgreSQL module magic block */
extern "C" {
#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif
}

/* LWLock tranche ID for vector buffer operations */
int vexdb_lite_lock_tranche_id;

/* Track if extension was preloaded */
static bool vexdb_lite_preloaded = false;

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

    /* GIState shared hash table */
    size = add_size(size, hash_estimate_size(128, sizeof(GIStateEntry)));

    return size;
}

static void
vexdb_lite_shmem_request(void)
{
    if (prev_shmem_request_hook)
        prev_shmem_request_hook();
    
    RequestAddinShmemSpace(vecbuf_shmem_size());
    RequestNamedLWLockTranche("vector_buffer", 1);
    RequestNamedLWLockTranche("graph_index_state", 1);
    RequestNamedLWLockTranche("vector_file_write", VEX_VEC_WRITE_LOCK_STRIPES);
    RequestNamedLWLockTranche("graph_build_entry", VEX_GRAPH_BUILD_ENTRY_LOCK_STRIPES);
    RequestNamedLWLockTranche("graph_build_entry_wait", VEX_GRAPH_BUILD_ENTRY_WAIT_LOCK_STRIPES);
    RequestNamedLWLockTranche("graph_build_storage", VEX_GRAPH_BUILD_STORAGE_LOCK_STRIPES);
    RequestNamedLWLockTranche("graph_build_extension", VEX_GRAPH_BUILD_EXTENSION_LOCK_STRIPES);
    RequestNamedLWLockTranche("graph_build_point", VEX_GRAPH_BUILD_POINT_LOCK_STRIPES);
}

static void
vexdb_lite_shmem_startup(void)
{
    bool found;
    
    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();
    
    vexdb_lite_preloaded = true;
    
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

        /* Initialize per-index state hash table */
        graph_index_state_init();
    } else {
        /* Attaching to existing shared memory */
        vecbuf_shared_ctx = vecbuf_shared_state->vecbuf_ctx;
    }
}

static void
register_vecbuf_workers(void)
{
    int nworkers = vexdb_lite_get_vector_buffer_workers();
    
    if (nworkers <= 0)
        return;
    
    for (int i = 0; i < nworkers; i++) {
        BackgroundWorker worker = {0};
        
        worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
        worker.bgw_start_time = BgWorkerStart_ConsistentState;
        worker.bgw_restart_time = BGW_DEFAULT_RESTART_INTERVAL;
        strcpy(worker.bgw_library_name, "vexdb_lite");
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

void mem_align_free(void *ptr)
{
    pfree(ptr);
}

bool vexdb_lite_is_preloaded(void) { return vexdb_lite_preloaded; }

extern "C" {
void _PG_init(void);
}

void _PG_init(void)
{
    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook = vexdb_lite_shmem_request;
    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = vexdb_lite_shmem_startup;

    vexdb_lite_init_guc();

    /* Initialize distance functions */
    g_instance.annvec_cxt.l2_squared_distance = ann_helper::get_general_distance_func(Metric::L2);
    g_instance.annvec_cxt.negative_inner_product = ann_helper::get_general_distance_func(Metric::INNER_PRODUCT);
    g_instance.annvec_cxt.cosine_distance = ann_helper::get_general_distance_func(Metric::COSINE);

    /* halfvector C++ implementation removed; half_* distance slots are unused. */
    g_instance.annvec_cxt.half_l2_squared_distance = nullptr;
    g_instance.annvec_cxt.half_negative_inner_product = nullptr;
    g_instance.annvec_cxt.half_cosine_distance = nullptr;

    g_instance.annvec_cxt.int8_l2_squared_distance = ann_helper::get_general_int8_distance_func(Metric::L2);
    g_instance.annvec_cxt.int8_negative_inner_product = ann_helper::get_general_int8_distance_func(Metric::INNER_PRODUCT);
    g_instance.annvec_cxt.int8_cosine_distance = ann_helper::get_general_int8_distance_func(Metric::COSINE);

    g_instance.annvec_cxt.float_to_half = nullptr;
    g_instance.annvec_cxt.half_to_float = nullptr;

    ann_helper::init_rabitq_func();

    g_instance.annvec_cxt.ann_cxt = nullptr;
    g_instance.annvec_cxt.redistrib_elem_tracker = nullptr;

    g_instance.diskann_cxt.vector_buffers = vexdb_lite_get_vector_buffers();
    g_instance.diskann_cxt.enable_buffer_manager = vexdb_lite_get_enable_vec_buffer_manager();
    g_instance.diskann_cxt.vector_buffer_workers = vexdb_lite_get_vector_buffer_workers();
    
    /* Register background worker if enabled */
    if (vexdb_lite_get_enable_vec_buffer_manager() &&
        process_shared_preload_libraries_in_progress) {
        register_vecbuf_workers();
    }
}
