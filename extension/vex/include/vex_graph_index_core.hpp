#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/execution/index/fixed_size_allocator.hpp"
#include "duckdb/execution/index/index_pointer.hpp"
#include "vex_distance.hpp"
#include "vex_hnsw_node.hpp"
#include "vex_quantizer.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>

#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include <thread>
#include <vector>

namespace duckdb {

// ============================================================
// Simple Read-Write Lock (C++11 compatible)
// Search phases use shared (read) lock for parallelism,
// connect/allocate phases use exclusive (write) lock.
// ============================================================
#ifdef VEX_MOBILE_MODE
// Mobile-friendly RWLock: uses mutex + condvar instead of spinlock
// to avoid wasting battery on busy-waiting.
// Non-recursive: a thread must not re-acquire a lock it already holds.
class SimpleRWLock {
	std::mutex mtx_;
	std::condition_variable cv_;
	int readers_ = 0;
	bool writer_ = false;
	int writer_waiters_ = 0;

public:
	void lock_shared() {
		std::unique_lock<std::mutex> lk(mtx_);
		cv_.wait(lk, [this] { return !writer_ && writer_waiters_ == 0; });
		++readers_;
	}

	void unlock_shared() {
		std::unique_lock<std::mutex> lk(mtx_);
		if (--readers_ == 0) {
			cv_.notify_all();
		}
	}

	void lock() {
		std::unique_lock<std::mutex> lk(mtx_);
		++writer_waiters_;
		try {
			cv_.wait(lk, [this] { return !writer_ && readers_ == 0; });
		} catch (...) {
			--writer_waiters_;
			cv_.notify_all();
			throw;
		}
		--writer_waiters_;
		writer_ = true;
	}

	void unlock() {
		std::unique_lock<std::mutex> lk(mtx_);
		writer_ = false;
		cv_.notify_all();
	}
};
#else
class SimpleRWLock {
	// Atomic-based RW lock with backoff.
	// State encoding: >=0 = reader count, -1 = writer active
	std::atomic<int> state_{0};

	static inline void cpu_pause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
		_mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
		__builtin_ia32_pause();
#elif defined(__aarch64__)
		asm volatile("yield");
#endif
	}

	static inline void backoff(int &spin) {
		if (spin < 16) {
			cpu_pause();
		} else if (spin < 128) {
			for (int i = 0; i < 8; i++) cpu_pause();
		} else {
			std::this_thread::yield();
		}
		spin++;
	}

public:
	void lock_shared() {
		int spin = 0;
		int s;
		do {
			s = state_.load(std::memory_order_acquire);
			while (s < 0) {
				backoff(spin);
				s = state_.load(std::memory_order_acquire);
			}
		} while (!state_.compare_exchange_weak(s, s + 1,
		                                        std::memory_order_acq_rel,
		                                        std::memory_order_relaxed));
	}

	void unlock_shared() {
		state_.fetch_sub(1, std::memory_order_release);
	}

	void lock() {
		int spin = 0;
		int expected = 0;
		while (!state_.compare_exchange_weak(expected, -1,
		                                      std::memory_order_acq_rel,
		                                      std::memory_order_relaxed)) {
			expected = 0;
			backoff(spin);
		}
	}

	void unlock() {
		state_.store(0, std::memory_order_release);
	}
};
#endif // VEX_MOBILE_MODE

//! RAII guard for shared (read) lock
class SharedLockGuard {
	SimpleRWLock &lock_;

public:
	explicit SharedLockGuard(SimpleRWLock &l) : lock_(l) {
		lock_.lock_shared();
	}
	~SharedLockGuard() {
		lock_.unlock_shared();
	}
	SharedLockGuard(const SharedLockGuard &) = delete;
	SharedLockGuard &operator=(const SharedLockGuard &) = delete;
};

