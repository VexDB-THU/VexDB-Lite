#include "pg_compat.h"

extern "C" {
#include "optimizer/optimizer.h"
#include "optimizer/cost.h"
}

#include "graph_index/graph_index.h"
#include "graph_index/graph_index_storage.h"
#include "graph_index/graph_index_algorithm.h"
#include "guc_config.h"
#include "ann_utils.h"

using namespace ann_helper;

struct LimitInfo {
    bool limit_set = false;
    bool with_limit = false;
    uint64 limit_offset = 0;
    uint64 limit_count = 0;
    void set_limit(bool l, int64 offset, int64 count) {
        if (limit_set) return;
        limit_set = true;
        if (l) {
            with_limit = true;
            limit_offset = uint64(offset);
            limit_count = uint64(count);
        }
    }
    bool has_limit() const { return limit_set && with_limit; }
    uint64 get_noffset() const { return limit_offset + limit_count; }
};

struct GraphIndexScanState {
    LimitInfo linfo;
};

Datum graph_index_build(PG_FUNCTION_ARGS)
{
    Relation heap = (Relation)PG_GETARG_POINTER(0);
    Relation index = (Relation)PG_GETARG_POINTER(1);
    IndexInfo *indexinfo = (IndexInfo *)PG_GETARG_POINTER(2);
    IndexBuildResult *result = graph_index_build_internal(heap, index, indexinfo);
    PG_RETURN_POINTER(result);
}

Datum graph_index_buildempty(PG_FUNCTION_ARGS)
{
    Relation index = (Relation)PG_GETARG_POINTER(0);
    graph_index_buildempty_internal(index);
    PG_RETURN_VOID();
}

Datum graph_index_insert(PG_FUNCTION_ARGS)
{
    Relation rel = (Relation)PG_GETARG_POINTER(0);
    Datum *values = (Datum *)PG_GETARG_POINTER(1);
    bool *isnull = (bool *)PG_GETARG_POINTER(2);
    ItemPointer ht_ctid = (ItemPointer)PG_GETARG_POINTER(3);
    Relation heaprel = (Relation)PG_GETARG_POINTER(4);
    bool result = graph_index_insert_internal(rel, heaprel, values, isnull, ht_ctid, GRAPH_INDEX_METAPAGE_BLKNO);
    PG_RETURN_BOOL(result);
}

Datum graph_index_bulkdelete(PG_FUNCTION_ARGS)
{
    IndexVacuumInfo *info = (IndexVacuumInfo *)PG_GETARG_POINTER(0);
    IndexBulkDeleteResult *volatile stats = (IndexBulkDeleteResult *)PG_GETARG_POINTER(1);
    IndexBulkDeleteCallback callback = (IndexBulkDeleteCallback)PG_GETARG_POINTER(2);
    void *callback_state = (void *)PG_GETARG_POINTER(3);
    int nparallel = graph_index_get_build_parallel(info->index);
    stats = graph_index_bulkdelete_internal(info->index, stats, nparallel, callback, callback_state,
        GRAPH_INDEX_METAPAGE_BLKNO);
    PG_RETURN_POINTER(stats);
}

Datum graph_index_vacuumcleanup(PG_FUNCTION_ARGS)
{
    IndexVacuumInfo *info = (IndexVacuumInfo *)PG_GETARG_POINTER(0);
    IndexBulkDeleteResult *stats = (IndexBulkDeleteResult *)PG_GETARG_POINTER(1);
    stats = graph_index_vacuumcleanup_internal(info, stats);
    PG_RETURN_POINTER(stats);
}

