/**
 * Copyright (c) 2026 VexDB-THU
 */

#ifndef GRAPH_INDEX_ALGORITHM_H
#define GRAPH_INDEX_ALGORITHM_H

#include <cmath>
#include <cassert>
#include <cfloat>
#include <numeric>      /* accumulate */
#include <algorithm>    /* max_element */
#include <vector>       /* std::vector for VLA replacements */

#include <vtl/bitvector>
#include <vtl/optional>
#include <vtl/priority_queue>
#include <vtl/hashtable>
#include <vtl/holder>
#include <vtl/span>
#include <vtl/tuple>

#include "graph_index/graph_index_depend.h"

#include "distance/core/distance.h"
#include "module/perf_usage.h"
#include <vtl/allocator>

inline int_fast8_t get_insert_level(uint_fast16_t m)
{
    return std::min<int_fast8_t>((-log(RandomDouble()) * (1 / log(m))), (GRAPH_INDEX_MAX_LEVEL - 1));
}

PERF_DECLARE_CATS(AlgPerfCats, false, insert_tid, split_phase1, split_phase2, split_phase3,
    reallocate, get_eviction, get_data, get_center, update_stat);

template <typename Store, typename Dister, template<typename> class Alloc = PgAlloc>
class GraphIndexAlgorithm : public PERFER(AlgPerfCats) {
    using PerfCats = AlgPerfCats;
    using T = typename Store::T;
    using point_type = typename Store::point_type;
    using Cand = GraphIndexCandidate<T>;
    static constexpr bool need_refine = Dister::need_refine &&
        !std::is_same<Store, MemStore<T, point_type>>::value;
    static constexpr bool has_est = Dister::has_estimation_func &&
        !std::is_same<Store, MemStore<T, point_type>>::value;
    static constexpr bool mem_store =
        std::is_same<Store, MemStore<T, point_type>>::value;

    static constexpr bool clustered = false;
    static constexpr bool use_dist_cache = Store::use_dist_cache;

    /* Alloc-injected container aliases */
    template<typename U> using Vec = Vector<U, Alloc<U>>;
    template<typename K, typename V, typename H, typename E>
    using Map = UnorderedMap<K, V, H, E, Alloc<Pair<K, V>>>;
    template<typename U, typename H = impl::DefaultHasher<U>, typename E = impl::DefaultEqual<U>>
    using USet = UnorderedSet<U, H, E, Alloc<U>>;

    struct ClosestCompare {
        bool operator()(const Cand &a, const Cand &b) const {
            return a.dist < b.dist;
        }
    };
    struct FurthestCompare {
        bool operator()(const Cand &a, const Cand &b) const {
            return a.dist > b.dist;
        }
    };
    struct PairHasher {
        uint32 operator()(const Pair<T, T> &p) const
        {
            constexpr uint64 prime = 0x9e3779b97f4a7c15;
            uint64 k = static_cast<uint64>(p.first) * p.second;
            k ^= k >> 33;
            k *= prime;
            k ^= k >> 29;
            return k;
        }
    };
    struct PairCmp {
        bool operator()(const Pair<T, T> &a, const Pair<T, T> &b) const
        {
            return a.first == b.first && a.second == b.second;
        }
    };
    using fpq = PriorityQueue<Cand, FurthestCompare, Alloc<Cand>>;
    using cpq = PriorityQueue<Cand, ClosestCompare, Alloc<Cand>>;
    uint_fast16_t ef_construction;
    uint_fast16_t m;
    Store &store;
    Dister &distancer;
    Holder<Map<Pair<T, T>, float, PairHasher, PairCmp>> dist_cache;
public:
    struct InsertContextBase {
        InsertContextBase(PointExtensionContext &c, const char *q, const ItemPointer t)
            : ctx(c), query(q), tid(t ? *(ItemPointer)t : ItemPointerData{}) {}
        PointExtensionContext &ctx;
        const char *query;
        ItemPointerData tid;
        // Published by insert() for adapters that maintain row-id side maps.
        // It is the newly allocated node id, or the existing node id when the
        // vector was deduplicated into an existing graph point.
        T result_id = static_cast<T>(INVALID_VECTOR_ID);
        static constexpr void check_round() {}
        static constexpr bool round_full() { return true; }
        static constexpr void destroy() {}
    };
    using InsertContext = InsertContextBase;

    GraphIndexAlgorithm(uint_fast16_t ef_construction, uint_fast16_t m, Store &store, Dister &distancer)
        : ef_construction(ef_construction),
          m(m),
          store(store),
          distancer(distancer),
          dist_cache() {}
    GraphIndexAlgorithm(const GraphIndexMetaPage metap, Store &store, Dister &distancer)
        : GraphIndexAlgorithm(metap->ef_construction, metap->m, store, distancer) {}

    void destroy()
    {
        REPORT_PERF(NOTICE);
        PERF_DESTROY();
    }

