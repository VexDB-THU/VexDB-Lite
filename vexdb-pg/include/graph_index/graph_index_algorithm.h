/**
 * Copyright ...
 */

#ifndef GRAPH_INDEX_ALGORITHM_H
#define GRAPH_INDEX_ALGORITHM_H

#include <cmath>
#include <cfloat>
#include <numeric>      /* accumulate */
#include <algorithm>    /* max_element */
#include <type_traits>
#include <vtl/bitvector>
#include <vtl/priority_queue>
#include <vtl/hashtable>
#include <vtl/holder>
#include <vtl/span>

#include "postgres.h"
#include "knl/knl_variable.h"
#include "module/timer.h"
#include "graph_index/graph_index_cluster.h"
#include "graph_index/graph_index_storage.h"
#include "vector_smgr.h"
#include "distance/distance.h"

template <typename Store, typename Distancer>
class GraphIndexAlgorithm {
    using T = typename Store::T;
    using point_type = typename Store::point_type;
    using Cand = GraphIndexCandidate<T>;
    static constexpr bool need_refine = Distancer::need_refine &&
        !std::is_same<Store, MemStore<T, point_type>>::value;
    static constexpr bool has_est = Distancer::has_estimation_func &&
        !std::is_same<Store, MemStore<T, point_type>>::value;
    /**
     * For cohere1m 100k insertion from empty index case,
     * using dist cache reduce calculation count from 3432602848 to 1959123890, and
     * read vector count from 698463845 to 579761710.
     */
    static constexpr bool use_dist_cache = !Store::has_occlusion_cache;
    static constexpr bool clustered = Store::clustered;
    template <typename D, typename = void>
    struct has_cluster_maint_traits : std::false_type {};

    template <typename D>
    struct has_cluster_maint_traits<D,
        std::void_t<
            decltype(D::dpt),
            typename D::template transform_type<TransformOp::ADD>,
            typename D::template transform_type<TransformOp::MUL_SCALAR>>>
        : std::true_type {};

    static constexpr bool distancer_supports_cluster_maint =
        has_cluster_maint_traits<Distancer>::value;

    template <typename D, typename = void>
    struct has_compute_code_traits : std::false_type {};

    template <typename D>
    struct has_compute_code_traits<D,
        std::void_t<decltype(std::declval<D &>().compute_code((float *)nullptr, (char *)nullptr))>> : std::true_type {};

    static constexpr bool distancer_supports_code_compute =
        has_compute_code_traits<Distancer>::value;

    void add_vector_for_store(T id, const char *query)
    {
        if constexpr (distancer_supports_code_compute) {
            store.add_vector(id, query, distancer);
        } else {
            store.add_vector(id, query);
        }
    }

    struct ClosestCompare {
        bool operator()(const Cand &a, const Cand &b) const {
            return a.dist < b.dist; /* smallest distance has highest priority (min-heap) */
        }
    };
    struct FurthestCompare {
        bool operator()(const Cand &a, const Cand &b) const {
            return a.dist > b.dist; /* largest distance has highest priority (max-heap) */
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
    using fpq = PriorityQueue<Cand, FurthestCompare>;
    using cpq = PriorityQueue<Cand, ClosestCompare>;
    uint_fast16_t ef_construction;
    uint_fast16_t m;
    Store &store;
    Distancer &distancer;
    Holder<UnorderedMap<Pair<T, T>, float, PairHasher, PairCmp>> dist_cache;
public:
    GraphIndexAlgorithm(uint_fast16_t ef_construction, uint_fast16_t m, Store &store, Distancer &distancer)
        : ef_construction(ef_construction),
          m(m),
          store(store),
          distancer(distancer),
          dist_cache() {}
    GraphIndexAlgorithm(const GraphIndexMetaPage metap, Store &store, Distancer &distancer)
        : GraphIndexAlgorithm(metap->ef_construction, metap->m, store, distancer) {}

    /* only disk_store has search() */
    Pair<Vector<ItemPointerData>, Vector<float>> search(PointExtensionContext &ctx,
        const char *query, uint_fast16_t ef_search)
    {
        CONSTEXPR_IF (need_refine) {
            constexpr float refine_factor = 1.25;
            ef_search *= refine_factor;
        }
        Pair<Vector<ItemPointerData>, Vector<float>> res{};

        auto [entry_info, shared_lock] = store.template get_entry<false>();
        if (entry_info.id == INVALID_VECTOR_ID || entry_info.level < 0) {
            return res;
        }
        Vector<Cand> ep(ef_search);
        ep.emplace_back((T)entry_info.id, (T)entry_info.cur_layer_idx, get_distance(query, entry_info.id));
        for (int_fast8_t l = entry_info.level; l > 0; --l) {
            search_upper_layer(query, ep);
            replace_lower_layer_idx(ep);
        }
        ep = search_layer<true>(query, std::move(ep), ef_search, dummy_filter);
        refine(ctx, ep, query);
        std::sort(ep.begin(), ep.end(), [](const Cand &a, const Cand &b) {
            return a.dist < b.dist || (a.dist == b.dist && a.id < b.id);
        });

        res.first.reserve(ep.size());
        res.second.reserve(ep.size());
        for (const Cand &cand : ep) {
            store.get_itempointer(cand.id, [&](const point_type *elem) -> void {
                uint32 ntid = elem->get_tids(res.first, ctx);
                for (uint32 i = 0; i < ntid; ++i) {
                    res.second.push_back(cand.dist);
                }
            });
        }
        ann_helper::optional_destroy(ep);
        return res;
    }

    Vector<Pair<T, float>> search_async_heap(const char *query, uint_fast16_t top_k)
    {
        fpq max_heap;
        store.for_each_async_id([&](T id) {
            float dist = get_distance(query, id);
            max_heap.emplace(id, (T)INVALID_VECTOR_ID, dist);
            if (max_heap.size() > top_k) {
                max_heap.pop();
            }
        });
        Vector<Pair<T, float>> results(max_heap.size());
        while (!max_heap.empty()) {
            auto &c = max_heap.top();
            results.emplace_back(c.id, c.dist);
            max_heap.pop();
        }
        std::reverse(results.begin(), results.end());
        return results;
    }

    Pair<Vector<ItemPointerData>, Vector<float>> search_with_async(
        PointExtensionContext &ctx, const char *query, uint_fast16_t ef_search)
    {
        /* Step 1: brute-force search on async pending data */
        auto async_results = search_async_heap(query, ef_search);

        /* Step 2: check if graph has data */
        bool graph_has_data;
        {
            auto [entry_info, shared_lock] = store.template get_entry<false>();
            graph_has_data = entry_info.level >= 0;
            store.release_entry_lock(shared_lock);
        }

        /* Step 3: if graph is empty, return async results only */
        if (!graph_has_data) {
            Vector<ItemPointerData> res;
            Vector<float> dists;
            for (size_t i = 0; i < async_results.size(); ++i) {
                auto &[id, dist] = async_results[i];
                store.get_itempointer(id, [&](const point_type *elem) -> void {
                    uint32 ntid = elem->get_tids(res, ctx);
                    for (uint32 j = 0; j < ntid; ++j) {
                        dists.push_back(dist);
                    }
                });
            }
            ann_helper::optional_destroy(async_results);
            return {res, dists};
        }

        /* Step 4: graph has data - normal graph search */
        auto [graph_res, graph_dists] = search(ctx, query, ef_search);

        /* Step 5: merge graph + async results */
        Vector<ItemPointerData> all_tids;
        Vector<float> all_dists;

        for (size_t i = 0; i < graph_res.size(); ++i) {
            all_tids.push_back(graph_res[i]);
            all_dists.push_back(graph_dists[i]);
        }

        for (size_t i = 0; i < async_results.size(); ++i) {
            auto &[id, dist] = async_results[i];
            store.get_itempointer(id, [&](const point_type *elem) -> void {
                uint32 ntid = elem->get_tids(all_tids, ctx);
                for (uint32 j = 0; j < ntid; ++j) {
                    all_dists.push_back(dist);
                }
            });
        }

        /* Sort by distance ascending using index vector */
        size_t n = all_tids.size();
        Vector<size_t> indices(n);
        for (size_t i = 0; i < n; ++i) {
            indices.push_back(i);
        }
        std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
            return all_dists[a] < all_dists[b];
        });