void graph_index_costestimate_internal(PlannerInfo *root, IndexPath *path, double loop_count,
    Cost *indexStartupCost, Cost *indexTotalCost, Selectivity *indexSelectivity,
    double *indexCorrelation, double *indexPages)
{
    int nlimit_temp = root->limit_tuples;
    bool has_limit = nlimit_temp > 0;
    if (!has_limit) {
        PlannerInfo *cur = root;
        while (cur && cur->parse && !has_limit) {
            has_limit = cur->parse->limitCount;
            nlimit_temp = cur->limit_tuples;
            cur = cur->parent_root;
        }
    }
    if (list_length(path->indexorderbys) <= 0 ||
        (!has_limit && enable_seqscan)) {
        *indexStartupCost = DBL_MAX;
        *indexTotalCost = DBL_MAX;
        *indexSelectivity = 0;
        *indexCorrelation = 0;
        return;
    }
    constexpr uint32 default_nlimit = 10;
    const uint32 nlimit = nlimit_temp <= 0 ? default_nlimit : nlimit_temp + 1;
    uint32 search_list_size = uint32(vexdb_lite_get_ef_search());
    if (search_list_size < nlimit) {
        search_list_size = nlimit;
    }
    Relation index = index_open(path->indexinfo->indexoid, NoLock);
    const int m = graph_index_get_m(index);
    Form_pg_attribute att = TupleDescAttr(index->rd_att, 0);
    bool toasted = att->attstorage == 'x' || att->attstorage == 'm';
    index_close(index, NoLock);

    float4 opr_cost = 4;
    uint32 vec_size = 768 * 4;
    Node *node = (Node *)lfirst(list_head(path->indexorderbys));
    if (IsA(node, OpExpr)) {
        OpExpr *op = (OpExpr *)node;
        if (list_length(op->args) != 2) {
            return;
        }
        Node *arg1 = (Node *)lfirst(list_head(op->args));
        Node *arg2 = (Node *)lfirst(list_tail(op->args));
        Var *var = NULL;
        if (IsA(arg1, Var)) {
            var = (Var *)arg1;
        } else if (IsA(arg2, Var)) {
            var = (Var *)arg2;
        }
        if (var) {
            Oid floatvector_oid = get_floatvector_oid();
            Oid int8vector_oid = get_int8vector_oid();
            uint32 elem_size = var->vartype == floatvector_oid ? sizeof(float) :
                               var->vartype == int8vector_oid ? sizeof(int8) : 4;
            vec_size = var->vartypmod > 0 ? var->vartypmod * elem_size : vec_size;
            toasted &= vec_size >= TOAST_INDEX_TARGET;
        }
    }
    double spc_seq_page_cost, spc_random_page_cost;
    get_tablespace_page_costs(path->indexinfo->reltablespace, &spc_random_page_cost,
                              &spc_seq_page_cost);
    constexpr Cost base_cost = 100;
    Cost startup_cost = base_cost;
    uint32 nnode, nvec;
loop:
    nnode = search_list_size * 1.1;
    nvec = m * 2 * search_list_size * 2;
    constexpr double scaling_factor = 0.525;
    constexpr double default_factor = 0.5;
    nvec *= path->indexinfo->tuples > 0 ?
        scaling_factor * log(path->indexinfo->tuples) / (log(m) * (1 + log(search_list_size))) :
        default_factor;
    double vec_read_cost = spc_seq_page_cost / 8;
    vec_read_cost += spc_random_page_cost / 5;
    startup_cost += (nnode + 2) * spc_random_page_cost;
    startup_cost += nvec * (cpu_operator_cost * opr_cost + vec_read_cost);
    Assert(path->path.rows > 0);
    if (search_list_size / nlimit / 1.12 < path->indexinfo->tuples / path->path.rows) {
        search_list_size *= 2;
        goto loop;
    }

    double ratio = path->indexinfo->tuples > 0 ? nvec / path->indexinfo->tuples : 1.0;
    if (ratio < 0.85) {
        constexpr double compression_rate = 1.28;
        double rel_min_page = (path->indexinfo->tuples * vec_size * compression_rate) / BLCKSZ;
        double vec_toast_pages = rel_min_page - path->indexinfo->pages;
        if (vec_toast_pages > 0) {
            double lz_cost = toasted ? cpu_operator_cost * 500 : 0.0;
            double reduce_cost = path->indexinfo->tuples == path->path.rows
                ? vec_toast_pages * (spc_seq_page_cost + lz_cost)
                : vec_toast_pages / path->indexinfo->tuples * path->path.rows *
                  (spc_random_page_cost + lz_cost);
            startup_cost -= std::min(reduce_cost, startup_cost * (1 - ratio));
            startup_cost = std::max(startup_cost, base_cost);
        }
    }
    startup_cost *= loop_count;

    *indexStartupCost = startup_cost;
    *indexTotalCost = startup_cost + search_list_size * cpu_index_tuple_cost;
    *indexSelectivity = 1.0;
    *indexCorrelation = 0.0;
    *indexPages = 0.0;
}

Datum graph_index_costestimate(PG_FUNCTION_ARGS)
{
    PlannerInfo *root = (PlannerInfo *)PG_GETARG_POINTER(0);
    IndexPath *path = (IndexPath *)PG_GETARG_POINTER(1);
    double loopcount = (double)PG_GETARG_FLOAT8(2);
    Cost *startupcost = (Cost *)PG_GETARG_POINTER(3);
    Cost *totalcost = (Cost *)PG_GETARG_POINTER(4);
    Selectivity *selectivity = (Selectivity *)PG_GETARG_POINTER(5);
    double *correlation = (double *)PG_GETARG_POINTER(6);
    double *indexpages = (double *)PG_GETARG_POINTER(7);
    graph_index_costestimate_internal(root, path, loopcount, startupcost, totalcost, selectivity, correlation, indexpages);

    PG_RETURN_VOID();
}

