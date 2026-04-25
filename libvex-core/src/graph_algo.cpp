#include "vex/vex_graph_algo.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>

namespace vex {

namespace {

using SteadyClock = std::chrono::steady_clock;

inline double ElapsedMs(const SteadyClock::time_point &start) {
    return std::chrono::duration<double, std::milli>(SteadyClock::now() - start).count();
}

} // namespace

HNSWGraph::HNSWGraph(NodeStore &store, Metric metric, int m, int ef_construction)
    : store_(store),
      distance_func_(GetDistanceFunc(metric)),
      m_(m),
      ef_construction_(ef_construction),
      dim_(store.GetDimension()),
      rng_(std::random_device{}()),
      dist_(0.0, 1.0) {
    if (m_ < GraphIndexConfig::MIN_M || m_ > GraphIndexConfig::MAX_M) {
        throw std::invalid_argument("HNSWGraph: invalid m");
    }
    if (ef_construction_ < GraphIndexConfig::MIN_EF_CONSTRUCTION ||
        ef_construction_ > GraphIndexConfig::MAX_EF_CONSTRUCTION) {
        throw std::invalid_argument("HNSWGraph: invalid ef_construction");
    }
}

int HNSWGraph::GetRandomLevel() {
    double ml = GraphIndexConfig::GetMl(m_);
    double r = dist_(rng_);
    if (r == 0.0) {
        r = std::numeric_limits<double>::min();
    }
    int level = static_cast<int>(-std::log(r) * ml);
    return std::min(level, GraphIndexConfig::GetMaxLevel(m_));
}

node_id_t HNSWGraph::AddPoint(row_id_t row_id, const float *vec, uint32_t dim) {
    return AddPointWithLevel(row_id, vec, dim, static_cast<uint8_t>(GetRandomLevel()));
}

node_id_t HNSWGraph::AddPointWithLevel(row_id_t row_id, const float *vec, uint32_t dim, uint8_t level) {
    if (vec == nullptr) {
        throw std::invalid_argument("AddPoint: vec is null");
    }
    if (dim != dim_) {
        throw std::invalid_argument("AddPoint: dimension mismatch");
    }
    auto allocate_start = SteadyClock::now();
    const node_id_t node_id = store_.AllocateNode(row_id, vec, dim, level);
    insert_profile_.allocate_node_ms += ElapsedMs(allocate_start);
    insert_profile_.add_point_calls++;
    node_count_++;

    auto insert_start = SteadyClock::now();
    InsertNode(node_id, m_, ef_construction_, distance_func_);
    insert_profile_.insert_total_ms += ElapsedMs(insert_start);
    return node_id;
}

void HNSWGraph::SearchLayer(const float *query, node_id_t ep, int ef, int layer_num,
                            std::vector<GraphCandidate> &candidates, VisitedSet &visited,
                            distance_func_t distance_func) {
    std::priority_queue<GraphCandidate, std::vector<GraphCandidate>, std::greater<GraphCandidate>> candidate_queue;
    std::priority_queue<GraphCandidate, std::vector<GraphCandidate>, std::less<GraphCandidate>> visited_queue;

    auto ep_h = store_.PinNode(ep);
    if (!ep_h) {
        return;
    }
    const auto *ep_header = ep_h->Header();
    if (ep_header == nullptr || ep_header->deleted) {
        return;
    }

    float ep_dist = distance_func(query, ep_h->Vector(), dim_);
    visited.Insert(ep);
    candidate_queue.push({ep, ep_dist});
    visited_queue.push({ep, ep_dist});

    while (!candidate_queue.empty()) {
        auto current = candidate_queue.top();
        candidate_queue.pop();

        if (!visited_queue.empty() && current.distance > visited_queue.top().distance) {
            break;
        }

        auto cur_h = store_.PinNode(current.node_id);
        if (!cur_h) {
            continue;
        }
        const auto *cur_header = cur_h->Header();
        if (cur_header == nullptr || cur_header->deleted) {
            continue;
        }
        if (layer_num > static_cast<int>(cur_header->level)) {
            continue;
        }

        const node_id_t *neighbors = nullptr;
        uint16_t neighbor_count = 0;
        if (layer_num == 0) {
            neighbors = cur_h->Level0Neighbors();
            neighbor_count = cur_h->Level0Count();
        } else {
            neighbors = cur_h->UpperNeighbors(layer_num - 1);
            neighbor_count = cur_h->UpperCount(layer_num - 1);
        }

        if (neighbors == nullptr) {
            continue;
        }

        for (uint16_t i = 0; i < neighbor_count; i++) {
            node_id_t neighbor_id = neighbors[i];
            if (neighbor_id == INVALID_NODE_ID) {
                continue;
            }
            auto nb_h = store_.PinNode(neighbor_id);
            if (!nb_h) {
                continue;
            }
            const auto *nb_header = nb_h->Header();
            if (nb_header == nullptr || nb_header->deleted) {
                continue;
            }
            if (!visited.Insert(neighbor_id)) {
                continue;
            }

            float nd = distance_func(query, nb_h->Vector(), dim_);
            if (static_cast<int>(visited_queue.size()) < ef || nd < visited_queue.top().distance) {
                candidate_queue.push({neighbor_id, nd});
                visited_queue.push({neighbor_id, nd});
                if (static_cast<int>(visited_queue.size()) > ef) {
                    visited_queue.pop();
                }
            }
        }
    }

    candidates.clear();
    while (!visited_queue.empty()) {
        auto cand = visited_queue.top();
        visited_queue.pop();
        auto h = store_.PinNode(cand.node_id);
        if (h && h->Header() != nullptr && !h->Header()->deleted) {
            candidates.push_back(cand);
        }
    }
}

std::vector<node_id_t> HNSWGraph::SelectNeighbors(const std::vector<GraphCandidate> &candidates,
                                                  int max_m, distance_func_t distance_func) {
    std::vector<node_id_t> result;
    result.reserve(static_cast<size_t>(max_m));

    std::vector<GraphCandidate> sorted = candidates;
    std::sort(sorted.begin(), sorted.end(), [](const GraphCandidate &a, const GraphCandidate &b) {
        return a.distance < b.distance;
    });

    for (const auto &cand : sorted) {
        if (static_cast<int>(result.size()) >= max_m) {
            break;
        }

        auto cand_h = store_.PinNode(cand.node_id);
        if (!cand_h) {
            continue;
        }

        bool good = true;
        const float *cand_vec = cand_h->Vector();
        for (auto sel_id : result) {
            auto sel_h = store_.PinNode(sel_id);
            if (!sel_h) {
                continue;
            }
            float dist_to_selected = distance_func(cand_vec, sel_h->Vector(), dim_);
            if (dist_to_selected <= cand.distance) {
                good = false;
                break;
            }
        }
        if (good) {
            result.push_back(cand.node_id);
        }
    }

    if (static_cast<int>(result.size()) < max_m) {
        for (const auto &cand : sorted) {
            if (static_cast<int>(result.size()) >= max_m) {
                break;
            }
            bool exists = std::find(result.begin(), result.end(), cand.node_id) != result.end();
            if (!exists) {
                result.push_back(cand.node_id);
            }
        }
    }

    return result;
}

void HNSWGraph::InsertNode(node_id_t new_node_id, int m_param, int ef_construction,
                           distance_func_t distance_func) {
    auto pin_new_start = SteadyClock::now();
    auto new_h = store_.PinNode(new_node_id);
    insert_profile_.pin_new_node_ms += ElapsedMs(pin_new_start);
    if (!new_h || new_h->Header() == nullptr || new_h->Header()->deleted) {
        return;
    }

    int node_level = static_cast<int>(new_h->Header()->level);
    if (!has_entry_point_) {
        entry_point_ = new_node_id;
        has_entry_point_ = true;
        max_level_ = node_level;
        return;
    }

    node_id_t ep = entry_point_;
    VisitedSet visited(std::max<uint64_t>(node_count_, 128));
    std::vector<GraphCandidate> candidates;

    for (int level = max_level_; level > node_level; level--) {
        candidates.clear();
        visited.Clear();
        auto upper_descent_start = SteadyClock::now();
        SearchLayer(new_h->Vector(), ep, 1, level, candidates, visited, distance_func);
        insert_profile_.upper_descent_ms += ElapsedMs(upper_descent_start);
        insert_profile_.upper_descent_calls++;
        if (!candidates.empty()) {
            std::sort(candidates.begin(), candidates.end(), [](const GraphCandidate &a, const GraphCandidate &b) {
                return a.distance < b.distance;
            });
            ep = candidates.front().node_id;
        }
    }

    const float *new_vec = new_h->Vector();
    for (int level = std::min(node_level, max_level_); level >= 0; level--) {
        candidates.clear();
        visited.Clear();
        auto search_start = SteadyClock::now();
        SearchLayer(new_vec, ep, ef_construction, level, candidates, visited, distance_func);
        insert_profile_.lower_level_search_ms += ElapsedMs(search_start);
        insert_profile_.lower_level_search_calls++;

        int layer_m = GraphIndexConfig::GetLayerM(m_param, level);
        auto select_start = SteadyClock::now();
        auto selected = SelectNeighbors(candidates, layer_m, distance_func);
        insert_profile_.select_neighbors_ms += ElapsedMs(select_start);
        insert_profile_.select_neighbors_calls++;

        auto write_new_start = SteadyClock::now();
        auto new_u = store_.PinNodeForUpdate(new_node_id);
        if (!new_u) {
            continue;
        }
        if (level == 0) {
            auto *new_neighbors = new_u->MutableLevel0Neighbors();
            uint16_t sel_count = static_cast<uint16_t>(selected.size());
            for (uint16_t i = 0; i < sel_count; i++) {
                new_neighbors[i] = selected[i];
            }
            new_u->SetLevel0Count(sel_count);
        } else {
            auto *new_neighbors = new_u->MutableUpperNeighbors(level - 1);
            uint16_t sel_count = static_cast<uint16_t>(selected.size());
            for (uint16_t i = 0; i < sel_count; i++) {
                new_neighbors[i] = selected[i];
            }
            new_u->SetUpperCount(level - 1, sel_count);
        }
        insert_profile_.write_new_node_neighbors_ms += ElapsedMs(write_new_start);

        for (auto sel_id : selected) {
            auto backlink_start = SteadyClock::now();
            auto sel_h = store_.PinNode(sel_id);
            if (!sel_h || sel_h->Header() == nullptr || sel_h->Header()->deleted) {
                insert_profile_.backlink_update_ms += ElapsedMs(backlink_start);
                continue;
            }

            auto sel_u = store_.PinNodeForUpdate(sel_id);
            if (!sel_u) {
                insert_profile_.backlink_update_ms += ElapsedMs(backlink_start);
                continue;
            }

            node_id_t *sel_neighbors = nullptr;
            uint16_t sel_count = 0;
            if (level == 0) {
                sel_neighbors = sel_u->MutableLevel0Neighbors();
                sel_count = sel_u->Level0Count();
            } else {
                sel_neighbors = sel_u->MutableUpperNeighbors(level - 1);
                sel_count = sel_u->UpperCount(level - 1);
            }
            if (sel_neighbors == nullptr) {
                insert_profile_.backlink_update_ms += ElapsedMs(backlink_start);
                continue;
            }

            if (sel_count < static_cast<uint16_t>(layer_m)) {
                sel_neighbors[sel_count] = new_node_id;
                if (level == 0) {
                    sel_u->SetLevel0Count(sel_count + 1);
                } else {
                    sel_u->SetUpperCount(level - 1, sel_count + 1);
                }
                insert_profile_.backlink_update_ms += ElapsedMs(backlink_start);
            } else {
                std::vector<GraphCandidate> neighbor_candidates;
                neighbor_candidates.reserve(sel_count + 1);

                const float *sel_vec = sel_h->Vector();
                for (uint16_t i = 0; i < sel_count; i++) {
                    node_id_t nn_id = sel_neighbors[i];
                    if (nn_id == INVALID_NODE_ID) {
                        continue;
                    }
                    auto nn_h = store_.PinNode(nn_id);
                    if (!nn_h || nn_h->Header() == nullptr || nn_h->Header()->deleted) {
                        continue;
                    }
                    float d = distance_func(sel_vec, nn_h->Vector(), dim_);
                    neighbor_candidates.push_back({nn_id, d});
                }

                float new_dist = distance_func(sel_vec, new_vec, dim_);
                neighbor_candidates.push_back({new_node_id, new_dist});

                auto prune_start = SteadyClock::now();
                auto pruned = SelectNeighbors(neighbor_candidates, layer_m, distance_func);
                insert_profile_.backlink_prune_ms += ElapsedMs(prune_start);
                insert_profile_.backlink_prune_calls++;
                uint16_t pruned_count = static_cast<uint16_t>(pruned.size());
                for (uint16_t i = 0; i < pruned_count; i++) {
                    sel_neighbors[i] = pruned[i];
                }
                for (uint16_t i = pruned_count; i < static_cast<uint16_t>(layer_m); i++) {
                    sel_neighbors[i] = INVALID_NODE_ID;
                }
                if (level == 0) {
                    sel_u->SetLevel0Count(pruned_count);
                } else {
                    sel_u->SetUpperCount(level - 1, pruned_count);
                }
                insert_profile_.backlink_update_ms += ElapsedMs(backlink_start);
            }
        }

        if (!candidates.empty()) {
            std::sort(candidates.begin(), candidates.end(), [](const GraphCandidate &a, const GraphCandidate &b) {
                return a.distance < b.distance;
            });
            ep = candidates.front().node_id;
        }
    }

    auto entry_update_start = SteadyClock::now();
    if (node_level > max_level_) {
        entry_point_ = new_node_id;
        max_level_ = node_level;
    }
    insert_profile_.entry_update_ms += ElapsedMs(entry_update_start);
}

void HNSWGraph::BruteForceSearch(const float *query_vec, uint32_t k,
                                 std::vector<row_id_t> &out_row_ids,
                                 std::vector<float> &out_distances,
                                 distance_func_t distance_func) {
    std::vector<GraphCandidate> all;
    all.reserve(static_cast<size_t>(node_count_));

    store_.ForEachNode([&](node_id_t node_id) {
        auto h = store_.PinNode(node_id);
        if (!h || h->Header() == nullptr || h->Header()->deleted) {
            return;
        }
        float dist = distance_func(query_vec, h->Vector(), dim_);
        all.push_back({node_id, dist});
    });

    std::sort(all.begin(), all.end(), [&](const GraphCandidate &a, const GraphCandidate &b) {
        if (a.distance != b.distance) {
            return a.distance < b.distance;
        }
        auto ha = store_.PinNode(a.node_id);
        auto hb = store_.PinNode(b.node_id);
        return ha->Header()->row_id < hb->Header()->row_id;
    });

    uint32_t count = std::min(k, static_cast<uint32_t>(all.size()));
    for (uint32_t i = 0; i < count; i++) {
        auto h = store_.PinNode(all[i].node_id);
        out_row_ids.push_back(h->Header()->row_id);
        out_distances.push_back(all[i].distance);
    }
}

void HNSWGraph::SearchLayerFiltered(const float *query, node_id_t ep, int ef, int layer_num,
                                    std::vector<GraphCandidate> &candidates, VisitedSet &visited,
                                    distance_func_t distance_func, const FilterPredicate &filter) {
    std::priority_queue<GraphCandidate, std::vector<GraphCandidate>, std::greater<GraphCandidate>> candidate_queue;
    std::priority_queue<GraphCandidate, std::vector<GraphCandidate>, std::less<GraphCandidate>> visited_queue;

    auto ep_h = store_.PinNode(ep);
    if (!ep_h || ep_h->Header() == nullptr || ep_h->Header()->deleted) {
        return;
    }

    float ep_dist = distance_func(query, ep_h->Vector(), dim_);
    candidate_queue.push({ep, ep_dist});
    visited.Insert(ep);

    auto *ep_meta = ep_h->Metadata();
    if (!ep_meta || filter.Matches(ep_meta)) {
        visited_queue.push({ep, ep_dist});
    }

    double selectivity = filter.Selectivity();
    int adaptive_ef = ef;
    if (selectivity > 0.0 && selectivity < 1.0) {
        adaptive_ef = std::min(static_cast<int>(ef / selectivity), ef * 10);
    }

    while (!candidate_queue.empty()) {
        auto current = candidate_queue.top();
        candidate_queue.pop();

        if (static_cast<int>(visited_queue.size()) >= adaptive_ef &&
            !visited_queue.empty() && current.distance > visited_queue.top().distance) {
            break;
        }

        auto cur_h = store_.PinNode(current.node_id);
        if (!cur_h || cur_h->Header() == nullptr || cur_h->Header()->deleted) {
            continue;
        }
        if (layer_num > static_cast<int>(cur_h->Header()->level)) {
            continue;
        }

        const node_id_t *neighbors = nullptr;
        uint16_t neighbor_count = 0;
        if (layer_num == 0) {
            neighbors = cur_h->Level0Neighbors();
            neighbor_count = cur_h->Level0Count();
        } else {
            neighbors = cur_h->UpperNeighbors(layer_num - 1);
            neighbor_count = cur_h->UpperCount(layer_num - 1);
        }
        if (!neighbors) {
            continue;
        }

        for (uint16_t i = 0; i < neighbor_count; i++) {
            node_id_t neighbor_id = neighbors[i];
            if (neighbor_id == INVALID_NODE_ID) {
                continue;
            }
            auto nb_h = store_.PinNode(neighbor_id);
            if (!nb_h || nb_h->Header() == nullptr || nb_h->Header()->deleted) {
                continue;
            }
            if (!visited.Insert(neighbor_id)) {
                continue;
            }

            float neighbor_dist = distance_func(query, nb_h->Vector(), dim_);
            candidate_queue.push({neighbor_id, neighbor_dist});

            auto *nb_meta = nb_h->Metadata();
            if (nb_meta && !filter.Matches(nb_meta)) {
                continue;
            }

            if (static_cast<int>(visited_queue.size()) < adaptive_ef ||
                neighbor_dist < visited_queue.top().distance) {
                visited_queue.push({neighbor_id, neighbor_dist});
                if (static_cast<int>(visited_queue.size()) > adaptive_ef) {
                    visited_queue.pop();
                }
            }
        }
    }

    candidates.clear();
    while (!visited_queue.empty()) {
        auto cand = visited_queue.top();
        visited_queue.pop();
        auto h = store_.PinNode(cand.node_id);
        if (h && h->Header() != nullptr && !h->Header()->deleted) {
            candidates.push_back(cand);
        }
    }
}

void HNSWGraph::BruteForceFilteredSearch(const float *query_vec, uint32_t k,
                                         std::vector<row_id_t> &out_row_ids,
                                         std::vector<float> &out_distances,
                                         distance_func_t distance_func, const FilterPredicate &filter) {
    std::vector<GraphCandidate> all;
    all.reserve(static_cast<size_t>(node_count_));

    store_.ForEachNode([&](node_id_t node_id) {
        auto h = store_.PinNode(node_id);
        if (!h || h->Header() == nullptr || h->Header()->deleted) {
            return;
        }
        auto *meta = h->Metadata();
        if (meta && !filter.Matches(meta)) {
            return;
        }
        float dist = distance_func(query_vec, h->Vector(), dim_);
        all.push_back({node_id, dist});
    });

    std::sort(all.begin(), all.end(), [&](const GraphCandidate &a, const GraphCandidate &b) {
        if (a.distance != b.distance) {
            return a.distance < b.distance;
        }
        auto ha = store_.PinNode(a.node_id);
        auto hb = store_.PinNode(b.node_id);
        return ha->Header()->row_id < hb->Header()->row_id;
    });

    uint32_t count = std::min(k, static_cast<uint32_t>(all.size()));
    for (uint32_t i = 0; i < count; i++) {
        auto h = store_.PinNode(all[i].node_id);
        out_row_ids.push_back(h->Header()->row_id);
        out_distances.push_back(all[i].distance);
    }
}

void HNSWGraph::Search(const float *query_vec, uint32_t k, int ef,
                       std::vector<row_id_t> &out_row_ids,
                       std::vector<float> &out_distances,
                       uint64_t brute_force_threshold) {
    if (!has_entry_point_ || node_count_ == 0) {
        return;
    }

    if (node_count_ <= brute_force_threshold) {
        BruteForceSearch(query_vec, k, out_row_ids, out_distances, distance_func_);
        return;
    }

    int actual_ef = ef > 0 ? ef : GraphIndexConfig::DEFAULT_EF_SEARCH;

    node_id_t ep = entry_point_;
    auto ep_h = store_.PinNode(ep);
    if (!ep_h || ep_h->Header() == nullptr || ep_h->Header()->deleted) {
        BruteForceSearch(query_vec, k, out_row_ids, out_distances, distance_func_);
        return;
    }

    VisitedSet visited(std::max<uint64_t>(node_count_, 128));
    std::vector<GraphCandidate> candidates;

    for (int level = max_level_; level > 0; level--) {
        candidates.clear();
        visited.Clear();
        SearchLayer(query_vec, ep, 1, level, candidates, visited, distance_func_);
        if (!candidates.empty()) {
            std::sort(candidates.begin(), candidates.end(), [](const GraphCandidate &a, const GraphCandidate &b) {
                return a.distance < b.distance;
            });
            ep = candidates.front().node_id;
        }
    }

    candidates.clear();
    visited.Clear();
    SearchLayer(query_vec, ep, std::max(actual_ef, static_cast<int>(k)), 0,
                candidates, visited, distance_func_);

    std::sort(candidates.begin(), candidates.end(), [&](const GraphCandidate &a, const GraphCandidate &b) {
        if (a.distance != b.distance) {
            return a.distance < b.distance;
        }
        auto ha = store_.PinNode(a.node_id);
        auto hb = store_.PinNode(b.node_id);
        return ha->Header()->row_id < hb->Header()->row_id;
    });

    uint32_t count = std::min(k, static_cast<uint32_t>(candidates.size()));
    for (uint32_t i = 0; i < count; i++) {
        auto h = store_.PinNode(candidates[i].node_id);
        out_row_ids.push_back(h->Header()->row_id);
        out_distances.push_back(candidates[i].distance);
    }
}

void HNSWGraph::FilteredSearch(const float *query_vec, uint32_t k, int ef,
                               std::vector<row_id_t> &out_row_ids,
                               std::vector<float> &out_distances,
                               const FilterPredicate &filter,
                               uint64_t brute_force_threshold) {
    if (!has_entry_point_ || node_count_ == 0) {
        return;
    }

    double selectivity = filter.Selectivity();
    if (selectivity < 0.01 && node_count_ > 10000) {
        BruteForceFilteredSearch(query_vec, k, out_row_ids, out_distances, distance_func_, filter);
        return;
    }
    if (node_count_ <= brute_force_threshold) {
        BruteForceFilteredSearch(query_vec, k, out_row_ids, out_distances, distance_func_, filter);
        return;
    }

    if (selectivity > 0.90) {
        uint64_t fetch_k64 = std::min<uint64_t>(static_cast<uint64_t>(k / selectivity) + k, node_count_);
        uint32_t fetch_k = static_cast<uint32_t>(std::min<uint64_t>(fetch_k64, std::numeric_limits<uint32_t>::max()));
        int post_ef = std::max(ef, static_cast<int>(fetch_k));

        node_id_t ep = entry_point_;
        auto ep_h = store_.PinNode(ep);
        if (!ep_h || ep_h->Header() == nullptr || ep_h->Header()->deleted) {
            BruteForceFilteredSearch(query_vec, k, out_row_ids, out_distances, distance_func_, filter);
            return;
        }

        VisitedSet visited(std::max<uint64_t>(node_count_, 128));
        std::vector<GraphCandidate> candidates;

        for (int level = max_level_; level > 0; level--) {
            candidates.clear();
            visited.Clear();
            SearchLayer(query_vec, ep, 1, level, candidates, visited, distance_func_);
            if (!candidates.empty()) {
                std::sort(candidates.begin(), candidates.end(), [](const GraphCandidate &a, const GraphCandidate &b) {
                    return a.distance < b.distance;
                });
                ep = candidates.front().node_id;
            }
        }

        candidates.clear();
        visited.Clear();
        SearchLayer(query_vec, ep, post_ef, 0, candidates, visited, distance_func_);

        std::sort(candidates.begin(), candidates.end(), [&](const GraphCandidate &a, const GraphCandidate &b) {
            if (a.distance != b.distance) {
                return a.distance < b.distance;
            }
            auto ha = store_.PinNode(a.node_id);
            auto hb = store_.PinNode(b.node_id);
            return ha->Header()->row_id < hb->Header()->row_id;
        });

        for (const auto &cand : candidates) {
            if (out_row_ids.size() >= k) {
                break;
            }
            auto h = store_.PinNode(cand.node_id);
            auto *meta = h->Metadata();
            if (meta && !filter.Matches(meta)) {
                continue;
            }
            out_row_ids.push_back(h->Header()->row_id);
            out_distances.push_back(cand.distance);
        }
        return;
    }

    int actual_ef = ef > 0 ? ef : GraphIndexConfig::DEFAULT_EF_SEARCH;
    node_id_t ep = entry_point_;
    auto ep_h = store_.PinNode(ep);
    if (!ep_h || ep_h->Header() == nullptr || ep_h->Header()->deleted) {
        BruteForceFilteredSearch(query_vec, k, out_row_ids, out_distances, distance_func_, filter);
        return;
    }

    VisitedSet visited(std::max<uint64_t>(node_count_, 128));
    std::vector<GraphCandidate> candidates;

    for (int level = max_level_; level > 0; level--) {
        candidates.clear();
        visited.Clear();
        SearchLayer(query_vec, ep, 1, level, candidates, visited, distance_func_);
        if (!candidates.empty()) {
            std::sort(candidates.begin(), candidates.end(), [](const GraphCandidate &a, const GraphCandidate &b) {
                return a.distance < b.distance;
            });
            ep = candidates.front().node_id;
        }
    }

    candidates.clear();
    visited.Clear();
    SearchLayerFiltered(query_vec, ep, std::max(actual_ef, static_cast<int>(k)), 0,
                        candidates, visited, distance_func_, filter);

    std::sort(candidates.begin(), candidates.end(), [&](const GraphCandidate &a, const GraphCandidate &b) {
        if (a.distance != b.distance) {
            return a.distance < b.distance;
        }
        auto ha = store_.PinNode(a.node_id);
        auto hb = store_.PinNode(b.node_id);
        return ha->Header()->row_id < hb->Header()->row_id;
    });

    uint32_t count = std::min(k, static_cast<uint32_t>(candidates.size()));
    for (uint32_t i = 0; i < count; i++) {
        auto h = store_.PinNode(candidates[i].node_id);
        out_row_ids.push_back(h->Header()->row_id);
        out_distances.push_back(candidates[i].distance);
    }
}

void HNSWGraph::SearchWithQuantizedCodes(const float *query_vec, uint32_t k, int ef,
                                         std::vector<row_id_t> &out_row_ids,
                                         std::vector<float> &out_distances,
                                         QuantDistancer &distancer,
                                         const std::function<const uint8_t *(node_id_t)> &code_lookup,
                                         uint64_t brute_force_threshold) {
    if (!has_entry_point_ || node_count_ == 0) {
        return;
    }
    if (!code_lookup || distancer.CodeSize() == 0) {
        Search(query_vec, k, ef, out_row_ids, out_distances, brute_force_threshold);
        return;
    }
    if (node_count_ <= brute_force_threshold) {
        BruteForceSearch(query_vec, k, out_row_ids, out_distances, distance_func_);
        return;
    }

    distancer.PrepareQuery(query_vec, dim_);
    int actual_ef = ef > 0 ? ef : GraphIndexConfig::DEFAULT_EF_SEARCH;

    node_id_t ep = entry_point_;
    auto ep_h = store_.PinNode(ep);
    if (!ep_h || ep_h->Header() == nullptr || ep_h->Header()->deleted) {
        BruteForceSearch(query_vec, k, out_row_ids, out_distances, distance_func_);
        return;
    }

    VisitedSet visited(std::max<uint64_t>(node_count_, 128));
    std::vector<GraphCandidate> candidates;

    for (int level = max_level_; level > 0; level--) {
        candidates.clear();
        visited.Clear();
        SearchLayer(query_vec, ep, 1, level, candidates, visited, distance_func_);
        if (!candidates.empty()) {
            std::sort(candidates.begin(), candidates.end(), [](const GraphCandidate &a, const GraphCandidate &b) {
                return a.distance < b.distance;
            });
            ep = candidates.front().node_id;
        }
    }

    std::priority_queue<GraphCandidate, std::vector<GraphCandidate>, std::greater<GraphCandidate>> cand_q;
    std::priority_queue<GraphCandidate, std::vector<GraphCandidate>, std::less<GraphCandidate>> visit_q;
    VisitedSet vis(std::max<uint64_t>(node_count_, 128));

    auto quant_dist = [&](node_id_t node_id) -> float {
        const uint8_t *code = code_lookup(node_id);
        if (code) {
            return distancer.DistanceSingle(code);
        }
        auto h = store_.PinNode(node_id);
        if (!h || h->Header() == nullptr) {
            return std::numeric_limits<float>::infinity();
        }
        return distance_func_(query_vec, h->Vector(), dim_);
    };

    int search_ef = std::max(actual_ef, static_cast<int>(k));
    float ep_dist = quant_dist(ep);
    cand_q.push({ep, ep_dist});
    visit_q.push({ep, ep_dist});
    vis.Insert(ep);

    while (!cand_q.empty()) {
        auto current = cand_q.top();
        cand_q.pop();
        if (!visit_q.empty() && current.distance > visit_q.top().distance) {
            break;
        }

        auto cur_h = store_.PinNode(current.node_id);
        if (!cur_h || cur_h->Header() == nullptr || cur_h->Header()->deleted) {
            continue;
        }

        const node_id_t *neighbors = cur_h->Level0Neighbors();
        uint16_t neighbor_count = cur_h->Level0Count();
        if (!neighbors) {
            continue;
        }

        for (uint16_t i = 0; i < neighbor_count; i++) {
            node_id_t neighbor_id = neighbors[i];
            if (neighbor_id == INVALID_NODE_ID || !vis.Insert(neighbor_id)) {
                continue;
            }
            auto nb_h = store_.PinNode(neighbor_id);
            if (!nb_h || nb_h->Header() == nullptr || nb_h->Header()->deleted) {
                continue;
            }
            float nd = quant_dist(neighbor_id);
            if (static_cast<int>(visit_q.size()) < search_ef || nd < visit_q.top().distance) {
                cand_q.push({neighbor_id, nd});
                visit_q.push({neighbor_id, nd});
                if (static_cast<int>(visit_q.size()) > search_ef) {
                    visit_q.pop();
                }
            }
        }
    }

    candidates.clear();
    while (!visit_q.empty()) {
        auto cand = visit_q.top();
        visit_q.pop();
        auto h = store_.PinNode(cand.node_id);
        if (h && h->Header() != nullptr && !h->Header()->deleted) {
            candidates.push_back(cand);
        }
    }

    for (auto &cand : candidates) {
        auto h = store_.PinNode(cand.node_id);
        if (!h || h->Header() == nullptr) {
            cand.distance = std::numeric_limits<float>::infinity();
            continue;
        }
        cand.distance = distance_func_(query_vec, h->Vector(), dim_);
    }

    std::sort(candidates.begin(), candidates.end(), [&](const GraphCandidate &a, const GraphCandidate &b) {
        if (a.distance != b.distance) {
            return a.distance < b.distance;
        }
        auto ha = store_.PinNode(a.node_id);
        auto hb = store_.PinNode(b.node_id);
        return ha->Header()->row_id < hb->Header()->row_id;
    });

    uint32_t count = std::min(k, static_cast<uint32_t>(candidates.size()));
    for (uint32_t i = 0; i < count; i++) {
        auto h = store_.PinNode(candidates[i].node_id);
        out_row_ids.push_back(h->Header()->row_id);
        out_distances.push_back(candidates[i].distance);
    }
}

void HNSWGraph::LoadState(bool has_entry_point, node_id_t entry_point, int max_level, uint64_t node_count) {
    if (has_entry_point) {
        auto h = store_.PinNode(entry_point);
        if (!h || h->Header() == nullptr || h->Header()->deleted) {
            has_entry_point_ = false;
            entry_point_ = 0;
            max_level_ = 0;
            node_count_ = store_.GetNodeCount();
            return;
        }
    }
    has_entry_point_ = has_entry_point;
    entry_point_ = entry_point;
    max_level_ = max_level;
    node_count_ = node_count > 0 ? node_count : store_.GetNodeCount();
}

} // namespace vex
