#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/execution/index/fixed_size_allocator.hpp"
#include "duckdb/execution/index/index_pointer.hpp"
#include "vex/vex_concurrency.hpp"
#include "vex/vex_config.hpp"
#include "vex_distance.hpp"
#include "vex_filter_predicate.hpp"
#include "vex_hnsw_node.hpp"
#include "vex_quantizer.hpp"
#include "vex/vex_quant_distancer.hpp"

#include <vector>

namespace duckdb {

using SimpleRWLock = ::vex::SimpleRWLock;
using SharedLockGuard = ::vex::SharedLockGuard;
using GraphIndexConfig = ::vex::GraphIndexConfig;
using VisitedSet = ::vex::VisitedSet;

struct GraphCandidate {
	IndexPointer node_ptr;
	float distance;

	bool operator>(const GraphCandidate &other) const {
		return distance > other.distance;
	}

	bool operator<(const GraphCandidate &other) const {
		return distance < other.distance;
	}
};

// ============================================================
// GraphIndexCore — shared graph core using FixedSizeAllocator
// ============================================================
struct GraphIndexCore {
	//! Entry point node
	IndexPointer entry_point;
	bool has_entry_point = false;
	int max_level = 0;
	idx_t node_count = 0;

	//! Below this threshold, use brute force instead of graph traversal
	static constexpr idx_t BRUTE_FORCE_THRESHOLD = 64;

	//! Index parameters (needed for segment size calculation)
	int m = GraphIndexConfig::DEFAULT_M;
	uint32_t dimension = 0;

	//! Fixed-size allocators for node data
	unique_ptr<FixedSizeAllocator> node_alloc;    // Allocator 0: Node headers
	unique_ptr<FixedSizeAllocator> vector_alloc;   // Allocator 1: Vector data
	unique_ptr<FixedSizeAllocator> upper_alloc;    // Allocator 2: Upper-level neighbors
	unique_ptr<FixedSizeAllocator> meta_alloc;     // Allocator 3: Per-node metadata (filter columns)

	//! Metadata segment size (0 = no metadata columns)
	uint32_t meta_segment_size = 0;

	//! Metadata column schema (describes layout within each meta segment)
	std::vector<vex::MetaColumnDesc> meta_columns;

	//! Row ID → IndexPointer mapping for O(1) node lookup (needed by Delete)
	//! With dedup, multiple row_ids (primary + extras) map to the same node_ptr.
	unordered_map<row_t, IndexPointer> row_id_map;
	bool row_id_map_built = false;

	//! Deduplication: extra row_ids per node (keyed by node_ptr.Get())
	//! Primary row_id is in the header; extras are in this map.
	static constexpr uint16_t DEFAULT_MAX_DEDUP = 8; // max row_ids per node (1 = disabled)
	uint16_t max_dedup = DEFAULT_MAX_DEDUP;
	unordered_map<idx_t, std::vector<row_t>> dedup_map_;

	//! Optional PQ quantizer for accelerated distance computation
	vex::ProductQuantizer pq;
	std::vector<uint8_t> pq_codes;
	//! Shared quant distance path from libvex-core (bridge mode only)
	unique_ptr<::vex::PQDistancerCore> pq_distancer_core;

	// ============================================================
	// Buffer pointer cache for lock-free parallel access
	// ============================================================
	//! FixedSizeBuffer::GetDeprecated() acquires a mutex on every call.
	//! During parallel build, this causes severe contention (100M+ mutex ops).
	//! This cache pre-pins all buffers and stores their base pointers,
	//! allowing lock-free pointer computation via simple arithmetic.
	struct BufferPtrCache {
		unordered_map<idx_t, data_ptr_t> origins; // buffer_id → origin of segment 0
		idx_t segment_size = 0;

		inline void Init(FixedSizeAllocator &alloc) {
			segment_size = alloc.GetSegmentSize();
		}

