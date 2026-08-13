#include "pg_compat.h"
#include "graph_index/graph_index_param.h"
#include "graph_index/graph_index.h"
#include "graph_index/graph_index_state.h"
#include "guc_config.h"
#include "ann_utils.h"

extern "C" {

/* Forward declarations of internal functions with correct signatures for IndexAmRoutine */
static IndexBuildResult *graph_index_ambuild(Relation heap, Relation index, IndexInfo *indexInfo);
static void graph_index_ambuildempty(Relation index);
static bool graph_index_aminsert(Relation index, Datum *values, bool *isnull,
                                  ItemPointer ht_ctid, Relation heap,
                                  IndexUniqueCheck checkUnique,
                                  bool indexUnchanged, IndexInfo *indexInfo);
static IndexBulkDeleteResult *graph_index_ambulkdelete(IndexVacuumInfo *info,
                                                       IndexBulkDeleteResult *stats,
                                                       IndexBulkDeleteCallback callback,
                                                       void *callback_state);
static IndexBulkDeleteResult *graph_index_amvacuumcleanup(IndexVacuumInfo *info,
                                                          IndexBulkDeleteResult *stats);
static void graph_index_amcostestimate(PlannerInfo *root, IndexPath *path,
                                       double loop_count, Cost *indexStartupCost,
                                       Cost *indexTotalCost, Selectivity *indexSelectivity,
                                       double *indexCorrelation, double *indexPages);
static bytea *graph_index_amoptions(Datum reloptions, bool validate);
static bool graph_index_amvalidate(Oid opclassoid);
static IndexScanDesc graph_index_ambeginscan(Relation index, int nkeys, int norderbys);
static void graph_index_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys,
                                 ScanKey orderbys, int norderbys);
static bool graph_index_amgettuple(IndexScanDesc scan, ScanDirection direction);
static void graph_index_amendscan(IndexScanDesc scan);

static IndexAmRoutine *graph_index_amroutine(void);

PG_FUNCTION_INFO_V1(graph_index_amhandler);
Datum graph_index_amhandler(PG_FUNCTION_ARGS)
{
    PG_RETURN_POINTER(graph_index_amroutine());
}

static IndexAmRoutine *
graph_index_amroutine(void)
{
    IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

    /* 2 个 ORDER BY strategy: 1 = <#>(pgvector 兼容), 2 = <~>(跨引擎统一). 二者同为
     * 负内积; metric 实际来自索引元数据(FUNCTION 1), scan 不读 strategy, amvalidate 恒真.
     * 此值是 PG 建 opclass 时对 operator number 的上界校验, 必须 >= 用到的最大 strategy. */
    amroutine->amstrategies = 2;
    amroutine->amsupport = 2;
    amroutine->amoptsprocnum = 0;
    amroutine->amcanorder = false;
    amroutine->amcanorderbyop = true;
#if PG_VERSION_NUM >= 180000
    amroutine->amcanhash = false;
    amroutine->amconsistentequality = true;
    amroutine->amconsistentordering = true;
#endif
    amroutine->amcanbackward = false;
    amroutine->amcanunique = false;
    amroutine->amcanmulticol = false;
    amroutine->amoptionalkey = true;
    amroutine->amsearcharray = false;
    amroutine->amsearchnulls = false;
    amroutine->amstorage = false;
    amroutine->amclusterable = false;
    amroutine->ampredlocks = false;
    amroutine->amcanparallel = false;
#if PG_VERSION_NUM >= 170000
    amroutine->amcanbuildparallel = false;
#endif
    amroutine->amcaninclude = false;
    amroutine->amusemaintenanceworkmem = false;
    amroutine->amsummarizing = false;
    amroutine->amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL;
    amroutine->amkeytype = InvalidOid;

    amroutine->ambuild = graph_index_ambuild;
    amroutine->ambuildempty = graph_index_ambuildempty;
    amroutine->aminsert = graph_index_aminsert;
#if PG_VERSION_NUM >= 170000
    amroutine->aminsertcleanup = NULL;
#endif
    amroutine->ambulkdelete = graph_index_ambulkdelete;
    amroutine->amvacuumcleanup = graph_index_amvacuumcleanup;
    amroutine->amcanreturn = NULL;
    amroutine->amcostestimate = graph_index_amcostestimate;
#if PG_VERSION_NUM >= 180000
    amroutine->amgettreeheight = NULL;
#endif
    amroutine->amoptions = graph_index_amoptions;
    amroutine->amproperty = NULL;
    amroutine->ambuildphasename = NULL;
    amroutine->amvalidate = graph_index_amvalidate;
    amroutine->amadjustmembers = NULL;
    amroutine->ambeginscan = graph_index_ambeginscan;
    amroutine->amrescan = graph_index_amrescan;
    amroutine->amgettuple = graph_index_amgettuple;
    amroutine->amgetbitmap = NULL;
    amroutine->amendscan = graph_index_amendscan;
    amroutine->ammarkpos = NULL;
    amroutine->amrestrpos = NULL;
    amroutine->amestimateparallelscan = NULL;
    amroutine->aminitparallelscan = NULL;
    amroutine->amparallelrescan = NULL;
#if PG_VERSION_NUM >= 180000
    amroutine->amtranslatestrategy = NULL;
    amroutine->amtranslatecmptype = NULL;
#endif

    return amroutine;
}

