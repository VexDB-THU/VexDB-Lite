/*
 * guc_config.cpp - GUC parameter definitions for vexdb_lite
 */

#include "pg_compat.h"
#include "guc_config.h"
#include "distance/distance_guc.h"

/* GUC variables */
static int vexdb_lite_ef_search = 64;
static bool vexdb_lite_enable_vec_buffer_manager = true;
static int vexdb_lite_vector_buffers = 2097152;  /* 2GB in KB */
static int vexdb_lite_vector_buffer_workers = 1;
static char *vexdb_lite_vec_architecture = NULL;


/* Reloption kind - initialized at startup */
relopt_kind vexdb_lite_relopt_kind;

/* Session struct pointer for assign hooks */
extern PgVexdbSessionAttrs vexdb_lite_session;

extern "C" {

/* Accessor functions */
int vexdb_lite_get_ef_search(void) { return vexdb_lite_ef_search; }
bool vexdb_lite_get_enable_vec_buffer_manager(void) { return vexdb_lite_enable_vec_buffer_manager; }
int vexdb_lite_get_vector_buffers(void) { return vexdb_lite_vector_buffers; }
int vexdb_lite_get_vector_buffer_workers(void) { return vexdb_lite_vector_buffer_workers; }

/* Assign hook for ef_search - syncs GUC to session struct */
static void assign_ef_search(int newval, void *extra)
{
    (void)extra;
    vexdb_lite_session.attr_storage.ef_search = newval;
}

/*
 * Initialize GUC parameters and reloptions
 */
void
vexdb_lite_init_guc(void)
{
    /* Register custom reloption kind */
    vexdb_lite_relopt_kind = add_reloption_kind();

    /* Register index reloptions */
    add_int_reloption(RELOPT_KIND_GRAPH_INDEX, "m",
                      "Number of neighbors for each node in the graph index.",
                      16, 2, 100, AccessExclusiveLock);
    add_int_reloption(RELOPT_KIND_GRAPH_INDEX, "ef_construction",
                      "Size of the dynamic candidate list for graph construction.",
                      64, 4, 1000, AccessExclusiveLock);
    add_int_reloption(RELOPT_KIND_GRAPH_INDEX, "parallel_workers",
                      "Number of parallel workers for index build.",
                      0, 0, INT_MAX, AccessExclusiveLock);
    add_string_reloption(RELOPT_KIND_GRAPH_INDEX, "quantizer",
                         "Quantizer type (none, pq, rabitq).",
                         NULL, NULL, AccessExclusiveLock);
    add_int_reloption(RELOPT_KIND_GRAPH_INDEX, "cluster_rate",
                      "Cluster rate for quantization.",
                      0, 0, INT_MAX, AccessExclusiveLock);
    add_bool_reloption(RELOPT_KIND_GRAPH_INDEX, "enable_async_insert",
                       "Enable asynchronous insert for index.",
                       false, AccessExclusiveLock);

    /* Stage 4: duck-side parity reloptions. Accepted by amoptions; runtime
     * usage may be partial — the goal here is that
     *   CREATE INDEX ... WITH (m=, ef_construction=, metric=, quantizer=,
     *                          pq_m=, memory_mode=, threads=)
     * does not raise "unrecognized parameter". Semantics mirror duck-side
     * GraphIndex::Create (vexdb_duckdb/index/graph_index.cpp). */
    add_string_reloption(RELOPT_KIND_GRAPH_INDEX, "metric",
                         "Distance metric: 'l2', 'cosine', or 'ip' (default 'l2'). "
                         "Note: PG normally derives the metric from the operator class; "
                         "this reloption is accepted for duck-side parity.",
                         NULL, NULL, AccessExclusiveLock);
    add_int_reloption(RELOPT_KIND_GRAPH_INDEX, "pq_m",
                      "Number of PQ subspaces. 0 disables PQ; otherwise must divide dimension.",
                      0, 0, INT_MAX, AccessExclusiveLock);
    add_string_reloption(RELOPT_KIND_GRAPH_INDEX, "memory_mode",
                         "'compact' requires an active PQ or RaBitQ quantizer and stores only "
                         "quantizer codes in the index vector fork; omitted quantizer selects PQ.",
                         NULL, NULL, AccessExclusiveLock);
    add_int_reloption(RELOPT_KIND_GRAPH_INDEX, "threads",
                      "Build-time worker count (duck-side name; alias of parallel_workers).",
                      1, 1, 1024, AccessExclusiveLock);

    /* GUC parameters */
    DefineCustomIntVariable("vexdb.ef_search",
                            "Search list size for graph index search.",
                            "Controls the size of the dynamic candidate list during search. "
                            "Higher values improve recall at the cost of speed.",
                            &vexdb_lite_ef_search,
                            64,
                            1, 65535,
                            PGC_USERSET,
                            GUC_NOT_IN_SAMPLE,
                            NULL, assign_ef_search, NULL);

    DefineCustomBoolVariable("vexdb.enable_vec_buffer_manager",
                              "Enable the vector buffer manager.",
                              "When enabled, uses a shared buffer pool for vector data.",
                              &vexdb_lite_enable_vec_buffer_manager,
                              true,
                              PGC_POSTMASTER,
                              GUC_NOT_IN_SAMPLE,
                              NULL, NULL, NULL);

    DefineCustomIntVariable("vexdb.vector_buffers",
                            "Memory size for vector buffers in KB.",
                            "Total memory for vector buffer manager. Each block is 1MB.",
                            &vexdb_lite_vector_buffers,
                            2097152,  /* 2GB in KB */
                            64 * 1024, INT_MAX / 2,
                            PGC_POSTMASTER,
                            GUC_UNIT_KB,
                            NULL, NULL, NULL);

    DefineCustomIntVariable("vexdb.vector_buffer_workers",
                            "Number of background workers for vector buffer management.",
                            "Workers handle buffer expansion and eviction. Set to 0 to disable.",
                            &vexdb_lite_vector_buffer_workers,
                            1, 0, 16,
                            PGC_POSTMASTER,
                            GUC_NOT_IN_SAMPLE,
                            NULL, NULL, NULL);

    DefineCustomStringVariable("vexdb.vec_architecture",
                               "SIMD architecture selection for distance functions.",
                               "Format: 'usage:arch[, usage:arch, ...]'. "
                               "Usage: all, float, half, int8, l2, ip, cos, or combinations like float_l2. "
                               "Arch: scalar, sse, avx, avx512, etc. Empty string means auto-detect.",
                               &vexdb_lite_vec_architecture,
                               "",
                               PGC_SUSET,
                               GUC_NOT_IN_SAMPLE,
                               check_vec_arch_str, assign_vec_arch, NULL);

    /* Initialize session struct from GUC defaults */
    vexdb_lite_session.attr_storage.ef_search = vexdb_lite_ef_search;
}

} /* extern "C" */
