#include "vex_graph_index_core.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>
#include <unordered_map>

namespace duckdb {

// ============================================================
// SearchLayer (Algorithm 3 from HNSW paper)
// ============================================================

void GraphIndexCore::SearchLayer(const float *query, GraphNode *ep, int ef, int layer_num,
                                 std::vector<GraphCandidate> &candidates, unordered_set<row_t> &visited,
                                 vex::distance_func_t distance_func, uint32_t dimension) {
	std::priority_queue<GraphCandidate, std::vector<GraphCandidate>, std::greater<GraphCandidate>> candidate_queue;
	std::priority_queue<GraphCandidate, std::vector<GraphCandidate>, std::less<GraphCandidate>> visited_queue;

	if (!ep || ep->deleted) {
		return;
	}

	float dist = distance_func(query, ep->vector, dimension);
	candidate_queue.push({ep, dist});
	visited_queue.push({ep, dist});
	visited.insert(ep->row_id);

	while (!candidate_queue.empty()) {
		auto current = candidate_queue.top();
		candidate_queue.pop();

		if (!visited_queue.empty() && current.distance > visited_queue.top().distance) {
			break;
		}

		if (layer_num >= static_cast<int>(current.node->neighbors.size())) {
			continue;
		}
		for (auto *neighbor : current.node->neighbors[layer_num]) {
			if (!neighbor || neighbor->deleted || visited.find(neighbor->row_id) != visited.end()) {
				continue;
			}
			visited.insert(neighbor->row_id);

			float neighbor_dist = distance_func(query, neighbor->vector, dimension);
			if (static_cast<int>(visited_queue.size()) < ef ||
			    neighbor_dist < visited_queue.top().distance) {
				candidate_queue.push({neighbor, neighbor_dist});
				visited_queue.push({neighbor, neighbor_dist});
				if (static_cast<int>(visited_queue.size()) > ef) {
					visited_queue.pop();
				}
			}
		}
	}

	candidates.clear();
	while (!visited_queue.empty()) {
		auto &c = visited_queue.top();
		if (!c.node->deleted) {
			candidates.push_back(c);
		}
		visited_queue.pop();
	}
}

// ============================================================
// SelectNeighbors (simple closest-M heuristic)
// ============================================================

std::vector<GraphNode *> GraphIndexCore::SelectNeighbors(const std::vector<GraphCandidate> &candidates, int m) {
	std::vector<GraphNode *> result;
	result.reserve(m);

	std::vector<GraphCandidate> sorted = candidates;
	std::sort(sorted.begin(), sorted.end(),
	          [](const GraphCandidate &a, const GraphCandidate &b) {
		          return a.distance < b.distance;
	          });

	int count = std::min(m, static_cast<int>(sorted.size()));
	for (int i = 0; i < count; i++) {
		result.push_back(sorted[i].node);
	}
	return result;
}

// ============================================================
// InsertNode (Algorithm 2 from HNSW paper, with neighbor pruning)
// ============================================================

void GraphIndexCore::InsertNode(GraphNode *new_node, int m, int ef_construction,
                                vex::distance_func_t distance_func, uint32_t dimension) {
	int node_level = new_node->level;

	if (!entry_point) {
		entry_point = new_node;
		max_level = node_level;
		return;
	}

	GraphNode *ep = entry_point;
	unordered_set<row_t> visited;
	std::vector<GraphCandidate> candidates;

	// Traverse from top level down to node's level + 1
	for (int level = max_level; level > node_level; level--) {
		if (!ep) break;
		candidates.clear();
		visited.clear();
		SearchLayer(new_node->vector, ep, 1, level, candidates, visited, distance_func, dimension);
		if (!candidates.empty()) {
			std::sort(candidates.begin(), candidates.end(),
			          [](const GraphCandidate &a, const GraphCandidate &b) {
				          return a.distance < b.distance;
			          });
			ep = candidates[0].node;
		}
	}

	// Insert at each level from node's level down to 0
	for (int level = std::min(node_level, max_level); level >= 0; level--) {
		candidates.clear();
		visited.clear();
		SearchLayer(new_node->vector, ep, ef_construction, level, candidates, visited, distance_func, dimension);

		int layer_m = GraphIndexConfig::GetLayerM(m, level);
		auto neighbors = SelectNeighbors(candidates, layer_m);

		// Set neighbors for new node
		new_node->neighbors[level] = neighbors;

		// Add bidirectional connections with pruning
		for (auto *neighbor : neighbors) {
			neighbor->neighbors[level].push_back(new_node);

			// Prune if too many connections
			if (static_cast<int>(neighbor->neighbors[level].size()) > layer_m) {
				std::vector<GraphCandidate> neighbor_candidates;
				for (auto *nn : neighbor->neighbors[level]) {
					if (!nn || nn->deleted) continue;
					float d = distance_func(neighbor->vector, nn->vector, dimension);
					neighbor_candidates.push_back({nn, d});
				}
				neighbor->neighbors[level] = SelectNeighbors(neighbor_candidates, layer_m);
			}
		}

		if (!candidates.empty()) {
			std::sort(candidates.begin(), candidates.end(),
			          [](const GraphCandidate &a, const GraphCandidate &b) {
				          return a.distance < b.distance;
			          });
			ep = candidates[0].node;
		}
	}

	// Update entry point if new node has higher level
	if (node_level > max_level) {
		entry_point = new_node;
		max_level = node_level;
	}
}

// ============================================================
// Search (multi-level graph search with brute-force fallback)
// ============================================================

void GraphIndexCore::Search(const float *query_vec, idx_t k, int ef,
                            std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
                            vex::distance_func_t distance_func, uint32_t dimension, int m) {
	if (!entry_point || node_count == 0) {
		return;
	}

	// Small graph: brute force is faster and more accurate
	if (node_count <= BRUTE_FORCE_THRESHOLD) {
		BruteForceSearch(query_vec, k, out_row_ids, out_distances, distance_func, dimension);
		return;
	}

	int actual_ef = (ef > 0) ? ef : GraphIndexConfig::DEFAULT_EF_SEARCH;

	GraphNode *ep = entry_point;
	if (ep->deleted) {
		ep = nullptr;
		for (const auto &node : nodes) {
			if (node && !node->deleted) {
				ep = node.get();
				break;
			}
		}
		if (!ep) return;
	}

	unordered_set<row_t> visited;
	std::vector<GraphCandidate> candidates;

	// Traverse from top level down to level 1
	for (int level = max_level; level > 0; level--) {
		candidates.clear();
		visited.clear();
		SearchLayer(query_vec, ep, 1, level, candidates, visited, distance_func, dimension);
		if (!candidates.empty()) {
			std::sort(candidates.begin(), candidates.end(),
			          [](const GraphCandidate &a, const GraphCandidate &b) {
				          return a.distance < b.distance;
			          });
			ep = candidates[0].node;
		}
	}

	// Search at level 0 with ef
	candidates.clear();
	visited.clear();
	SearchLayer(query_vec, ep, std::max(actual_ef, static_cast<int>(k)), 0, candidates, visited, distance_func, dimension);

	// Sort and return top-k
	std::sort(candidates.begin(), candidates.end(),
	          [](const GraphCandidate &a, const GraphCandidate &b) {
		          return a.distance < b.distance;
	          });

	idx_t count = std::min(k, static_cast<idx_t>(candidates.size()));
	for (idx_t i = 0; i < count; i++) {
		out_row_ids.push_back(candidates[i].node->row_id);
		out_distances.push_back(std::sqrt(candidates[i].distance));
	}
}

// ============================================================
// BruteForceSearch (for small partitions)
// ============================================================

void GraphIndexCore::BruteForceSearch(const float *query_vec, idx_t k,
                                      std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
                                      vex::distance_func_t distance_func, uint32_t dimension) {
	std::vector<GraphCandidate> all;
	all.reserve(node_count);

	for (auto &node : nodes) {
		if (!node || node->deleted) continue;
		float dist = distance_func(query_vec, node->vector, dimension);
		all.push_back({node.get(), dist});
	}

	std::sort(all.begin(), all.end(),
	          [](const GraphCandidate &a, const GraphCandidate &b) {
		          return a.distance < b.distance;
	          });

	idx_t count = std::min(k, static_cast<idx_t>(all.size()));
	for (idx_t i = 0; i < count; i++) {
		out_row_ids.push_back(all[i].node->row_id);
		out_distances.push_back(std::sqrt(all[i].distance));
	}
}

// ============================================================
// PQ: Train, Encode, Search
// ============================================================

void GraphIndexCore::TrainPQ(uint32_t pq_m) {
	if (node_count == 0 || nodes.empty()) return;

	// Determine dimension from first node
	uint32_t dim = static_cast<uint32_t>(nodes[0]->owned_vector.size());
	if (dim == 0) return;

	// Auto-select M if 0
	if (pq_m == 0) {
		pq_m = vex::ProductQuantizer::AutoSelectM(dim);
	}

	pq.Init(dim, pq_m);

	// Collect all non-deleted vectors for training
	std::vector<float> training_data;
	training_data.reserve(node_count * dim);
	for (auto &node : nodes) {
		if (!node || node->deleted) continue;
		training_data.insert(training_data.end(), node->vector, node->vector + dim);
	}

	uint32_t n_train = static_cast<uint32_t>(training_data.size() / dim);
	pq.Train(training_data.data(), n_train);

	// Encode all nodes
	EncodeAllPQ();
}

void GraphIndexCore::EncodeAllPQ() {
	if (!pq.trained || node_count == 0) return;

	uint32_t code_size = pq.CodeSize();
	pq_codes.resize(nodes.size() * code_size);

	for (idx_t i = 0; i < nodes.size(); i++) {
		if (!nodes[i] || nodes[i]->deleted) {
			std::memset(pq_codes.data() + i * code_size, 0, code_size);
			continue;
		}
		pq.Encode(nodes[i]->vector, pq_codes.data() + i * code_size);
	}
}

const uint8_t *GraphIndexCore::GetPQCode(idx_t node_idx) const {
	if (!pq.trained || node_idx >= nodes.size()) return nullptr;
	return pq_codes.data() + node_idx * pq.CodeSize();
}

void GraphIndexCore::SearchWithPQ(const float *query_vec, idx_t k, int ef,
                                  std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
                                  vex::distance_func_t distance_func, uint32_t dimension, int m) {
	if (!entry_point || node_count == 0) return;

	// If PQ not trained, fall back to exact search
	if (!pq.trained || pq_codes.empty()) {
		Search(query_vec, k, ef, out_row_ids, out_distances, distance_func, dimension, m);
		return;
	}

	// Small graph: brute force with PQ
	if (node_count <= BRUTE_FORCE_THRESHOLD) {
		BruteForceSearch(query_vec, k, out_row_ids, out_distances, distance_func, dimension);
		return;
	}

	// Precompute PQ distance table for this query
	uint32_t pq_m = pq.m;
	std::vector<float> dist_table(static_cast<size_t>(pq_m) * vex::ProductQuantizer::KSUB);
	pq.ComputeDistanceTable(query_vec, dist_table.data());

	// Build node index map for PQ code lookup
	std::unordered_map<GraphNode *, idx_t> node_to_idx;
	for (idx_t i = 0; i < nodes.size(); i++) {
		if (nodes[i]) node_to_idx[nodes[i].get()] = i;
	}

	int actual_ef = (ef > 0) ? ef : GraphIndexConfig::DEFAULT_EF_SEARCH;

	GraphNode *ep = entry_point;
	if (!ep || ep->deleted) {
		ep = nullptr;
		for (const auto &node : nodes) {
			if (node && !node->deleted) { ep = node.get(); break; }
		}
		if (!ep) return;
	}

	unordered_set<row_t> visited;
	std::vector<GraphCandidate> candidates;

	// Upper levels: use exact distance (few nodes)
	for (int level = max_level; level > 0; level--) {
		candidates.clear();
		visited.clear();
		SearchLayer(query_vec, ep, 1, level, candidates, visited, distance_func, dimension);
		if (!candidates.empty()) {
			std::sort(candidates.begin(), candidates.end(),
			          [](const GraphCandidate &a, const GraphCandidate &b) { return a.distance < b.distance; });
			ep = candidates[0].node;
		}
	}

	// Level 0: use PQ distance for candidate search
	// Custom SearchLayer with PQ distances
	{
		std::priority_queue<GraphCandidate, std::vector<GraphCandidate>, std::greater<GraphCandidate>> cand_q;
		std::priority_queue<GraphCandidate, std::vector<GraphCandidate>, std::less<GraphCandidate>> visit_q;
		unordered_set<row_t> vis;

		auto pq_dist = [&](GraphNode *n) -> float {
			auto it = node_to_idx.find(n);
			if (it == node_to_idx.end()) return distance_func(query_vec, n->vector, dimension);
			return vex::ProductQuantizer::DistanceFromTable(
				pq_codes.data() + it->second * pq.CodeSize(), dist_table.data(), pq_m);
		};

		int search_ef = std::max(actual_ef, static_cast<int>(k));

		float d = pq_dist(ep);
		cand_q.push({ep, d});
		visit_q.push({ep, d});
		vis.insert(ep->row_id);

		while (!cand_q.empty()) {
			auto current = cand_q.top();
			cand_q.pop();
			if (!visit_q.empty() && current.distance > visit_q.top().distance) break;

			for (auto *neighbor : current.node->neighbors[0]) {
				if (neighbor->deleted || vis.find(neighbor->row_id) != vis.end()) continue;
				vis.insert(neighbor->row_id);

				float nd = pq_dist(neighbor);
				if (static_cast<int>(visit_q.size()) < search_ef || nd < visit_q.top().distance) {
					cand_q.push({neighbor, nd});
					visit_q.push({neighbor, nd});
					if (static_cast<int>(visit_q.size()) > search_ef) visit_q.pop();
				}
			}
		}

		// Collect candidates from PQ search
		candidates.clear();
		while (!visit_q.empty()) {
			if (!visit_q.top().node->deleted) candidates.push_back(visit_q.top());
			visit_q.pop();
		}
	}

	// Rerank with exact distances
	for (auto &c : candidates) {
		c.distance = distance_func(query_vec, c.node->vector, dimension);
	}

	std::sort(candidates.begin(), candidates.end(),
	          [](const GraphCandidate &a, const GraphCandidate &b) { return a.distance < b.distance; });

	idx_t count = std::min(k, static_cast<idx_t>(candidates.size()));
	for (idx_t i = 0; i < count; i++) {
		out_row_ids.push_back(candidates[i].node->row_id);
		out_distances.push_back(std::sqrt(candidates[i].distance));
	}
}

// ============================================================
// Vacuum
// ============================================================

void GraphIndexCore::Vacuum() {
	// Build set of deleted node pointers
	unordered_set<GraphNode *> deleted_set;
	for (auto &node : nodes) {
		if (node && node->deleted) {
			deleted_set.insert(node.get());
		}
	}
	if (deleted_set.empty()) return;

	// Remove deleted nodes from all neighbor lists
	for (auto &node : nodes) {
		if (!node || node->deleted) continue;
		for (auto &level_neighbors : node->neighbors) {
			level_neighbors.erase(
			    std::remove_if(level_neighbors.begin(), level_neighbors.end(),
			                   [&](GraphNode *n) { return deleted_set.count(n) > 0; }),
			    level_neighbors.end());
		}
	}

	// Remove deleted nodes
	nodes.erase(
	    std::remove_if(nodes.begin(), nodes.end(),
	                   [](const unique_ptr<GraphNode> &n) { return !n || n->deleted; }),
	    nodes.end());

	node_count = nodes.size();

	// Update entry point
	if (!entry_point || deleted_set.count(entry_point) > 0) {
		entry_point = nullptr;
		max_level = 0;
		for (auto &node : nodes) {
			if (node && !node->deleted && static_cast<int>(node->level) > max_level) {
				entry_point = node.get();
				max_level = node->level;
			}
		}
	}
}

// ============================================================
// Clear
// ============================================================

void GraphIndexCore::Clear() {
	nodes.clear();
	entry_point = nullptr;
	max_level = 0;
	node_count = 0;
	pq_codes.clear();
	pq = vex::ProductQuantizer();
}

} // namespace duckdb
