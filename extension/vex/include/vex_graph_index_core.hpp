#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "vex_distance.hpp"
#include "vex_quantizer.hpp"

#include <vector>

namespace duckdb {

// ============================================================
// Graph Index Parameters
// ============================================================
struct GraphIndexConfig {
	static constexpr int DEFAULT_M = 16;
	static constexpr int MIN_M = 2;
	static constexpr int MAX_M = 100;
	static constexpr int DEFAULT_EF_CONSTRUCTION = 64;
	static constexpr int MIN_EF_CONSTRUCTION = 4;
	static constexpr int MAX_EF_CONSTRUCTION = 1000;
	static constexpr int DEFAULT_EF_SEARCH = 40;
	static constexpr int MIN_EF_SEARCH = 1;
	static constexpr int MAX_EF_SEARCH = 1000;

	int m = DEFAULT_M;
	int ef_construction = DEFAULT_EF_CONSTRUCTION;

	static double GetMl(int m) {
		return 1.0 / std::log(m);
	}

	static int GetMaxLevel(int m) {
		return 32;
	}

	static int GetLayerM(int m, int level) {
		return level == 0 ? m * 2 : m;
	}
};

// ============================================================
// Graph Node
// ============================================================
struct GraphNode {
	row_t row_id;
	std::vector<float> owned_vector;
	const float *vector;
	uint8_t level;
	bool deleted;
	std::vector<std::vector<GraphNode *>> neighbors;

	GraphNode(row_t id, const float *vec, uint32_t dim, uint8_t lvl, int m)
	    : row_id(id), owned_vector(vec, vec + dim), vector(owned_vector.data()), level(lvl), deleted(false) {
		neighbors.reserve(lvl + 1);
		for (uint8_t l = 0; l <= lvl; l++) {
			neighbors.emplace_back();
			int max_conn = (l == 0) ? m * 2 : m;
			neighbors[l].reserve(max_conn);
		}
	}
};

// ============================================================
// Search Candidate
// ============================================================
struct GraphCandidate {
	GraphNode *node;
	float distance;

	bool operator>(const GraphCandidate &other) const {
		return distance > other.distance;
	}

	bool operator<(const GraphCandidate &other) const {
		return distance < other.distance;
	}
};

// ============================================================
// GraphIndexCore — shared graph core used by GraphIndex and HybridIndex
// ============================================================
struct GraphIndexCore {
	std::vector<unique_ptr<GraphNode>> nodes;
	GraphNode *entry_point = nullptr;
	int max_level = 0;
	idx_t node_count = 0;

	//! Below this threshold, use brute force instead of graph traversal
	static constexpr idx_t BRUTE_FORCE_THRESHOLD = 64;

	//! Optional PQ quantizer for accelerated distance computation
	vex::ProductQuantizer pq;
	std::vector<uint8_t> pq_codes; // flat array: node_count * pq.CodeSize()

	//! Core graph algorithms
	void SearchLayer(const float *query, GraphNode *ep, int ef, int layer_num,
	                 std::vector<GraphCandidate> &candidates, unordered_set<row_t> &visited,
	                 vex::distance_func_t distance_func, uint32_t dimension);

	std::vector<GraphNode *> SelectNeighbors(const std::vector<GraphCandidate> &candidates, int m);

	void InsertNode(GraphNode *new_node, int m, int ef_construction,
	                vex::distance_func_t distance_func, uint32_t dimension);

	//! Search with automatic brute-force fallback for small graphs
	void Search(const float *query_vec, idx_t k, int ef,
	            std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
	            vex::distance_func_t distance_func, uint32_t dimension, int m);

	//! Brute-force search for small partitions
	void BruteForceSearch(const float *query_vec, idx_t k,
	                      std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
	                      vex::distance_func_t distance_func, uint32_t dimension);

	//! PQ-accelerated search: use PQ distances for graph traversal, rerank top results
	void SearchWithPQ(const float *query_vec, idx_t k, int ef,
	                  std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
	                  vex::distance_func_t distance_func, uint32_t dimension, int m);

	//! Train PQ codebook from current nodes
	void TrainPQ(uint32_t pq_m);

	//! Encode all nodes with current PQ codebook
	void EncodeAllPQ();

	//! Get PQ code for a node by index
	const uint8_t *GetPQCode(idx_t node_idx) const;

	//! Remove deleted nodes and clean neighbor lists
	void Vacuum();

	//! Clear all data
	void Clear();
};

} // namespace duckdb