        Vector<ItemPointerData> res;
        Vector<float> dists;
        for (size_t i = 0; i < n; ++i) {
            res.push_back(all_tids[indices[i]]);
            dists.push_back(all_dists[indices[i]]);
        }

        ann_helper::optional_destroy(indices);
        ann_helper::optional_destroy(all_tids);
        ann_helper::optional_destroy(all_dists);
        ann_helper::optional_destroy(graph_res);
        ann_helper::optional_destroy(graph_dists);
        ann_helper::optional_destroy(async_results);

        return {res, dists};
    }

    struct InsertContextBase {
        InsertContextBase(PointExtensionContext &c, const char *q, const ItemPointer t)
            : ctx(c), query(q), tid(*t) {}
        PointExtensionContext &ctx;
        const char *query;
        const ItemPointerData tid;
        static constexpr void check_round() {}
        static constexpr bool round_full() { return true; }
        static constexpr void destroy() {}
    };
    struct InsertContextCluster : public InsertContextBase {
        using InsertContextBase::InsertContextBase;
        constexpr static uint16 max_round = 5;
        struct Data {
            T closest_id;
            ItemPointerData tid;
            const char *d;

            Data(T i, const ItemPointerData &t, const char *d)
                : closest_id(i), tid(t), d(d) {}
        };
        Optional<Vector<Data>> to_reallocated;
        Optional<Vector<Data>> new_point;
        uint16 nround{0};
        char *buffer[max_round] = {NULL};
        const char *new_point_d;
        void check_round() const { Assert(nround < max_round); }
        bool round_full() const { return nround >= max_round; }
        void destroy()
        {
            InsertContextBase::destroy();
            ann_helper::optional_destroy(to_reallocated);
            ann_helper::optional_destroy(new_point);
            for (uint16 i = 0; i < nround; ++i) {
                if (buffer[i]) {
                    free_vector(buffer[i]);
                    buffer[i] = NULL;
                }
            }
        }
    };
    using InsertContext = typename std::conditional<clustered, InsertContextCluster, InsertContextBase>::type;

    void async_insert(InsertContext &ctx)
    {
        T id = store.template assign_vector_id<true>();
        store.add_async_id(id);
        store.add_elem(ctx.ctx, id, ctx.tid);
        add_vector_for_store(id, ctx.query);
    }

