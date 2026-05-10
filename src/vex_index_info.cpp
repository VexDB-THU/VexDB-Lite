#include "pg_compat.h"

extern "C" {
#include "funcapi.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/pg_class.h"
#include "catalog/pg_am.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "utils/builtins.h"
}

#include "graph_index/graph_index.h"
#include "graph_index/graph_index_param.h"
#include "graph_index/graph_index_struct.h"
#include "quantizer.h"
#include "ann_utils.h"

extern "C" {
PG_FUNCTION_INFO_V1(vex_index_info);
}

// Schema mirrors duckdb/vexdb-duck/functions/index_info_function.cpp.
// `indexname` and `index_name` are both emitted because translated tests
// reference both names interchangeably.
#define VEX_INDEX_INFO_NCOLS 18

static const char *metric_name(Metric m)
{
    switch (m) {
        case Metric::L2:            return "l2";
        case Metric::INNER_PRODUCT: return "ip";
        case Metric::COSINE:        return "cosine";
        case Metric::FAST_COSINE:   return "cosine";
        default:                    return "unknown";
    }
}

static int64 calculate_relation_total_size(Relation rel)
{
    int64 total = 0;
    for (int fork = 0; fork <= MAX_FORKNUM; ++fork) {
        if (smgrexists(RelationGetSmgr(rel), (ForkNumber)fork)) {
            total += (int64)RelationGetNumberOfBlocksInFork(rel, (ForkNumber)fork) * BLCKSZ;
        }
    }
    return total;
}

