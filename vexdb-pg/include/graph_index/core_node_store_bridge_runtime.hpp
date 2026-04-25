#pragma once

#include "pg_compat.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

#include "ann_utils.h"
#include "graph_index/core_node_store_bridge.h"
#include "graph_index/core_node_store_bridge_live.hpp"
#include "graph_index/core_node_store_bridge_readonly.hpp"
#include "graph_index/core_node_store_bridge_utils.h"
#include "graph_index/graph_index.h"
#include "graph_index/graph_index_struct.h"
#include "storage/itemptr.h"
#include "vex/vex_adapter_graph_runtime.hpp"
#include "vex/vex_adapter_graph_state.hpp"
#include "vex/vex_adapter_pg_stub.hpp"
#include "vex/vex_distance.hpp"
#include "vex/vex_graph_algo.hpp"

namespace pgvexdb {

struct CoreBridgeSearchResult {
    std::vector<ItemPointerData> heaptids;
    std::vector<float> dists;
    bool has_more_data = false;
};

inline Metric GetCoreBridgeMetric(Relation index)
{
    FmgrInfo *procinfo = index_getprocinfo(index, 1, GRAPH_INDEX_DISTANCE_PROC);
    if (procinfo == NULL) {
        return Metric::L2;
    }
    return get_func_metric(procinfo->fn_oid);
}

inline vex::Metric ToCoreBridgeMetric(Metric metric)
{
    switch (metric) {
        case Metric::L2:
            return vex::Metric::L2;
        case Metric::INNER_PRODUCT:
            return vex::Metric::INNER_PRODUCT;
        case Metric::FAST_COSINE:
            return vex::Metric::INNER_PRODUCT;
        case Metric::COSINE:
            return vex::Metric::COSINE;
        default:
            return vex::Metric::L2;
    }
}

inline bool CanUseCoreBridgeInsert(GraphIndexMetaPage metap, bool use_async)
{
    return !use_async &&
           metap->id_type == IdType::U32 &&
           metap->precision_type == DistPrecisionType::FLOAT &&
           metap->quantizer_metainfo.get_type() == QuantizerType::NONE;
}

inline bool CanUseCoreBridgeScan(GraphIndexMetaPage metap)
{
    return metap->id_type == IdType::U32 &&
           metap->precision_type == DistPrecisionType::FLOAT &&
           metap->quantizer_metainfo.get_type() == QuantizerType::NONE;
}

inline bool CanUseCoreBridgeBuild(IdType id_type, DistPrecisionType precision_type,
                                  QuantizerType qt_type, Metric metric)
{
    return id_type == IdType::U32 &&
           precision_type == DistPrecisionType::FLOAT &&
           qt_type == QuantizerType::NONE &&
           (metric == Metric::L2 ||
            metric == Metric::INNER_PRODUCT ||
            metric == Metric::FAST_COSINE);
}

inline bool CoreBridgeNeedsNormalizedQuery(Metric metric)
{
    return metric == Metric::FAST_COSINE;
}

template <typename Store>
struct CoreBridgeBuildRuntime {
    Store *store = nullptr;
    PointExtensionContext *ctx = nullptr;
    ItemPointerData current_tid{};
    std::shared_ptr<PgCoreLiveBinding<Store>> binding;
    std::shared_ptr<vex::PGNodeStore> node_store;
    std::unique_ptr<vex::HNSWGraph> core_graph;
    uint64_t add_point_count = 0;
    double add_point_total_ms = 0.0;
    double update_row_id_total_ms = 0.0;
    double store_graph_state_ms = 0.0;

