#include "pg_compat.h"
#include "utils/relcache.h"
#include "storage/itemptr.h"

#include "graph_index/graph_index.h"
#include "graph_index/graph_index_algorithm.h"
#include "ann_utils.h"
#include "distance/distance_dispatcher.h"
#include "annkmeans.h"
#include "buffer_manager.h"
#include "graph_index/core_node_store_bridge_runtime.hpp"

static void quantizer_auto_enable(Relation index, GraphIndexMetaPage metap)
{
    if (!RelationNeedsWAL(index) || metap->num_vectors < GRAPH_INDEX_MIN_QT_SAMPLES_SIZE) {
        return;
    }
    QuantizerMetaInfo &qt_metainfo = metap->quantizer_metainfo;
    QuantizerType setting_type = qt_metainfo.get_setting_type();
    QtUpdateMgr *qt_update_mgr = (QtUpdateMgr *)g_instance.annvec_cxt.qt_update_mgr;
    TimeRing *timering = qt_update_mgr->find_timering(index->rd_id);
    if (timering) {
        timering->visit();
    } else {
        timering = qt_update_mgr->insert_timgring(index->rd_id);
    }
    bool need_update = qt_metainfo.get_type() != qt_metainfo.get_setting_type();
    if (need_update && !qt_update_mgr->contain_updating(index->rd_id)) {
        /* launch a backgroud thread */
        float freq_10min = timering->get_all() / 10;
        if (freq_10min > 10000 && metap->num_vectors < MAX_SAMPLE_VECTOR_NUM) {
            return;
        }
        if (!qt_update_mgr->insert_updating(index->rd_id)) {
            return;
        }
        QuantizerUpdateParam param;
        param.qt_type = setting_type;
        param.enable = false;
        param.metablkno = GRAPH_INDEX_METAPAGE_BLKNO;
        param.freq_10min = freq_10min;
        param.force = false;
        add_quantizer_update_task(index, &param);
        QT_UPDATE_LOG("index \"%s\" num_vectors: %lu, freq_10min: %f, freq_10min <= 10000 or "
                      "num_vectors >= %u, add quantizer update task.",
                      RelationGetRelationName(index), metap->num_vectors, freq_10min, GRAPH_INDEX_MIN_QT_SAMPLES_SIZE);
    }

}

bool graph_index_insert_internal(Relation index, Relation heap, Datum *values, const bool *isnull,
    ItemPointer heap_tid, BlockNumber metablkno)
{
    if (isnull[0]) {
        return false;
    }

    MemoryContext insert_ctx = AllocSetContextCreate(CurrentMemoryContext,
        "graph_index insert temporary context", ALLOCSET_DEFAULT_SIZES);
    MemoryContext old_ctx = MemoryContextSwitchTo(insert_ctx);

    Buffer metabuf = ReadBuffer(index, metablkno);
    GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(BufferGetPage(metabuf));

    Pointer vec_p;
    char *v = DatumGetVector(values[0], metap->precision_type, &vec_p);
    char *query = v;
    bool is_alloc = false;
    FmgrInfo *normprocinfo = graph_index_optional_proc_info(index, GRAPH_INDEX_NORM_PROC);
    bool need_norm = normprocinfo != NULL;
    if (!is_aligned(query) || need_norm) {
        uint_fast32_t vec_size = metap->dimension * VEC_ELEM_SIZE(metap->precision_type);
        query = alloc_vector(vec_size);
        memcpy(query, v, vec_size);
        is_alloc = true;
    }
    if (need_norm) {
        auto func = ann_helper::get_vector_preprocess_func(Metric::FAST_COSINE, metap->precision_type, metap->dimension);
        func(query, metap->dimension, query);
    }

    quantizer_auto_enable(index, metap);

    bool use_async = graph_index_get_enable_async_insert(index);

    BulkBuffer *bulkbuf = GET_BULKBUF(index);
    DiskStoreVariant disk_store;
    create_disk_store(disk_store, index, heap, metabuf, bulkbuf, true);
    PointExtensionContext ctx(index, GRAPH_INDEX_PS_BLKNO, true);
    auto visitor = [&](auto &store) -> void {
        auto run_insert = [&](auto &distancer) {
                if (pgvexdb::TryInsertViaCoreBridge(store, index, metap, ctx, query, *heap_tid, use_async)) {
                    return;
                }
                distancer.prepare(index, metap);
                distancer.process(query);
                GraphIndexAlgorithm algo{metap, store, distancer};
                typename decltype(algo)::InsertContext ictx{ctx, query, heap_tid};
                if (use_async) {
                    algo.async_insert(ictx);
                } else {
                    algo.insert(ictx);
                }
                ictx.destroy();
        };

        if constexpr (std::decay_t<decltype(store)>::clustered) {
            return DispatchRunner<true,
                MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE>,
                DistPrecisionTypeList<
                    DistPrecisionType::FLOAT,
                    DistPrecisionType::HALF,
                    DistPrecisionType::INT8
                >, DispatcherMode::NO_QUANT>::call(metap, run_insert);
        } else {
            return DispatchRunner<true,
                MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE>,
                DistPrecisionTypeList<
                    DistPrecisionType::FLOAT,
                    DistPrecisionType::HALF,
                    DistPrecisionType::INT8
                >, DispatcherMode::DEFAULT>::call(metap, run_insert);
        }
    };
    visit(visitor, disk_store);
    disk_store.destroy();
    ctx.destroy();
    if (vec_p != DatumGetPointer(values[0])) {
        pfree(vec_p);
    }
    if (is_alloc) {
        free_vector(query);
    }
    ReleaseBuffer(metabuf);
    
    MemoryContextSwitchTo(old_ctx);
    MemoryContextDelete(insert_ctx);
    return true;
}
