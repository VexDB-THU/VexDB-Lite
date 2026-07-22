/*
 * graph_index_scan.cpp - Graph index scan implementation
 * Aligned with openGauss
 */

#include "pg_compat.h"

#include <vtl/optional>
#include <vtl/vector>
#include <vtl/variant>
#include <type_traits>

#include "graph_index/graph_index_cluster.h"
#include "graph_index/graph_index_struct.h"
#include "graph_index/graph_index.h"
#include "graph_index/graph_index_storage.h"
#include "graph_index/graph_index_algorithm.h"
#include "ann_utils.h"
#include "distance/core/distance_dispatcher.h"
#include "floatvector.h"
#include "vector_buffer/local_vec_cache.h"

struct GraphIndexScanOpaqueData {
    bool first;
    bool has_more_data;
    Optional<UnorderedSet<ItemPointerData>> returned;
    uint32 tid_offset;
    uint32 tid_count;
    GraphIndexSearchRes *res;
    Vector<ItemPointerData> unordered_tids;
    MemoryContext tmp_ctx;
    IndexTuple cached_itup;

    GraphIndexScanOpaqueData()
        : first(true),
          has_more_data(false),
          tid_offset(0),
          tid_count(0),
          res(nullptr),
          tmp_ctx(AllocSetContextCreate(CurrentMemoryContext,
                  "Graph Index scan temporary context", ALLOCSET_DEFAULT_SIZES)),
          cached_itup(nullptr) {}
};

using GraphIndexScanOpaque = GraphIndexScanOpaqueData *;

static Datum get_scan_value(IndexScanDesc scan)
{
    Datum value;
    if (scan->orderByData->sk_flags & SK_ISNULL) {
        value = PointerGetDatum(NULL);
    } else {
        value = scan->orderByData->sk_argument;
        Assert(!VARATT_IS_COMPRESSED(DatumGetPointer(value)));
        Assert(!VARATT_IS_EXTENDED(DatumGetPointer(value)));
    }
    return value;
}

IndexScanDesc graph_index_beginscan_internal(Relation index, int nkeys, int norderbys)
{
    IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);
    scan->opaque = new GraphIndexScanOpaqueData();

    if (norderbys > 0) {
        scan->xs_orderbyvals = (Datum *)palloc(sizeof(Datum) * norderbys);
        scan->xs_orderbynulls = (bool *)palloc(sizeof(bool) * norderbys);
        memset(scan->xs_orderbyvals, 0, sizeof(Datum) * norderbys);
        memset(scan->xs_orderbynulls, true, sizeof(bool) * norderbys);
        scan->xs_recheckorderby = false;
    }

    return scan;
}

void graph_index_rescan_internal(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
    GraphIndexScanOpaque so = (GraphIndexScanOpaque)scan->opaque;
    so->first = true;
    ann_helper::optional_destroy(so->returned);
    /* res is owned by tmp_ctx. MemoryContextReset frees it, so clear the
     * borrowed pointer before the next parameterized scan tries to pfree it. */
    so->res = nullptr;
    so->has_more_data = false;
    so->tid_offset = 0;
    so->tid_count = 0;
    so->unordered_tids.clear();
    so->cached_itup = nullptr;
    MemoryContextReset(so->tmp_ctx);

    if (keys && scan->numberOfKeys > 0) {
        memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
    }
    if (orderbys && scan->numberOfOrderBys > 0) {
        memmove(scan->orderByData, orderbys, scan->numberOfOrderBys * sizeof(ScanKeyData));
        memset(scan->xs_orderbyvals, 0, sizeof(Datum) * scan->numberOfOrderBys);
        memset(scan->xs_orderbynulls, true, sizeof(bool) * scan->numberOfOrderBys);
    }
}

