/*
 * guc_config.cpp - GUC parameter definitions for pg_vexdb
 */

#include "pg_compat.h"
#include "guc_config.h"
#include "distance/pg/distance_guc.h"

/* GUC variables */
static int pg_vexdb_ef_search = 64;
static bool pg_vexdb_enable_vec_buffer_manager = true;
static int pg_vexdb_vector_buffers = 2097152;  /* 2GB in KB */
static int pg_vexdb_vector_buffer_workers = 1;
static char *pg_vexdb_vec_architecture = NULL;

/* duck-side parity GUCs. Tests translated from .test files SET
 * vex_ef_search / vex_brute_force_threshold / vex_pq_search_mode.
 * vex_ef_search aliases pg_vexdb.ef_search; the other two are accepted
 * but currently no-ops on PG (search-side wiring is duck-only). */
static int pg_vexdb_vex_ef_search = 64;
static int pg_vexdb_vex_brute_force_threshold = 64;
static int pg_vexdb_vex_pq_search_mode = 0;
static const struct config_enum_entry pq_search_mode_options[] = {
    {"default", 0, false},
    {"pq_only", 1, false},
    {"refine", 2, false},
    {NULL, 0, false}
};

// duck-side debug GUC; PG-side accepts SET but does not act on it.
static char *pg_vexdb_disabled_optimizers = NULL;

/* Reloption kind - initialized at startup */
relopt_kind pg_vexdb_relopt_kind;

/* Session struct pointer for assign hooks */
extern PgVexdbSessionAttrs pg_vexdb_session;