    /* filter 只作用于 base 层结果集（不通过的点仍参与图遍历导航）；上层纯导航不过滤 */
    template <typename filter_func>
    Vec<Cand> search_internal(const GraphIndexEntryInfo &entry_info, const char *query, uint32 ef_search,
                              filter_func &&filter)
    {
        CONSTEXPR_IF (need_refine) {
            constexpr float refine_factor = 1.25;
            ef_search *= refine_factor;
        }
        Vec<Cand> ep(ef_search);
        ep.emplace_back((T)entry_info.id, (T)entry_info.cur_layer_idx, get_distance(query, entry_info.id));
        for (int_fast8_t l = entry_info.level; l > 0; --l) {
            search_upper_layer(query, ep);
            replace_lower_layer_idx(ep);
        }
        ep = search_layer<true>(query, std::move(ep), ef_search, filter);
        return ep;
    }
    Vec<Cand> search_internal(const GraphIndexEntryInfo &entry_info, const char *query, uint32 ef_search)
    {
        return search_internal(entry_info, query, ef_search, dummy_filter);
    }
    template <typename filter_func>
    Vector<GraphIndexSearchRes> search(PointExtensionContext &ctx, const char *query, uint32 ef_search,
                                       filter_func &&filter)
    {
        Vector<GraphIndexSearchRes> res;
        const GraphIndexEntryInfo entry_info = store.template get_entry<false>().first;
        if (!is_valid(entry_info.id)) {
            return res;
        }

        Vec<Cand> ep = search_internal(entry_info, query, ef_search, filter);
        refine(ctx, ep, query);

        res.reserve(ep.size());
        Vector<ItemPointerData> temp;
        for (const Cand &cand : ep) {
            temp.clear();
            store.get_itempointer(cand.id, [&](const point_type *elem) -> void {
                uint32 ntid = elem->get_tids(temp, ctx);
                for (uint32 i = 0; i < ntid; ++i) {
                    res.push_back({temp[i], cand.dist});
                }
            });
        }
        ann_helper::optional_destroy(temp);
        ann_helper::optional_destroy(ep);
        return res;
    }
    Vector<GraphIndexSearchRes> search(PointExtensionContext &ctx, const char *query, uint32 ef_search)
    {
        return search(ctx, query, ef_search, dummy_filter);
    }

    void insert(InsertContext &ctx)
    {
        ctx.check_round();
        bool retried = false;
        int_fast8_t insert_level = get_insert_level(m);
retry:
        const bool bottom_only = false && !retried;
        auto entry_lock_state = bottom_only
            ? store.template get_entry<true, true>(insert_level)
            : store.template get_entry<true, false>(insert_level);
        auto entry_info = entry_lock_state.first;
        bool shared_lock = entry_lock_state.second;
        int_fast8_t entry_level = entry_info.level;
        if (graph_is_empty(entry_level)) {
            /* graph is empty, insert the first point */
            T id = store.template assign_vector_id<true>();
            init_range_elem<InsertContext>(ctx.ctx, id, ctx);
            add_first_basepoint();
            store.add_vector(distancer, id, ctx.query);
            store.set_entrypoint(id, id, 0);
            ctx.result_id = id;
            store.release_entry_lock(shared_lock);
            return;
        }

        /* search to insert_level */
        float dist = get_distance(ctx.query, entry_info.id);
        Vec<Cand> ep(1);
        ep.emplace_back(entry_info.id, entry_info.cur_layer_idx, dist);

        for (int_fast8_t l = entry_level; l > insert_level; --l) {
            search_upper_layer(ctx.query, ep);
            replace_lower_layer_idx(ep);
        }

        /* search to base layer */
        int_fast8_t search_level = bottom_only ? 0 : std::min(insert_level, entry_level);
        std::vector<Vec<Cand>> nbr_record(search_level); /* record nbr in search */
        
        for (int_fast8_t l = search_level; l > 0; --l) {
            ep = search_layer<false>(ctx.query, std::move(ep), ef_construction, dummy_filter);
            nbr_record[l - 1] = ep;
            replace_lower_layer_idx(ep);
        }
        ep = search_layer<true>(ctx.query, std::move(ep), ef_construction, dummy_filter);

        const auto release_and_destroy = [&]() -> void {
            store.release_entry_lock(shared_lock);
            ann_helper::optional_destroy(ep);
            for (int_fast8_t i = 0; i < search_level; ++i) {
                ann_helper::optional_destroy(nbr_record[i]);
            }
        };

        /* find duplicate in base layer */
        get_neighbors_data(ep);
        auto strat = apply_arrangement(ctx, ep, bottom_only);
        bool new_point_inserted = false;
        switch (strat) {
            case InsertStrategy::RetrySincePossibleInsert:
                release_and_destroy();
                retried = true;
                goto retry;
            case InsertStrategy::Trivial:
                release_and_destroy();
                break;
            case InsertStrategy::InsertPoint: {
                if (bottom_only && unlikely(entry_level < insert_level)) {
                    release_and_destroy();
                    retried = true;
                    goto retry;
                }
                Span<Vec<Cand>> nbr_span{nbr_record.data(), (size_t)search_level};
                auto [id, cur_layer_idx] = insert_new_point(ctx, std::move(ep), nbr_span);
                update_entry(id, cur_layer_idx, entry_level, insert_level);
                store.release_entry_lock(shared_lock);
            } break;
            default:
                break;
        }
    }

    GraphIndexEntryInfo get_entry_with_exclusive_lock()
        { return store.template get_entry<true>(GRAPH_INDEX_MAX_LEVEL + 1).first; }

    void release_exclusive_lock() { store.release_entry_lock(false); }