bool graph_index_gettuple_internal(IndexScanDesc scan, void *in_so, BlockNumber metablkno, size_t ef, float *dist_out)
{
    Relation index = scan->indexRelation;
    Relation heap = scan->heapRelation;
    GraphIndexScanOpaque so = (GraphIndexScanOpaque)in_so;

    if (so->first) {
        if (scan->orderByData == NULL) {
            MemoryContext old_ctx = MemoryContextSwitchTo(so->tmp_ctx);
            Buffer metabuf = ReadBuffer(index, metablkno);
            GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(BufferGetPage(metabuf));

            DiskStoreVariant disk_store;
            create_disk_store(disk_store, index, heap, metabuf, false);
            PointExtensionContext ctx(index, GRAPH_INDEX_PS_BLKNO, false);

            auto collect_tids = [&](auto &store) -> void {
                using Store = std::remove_reference_t<decltype(store)>;
                using T = typename Store::T;
                T num_vectors = (T)metap->num_vectors;
                for (T id = 0; id < num_vectors; ++id) {
                    store.get_itempointer(id, [&](const typename Store::point_type *elem) -> void {
                        elem->apply_on_tids(ctx, [&](const ItemPointerData &tid) -> bool {
                            so->unordered_tids.push_back(tid);
                            return false;
                        });
                    });
                }
            };

            visit(collect_tids, disk_store);
            ctx.destroy();
            disk_store.destroy();
            ReleaseBuffer(metabuf);
            so->tid_count = so->unordered_tids.size();
            so->first = false;
            so->has_more_data = false;
            MemoryContextSwitchTo(old_ctx);
            goto retry2;
        }
        if (scan->orderByData->sk_flags & SK_ISNULL) {
            return false;
        }

retry:
        MemoryContext old_ctx = MemoryContextSwitchTo(so->tmp_ctx);

        Buffer metabuf = ReadBuffer(index, metablkno);
        GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(BufferGetPage(metabuf));

        Datum value = get_scan_value(scan);
        Pointer vec_p = NULL;
        char *v = DatumGetVector(value, metap->precision_type, &vec_p);

        if (uint16(((FloatVector *)vec_p)->dim) != metap->dimension) {
            if (vec_p != DatumGetPointer(value)) {
                pfree(vec_p);
            }
            ReleaseBuffer(metabuf);
            MemoryContextSwitchTo(old_ctx);
            elog(ERROR, "incorrect dimension of query vector");
        }

        char *query = v;
        bool alloced = false;
        if (!is_aligned(query)) {
            uint_fast32_t vec_size = metap->dimension * VEC_ELEM_SIZE(metap->precision_type);
            char *temp = alloc_vector(vec_size);
            memcpy(temp, query, vec_size);
            query = temp;
            alloced = true;
        }

        /* Track returned TIDs across ef-retry cycles to deduplicate */
        if (so->res) {
            if (!so->returned.has_value()) {
                so->returned.emplace(so->tid_count);
            }
            for (uint32 i = 0; i < so->tid_count; ++i) {
                so->returned->insert(so->res[i].tid);
            }
            pfree(so->res);
        }

        DiskStoreVariant disk_store;
        create_disk_store(disk_store, index, heap, metabuf, false);

        auto visitor = [&](auto &store) -> void {
            constexpr DispatcherMode mode = DispatcherMode::DEFAULT;
            DispatchRunner<true,
                MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE>,
                DistPrecisionTypeList<
                    DistPrecisionType::FLOAT
                >, mode>::call(metap, [&](auto &distancer) {
                distancer.prepare(index, metap);
                distancer.process(query);
                GraphIndexAlgorithm algo{metap, store, distancer};
                PointExtensionContext ctx(index, GRAPH_INDEX_PS_BLKNO, false);
                /* 本地缓存(local_vec_cache)经实测证伪(命中率 0.4%,HNSW search 无时间
                 * 局部性),暂不激活;改走 locmap 分区方案。代码保留但 dormant。 */
                Vector<GraphIndexSearchRes> res = algo.search(ctx, query, ef);
                ctx.destroy();
                algo.destroy();

                so->tid_count = res.size();
                so->res = res.data();
                so->first = false;
                so->has_more_data = res.size() >= ef;
            });
        };

        visit(visitor, disk_store);
        disk_store.destroy();

        ReleaseBuffer(metabuf);
        if (alloced) {
            free_vector(query);
        }
        if (vec_p != DatumGetPointer(value)) {
            pfree(vec_p);
        }
        MemoryContextSwitchTo(old_ctx);
    }

retry2:
    if (so->tid_offset >= so->tid_count) {
        if (!so->has_more_data) {
            return false;
        }
        ef = std::max((size_t)(so->tid_count * 2), (size_t)25);
        so->tid_offset = 0;
        goto retry;
    }

    int offset = so->tid_offset++;
    if (so->returned.has_value() && so->returned->contains(so->res[offset].tid)) {
        goto retry2;
    }

    const bool ordered_scan = scan->numberOfOrderBys > 0;
    scan->xs_heaptid = ordered_scan ? so->res[offset].tid : so->unordered_tids[offset];
    scan->xs_heap_continue = (so->tid_offset < so->tid_count);

    if (ordered_scan) {
        scan->xs_orderbyvals[0] = Float4GetDatum(so->res[offset].dist);
        scan->xs_orderbynulls[0] = false;
        scan->xs_recheckorderby = false;
    } else {
        TupleDesc itupdesc = RelationGetDescr(scan->indexRelation);
        if (so->cached_itup == nullptr) {
            Datum values[INDEX_MAX_KEYS];
            bool isnull[INDEX_MAX_KEYS];
            for (int i = 0; i < itupdesc->natts; i++) {
                values[i] = (Datum)0;
                isnull[i] = true;
            }
            MemoryContext old = MemoryContextSwitchTo(so->tmp_ctx);
            so->cached_itup = index_form_tuple(itupdesc, values, isnull);
            MemoryContextSwitchTo(old);
        }
        so->cached_itup->t_tid = scan->xs_heaptid;
        scan->xs_itup = so->cached_itup;
        scan->xs_itupdesc = itupdesc;
    }

    if (dist_out && ordered_scan) {
        *dist_out = so->res[offset].dist;
    }
    return true;
}

void graph_index_endscan_internal(IndexScanDesc scan)
{
    GraphIndexScanOpaque so = (GraphIndexScanOpaque)scan->opaque;
    MemoryContextDelete(so->tmp_ctx);
    delete so;
    scan->opaque = NULL;
}