		//! Pre-pin a buffer and cache its origin. Call single-threaded before parallel phase.
		inline void Pin(FixedSizeAllocator &alloc, IndexPointer ptr) {
			auto buf_id = ptr.GetBufferId();
			if (origins.find(buf_id) != origins.end()) return;
			// Call Get() once (acquires mutex) to pin the buffer
			data_ptr_t seg_ptr = alloc.Get(ptr);
			// Back-compute origin: seg_ptr = origin + offset * segment_size
			origins[buf_id] = seg_ptr - ptr.GetOffset() * segment_size;
		}

		//! Lock-free pointer computation (parallel-safe when no concurrent New()/Free())
		inline data_ptr_t Get(IndexPointer ptr) const {
			auto it = origins.find(ptr.GetBufferId());
			return it->second + ptr.GetOffset() * segment_size;
		}

		inline bool IsActive() const { return !origins.empty(); }
		inline void Clear() { origins.clear(); segment_size = 0; }
	};

	BufferPtrCache node_cache_;
	BufferPtrCache vec_cache_;
	BufferPtrCache upper_cache_;
	BufferPtrCache meta_cache_;

	//! Build buffer caches for all allocated nodes (call single-threaded before parallel ops)
	void BuildBufferCaches();
	//! Clear buffer caches (call after parallel ops complete)
	void ClearBufferCaches();

	//! Evict clean, on-disk buffers from memory to reduce footprint.
	//! Call only when no SegmentHandles or raw pointers reference index data.
	//! Requires VEX_HAS_BUFFER_EVICTION (forked DuckDB with TryEvict/EvictCleanBuffers).
	idx_t EvictCleanBuffers() {
#ifdef VEX_HAS_BUFFER_EVICTION
		if (!node_alloc || !vector_alloc || !upper_alloc) return 0;
		idx_t n = 0;
		n += node_alloc->EvictCleanBuffers();
		n += vector_alloc->EvictCleanBuffers();
		n += upper_alloc->EvictCleanBuffers();
		return n;
#else
		return 0;
#endif
	}

	// ============================================================
	// Node access helpers
	// ============================================================

	//! Get node header (read-write) — raw pointer, caller must ensure buffer stays pinned
	inline vex::HNSWNodeHeader *GetNode(IndexPointer ptr) {
		if (node_cache_.IsActive()) {
			return reinterpret_cast<vex::HNSWNodeHeader *>(node_cache_.Get(ptr));
		}
		return reinterpret_cast<vex::HNSWNodeHeader *>(node_alloc->Get(ptr));
	}

	//! Get vector data for a node — raw pointer
	inline float *GetVector(IndexPointer vec_ptr) {
		if (vec_cache_.IsActive()) {
			return reinterpret_cast<float *>(vec_cache_.Get(vec_ptr));
		}
		return reinterpret_cast<float *>(vector_alloc->Get(vec_ptr));
	}

	//! Get upper-level data for a node — raw pointer
	inline vex::HNSWUpperLevel *GetUpper(IndexPointer upper_ptr) {
		if (upper_cache_.IsActive()) {
			return reinterpret_cast<vex::HNSWUpperLevel *>(upper_cache_.Get(upper_ptr));
		}
		return reinterpret_cast<vex::HNSWUpperLevel *>(upper_alloc->Get(upper_ptr));
	}

	//! Get metadata for a node (returns nullptr if no metadata)
	inline uint8_t *GetMeta(IndexPointer meta_ptr) {
		if (!meta_ptr.Get()) return nullptr;
		if (meta_cache_.IsActive()) {
			return meta_cache_.Get(meta_ptr);
		}
		return meta_alloc->Get(meta_ptr);
	}