    void repair_entry(const UnorderedSet<size_t> &deleted)
    {
        /* acquire exclusive lock forcefully */
        GraphIndexEntryInfo entry_info = get_entry_with_exclusive_lock();
        if (!deleted.contains(entry_info.id)) {
            release_exclusive_lock();
            return;
        }
        size_t new_entry_id = INVALID_VECTOR_ID;
        size_t new_entry_cur_layer_idx = INVALID_VECTOR_ID;
        int8 new_entry_level = -1;
        auto vecbuf = store.read_data((T)entry_info.id);
        const char *query = vecbuf.get_vecbuf();
        Vec<Cand> ep(1);
        ep.emplace_back((T)entry_info.id, (T)entry_info.cur_layer_idx, 0.0f);
        for (int8 l = entry_info.level; l > 0; --l) {
            ep = search_layer<false>(query, std::move(ep), 1, [&](T id) -> bool {
                return id != (T)entry_info.id && !deleted.contains(id);
            });
            if (!ep.empty()) {
                const Cand &new_entry = ep[0];
                new_entry_id = new_entry.id;
                new_entry_cur_layer_idx = new_entry.cur_layer_idx;
                new_entry_level = l;
                break;
            }
            /* current layer is empty, cannot find any existing point */
            auto lower_layer_idx = std::get<1>(store.template get_point_info<false>((T)entry_info.cur_layer_idx));
            ep.emplace_back((T)entry_info.id, lower_layer_idx, 0.0f);
        }
        if (!is_valid(new_entry_id)) {
            ep = search_layer<true>(query, std::move(ep), 1, [&](T id) -> bool {
                return id != (T)entry_info.id && !deleted.contains(id);
            });
            if (!ep.empty()) {
                const Cand &new_entry = ep[0];
                new_entry_id = new_entry.id;
                new_entry_cur_layer_idx = new_entry.cur_layer_idx;
                new_entry_level = 0;
            }
        }
        vecbuf.release();
        store.set_entrypoint(new_entry_id, new_entry_cur_layer_idx, new_entry_level);
        release_exclusive_lock();
    }

    void init_dist_cache() { dist_cache.emplace(); }

    tuple<size_t, size_t, size_t, size_t> get_repair_info()
    {
        return {store.base_layer.size(), store.upper_layer.size(),
                store.base_layer.n_data_per_block(), store.upper_layer.n_data_per_block()};
    }

    void repair_basepoint(T id, const UnorderedSet<size_t> &deleted)
    {
        auto neighbors_id = std::get<0>(store.template get_point_info<true>(id));
        uint16 nbr_num = m * 2;
        if (!need_update(deleted, neighbors_id, nbr_num)) {
            return;
        }

        Vec<Cand> ep(ef_construction);
        ep.emplace_back(id, id, 0);
        auto vecbuf = store.read_data(id);
        const char *query = vecbuf.get_vecbuf();
        ep = search_layer<true>(query, std::move(ep), ef_construction, [&](T check_id) -> bool {
            return id != check_id && !deleted.contains(check_id);
        });
        get_neighbors_data(ep);
        Vec<Cand> new_neighbors = select_neighbors<true>(std::move(ep));
        std::vector<T> new_neighbors_id(nbr_num); /* nbr_num = m*2, max 256 */
        for (uint16 i = 0; i < nbr_num; ++i) {
            assert(new_neighbors[i].id != id);
            new_neighbors_id[i] = new_neighbors[i].id;
        }
        set_base_neighbors(id, new_neighbors_id.data());
        update_reverse_edges<true>(std::move(new_neighbors), query, id, id);
        vecbuf.release();
        dist_cache->clear();
    }

    void repair_upperpoint(T cur_layer_idx, const UnorderedSet<size_t> &deleted)
    {
        auto [neighbors_info, unused, id] = store.template get_point_info<false>(cur_layer_idx);
        (void)unused;
        if (deleted.contains((size_t)id)) {
            return;
        }
        uint16 nbr_num = m;
        if (!need_update(deleted, neighbors_info, nbr_num)) {
            return;
        }

        Vec<Cand> ep(ef_construction);
        ep.emplace_back(id, cur_layer_idx, 0);
        auto vecbuf = store.read_data(id);
        const char *query = vecbuf.get_vecbuf();
        ep = search_layer<false>(query, std::move(ep), ef_construction, [&](T check_id) -> bool {
            return id != check_id && !deleted.contains(check_id);
        });
        get_neighbors_data(ep);
        Vec<Cand> new_neighbors = select_neighbors<false>(std::move(ep));
        std::vector<T> new_neighbors_info(nbr_num * 2); /* nbr_num = m, max 256 total */
        T *new_neighbors_id = new_neighbors_info.data();
        T *new_neighobrs_cur_layer_idx = new_neighbors_info.data() + m;
        for (uint16 i = 0; i < nbr_num; ++i) {
            assert(new_neighbors[i].id != id);
            assert(!(new_neighbors[i].id != (T)INVALID_VECTOR_ID && new_neighbors[i].cur_layer_idx == (T)INVALID_VECTOR_ID));
            new_neighbors_id[i] = new_neighbors[i].id;
            new_neighobrs_cur_layer_idx[i] = new_neighbors[i].cur_layer_idx;
        }
        store.set_upper_neighbors(cur_layer_idx, new_neighbors_info.data());
        update_reverse_edges<false>(std::move(new_neighbors), query, id, cur_layer_idx);
        vecbuf.release();
        dist_cache->clear();
    }

private:
    static bool graph_is_empty(int_fast8_t entry_level) { return unlikely(entry_level == -1); }
    static bool is_valid(T id) { return likely(id != (T)INVALID_VECTOR_ID); }
    template <bool is_base_layer> uint_fast16_t get_nbr_num() const
        { return is_base_layer ? m * 2 : m; }

