// Disposable diagnostic module. Export existing graph edges/TIDs without
// rebuilding or changing product search. Not installed by the extension.
#include "pg_compat.h"
#include <vtl/variant>
#include "graph_index/graph_index_struct.h"
#include "graph_index/graph_index_storage.h"
#include "graph_index/graph_index_algorithm.h"
#include "distance/core/distance_dispatcher.h"
#include "floatvector.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"

extern "C" {
PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(test_graph_topology_export);
Datum test_graph_topology_export(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(test_graph_search_trace);
Datum test_graph_search_trace(PG_FUNCTION_ARGS);
}

Datum test_graph_topology_export(PG_FUNCTION_ARGS) {
    Relation index = index_open(PG_GETARG_OID(0), AccessShareLock);
    Relation heap = table_open(index->rd_index->indrelid, AccessShareLock);
    Buffer meta = ReadBuffer(index, GRAPH_INDEX_METAPAGE_BLKNO);
    auto metap = GRAPH_INDEX_PAGE_GET_META(BufferGetPage(meta));
    StringInfoData out;
    initStringInfo(&out);
    DiskStoreVariant variant;
    create_disk_store(variant, index, heap, meta, false);
    PointExtensionContext ctx(index, GRAPH_INDEX_PS_BLKNO, false);
    auto dump = [&](auto &store) {
        using Store = std::remove_reference_t<decltype(store)>;
        using T = typename Store::T;
        const auto entry = store.template get_entry<false>().first;
        appendStringInfo(&out, "META %llu %u %llu %d\n", (unsigned long long)metap->num_vectors,
                         unsigned(metap->m), (unsigned long long)entry.id, int(entry.level));
        Vector<GraphIndexCandidate<T>> neighbors;
        for (T id = 0; id < T(metap->num_vectors); ++id) {
            CHECK_FOR_INTERRUPTS();
            neighbors.clear();
            GraphIndexCandidate<T> point(id, id, T(INVALID_VECTOR_ID), 0, nullptr);
            store.template get_neighbors<true>(neighbors, point);
            appendStringInfo(&out, "N %llu", (unsigned long long)id);
            store.get_itempointer(id, [&](const typename Store::point_type *elem) {
                elem->apply_on_tids(ctx, [&](const ItemPointerData &tid) {
                    appendStringInfo(&out, " T %u %u", ItemPointerGetBlockNumber(&tid),
                                     unsigned(ItemPointerGetOffsetNumber(&tid)));
                    return false;
                });
            });
            appendStringInfo(&out, " E");
            for (auto neighbor : neighbors) appendStringInfo(&out, " %llu", (unsigned long long)neighbor.id);
            appendStringInfoChar(&out, '\n');
        }
        neighbors.destroy();
    };
    visit(dump, variant);
    ctx.destroy(); variant.destroy(); ReleaseBuffer(meta);
    table_close(heap, AccessShareLock); index_close(index, AccessShareLock);
    text *result = cstring_to_text_with_len(out.data, out.len);
    pfree(out.data);
    PG_RETURN_TEXT_P(result);
}

template <typename ID> struct TraceDiskStore : DiskStore<ID> {
    using Base = DiskStore<ID>;
    using Base::get_distance;
    ID target;
    bool scored=false, expanded=false;
    size_t scored_count=0, expanded_count=0;
    TraceDiskStore(Relation index, Relation heap, Buffer meta, ID watched)
        : Base(index, heap, meta, false), target(watched) {}
    template <typename D> float get_distance(const D &d, const char *q, ID id) {
        scored|=id==target;++scored_count;return Base::get_distance(d, q, id);
    }
    template <typename D, typename IDs>
    void get_distance_batch(const D &d, const char *q, const IDs &ids, float *out) {
        for (auto id : ids) {scored|=id==target;++scored_count;}
        Base::get_distance_batch(d, q, ids, out);
    }
    template <bool base> auto get_point_info(ID id) {
        if constexpr (base) {expanded|=id==target;++expanded_count;}
        return Base::template get_point_info<base>(id);
    }
};

Datum test_graph_search_trace(PG_FUNCTION_ARGS) {
    Relation index = index_open(PG_GETARG_OID(0), AccessShareLock);
    Relation heap = table_open(index->rd_index->indrelid, AccessShareLock);
    Buffer meta = ReadBuffer(index, GRAPH_INDEX_METAPAGE_BLKNO);
    auto metap = GRAPH_INDEX_PAGE_GET_META(BufferGetPage(meta));
    if (metap->id_type != IdType::U32 || metap->quantizer_metainfo.get_type() != QuantizerType::NONE)
        elog(ERROR, "trace requires a U32 original-vector graph");
    auto query = PG_GETARG_FLOATVECTOR_P(1);
    if (query->dim != metap->dimension) elog(ERROR, "trace dimension mismatch");
    const int ef = PG_GETARG_INT32(2);
    if (ef < 1 || ef > 65536) elog(ERROR, "trace width out of range");
    const uint32 target = uint32(PG_GETARG_INT64(3));
    if (target>=metap->num_vectors) elog(ERROR, "trace target out of range");
    TraceDiskStore<uint32> store(index, heap, meta, target);
    StringInfoData out; initStringInfo(&out);
    DispatchRunner<true, MetricList<Metric::L2>, DistPrecisionTypeList<DistPrecisionType::FLOAT>,
                   DispatcherMode::DEFAULT>::call(metap, [&](auto &distancer) {
        distancer.prepare(index, metap); distancer.process((char *)query->x);
        GraphIndexAlgorithm algo(metap, store, distancer);
        auto candidates = algo.search_internal(store.template get_entry<false>().first, (char *)query->x, ef);
        bool found = false;
        for (auto c : candidates) if (c.id == target) found = true;
        appendStringInfo(&out, "target=%u scored=%d expanded=%d candidate=%d score_calls=%zu expanded_total=%zu",
                         target, int(store.scored), int(store.expanded), int(found),
                         store.scored_count, store.expanded_count);
        candidates.destroy(); algo.destroy();
    });
    store.destroy(); ReleaseBuffer(meta);
    table_close(heap, AccessShareLock); index_close(index, AccessShareLock);
    text *result = cstring_to_text_with_len(out.data, out.len); pfree(out.data);
    PG_FREE_IF_COPY(query, 1);
    PG_RETURN_TEXT_P(result);
}