Datum vex_index_info(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;

    if (SRF_IS_FIRSTCALL()) {
        funcctx = SRF_FIRSTCALL_INIT();
        MemoryContext oldctx = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        TupleDesc tupdesc = CreateTemplateTupleDesc(VEX_INDEX_INFO_NCOLS);
        AttrNumber a = 1;
        TupleDescInitEntry(tupdesc, a++, "index_name",        TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, a++, "indexname",         TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, a++, "index_type",        TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, a++, "table_name",        TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, a++, "partition_count",   INT4OID, -1, 0);
        TupleDescInitEntry(tupdesc, a++, "node_count",        INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, a++, "max_level",         INT4OID, -1, 0);
        TupleDescInitEntry(tupdesc, a++, "dimension",         INT4OID, -1, 0);
        TupleDescInitEntry(tupdesc, a++, "row_id_map_size",   INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, a++, "m",                 INT4OID, -1, 0);
        TupleDescInitEntry(tupdesc, a++, "ef_construction",   INT4OID, -1, 0);
        TupleDescInitEntry(tupdesc, a++, "metric",            TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, a++, "use_pq",            BOOLOID, -1, 0);
        TupleDescInitEntry(tupdesc, a++, "pq_m",              INT4OID, -1, 0);
        TupleDescInitEntry(tupdesc, a++, "memory_bytes",      INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, a++, "pq_codes_bytes",    INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, a++, "pq_codebook_bytes", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, a++, "memory_mode",       TEXTOID, -1, 0);
        funcctx->tuple_desc = BlessTupleDesc(tupdesc);

        Oid am_oid = GetSysCacheOid1(AMNAME, Anum_pg_am_oid,
                                     CStringGetDatum("vexdb_graph"));
        List *oids = NIL;
        if (OidIsValid(am_oid)) {
            Relation cls = table_open(RelationRelationId, AccessShareLock);
            ScanKeyData skey;
            ScanKeyInit(&skey, Anum_pg_class_relam, BTEqualStrategyNumber,
                        F_OIDEQ, ObjectIdGetDatum(am_oid));
            SysScanDesc scan = systable_beginscan(cls, InvalidOid, false,
                                                  GetActiveSnapshot(), 1, &skey);
            HeapTuple tup;
            while ((tup = systable_getnext(scan)) != NULL) {
                Form_pg_class form = (Form_pg_class)GETSTRUCT(tup);
                if (form->relkind == RELKIND_INDEX) {
                    Oid relid;
                    relid = form->oid;
                    oids = lappend_oid(oids, relid);
                }
            }
            systable_endscan(scan);
            table_close(cls, AccessShareLock);
        }
        funcctx->user_fctx = oids;
        funcctx->max_calls = list_length(oids);
        MemoryContextSwitchTo(oldctx);
        if (funcctx->max_calls == 0) SRF_RETURN_DONE(funcctx);
    }

    funcctx = SRF_PERCALL_SETUP();
    List *oids = (List *)funcctx->user_fctx;
    if ((int64)funcctx->call_cntr >= (int64)funcctx->max_calls) {
        SRF_RETURN_DONE(funcctx);
    }
    Oid index_oid = list_nth_oid(oids, (int)funcctx->call_cntr);
    Relation index = relation_open(index_oid, AccessShareLock);

    Datum values[VEX_INDEX_INFO_NCOLS];
    bool nulls[VEX_INDEX_INFO_NCOLS] = {false};
    int c = 0;

    const char *name = RelationGetRelationName(index);
    values[c++] = CStringGetTextDatum(name);                 // index_name
    values[c++] = CStringGetTextDatum(name);                 // indexname (alias)
    values[c++] = CStringGetTextDatum("GRAPH_INDEX");        // index_type
    char *tabname = get_rel_name(index->rd_index->indrelid);
    values[c++] = tabname ? CStringGetTextDatum(tabname) : CStringGetTextDatum("");
    values[c++] = Int32GetDatum(0);                          // partition_count

    Buffer mb = ReadBuffer(index, GRAPH_INDEX_METAPAGE_BLKNO);
    LockBuffer(mb, BUFFER_LOCK_SHARE);
    GraphIndexMetaPage mp = GRAPH_INDEX_PAGE_GET_META(BufferGetPage(mb));

    // num_vectors on the metapage tracks live in-memory graph nodes (some
    // builds populate level-0 lazily). For a CREATE INDEX-just-finished
    // count, prefer the heap's reltuples, which the build path stamps.
    int64 node_count = (int64)mp->num_vectors;
    HeapTuple htup = SearchSysCache1(RELOID,
                                     ObjectIdGetDatum(index->rd_index->indrelid));
    if (HeapTupleIsValid(htup)) {
        Form_pg_class form = (Form_pg_class)GETSTRUCT(htup);
        if (form->reltuples > 0) node_count = (int64)form->reltuples;
        ReleaseSysCache(htup);
    }
    values[c++] = Int64GetDatum(node_count);                  // node_count
    values[c++] = Int32GetDatum((int32)mp->entry_level + 1); // max_level
    values[c++] = Int32GetDatum((int32)mp->dimension);
    values[c++] = Int64GetDatum(0);                          // row_id_map_size
    values[c++] = Int32GetDatum((int32)mp->m);
    values[c++] = Int32GetDatum((int32)mp->ef_construction);
    values[c++] = CStringGetTextDatum(metric_name(mp->metric));

    QuantizerType qt = mp->quantizer_metainfo.get_type();
    values[c++] = BoolGetDatum(qt == QuantizerType::PQ);
    int32 pq_m = (mp->quantizer_metainfo.get_setting_type() == QuantizerType::PQ)
                     ? (int32)mp->quantizer_metainfo.get_pq_metainfo().m
                     : 0;
    values[c++] = Int32GetDatum(pq_m);

    UnlockReleaseBuffer(mb);

    int64 mem = calculate_relation_total_size(index);
    values[c++] = Int64GetDatum(mem);
    values[c++] = Int64GetDatum(0);                          // pq_codes_bytes
    values[c++] = Int64GetDatum(0);                          // pq_codebook_bytes
    values[c++] = CStringGetTextDatum("full");               // memory_mode

    relation_close(index, AccessShareLock);

    HeapTuple tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
    SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
}