extern "C" {

/* Accessor functions */
int pg_vexdb_get_ef_search(void) { return pg_vexdb_ef_search; }
bool pg_vexdb_get_enable_vec_buffer_manager(void) { return pg_vexdb_enable_vec_buffer_manager; }
int pg_vexdb_get_vector_buffers(void) { return pg_vexdb_vector_buffers; }
int pg_vexdb_get_vector_buffer_workers(void) { return pg_vexdb_vector_buffer_workers; }

/* Assign hook for ef_search - syncs GUC to session struct */
static void assign_ef_search(int newval, void *extra)
{
    (void)extra;
    pg_vexdb_session.attr_storage.ef_search = newval;
}

/* vex_ef_search aliases pg_vexdb.ef_search */
static void assign_vex_ef_search(int newval, void *extra)
{
    (void)extra;
    pg_vexdb_ef_search = newval;
    pg_vexdb_session.attr_storage.ef_search = newval;
}

/*
 * Initialize GUC parameters and reloptions
 */
void
pg_vexdb_init_guc(void)
{
    /* Register custom reloption kind */
    pg_vexdb_relopt_kind = add_reloption_kind();

    /* Register index reloptions */
    add_int_reloption(RELOPT_KIND_GRAPH_INDEX, "m",
                      "Number of neighbors for each node in the HNSW graph.",
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
     * GraphIndex::Create (vexdb-duck/index/graph_index.cpp). */
    add_string_reloption(RELOPT_KIND_GRAPH_INDEX, "metric",
                         "Distance metric: 'l2', 'cosine', or 'ip' (default 'l2'). "
                         "Note: PG normally derives the metric from the operator class; "
                         "this reloption is accepted for duck-side parity.",
                         NULL, NULL, AccessExclusiveLock);
    add_int_reloption(RELOPT_KIND_GRAPH_INDEX, "pq_m",
                      "Number of PQ subspaces. 0 disables PQ; otherwise must divide dimension.",
                      0, 0, INT_MAX, AccessExclusiveLock);
    add_string_reloption(RELOPT_KIND_GRAPH_INDEX, "memory_mode",
                         "'full' keeps raw vectors; 'compact' releases raw vectors after PQ "
                         "training (auto-selects pq_m if 0).",
                         NULL, NULL, AccessExclusiveLock);
    add_int_reloption(RELOPT_KIND_GRAPH_INDEX, "threads",
                      "Build-time worker count (duck-side name; alias of parallel_workers).",
                      1, 1, 1024, AccessExclusiveLock);

    /* GUC parameters */
    DefineCustomIntVariable("pg_vexdb.ef_search",
                            "Search list size for HNSW index search.",
                            "Controls the size of the dynamic candidate list during search. "
                            "Higher values improve recall at the cost of speed.",
                            &pg_vexdb_ef_search,
                            64,
                            1, 65535,
                            PGC_USERSET,
                            GUC_NOT_IN_SAMPLE,
                            NULL, assign_ef_search, NULL);

    DefineCustomBoolVariable("pg_vexdb.enable_vec_buffer_manager",
                              "Enable the vector buffer manager.",
                              "When enabled, uses a shared buffer pool for vector data.",
                              &pg_vexdb_enable_vec_buffer_manager,
                              true,
                              PGC_POSTMASTER,
                              GUC_NOT_IN_SAMPLE,
                              NULL, NULL, NULL);

    DefineCustomIntVariable("pg_vexdb.vector_buffers",
                            "Memory size for vector buffers in KB.",
                            "Total memory for vector buffer manager. Each block is 1MB.",
                            &pg_vexdb_vector_buffers,
                            2097152,  /* 2GB in KB */
                            64 * 1024, INT_MAX / 2,
                            PGC_POSTMASTER,
                            GUC_UNIT_KB,
                            NULL, NULL, NULL);

    DefineCustomIntVariable("pg_vexdb.vector_buffer_workers",
                            "Number of background workers for vector buffer management.",
                            "Workers handle buffer expansion and eviction. Set to 0 to disable.",
                            &pg_vexdb_vector_buffer_workers,
                            1, 0, 16,
                            PGC_POSTMASTER,
                            GUC_NOT_IN_SAMPLE,
                            NULL, NULL, NULL);

    DefineCustomStringVariable("pg_vexdb.vec_architecture",
                               "SIMD architecture selection for distance functions.",
                               "Format: 'usage:arch[, usage:arch, ...]'. "
                               "Usage: all, float, half, int8, l2, ip, cos, or combinations like float_l2. "
                               "Arch: scalar, sse, avx, avx512, etc. Empty string means auto-detect.",
                               &pg_vexdb_vec_architecture,
                               "",
                               PGC_SUSET,
                               GUC_NOT_IN_SAMPLE,
                               check_vec_arch_str, assign_vec_arch, NULL);

    /* duck-side parity GUCs */
    DefineCustomIntVariable("vex_ef_search",
                            "Alias for pg_vexdb.ef_search (duck-side parity).",
                            NULL,
                            &pg_vexdb_vex_ef_search,
                            64, 1, 65535,
                            PGC_USERSET, GUC_NOT_IN_SAMPLE,
                            NULL, assign_vex_ef_search, NULL);

    DefineCustomIntVariable("vex_brute_force_threshold",
                            "Below this node count the search may fall back to brute force "
                            "(duck-side; PG search wiring is a TODO and currently ignores this).",
                            NULL,
                            &pg_vexdb_vex_brute_force_threshold,
                            64, 0, INT_MAX,
                            PGC_USERSET, GUC_NOT_IN_SAMPLE,
                            NULL, NULL, NULL);

    DefineCustomStringVariable("disabled_optimizers",
                               "duck-side debug GUC; accepted but ignored on PG.",
                               NULL,
                               &pg_vexdb_disabled_optimizers,
                               "",
                               PGC_USERSET, GUC_NOT_IN_SAMPLE,
                               NULL, NULL, NULL);

    DefineCustomEnumVariable("vex_pq_search_mode",
                             "PQ search mode: 'default', 'pq_only', 'refine' "
                             "(duck-side; PG search path currently ignores this).",
                             NULL,
                             &pg_vexdb_vex_pq_search_mode,
                             0, pq_search_mode_options,
                             PGC_USERSET, GUC_NOT_IN_SAMPLE,
                             NULL, NULL, NULL);

    /* Initialize session struct from GUC defaults */
    pg_vexdb_session.attr_storage.ef_search = pg_vexdb_ef_search;
}

} /* extern "C" */
