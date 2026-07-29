/**
 * Copyright (c) 2021-2024 Huawei Technologies Co., Ltd.
 * Index inspection for graph index.
 * Adapted for PostgreSQL from openGauss src/gausskernel/storage/access/graph_index/graph_index_inspect.cpp
 */

#include "pg_compat.h"

#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/stringize.hpp>
#include <sys/stat.h>

extern "C" {
#include "funcapi.h"
#include "catalog/pg_class.h"
#include "utils/builtins.h"
#include "utils/rel.h"
}

#include "index_inspect.h"
#include "graph_index/graph_index.h"
#include "graph_index/graph_index_param.h"
#include "graph_index/graph_index_storage.h"
#include "module/size_format.h"

#define ARCH_NAME_CASE(r, data, isa) \
    case Arch::isa:                  \
        return BOOST_PP_STRINGIZE(isa);

static const char *arch_name(Arch arch)
{
    switch (arch) {
        BOOST_PP_SEQ_FOR_EACH(ARCH_NAME_CASE, _, DISTANCER_ISAS)
        default:
            return "unknown";
    }
}

#undef ARCH_NAME_CASE

static int64 calculate_relation_size(Relation rel, ForkNumber forknum)
{
    int64 totalsize = 0;
#if PG_VERSION_NUM >= 180000
    RelPathStr relationpath;
#else
    char *relationpath;
#endif
    char pathname[MAXPGPATH];
    unsigned int segcount = 0;

    relationpath = relpathbackend(rel->rd_locator, rel->rd_backend, forknum);

    for (segcount = 0;; segcount++) {
        struct stat fst;

        CHECK_FOR_INTERRUPTS();

#if PG_VERSION_NUM >= 180000
        if (segcount == 0)
            snprintf(pathname, MAXPGPATH, "%s", relationpath.str);
        else
            snprintf(pathname, MAXPGPATH, "%s.%u", relationpath.str, segcount);
#else
        if (segcount == 0)
            snprintf(pathname, MAXPGPATH, "%s", relationpath);
        else
            snprintf(pathname, MAXPGPATH, "%s.%u", relationpath, segcount);
#endif

        if (stat(pathname, &fst) < 0) {
            if (errno == ENOENT)
                break;
            else
                ereport(ERROR,
                        (errcode_for_file_access(),
                         errmsg("could not stat file \"%s\": %m", pathname)));
        }
        totalsize += fst.st_size;
    }

#if PG_VERSION_NUM < 180000
    pfree(relationpath);
#endif

    return totalsize;
}

static void graph_index_inspect(Relation index, IndexInspectResult &res)
{
    RelationGetSmgr(index);
    
    const size_t vector_size = smgrexists(index->rd_smgr, VECTOR_FORKNUM) ?
        calculate_relation_size(index, VECTOR_FORKNUM) : 0;
    
    size_t total_size = (size_t)RelationGetNumberOfBlocksInFork(index, MAIN_FORKNUM) * BLCKSZ;
    total_size += vector_size;
    total_size += smgrexists(index->rd_smgr, FSM_FORKNUM) ?
        calculate_relation_size(index, FSM_FORKNUM) : 0;
    
    res.append_attr("Used Space");
    res.fill_content(total_size);
    res.append_attr("Vector Space");
    res.fill_content(vector_size);

    constexpr BlockNumber meta_blkno = GRAPH_INDEX_METAPAGE_BLKNO;
    Buffer metabuf = ReadBuffer(index, meta_blkno);
    
    GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(BufferGetPage(metabuf));
    
    if (metap->id_type == IdType::U32) {
        DiskStore<uint32_t> disk_store{index, nullptr, metabuf, false};
        disk_store.inspect(res);
        disk_store.destroy();
    } else {
        DiskStore<size_t> disk_store{index, nullptr, metabuf, false};
        disk_store.inspect(res);
        disk_store.destroy();
    }

    Arch used_arch = get_best_arch(metap->metric, metap->precision_type, metap->dimension);
    res.append_attr("Architecture Usage");
    res.fill_content(arch_name(used_arch));

    res.append_attr("Setting Quantizer");
    QuantizerType sqt = metap->quantizer_metainfo.get_setting_type();
    res.fill_content(quantizer_name(sqt));

    QuantizerType working_qt = metap->quantizer_metainfo.get_type();
    const bool compact_mode = graph_index_get_compact_mode(index) &&
        working_qt != QuantizerType::NONE;
    res.append_attr("Memory Mode");
    res.fill_content(compact_mode ? "compact" : "full");
    res.append_attr("Vector Storage");
    if (working_qt == QuantizerType::PQ) {
        res.fill_content("PQ Code");
    } else if (working_qt == QuantizerType::RABITQ) {
        res.fill_content("RaBitQ Code");
    } else {
        res.fill_content("Raw Vector");
    }

    if (sqt != QuantizerType::NONE) {
        res.append_attr("Working Quantizer");
        res.fill_content(quantizer_name(working_qt));
        res.append_attr("Number of Data Waiting for Quantizer Update");
        res.fill_content("%lu", metap->quantizer_metainfo.num_new_data);
    }
    
    ReleaseBuffer(metabuf);
}

extern "C" {
PG_FUNCTION_INFO_V1(index_inspect);
}

Datum index_inspect(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    
    if (SRF_IS_FIRSTCALL()) {
        funcctx = SRF_FIRSTCALL_INIT();
        MemoryContext oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
        
        Oid index_oid = PG_GETARG_OID(0);
        Relation index = index_open(index_oid, AccessShareLock);
        
        IndexInspectResult *res = new IndexInspectResult();
        
        graph_index_inspect(index, *res);
        
        index_close(index, AccessShareLock);
        
        TupleDesc tupdesc;
        if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE) {
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("index_inspect() must be called in a context that "
                       "expects a record type")));
        }
        funcctx->tuple_desc = BlessTupleDesc(tupdesc);
        funcctx->max_calls = res->nattr;
        funcctx->user_fctx = res;
        
        MemoryContextSwitchTo(oldcontext);
        
        if (funcctx->max_calls == 0) {
            delete res;
            SRF_RETURN_DONE(funcctx);
        }
    }
    
    funcctx = SRF_PERCALL_SETUP();
    IndexInspectResult *res = (IndexInspectResult *)funcctx->user_fctx;
    
    if (res && funcctx->call_cntr < funcctx->max_calls) {
        Datum values[2];
        bool nulls[2] = {false};
        values[0] = res->attributes[funcctx->call_cntr];
        values[1] = res->contents[funcctx->call_cntr];
        
        HeapTuple tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
        if (!tuple) {
            delete res;
            SRF_RETURN_DONE(funcctx);
        }
        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }
    
    delete res;
    SRF_RETURN_DONE(funcctx);
}
