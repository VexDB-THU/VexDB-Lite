#ifndef VEX_GRAPH_ALGO_HPP
#define VEX_GRAPH_ALGO_HPP

#include "vex/vex_concurrency.hpp"
#include "vex/vex_config.hpp"
#include "vex/vex_distance.hpp"
#include "vex/vex_filter.hpp"
#include "vex/vex_node_store.hpp"
#include "vex/vex_quant_distancer.hpp"

#include <cstdint>
#include <functional>
#include <random>
#include <vector>

namespace vex {

struct GraphCandidate {
    node_id_t node_id;
    float distance;

    bool operator>(const GraphCandidate &other) const {
        return distance > other.distance;
    }

    bool operator<(const GraphCandidate &other) const {
        return distance < other.distance;
    }
};

struct HNSWInsertProfile {
    uint64_t add_point_calls = 0;

    double allocate_node_ms = 0.0;
    double insert_total_ms = 0.0;
    double pin_new_node_ms = 0.0;
    double upper_descent_ms = 0.0;
    double lower_level_search_ms = 0.0;
    double select_neighbors_ms = 0.0;
    double write_new_node_neighbors_ms = 0.0;
    double backlink_update_ms = 0.0;
    double backlink_prune_ms = 0.0;
    double entry_update_ms = 0.0;

    uint64_t upper_descent_calls = 0;
    uint64_t lower_level_search_calls = 0;
    uint64_t select_neighbors_calls = 0;
    uint64_t backlink_prune_calls = 0;
};

class HNSWGraph {
public:
    explicit HNSWGraph(NodeStore &store, Metric metric = Metric::L2,
                       int m = GraphIndexConfig::DEFAULT_M,
                       int ef_construction = GraphIndexConfig::DEFAULT_EF_CONSTRUCTION);

    node_id_t AddPoint(row_id_t row_id, const float *vec, uint32_t dim);
    node_id_t AddPointWithLevel(row_id_t row_id, const float *vec, uint32_t dim, uint8_t level);

    void Search(const float *query_vec, uint32_t k, int ef,
                std::vector<row_id_t> &out_row_ids,
                std::vector<float> &out_distances,
                uint64_t brute_force_threshold = 64);
    void FilteredSearch(const float *query_vec, uint32_t k, int ef,
                        std::vector<row_id_t> &out_row_ids,
                        std::vector<float> &out_distances,
                        const FilterPredicate &filter,
                        uint64_t brute_force_threshold = 64);
    void SearchWithQuantizedCodes(const float *query_vec, uint32_t k, int ef,
                                  std::vector<row_id_t> &out_row_ids,
                                  std::vector<float> &out_distances,
                                  QuantDistancer &distancer,
                                  const std::function<const uint8_t *(node_id_t)> &code_lookup,
                                  uint64_t brute_force_threshold = 64);

    uint64_t NodeCount() const {
        return node_count_;
    }

    node_id_t EntryPoint() const {
        return entry_point_;
    }

    bool HasEntryPoint() const {
        return has_entry_point_;
    }

    int MaxLevel() const {
        return max_level_;
    }

    const HNSWInsertProfile &GetInsertProfile() const {
        return insert_profile_;
    }

    void ResetInsertProfile() {
        insert_profile_ = {};
    }

    void LoadState(bool has_entry_point, node_id_t entry_point, int max_level, uint64_t node_count);

private:
    void SearchLayer(const float *query, node_id_t ep, int ef, int layer_num,
                     std::vector<GraphCandidate> &candidates, VisitedSet &visited,
                     distance_func_t distance_func);

    std::vector<node_id_t> SelectNeighbors(const std::vector<GraphCandidate> &candidates,
                                           int max_m, distance_func_t distance_func);

    void InsertNode(node_id_t new_node_id, int m_param, int ef_construction,
                    distance_func_t distance_func);

    void BruteForceSearch(const float *query_vec, uint32_t k,
                          std::vector<row_id_t> &out_row_ids,
                          std::vector<float> &out_distances,
                          distance_func_t distance_func);
    void SearchLayerFiltered(const float *query, node_id_t ep, int ef, int layer_num,
                             std::vector<GraphCandidate> &candidates, VisitedSet &visited,
                             distance_func_t distance_func, const FilterPredicate &filter);
    void BruteForceFilteredSearch(const float *query_vec, uint32_t k,
                                  std::vector<row_id_t> &out_row_ids,
                                  std::vector<float> &out_distances,
                                  distance_func_t distance_func, const FilterPredicate &filter);

    int GetRandomLevel();

private:
    NodeStore &store_;
    distance_func_t distance_func_;

    int m_;
    int ef_construction_;
    uint32_t dim_;

    node_id_t entry_point_ = 0;
    bool has_entry_point_ = false;
    int max_level_ = 0;
    uint64_t node_count_ = 0;
    HNSWInsertProfile insert_profile_{};

    std::mt19937 rng_;
    std::uniform_real_distribution<double> dist_;
};

} // namespace vex

#endif // VEX_GRAPH_ALGO_HPP
