#include "pg_compat.h"
#include "graph_signature.h"
#include "utils/relcache.h"
#include "storage/itemptr.h"

#include "graph_index/graph_index.h"
#include "graph_index/graph_index_algorithm.h"
#include "ann_utils.h"
#include "distance/core/distance_dispatcher.h"
#include "annkmeans.h"


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

    graph_signature_invalidate(index,metap);

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

    DiskStoreVariant disk_store;
    create_disk_store(disk_store, index, heap, metabuf, true);
    PointExtensionContext ctx(index, GRAPH_INDEX_PS_BLKNO, true);
    auto visitor = [&](auto &store) -> void {
        constexpr DispatcherMode mode = DispatcherMode::DEFAULT;
        return DispatchRunner<true,
            MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE>,
            DistPrecisionTypeList<
                DistPrecisionType::FLOAT
            >, mode>::call(metap, [&](auto &distancer) {
                distancer.prepare(index, metap);
                distancer.process(query);
                GraphIndexAlgorithm algo{metap, store, distancer};
                typename decltype(algo)::InsertContext ictx{ctx, query, heap_tid};
                algo.insert(ictx);
                ictx.destroy();
            }
        );
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
