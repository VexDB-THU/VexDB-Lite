#include "vex_graph_index_core.hpp"
#include "vex/vex_adapter_graph_runtime.hpp"
#include "vex/vex_adapter_quant_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>

namespace duckdb {

// ============================================================
// Allocator Initialization
// ============================================================

void GraphIndexCore::InitAllocators(BlockManager &block_manager) {
	node_alloc = make_uniq<FixedSizeAllocator>(vex::HNSWNodeHeader::SegmentSize(m), block_manager);
	vector_alloc = make_uniq<FixedSizeAllocator>(static_cast<idx_t>(dimension) * sizeof(float), block_manager);
	upper_alloc = make_uniq<FixedSizeAllocator>(vex::HNSWUpperLevel::SegmentSize(m), block_manager);
	// Reserve slot 0 in each allocator so all real allocations return
	// IndexPointer with Get() > 0, avoiding collision with the null sentinel
	auto p0 = node_alloc->New();
	auto p1 = vector_alloc->New();
	auto p2 = upper_alloc->New();
	// Touch reserved slots to mark their buffers as dirty.
	// Without this, FixedSizeAllocator::New() modifies the bitmask via SegmentHandle
	// which does NOT set dirty=true on the buffer. If an allocator only has the slot-0
	// reservation and no real data (e.g., upper_alloc when all nodes are level 0),
	// SerializeToWAL() → SetAllocationSize() skips non-dirty buffers → allocation_size=0
	// → WAL writes 0 bytes → on replay, bitmask is all zeros → New() fails with
	// "Invalid bitmask for FixedSizeAllocator".
	node_alloc->Get(p0);
	vector_alloc->Get(p1);
	upper_alloc->Get(p2);

	// Initialize metadata allocator if metadata columns are defined
	if (meta_segment_size > 0) {
		meta_alloc = make_uniq<FixedSizeAllocator>(static_cast<idx_t>(meta_segment_size), block_manager);
		auto p3 = meta_alloc->New();
		meta_alloc->Get(p3);
	}
}

// ============================================================
// Buffer Pointer Caches
// ============================================================

void GraphIndexCore::BuildBufferCaches() {
	node_cache_.Init(*node_alloc);
	vec_cache_.Init(*vector_alloc);
	upper_cache_.Init(*upper_alloc);
	if (meta_alloc) {
		meta_cache_.Init(*meta_alloc);
	}

	// Iterate all allocated nodes and pin their buffers
	for (auto &pair : row_id_map) {
		auto node_ptr = pair.second;
		node_cache_.Pin(*node_alloc, node_ptr);

		// Access node header to get vector and upper pointers
		auto *header = reinterpret_cast<vex::HNSWNodeHeader *>(node_alloc->Get(node_ptr));
		if (header->vector_ptr.Get()) {
			vec_cache_.Pin(*vector_alloc, header->vector_ptr);
		}
		if (header->level > 0 && header->upper_ptr.Get()) {
			upper_cache_.Pin(*upper_alloc, header->upper_ptr);
		}
		if (header->metadata_ptr.Get() && meta_alloc) {
			meta_cache_.Pin(*meta_alloc, header->metadata_ptr);
		}
	}
}

void GraphIndexCore::ClearBufferCaches() {
	node_cache_.Clear();
	vec_cache_.Clear();
	upper_cache_.Clear();
	meta_cache_.Clear();
}

// ============================================================
// Node Access Helpers
// ============================================================

void GraphIndexCore::GetNeighbors(IndexPointer node_ptr, int level,
                                   IndexPointer *&out_neighbors, uint16_t &out_count) {
	auto *header = GetNode(node_ptr);
	if (level == 0) {
		out_neighbors = header->GetLevel0Neighbors();
		out_count = header->level0_count;
	} else {
		if (!header->upper_ptr.Get()) {
			out_neighbors = nullptr;
			out_count = 0;
			return;
		}
		auto *upper = GetUpper(header->upper_ptr);
		int upper_idx = level - 1; // level 1 → index 0
		if (upper_idx >= vex::HNSW_MAX_UPPER_LEVELS) {
			out_neighbors = nullptr;
			out_count = 0;
			return;
		}
		out_neighbors = upper->GetNeighbors(upper_idx, m);
		out_count = upper->counts[upper_idx];
	}
}

void GraphIndexCore::SetNeighborCount(IndexPointer node_ptr, int level, uint16_t count) {
	auto *header = GetNode(node_ptr);
	if (level == 0) {
		header->level0_count = count;
	} else {
		if (!header->upper_ptr.Get()) return;
		auto *upper = GetUpper(header->upper_ptr);
		int upper_idx = level - 1;
		if (upper_idx < vex::HNSW_MAX_UPPER_LEVELS) {
			upper->counts[upper_idx] = count;
		}
	}
}

IndexPointer GraphIndexCore::AllocateNode(row_t row_id, const float *vec, uint32_t dim, uint8_t level) {
	// Allocate node header
	IndexPointer node_ptr = node_alloc->New();
	auto *header = GetNode(node_ptr);
	std::memset(header, 0, vex::HNSWNodeHeader::SegmentSize(m));

	// Allocate vector data
	IndexPointer vec_ptr = vector_alloc->New();
	auto *vec_data = GetVector(vec_ptr);
	std::memcpy(vec_data, vec, dim * sizeof(float));

	// Fill header
	header->row_id = row_id;
	header->level = level;
	header->deleted = 0;
	header->level0_count = 0;
	header->extra_row_count = 0;
	header->reserved = 0;
	header->vector_ptr = vec_ptr;
	header->upper_ptr = IndexPointer(); // null
	header->metadata_ptr = IndexPointer(); // null

	// Allocate upper-level segment if needed
	if (level > 0) {
		IndexPointer upper_ptr = upper_alloc->New();
		auto *upper = GetUpper(upper_ptr);
		std::memset(upper, 0, vex::HNSWUpperLevel::SegmentSize(m));
		header->upper_ptr = upper_ptr;
	}

	// Register in row_id_map
	row_id_map[row_id] = node_ptr;

	node_count++;
	return node_ptr;
}

IndexPointer GraphIndexCore::AllocateNode(row_t row_id, const float *vec, uint32_t dim, uint8_t level,
                                           const uint8_t *metadata) {
	// Allocate node header
	IndexPointer node_ptr = node_alloc->New();
	auto *header = GetNode(node_ptr);
	std::memset(header, 0, vex::HNSWNodeHeader::SegmentSize(m));

	// Allocate vector data
	IndexPointer vec_ptr = vector_alloc->New();
	auto *vec_data = GetVector(vec_ptr);
	std::memcpy(vec_data, vec, dim * sizeof(float));

	// Fill header
	header->row_id = row_id;
	header->level = level;
	header->deleted = 0;
	header->level0_count = 0;
	header->extra_row_count = 0;
	header->reserved = 0;
	header->vector_ptr = vec_ptr;
	header->upper_ptr = IndexPointer(); // null
	header->metadata_ptr = IndexPointer(); // null

	// Allocate upper-level segment if needed
	if (level > 0) {
		IndexPointer upper_ptr = upper_alloc->New();
		auto *upper = GetUpper(upper_ptr);
		std::memset(upper, 0, vex::HNSWUpperLevel::SegmentSize(m));
		header->upper_ptr = upper_ptr;
	}

	// Allocate metadata segment if metadata is provided and meta_alloc exists
	if (metadata && meta_alloc && meta_segment_size > 0) {
		IndexPointer meta_ptr = meta_alloc->New();
		auto *meta_data = GetMeta(meta_ptr);
		std::memcpy(meta_data, metadata, meta_segment_size);
		header->metadata_ptr = meta_ptr;
	}

	// Register in row_id_map
	row_id_map[row_id] = node_ptr;

	node_count++;
	return node_ptr;
}

void GraphIndexCore::FreeNode(IndexPointer node_ptr) {
	auto *header = GetNode(node_ptr);

	// Free vector data
	if (header->vector_ptr.Get()) {
		vector_alloc->Free(header->vector_ptr);
	}

	// Free upper-level data
	if (header->upper_ptr.Get()) {
		upper_alloc->Free(header->upper_ptr);
	}

	// Free metadata
	if (header->metadata_ptr.Get() && meta_alloc) {
		meta_alloc->Free(header->metadata_ptr);
	}

	// Free node header
	node_alloc->Free(node_ptr);

	// NOTE: Caller is responsible for updating row_id_map and node_count.
	// This function only releases allocator segments.
}

void GraphIndexCore::EnsureRowIdMap() {
	if (row_id_map_built) return;

	// Scan all allocated segments in node_alloc to rebuild the map
	// We iterate through the allocator's segment count
	// Since FixedSizeAllocator doesn't expose an iterator, we track all nodes via row_id_map
	// during AllocateNode. On deserialization, the map is rebuilt from storage info.
	row_id_map_built = true;
}

// ============================================================
// SearchLayer (Algorithm 3 from HNSW paper)
// ============================================================

void GraphIndexCore::SearchLayer(const float *query, IndexPointer ep, int ef, int layer_num,
                                  std::vector<GraphCandidate> &candidates, VisitedSet &visited,
                                  vex::distance_func_t distance_func) {
	std::priority_queue<GraphCandidate, std::vector<GraphCandidate>, std::greater<GraphCandidate>> candidate_queue;
	std::priority_queue<GraphCandidate, std::vector<GraphCandidate>, std::less<GraphCandidate>> visited_queue;

	// Use RAII handles when buffer cache is not active (non-parallel path)
	bool use_handles = !node_cache_.IsActive();

	auto visit_check = [&](IndexPointer ptr) -> bool {
		return !visited.Insert(ptr.Get());
	};

	// Entry point
	{
		bool ep_deleted;
		float dist;
		if (use_handles) {
			auto ep_node_h = GetNodeHandle(ep);
			auto *ep_header = ep_node_h.GetPtr<vex::HNSWNodeHeader>();
			ep_deleted = ep_header->deleted;
			if (!ep_deleted) {
				auto ep_vec_h = GetVectorHandle(ep_header->vector_ptr);
				dist = distance_func(query, ep_vec_h.GetPtr<float>(), dimension);
			}
		} else {
			auto *ep_header = GetNode(ep);
			ep_deleted = ep_header->deleted;
			if (!ep_deleted) {
				dist = distance_func(query, GetVector(ep_header->vector_ptr), dimension);
			}
		}
		if (ep_deleted) return;
		visited.Insert(ep.Get());
		candidate_queue.push({ep, dist});
		visited_queue.push({ep, dist});
	}

	while (!candidate_queue.empty()) {
		auto current = candidate_queue.top();
		candidate_queue.pop();

		if (!visited_queue.empty() && current.distance > visited_queue.top().distance) {
			break;
		}

		// Read header once, get neighbors directly from it
		IndexPointer *neighbors;
		uint16_t neighbor_count;

		if (use_handles) {
			auto cur_node_h = GetNodeHandle(current.node_ptr);
			auto *cur_header = cur_node_h.GetPtr<vex::HNSWNodeHeader>();
			if (layer_num > static_cast<int>(cur_header->level)) continue;

			if (layer_num == 0) {
				neighbors = cur_header->GetLevel0Neighbors();
				neighbor_count = cur_header->level0_count;
			} else {
				if (!cur_header->upper_ptr.Get()) continue;
				auto upper_h = GetUpperHandle(cur_header->upper_ptr);
				auto *upper = upper_h.GetPtr<vex::HNSWUpperLevel>();
				int upper_idx = layer_num - 1;
				if (upper_idx >= vex::HNSW_MAX_UPPER_LEVELS) continue;
				neighbors = upper->GetNeighbors(upper_idx, m);
				neighbor_count = upper->counts[upper_idx];

				for (uint16_t i = 0; i < neighbor_count; i++) {
					IndexPointer neighbor_ptr = neighbors[i];
					if (!neighbor_ptr.Get() || visit_check(neighbor_ptr)) continue;
					auto nb_node_h = GetNodeHandle(neighbor_ptr);
					auto *nb_header = nb_node_h.GetPtr<vex::HNSWNodeHeader>();
					if (nb_header->deleted) continue;
					auto nb_vec_h = GetVectorHandle(nb_header->vector_ptr);
					float nd = distance_func(query, nb_vec_h.GetPtr<float>(), dimension);
					if (static_cast<int>(visited_queue.size()) < ef || nd < visited_queue.top().distance) {
						candidate_queue.push({neighbor_ptr, nd});
						visited_queue.push({neighbor_ptr, nd});
						if (static_cast<int>(visited_queue.size()) > ef) visited_queue.pop();
					}
				}
				continue;
			}

			// Level 0 neighbors with prefetch (P1)
			for (uint16_t i = 0; i < neighbor_count; i++) {
				IndexPointer neighbor_ptr = neighbors[i];
				if (!neighbor_ptr.Get() || visit_check(neighbor_ptr)) continue;
				auto nb_node_h = GetNodeHandle(neighbor_ptr);
				auto *nb_header = nb_node_h.GetPtr<vex::HNSWNodeHeader>();
				if (nb_header->deleted) continue;
				auto nb_vec_h = GetVectorHandle(nb_header->vector_ptr);
				float nd = distance_func(query, nb_vec_h.GetPtr<float>(), dimension);
				if (static_cast<int>(visited_queue.size()) < ef || nd < visited_queue.top().distance) {
					candidate_queue.push({neighbor_ptr, nd});
					visited_queue.push({neighbor_ptr, nd});
					if (static_cast<int>(visited_queue.size()) > ef) visited_queue.pop();
				}
			}
		} else {
			// Raw pointer path (buffer cache active) — parallel build
			auto *cur_header = GetNode(current.node_ptr);
			if (layer_num > static_cast<int>(cur_header->level)) continue;

			if (layer_num == 0) {
				neighbors = cur_header->GetLevel0Neighbors();
				neighbor_count = cur_header->level0_count;
			} else {
				if (!cur_header->upper_ptr.Get()) continue;
				auto *upper = GetUpper(cur_header->upper_ptr);
				int upper_idx = layer_num - 1;
				if (upper_idx >= vex::HNSW_MAX_UPPER_LEVELS) continue;
				neighbors = upper->GetNeighbors(upper_idx, m);
				neighbor_count = upper->counts[upper_idx];
			}

			for (uint16_t i = 0; i < neighbor_count; i++) {
				IndexPointer neighbor_ptr = neighbors[i];
				if (!neighbor_ptr.Get() || visit_check(neighbor_ptr)) continue;
				auto *nb_header = GetNode(neighbor_ptr);
				if (nb_header->deleted) continue;
				// Prefetch next neighbor's vector to hide cache miss latency
				if (i + 1 < neighbor_count && neighbors[i + 1].Get()) {
					auto *next_h = GetNode(neighbors[i + 1]);
					if (next_h->vector_ptr.Get()) {
						__builtin_prefetch(GetVector(next_h->vector_ptr), 0, 0);
					}
				}
				auto *nb_vec = GetVector(nb_header->vector_ptr);
				float nd = distance_func(query, nb_vec, dimension);
				if (static_cast<int>(visited_queue.size()) < ef || nd < visited_queue.top().distance) {
					candidate_queue.push({neighbor_ptr, nd});
					visited_queue.push({neighbor_ptr, nd});
					if (static_cast<int>(visited_queue.size()) > ef) visited_queue.pop();
				}
			}
		}
	}

	candidates.clear();
	while (!visited_queue.empty()) {
		auto &c = visited_queue.top();
		if (use_handles) {
			auto h = GetNodeHandle(c.node_ptr);
			if (!h.GetPtr<vex::HNSWNodeHeader>()->deleted) {
				candidates.push_back(c);
			}
		} else {
			if (!GetNode(c.node_ptr)->deleted) {
				candidates.push_back(c);
			}
		}
		visited_queue.pop();
	}
}

// ============================================================
// SelectNeighbors (heuristic pruning — HNSW paper Algorithm 4)
// Ensures diverse connectivity by rejecting candidates that are closer
// to an already-selected neighbor than to the query. This is critical
// for high-dimensional data where simple closest-M degrades recall.
// ============================================================

std::vector<IndexPointer> GraphIndexCore::SelectNeighbors(const std::vector<GraphCandidate> &candidates, int max_m,
                                                          vex::distance_func_t distance_func) {
	std::vector<IndexPointer> result;
	result.reserve(max_m);

	std::vector<GraphCandidate> sorted = candidates;
	std::sort(sorted.begin(), sorted.end(),
	          [](const GraphCandidate &a, const GraphCandidate &b) {
		          return a.distance < b.distance;
	          });

	for (auto &cand : sorted) {
		if (static_cast<int>(result.size()) >= max_m) break;

		// Check if this candidate is closer to query than to any already-selected neighbor
		bool good = true;
		const float *cand_vec = GetVector(GetNode(cand.node_ptr)->vector_ptr);
		for (auto &sel_ptr : result) {
			const float *sel_vec = GetVector(GetNode(sel_ptr)->vector_ptr);
			float dist_to_selected = distance_func(cand_vec, sel_vec, dimension);
			if (dist_to_selected <= cand.distance) {
				good = false;
				break;
			}
		}
		if (good) {
			result.push_back(cand.node_ptr);
		}
	}

	// If heuristic is too aggressive and we have too few neighbors,
	// fill remaining slots with closest unused candidates
	if (static_cast<int>(result.size()) < max_m) {
		for (auto &cand : sorted) {
			if (static_cast<int>(result.size()) >= max_m) break;
			bool already_in = false;
			for (auto &r : result) {
				if (r.Get() == cand.node_ptr.Get()) {
					already_in = true;
					break;
				}
			}
			if (!already_in) {
				result.push_back(cand.node_ptr);
			}
		}
	}

	return result;
}

// ============================================================
// InsertNode (Algorithm 2 from HNSW paper, with neighbor pruning)
// ============================================================

void GraphIndexCore::InsertNode(IndexPointer new_node_ptr, int m_param, int ef_construction,
                                 vex::distance_func_t distance_func) {
	auto *new_header = GetNode(new_node_ptr);
	int node_level = new_header->level;

	if (!has_entry_point) {
		entry_point = new_node_ptr;
		has_entry_point = true;
		max_level = node_level;
		return;
	}

	IndexPointer ep = entry_point;
	VisitedSet visited(std::max(node_count, idx_t(128)));
	std::vector<GraphCandidate> candidates;

	// Traverse from top level down to node's level + 1
	for (int level = max_level; level > node_level; level--) {
		if (!ep.Get()) break;
		candidates.clear();
		visited.Clear();
		SearchLayer(new_header->vector_ptr.Get() ? GetVector(new_header->vector_ptr) : nullptr,
		            ep, 1, level, candidates, visited, distance_func);
		if (!candidates.empty()) {
			std::sort(candidates.begin(), candidates.end(),
			          [](const GraphCandidate &a, const GraphCandidate &b) {
				          return a.distance < b.distance;
			          });
			ep = candidates[0].node_ptr;
		}
	}

	auto *new_vec = GetVector(new_header->vector_ptr);

	// Insert at each level from node's level down to 0
	for (int level = std::min(node_level, max_level); level >= 0; level--) {
		candidates.clear();
		visited.Clear();
		SearchLayer(new_vec, ep, ef_construction, level, candidates, visited, distance_func);

		int layer_m = GraphIndexConfig::GetLayerM(m_param, level);
		auto selected = SelectNeighbors(candidates, layer_m, distance_func);

		// Set neighbors for new node at this level
		{
			IndexPointer *new_neighbors;
			uint16_t dummy_count;
			GetNeighbors(new_node_ptr, level, new_neighbors, dummy_count);
			if (new_neighbors) {
				uint16_t sel_count = static_cast<uint16_t>(selected.size());
				for (uint16_t i = 0; i < sel_count; i++) {
					new_neighbors[i] = selected[i];
				}
				SetNeighborCount(new_node_ptr, level, sel_count);
			}
		}

		// Add bidirectional connections with pruning
		for (auto &sel_ptr : selected) {
			auto *sel_header = GetNode(sel_ptr);

			IndexPointer *sel_neighbors;
			uint16_t sel_count;
			GetNeighbors(sel_ptr, level, sel_neighbors, sel_count);
			if (!sel_neighbors) continue;

			// Append new_node_ptr
			if (sel_count < static_cast<uint16_t>(layer_m)) {
				sel_neighbors[sel_count] = new_node_ptr;
				SetNeighborCount(sel_ptr, level, sel_count + 1);
			} else {
				// Need pruning: build candidate list and select best
				std::vector<GraphCandidate> neighbor_candidates;
				auto *sel_vec = GetVector(sel_header->vector_ptr);

				// Existing neighbors
				for (uint16_t i = 0; i < sel_count; i++) {
					IndexPointer nn_ptr = sel_neighbors[i];
					if (!nn_ptr.Get()) continue;
					auto *nn_header = GetNode(nn_ptr);
					if (nn_header->deleted) continue;
					auto *nn_vec = GetVector(nn_header->vector_ptr);
					float d = distance_func(sel_vec, nn_vec, dimension);
					neighbor_candidates.push_back({nn_ptr, d});
				}
				// Add new node
				float d = distance_func(sel_vec, new_vec, dimension);
				neighbor_candidates.push_back({new_node_ptr, d});

				auto pruned = SelectNeighbors(neighbor_candidates, layer_m, distance_func);
				uint16_t pruned_count = static_cast<uint16_t>(pruned.size());
				for (uint16_t i = 0; i < pruned_count; i++) {
					sel_neighbors[i] = pruned[i];
				}
				// Clear remaining slots
				for (uint16_t i = pruned_count; i < static_cast<uint16_t>(layer_m); i++) {
					sel_neighbors[i].Clear();
				}
				SetNeighborCount(sel_ptr, level, pruned_count);
			}
		}

		if (!candidates.empty()) {
			std::sort(candidates.begin(), candidates.end(),
			          [](const GraphCandidate &a, const GraphCandidate &b) {
				          return a.distance < b.distance;
			          });
			ep = candidates[0].node_ptr;
		}
	}

	// Update entry point if new node has higher level
	if (node_level > max_level) {
		entry_point = new_node_ptr;
		max_level = node_level;
	}
}

// ============================================================
// AllocateNodeConcurrent (thread-safe node allocation)
// ============================================================

void GraphIndexCore::InitGraphMutex() {
	if (!graph_mutex_) {
		graph_mutex_ = make_uniq<SimpleRWLock>();
	}
	if (!node_stripes_) {
		node_stripes_ = std::unique_ptr<SpinLock[]>(new SpinLock[STRIPE_COUNT]);
	}
}

IndexPointer GraphIndexCore::AllocateNodeConcurrent(row_t row_id, const float *vec, uint32_t dim, uint8_t level) {
	std::lock_guard<SimpleRWLock> lock(*graph_mutex_);
	return AllocateNode(row_id, vec, dim, level);
}

// ============================================================
// InsertNodeConcurrent (lock-separated search and connect)
// ============================================================

void GraphIndexCore::InsertNodeConcurrent(IndexPointer new_node_ptr, int m_param, int ef_construction,
                                           vex::distance_func_t distance_func) {
	auto *new_header = GetNode(new_node_ptr);
	int node_level = new_header->level;

	// Step 1: Handle empty graph (exclusive lock)
	{
		std::lock_guard<SimpleRWLock> lock(*graph_mutex_);
		if (!has_entry_point) {
			entry_point = new_node_ptr;
			has_entry_point = true;
			max_level = node_level;
			return;
		}
	}

	// Step 2: Capture current entry point (shared lock)
	IndexPointer ep;
	int current_max_level;
	{
		SharedLockGuard lock(*graph_mutex_);
		ep = entry_point;
		current_max_level = max_level;
	}

	auto *new_vec = GetVector(new_header->vector_ptr);
	VisitedSet visited(std::max(node_count, idx_t(128)));
	std::vector<GraphCandidate> candidates;

	// Step 3: Search upper levels (shared lock - multiple threads can search concurrently)
	{
		SharedLockGuard lock(*graph_mutex_);
		for (int level = current_max_level; level > node_level; level--) {
			if (!ep.Get()) break;
			candidates.clear();
			visited.Clear();
			SearchLayer(new_vec, ep, 1, level, candidates, visited, distance_func);
			if (!candidates.empty()) {
				std::sort(candidates.begin(), candidates.end(),
				          [](const GraphCandidate &a, const GraphCandidate &b) {
					          return a.distance < b.distance;
				          });
				ep = candidates[0].node_ptr;
			}
		}
	}

	// Step 4: For each insertion level - search (shared) then connect (exclusive)
	for (int level = std::min(node_level, current_max_level); level >= 0; level--) {
		// Search phase (shared lock - parallel across threads)
		{
			SharedLockGuard lock(*graph_mutex_);
			candidates.clear();
			visited.Clear();
			SearchLayer(new_vec, ep, ef_construction, level, candidates, visited, distance_func);
		}

		int layer_m = GraphIndexConfig::GetLayerM(m_param, level);
		auto selected = SelectNeighbors(candidates, layer_m, distance_func);

		// Connect phase (exclusive lock - serialized)
		{
			std::lock_guard<SimpleRWLock> lock(*graph_mutex_);

			// Set neighbors for new node at this level
			{
				IndexPointer *new_neighbors;
				uint16_t dummy_count;
				GetNeighbors(new_node_ptr, level, new_neighbors, dummy_count);
				if (new_neighbors) {
					uint16_t sel_count = static_cast<uint16_t>(selected.size());
					for (uint16_t i = 0; i < sel_count; i++) {
						new_neighbors[i] = selected[i];
					}
					SetNeighborCount(new_node_ptr, level, sel_count);
				}
			}

			// Add bidirectional connections with pruning
			for (auto &sel_ptr : selected) {
				auto *sel_header = GetNode(sel_ptr);
				IndexPointer *sel_neighbors;
				uint16_t sel_count;
				GetNeighbors(sel_ptr, level, sel_neighbors, sel_count);
				if (!sel_neighbors) continue;

				if (sel_count < static_cast<uint16_t>(layer_m)) {
					sel_neighbors[sel_count] = new_node_ptr;
					SetNeighborCount(sel_ptr, level, sel_count + 1);
				} else {
					std::vector<GraphCandidate> neighbor_candidates;
					auto *sel_vec = GetVector(sel_header->vector_ptr);
					for (uint16_t i = 0; i < sel_count; i++) {
						IndexPointer nn_ptr = sel_neighbors[i];
						if (!nn_ptr.Get()) continue;
						auto *nn_header = GetNode(nn_ptr);
						if (nn_header->deleted) continue;
						auto *nn_vec = GetVector(nn_header->vector_ptr);
						float d = distance_func(sel_vec, nn_vec, dimension);
						neighbor_candidates.push_back({nn_ptr, d});
					}
					float d = distance_func(sel_vec, new_vec, dimension);
					neighbor_candidates.push_back({new_node_ptr, d});

					auto pruned = SelectNeighbors(neighbor_candidates, layer_m, distance_func);
					uint16_t pruned_count = static_cast<uint16_t>(pruned.size());
					for (uint16_t i = 0; i < pruned_count; i++) {
						sel_neighbors[i] = pruned[i];
					}
					for (uint16_t i = pruned_count; i < static_cast<uint16_t>(layer_m); i++) {
						sel_neighbors[i].Clear();
					}
					SetNeighborCount(sel_ptr, level, pruned_count);
				}
			}
		}

		// Update ep for next level (no lock needed, ep is thread-local)
		if (!candidates.empty()) {
			std::sort(candidates.begin(), candidates.end(),
			          [](const GraphCandidate &a, const GraphCandidate &b) {
				          return a.distance < b.distance;
			          });
			ep = candidates[0].node_ptr;
		}
	}

	// Step 5: Update entry point if new node has higher level (exclusive lock)
	{
		std::lock_guard<SimpleRWLock> lock(*graph_mutex_);
		if (node_level > max_level) {
			entry_point = new_node_ptr;
			max_level = node_level;
		}
	}
}

// ============================================================
// InsertNodeParallel (shared-lock search + exclusive connect)
// Precondition: All nodes are pre-allocated. No concurrent New() calls.
// Uses atomic-based SimpleRWLock for low-overhead shared locking.
// ============================================================

bool GraphIndexCore::InsertNodeParallel(IndexPointer new_node_ptr, int m_param, int ef_construction,
                                        vex::distance_func_t distance_func) {
	auto *new_header = GetNode(new_node_ptr);
	int node_level = new_header->level;
	auto *new_vec = GetVector(new_header->vector_ptr);

	// Read entry point (atomic read, no lock — entry_point is set before parallel phase
	// and only updated by higher-level nodes which are extremely rare)
	if (!has_entry_point) return false;
	IndexPointer ep = entry_point;
	int current_max_level = max_level;

	VisitedSet visited(std::max(node_count, idx_t(128)));
	visited.Reserve(static_cast<idx_t>(ef_construction) * 4);
	std::vector<GraphCandidate> candidates;
	candidates.reserve(ef_construction * 2);

	// ============================================================
	// Phase 1: LOCK-FREE search all levels (collect results before any connections)
	// ============================================================

	// Search upper levels (greedy descent, above node's level)
	for (int level = current_max_level; level > node_level; level--) {
		if (!ep.Get()) break;
		candidates.clear();
		visited.Clear();
		SearchLayer(new_vec, ep, 1, level, candidates, visited, distance_func);
		if (!candidates.empty()) {
			std::sort(candidates.begin(), candidates.end(),
			          [](const GraphCandidate &a, const GraphCandidate &b) {
				          return a.distance < b.distance;
			          });
			ep = candidates[0].node_ptr;
		}
	}

	// Search insertion levels and collect per-level results
	int start_level = std::min(node_level, current_max_level);
	struct LevelSearchResult {
		std::vector<GraphCandidate> candidates;
		std::vector<IndexPointer> selected;
		IndexPointer best_ep;
	};
	std::vector<LevelSearchResult> level_results(start_level + 1);

	for (int level = start_level; level >= 0; level--) {
		candidates.clear();
		visited.Clear();
		SearchLayer(new_vec, ep, ef_construction, level, candidates, visited, distance_func);

		int layer_m = GraphIndexConfig::GetLayerM(m_param, level);
		auto selected = SelectNeighbors(candidates, layer_m, distance_func);

		// Save for connect phase
		level_results[level].candidates = std::move(candidates);
		level_results[level].selected = std::move(selected);

		// Update ep for next level
		if (!level_results[level].candidates.empty()) {
			std::sort(level_results[level].candidates.begin(),
			          level_results[level].candidates.end(),
			          [](const GraphCandidate &a, const GraphCandidate &b) {
				          return a.distance < b.distance;
			          });
			ep = level_results[level].candidates[0].node_ptr;
		}
	}

	// ============================================================
	// Phase 2: Dedup check at level 0 (BEFORE any connections are made)
	// Follows VexDB approach — O(ef_construction) instead of O(n²).
	// ============================================================
	if (max_dedup > 1 && !level_results[0].candidates.empty()) {
		for (auto &cand : level_results[0].candidates) {
			if (cand.distance > 1e-5f) break; // candidates are sorted, no more matches
			if (cand.node_ptr.Get() == new_node_ptr.Get()) continue; // skip self
			auto *cand_header = GetNode(cand.node_ptr);
			if (cand_header->deleted) continue;
			auto *cand_vec = GetVector(cand_header->vector_ptr);
			if (std::memcmp(new_vec, cand_vec, dimension * sizeof(float)) != 0) continue;
			// Exact match — try to merge under lock
			SpinLockGuard lock(GetNodeLock(cand.node_ptr));
			if (cand_header->extra_row_count + 1 >= max_dedup) continue; // full
			idx_t node_key = cand.node_ptr.Get();
			dedup_map_[node_key].push_back(new_header->row_id);
			cand_header->extra_row_count++;
			row_id_map[new_header->row_id] = cand.node_ptr;
			return true; // merged — no connections were made, safe to free
		}
	}

	// ============================================================
	// Phase 3: Connect all levels (per-node striped locks)
	// ============================================================
	for (int level = start_level; level >= 0; level--) {
		auto &selected = level_results[level].selected;
		int layer_m = GraphIndexConfig::GetLayerM(m_param, level);

		// Connect phase — per-node striped locks (fine-grained, minimal contention)
		// Lock our own node's stripe for setting our neighbors
		{
			SpinLockGuard lock(GetNodeLock(new_node_ptr));
			IndexPointer *new_neighbors;
			uint16_t dummy_count;
			GetNeighbors(new_node_ptr, level, new_neighbors, dummy_count);
			if (new_neighbors) {
				uint16_t sel_count = static_cast<uint16_t>(selected.size());
				for (uint16_t i = 0; i < sel_count; i++) {
					new_neighbors[i] = selected[i];
				}
				SetNeighborCount(new_node_ptr, level, sel_count);
			}
		}

		// Add bidirectional connections — lock each neighbor's stripe individually
		// Key optimization: distance computation happens OUTSIDE the lock.
		// We snapshot neighbors lock-free, compute distances, then lock only for writes.
		for (auto &sel_ptr : selected) {
			auto *sel_header = GetNode(sel_ptr);

			// Step 1: Snapshot neighbors and compute distances WITHOUT lock
			// Reading neighbor arrays lock-free is safe: worst case we read a partially
			// updated list, but HNSW is approximate and tolerates this.
			IndexPointer *sel_neighbors;
			uint16_t sel_count;
			GetNeighbors(sel_ptr, level, sel_neighbors, sel_count);
			if (!sel_neighbors) continue;

			if (sel_count < static_cast<uint16_t>(layer_m)) {
				// Simple append — brief lock
				SpinLockGuard lock(GetNodeLock(sel_ptr));
				// Re-read count under lock (may have changed)
				GetNeighbors(sel_ptr, level, sel_neighbors, sel_count);
				if (sel_count < static_cast<uint16_t>(layer_m)) {
					sel_neighbors[sel_count] = new_node_ptr;
					SetNeighborCount(sel_ptr, level, sel_count + 1);
				}
				// If full now (another thread filled it), skip pruning — approximate is OK
			} else {
				// Expensive path: compute distances OUTSIDE lock
				std::vector<GraphCandidate> neighbor_candidates;
				auto *sel_vec = GetVector(sel_header->vector_ptr);
				for (uint16_t i = 0; i < sel_count; i++) {
					IndexPointer nn_ptr = sel_neighbors[i];
					if (!nn_ptr.Get()) continue;
					auto *nn_header = GetNode(nn_ptr);
					if (nn_header->deleted) continue;
					auto *nn_vec = GetVector(nn_header->vector_ptr);
					float d = distance_func(sel_vec, nn_vec, dimension);
					neighbor_candidates.push_back({nn_ptr, d});
				}
				float d = distance_func(sel_vec, new_vec, dimension);
				neighbor_candidates.push_back({new_node_ptr, d});

				auto pruned = SelectNeighbors(neighbor_candidates, layer_m, distance_func);

				// Step 2: Lock only for writing the pruned neighbor list
				SpinLockGuard lock(GetNodeLock(sel_ptr));
				uint16_t pruned_count = static_cast<uint16_t>(pruned.size());
				// Re-read neighbor pointer under lock (pointer itself is stable)
				GetNeighbors(sel_ptr, level, sel_neighbors, sel_count);
				for (uint16_t i = 0; i < pruned_count; i++) {
					sel_neighbors[i] = pruned[i];
				}
				for (uint16_t i = pruned_count; i < static_cast<uint16_t>(layer_m); i++) {
					sel_neighbors[i].Clear();
				}
				SetNeighborCount(sel_ptr, level, pruned_count);
			}
		}

	}

	// Update entry point if new node has higher level (rare, use global lock)
	if (node_level > current_max_level) {
		std::lock_guard<SimpleRWLock> lock(*graph_mutex_);
		if (node_level > max_level) {
			entry_point = new_node_ptr;
			max_level = node_level;
		}
	}
	return false;
}

// ============================================================
// Search (multi-level graph search with brute-force fallback)
// ============================================================

void GraphIndexCore::Search(const float *query_vec, idx_t k, int ef,
                             std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
                             vex::distance_func_t distance_func, idx_t brute_force_threshold) {
	if (!has_entry_point || node_count == 0) {
		return;
	}

	// Small graph: brute force is faster and more accurate
	if (node_count <= brute_force_threshold) {
		BruteForceSearch(query_vec, k, out_row_ids, out_distances, distance_func);
		return;
	}

	int actual_ef = (ef > 0) ? ef : GraphIndexConfig::DEFAULT_EF_SEARCH;
	bool use_handles = !node_cache_.IsActive();

	VisitedSet visited(std::max(node_count, idx_t(128)));
	std::vector<GraphCandidate> candidates;
	IndexPointer ep;
	if (!::vex::ResolveLevel0EntryPoint(
	        entry_point, max_level, row_id_map, candidates, visited,
	        [this, use_handles](IndexPointer ptr) {
		        if (use_handles) {
			        auto h = GetNodeHandle(ptr);
		        return h.GetPtr<vex::HNSWNodeHeader>()->deleted != 0;
		        }
		        return GetNode(ptr)->deleted != 0;
	        },
	        [this, use_handles](IndexPointer ptr) {
		        if (use_handles) {
			        auto h = GetNodeHandle(ptr);
			        return static_cast<int>(h.GetPtr<vex::HNSWNodeHeader>()->level);
		        }
		        return static_cast<int>(GetNode(ptr)->level);
	        },
	        [this, query_vec, distance_func](IndexPointer current_ep, int level,
	                                         std::vector<GraphCandidate> &out_candidates, VisitedSet &out_visited) {
		        SearchLayer(query_vec, current_ep, 1, level, out_candidates, out_visited, distance_func);
	        },
	        [](const GraphCandidate &candidate) { return candidate.node_ptr; },
	        ep)) {
		return;
	}

	// Search at level 0 with ef
	candidates.clear();
	visited.Clear();
	SearchLayer(query_vec, ep, std::max(actual_ef, static_cast<int>(k)), 0,
	            candidates, visited, distance_func);

	if (use_handles) {
		::vex::RefineAndSortCandidates(
		    candidates,
		    [](GraphCandidate &candidate) { return candidate.distance; },
		    [this](const GraphCandidate &candidate) {
			    auto h = GetNodeHandle(candidate.node_ptr);
			    return h.GetPtr<vex::HNSWNodeHeader>()->row_id;
		    });
	} else {
		::vex::RefineAndSortCandidates(
		    candidates,
		    [](GraphCandidate &candidate) { return candidate.distance; },
		    [this](const GraphCandidate &candidate) { return GetNode(candidate.node_ptr)->row_id; });
	}

	idx_t count = std::min(k, static_cast<idx_t>(candidates.size()));
	for (idx_t i = 0; i < count && out_row_ids.size() < k; i++) {
		const auto &candidate = candidates[i];
		row_t row_id;
		uint8_t extra_count;
		if (use_handles) {
			auto h = GetNodeHandle(candidate.node_ptr);
			auto *hdr = h.GetPtr<vex::HNSWNodeHeader>();
			row_id = hdr->row_id;
			extra_count = hdr->extra_row_count;
		} else {
			auto *h = GetNode(candidate.node_ptr);
			row_id = h->row_id;
			extra_count = h->extra_row_count;
		}
		::vex::AppendResultRowWithExtras(
		    row_id, extra_count, candidate.distance, k, out_row_ids, out_distances,
		    [this, &candidate](size_t remaining, float distance,
		                       std::vector<row_t> &rows, std::vector<float> &distances) {
			    auto dit = dedup_map_.find(candidate.node_ptr.Get());
			    if (dit == dedup_map_.end()) {
				    return;
			    }
			    ::vex::AppendExtraRowsWithDistance(dit->second, distance, remaining, rows, distances);
		    });
	}
}

// ============================================================
// BruteForceSearch (for small partitions)
// ============================================================

void GraphIndexCore::BruteForceSearch(const float *query_vec, idx_t k,
                                       std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
                                       vex::distance_func_t distance_func) {
	std::vector<GraphCandidate> all;
	all.reserve(node_count);
	bool use_handles = !node_cache_.IsActive();

	unordered_set<idx_t> seen_nodes;
	for (auto &pair : row_id_map) {
		if (seen_nodes.find(pair.second.Get()) != seen_nodes.end()) continue;
		seen_nodes.insert(pair.second.Get());
		if (use_handles) {
			auto node_h = GetNodeHandle(pair.second);
			auto *header = node_h.GetPtr<vex::HNSWNodeHeader>();
			if (header->deleted) continue;
			auto vec_h = GetVectorHandle(header->vector_ptr);
			float dist = distance_func(query_vec, vec_h.GetPtr<float>(), dimension);
			all.push_back({pair.second, dist});
		} else {
			auto *header = GetNode(pair.second);
			if (header->deleted) continue;
			auto *vec = GetVector(header->vector_ptr);
			float dist = distance_func(query_vec, vec, dimension);
			all.push_back({pair.second, dist});
		}
	}

	if (use_handles) {
		::vex::RefineAndSortCandidates(
		    all,
		    [](GraphCandidate &candidate) { return candidate.distance; },
		    [this](const GraphCandidate &candidate) {
			    auto h = GetNodeHandle(candidate.node_ptr);
			    return h.GetPtr<vex::HNSWNodeHeader>()->row_id;
		    });
	} else {
		::vex::RefineAndSortCandidates(
		    all,
		    [](GraphCandidate &candidate) { return candidate.distance; },
		    [this](const GraphCandidate &candidate) { return GetNode(candidate.node_ptr)->row_id; });
	}

	idx_t count = std::min(k, static_cast<idx_t>(all.size()));
	for (idx_t i = 0; i < count && out_row_ids.size() < k; i++) {
		const auto &candidate = all[i];
		row_t row_id;
		uint8_t extra_count;
		if (use_handles) {
			auto h = GetNodeHandle(candidate.node_ptr);
			auto *hdr = h.GetPtr<vex::HNSWNodeHeader>();
			row_id = hdr->row_id;
			extra_count = hdr->extra_row_count;
		} else {
			auto *h = GetNode(candidate.node_ptr);
			row_id = h->row_id;
			extra_count = h->extra_row_count;
		}
		::vex::AppendResultRowWithExtras(
		    row_id, extra_count, candidate.distance, k, out_row_ids, out_distances,
		    [this, &candidate](size_t remaining, float distance,
		                       std::vector<row_t> &rows, std::vector<float> &distances) {
			    auto dit = dedup_map_.find(candidate.node_ptr.Get());
			    if (dit == dedup_map_.end()) {
				    return;
			    }
			    ::vex::AppendExtraRowsWithDistance(dit->second, distance, remaining, rows, distances);
		    });
	}
}

// ============================================================
// PQ: Train, Encode, Search
// ============================================================

void GraphIndexCore::TrainPQ(uint32_t pq_m) {
	if (node_count == 0) return;
	if (dimension == 0) return;

	pq_m = ::vex::ResolveQuantizerSubspaces<vex::ProductQuantizer>(dimension, pq_m);

	pq.Init(dimension, pq_m);

	auto training_data = ::vex::CollectTrainingVectors(
	    row_id_map, dimension,
	    [this](IndexPointer node_ptr) { return GetNode(node_ptr); },
	    [this](const vex::HNSWNodeHeader &header) { return GetVector(header.vector_ptr); });

	uint32_t n_train = static_cast<uint32_t>(training_data.size() / dimension);
	pq.Train(training_data.data(), n_train);
	if (!pq_distancer_core) {
		pq_distancer_core = make_uniq<::vex::PQDistancerCore>(pq_m);
	}
	::vex::LoadQuantizerIntoDistancer(pq_distancer_core.get(), pq);

	EncodeAllPQ();
}

void GraphIndexCore::EncodeAllPQ() {
	if (!pq.trained || node_count == 0) return;

	::vex::EncodeAllQuantizedCodes(
	    row_id_map, pq,
	    [this](IndexPointer node_ptr) { return GetNode(node_ptr); },
	    [this](const vex::HNSWNodeHeader &header) { return GetVector(header.vector_ptr); },
	    pq_codes);
}

void GraphIndexCore::SearchWithPQ(const float *query_vec, idx_t k, int ef,
                                   std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
                                   vex::distance_func_t distance_func, idx_t brute_force_threshold) {
	if (!has_entry_point || node_count == 0) return;

	// If PQ not trained, fall back to exact search
	if (!pq.trained || pq_codes.empty()) {
		Search(query_vec, k, ef, out_row_ids, out_distances, distance_func, brute_force_threshold);
		return;
	}

	// Small graph: brute force
	if (node_count <= brute_force_threshold) {
		BruteForceSearch(query_vec, k, out_row_ids, out_distances, distance_func);
		return;
	}

	// Precompute PQ distance table
	std::vector<float> dist_table;
	auto *active_pq_distancer = pq_distancer_core ? pq_distancer_core.get() : nullptr;
	::vex::PrepareQuantizedQuery(pq, active_pq_distancer, query_vec, dimension, dist_table);

	// Build node-to-pq-index map
	unordered_map<row_t, size_t> row_to_pq_idx;
	::vex::BuildRowCodeIndex(row_id_map, row_to_pq_idx);

	int actual_ef = (ef > 0) ? ef : GraphIndexConfig::DEFAULT_EF_SEARCH;

	bool use_handles = !node_cache_.IsActive();
	VisitedSet visited(std::max(node_count, idx_t(128)));
	std::vector<GraphCandidate> candidates;
	IndexPointer ep;
	if (!::vex::ResolveLevel0EntryPoint(
	        entry_point, max_level, row_id_map, candidates, visited,
	        [this, use_handles](IndexPointer ptr) {
		        if (use_handles) {
			        auto h = GetNodeHandle(ptr);
			        return h.GetPtr<vex::HNSWNodeHeader>()->deleted != 0;
		        }
		        return GetNode(ptr)->deleted != 0;
	        },
	        [this, use_handles](IndexPointer ptr) {
		        if (use_handles) {
			        auto h = GetNodeHandle(ptr);
			        return static_cast<int>(h.GetPtr<vex::HNSWNodeHeader>()->level);
		        }
		        return static_cast<int>(GetNode(ptr)->level);
	        },
	        [this, query_vec, distance_func](IndexPointer current_ep, int level,
	                                         std::vector<GraphCandidate> &out_candidates, VisitedSet &out_visited) {
		        SearchLayer(query_vec, current_ep, 1, level, out_candidates, out_visited, distance_func);
	        },
	        [](const GraphCandidate &candidate) { return candidate.node_ptr; },
	        ep, false)) {
		return;
	}

	// Level 0: PQ distance search
	{
		std::priority_queue<GraphCandidate, std::vector<GraphCandidate>, std::greater<GraphCandidate>> cand_q;
		std::priority_queue<GraphCandidate, std::vector<GraphCandidate>, std::less<GraphCandidate>> visit_q;
		VisitedSet vis(std::max(node_count, idx_t(128)));

		auto pq_dist = [&](IndexPointer ptr) -> float {
			if (use_handles) {
				auto h = GetNodeHandle(ptr);
				auto *hdr = h.GetPtr<vex::HNSWNodeHeader>();
				auto it = row_to_pq_idx.find(hdr->row_id);
				if (it == row_to_pq_idx.end()) {
					auto vh = GetVectorHandle(hdr->vector_ptr);
					return distance_func(query_vec, vh.GetPtr<float>(), dimension);
				}
				return ::vex::DistanceForQuantizedCode(pq, pq_codes, it->second, dist_table, active_pq_distancer);
			} else {
				auto *h = GetNode(ptr);
				auto it = row_to_pq_idx.find(h->row_id);
				if (it == row_to_pq_idx.end()) {
					auto *v = GetVector(h->vector_ptr);
					return distance_func(query_vec, v, dimension);
				}
				return ::vex::DistanceForQuantizedCode(pq, pq_codes, it->second, dist_table, active_pq_distancer);
			}
		};

		int search_ef = std::max(actual_ef, static_cast<int>(k));

		float d = pq_dist(ep);
		cand_q.push({ep, d});
		visit_q.push({ep, d});
		vis.Insert(ep.Get());

		while (!cand_q.empty()) {
			auto current = cand_q.top();
			cand_q.pop();
			if (!visit_q.empty() && current.distance > visit_q.top().distance) break;

			if (use_handles) {
				auto cur_h = GetNodeHandle(current.node_ptr);
				auto *cur_hdr = cur_h.GetPtr<vex::HNSWNodeHeader>();
				IndexPointer *neighbors = cur_hdr->GetLevel0Neighbors();
				uint16_t nb_count = cur_hdr->level0_count;
				for (uint16_t i = 0; i < nb_count; i++) {
					IndexPointer nb_ptr = neighbors[i];
					if (!nb_ptr.Get() || !vis.Insert(nb_ptr.Get())) continue;
					auto nb_h = GetNodeHandle(nb_ptr);
					auto *nb_hdr = nb_h.GetPtr<vex::HNSWNodeHeader>();
					if (nb_hdr->deleted) continue;
					float nd = pq_dist(nb_ptr);
					if (static_cast<int>(visit_q.size()) < search_ef || nd < visit_q.top().distance) {
						cand_q.push({nb_ptr, nd});
						visit_q.push({nb_ptr, nd});
						if (static_cast<int>(visit_q.size()) > search_ef) visit_q.pop();
					}
				}
			} else {
				IndexPointer *neighbors;
				uint16_t nb_count;
				GetNeighbors(current.node_ptr, 0, neighbors, nb_count);
				if (!neighbors) continue;
				for (uint16_t i = 0; i < nb_count; i++) {
					IndexPointer nb_ptr = neighbors[i];
					if (!nb_ptr.Get() || !vis.Insert(nb_ptr.Get())) continue;
					auto *nb_h = GetNode(nb_ptr);
					if (nb_h->deleted) continue;
					float nd = pq_dist(nb_ptr);
					if (static_cast<int>(visit_q.size()) < search_ef || nd < visit_q.top().distance) {
						cand_q.push({nb_ptr, nd});
						visit_q.push({nb_ptr, nd});
						if (static_cast<int>(visit_q.size()) > search_ef) visit_q.pop();
					}
				}
			}
		}

		candidates.clear();
		while (!visit_q.empty()) {
			if (use_handles) {
				auto h = GetNodeHandle(visit_q.top().node_ptr);
				if (!h.GetPtr<vex::HNSWNodeHeader>()->deleted) candidates.push_back(visit_q.top());
			} else {
				if (!GetNode(visit_q.top().node_ptr)->deleted) candidates.push_back(visit_q.top());
			}
			visit_q.pop();
		}
	}

	// Rerank with exact distances
	if (use_handles) {
		::vex::RefineAndSortCandidates(
		    candidates,
		    [this, query_vec, distance_func](GraphCandidate &candidate) {
			    auto h = GetNodeHandle(candidate.node_ptr);
			    auto vh = GetVectorHandle(h.GetPtr<vex::HNSWNodeHeader>()->vector_ptr);
			    return distance_func(query_vec, vh.GetPtr<float>(), dimension);
		    },
		    [this](const GraphCandidate &candidate) {
			    auto handle = GetNodeHandle(candidate.node_ptr);
			    return handle.GetPtr<vex::HNSWNodeHeader>()->row_id;
		    });
	} else {
		::vex::RefineAndSortCandidates(
		    candidates,
		    [this, query_vec, distance_func](GraphCandidate &candidate) {
			    auto *header = GetNode(candidate.node_ptr);
			    auto *vec = GetVector(header->vector_ptr);
			    return distance_func(query_vec, vec, dimension);
		    },
		    [this](const GraphCandidate &candidate) { return GetNode(candidate.node_ptr)->row_id; });
	}

	idx_t count = std::min(k, static_cast<idx_t>(candidates.size()));
	for (idx_t i = 0; i < count && out_row_ids.size() < k; i++) {
		const auto &candidate = candidates[i];
		row_t row_id;
		uint8_t extra_count;
		if (use_handles) {
			auto h = GetNodeHandle(candidate.node_ptr);
			auto *hdr = h.GetPtr<vex::HNSWNodeHeader>();
			row_id = hdr->row_id;
			extra_count = hdr->extra_row_count;
		} else {
			auto *h = GetNode(candidate.node_ptr);
			row_id = h->row_id;
			extra_count = h->extra_row_count;
		}
		::vex::AppendResultRowWithExtras(
		    row_id, extra_count, candidate.distance, k, out_row_ids, out_distances,
		    [this, &candidate](size_t remaining, float distance,
		                       std::vector<row_t> &rows, std::vector<float> &distances) {
			    auto dit = dedup_map_.find(candidate.node_ptr.Get());
			    if (dit == dedup_map_.end()) {
				    return;
			    }
			    ::vex::AppendExtraRowsWithDistance(dit->second, distance, remaining, rows, distances);
		    });
	}
}

// ============================================================
// Deduplication
// ============================================================

bool GraphIndexCore::TryDedup(row_t row_id, const float *vec, uint32_t dim,
                               vex::distance_func_t distance_func) {
	if (max_dedup <= 1 || node_count == 0 || !has_entry_point) {
		return false;
	}

	// Search for nearest neighbor
	std::vector<row_t> nn_row_ids;
	std::vector<float> nn_distances;

	// Use graph search to find closest node
	// We need raw graph search (not the dedup-aware Search), so inline a simple 1-NN lookup
	VisitedSet visited(std::max(node_count, idx_t(128)));
	std::vector<GraphCandidate> candidates;
	IndexPointer ep;
	if (!::vex::ResolveLevel0EntryPoint(
	        entry_point, max_level, row_id_map, candidates, visited,
	        [this](IndexPointer ptr) { return GetNode(ptr)->deleted != 0; },
	        [this](IndexPointer ptr) { return static_cast<int>(GetNode(ptr)->level); },
	        [this, vec, distance_func](IndexPointer current_ep, int level,
	                                   std::vector<GraphCandidate> &out_candidates, VisitedSet &out_visited) {
		        SearchLayer(vec, current_ep, 1, level, out_candidates, out_visited, distance_func);
	        },
	        [](const GraphCandidate &candidate) { return candidate.node_ptr; },
	        ep, false)) {
		return false;
	}

	// Search level 0 with small ef (we only need 1-NN)
	candidates.clear();
	visited.Clear();
	SearchLayer(vec, ep, 4, 0, candidates, visited, distance_func);

	if (candidates.empty()) return false;

	// Sort by distance
	std::sort(candidates.begin(), candidates.end(),
	          [](const GraphCandidate &a, const GraphCandidate &b) { return a.distance < b.distance; });

	// Try all candidates with near-zero distance (identical vectors)
	// Use memcmp for exact byte comparison instead of distance threshold,
	// because cosine distance (1 - dot) has float precision issues (~1e-8 for identical vectors).
	for (auto &cand : candidates) {
		if (cand.distance > 1e-5f) {
			break; // No more potential matches
		}

		// Verify vectors are byte-identical (handles all metrics correctly)
		auto *cand_vec = GetVector(GetNode(cand.node_ptr)->vector_ptr);
		if (std::memcmp(vec, cand_vec, dim * sizeof(float)) != 0) {
			continue; // Close but not identical
		}

		auto *header = GetNode(cand.node_ptr);
		uint16_t current_extra = header->extra_row_count;

		// Check capacity
		if (current_extra + 1 >= max_dedup) {
			continue; // This node is full, try next candidate
		}

		// Add this row_id as an extra
		idx_t node_key = cand.node_ptr.Get();
		dedup_map_[node_key].push_back(row_id);
		header->extra_row_count = current_extra + 1;

		// Register in row_id_map (maps to same node)
		row_id_map[row_id] = cand.node_ptr;

		return true;
	}

	return false;
}

void GraphIndexCore::CollectNodeRowIds(IndexPointer node_ptr, std::vector<row_t> &out_row_ids) const {
	auto *header = reinterpret_cast<const vex::HNSWNodeHeader *>(
	    const_cast<FixedSizeAllocator *>(node_alloc.get())->Get(node_ptr));
	out_row_ids.push_back(header->row_id);

	if (header->extra_row_count > 0) {
		auto it = dedup_map_.find(node_ptr.Get());
		if (it != dedup_map_.end()) {
			for (auto &rid : it->second) {
				out_row_ids.push_back(rid);
			}
		}
	}
}

// ============================================================
// Clear
// ============================================================

void GraphIndexCore::Clear() {
	if (node_alloc) node_alloc->Reset();
	if (vector_alloc) vector_alloc->Reset();
	if (upper_alloc) upper_alloc->Reset();
	if (meta_alloc) meta_alloc->Reset();
	// Re-reserve slot 0 after reset, and mark buffers dirty (see InitAllocators comment)
	if (node_alloc) {
		auto p0 = node_alloc->New();
		auto p1 = vector_alloc->New();
		auto p2 = upper_alloc->New();
		node_alloc->Get(p0);
		vector_alloc->Get(p1);
		upper_alloc->Get(p2);
		if (meta_alloc) {
			auto p3 = meta_alloc->New();
			meta_alloc->Get(p3);
		}
	}
	entry_point.Clear();
	has_entry_point = false;
	max_level = 0;
	node_count = 0;
	row_id_map.clear();
	row_id_map_built = false;
	pq_codes.clear();
	pq = vex::ProductQuantizer();
	pq_distancer_core.reset();
	dedup_map_.clear();
}

// C++11/14 out-of-line definitions for static constexpr members (required for ODR-use)
// ============================================================
// Filtered Search (ACORN-style in-graph filtering)
// ============================================================

void GraphIndexCore::SearchLayerFiltered(const float *query, IndexPointer ep, int ef, int layer_num,
                                         std::vector<GraphCandidate> &candidates, VisitedSet &visited,
                                         vex::distance_func_t distance_func, const vex::FilterPredicate &filter) {
	std::priority_queue<GraphCandidate, std::vector<GraphCandidate>, std::greater<GraphCandidate>> candidate_queue;
	std::priority_queue<GraphCandidate, std::vector<GraphCandidate>, std::less<GraphCandidate>> visited_queue;

	auto *ep_header = GetNode(ep);
	if (ep_header->deleted) return;

	auto *ep_vec = GetVector(ep_header->vector_ptr);
	float dist = distance_func(query, ep_vec, dimension);
	candidate_queue.push({ep, dist});
	visited.Insert(ep.Get());

	// ACORN: entry point goes to visited_queue only if it matches the filter
	auto *ep_meta = GetMeta(ep_header->metadata_ptr);
	if (!ep_meta || filter.Matches(ep_meta)) {
		visited_queue.push({ep, dist});
	}

	// Adaptively increase ef based on selectivity to ensure enough matching results
	double selectivity = filter.Selectivity();
	int adaptive_ef = ef;
	if (selectivity > 0.0 && selectivity < 1.0) {
		adaptive_ef = std::min(static_cast<int>(ef / selectivity), ef * 10);
	}

	while (!candidate_queue.empty()) {
		auto current = candidate_queue.top();
		candidate_queue.pop();

		// Stop condition: use adaptive_ef for candidate expansion
		if (static_cast<int>(visited_queue.size()) >= adaptive_ef &&
		    !visited_queue.empty() && current.distance > visited_queue.top().distance) {
			break;
		}

		auto *cur_header = GetNode(current.node_ptr);
		if (layer_num > static_cast<int>(cur_header->level)) {
			continue;
		}

		IndexPointer *neighbors;
		uint16_t neighbor_count;
		GetNeighbors(current.node_ptr, layer_num, neighbors, neighbor_count);
		if (!neighbors) continue;

		for (uint16_t i = 0; i < neighbor_count; i++) {
			IndexPointer neighbor_ptr = neighbors[i];
			if (!neighbor_ptr.Get()) continue;

			auto *nb_header = GetNode(neighbor_ptr);
			if (nb_header->deleted || !visited.Insert(neighbor_ptr.Get())) {
				continue;
			}

			auto *nb_vec = GetVector(nb_header->vector_ptr);
			float neighbor_dist = distance_func(query, nb_vec, dimension);

			// ACORN: always add to candidate_queue for graph connectivity
			candidate_queue.push({neighbor_ptr, neighbor_dist});

			// Only add matching nodes to the result set (visited_queue)
			auto *nb_meta = GetMeta(nb_header->metadata_ptr);
			if (nb_meta && !filter.Matches(nb_meta)) {
				continue; // skip non-matching nodes for results
			}

			if (static_cast<int>(visited_queue.size()) < adaptive_ef ||
			    neighbor_dist < visited_queue.top().distance) {
				visited_queue.push({neighbor_ptr, neighbor_dist});
				if (static_cast<int>(visited_queue.size()) > adaptive_ef) {
					visited_queue.pop();
				}
			}
		}
	}

	candidates.clear();
	while (!visited_queue.empty()) {
		auto &c = visited_queue.top();
		auto *h = GetNode(c.node_ptr);
		if (!h->deleted) {
			candidates.push_back(c);
		}
		visited_queue.pop();
	}
}

void GraphIndexCore::BruteForceFilteredSearch(const float *query_vec, idx_t k,
                                               std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
                                               vex::distance_func_t distance_func, const vex::FilterPredicate &filter) {
	std::vector<GraphCandidate> all;
	all.reserve(node_count);

	unordered_set<idx_t> seen_nodes;
	for (auto &pair : row_id_map) {
		if (seen_nodes.find(pair.second.Get()) != seen_nodes.end()) continue;
		seen_nodes.insert(pair.second.Get());
		auto *header = GetNode(pair.second);
		if (header->deleted) continue;

		// Apply filter
		auto *meta = GetMeta(header->metadata_ptr);
		if (meta && !filter.Matches(meta)) continue;

		auto *vec = GetVector(header->vector_ptr);
		float dist = distance_func(query_vec, vec, dimension);
		all.push_back({pair.second, dist});
	}

	std::sort(all.begin(), all.end(),
	          [this](const GraphCandidate &a, const GraphCandidate &b) {
		          if (a.distance != b.distance) return a.distance < b.distance;
		          return GetNode(a.node_ptr)->row_id < GetNode(b.node_ptr)->row_id;
	          });

	idx_t count = std::min(k, static_cast<idx_t>(all.size()));
	for (idx_t i = 0; i < count && out_row_ids.size() < k; i++) {
		const auto &candidate = all[i];
		auto *h = GetNode(candidate.node_ptr);
		::vex::AppendResultRowWithExtras(
		    h->row_id, h->extra_row_count, candidate.distance, k, out_row_ids, out_distances,
		    [this, &candidate](size_t remaining, float distance,
		                       std::vector<row_t> &rows, std::vector<float> &distances) {
			    auto dit = dedup_map_.find(candidate.node_ptr.Get());
			    if (dit == dedup_map_.end()) {
				    return;
			    }
			    ::vex::AppendExtraRowsWithDistance(dit->second, distance, remaining, rows, distances);
		    });
	}
}

void GraphIndexCore::FilteredSearch(const float *query_vec, idx_t k, int ef,
                                     std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
                                     vex::distance_func_t distance_func, const vex::FilterPredicate &filter,
                                     idx_t brute_force_threshold) {
	if (!has_entry_point || node_count == 0) return;

	double selectivity = filter.Selectivity();

	// Strategy routing based on selectivity
	// PRE_FILTER: very selective queries on large graphs -> brute-force scan matching nodes
	if (selectivity < 0.01 && node_count > 10000) {
		BruteForceFilteredSearch(query_vec, k, out_row_ids, out_distances, distance_func, filter);
		return;
	}

	// Small graph: always brute force
	if (node_count <= brute_force_threshold) {
		BruteForceFilteredSearch(query_vec, k, out_row_ids, out_distances, distance_func, filter);
		return;
	}

	// POST_FILTER: nearly unselective -> standard ANN + post-filter
	if (selectivity > 0.90) {
		// Over-fetch to account for post-filter losses
		idx_t fetch_k = std::min(static_cast<idx_t>(k / selectivity) + k, node_count);
		int post_ef = std::max(ef, static_cast<int>(fetch_k));

		VisitedSet visited(std::max(node_count, idx_t(128)));
		std::vector<GraphCandidate> candidates;
		IndexPointer ep;
		if (!::vex::ResolveLevel0EntryPoint(
		        entry_point, max_level, row_id_map, candidates, visited,
		        [this](IndexPointer ptr) { return GetNode(ptr)->deleted != 0; },
		        [this](IndexPointer ptr) { return static_cast<int>(GetNode(ptr)->level); },
		        [this, query_vec, distance_func](IndexPointer current_ep, int level,
		                                         std::vector<GraphCandidate> &out_candidates, VisitedSet &out_visited) {
			        SearchLayer(query_vec, current_ep, 1, level, out_candidates, out_visited, distance_func);
		        },
		        [](const GraphCandidate &candidate) { return candidate.node_ptr; },
		        ep)) {
			return;
		}

		candidates.clear();
		visited.Clear();
		SearchLayer(query_vec, ep, post_ef, 0, candidates, visited, distance_func);

		::vex::RefineAndSortCandidates(
		    candidates,
		    [](GraphCandidate &candidate) { return candidate.distance; },
		    [this](const GraphCandidate &candidate) { return GetNode(candidate.node_ptr)->row_id; });

		for (auto &c : candidates) {
			if (out_row_ids.size() >= k) break;
			auto *h = GetNode(c.node_ptr);
			auto *meta = GetMeta(h->metadata_ptr);
			if (meta && !filter.Matches(meta)) continue;
			::vex::AppendResultRowWithExtras(
			    h->row_id, h->extra_row_count, c.distance, k, out_row_ids, out_distances,
			    [this, &c](size_t remaining, float distance,
			               std::vector<row_t> &rows, std::vector<float> &distances) {
				    auto dit = dedup_map_.find(c.node_ptr.Get());
				    if (dit == dedup_map_.end()) {
					    return;
				    }
				    ::vex::AppendExtraRowsWithDistance(dit->second, distance, remaining, rows, distances);
			    });
		}
		return;
	}

	// IN_GRAPH_FILTER: ACORN-style filtered graph traversal
	int actual_ef = (ef > 0) ? ef : GraphIndexConfig::DEFAULT_EF_SEARCH;

	VisitedSet visited(std::max(node_count, idx_t(128)));
	std::vector<GraphCandidate> candidates;
	IndexPointer ep;
	if (!::vex::ResolveLevel0EntryPoint(
	        entry_point, max_level, row_id_map, candidates, visited,
	        [this](IndexPointer ptr) { return GetNode(ptr)->deleted != 0; },
	        [this](IndexPointer ptr) { return static_cast<int>(GetNode(ptr)->level); },
	        [this, query_vec, distance_func](IndexPointer current_ep, int level,
	                                         std::vector<GraphCandidate> &out_candidates, VisitedSet &out_visited) {
		        SearchLayer(query_vec, current_ep, 1, level, out_candidates, out_visited, distance_func);
	        },
	        [](const GraphCandidate &candidate) { return candidate.node_ptr; },
	        ep)) {
		return;
	}

	// Level 0: filtered search
	candidates.clear();
	visited.Clear();
	SearchLayerFiltered(query_vec, ep, std::max(actual_ef, static_cast<int>(k)), 0,
	                    candidates, visited, distance_func, filter);

	::vex::RefineAndSortCandidates(
	    candidates,
	    [](GraphCandidate &candidate) { return candidate.distance; },
	    [this](const GraphCandidate &candidate) { return GetNode(candidate.node_ptr)->row_id; });

	for (idx_t i = 0; i < candidates.size() && out_row_ids.size() < k; i++) {
		const auto &candidate = candidates[i];
		auto *h = GetNode(candidate.node_ptr);
		::vex::AppendResultRowWithExtras(
		    h->row_id, h->extra_row_count, candidate.distance, k, out_row_ids, out_distances,
		    [this, &candidate](size_t remaining, float distance,
		                       std::vector<row_t> &rows, std::vector<float> &distances) {
			    auto dit = dedup_map_.find(candidate.node_ptr.Get());
			    if (dit == dedup_map_.end()) {
				    return;
			    }
			    ::vex::AppendExtraRowsWithDistance(dit->second, distance, remaining, rows, distances);
		    });
	}
}

constexpr idx_t GraphIndexCore::BRUTE_FORCE_THRESHOLD;
constexpr uint16_t GraphIndexCore::DEFAULT_MAX_DEDUP;
constexpr idx_t GraphIndexCore::STRIPE_COUNT;

} // namespace duckdb
