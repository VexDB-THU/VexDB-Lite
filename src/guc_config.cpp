/*
 * guc_config.cpp - GUC parameter definitions for pg_vexdb
 */

#include "pg_compat.h"
#include "guc_config.h"
#include "distance/distance_guc.h"

/* GUC variables */
static int pg_vexdb_ef_search = 64;
static bool pg_vexdb_enable_vec_buffer_manager = true;
static int pg_vexdb_vector_buffers = 2097152;  /* 2GB in KB */
static int pg_vexdb_vector_buffer_workers = 1;
static char *pg_vexdb_vec_architecture = NULL;

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

    /* Initialize session struct from GUC defaults */
    pg_vexdb_session.attr_storage.ef_search = pg_vexdb_ef_search;
}

} /* extern "C" */