    bool Init(Store &store_ref, PointExtensionContext &ctx_ref, Relation index, ForkNumber fork_num,
              BlockNumber metablkno, uint_fast16_t dimension, uint_fast16_t m,
              uint_fast16_t ef_construction, Metric metric)
    {
        if constexpr (std::decay_t<Store>::clustered) {
            return false;
        }

        store = &store_ref;
        ctx = &ctx_ref;

        PgCoreBindingConfig binding_cfg{};
        binding_cfg.index_rel = index;
        binding_cfg.fork_num = fork_num;
        binding_cfg.dimension = dimension;
        binding_cfg.m = m;
        binding_cfg.metadata_size = 0;
        binding_cfg.index_name = RelationGetRelationName(index);
        binding_cfg.metablkno = metablkno;
        binding_cfg.trust_live_header_cache_for_read = true;
        binding_cfg.load_graph_state_cb = [binding_cfg](PgCoreGraphState &out) {
            return LoadPgGraphStateFromMetaPage(binding_cfg, out);
        };
        binding_cfg.store_graph_state_cb = [binding_cfg](const PgCoreGraphState &in) {
            return StorePgGraphStateToMetaPage(binding_cfg, in);
        };
        binding_cfg.on_allocate_node_cb = [this](vex::node_id_t node_id, vex::row_id_t, uint8_t) {
            store->add_elem(*ctx, static_cast<typename Store::T>(node_id), current_tid);
            return true;
        };
        binding_cfg.resolve_storage_node_key_cb = [](vex::node_id_t node_id, uint64_t &storage_key) {
            storage_key = static_cast<uint64_t>(node_id);
            return true;
        };

        binding = CreatePgCoreLiveBinding(*store, binding_cfg);

        vex::PGNodeStoreConfig cfg{};
        cfg.dimension = dimension;
        cfg.m = m;
        cfg.metadata_size = 0;
        cfg.pg_relation = index;
        cfg.index_name = RelationGetRelationName(index);
        cfg.low_level_binding = binding;
        node_store = std::make_shared<vex::PGNodeStore>(cfg);
        return vex::CreateGraphRuntime(binding, *node_store, ToCoreBridgeMetric(metric),
                                       cfg.m, ef_construction, core_graph);
    }

    void AddPoint(const char *query, const ItemPointerData &heap_tid, uint_fast16_t dimension)
    {
        auto add_point_start = std::chrono::high_resolution_clock::now();
        current_tid = heap_tid;
        auto node_id = vex::AddPointToGraph(*core_graph, *node_store, static_cast<vex::row_id_t>(0),
                                            reinterpret_cast<const float *>(query), dimension,
                                            static_cast<vex::row_id_t>(0));
        add_point_total_ms += std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - add_point_start).count();
        ++add_point_count;
        if (node_id != vex::INVALID_NODE_ID) {
            auto update_row_id_start = std::chrono::high_resolution_clock::now();
            (void)vex::UpdateNodeRowId(*node_store, node_id, static_cast<vex::row_id_t>(node_id));
            update_row_id_total_ms += std::chrono::duration<double, std::milli>(
                std::chrono::high_resolution_clock::now() - update_row_id_start).count();
        }

        if (add_point_count % 100 == 0) {
            const auto &insert_profile = core_graph->GetInsertProfile();
            const auto &access_stats = binding->GetAccessStats();
            ereport(NOTICE,
                    (errmsg("PG core bridge AddPoint progress"),
                     errdetail("points=%lu add_point_total_ms=%.3f add_point_avg_ms=%.3f update_row_id_total_ms=%.3f allocate_ms=%.3f insert_ms=%.3f upper_descent_ms=%.3f lower_search_ms=%.3f select_ms=%.3f write_new_ms=%.3f backlink_ms=%.3f prune_ms=%.3f pin_read_calls=%lu pin_read_ms=%.3f pin_write_calls=%lu pin_write_ms=%.3f unpin_calls=%lu unpin_ms=%.3f pin_vector_ms=%.3f pin_header_ms=%.3f pin_metadata_ms=%.3f pin_level0_ms=%.3f pin_upper_ms=%.3f",
                               static_cast<unsigned long>(add_point_count),
                               add_point_total_ms,
                               add_point_total_ms / static_cast<double>(add_point_count),
                               update_row_id_total_ms,
                               insert_profile.allocate_node_ms,
                               insert_profile.insert_total_ms,
                               insert_profile.upper_descent_ms,
                               insert_profile.lower_level_search_ms,
                               insert_profile.select_neighbors_ms,
                               insert_profile.write_new_node_neighbors_ms,
                               insert_profile.backlink_update_ms,
                               insert_profile.backlink_prune_ms,
                               static_cast<unsigned long>(access_stats.pin_read_calls),
                               access_stats.pin_read_ms,
                               static_cast<unsigned long>(access_stats.pin_write_calls),
                               access_stats.pin_write_ms,
                               static_cast<unsigned long>(access_stats.unpin_calls),
                               access_stats.unpin_ms,
                               access_stats.pin_vector_ms,
                               access_stats.pin_header_ms,
                               access_stats.pin_metadata_ms,
                               access_stats.pin_level0_ms,
                               access_stats.pin_upper_ms)));
        }
    }