    template <bool estimate = false>
    float get_distance(const char *query, T id)
    {
        CONSTEXPR_IF (estimate) {
            return store.get_distance_est(distancer, query, id);
        } else {
            return store.get_distance(distancer, query, id);
        }
    }

    float get_distance(const char *query, const char *val)
        { return store.get_distance(distancer, query, val); }

    float get_distance_precise(const char *query, const char *val)
        { return store.get_distance_precise(distancer, query, val); }

    void update_entry(T id, T cur_layer_idx, int_fast8_t entry_level, int_fast8_t insert_level) 
    {
        if (unlikely(entry_level < insert_level)) {
            do {
                ++entry_level;
                T lower_layer_idx = cur_layer_idx;
                cur_layer_idx = store.template assign_vector_id<false>();
                add_first_upperpoint(cur_layer_idx, lower_layer_idx, id);
            } while (entry_level < insert_level);
            store.set_entrypoint(id, cur_layer_idx, insert_level);
        }
    }

    enum class InsertStrategy {
        Trivial,        /* inserted to point and do nothing */
        InsertPoint,    /* treat the insert data as a new point */
        UpdateCenter,   /* inserted to point and the center is updated, old points need to migrate */
            /**
             * In this case, we keep the center neighbors unchanged as the update will consider
             * them to keep the center being the center of these neighbors, guarenteed by P_local penalty.
             * Even that may not hold (due to relative distribution penalty),
             * the problem can only be temporal until vacuum, split or merge. We dont really think of
             * a scenario that a majority number of this temporal cases occuring simutanously.
             * 
             * It should return a vector of old points in this center to be redistributed.
             * There are two things need to be taken special care:
             *  concurrency: old points cannot be instantly removed, we can only remove it after it
             *    can be seen in other centers. So setting a under removing flag is necessary
             *    (prevent others from operations).
             *  completeness: if we are interrupted during redistribution, the old data is not removed
             *    and the new duplicates in other centers are okay to be ignored. The only thing
             *    should be considered is the under removal flag. There is no solution recovering
             *    writen inplace flag that can be both trivial and non-risky. So the flag should be
             *    preferably stored rather outside the shared buffer but in running memory.
             */
        UpdateAndInsert,/* the point split and we need to handle two points now */
            /**
             * Currently, we don't think about moving points. So it's basically update the existing
             * center with one point and insert a center with a new point. And the reassignment
             * needs to be done on both of them.
             */
        RetrySincePossibleInsert,   /* go retry */
            /**
             * As non-trivial operations may introduce new points insertion with current
             *  candidates, and graph cluster assume searching bottom by default. Thus for any
             *  possible cases that may trigger insertions need a rerun with a full candiate pruning
             *  and entry lock handling.
             */
        /* currently we only deal with insert, so merge operation is neither needed nor reachable */
    };

    template <typename TidHolder>
    static bool insert_range_tid(PointExtensionContext &ctx, Span<TidHolder> data, point_type &elem)
    {
        std::vector<typename point_type::Data> temp(data.size());
        for (size_t i = 0; i < data.size(); ++i) {
            temp[i] = data[i].tid;
        }
        return elem.insert_tid(ctx, {temp.data(), data.size()});
    }
    template <typename TidHolder>
    void init_range_elem(PointExtensionContext &ctx, T id, Span<TidHolder> data)
    {
        std::vector<typename point_type::Data> temp(data.size());
        for (size_t i = 0; i < data.size(); ++i) {
            temp[i] = data[i].tid;
        }
        Span<const typename point_type::Data> tids(temp.data(), data.size());
        store.add_elem(ctx, id, tids);
    }

    InsertStrategy apply_arrangement(InsertContext &ctx, Span<Cand> ep, bool need_retry)
    {
        for (const auto &p : ep) {
            /* ep is sorted in order, only need to compare the nearest point */
            if (likely(memcmp(p.val, ctx.query, store.get_elemsize()) != 0)) {
                break;
            }
            if (store.apply_elem(p.id, [&](point_type &pt) -> bool {
                /* skip elem under vacuum */
                if (pt.empty()) {
                    return false;
                }
                return insert_range_tid<InsertContext>(ctx.ctx, ctx, pt);
            })) {
                ctx.result_id = p.id;
                return InsertStrategy::Trivial;
            }
        }
        return InsertStrategy::InsertPoint;
    }