	//! RAII handle versions — buffer is unpinned when handle is destroyed (if clean + on-disk)
	inline SegmentHandle GetNodeHandle(IndexPointer ptr) {
		return node_alloc->GetHandle(ptr);
	}
	inline SegmentHandle GetVectorHandle(IndexPointer vec_ptr) {
		return vector_alloc->GetHandle(vec_ptr);
	}
	inline SegmentHandle GetUpperHandle(IndexPointer upper_ptr) {
		return upper_alloc->GetHandle(upper_ptr);
	}

	//! Get neighbor pointers and count for a given level
	//! Returns (neighbor_array, count)
	void GetNeighbors(IndexPointer node_ptr, int level, IndexPointer *&out_neighbors, uint16_t &out_count);

	//! Set neighbor count for a given level
	void SetNeighborCount(IndexPointer node_ptr, int level, uint16_t count);

	//! Allocate a new node with vector data, returns IndexPointer to node header
	IndexPointer AllocateNode(row_t row_id, const float *vec, uint32_t dim, uint8_t level);

	//! Allocate a new node with vector data and metadata
	IndexPointer AllocateNode(row_t row_id, const float *vec, uint32_t dim, uint8_t level,
	                          const uint8_t *metadata);

	//! Free a node's allocator segments (vector, upper, metadata, node header).
	//! Does NOT update row_id_map or node_count — caller must handle those.
	void FreeNode(IndexPointer node_ptr);

	//! Build row_id_map by scanning all node headers (lazy, called on first Delete)
	void EnsureRowIdMap();

	// ============================================================
	// Core graph algorithms
	// ============================================================

	void SearchLayer(const float *query, IndexPointer ep, int ef, int layer_num,
	                 std::vector<GraphCandidate> &candidates, VisitedSet &visited,
	                 vex::distance_func_t distance_func);

	std::vector<IndexPointer> SelectNeighbors(const std::vector<GraphCandidate> &candidates, int max_m,
	                                          vex::distance_func_t distance_func);

	void InsertNode(IndexPointer new_node_ptr, int m, int ef_construction,
	                vex::distance_func_t distance_func);

	//! Thread-safe node allocation (acquires unique lock internally)
	IndexPointer AllocateNodeConcurrent(row_t row_id, const float *vec, uint32_t dim, uint8_t level);

	//! Thread-safe node insertion with read-write lock separation
	//! Search phase uses shared lock (parallel), connect phase uses unique lock (serialized)
	void InsertNodeConcurrent(IndexPointer new_node_ptr, int m, int ef_construction,
	                          vex::distance_func_t distance_func);

	//! Lock-free search + exclusive connect for parallel bulk build.
	//! PRECONDITION: No concurrent allocations (all nodes pre-allocated before calling).
	//! Search is lock-free (safe since FixedSizeAllocator buffers won't change).
	//! Connect uses graph_mutex_ exclusively (brief).
	//! Returns true if the node was deduped (merged into existing node), false if normally inserted.
	bool InsertNodeParallel(IndexPointer new_node_ptr, int m, int ef_construction,
	                        vex::distance_func_t distance_func);

	//! Search with automatic brute-force fallback for small graphs
	void Search(const float *query_vec, idx_t k, int ef,
	            std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
	            vex::distance_func_t distance_func, idx_t brute_force_threshold = BRUTE_FORCE_THRESHOLD);

	//! Brute-force search for small partitions
	void BruteForceSearch(const float *query_vec, idx_t k,
	                      std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
	                      vex::distance_func_t distance_func);

	// ============================================================
	// Filtered search (ACORN-style in-graph filtering)
	// ============================================================

	//! Filtered search with selectivity-based routing
	void FilteredSearch(const float *query_vec, idx_t k, int ef,
	                    std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
	                    vex::distance_func_t distance_func, const vex::FilterPredicate &filter,
	                    idx_t brute_force_threshold = BRUTE_FORCE_THRESHOLD);