    bool StoreGraphState()
    {
        auto store_start = std::chrono::high_resolution_clock::now();
        bool ok = binding && core_graph && vex::PersistGraphRuntime(*binding, *core_graph);
        store_graph_state_ms = std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - store_start).count();
        return ok;
    }

    void ReportBuildStats() const
    {
        double add_point_avg_ms = add_point_count == 0 ? 0.0
            : add_point_total_ms / static_cast<double>(add_point_count);
        const auto &insert_profile = core_graph->GetInsertProfile();
        const auto &access_stats = binding->GetAccessStats();
        ereport(NOTICE,
                (errmsg("PG core bridge build stats"),
                 errdetail("points=%lu add_point_total_ms=%.3f add_point_avg_ms=%.3f update_row_id_total_ms=%.3f store_graph_state_ms=%.3f allocate_ms=%.3f insert_ms=%.3f pin_new_ms=%.3f upper_descent_ms=%.3f upper_descent_calls=%lu lower_search_ms=%.3f lower_search_calls=%lu select_ms=%.3f select_calls=%lu write_new_ms=%.3f backlink_ms=%.3f prune_ms=%.3f prune_calls=%lu entry_update_ms=%.3f pin_read_calls=%lu pin_read_ms=%.3f pin_write_calls=%lu pin_write_ms=%.3f unpin_calls=%lu unpin_ms=%.3f pin_vector_ms=%.3f pin_header_ms=%.3f pin_metadata_ms=%.3f pin_level0_ms=%.3f pin_upper_ms=%.3f",
                           static_cast<unsigned long>(add_point_count),
                           add_point_total_ms,
                           add_point_avg_ms,
                           update_row_id_total_ms,
                           store_graph_state_ms,
                           insert_profile.allocate_node_ms,
                           insert_profile.insert_total_ms,
                           insert_profile.pin_new_node_ms,
                           insert_profile.upper_descent_ms,
                           static_cast<unsigned long>(insert_profile.upper_descent_calls),
                           insert_profile.lower_level_search_ms,
                           static_cast<unsigned long>(insert_profile.lower_level_search_calls),
                           insert_profile.select_neighbors_ms,
                           static_cast<unsigned long>(insert_profile.select_neighbors_calls),
                           insert_profile.write_new_node_neighbors_ms,
                           insert_profile.backlink_update_ms,
                           insert_profile.backlink_prune_ms,
                           static_cast<unsigned long>(insert_profile.backlink_prune_calls),
                           insert_profile.entry_update_ms,
                           static_cast<unsigned long>(access_stats.pin_read_calls),
                           access_stats.pin_read_ms,
                           static_cast<unsigned long>(access_stats.pin_write_calls),
                           access_stats.pin_write_ms,
                           static_cast<unsigned long>(access_stats.unpin_calls),
                           access_stats.unpin_ms,
                           access_stats.pin_vector_ms,
                           access_stats.pin_header_ms,
                           access_stats.pin_metadata_ms,
                           access_stats.pin_level0_ms,
                           access_stats.pin_upper_ms)));
    }

    void Reset()
    {
        core_graph.reset();
        node_store.reset();
        binding.reset();
        ctx = nullptr;
        store = nullptr;
    }
};

