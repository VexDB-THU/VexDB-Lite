/*
 * graph_index_scan.cpp - Graph index scan implementation
 * Aligned with openGauss
 */

#include "pg_compat.h"
#include "graph_signature.h"
#include "guc_config.h"

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
extern "C" {
#include "utils/builtins.h"
PG_FUNCTION_INFO_V1(vexdb_last_search_info);
Datum vexdb_last_search_info(PG_FUNCTION_ARGS);
}

struct GraphScanInfo {
    const char *route = "none", *signature_state = "disabled";
    size_t rounds = 0, graph_ef = 0, distance_calls = 0, rescue_refinements = 0;
    size_t counted_rounds = 0;
};
// Backend-local diagnostics, never shared across sessions or cached as answers.
static GraphScanInfo last_search_info;
Datum vexdb_last_search_info(PG_FUNCTION_ARGS) {
    const auto &s=last_search_info;
    PG_RETURN_TEXT_P(cstring_to_text(psprintf(
        "{\"route\":\"%s\",\"signature_state\":\"%s\",\"rounds\":%zu,"
        "\"graph_ef\":%zu,\"distance_calls\":%zu,\"rescue_refinements\":%zu,\"distance_counted\":%s}",
        s.route,s.signature_state,s.rounds,s.graph_ef,s.distance_calls,s.rescue_refinements,
        s.rounds && s.counted_rounds==s.rounds?"true":"false")));
}

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
    size_t graph_ef = 0;
    GraphScanInfo info;

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
    so->graph_ef = 0;
    so->info = {};
    so->unordered_tids.clear();
    so->cached_itup = nullptr;
    MemoryContextReset(so->tmp_ctx);
    // The previous candidate array belonged to tmp_ctx. A parameterized
    // nested-loop scan reuses this descriptor after resetting that context.
    // Never treat its freed array as a live retry result on the next query.
    so->res = nullptr;
    so->has_more_data = false;

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
        last_search_info = {};
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
            store.set_heap_snapshot(scan->xs_snapshot);
            constexpr DispatcherMode mode = DispatcherMode::DEFAULT;
            DispatchRunner<true,
                MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE>,
                DistPrecisionTypeList<
                    DistPrecisionType::FLOAT
                >, mode>::call(metap, [&](auto &distancer) {
                distancer.prepare(index, metap);
                distancer.process(query);

                PointExtensionContext ctx(index, GRAPH_INDEX_PS_BLKNO, false);
                /* 本地缓存(local_vec_cache)经实测证伪(命中率 0.4%,HNSW search 无时间
                 * 局部性),暂不激活;改走 locmap 分区方案。代码保留但 dormant。 */
                Vector<GraphIndexSearchRes> res;
                PgGraphSignature signature;
                size_t graph_count=0;
                bool exact_fallback=false;
                const bool rescue_enabled=vexdb_lite_get_signature_rescue();
                const bool ready=rescue_enabled && signature.load(index,metap);
                so->info.signature_state=rescue_enabled?signature.state:"disabled";
                ++so->info.rounds;
                if (ready) {
                    ++so->info.counted_rounds;
                    size_t calls=0;
                    auto seen=(unsigned char*)palloc0((signature.header.count+7)/8);
                    signature_rescue::Observer observer{seen,size_t(signature.header.count),&calls};
                    using Store=std::remove_reference_t<decltype(store)>;
                    using Dister=std::remove_reference_t<decltype(distancer)>;
                    GraphIndexAlgorithm<Store,Dister,PgAlloc,signature_rescue::Observer> algo(metap,store,distancer,observer);
                    res=algo.search(ctx,query,ef,[](auto){return true;},[&](auto &points,auto &dister) {
                        graph_count=points.size();
                        so->info.rescue_refinements+=signature.complete(index,store,dister,query,observer,points);
                    });
                    so->info.distance_calls+=calls;
                    so->info.route=so->info.rescue_refinements?"graph_rescue":"graph";
                    algo.destroy();pfree(seen);signature.destroy();
                } else if (rescue_enabled && graph_signature_supported(metap)) {
                    // A write can invalidate an auxiliary even after rollback.
                    // Returning to a narrow graph silently would lose quality.
                    // Enumerate all raw nodes; the executor still checks heap
                    // visibility and SQL predicates under its own snapshot.
                    exact_fallback=true;
                    ++so->info.counted_rounds;
                    using Store=std::remove_reference_t<decltype(store)>;
                    for (typename Store::T id=0;id<metap->num_vectors;++id) {
                        CHECK_FOR_INTERRUPTS();
                        const float distance=store.get_distance(distancer,query,id);
                        ++so->info.distance_calls;
                        store.get_itempointer(id,[&](const typename Store::point_type *elem) {
                            elem->apply_on_tids(ctx,[&](const ItemPointerData &tid) {
                                res.push_back({tid,distance});return false;
                            });
                        });
                    }
                    std::sort(res.begin(),res.end(),[](const auto&a,const auto&b){return a.dist<b.dist;});
                    so->info.route="exact_signature_fallback";
                } else {
                    // Keep the original unobserved graph/PQ implementation on
                    // the default route. Zero here means unmeasured, not free.
                    GraphIndexAlgorithm algo{metap,store,distancer};
                    res=algo.search(ctx,query,ef);
                    graph_count=std::min<size_t>(res.size(),metap->num_vectors);
                    so->info.route="graph";algo.destroy();
                }
                ctx.destroy();

                so->tid_count = res.size();
                so->res = res.data();
                so->first = false;
                so->graph_ef=ef;so->info.graph_ef=exact_fallback?0:ef;
                so->has_more_data = !exact_fallback && graph_count >= ef;
                last_search_info=so->info;
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
        // TIDs can grow through dedup groups or rescue, independently of the
        // graph search width. Neither is a reason to inflate the next search.
        ef = std::min<size_t>(UINT32_MAX,std::max<size_t>(so->graph_ef*2,25));
        if (ef<=so->graph_ef) return false;
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
