/**
 * Graph Index AM
 */
#ifndef GRAPH_INDEX_H
#define GRAPH_INDEX_H

#include "pg_compat.h"
#include "utils/relcache.h"

#include "graph_index/graph_index_struct.h"

/* Graph index functions */
extern Datum graph_index_build(PG_FUNCTION_ARGS);
extern Datum graph_index_buildempty(PG_FUNCTION_ARGS);
extern Datum graph_index_insert(PG_FUNCTION_ARGS);
extern Datum graph_index_bulkdelete(PG_FUNCTION_ARGS);
extern Datum graph_index_vacuumcleanup(PG_FUNCTION_ARGS);
extern Datum graph_index_costestimate(PG_FUNCTION_ARGS);
extern Datum graph_index_options(PG_FUNCTION_ARGS);
extern Datum graph_index_validate(PG_FUNCTION_ARGS);
extern Datum graph_index_beginscan(PG_FUNCTION_ARGS);
extern Datum graph_index_rescan(PG_FUNCTION_ARGS);
extern Datum graph_index_gettuple(PG_FUNCTION_ARGS);
extern Datum graph_index_endscan(PG_FUNCTION_ARGS);

/* build */
void graph_index_buildempty_internal(Relation index);
IndexBuildResult *graph_index_build_internal(Relation heap, Relation index, IndexInfo *indexInfo);
BlockNumber build_graph_index(Relation heap, Relation index, IndexInfo *index_info, ForkNumber fork_num,
    double *reltuples, double *indtuples);

/* insert */
bool graph_index_insert_internal(Relation index, Relation heap, Datum *values, const bool *isnull,
    ItemPointer heap_tid, BlockNumber metablkno);

/* scan */
IndexScanDesc graph_index_beginscan_internal(Relation index, int nkeys, int norderbys);
void graph_index_rescan_internal(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys);
bool graph_index_gettuple_internal(IndexScanDesc scan, void *in_so, BlockNumber metablkno, size_t ef, float *dist_out);
void graph_index_endscan_internal(IndexScanDesc scan);

/* vacuum */
IndexBulkDeleteResult *graph_index_bulkdelete_internal(Relation index, IndexBulkDeleteResult *stats, 
    int nparallel, IndexBulkDeleteCallback callback, void *callback_state, BlockNumber metablkno);
IndexBulkDeleteResult *graph_index_vacuumcleanup_internal(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);

/* cost estimate */
void graph_index_costestimate_internal(PlannerInfo *root, IndexPath *path, double loop_count,
    Cost *indexStartupCost, Cost *indexTotalCost, Selectivity *indexSelectivity, 
    double *indexCorrelation, double *indexPages);

/* options */
bytea *graph_index_options_internal(Datum reloptions, bool validate);

/* validate */
bool graph_index_validate_internal(Oid opclassoid);

/* utils */
int graph_index_get_m(Relation index);
int graph_index_get_ef_construction(Relation index);
int graph_index_get_build_parallel(Relation index);
IdType graph_index_get_id_type(Relation index);
bool graph_index_get_enable_async_insert(Relation index);
void graph_index_init_page(Buffer buf, Page page);
QuantizerType graph_index_get_quantizer_type(Relation index);
FmgrInfo *graph_index_optional_proc_info(Relation index, uint16 procnum);
void create_vec_data(Relation index, bool create);
uint16_t graph_index_get_dim(Relation index);

#endif /* GRAPH_INDEX_H */