    template <bool try_bottom_first = clustered>
    void insert(InsertContext &ctx)
    {
        ctx.check_round();
        bool retried = false;
        int_fast8_t insert_level = get_insert_level();
retry:
        const bool bottom_only = try_bottom_first && !retried;
        auto [entry_info, shared_lock] = bottom_only
            ? store.template get_entry<true, true>(insert_level)
            : store.template get_entry<true, false>(insert_level);
        int_fast8_t entry_level = entry_info.level;
        if (unlikely(entry_level == -1)) {
            /* graph is empty, insert the first point */
            store.template assign_vector_id<true>();
            store.add_elem(ctx.ctx, 0, ctx.tid);
            add_first_basepoint();
            add_vector_for_store(0, ctx.query);
            store.set_entrypoint(0, 0, 0);
            store.release_entry_lock(shared_lock);
            return;
        }

        /* search to insert_level */
        float dist = get_distance(ctx.query, entry_info.id);
        Vector<Cand> ep(1);
        ep.emplace_back(entry_info.id, entry_info.cur_layer_idx, dist);

        for (int_fast8_t l = entry_level; l > insert_level; --l) {
            search_upper_layer(ctx.query, ep);
            replace_lower_layer_idx(ep);
        }

        /* search to base layer */
        int_fast8_t search_level = bottom_only ? 0 : std::min(insert_level, entry_level);
        Vector<Cand> nbr_record[search_level]; /* record nbr in search */
        
        for (int_fast8_t l = search_level; l > 0; --l) {
            ep = search_layer<false>(ctx.query, std::move(ep), ef_construction, dummy_filter);
            nbr_record[l - 1] = ep;
            replace_lower_layer_idx(ep);
        }
        ep = search_layer<true>(ctx.query, std::move(ep), ef_construction, dummy_filter);

        const auto release_and_destroy = [&]() -> void {
            store.release_entry_lock(shared_lock);
            ann_helper::optional_destroy(ep);
            for (auto &nbr : nbr_record) {
                ann_helper::optional_destroy(nbr);
            }
        };

        const auto update_entry = [&](T id, T cur_layer_idx) -> void {
            if (unlikely(entry_level < insert_level)) {
                do {
                    ++entry_level;
                    T lower_layer_idx = cur_layer_idx;
                    cur_layer_idx = store.template assign_vector_id<false>();
                    add_first_upperpoint(cur_layer_idx, lower_layer_idx, id);
                } while (entry_level < insert_level);
                store.set_entrypoint(id, cur_layer_idx, insert_level);
            }
        };

        /* find duplicate in base layer */
        get_neighbors_data(ep);
        auto strat = apply_arrangement(ctx, ep, bottom_only);
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
                Span<Vector<Cand>> nbr_span{nbr_record, (size_t)search_level};
                auto [id, cur_layer_idx] = insert_new_point(ctx, std::move(ep), nbr_span);
                update_entry(id, cur_layer_idx);
                store.release_entry_lock(shared_lock);
            } break;
            case InsertStrategy::UpdateAndInsert:
                Assume(clustered);
                if constexpr (clustered) {
                    // TD
                    if (!ctx.to_reallocated) {
                        release_and_destroy();
                        break;
                    }
                }
                /* fall through */
            case InsertStrategy::UpdateCenter:
                Assume(clustered);
                static_assert(__cplusplus >= 201703L, "only compilable with c++17 or greater");
                if constexpr (clustered) {
                    ++ctx.nround;
                    using Data = typename InsertContextCluster::Data;
                    Vector<Data> outliers{std::move(ctx.to_reallocated).value()};
                    std::sort(outliers.begin(), outliers.end(),
                        [](const Data &a, const Data &b) -> bool {
                            return a.closest_id < b.closest_id;
                        });
                    for (size_t idx = 0; idx < outliers.size();) {
                        size_t end = idx + 1;
                        T insert_point = outliers[idx].closest_id;
                        for (; end < outliers.size(); ++end) {
                            if (outliers[end].closest_id != insert_point) {
                                break;
                            }
                        }
                        if (!is_valid(insert_point)) {
                            if (likely(entry_level >= insert_level)) {
                                Vector<Cand> temp_nbr[search_level];
                                for (int_fast8_t i = 0; i < search_level; ++i) {
                                    temp_nbr[i] = nbr_record[i];
                                }
                                auto [id, cur_layer_idx] = insert_new_point(ctx, Vector<Cand>(ep),
                                    Span<Vector<Cand>>{temp_nbr, (size_t)search_level});
                                update_entry(id, cur_layer_idx);
                                ++idx;
                            }
                            for (; idx < end; ++idx) {
                                ctx.query = outliers[idx].d;
                                insert<false>(ctx);
                            }
                        } else {
                            auto target = std::find_if(ep.begin() + 1, ep.end(),
                                [insert_point](const Cand &c) -> bool {
                                    return c.id == insert_point;
                                });
                            Assert(target != ep.end());
                            std::swap(ep.front(), *target);
                            ctx.query = outliers[idx].d;
                            Assert(!bottom_only);
                            auto istrat = apply_arrangement(ctx, ep, false);
                            std::swap(ep.front(), *target);
                            switch (istrat) {
                                case InsertStrategy::Trivial:
                                    break;
                                case InsertStrategy::InsertPoint:
                                    ctx.query = outliers[idx].d;
                                    insert<false>(ctx); // TD: reasoning about usage
                                    break;
                                case InsertStrategy::UpdateCenter:
                                    // TD
                                    break;
                                case InsertStrategy::UpdateAndInsert:
                                    // TD
                                    break;
                                case InsertStrategy::RetrySincePossibleInsert:
                                    Assume(false);
                            }
                            // TD
                            idx = end;
                        }
                    }
                    release_and_destroy();
                    UnorderedSet<ItemPointerData> deleted;
                    for (const auto &d : outliers) {
                        deleted.insert(d.tid);
                    }
                    ann_helper::optional_destroy(outliers);
                    store.apply_elem(ep.front().id, [&](GraphIndexCluster &elem) -> bool {
                        bool dirty;
                        elem.vacuum_tids([&](const ItemPointerData &tid) -> bool {
                            auto it = deleted.find(tid);
                            if (it != deleted.end()) {
                                deleted.erase(it);
                                return true;
                            }
                            return false;
                        }, ctx.ctx, dirty);
                        return dirty;
                    });
                    reset_under_redistrib(store.get_index(), ep.front().id);
                    ann_helper::optional_destroy(deleted);
                    /* we don't decrement nround, as 5 insertion is more reasonable than 5^5 */
                }
                break;
        }
    }

    void repair_entry(const UnorderedSet<size_t> &deleted)
    {
        /* acquire exclusive lock forcefully */
        auto [entry_info, shared_lock] = store.template get_entry<true>(GRAPH_INDEX_MAX_LEVEL + 1);
        if (!deleted.contains(entry_info.id)) {
            store.release_entry_lock(shared_lock);
            return;
        }
        size_t new_entry_id = INVALID_VECTOR_ID;
        size_t new_entry_cur_layer_idx = INVALID_VECTOR_ID;
        int8 new_entry_level = -1;
        char *query = store.get_data((T)entry_info.id);
        Vector<Cand> ep(1);
        ep.emplace_back((T)entry_info.id, (T)entry_info.cur_layer_idx, INVALID_VECTOR_ID, 0, nullptr);
        for (int8 l = entry_info.level; l > 0; --l) {
            ep = search_layer<false>(query, std::move(ep), 1, [&](T id) -> bool {
                return id != (T)entry_info.id && !deleted.contains(id);
            });
            if (!ep.empty()) {
                Cand new_entry = ep[0];
                new_entry_id = new_entry.id;
                new_entry_cur_layer_idx = new_entry.cur_layer_idx;
                new_entry_level = l;
                break;
            }
            /* current layer is empty, cannot find any existing point */
            auto [unused, lower_layer_idx, unused1] = store.template get_point_info<false>((T)entry_info.cur_layer_idx);
            ep.emplace_back((T)entry_info.id, lower_layer_idx, INVALID_VECTOR_ID, 0, nullptr);
        }
        if (!is_valid(new_entry_id)) {
            ep = search_layer<true>(query, std::move(ep), 1, [&](T id) -> bool {
                return id != (T)entry_info.id && !deleted.contains(id);
            });
            if (!ep.empty()) {
                new_entry_id = ep[0].id;
                new_entry_cur_layer_idx = ep[0].cur_layer_idx;
                new_entry_level = 0;
            }
        }
        store.set_entrypoint(new_entry_id, new_entry_cur_layer_idx, new_entry_level);
        store.release_entry_lock(shared_lock);
    }

    void repair_graph_parallel(RepairGraphSharedState &ss)
    {
        CONSTEXPR_IF (use_dist_cache) {
            dist_cache.emplace();
        }
        /* Phase 1: steal base-layer pages */
        while (true) {
            vacuum_delay_point();
            size_t start = ss.base_counter->fetch_add(ss.base_batch_size, std::memory_order_relaxed);
            if (start >= ss.basepoint_num) {
                break;
            }
            size_t end = Min(start + ss.base_batch_size, ss.basepoint_num);
            auto &deleted = *reinterpret_cast<UnorderedSet<size_t> *>(ss.deleted);
            repair_base_range(start, end - start, deleted);
        }

        /* Phase 2: steal upper-layer pages */
        while (true) {
            vacuum_delay_point();
            size_t start = ss.upper_counter->fetch_add(ss.upper_batch_size, std::memory_order_relaxed);
            if (start >= ss.upperpoint_num) {
                break;
            }
            size_t end = Min(start + ss.upper_batch_size, ss.upperpoint_num);
            auto &deleted = *reinterpret_cast<UnorderedSet<size_t> *>(ss.deleted);
            repair_upper_range(start, end - start, deleted);
        }
        CONSTEXPR_IF (use_dist_cache) {
            ann_helper::optional_destroy(*dist_cache);
        }
    }

    void repair_graph_remaining(RepairGraphSharedState &ss)
    {
        CONSTEXPR_IF (use_dist_cache) {
            dist_cache.emplace();
        }
        /* step 2.2 */
        /* repair most new points without exclusive lock first */
        size_t prev_basepoint_num = store.base_layer.size();
        size_t prev_upperpoint_num = store.upper_layer.size();
        if (prev_basepoint_num > ss.basepoint_num) {
            auto &deleted = *reinterpret_cast<UnorderedSet<size_t> *>(ss.deleted);
            repair_base_range(ss.basepoint_num, prev_basepoint_num - ss.basepoint_num, deleted);
        }
        if (prev_upperpoint_num > ss.upperpoint_num) {
            auto &deleted = *reinterpret_cast<UnorderedSet<size_t> *>(ss.deleted);
            repair_upper_range(ss.upperpoint_num, prev_upperpoint_num - ss.upperpoint_num, deleted);
        }

        /* step 2.3 */
        /* repair the newest points with exclusive lock finally */
        auto [entry_info2, shared_lock2] = store.template get_entry<true>(GRAPH_INDEX_MAX_LEVEL + 1);
        size_t final_basepoint_num = store.base_layer.size();
        size_t final_upperpoint_num = store.upper_layer.size();
        if (final_basepoint_num > prev_basepoint_num) {
            auto &deleted = *reinterpret_cast<UnorderedSet<size_t> *>(ss.deleted);
            repair_base_range(prev_basepoint_num, final_basepoint_num - prev_basepoint_num, deleted);
        }
        if (final_upperpoint_num > prev_upperpoint_num) {
            auto &deleted = *reinterpret_cast<UnorderedSet<size_t> *>(ss.deleted);
            repair_upper_range(prev_upperpoint_num, final_upperpoint_num - prev_upperpoint_num, deleted);
        }
        store.release_entry_lock(shared_lock2);
        CONSTEXPR_IF (use_dist_cache) {
            ann_helper::optional_destroy(*dist_cache);
        }
    }

    tuple<size_t, size_t, size_t, size_t> get_repair_info()
    {
        return {store.base_layer.size(), store.upper_layer.size(),
                store.base_layer.n_data_per_block(), store.upper_layer.n_data_per_block()};
    }