template <typename Store>
inline bool TryInsertViaCoreBridge(Store &store, Relation index, GraphIndexMetaPage metap,
                                   PointExtensionContext &ctx, const char *query,
                                   const ItemPointerData &heap_tid, bool use_async)
{
    if (!CanUseCoreBridgeInsert(metap, use_async)) {
        return false;
    }

    if constexpr (std::decay_t<Store>::clustered) {
        return false;
    }

    PgCoreBindingConfig binding_cfg{};
    binding_cfg.index_rel = index;
    binding_cfg.fork_num = MAIN_FORKNUM;
    binding_cfg.dimension = metap->dimension;
    binding_cfg.m = metap->m;
    binding_cfg.metadata_size = 0;
    binding_cfg.index_name = RelationGetRelationName(index);
    binding_cfg.metablkno = GRAPH_INDEX_METAPAGE_BLKNO;
    binding_cfg.load_graph_state_cb = [binding_cfg](PgCoreGraphState &out) {
        return LoadPgGraphStateFromMetaPage(binding_cfg, out);
    };
    binding_cfg.store_graph_state_cb = [binding_cfg](const PgCoreGraphState &in) {
        return StorePgGraphStateToMetaPage(binding_cfg, in);
    };
    binding_cfg.on_allocate_node_cb = [&](vex::node_id_t node_id, vex::row_id_t, uint8_t) {
        store.add_elem(ctx, static_cast<typename Store::T>(node_id), heap_tid);
        return true;
    };
    binding_cfg.resolve_storage_node_key_cb = [](vex::node_id_t node_id, uint64_t &storage_key) {
        storage_key = static_cast<uint64_t>(node_id);
        return true;
    };

    auto binding = CreatePgCoreLiveBinding(store, binding_cfg);
    vex::PGNodeStoreConfig cfg{};
    cfg.dimension = metap->dimension;
    cfg.m = metap->m;
    cfg.metadata_size = 0;
    cfg.pg_relation = index;
    cfg.index_name = RelationGetRelationName(index);
    cfg.low_level_binding = binding;

    vex::PGNodeStore node_store(cfg);
    std::unique_ptr<vex::HNSWGraph> core_graph;
    if (!vex::CreateGraphRuntime(binding, node_store,
                                 ToCoreBridgeMetric(GetCoreBridgeMetric(index)),
                                 cfg.m, metap->ef_construction, core_graph)) {
        return false;
    }

    auto node_id = vex::AddPointToGraph(*core_graph, node_store, static_cast<vex::row_id_t>(0),
                                        reinterpret_cast<const float *>(query), metap->dimension,
                                        static_cast<vex::row_id_t>(0));
    if (node_id == vex::INVALID_NODE_ID) {
        return false;
    }
    if (!vex::UpdateNodeRowId(node_store, node_id, static_cast<vex::row_id_t>(node_id))) {
        return false;
    }

    return vex::PersistGraphRuntime(*binding, *core_graph);
}

