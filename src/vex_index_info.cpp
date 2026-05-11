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

Datum vex_index_info(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;

    if (SRF_IS_FIRSTCALL()) {
        funcctx = SRF_FIRSTCALL_INIT();
        MemoryContext oldctx = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        // Use the SQL-declared RETURNS TABLE schema so we don't drift
        // from the .sql definition (which would give "wrong record type"
        // on tuple build).
        TupleDesc tupdesc;
        if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE) {
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("vex_index_info() must be called in a context that "
                       "expects a record type")));
        }
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
                    oids = lappend_oid(oids, form->oid);
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
    if (funcctx->call_cntr >= funcctx->max_calls) SRF_RETURN_DONE(funcctx);

    List *oids = (List *)funcctx->user_fctx;
    Oid index_oid = list_nth_oid(oids, (int)funcctx->call_cntr);
    Relation index = relation_open(index_oid, AccessShareLock);

    Datum values[VEX_INDEX_INFO_NCOLS];
    bool nulls[VEX_INDEX_INFO_NCOLS];
    memset(nulls, 0, sizeof(nulls));
    int c = 0;

    const char *iname = RelationGetRelationName(index);
    values[c++] = CStringGetTextDatum(iname);
    values[c++] = CStringGetTextDatum(iname);
    values[c++] = CStringGetTextDatum("GRAPH_INDEX");

    Oid table_oid = index->rd_index ? index->rd_index->indrelid : InvalidOid;
    char *tabname = OidIsValid(table_oid) ? get_rel_name(table_oid) : NULL;
    values[c++] = CStringGetTextDatum(tabname ? tabname : "");
    values[c++] = Int32GetDatum(0);                            // partition_count

    int64 node_count = 0;
    int32 max_level = 0;
    int32 dim = 0, m_val = 0, efc_val = 0;
    const char *metric_str = "l2";
    bool use_pq = false;
    int32 pq_m_val = 0;

    // Best-effort metapage read — guard against indexes whose main fork
    // is empty (e.g., ambuildempty before any CREATE INDEX populates it).
    if (RelationGetNumberOfBlocks(index) > GRAPH_INDEX_METAPAGE_BLKNO) {
        Buffer mb = ReadBuffer(index, GRAPH_INDEX_METAPAGE_BLKNO);
        LockBuffer(mb, BUFFER_LOCK_SHARE);
        GraphIndexMetaPage mp = GRAPH_INDEX_PAGE_GET_META(BufferGetPage(mb));
        if (mp->magic_number == GRAPH_INDEX_MAGIC_NUMBER) {
            node_count = (int64)mp->num_vectors;
            max_level  = (int32)(mp->entry_level + 1);
            dim        = (int32)mp->dimension;
            m_val      = (int32)mp->m;
            efc_val    = (int32)mp->ef_construction;
            metric_str = metric_name(mp->metric);
            use_pq     = (mp->quantizer_metainfo.get_setting_type() == QuantizerType::PQ);
            if (mp->quantizer_metainfo.get_setting_type() == QuantizerType::PQ) {
                pq_m_val = (int32)mp->quantizer_metainfo.get_pq_metainfo().m;
            }
        }
        UnlockReleaseBuffer(mb);
    }

    // Prefer heap reltuples for node_count if positive.
    if (OidIsValid(table_oid)) {
        HeapTuple htup = SearchSysCache1(RELOID, ObjectIdGetDatum(table_oid));
        if (HeapTupleIsValid(htup)) {
            Form_pg_class form = (Form_pg_class)GETSTRUCT(htup);
            if (form->reltuples > 0) node_count = (int64)form->reltuples;
            ReleaseSysCache(htup);
        }
    }

    values[c++] = Int64GetDatum(node_count);
    values[c++] = Int32GetDatum(max_level);
    values[c++] = Int32GetDatum(dim);
    values[c++] = Int64GetDatum(0);                            // row_id_map_size
    values[c++] = Int32GetDatum(m_val);
    values[c++] = Int32GetDatum(efc_val);
    values[c++] = CStringGetTextDatum(metric_str);
    values[c++] = BoolGetDatum(use_pq);
    values[c++] = Int32GetDatum(pq_m_val);

    int64 mem = (int64)RelationGetNumberOfBlocks(index) * BLCKSZ;
    values[c++] = Int64GetDatum(mem);
    values[c++] = Int64GetDatum(0);                            // pq_codes_bytes
    values[c++] = Int64GetDatum(0);                            // pq_codebook_bytes
    values[c++] = CStringGetTextDatum("full");

    relation_close(index, AccessShareLock);

    HeapTuple tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
    SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
}