private:
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
    bool is_valid(T id) { return likely(id != (T)INVALID_VECTOR_ID); }
    int_fast8_t get_insert_level()
    {
        return std::min<int_fast8_t>((-log(RandomDouble()) * (1 / log(m))), (GRAPH_INDEX_MAX_LEVEL - 1));
    }
    template <bool is_base_layer> uint_fast16_t get_nbr_num() { return is_base_layer ? m * 2 : m; }
    bool check_insertable(const Cand &ep, const char *query)
        { return unlikely(memcmp(ep.val, query, store.get_elemsize()) == 0); }

    enum class InsertStrategy {
        Trivial,        /* inserted to point and do nothing */
        InsertPoint,    /* treat the insert data as a new point */
        UpdateCenter,   /* inserted to point and the center is updated, old points need to migrate */
            /**
             * In this case, we keep the center neighbors unchanged as the update will consider
             * them to keep the center being the center of these neighbors, guarenteed by P_local penalty.
             * Even that may not hold (due to low lambda or extreme distribution),
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
        typename point_type::Data temp[data.size()];
        if constexpr (clustered) {
            for (size_t i = 0; i < data.size(); ++i) {
                temp[i].tid = data[i].tid;
            }
            // TD on code
        } else {
            for (size_t i = 0; i < data.size(); ++i) {
                temp[i] = data[i].tid;
            }
        }
        return elem.insert_tid(ctx, {temp, data.size()});
    }

    InsertStrategy apply_arrangement(InsertContext &ctx, const Vector<Cand> &ep, bool need_retry)
    {
        static_assert(__cplusplus >= 201703L, "only compilable with c++17 or greater");
        if constexpr (clustered) {  /* graph cluster path */
            Assert(!ep.empty());
            GraphIndexStats stats = store.get_stats();
            T k_buckets = store.get_vector_num();
            double alpha = ep.size() / (double)k_buckets;
            double mu_g = stats->global_welford.mean;
            bool inserted;
            bool ret_retry = false;
            store.apply_elem(ep.front().id, [&](GraphIndexCluster &elem) -> bool {
                /* 1. calculate threshold */
                auto get_threshold = [&](GIwelford<false> &gi) -> float {
                    double mu_l_j_star = gi.mean;
                    double H = ((1 + alpha) * mu_l_j_star * mu_g) / (alpha * mu_l_j_star + mu_g);
                    double S_comb =
                        sqrt(gi.m2 / ep.size() + stats->global_welford.m2 / k_buckets);
                    float threshold = H * (1 + stats->beta * S_comb);
                    return threshold;
                };
                float threshold = get_threshold(elem.welford);
                if (threshold > ep.front().dist) {
                    inserted = false;
                    return false;
                }
                /* 2. insert data into the point */
                uint8 freq_threshold = GraphIndexStatsData::maintenance_freq(k_buckets);
                if (unlikely(need_retry) && elem.new_inserted + 1 >= freq_threshold) {
                    ret_retry = true;
                    inserted = false;
                    return false;
                }
                inserted = insert_range_tid(ctx.ctx, Span{ctx}, elem);
                if (unlikely(!inserted)) {
                    return false;
                }
                /* 2.0. update freq  */
                ++elem.new_inserted;
                if constexpr (!distancer_supports_cluster_maint) {
                    if (elem.new_inserted >= freq_threshold) {
                        elem.new_inserted = 0;
                    }
                    return true;
                }
                if (freq_threshold > elem.new_inserted || ctx.round_full() ||
                    !try_set_under_redistrib(store.get_index(), ep.front().id)) {
                    return true;
                }
                elem.new_inserted = 0;
                /* 2.1. calculate and update new centers if freq meets update requirement */
                if constexpr (distancer_supports_cluster_maint) {
                    Vector<ItemPointerData> tids;
                    uint32 ndata = elem.get_tids(tids, ctx.ctx);
                    const auto norm_func = get_norm_func();
                    size_t buf_size =
                        (Distancer::dpt == DistPrecisionType::INT8 ? 2 : 1) * store.get_vecsize();
                    buf_size = get_aligned_vec_size(buf_size);
                    size_t vec_size = get_aligned_vec_size(store.get_vecsize());
                    char *vec = alloc_vector(buf_size, ndata + 2);
                    char *center = vec + buf_size * ndata;
                    char *temp_center = center + buf_size;
                    for (auto it = tids.cbegin(); it != tids.cend();) {
                        if (store.fetch_vec_from_heap(*it, vec)) {
                            if (norm_func) {
                                norm_func(vec, store.get_dim(), vec);
                            }
                            vec += vec_size;
                            ++it;
                        } else {
                            it = tids.erase(it);
                        }
                    }
                    ndata = tids.size();
                    vec -= vec_size * ndata;
                    // uint16 aligned_dim = vec_size / VEC_ELEM_SIZE(Distancer::dpt);
                    uint16 dim = store.get_dim();
                    memset(center, 0, buf_size);
                    // TD: handle int8
                    typename Distancer::template transform_type<TransformOp::ADD> adder;
                    typename Distancer::template transform_type<TransformOp::MUL_SCALAR> muler;
                    constexpr uint32 nstep = 250; /* prevent overflow */
                    uint32 i = 0;
                    for (; i + nstep < ndata; i += nstep) {
                        memset(temp_center, 0, buf_size);
                        for (uint32 j = 0; j < nstep; ++j) {
                            adder.transform_single(temp_center, vec + (i + j) * buf_size, temp_center, dim);
                        }
                        muler.transform_single(temp_center, transform_scalar_to_ptr(1.0f / ndata), temp_center, dim);
                        adder.transform_single(temp_center, center, center, dim);
                    }
                    memset(temp_center, 0, buf_size);
                    for (; i < ndata; ++i) {
                        adder.transform_single(temp_center, vec + i * buf_size, temp_center, dim);
                    }
                    muler.transform_single(temp_center, transform_scalar_to_ptr(1.0f / ndata), temp_center, dim);
                    adder.transform_single(temp_center, center, center, dim);
                    typename Distancer::template transform_type<TransformOp::ADD> suber;
                    float *dists = alloc_floatvector(1, ndata);
                    // TD: use batch version
                    for (uint32 i = 0; i < ndata; ++i) {
                        dists[i] = get_distance(center, vec + i * buf_size);
                    }
                    /* 2.2. update stats */
                    double new_mu = std::accumulate(dists, dists + ndata, 0.0f) / ndata;
                    double new_m2 = std::accumulate(dists, dists + ndata, 0.0f,
                        [&](const float &a, const float &b) -> float {
                            return a + std::pow(b - new_mu, 2);
                        }) / ndata;
                    GIwelford<false> welford{new_mu, new_m2, ndata};

                    bool need_split = false;
                    // TD
                    if (need_split) { /* 3. if split is viable */
                        if (!ctx.to_reallocated) {
                            ctx.to_reallocated.emplace();
                        }
                        /* 3.1. get split partition */
                        // TD, auto [a, b] = xxx
                        /* 3.2. gather reallocation from a and b */
                        // TD
                        // dirty = xxx
                    } else {    /* 4. eviction round */
                        for (;;) {
                            threshold = get_threshold(welford);
                            float *max_d = std::max_element(dists, dists + ndata);
                            if (*max_d <= threshold) {
                                break;
                            }
                            welford.reverse(*max_d);
                            *max_d = -FLT_MAX;
                            T closest_id = -1;
                            const uint_fast16_t evict_idx = max_d - dists;
                            if (!ctx.to_reallocated) {
                                ctx.to_reallocated.emplace();
                            }
                            ctx.to_reallocated->emplace_back(closest_id, tids[evict_idx], vec + evict_idx * vec_size);
                        }
                    }

                    /* 5. accumulate stats for update the global */
                    // TD
                    // TD: set closest id, invalid if current one is closest and we make new point if so
                    ann_helper::optional_destroy(tids);
                    adder.destroy();
                    muler.destroy();
                    suber.destroy();
                } else {
                    elem.new_inserted = 0;
                }
                return true;
            });
            if (inserted) {
                /* 5.1. update global stats */
                // TD
                if (ctx.new_point) {
                    return InsertStrategy::UpdateAndInsert;
                }
                if (ctx.to_reallocated) {
                    return InsertStrategy::UpdateCenter;
                }
                return InsertStrategy::Trivial;
            }
            if (ret_retry) {
                return InsertStrategy::RetrySincePossibleInsert;
            }
            return InsertStrategy::InsertPoint;
        } else {                    /* default path */
            for (const auto &p : ep) {
                /* ep is sorted in order, only need to compare the nearest point */
                if (likely(memcmp(p.val, ctx.query, store.get_elemsize()) != 0)) {
                    break;
                }
                if (store.apply_elem(p.id, [&](point_type &pt) -> bool {
                    return insert_range_tid(ctx.ctx, Span{ctx}, pt);
                })) {
                    return InsertStrategy::Trivial;
                }
            }
            return InsertStrategy::InsertPoint;
        }
    }

    Pair<T, T> insert_new_point(InsertContext &ctx, Vector<Cand> &&ep, Span<Vector<Cand>> nbr_record)
    {
        int_fast8_t search_level = (int_fast8_t)nbr_record.size();
        T id = store.template assign_vector_id<true>();
        store.add_elem(ctx.ctx, id, ctx.tid);
        CONSTEXPR_IF (use_dist_cache) {
            dist_cache.emplace((m + ef_construction) * (1 + search_level));
        }
        /* append the element from base layer to upper layer */
        T cur_layer_idx = id;
        T lower_layer_idx = (T)INVALID_VECTOR_ID;
        Vector<Cand> base_neighbors{select_neighbors<true>(std::move(ep))};
        add_basepoint(id, base_neighbors);
        add_vector_for_store(id, ctx.query);
        update_reverse_edges<true>(std::move(base_neighbors), ctx.query, id, cur_layer_idx);
        for (int_fast8_t l = 1; l <= search_level; ++l) {
            lower_layer_idx = cur_layer_idx;
            cur_layer_idx = store.template assign_vector_id<false>();
            get_neighbors_data(nbr_record[l - 1]);
            Vector<Cand> upper_neighbors{select_neighbors<false>(std::move(nbr_record[l - 1]))};
            add_upperpoint(cur_layer_idx, lower_layer_idx, id, upper_neighbors);
            update_reverse_edges<false>(std::move(upper_neighbors), ctx.query, id, cur_layer_idx);
        }
        CONSTEXPR_IF (use_dist_cache) {
            ann_helper::optional_destroy(*dist_cache);
        }
        return {id, cur_layer_idx};
    }

    /* ef == 1 */
    void search_upper_layer(const char *query, Vector<Cand> &entrypoint)
    {
        Assert(entrypoint.size() == 1);
        Cand &cur_point = entrypoint[0];
        UnorderedSet<T> visited(m * 2);
        Vector<T> nbr_id(m);
        Vector<T> nbr_cur_layer_idx(m);
        float *dists = (float *)palloc(sizeof(float) * m);
        float closest_dist = FLT_MAX;
        bool converged;
        do {
            converged = true;
            T lock_point_cur_layer_idx = cur_point.cur_layer_idx;
            store.template lock_point<false, true>(lock_point_cur_layer_idx);
            auto [neighbors_id, lower_layer_idx, unused] = store.template get_point_info<false>(cur_point.cur_layer_idx); 
            T *neighbors_cur_layer_idx = neighbors_id + m;
            cur_point.lower_layer_idx = lower_layer_idx;

            nbr_id.clear();
            nbr_cur_layer_idx.clear();
            for (uint_fast16_t i = 0; i < m; ++i) {
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
        pfree(dists);
        ann_helper::optional_destroy(nbr_cur_layer_idx);
        ann_helper::optional_destroy(nbr_id);
        ann_helper::optional_destroy(visited);
    }

    template <bool is_base_layer, typename filter_func>
    Vector<Cand> search_layer(const char *query, Vector<Cand> &&entrypoint, uint_fast16_t ef,
                              filter_func &&filter)
    {
        uint_fast16_t nbr_num = get_nbr_num<is_base_layer>();
        fpq furthest(ef + 1);
        UnorderedSet<T> visited(m * ef * 2);
        for (const Cand &cand : entrypoint) {
            visited.insert(cand.id);
        }
        cpq closest(std::move(entrypoint), true);
        Vector<T> nbr_id(nbr_num);
        Vector<T> nbr_cur_layer_idx(nbr_num);
        float *dists = (float *)palloc(sizeof(float) * nbr_num);
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
                continue;
            }
            furthest.emplace(cur_point.id, cur_point.cur_layer_idx, lower_layer_idx, cur_point.dist, nullptr);
            if (furthest.size() > ef) {
                furthest.pop();
            }
            store.template unlock_point<is_base_layer, true>(cur_point.cur_layer_idx);
        }

        pfree(dists);
        ann_helper::optional_destroy(nbr_cur_layer_idx);
        ann_helper::optional_destroy(nbr_id);
        closest.destroy();
        ann_helper::optional_destroy(visited);
        furthest.sort();
        return std::move(furthest).data();
    }

    auto get_norm_func() const
    {
        return OidIsValid(index_getprocid(store.get_index(), 1, GRAPH_INDEX_NORM_PROC))
            ? ann_helper::get_vector_preprocess_func(Metric::FAST_COSINE, store.get_precision(), store.get_dim())
            : nullptr;
    }

    void refine(PointExtensionContext &ctx, Vector<Cand> &candidates, const char *query) {
        CONSTEXPR_IF (need_refine) {
            auto norm_func = get_norm_func();
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
        }
    }

    struct PruneNeighbor {
        const char *val;
        T id;
        float dist;
        uint_fast16_t idx;
        PruneNeighbor(const char *v, uint_fast16_t i, float d, T id) : val(v), id(id), dist(d), idx(i) {}
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
    Vector<Cand> select_neighbors(Vector<Cand> &&c, bool sorted = true)
    {
        uint_fast16_t nbr_num = get_nbr_num<is_base_layer>();
        if (unlikely(c.size() <= nbr_num)) {
            /* add dummy edges */
            while (c.size() < nbr_num) {
                c.emplace_back();
            }
            Vector<Cand> res(std::move(c));
            return res;
        }
        
        Vector<Cand> r(nbr_num);
        Vector<Cand> discarded(nbr_num); /* since closest pop in order, discarded is in order naturally */
        cpq closest(std::move(c), sorted);

        while (closest.size() > 0 && r.size() < nbr_num) {
            Cand cur_point = closest.top();
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
        for (size_t i = 0; i < discarded.size() && r.size() < nbr_num; ++i) {
            r.emplace_back(discarded[i]);
        }

        discarded.destroy();
        closest.destroy();
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-local-addr"    /* Vector does not have destructor */
        Vector<Cand> res(std::move(r));
        return res;
#pragma GCC diagnostic pop
    }

    /* backward: neighbors -> new_point, select one neighbor which will be replaced */
    template <bool is_base_layer>
    int16 select_neighbors(Vector<Cand> &&c, T new_point_id, BitSpan<uint> stat, const Cand &self,
                           const char *query)
    {
        int16 pruned = -1;
        uint_fast16_t nbr_num = get_nbr_num<is_base_layer>();
        if (c.size() < nbr_num) {
            pruned = c.size();
            ann_helper::optional_destroy(c);
            return pruned;
        }

        Vector<PruneNeighbor> r(nbr_num);
        Vector<PruneNeighbor> discarded(nbr_num); /* since closest pop in order, discarded is in order naturally */
        PriorityQueue<PruneNeighbor> closest(nbr_num + 1);

        get_neighbors_data(c);
        for (uint_fast16_t i = 0; i < c.size(); ++i) {
            const Cand &nbr = c[i];
            Assert(nbr.id != self.id);
            float dist;
            CONSTEXPR_IF (!use_dist_cache) {
                dist = nbr.dist;
            } else {
                dist = get_distance(self, nbr);
            }
            closest.emplace(nbr.val, i, dist, nbr.id);
        }
        closest.emplace(query, (T)INVALID_VECTOR_ID, self.dist, self.id);

        const auto elem_closer = [&](const Vector<PruneNeighbor> &set, const PruneNeighbor &p) -> bool {
            for (const PruneNeighbor &ri : set) {
                if (get_distance(p, ri) <= p.dist) {
                    return false;
                }
            }
            return true;
        };
        Holder<Vector<PruneNeighbor>> added;
        CONSTEXPR_IF (!use_dist_cache) {
            added.emplace(nbr_num);
        }
        bool has_stats = store.has_stat(stat);
        store.set_stat(stat);
        bool has_remove = false;
        bool closer;
        do {
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
        closest.destroy();

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

    bool repair_basepoint(T *neighbors_id, T self_id, const UnorderedSet<size_t> &deleted)
    {
        uint16 nbr_num = m * 2;
        if (!need_update(deleted, neighbors_id, nbr_num)) {
            return false;
        }
        char *query = store.get_data(self_id);
        Vector<Cand> ep(ef_construction);
        ep.emplace_back(self_id, self_id, 0);
        ep = search_layer<true>(query, std::move(ep), ef_construction, [&](T id) -> bool {
            return id != self_id && !deleted.contains(id);
        });
        get_neighbors_data(ep);
        Vector<Cand> new_neighbors{select_neighbors<true>(std::move(ep))};
        T new_neighbors_id[nbr_num];
        for (uint16 i = 0; i < nbr_num; ++i) {
            Assert(new_neighbors[i].id != self_id);
            new_neighbors_id[i] = new_neighbors[i].id;
        }
        store.set_base_neighbors(self_id, new_neighbors_id);
        update_reverse_edges<true>(std::move(new_neighbors), query, self_id, self_id);
        return true;
    }

    bool repair_upperpoint(T *neighbors_id, T self_id, T self_cur_layer_idx, const UnorderedSet<size_t> &deleted)
    {
        uint16 nbr_num = m;
        if (!need_update(deleted, neighbors_id, nbr_num)) {
            return false;
        }
        char *query = store.get_data(self_id);
        Vector<Cand> ep(ef_construction);
        ep.emplace_back(self_id, self_cur_layer_idx, 0);
        ep = search_layer<false>(query, std::move(ep), ef_construction, [&](T id) -> bool {
            return id != self_id && !deleted.contains(id);
        });
        get_neighbors_data(ep);
        Vector<Cand> new_neighbors{select_neighbors<false>(std::move(ep))};
        T new_neighbors_info[nbr_num * 2];
        T *new_neighbors_id = new_neighbors_info;
        T *new_neighobrs_cur_layer_idx = new_neighbors_info + m;
        for (uint16 i = 0; i < nbr_num; ++i) {
            Assert(new_neighbors[i].id != self_id);
            new_neighbors_id[i] = new_neighbors[i].id;
            new_neighobrs_cur_layer_idx[i] = new_neighbors[i].cur_layer_idx;
        }
        store.set_upper_neighbors(self_cur_layer_idx, new_neighbors_info);
        update_reverse_edges<false>(std::move(new_neighbors), query, self_id, self_cur_layer_idx);
        return true;
    }

    void repair_base_range(size_t start, size_t num, const UnorderedSet<size_t> &deleted)
    {
        for (size_t i = 0; i < num; ++i) {
            CHECK_FOR_INTERRUPTS();
            T id = start + i;
            if (deleted.contains((size_t)id)) {
                continue;
            }
            auto [neighbors_id, unused, unused1] = store.template get_point_info<true>(id);
            repair_basepoint(neighbors_id, id, deleted);
        }
    }

    void repair_upper_range(size_t start, size_t num, const UnorderedSet<size_t> &deleted)
    {
        for (size_t i = 0; i < num; ++i) {
            CHECK_FOR_INTERRUPTS();
            T cur_layer_idx = start + i;
            auto [neighbors_info, unused, id] = store.template get_point_info<false>(cur_layer_idx);
            if (deleted.contains((size_t)id)) {
                continue;
            }
            repair_upperpoint(neighbors_info, id, cur_layer_idx, deleted);
        }
    }

    template <bool is_base_layer>
    void update_reverse_edges(Vector<Cand> &&neighbors, const char *query, T newpoint_id,
                              T newpoint_cur_layer_idx)
    {
        for (const Cand &nbr : neighbors) {
            if (!is_valid(nbr.id)) {
                break;
            }
            store.reset_neighbors_val_pool();
            Vector<Cand> r;
            store.template lock_point<is_base_layer, false>(nbr.cur_layer_idx);
            store.template get_neighbors<is_base_layer>(r, nbr);
            auto p = store.template get_neighbor_stats<is_base_layer>(nbr.cur_layer_idx);
            const_cast<Cand &>(nbr).val = store.get_data(nbr.id);
            int16 pruned = select_neighbors<is_base_layer>(std::move(r), newpoint_id, p.second, nbr, query);
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

    void replace_lower_layer_idx(Vector<Cand> &c)
    {
        for (Cand &point : c) {
            point.cur_layer_idx = point.lower_layer_idx;
            point.lower_layer_idx = (T)INVALID_VECTOR_ID;
        }
    }

    void get_neighbors_data(Vector<Cand> &c)
    {
        store.reset_neighbors_val_pool();
        for (Cand &point : c) {
            point.val = store.get_data(point.id);
        }
    }

    void add_upperpoint(T cur_layer_idx, T lower_layer_idx, T id, const Vector<Cand> &neighbors)
    {
        uint_fast16_t nbr_num = m;
        T neighbors_info[nbr_num * 2];
        T *neighbors_id = neighbors_info;
        T *neighbors_cur_layer_idx = neighbors_id + nbr_num;
        for (uint_fast16_t i = 0; i < nbr_num; ++i) {
            Assert(neighbors[i].cur_layer_idx != cur_layer_idx);
            neighbors_id[i] = neighbors[i].id;
            neighbors_cur_layer_idx[i] = neighbors[i].cur_layer_idx;
        }
        store.add_upperpoint(cur_layer_idx, lower_layer_idx, id, neighbors_info);
        CONSTEXPR_IF (!use_dist_cache) {
            float *dists = store.template get_neighbor_stats<false>(cur_layer_idx).first;
            for (uint_fast16_t i = 0; i < nbr_num; ++i) {
                dists[i] = neighbors[i].dist;
            }
        }
    }

    void add_basepoint(T id, const Vector<Cand> &neighbors)
    {
        uint_fast16_t nbr_num = m * 2;
        T neighbors_id[nbr_num];
        for (uint_fast16_t i = 0; i < nbr_num; ++i) {
            Assert(neighbors[i].id != id);
            neighbors_id[i] = neighbors[i].id;
        }
        store.add_basepoint(id, neighbors_id);
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
        T neighbors_id[nbr_num];
        for (uint_fast16_t i = 0; i < nbr_num; ++i) {
            neighbors_id[i] = (T)INVALID_VECTOR_ID;
        }
        store.add_basepoint(0, neighbors_id);
    }

    void add_first_upperpoint(T cur_idx, T lower_layer_idx, T id)
    {
        T neighbors_info[m * 2];
        for (uint_fast16_t i = 0; i < m * 2; ++i) {
            neighbors_info[i] = (T)INVALID_VECTOR_ID;
        }
        store.add_upperpoint(cur_idx, lower_layer_idx, id, neighbors_info);
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