    Pair<T, T> insert_new_point(InsertContext &ctx, Vec<Cand> &&ep, Span<Vec<Cand>> nbr_record)
    {
        int_fast8_t search_level = (int_fast8_t)nbr_record.size();
        T id = store.template assign_vector_id<true>();
        ctx.result_id = id;
        init_range_elem<InsertContext>(ctx.ctx, id, ctx);
        CONSTEXPR_IF (use_dist_cache) {
            dist_cache.emplace((m + ef_construction) * (1 + search_level));
        }
        /* append the element from base layer to upper layer */
        T cur_layer_idx = id;
        T lower_layer_idx = (T)INVALID_VECTOR_ID;
        Vec<Cand> base_neighbors = select_neighbors<true>(std::move(ep));
        add_basepoint(id, base_neighbors);
        store.add_vector(distancer, id, ctx.query);
        update_reverse_edges<true>(std::move(base_neighbors), ctx.query, id, cur_layer_idx);
        for (int_fast8_t l = 1; l <= search_level; ++l) {
            lower_layer_idx = cur_layer_idx;
            cur_layer_idx = store.template assign_vector_id<false>();
            get_neighbors_data(nbr_record[l - 1]);
            Vec<Cand> upper_neighbors = select_neighbors<false>(std::move(nbr_record[l - 1]));
            add_upperpoint(cur_layer_idx, lower_layer_idx, id, upper_neighbors);
            update_reverse_edges<false>(std::move(upper_neighbors), ctx.query, id, cur_layer_idx);
        }
        CONSTEXPR_IF (use_dist_cache) {
            ann_helper::optional_destroy(*dist_cache);
        }
        return {id, cur_layer_idx};
    }

    /* ef == 1 */
    void search_upper_layer(const char *query, Vec<Cand> &entrypoint)
    {
        assert(entrypoint.size() == 1);
        Cand &cur_point = entrypoint[0];
        USet<T> visited(m * 2);
        Vec<T> nbr_id(m);
        Vec<T> nbr_cur_layer_idx(m);
        float *dists = (float *)Alloc<char>().allocate(sizeof(float) * m);
        float closest_dist = FLT_MAX;
        bool converged;
        do {
            converged = true;
            T lock_point_cur_layer_idx = cur_point.cur_layer_idx;
            store.template lock_point<false, true>(lock_point_cur_layer_idx);
            auto [neighbors_id, lower_layer_idx, unused] = store.template get_point_info<false>(cur_point.cur_layer_idx); 
            (void)unused;
            T *neighbors_cur_layer_idx = neighbors_id + m;
            cur_point.lower_layer_idx = lower_layer_idx;

            nbr_id.clear();
            nbr_cur_layer_idx.clear();
            for (uint_fast16_t i = 0; i < m; ++i) {
                T id = neighbors_id[i];
                T cur_layer_idx = neighbors_cur_layer_idx[i];
                if (!is_valid(id)) {
                    break;
                }
                if (!visited.insert(id).second) {
                    continue;
                }
                assert(is_valid(cur_layer_idx));
                nbr_id.push_back(id);
                nbr_cur_layer_idx.push_back(cur_layer_idx);
            }
            store.get_distance_batch(distancer, query, nbr_id, dists);
            for (size_t i = 0; i < nbr_id.size(); ++i) {
                float dist = dists[i];
                if (dist < closest_dist) {
                    closest_dist = dist;
                    converged = false;
                    cur_point.id = nbr_id[i];
                    cur_point.cur_layer_idx = nbr_cur_layer_idx[i];
                }
            }
            store.template unlock_point<false, true>(lock_point_cur_layer_idx);
        } while (!converged);
        cur_point.dist = closest_dist != FLT_MAX ? closest_dist : cur_point.dist;
        Alloc<char>().deallocate((char*)dists, sizeof(float) * m);
        ann_helper::optional_destroy(nbr_cur_layer_idx);
        ann_helper::optional_destroy(nbr_id);
        ann_helper::optional_destroy(visited);
    }

    template <bool is_base_layer, typename filter_func>
    Vec<Cand> search_layer(const char *query, Vec<Cand> &&entrypoint, uint32 ef,
                              filter_func &&filter)
    {
        uint_fast16_t nbr_num = get_nbr_num<is_base_layer>();
        fpq furthest(ef + 1);
        USet<T> visited(m * ef * 2);
        for (const Cand &cand : entrypoint) {
            visited.insert(cand.id);
        }
        cpq closest(std::move(entrypoint), true);
        Vec<T> nbr_id(nbr_num);
        Vec<T> nbr_cur_layer_idx(nbr_num);
        float *dists = (float *)Alloc<char>().allocate(sizeof(float) * nbr_num);
        while (!closest.empty()) {
            Cand cur_point = closest.top();
            closest.pop();
            if (furthest.size() == ef && cur_point.dist > furthest.top().dist) {
                /* this judgment can ensure `cur_point` must in furthest */
                break;
            }

            store.template lock_point<is_base_layer, true>(cur_point.cur_layer_idx);
            auto [neighbors_id, lower_layer_idx, unused] =
                store.template get_point_info<is_base_layer>(cur_point.cur_layer_idx);
            (void)unused;
            T *neighbors_cur_layer_idx = is_base_layer ? neighbors_id : neighbors_id + nbr_num;

            nbr_id.clear();
            nbr_cur_layer_idx.clear();
            for (uint_fast16_t i = 0; i < nbr_num; ++i) {
                T id = neighbors_id[i];
                if (!is_valid(id)) {
                    break;
                }
                if (!visited.insert(id).second) {
                    continue;
                }
                nbr_id.push_back(id);
                nbr_cur_layer_idx.push_back(neighbors_cur_layer_idx[i]);
            }

            store.get_distance_batch(distancer, query, nbr_id, dists);
            const float threshold = furthest.size() < ef ? FLT_MAX : furthest.top().dist;
            for (size_t i = 0; i < nbr_id.size(); ++i) {
                float dist = dists[i];
                if (dist >= threshold) {
                    continue;
                }
                closest.emplace(nbr_id[i], nbr_cur_layer_idx[i], dist);
            }
            if (!filter(cur_point.id)) {
                store.template unlock_point<is_base_layer, true>(cur_point.cur_layer_idx);
                continue;
            }
            furthest.emplace(cur_point.id, cur_point.cur_layer_idx, lower_layer_idx, cur_point.dist, nullptr);
            if (furthest.size() > ef) {
                furthest.pop();
            }
            store.template unlock_point<is_base_layer, true>(cur_point.cur_layer_idx);
        }
        furthest.sort();

        Alloc<char>().deallocate((char*)dists, sizeof(float) * nbr_num);
        ann_helper::optional_destroy(nbr_cur_layer_idx);
        ann_helper::optional_destroy(nbr_id);
        ann_helper::optional_destroy(closest);
        ann_helper::optional_destroy(visited);
        return std::move(furthest).data();
    }