// ============================================================
// Graph Index Parameters
// ============================================================
struct GraphIndexConfig {
	static constexpr int DEFAULT_M = 16;
	static constexpr int MIN_M = 2;
	static constexpr int MAX_M = 128;
	static constexpr int DEFAULT_EF_CONSTRUCTION = 64;
	static constexpr int MIN_EF_CONSTRUCTION = 1;
	static constexpr int MAX_EF_CONSTRUCTION = 10000;
	static constexpr int DEFAULT_EF_SEARCH = 40;
	static constexpr int MIN_EF_SEARCH = 1;
	static constexpr int MAX_EF_SEARCH = 10000;

	int m = DEFAULT_M;
	int ef_construction = DEFAULT_EF_CONSTRUCTION;

	static double GetMl(int m) {
		return 1.0 / std::log(m);
	}

	static int GetMaxLevel(int m) {
		return vex::HNSW_MAX_UPPER_LEVELS;
	}

	static int GetLayerM(int m, int level) {
		return level == 0 ? m * 2 : m;
	}
};

// ============================================================
// Fast visited set using open-addressing hash table with linear probing.
// ~3-5x faster than unordered_set for HNSW search patterns.
// ============================================================
class VisitedSet {
public:
	explicit VisitedSet(idx_t capacity_hint = 128) {
		// Round up to power of 2 for fast modulo (bitwise AND)
		capacity_ = 64;
		while (capacity_ < capacity_hint * 2) {
			capacity_ <<= 1;
		}
		mask_ = capacity_ - 1;
		table_.resize(capacity_, EmptyVal());
	}

	void Clear() {
		std::fill(table_.begin(), table_.end(), EmptyVal());
	}

	void Reserve(idx_t capacity_hint) {
		idx_t new_cap = 64;
		while (new_cap < capacity_hint * 2) {
			new_cap <<= 1;
		}
		if (new_cap > capacity_) {
			capacity_ = new_cap;
			mask_ = capacity_ - 1;
			table_.resize(capacity_, EmptyVal());
		}
		Clear();
	}

	//! Insert a key. Returns true if newly inserted, false if already present.
	inline bool Insert(idx_t key) {
		idx_t idx = Hash(key) & mask_;
		while (true) {
			if (table_[idx] == EmptyVal()) {
				table_[idx] = key;
				return true;
			}
			if (table_[idx] == key) {
				return false; // already present
			}
			idx = (idx + 1) & mask_;
		}
	}

	//! Check if key is present.
	inline bool Contains(idx_t key) const {
		idx_t idx = Hash(key) & mask_;
		while (true) {
			if (table_[idx] == EmptyVal()) return false;
			if (table_[idx] == key) return true;
			idx = (idx + 1) & mask_;
		}
	}

private:
	static idx_t EmptyVal() { return ~idx_t(0); }

	inline idx_t Hash(idx_t key) const {
		// Fast integer hash (splitmix64 finalizer)
		key ^= key >> 30;
		key *= 0xbf58476d1ce4e5b9ULL;
		key ^= key >> 27;
		key *= 0x94d049bb133111ebULL;
		key ^= key >> 31;
		return key;
	}

	std::vector<idx_t> table_;
	idx_t capacity_;
	idx_t mask_;
};

// ============================================================
// Search Candidate (uses IndexPointer instead of raw pointer)
// ============================================================
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

	//! Build buffer caches for all allocated nodes (call single-threaded before parallel ops)
	void BuildBufferCaches();
	//! Clear buffer caches (call after parallel ops complete)
	void ClearBufferCaches();

	//! Evict clean, on-disk buffers from memory to reduce footprint.
	//! Call only when no SegmentHandles or raw pointers reference index data.
	idx_t EvictCleanBuffers() {
		if (!node_alloc || !vector_alloc || !upper_alloc) return 0;
		idx_t n = 0;
		n += node_alloc->EvictCleanBuffers();
		n += vector_alloc->EvictCleanBuffers();
		n += upper_alloc->EvictCleanBuffers();
		return n;
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

	//! Free a node's allocator segments (vector, upper, node header).
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