	//! SearchLayer with optional filter predicate (ACORN-style)
	void SearchLayerFiltered(const float *query, IndexPointer ep, int ef, int layer_num,
	                         std::vector<GraphCandidate> &candidates, VisitedSet &visited,
	                         vex::distance_func_t distance_func, const vex::FilterPredicate &filter);

	//! Brute-force filtered search (for pre-filter strategy or small graphs)
	void BruteForceFilteredSearch(const float *query_vec, idx_t k,
	                              std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
	                              vex::distance_func_t distance_func, const vex::FilterPredicate &filter);

	//! PQ-accelerated search
	void SearchWithPQ(const float *query_vec, idx_t k, int ef,
	                  std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
	                  vex::distance_func_t distance_func, idx_t brute_force_threshold = BRUTE_FORCE_THRESHOLD);

	//! Train PQ codebook from current nodes
	void TrainPQ(uint32_t pq_m);
	//! Encode all nodes with current PQ codebook
	void EncodeAllPQ();

	//! Try to deduplicate a vector into an existing node.
	//! Returns true if the vector was merged (exact match found with capacity).
	bool TryDedup(row_t row_id, const float *vec, uint32_t dim,
	              vex::distance_func_t distance_func);

	//! Collect all row_ids for a node (primary + extras)
	void CollectNodeRowIds(IndexPointer node_ptr, std::vector<row_t> &out_row_ids) const;

	//! Clear all data
	void Clear();

	//! Initialize allocators (for construction or deserialization)
	void InitAllocators(BlockManager &block_manager);

	//! Read-write lock for concurrent insert support (wrapped in unique_ptr for movability)
	//! Search phases use shared (read) lock, connect phases use exclusive (write) lock.
	//! Must be initialized via InitGraphMutex() before calling concurrent methods.
	unique_ptr<SimpleRWLock> graph_mutex_;

	//! Lightweight spinlock for per-node striped locking.
#ifdef VEX_MOBILE_MODE
	//! Mobile-friendly: uses std::mutex instead of spinning to save battery.
	struct SpinLock {
		std::mutex mtx_;
		void lock() { mtx_.lock(); }
		void unlock() { mtx_.unlock(); }
	};
#else
	//! Uses test-and-test-and-set (TTAS) pattern with pure CPU pause backoff.
	//! No syscalls (no yield/sched_yield) to avoid kernel overhead.
	struct SpinLock {
		std::atomic<bool> locked_{false};

		void lock() {
			for (;;) {
				// Fast path: try to acquire
				if (!locked_.exchange(true, std::memory_order_acquire)) return;
				// Spin on read (TTAS: avoid cache line bouncing from failed CAS)
				while (locked_.load(std::memory_order_relaxed)) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
					_mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
					__builtin_ia32_pause();
#elif defined(__aarch64__)
					asm volatile("yield");
#endif
				}
			}
		}

		void unlock() {
			locked_.store(false, std::memory_order_release);
		}
	};
#endif // VEX_MOBILE_MODE

	//! RAII guard for SpinLock
	struct SpinLockGuard {
		SpinLock &lock_;
		explicit SpinLockGuard(SpinLock &l) : lock_(l) { lock_.lock(); }
		~SpinLockGuard() { lock_.unlock(); }
		SpinLockGuard(const SpinLockGuard &) = delete;
		SpinLockGuard &operator=(const SpinLockGuard &) = delete;
	};

	//! Striped spinlock array for fine-grained per-node locking during parallel build.
	//! Uses atomic spinlocks instead of std::mutex to avoid kernel syscall overhead.
	static constexpr idx_t STRIPE_COUNT = 1024;
	std::unique_ptr<SpinLock[]> node_stripes_;

	//! Lock the stripe for a given node pointer
	inline SpinLock &GetNodeLock(IndexPointer ptr) {
		return node_stripes_[ptr.Get() % STRIPE_COUNT];
	}

	//! Initialize the graph mutex (call once before concurrent operations)
	void InitGraphMutex();
};

} // namespace duckdb