    auto get_norm_func() const
    {
        return OidIsValid(index_getprocid(store.get_index(), 1, GRAPH_INDEX_NORM_PROC))
            ? ann_helper::get_vector_preprocess_func(Metric::FAST_COSINE, store.get_precision(), store.get_dim())
            : nullptr;
    }

    void refine(PointExtensionContext &ctx, Vec<Cand> &candidates, const char *query) {
        CONSTEXPR_IF (need_refine) {
            const auto norm_func = get_norm_func();
            char *vec = alloc_vector(store.get_vecsize());
            for (Cand &point : candidates) {
                if (store.fetch_vec_from_heap(ctx, point.id, vec)) {
                    if (norm_func) {
                        norm_func(vec, store.get_dim(), vec);
                    }
                    point.dist = get_distance_precise(query, vec);
                } else {
                    point.dist = INVALID_DIST;
                }
            }
            free_vector(vec);
            std::sort(candidates.begin(), candidates.end(), [](const Cand &a, const Cand &b) -> bool {
                return a.dist < b.dist;
            });
        }
    }

    struct PruneNeighbor {
        const char *val;
        uint_fast16_t idx;
        float dist;
        T id;
        PruneNeighbor(const char *v, uint_fast16_t i, float d, T id) : val(v), idx(i), dist(d), id(id) {}
        bool operator<(const PruneNeighbor &other) const
            { return dist < other.dist || (dist == other.dist && id < other.id); }
    };

    float get_distance(const Cand &a, const Cand &b)
    {
        CONSTEXPR_IF (!use_dist_cache) {
            return get_distance(a.val, b.val);
        } else {
            auto [it, inserted] = dist_cache->try_emplace(Pair<T, T>(a.id, b.id), 0);
            if (inserted) {
                it->second = get_distance(a.val, b.val);
            }
            return it->second;
        }
    }

    float get_distance(const PruneNeighbor &a, const PruneNeighbor &b)
    {
        CONSTEXPR_IF (!use_dist_cache) {
            return get_distance(a.val, b.val);
        } else {
            auto [it, inserted] = dist_cache->try_emplace(Pair<T, T>(a.id, b.id), 0);
            if (inserted) {
                it->second = get_distance(a.val, b.val);
            }
            return it->second;
        }
    }

    /* forward: new_point -> neighbors, select `m/2m` neighbors from current candidate */
    template <bool is_base_layer>
    Vec<Cand> select_neighbors(Vec<Cand> &&c, bool sorted = true)
    {
        uint_fast16_t nbr_num = get_nbr_num<is_base_layer>();
        Vec<Cand> r(nbr_num);
        if (unlikely(c.size() <= nbr_num)) {
            r.push_back(c.cbegin(), c.cend());
            r.resize(nbr_num);
            ann_helper::optional_destroy(c);
            return r;
        }

        Vec<Cand> discarded(nbr_num); /* since closest pop in order, discarded is in order naturally */
        cpq closest(std::move(c), sorted);

        while (closest.size() > 0 && r.size() < nbr_num) {
            const Cand &cur_point = closest.top();
            bool cur_point_is_closer = true;
            for (const Cand &ri : r) {
                if (get_distance(cur_point, ri) <= cur_point.dist) {
                    cur_point_is_closer = false;
                    break;
                }
            }
            if (cur_point_is_closer) {
                r.push_back(cur_point);
            } else {
                discarded.push_back(cur_point);
            }
            closest.pop();
        }

        /* always keep pruned */
        for (const auto &d : discarded) {
            if (r.size() >= nbr_num) {
                break;
            }
            r.emplace_back(d);
        }

        ann_helper::optional_destroy(discarded);
        ann_helper::optional_destroy(closest);
        return r;
    }