/* PG 19 在 add_reloption_kind 自定义 KIND 上跳过严格 INT 校验, "parallel_workers=1.1"
 * 等浮点字面量被 silent 接受 (原样写入 pg_class.reloptions)。在 build_reloptions
 * 之前先遍历原始 text[] Datum, 对所有 INT 字段做严格数字字符校验, 含 '.' / 'e' / 'E' 直接拒。
 */
static void
graph_index_strict_int_reloptions(Datum reloptions)
{
    if (DatumGetPointer(reloptions) == NULL)
        return;
    static const char *int_opts[] = {
        "m", "ef_construction", "parallel_workers",
        "cluster_rate", "pq_m", "threads"
    };
    ArrayType *arr = DatumGetArrayTypeP(reloptions);
    Datum *elems = NULL;
    int nelems = 0;
    deconstruct_array(arr, TEXTOID, -1, false, 'i', &elems, NULL, &nelems);
    for (int i = 0; i < nelems; i++) {
        char *kv = TextDatumGetCString(elems[i]);
        char *eq = strchr(kv, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = kv;
        const char *value = eq + 1;
        bool is_int_opt = false;
        for (size_t k = 0; k < lengthof(int_opts); k++) {
            if (pg_strcasecmp(key, int_opts[k]) == 0) {
                is_int_opt = true;
                break;
            }
        }
        if (!is_int_opt) continue;
        const char *p = value;
        if (*p == '+' || *p == '-') p++;
        if (!*p) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("Invalid integer value for option \"%s\": '%s'", key, value)));
        }
        for (; *p; p++) {
            if (*p < '0' || *p > '9') {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("Invalid integer value for option \"%s\": '%s' (must be a plain integer, no fraction or exponent)", key, value)));
            }
        }
    }
}

bytea *graph_index_options_internal(Datum reloptions, bool validate)
{
    static const relopt_parse_elt tab[] = {
        {"m", RELOPT_TYPE_INT, offsetof(GraphIndexOptions, m)},
        {"ef_construction", RELOPT_TYPE_INT, offsetof(GraphIndexOptions, ef_construction)},
        {"parallel_workers", RELOPT_TYPE_INT, offsetof(GraphIndexOptions, parallel_workers)},
        {"quantizer", RELOPT_TYPE_STRING, offsetof(GraphIndexOptions, qt_type_offset)},
        {"cluster_rate", RELOPT_TYPE_INT, offsetof(GraphIndexOptions, cluster_rate)},
        {"enable_async_insert", RELOPT_TYPE_BOOL, offsetof(GraphIndexOptions, enable_async_insert)},
        /* Stage 4 duck-parity options */
        {"metric", RELOPT_TYPE_STRING, offsetof(GraphIndexOptions, metric_offset)},
        {"pq_m", RELOPT_TYPE_INT, offsetof(GraphIndexOptions, pq_m)},
        {"memory_mode", RELOPT_TYPE_STRING, offsetof(GraphIndexOptions, memory_mode_offset)},
        {"threads", RELOPT_TYPE_INT, offsetof(GraphIndexOptions, threads)}
    };
    if (validate)
        graph_index_strict_int_reloptions(reloptions);
    GraphIndexOptions *rdopts = (GraphIndexOptions *)build_reloptions(reloptions, validate,
        RELOPT_KIND_GRAPH_INDEX, sizeof(GraphIndexOptions), tab, lengthof(tab));
    if (rdopts && validate) {
        constexpr int min_ncluster = 3;
        if (rdopts->cluster_rate > 0 && rdopts->cluster_rate < min_ncluster) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("Invalid parameter value for \"cluster_rate\": %d, "
                        "the value should be either 0 or not less than %d",
                        rdopts->cluster_rate, min_ncluster)));
        }
        /* Reject malformed quantizer values instead of silently falling back to
         * NONE. Numeric literals (quantizer=1) reach here as the string "1"
         * via STRING reloption coercion and are rejected. */
        if (rdopts->qt_type_offset > 0) {
            const char *s = (const char *)rdopts + rdopts->qt_type_offset;
            if (pg_strcasecmp(s, "none") != 0 && pg_strcasecmp(s, "pq") != 0 &&
                pg_strcasecmp(s, "rabitq") != 0) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("Invalid \"quantizer\" value '%s' (expected 'none', 'pq', or 'rabitq')", s)));
            }
        }
        /* Validate Stage 4 options to match duck-side semantics. */
        if (rdopts->metric_offset > 0) {
            const char *s = (const char *)rdopts + rdopts->metric_offset;
            if (pg_strcasecmp(s, "l2") != 0 && pg_strcasecmp(s, "cosine") != 0 &&
                pg_strcasecmp(s, "ip") != 0) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("Invalid \"metric\" value '%s' (expected 'l2', 'cosine', or 'ip')", s)));
            }
        }
        if (rdopts->pq_m < 0) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("Invalid \"pq_m\" value: %d (must be >= 0)", rdopts->pq_m)));
        }
        if (rdopts->memory_mode_offset > 0) {
            const char *s = (const char *)rdopts + rdopts->memory_mode_offset;
            if (pg_strcasecmp(s, "full") != 0 && pg_strcasecmp(s, "compact") != 0) {
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("Invalid \"memory_mode\" value '%s' (expected 'full' or 'compact')", s)));
            }
        }
        const bool compact_mode = rdopts->memory_mode_offset > 0 &&
            pg_strcasecmp((const char *)rdopts + rdopts->memory_mode_offset, "compact") == 0;
        const char *quantizer = rdopts->qt_type_offset > 0 ?
            (const char *)rdopts + rdopts->qt_type_offset : NULL;

        if (compact_mode && quantizer != NULL && pg_strcasecmp(quantizer, "none") == 0) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("memory_mode='compact' requires quantizer='pq' or quantizer='rabitq'")));
        }
        if (quantizer != NULL && pg_strcasecmp(quantizer, "rabitq") == 0 &&
            rdopts->pq_m > 0) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("pq_m is not valid with quantizer='rabitq'")));
        }
        /* `threads` reloption is accepted for duck parity. PG build is currently
         * driven by `parallel_workers`; if the user only set `threads`, mirror
         * it into parallel_workers so build_with_workers picks it up. */
        if (rdopts->threads > 1 && rdopts->parallel_workers == 0) {
            rdopts->parallel_workers = rdopts->threads;
        }
    }
    return (bytea *)rdopts;
}

