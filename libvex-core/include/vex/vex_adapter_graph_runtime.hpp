#ifndef VEX_ADAPTER_GRAPH_RUNTIME_HPP
#define VEX_ADAPTER_GRAPH_RUNTIME_HPP

#include "vex/vex_adapter_graph_state.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace vex {

template <typename BindingT>
inline bool ReloadGraphRuntime(BindingT &binding, HNSWGraph &graph, AdapterGraphState *state_out = nullptr) {
    AdapterGraphState state{};
    if (!LoadGraphStateFromBinding(binding, state)) {
        return false;
    }
    ApplyGraphState(graph, state);
    if (state_out) {
        *state_out = state;
    }
    return true;
}

template <typename BindingT, typename NodeStoreT>
inline bool CreateGraphRuntime(std::shared_ptr<BindingT> binding, NodeStoreT &store, Metric metric, int m,
                               int ef_construction, std::unique_ptr<HNSWGraph> &graph_out,
                               AdapterGraphState *state_out = nullptr) {
    if (!binding) {
        return false;
    }
    graph_out = std::unique_ptr<HNSWGraph>(new HNSWGraph(store, metric, m, ef_construction));
    return ReloadGraphRuntime(*binding, *graph_out, state_out);
}

template <typename BindingT>
inline bool PersistGraphRuntime(BindingT &binding, const HNSWGraph &graph, AdapterGraphState *state_out = nullptr) {
    AdapterGraphState state = CaptureGraphState(graph);
    if (state_out) {
        *state_out = state;
    }
    return StoreGraphStateToBinding(binding, state);
}

template <typename NodeStoreT>
inline bool UpdateNodeRowId(NodeStoreT &store, node_id_t node_id, row_id_t row_id) {
    auto handle = store.PinNodeForUpdate(node_id);
    if (!handle || !handle->MutableHeader()) {
        return false;
    }
    handle->MutableHeader()->row_id = row_id;
    return true;
}

inline const float *PrepareQueryVector(const float *query, uint32_t dim, bool normalize_query,
                                       std::vector<float> &scratch) {
    if (!normalize_query) {
        return query;
    }
    scratch.assign(query, query + dim);
    NormalizeVector(scratch.data(), dim);
    return scratch.data();
}

template <typename EntryPointT, typename RowMapT, typename IsDeletedFn, typename LevelFn>
inline bool ResolveLiveEntryPoint(EntryPointT entry_point, const RowMapT &row_id_map,
                                  IsDeletedFn &&is_deleted, LevelFn &&level_of,
                                  EntryPointT &out_entry_point, bool prefer_highest_level = true) {
    if (!is_deleted(entry_point)) {
        out_entry_point = entry_point;
        return true;
    }

    bool found = false;
    int best_level = -1;
    for (const auto &entry : row_id_map) {
        const auto &candidate = entry.second;
        if (is_deleted(candidate)) {
            continue;
        }
        if (!prefer_highest_level) {
            out_entry_point = candidate;
            return true;
        }

        const int candidate_level = level_of(candidate);
        if (!found || candidate_level > best_level) {
            out_entry_point = candidate;
            best_level = candidate_level;
            found = true;
        }
    }
    return found;
}

template <typename EntryPointT, typename CandidateVecT, typename VisitedSetT,
          typename SearchLayerFn, typename CandidateNodeFn>
inline EntryPointT GreedyDescendUpperLayers(EntryPointT entry_point, int max_level,
                                            CandidateVecT &candidates, VisitedSetT &visited,
                                            SearchLayerFn &&search_layer, CandidateNodeFn &&candidate_node) {
    using CandidateT = typename CandidateVecT::value_type;
    EntryPointT ep = entry_point;
    for (int level = max_level; level > 0; --level) {
        candidates.clear();
        visited.Clear();
        search_layer(ep, level, candidates, visited);
        if (!candidates.empty()) {
            std::sort(candidates.begin(), candidates.end(), [](const CandidateT &lhs, const CandidateT &rhs) {
                return lhs.distance < rhs.distance;
            });
            ep = candidate_node(candidates.front());
        }
    }
    return ep;
}

template <typename EntryPointT, typename RowMapT, typename CandidateVecT, typename VisitedSetT,
          typename IsDeletedFn, typename LevelFn, typename SearchLayerFn, typename CandidateNodeFn>
inline bool ResolveLevel0EntryPoint(EntryPointT entry_point, int max_level, const RowMapT &row_id_map,
                                    CandidateVecT &candidates, VisitedSetT &visited,
                                    IsDeletedFn &&is_deleted, LevelFn &&level_of,
                                    SearchLayerFn &&search_upper, CandidateNodeFn &&candidate_node,
                                    EntryPointT &out_entry_point, bool prefer_highest_level = true) {
    EntryPointT ep = entry_point;
    if (!ResolveLiveEntryPoint(ep, row_id_map, std::forward<IsDeletedFn>(is_deleted),
                               std::forward<LevelFn>(level_of), ep, prefer_highest_level)) {
        return false;
    }

    out_entry_point = GreedyDescendUpperLayers(ep, max_level, candidates, visited,
                                               std::forward<SearchLayerFn>(search_upper),
                                               std::forward<CandidateNodeFn>(candidate_node));
    return true;
}