/* Internal implementations */
static IndexBuildResult *
graph_index_ambuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
    check_ann_attributes(index);
    return graph_index_build_internal(heap, index, indexInfo);
}

static void
graph_index_ambuildempty(Relation index)
{
    graph_index_buildempty_internal(index);
}

static bool
graph_index_aminsert(Relation index, Datum *values, bool *isnull,
                     ItemPointer ht_ctid, Relation heap,
                     IndexUniqueCheck checkUnique,
                     bool indexUnchanged, IndexInfo *indexInfo)
{
    (void)checkUnique;
    (void)indexUnchanged;
    (void)indexInfo;
    return graph_index_insert_internal(index, heap, values, isnull, ht_ctid, GRAPH_INDEX_METAPAGE_BLKNO);
}

static IndexBulkDeleteResult *
graph_index_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
                         IndexBulkDeleteCallback callback, void *callback_state)
{
    int nparallel = graph_index_get_build_parallel(info->index);
    return graph_index_bulkdelete_internal(info->index, stats, nparallel, callback, callback_state,
                                           GRAPH_INDEX_METAPAGE_BLKNO);
}

static IndexBulkDeleteResult *
graph_index_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
    return graph_index_vacuumcleanup_internal(info, stats);
}

static void
graph_index_amcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
                           Cost *indexStartupCost, Cost *indexTotalCost,
                           Selectivity *indexSelectivity, double *indexCorrelation,
                           double *indexPages)
{
    graph_index_costestimate_internal(root, path, loop_count, indexStartupCost,
                                      indexTotalCost, indexSelectivity, indexCorrelation, indexPages);
}

static bytea *
graph_index_amoptions(Datum reloptions, bool validate)
{
    return graph_index_options_internal(reloptions, validate);
}

static bool
graph_index_amvalidate(Oid opclassoid)
{
    return graph_index_validate_internal(opclassoid);
}

static IndexScanDesc
graph_index_ambeginscan(Relation index, int nkeys, int norderbys)
{
    return graph_index_beginscan_internal(index, nkeys, norderbys);
}

static void
graph_index_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys,
                     ScanKey orderbys, int norderbys)
{
    graph_index_rescan_internal(scan, keys, nkeys, orderbys, norderbys);
}

static bool
graph_index_amgettuple(IndexScanDesc scan, ScanDirection direction)
{
    (void)direction;
    float dist_out;
    return graph_index_gettuple_internal(scan, scan->opaque, GRAPH_INDEX_METAPAGE_BLKNO,
                                         vexdb_lite_get_ef_search(), &dist_out);
}

static void
graph_index_amendscan(IndexScanDesc scan)
{
    graph_index_endscan_internal(scan);
}

} /* extern "C" */
