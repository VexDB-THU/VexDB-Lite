#include "pg_compat.h"

#include <cstdlib>

#include "distance/core/distance.h"
#include "global_instance.h"
#include "graph_index/graph_index_state.h"
#include "graph_index/parallel_build_locks.h"
#include "guc_config.h"

extern "C" {
#include "storage/shmem.h"
#include "utils/hsearch.h"
}

extern "C" {
#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif
}

/* Kept for the openGauss compatibility macro in pg_compat.h. */
int vexdb_lite_lock_tranche_id;

LWLockPadded *VexGraphBuildEntryLocks = NULL;
LWLockPadded *VexGraphBuildEntryWaitLocks = NULL;
LWLockPadded *VexGraphBuildStorageLocks = NULL;
LWLockPadded *VexGraphBuildExtensionLocks = NULL;
LWLockPadded *VexGraphBuildPointLocks = NULL;

static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

static Size
vexdb_lite_shmem_size(void)
{
    return hash_estimate_size(128, sizeof(GIStateEntry));
}

void
init_graph_build_locks(void)
{
    VexGraphBuildEntryLocks = GetNamedLWLockTranche("graph_build_entry");
    VexGraphBuildEntryWaitLocks =
        GetNamedLWLockTranche("graph_build_entry_wait");
    VexGraphBuildStorageLocks =
        GetNamedLWLockTranche("graph_build_storage");
    VexGraphBuildExtensionLocks =
        GetNamedLWLockTranche("graph_build_extension");
    VexGraphBuildPointLocks =
        GetNamedLWLockTranche("graph_build_point");
}

static void
vexdb_lite_shmem_request(void)
{
    if (prev_shmem_request_hook) {
        prev_shmem_request_hook();
    }

    RequestAddinShmemSpace(vexdb_lite_shmem_size());
    RequestNamedLWLockTranche("graph_index_state", 1);
    RequestNamedLWLockTranche("graph_build_entry",
                              VEX_GRAPH_BUILD_ENTRY_LOCK_STRIPES);
    RequestNamedLWLockTranche("graph_build_entry_wait",
                              VEX_GRAPH_BUILD_ENTRY_WAIT_LOCK_STRIPES);
    RequestNamedLWLockTranche("graph_build_storage",
                              VEX_GRAPH_BUILD_STORAGE_LOCK_STRIPES);
    RequestNamedLWLockTranche("graph_build_extension",
                              VEX_GRAPH_BUILD_EXTENSION_LOCK_STRIPES);
    RequestNamedLWLockTranche("graph_build_point",
                              VEX_GRAPH_BUILD_POINT_LOCK_STRIPES);
}

static void
vexdb_lite_shmem_startup(void)
{
    if (prev_shmem_startup_hook) {
        prev_shmem_startup_hook();
    }

    init_graph_build_locks();
    graph_index_state_init();
}

void *
mem_align_alloc(size_t alignment, size_t size)
{
    return palloc_aligned(size, alignment, 0);
}

void
mem_align_free(void *ptr)
{
    pfree(ptr);
}

extern "C" {
void _PG_init(void);
}

void
_PG_init(void)
{
    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook = vexdb_lite_shmem_request;
    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = vexdb_lite_shmem_startup;

    vexdb_lite_init_guc();

    g_instance.annvec_cxt.l2_squared_distance =
        ann_helper::get_general_distance_func(Metric::L2);
    g_instance.annvec_cxt.negative_inner_product =
        ann_helper::get_general_distance_func(Metric::INNER_PRODUCT);
    g_instance.annvec_cxt.cosine_distance =
        ann_helper::get_general_distance_func(Metric::COSINE);

    g_instance.annvec_cxt.half_l2_squared_distance = nullptr;
    g_instance.annvec_cxt.half_negative_inner_product = nullptr;
    g_instance.annvec_cxt.half_cosine_distance = nullptr;

    g_instance.annvec_cxt.int8_l2_squared_distance =
        ann_helper::get_general_int8_distance_func(Metric::L2);
    g_instance.annvec_cxt.int8_negative_inner_product =
        ann_helper::get_general_int8_distance_func(Metric::INNER_PRODUCT);
    g_instance.annvec_cxt.int8_cosine_distance =
        ann_helper::get_general_int8_distance_func(Metric::COSINE);

    g_instance.annvec_cxt.float_to_half = nullptr;
    g_instance.annvec_cxt.half_to_float = nullptr;

    ann_helper::init_rabitq_func();

    g_instance.annvec_cxt.ann_cxt = nullptr;
    g_instance.annvec_cxt.redistrib_elem_tracker = nullptr;
}
