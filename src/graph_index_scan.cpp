/*
 * graph_index_scan.cpp - Graph index scan implementation
 * Adapted from openGauss for PostgreSQL
 */

#include "pg_compat.h"

#include <vtl/optional>
#include <vtl/vector>
#include <vtl/variant>

#include "graph_index/graph_index_cluster.h"
#include "graph_index/graph_index_struct.h"
#include "graph_index/graph_index.h"
#include "graph_index/graph_index_storage.h"
#include "graph_index/graph_index_algorithm.h"
#include "ann_utils.h"
#include "distance/core/distance_dispatcher.h"
#include "floatvector.h"

struct GraphIndexScanOpaqueData {
    bool first;
    bool has_more_data;
    int tid_offset;
    int tid_count;
    ItemPointer heaptids;
    float *dists;
    MemoryContext tmp_ctx;
    
    GraphIndexScanOpaqueData()
        : first(true),
          has_more_data(false),
          tid_offset(0),
          tid_count(0),
          heaptids(nullptr),
          dists(nullptr),
          tmp_ctx(AllocSetContextCreate(CurrentMemoryContext,
                  "Graph Index scan temporary context", ALLOCSET_DEFAULT_SIZES)) {}
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
    so->tid_offset = 0;
    so->tid_count = 0;
    MemoryContextReset(so->tmp_ctx);
    
    if (keys && scan->numberOfKeys > 0) {
        memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
    }
    if (orderbys && scan->numberOfOrderBys > 0) {
        memmove(scan->orderByData, orderbys, scan->numberOfOrderBys * sizeof(ScanKeyData));
        /* Reset orderby results */
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
            elog(ERROR, "cannot scan hnsw index without order");
        }
        if (scan->orderByData->sk_flags & SK_ISNULL) {
            return false;
        }

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

        DiskStoreVariant disk_store;
        create_disk_store(disk_store, index, heap, metabuf, NULL, false);

        auto visitor = [&](auto &store) -> void {
            constexpr DispatcherMode mode = 
                store.clustered ? DispatcherMode::NO_QUANT : DispatcherMode::DEFAULT;
            DispatchRunner<true,
                MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE>,
                DistPrecisionTypeList<
                    DistPrecisionType::FLOAT,
                    DistPrecisionType::HALF
                >, mode>::call(metap, [&](auto &distancer) {
                distancer.prepare(index, metap);
                distancer.process(query);
                GraphIndexAlgorithm algo{metap, store, distancer};
                PointExtensionContext ctx(index, GRAPH_INDEX_PS_BLKNO, false);
                
                auto [entry_info, shared_lock] = store.template get_entry<false>();
                auto [res, dists] = algo.search(ctx, query, ef);
                ctx.destroy();

                /* Copy results to scan's memory context */
                so->tid_count = res.size();
                if (so->tid_count > 0) {
                    so->heaptids = (ItemPointer)MemoryContextAlloc(so->tmp_ctx, 
                                    so->tid_count * sizeof(ItemPointerData));
                    so->dists = (float *)MemoryContextAlloc(so->tmp_ctx, 
                                    so->tid_count * sizeof(float));
                    memcpy(so->heaptids, res.data(), so->tid_count * sizeof(ItemPointerData));
                    memcpy(so->dists, dists.data(), so->tid_count * sizeof(float));
                }
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

    if (so->tid_offset >= so->tid_count) {
        return false;
    }
    
    int offset = so->tid_offset++;
    scan->xs_heaptid = so->heaptids[offset];
    scan->xs_heap_continue = (so->tid_offset < so->tid_count);
    
    if (scan->numberOfOrderBys > 0) {
        scan->xs_orderbyvals[0] = Float4GetDatum(so->dists[offset]);
        scan->xs_orderbynulls[0] = false;
        scan->xs_recheckorderby = false;
    }
    
    if (dist_out) {
        *dist_out = so->dists[offset];
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