Datum graph_index_options(PG_FUNCTION_ARGS)
{
    Datum reloptions = PG_GETARG_DATUM(0);
    bool validate = PG_GETARG_BOOL(1);
    bytea *result = graph_index_options_internal(reloptions, validate);
    if (result != NULL) {
        PG_RETURN_BYTEA_P(result);
    }
    PG_RETURN_NULL();
}

Datum graph_index_validate(PG_FUNCTION_ARGS)
{
    Oid opclassoid = PG_GETARG_OID(0);
    PG_RETURN_BOOL(graph_index_validate_internal(opclassoid));
}

bool graph_index_validate_internal(Oid opclassoid)
{
    (void)opclassoid;
    return true;
}

Datum graph_index_beginscan(PG_FUNCTION_ARGS)
{
    Relation rel = (Relation)PG_GETARG_POINTER(0);
    int nkeys = PG_GETARG_INT32(1);
    int norderbys = PG_GETARG_INT32(2);
    IndexScanDesc scan = graph_index_beginscan_internal(rel, nkeys, norderbys);
    PG_RETURN_POINTER(scan);
}

Datum graph_index_rescan(PG_FUNCTION_ARGS)
{
    IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);
    ScanKey scankey = (ScanKey)PG_GETARG_POINTER(1);
    int nkeys = PG_GETARG_INT32(2);
    ScanKey orderbys = (ScanKey)PG_GETARG_POINTER(3);
    int norderbys = PG_GETARG_INT32(4);
    graph_index_rescan_internal(scan, scankey, nkeys, orderbys, norderbys);
    PG_RETURN_VOID();
}

Datum graph_index_gettuple(PG_FUNCTION_ARGS)
{
    IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);
    if (!scan) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid arguments for function graph_indexgettuple")));
    }
    GraphIndexScanState *state = (GraphIndexScanState *)scan->opaque;
    size_t ef = vexdb_lite_get_ef_search();
    if (state && state->linfo.has_limit() && ef < state->linfo.get_noffset()) {
        ef = state->linfo.get_noffset();
    }
    bool result = graph_index_gettuple_internal(scan, scan->opaque, GRAPH_INDEX_METAPAGE_BLKNO, ef, NULL);
    PG_RETURN_BOOL(result);
}

Datum graph_index_endscan(PG_FUNCTION_ARGS)
{
    IndexScanDesc scan = (IndexScanDesc)PG_GETARG_POINTER(0);
    graph_index_endscan_internal(scan);
    PG_RETURN_VOID();
}