    /* backward: neighbors -> new_point, select one neighbor which will be replaced */
    template <bool is_base_layer, bool check_exist = false>
    int16 select_neighbors(Vec<Cand> &&c, T new_point_id, BitSpan<uint> stat, const Cand &self,
                           const char *query)
    {
        int16 pruned = -1;
        CONSTEXPR_IF (check_exist) {
            for (const auto &can : c) {
                if (can.id == new_point_id) {
                    ann_helper::optional_destroy(c);
                    return pruned;
                }
            }
        }

        uint_fast16_t nbr_num = get_nbr_num<is_base_layer>();
        if (c.size() < nbr_num) {
            pruned = c.size();
            ann_helper::optional_destroy(c);
            return pruned;
        }

        Vec<PruneNeighbor> r(nbr_num);
        Vec<PruneNeighbor> discarded(nbr_num); /* since closest pop in order, discarded is in order naturally */
        PriorityQueue<PruneNeighbor, std::less<PruneNeighbor>, Alloc<PruneNeighbor>> closest(nbr_num + 1);

        get_neighbors_data(c);
        const_cast<Cand &>(self).val = store.get_data(self.id);
        for (uint_fast16_t i = 0; i < c.size(); ++i) {
            const Cand &nbr = c[i];
            assert(nbr.id != self.id);
            float dist;
            CONSTEXPR_IF (!use_dist_cache) {
                dist = nbr.dist;
            } else {
                dist = get_distance(self, nbr);
            }
            closest.emplace(nbr.val, i, dist, nbr.id);
        }
        /*
         * DiskStore caches pair distances by candidate IDs.  The candidate
         * below contains the new point's vector, so its cache identity must
         * also be new_point_id.  Reusing self.id aliases it with the existing
         * reverse-edge target: a cached distance between self and one of its
         * old neighbors is then incorrectly reused as the distance from the
         * new point to that neighbor.  MemStore did not expose this because
         * its stats path deliberately uses self.id only as a marker and does
         * not enable the pair-distance cache.
         */
        const T inserted_candidate_id = use_dist_cache ? new_point_id : self.id;
        closest.emplace(store.get_data(new_point_id), (T)INVALID_VECTOR_ID,
                        self.dist, inserted_candidate_id);

        const auto elem_closer = [&](const Vec<PruneNeighbor> &set, const PruneNeighbor &p) -> bool {
            for (const PruneNeighbor &ri : set) {
                if (get_distance(p, ri) <= p.dist) {
                    return false;
                }
            }
            return true;
        };
        Holder<Vec<PruneNeighbor>> added;
        CONSTEXPR_IF (!use_dist_cache) {
            added.emplace(nbr_num);
        }
        bool has_stats = store.has_stat(stat);
        store.set_stat(stat);
        bool has_remove = false;
        bool closer;
        do {
#ifndef NDEBUG
            for (size_t _i = 0; _i < r.size(); ++_i) {
                assert(r[_i].val != nullptr);
            }
#endif
            const PruneNeighbor &cur_point = closest.top();
            bool cur_point_is_closer;
            CONSTEXPR_IF (use_dist_cache) {
                cur_point_is_closer = elem_closer(r, cur_point);
            } else {
                if (!has_stats) {
                    cur_point_is_closer = elem_closer(r, cur_point);
                    if (cur_point.id != self.id) {
                        stat.set(cur_point.idx, cur_point_is_closer);
                    } else {
                        closer = cur_point_is_closer;
                    }
                } else if (!added->empty()) {
                    cur_point_is_closer = stat.get(cur_point.idx);
                    if (cur_point_is_closer) {
                        cur_point_is_closer = elem_closer(*added, cur_point);
                        if (!cur_point_is_closer) {
                            has_remove = true;
                            stat.set(cur_point.idx);
                        }
                    } else if (has_remove) {
                        cur_point_is_closer = elem_closer(r, cur_point);
                        if (cur_point_is_closer) {
                            added->push_back(cur_point);
                            stat.set(cur_point.idx);
                        }
                    }
                } else if (cur_point.id == self.id) {
                    cur_point_is_closer = elem_closer(r, cur_point);
                    if (cur_point_is_closer) {
                        added->push_back(cur_point);
                    }
                    closer = cur_point_is_closer;
                } else {
                    cur_point_is_closer = stat.get(cur_point.idx);
                }
            }

            if (cur_point_is_closer) {
                r.push_back(cur_point);
                assert(r.back().val != nullptr);
                assert(r.back().id != INVALID_VECTOR_ID || cur_point.id == self.id);
                if (r.size() >= nbr_num) {
                    break;
                }
            } else {
                discarded.push_back(cur_point);
            }
            closest.pop();
        } while (!closest.empty());

        /* always keep pruned, no need to emplace r actually  */
        size_t discarded_idx = 0;
        size_t r_size = r.size();
        while (discarded_idx < discarded.size() && r_size < nbr_num) {
            ++discarded_idx;
            ++r_size;
        }

        /* choose pruned - return the "least worth keeping" candidate index */
        if (discarded_idx < discarded.size()) {
            /* return the first discarded element that wasn't backfilled */
            pruned = discarded[discarded_idx].idx;
        } else if (closest.size() > 0) {
            /* no elements were discarded, return the furthest unprocessed candidate */
            while (closest.size() > 1) {
                closest.pop();
            }
            pruned = closest.top().idx;
        }

        CONSTEXPR_IF (!use_dist_cache) {
            if (pruned >= 0) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
                stat.set(pruned, closer);
#pragma GCC diagnostic pop
            }
            ann_helper::optional_destroy(*added);
        }
        ann_helper::optional_destroy(c);
        ann_helper::optional_destroy(r);
        ann_helper::optional_destroy(discarded);
        ann_helper::optional_destroy(closest);
        return pruned;
    }

    bool need_update(const UnorderedSet<size_t> &deleted, const T *neighbors_id, uint16 nbr_num)
    {
        for (uint16 i = 0; i < nbr_num && is_valid(neighbors_id[i]); ++i) {
            if (deleted.contains((size_t)neighbors_id[i])) {
                return true;
            }
        }
        return false;
    }

    template <bool is_base_layer, bool check_exist = false>
    void update_reverse_edges(Vec<Cand> &&neighbors, const char *query, T newpoint_id,
                              T newpoint_cur_layer_idx)
    {
        for (const Cand &nbr : neighbors) {
            if (!is_valid(nbr.id)) {
                break;
            }
            Vec<Cand> r;
            store.template lock_point<is_base_layer, false>(nbr.cur_layer_idx);
            store.template get_neighbors<is_base_layer>(r, nbr);
            auto p = store.template get_neighbor_stats<is_base_layer>(nbr.cur_layer_idx);
            int16 pruned = select_neighbors<is_base_layer, check_exist>(std::move(r), newpoint_id, p.second, nbr, query);
            if (pruned >= 0) {
                store.template set_neighbor<is_base_layer>(nbr.cur_layer_idx, pruned, newpoint_id,
                                                           newpoint_cur_layer_idx);
                CONSTEXPR_IF (!use_dist_cache) {
                    p.first[pruned] = nbr.dist;
                }
            }
            store.template unlock_point<is_base_layer, false>(nbr.cur_layer_idx);
        }
        ann_helper::optional_destroy(neighbors);
    }

    void replace_lower_layer_idx(Vec<Cand> &c)
    {
        for (Cand &point : c) {
            point.cur_layer_idx = point.lower_layer_idx;
            point.lower_layer_idx = (T)INVALID_VECTOR_ID;
        }
    }

    void get_neighbors_data(Vec<Cand> &c)
    {
        store.reset_neighbors_val_pool();
        for (Cand &point : c) {
            point.val = store.get_data(point.id);
        }
    }

    void add_upperpoint(T cur_layer_idx, T lower_layer_idx, T id, const Vec<Cand> &neighbors)
    {
        uint_fast16_t nbr_num = m;
        std::vector<T> neighbors_info(nbr_num * 2); /* nbr_num = m, max 256 total */
        T *neighbors_id = neighbors_info.data();
        T *neighbors_cur_layer_idx = neighbors_id + nbr_num;
        for (uint_fast16_t i = 0; i < nbr_num; ++i) {
            assert(neighbors[i].cur_layer_idx != cur_layer_idx);
            neighbors_id[i] = neighbors[i].id;
            neighbors_cur_layer_idx[i] = neighbors[i].cur_layer_idx;
        }
        store.add_upperpoint(cur_layer_idx, lower_layer_idx, id, neighbors_info.data());
        CONSTEXPR_IF (!use_dist_cache) {
            float *dists = store.template get_neighbor_stats<false>(cur_layer_idx).first;
            for (uint_fast16_t i = 0; i < nbr_num; ++i) {
                dists[i] = neighbors[i].dist;
            }
        }
    }

    void add_basepoint(T id, const Vec<Cand> &neighbors)
    {
        uint_fast16_t nbr_num = m * 2;
        std::vector<T> neighbors_id(nbr_num); /* nbr_num = m*2, max 256 */
        for (uint_fast16_t i = 0; i < nbr_num; ++i) {
            assert(neighbors[i].id != id);
            neighbors_id[i] = neighbors[i].id;
        }
        store.add_basepoint(id, neighbors_id.data());
        CONSTEXPR_IF (!use_dist_cache) {
            float *dists = store.template get_neighbor_stats<true>(id).first;
            for (uint_fast16_t i = 0; i < nbr_num; ++i) {
                dists[i] = neighbors[i].dist;
            }
        }
    }

    void add_first_basepoint()
    {
        uint_fast16_t nbr_num = m * 2;
        std::vector<T> neighbors_id(nbr_num, (T)INVALID_VECTOR_ID); /* nbr_num = m*2, max 256 */
        store.add_basepoint(0, neighbors_id.data());
    }

    void add_first_upperpoint(T cur_idx, T lower_layer_idx, T id)
    {
        std::vector<T> neighbors_info(m * 2, (T)INVALID_VECTOR_ID); /* m*2, max 256 */
        store.add_upperpoint(cur_idx, lower_layer_idx, id, neighbors_info.data());
    }

    void set_base_neighbors(T id, T *neighbors_id) { store.set_base_neighbors(id, neighbors_id); }

    void set_base_neighbors(T id, const Vec<Cand> &neighbors)
    {
        uint_fast16_t nbr_num = m * 2;
        std::vector<T> neighbors_id(nbr_num); /* nbr_num = m*2, max 256 */
        for (uint_fast16_t i = 0; i < nbr_num; ++i) {
            assert(neighbors[i].id != id);
            neighbors_id[i] = neighbors[i].id;
        }
        store.set_base_neighbors(id, neighbors_id.data());
    }

    struct DummyFilter {
        template <typename ...Args>
        constexpr bool operator()(Args &&...) const { return true; }
    };
    /**
     * `constexpr` to make it initialized at compile time,
     *  it does not have to be compile time, but it has to be declared like this to
     *  be initialized in header file rather than mannualy make it in every constructor
     * `inline` to resolve linker errors brought by `static`
     */
    static constexpr DummyFilter dummy_filter = {};
};

#endif /* GRAPH_INDEX_ALGORITHM_H */