template <typename Store>
inline bool TrySearchViaCoreBridge(Store &store, Relation index, GraphIndexMetaPage metap,
                                   PointExtensionContext &ctx, const char *query, uint_fast16_t ef_search,
                                   CoreBridgeSearchResult &out)
{
    if (!CanUseCoreBridgeScan(metap)) {
        return false;
    }

    auto metric = GetCoreBridgeMetric(index);
    auto core_metric = ToCoreBridgeMetric(metric);

    std::vector<float> normalized_query;
    const float *core_query = vex::PrepareQueryVector(reinterpret_cast<const float *>(query),
                                                      metap->dimension,
                                                      CoreBridgeNeedsNormalizedQuery(metric),
                                                      normalized_query);

    auto binding = CreatePgCoreReadOnlyBinding(store);
    vex::PGNodeStoreConfig cfg{};
    cfg.dimension = metap->dimension;
    cfg.m = metap->m;
    cfg.metadata_size = 0;
    cfg.index_name = RelationGetRelationName(index);
    cfg.low_level_binding = binding;

    vex::PGNodeStore node_store(cfg);
    std::vector<vex::row_id_t> core_row_ids;
    std::vector<float> core_distances;
    if (!vex::ExecuteBindingSearch(binding, node_store, core_metric, cfg.m,
                                   metap->ef_construction, core_query, ef_search, ef_search,
                                   core_row_ids, core_distances)) {
        return false;
    }

    Vector<ItemPointerData> res;
    Vector<float> dists;
    vex::ExpandSearchResults(core_row_ids, core_distances, static_cast<size_t>(ef_search),
                             [&](vex::row_id_t core_row_id, float core_distance, size_t remaining) -> size_t {
        size_t before = res.size();
        store.get_itempointer(static_cast<typename Store::T>(core_row_id), [&](const typename Store::point_type *elem) {
            (void)elem->get_tids(res, ctx);
        });

        size_t appended = res.size() - before;
        if (appended > remaining) {
            res.resize(before + remaining);
            appended = remaining;
        }

        for (size_t j = 0; j < appended; ++j) {
                dists.push_back(core_distance);
        }
        return appended;
    });

    out.heaptids.assign(res.begin(), res.end());
    out.dists.assign(dists.begin(), dists.end());
    out.has_more_data = res.size() >= ef_search;
    return true;
}

template <typename Store>
inline bool RefreshEntryStateViaCoreBridge(Store &store, Relation index, BlockNumber metablkno)
{
    PgCoreBindingConfig cfg{};
    cfg.index_rel = index;
    cfg.fork_num = MAIN_FORKNUM;
    cfg.metablkno = metablkno;
    cfg.dimension = store.get_dim();
    cfg.m = store.get_m();
    cfg.metadata_size = 0;
    cfg.index_name = RelationGetRelationName(index);
    cfg.load_graph_state_cb = [cfg](PgCoreGraphState &out) {
        return LoadPgGraphStateFromMetaPage(cfg, out);
    };
    cfg.store_graph_state_cb = [cfg](const PgCoreGraphState &in) {
        return StorePgGraphStateToMetaPage(cfg, in);
    };

    auto binding = CreatePgCoreLiveBinding(store, cfg);

    vex::AdapterGraphState state{};
    if (!vex::LoadGraphStateFromBinding(*binding, state)) {
        return false;
    }

    auto is_live_node = [&](vex::node_id_t node_id, int *level_out) -> bool {
        vex::PGNodeLayoutView view{};
        if (!binding->PinNode(node_id, false, view)) {
            return false;
        }
        bool is_live = view.header != nullptr && !view.header->deleted;
        if (is_live && level_out != nullptr) {
            *level_out = static_cast<int>(view.header->level);
        }
        binding->UnpinNode(view);
        return is_live;
    };

    int current_level = 0;
    if (state.has_entry_point && is_live_node(state.entry_point, &current_level)) {
        state.max_level = current_level;
        return vex::StoreGraphStateToBinding(*binding, state);
    }

    vex::node_id_t best_node = vex::INVALID_NODE_ID;
    int best_level = -1;
    binding->ForEachNode([&](vex::node_id_t node_id) {
        int level = 0;
        if (!is_live_node(node_id, &level)) {
            return;
        }
        if (best_node == vex::INVALID_NODE_ID || level > best_level ||
            (level == best_level && node_id < best_node)) {
            best_node = node_id;
            best_level = level;
        }
    });

    state.has_entry_point = (best_node != vex::INVALID_NODE_ID);
    state.entry_point = state.has_entry_point ? best_node : vex::INVALID_NODE_ID;
    state.max_level = state.has_entry_point ? best_level : 0;
    state.node_count = static_cast<uint64_t>(store.get_vector_num());
    return vex::StoreGraphStateToBinding(*binding, state);
}

template <typename Store>
inline void RefreshEntryStateAfterVacuum(Store &store, Relation index, BlockNumber metablkno)
{
    if constexpr (std::decay_t<Store>::clustered) {
        return;
    } else {
        (void)RefreshEntryStateViaCoreBridge<Store>(store, index, metablkno);
    }
}

} // namespace pgvexdb
