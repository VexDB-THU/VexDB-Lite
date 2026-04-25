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
    uint32 search_list_size = uint32(pg_vexdb_get_ef_search());
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
            Oid halfvector_oid = get_halfvector_oid();
            Oid int8vector_oid = get_int8vector_oid();
            uint32 elem_size = var->vartype == floatvector_oid ? sizeof(float) :
                               var->vartype == halfvector_oid ? sizeof(half) :
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

bytea *graph_index_options_internal(Datum reloptions, bool validate)
{
    static const relopt_parse_elt tab[] = {
        {"m", RELOPT_TYPE_INT, offsetof(GraphIndexOptions, m)},
        {"ef_construction", RELOPT_TYPE_INT, offsetof(GraphIndexOptions, ef_construction)},
        {"parallel_workers", RELOPT_TYPE_INT, offsetof(GraphIndexOptions, parallel_workers)},
        {"quantizer", RELOPT_TYPE_STRING, offsetof(GraphIndexOptions, qt_type_offset)},
        {"cluster_rate", RELOPT_TYPE_INT, offsetof(GraphIndexOptions, cluster_rate)},
        {"enable_async_insert", RELOPT_TYPE_BOOL, offsetof(GraphIndexOptions, enable_async_insert)}
    };
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
    size_t ef = pg_vexdb_get_ef_search();
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