template <typename NodeStoreT>
inline node_id_t AddPointToGraph(HNSWGraph &graph, NodeStoreT &store, row_id_t graph_row_id,
                                 const float *vec, uint32_t dim, row_id_t stored_row_id) {
    node_id_t node_id = graph.AddPoint(graph_row_id, vec, dim);
    if (stored_row_id != graph_row_id && !UpdateNodeRowId(store, node_id, stored_row_id)) {
        return INVALID_NODE_ID;
    }
    return node_id;
}

template <typename NodeStoreT>
inline node_id_t AddPointToGraph(HNSWGraph &graph, NodeStoreT &store, row_id_t graph_row_id,
                                 const float *vec, uint32_t dim, row_id_t stored_row_id, uint8_t level) {
    node_id_t node_id = graph.AddPointWithLevel(graph_row_id, vec, dim, level);
    if (stored_row_id != graph_row_id && !UpdateNodeRowId(store, node_id, stored_row_id)) {
        return INVALID_NODE_ID;
    }
    return node_id;
}

template <typename BindingT, typename NodeStoreT>
inline bool ExecuteBindingSearch(std::shared_ptr<BindingT> binding, NodeStoreT &store, Metric metric, int m,
                                 int ef_construction, const float *query_vec, uint32_t k, int ef,
                                 std::vector<row_id_t> &out_row_ids, std::vector<float> &out_distances,
                                 uint64_t brute_force_threshold = 64,
                                 AdapterGraphState *state_out = nullptr) {
    std::unique_ptr<HNSWGraph> graph;
    if (!CreateGraphRuntime(std::move(binding), store, metric, m, ef_construction, graph, state_out)) {
        return false;
    }
    graph->Search(query_vec, k, ef, out_row_ids, out_distances, brute_force_threshold);
    return true;
}

template <typename BindingT, typename NodeStoreT>
inline bool ExecuteBindingFilteredSearch(std::shared_ptr<BindingT> binding, NodeStoreT &store, Metric metric, int m,
                                         int ef_construction, const float *query_vec, uint32_t k, int ef,
                                         std::vector<row_id_t> &out_row_ids, std::vector<float> &out_distances,
                                         const FilterPredicate &filter, uint64_t brute_force_threshold = 64,
                                         AdapterGraphState *state_out = nullptr) {
    std::unique_ptr<HNSWGraph> graph;
    if (!CreateGraphRuntime(std::move(binding), store, metric, m, ef_construction, graph, state_out)) {
        return false;
    }
    graph->FilteredSearch(query_vec, k, ef, out_row_ids, out_distances, filter, brute_force_threshold);
    return true;
}

template <typename BindingT, typename NodeStoreT>
inline bool ExecuteBindingQuantizedSearch(
    std::shared_ptr<BindingT> binding, NodeStoreT &store, Metric metric, int m, int ef_construction,
    const float *query_vec, uint32_t k, int ef, std::vector<row_id_t> &out_row_ids,
    std::vector<float> &out_distances, QuantDistancer &distancer,
    const std::function<const uint8_t *(node_id_t)> &code_lookup, uint64_t brute_force_threshold = 64,
    AdapterGraphState *state_out = nullptr) {
    std::unique_ptr<HNSWGraph> graph;
    if (!CreateGraphRuntime(std::move(binding), store, metric, m, ef_construction, graph, state_out)) {
        return false;
    }
    graph->SearchWithQuantizedCodes(query_vec, k, ef, out_row_ids, out_distances, distancer, code_lookup,
                                    brute_force_threshold);
    return true;
}

template <typename ExpandFn>
inline void ExpandSearchResults(const std::vector<row_id_t> &core_row_ids,
                                const std::vector<float> &core_distances, size_t limit,
                                ExpandFn &&expand_one) {
    size_t remaining = limit;
    const size_t count = std::min(core_row_ids.size(), core_distances.size());
    for (size_t i = 0; i < count && remaining > 0; ++i) {
        size_t appended = expand_one(core_row_ids[i], core_distances[i], remaining);
        if (appended >= remaining) {
            return;
        }
        remaining -= appended;
    }
}

template <typename ExtraRowsT, typename DistanceT, typename RowIdVecT, typename DistanceVecT>
inline size_t AppendExtraRowsWithDistance(const ExtraRowsT &extra_row_ids, DistanceT distance, size_t remaining,
                                          RowIdVecT &out_row_ids, DistanceVecT &out_distances) {
    size_t appended = 0;
    for (const auto &extra_row_id : extra_row_ids) {
        if (appended >= remaining) {
            break;
        }
        out_row_ids.push_back(extra_row_id);
        out_distances.push_back(distance);
        appended++;
    }
    return appended;
}

template <typename RowIdT, typename DistanceT, typename RowIdVecT, typename DistanceVecT, typename AppendExtrasFn>
inline void AppendResultRowWithExtras(RowIdT row_id, uint16_t extra_count, DistanceT distance, size_t limit,
                                      RowIdVecT &out_row_ids, DistanceVecT &out_distances,
                                      AppendExtrasFn &&append_extras) {
    if (out_row_ids.size() >= limit) {
        return;
    }

    out_row_ids.push_back(row_id);
    out_distances.push_back(distance);

    if (extra_count == 0 || out_row_ids.size() >= limit) {
        return;
    }

    append_extras(limit - out_row_ids.size(), distance, out_row_ids, out_distances);
}

} // namespace vex

#endif // VEX_ADAPTER_GRAPH_RUNTIME_HPP
