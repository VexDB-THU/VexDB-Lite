#pragma once

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/execution/index/fixed_size_allocator.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/storage/block_manager.hpp"

#include "vex/vex_duck_point.hpp"
#include "vex/vex_duck_memstore.hpp"
#include "vex/vex_duckdb_compat.hpp"
#include "vex_hnsw_node.hpp"
#include "duck_pg_shim.hpp"

#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <atomic>
#include <shared_mutex>
#include <algorithm>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "vtl/span"

namespace duckdb {
using Oid = uint32_t;
}

using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using int8 = int8_t;
using int16 = int16_t;
using int32 = int32_t;
using int64 = int64_t;
using uint = unsigned int;
using Size = size_t;

class BaseObject {
};

struct PointExtensionContext;

enum class DistPrecisionType : uint8_t;

#ifndef Assert
#define Assert(cond) VEXDB_DUCK_ASSERT(cond)
#endif

#ifndef CONSTEXPR_IF
#define CONSTEXPR_IF if constexpr
#endif

#ifndef likely
#define likely(x) (x)
#endif

#ifndef unlikely
#define unlikely(x) (x)
#endif

#ifndef INVALID_VECTOR_ID
#define INVALID_VECTOR_ID SIZE_MAX
#endif

#ifndef INVALID_DIST
#define INVALID_DIST FLT_MAX
#endif

#ifndef GRAPH_INDEX_NORM_PROC
#define GRAPH_INDEX_NORM_PROC 2
#endif

#ifndef CHECK_FOR_INTERRUPTS
#define CHECK_FOR_INTERRUPTS() ((void)0)
#endif

#include "vtl/bitvector"

using Oid = duckdb::Oid;

inline bool OidIsValid(Oid oid) {
    return oid != 0;
}

inline Oid index_getprocid(void *index, int attnum, int procnum) {
    (void)index;
    (void)attnum;
    (void)procnum;
    return 0;
}

#include "quantizer_type.h"

using Relation = void *;

struct BlockIdData {
    uint16 bi_hi = 0;
    uint16 bi_lo = 0;
};

struct ItemPointerData {
    BlockIdData ip_blkid;
    uint16 ip_posid = 0;
    duckdb::row_t row_id = 0;
};

using ItemPointer = ItemPointerData *;

inline bool ItemPointerEquals(ItemPointerData *a, ItemPointerData *b) {
    return a->row_id == b->row_id;
}

struct GraphIndexMetaPageData {
    uint16 ef_construction = 64;
    uint16 m = 16;
};
using GraphIndexMetaPage = GraphIndexMetaPageData *;

struct GraphIndexSearchRes {
    ItemPointerData tid;
    float dist;
    bool operator<(const GraphIndexSearchRes &other) const {
        return dist < other.dist;
    }
};

template <typename T>
struct GraphIndexCandidate {
    T id;
    T cur_layer_idx;
    T lower_layer_idx;
    float dist;
    const char *val;

    GraphIndexCandidate()
        : id((T)INVALID_VECTOR_ID), cur_layer_idx((T)INVALID_VECTOR_ID), lower_layer_idx((T)INVALID_VECTOR_ID),
          dist(INVALID_DIST), val(nullptr) {
    }
    GraphIndexCandidate(T id_val, T cur_idxx, float dist_val)
        : id(id_val), cur_layer_idx(cur_idxx), lower_layer_idx((T)INVALID_VECTOR_ID), dist(dist_val), val(nullptr) {
    }
    GraphIndexCandidate(T id_val, T cur_idxx, T lower_idx, float dist_val, const char *val_ptr)
        : id(id_val), cur_layer_idx(cur_idxx), lower_layer_idx(lower_idx), dist(dist_val), val(val_ptr) {
    }
};

struct GraphIndexEntryInfo {
    size_t id;
    size_t cur_layer_idx;
    int_fast8_t level;

    GraphIndexEntryInfo() : id(INVALID_VECTOR_ID), cur_layer_idx(INVALID_VECTOR_ID), level(-1) {
    }
    void set(size_t new_id, size_t new_cur_layer_idx, int_fast8_t new_level) {
        id = new_id;
        cur_layer_idx = new_cur_layer_idx;
        level = new_level;
    }
};

struct GraphIndexPoint {
    using Data = ItemPointerData;
    uint8 new_inserted = 0;
    std::vector<Data> tids;

    // Coarse-grained lock guarding all tids reads/writes. During parallel BuildBulk,
    // dedup can route multiple workers to the SAME point's tids concurrently; a bare
    // std::vector::push_back then reallocs while another worker reads → heap overflow
    // (insert_range_tid crash). std::vector can't split push_back like the main repo's
    // custom Vector, so we just serialize: writers take unique, readers take shared.
    // One process-wide lock (coarse) — build is one-shot, correctness over throughput.
    static std::shared_mutex &tid_lock() {
        static std::shared_mutex m;
        return m;
    }

    bool empty() const {
        return tids.empty();
    }
    bool insert_tid(PointExtensionContext &, Span<const Data> data, bool &overwritten) {
        std::unique_lock<std::shared_mutex> _lk(tid_lock());
        overwritten = false;
        for (size_t i = 0; i < data.size(); ++i) {
            tids.push_back(data[i]);
            overwritten = true;
        }
        return true;
    }
    bool insert_tid(PointExtensionContext &ctx, Span<const Data> data) {
        bool overwritten = false;
        return insert_tid(ctx, data, overwritten);
    }
    template <typename SpanLike>
    bool insert_tid(PointExtensionContext &, SpanLike data, bool &overwritten) {
        std::unique_lock<std::shared_mutex> _lk(tid_lock());
        overwritten = false;
        for (size_t i = 0; i < data.size(); ++i) {
            tids.push_back(data[i]);
            overwritten = true;
        }
        return true;
    }
    template <typename SpanLike>
    bool insert_tid(PointExtensionContext &ctx, SpanLike data) {
        bool overwritten = false;
        return insert_tid(ctx, data, overwritten);
    }
    template <typename Vec>
    uint32 get_tids(Vec &tids, struct PointExtensionContext &ctx) const {
        (void)ctx;
        std::shared_lock<std::shared_mutex> _lk(tid_lock());
        for (const auto &tid : this->tids) {
            tids.push_back(tid);
        }
        return uint32(this->tids.size());
    }
    template <typename Func>
    uint32 vacuum_tids(Func &&func, PointExtensionContext &, bool &dirty) {
        dirty = false;
        auto it = std::remove_if(tids.begin(), tids.end(), [&](const auto &tid) {
            if (func(tid)) {
                dirty = true;
                return true;
            }
            return false;
        });
        tids.erase(it, tids.end());
        return uint32(tids.size());
    }
};

struct PointExtensionContext {
    void destroy() {
    }
};

struct RepairGraphSharedState {
    void *deleted = nullptr;
    std::atomic<size_t> *base_counter = nullptr;
    std::atomic<size_t> *upper_counter = nullptr;
    size_t basepoint_num = 0;
    size_t upperpoint_num = 0;
    size_t base_batch_size = 0;
    size_t upper_batch_size = 0;
};

constexpr int GRAPH_INDEX_MAX_LEVEL = 32;

inline void vacuum_delay_point(bool) {
}

template <typename T>
inline T Min(const T &a, const T &b) {
    return a < b ? a : b;
}

// 全局镜像内存池。所有 GRAPH_INDEX 的 vectors[] 镜像(经 DuckDB Allocator 分配，
// 不挂到 buffer pool / 不计入 DuckDB
// memory_limit)共享 vexdb_graph_memory_limit 这一个字节预算——预算是“全库镜像总量”
// 而非单索引上限。MemStore 是 header-only 模板，裸 static 全局会每个 TU 一份，故用
// inline 函数包 function-local static 保证跨 TU 单实例(C++11 magic static，线程安全初始化)。
// 作用域为进程级；同进程多 DatabaseInstance 共享同一池(嵌入式常规单实例，可接受)。
inline std::atomic<size_t> &GlobalMirrorUsedBytes() {
    static std::atomic<size_t> used{0};
    return used;
}
// 从全局池申领 want 字节，返回实际授予(<=want；池满则更少，可能为 0)。limit 为全局上限。
inline size_t ClaimGlobalMirrorBytes(size_t want, size_t limit) {
    auto &used = GlobalMirrorUsedBytes();
    size_t cur = used.load(std::memory_order_relaxed);
    for (;;) {
        size_t avail = cur < limit ? limit - cur : 0;
        size_t grant = want < avail ? want : avail;
        if (grant == 0) {
            return 0;
        }
        if (used.compare_exchange_weak(cur, cur + grant, std::memory_order_acq_rel,
                                       std::memory_order_relaxed)) {
            return grant;
        }
    }
}
inline void ReleaseGlobalMirrorBytes(size_t bytes) {
    if (bytes) {
        GlobalMirrorUsedBytes().fetch_sub(bytes, std::memory_order_acq_rel);
    }
}

template <typename IdType = uint32, typename elem_type = GraphIndexPoint>
class MemStore;

template <typename IdType, typename elem_type>
class MemStore {
public:
    using T = IdType;
    using point_type = elem_type;

    static constexpr bool use_dist_cache = false;
    static constexpr bool has_occlusion_cache = true;
    static constexpr bool clustered = false;

    struct BasePointRec {
        std::vector<T> neighbors;
        std::vector<float> dists;
        std::vector<uint32> stat_words;
    };
    struct UpperPointRec {
        T lower_layer_idx = T(INVALID_VECTOR_ID);
        T id = T(INVALID_VECTOR_ID);
        std::vector<T> neighbors_info;
        std::vector<float> dists;
        std::vector<uint32> stat_words;
    };
    struct MirrorVectorSlot {
        duckdb::Allocator *allocator = nullptr;
        duckdb::data_ptr_t ptr = nullptr;
        duckdb::idx_t size_bytes = 0;

        MirrorVectorSlot() = default;
        MirrorVectorSlot(const MirrorVectorSlot &) = delete;
        MirrorVectorSlot &operator=(const MirrorVectorSlot &) = delete;
        MirrorVectorSlot(MirrorVectorSlot &&other) noexcept
            : allocator(other.allocator), ptr(other.ptr), size_bytes(other.size_bytes) {
            other.allocator = nullptr;
            other.ptr = nullptr;
            other.size_bytes = 0;
        }
        MirrorVectorSlot &operator=(MirrorVectorSlot &&other) noexcept {
            if (this != &other) {
                Reset();
                allocator = other.allocator;
                ptr = other.ptr;
                size_bytes = other.size_bytes;
                other.allocator = nullptr;
                other.ptr = nullptr;
                other.size_bytes = 0;
            }
            return *this;
        }
        ~MirrorVectorSlot() {
            Reset();
        }

        void Reset() {
            if (ptr && allocator) {
                allocator->FreeData(ptr, size_bytes);
            }
            allocator = nullptr;
            ptr = nullptr;
            size_bytes = 0;
        }
        void Allocate(duckdb::Allocator &alloc, duckdb::idx_t bytes) {
            if (ptr && allocator == &alloc && size_bytes == bytes) {
                return;
            }
            Reset();
            if (bytes == 0) {
                return;
            }
            allocator = &alloc;
            ptr = alloc.AllocateData(bytes);
            size_bytes = bytes;
        }
        bool empty() const {
            return ptr == nullptr || size_bytes == 0;
        }
        duckdb::idx_t size() const {
            return size_bytes;
        }
        char *data() {
            return reinterpret_cast<char *>(ptr);
        }
        const char *data() const {
            return reinterpret_cast<const char *>(ptr);
        }
    };
    struct LayerView {
        // assign_vector_id fast-path 读 current_size 不持锁；slow-path 在
        // EXCLUSIVE elems_veclock 下 store 新 size。原子化建立 happens-before
        // 边：fast-path acquire 读，slow-path release 写。
        std::atomic<size_t> current_size{0};
        size_t size() const {
            return current_size.load(std::memory_order_acquire);
        }
        size_t n_data_per_block() const {
            return 1024;
        }
    };

    uint_fast16_t dim = 0;
    uint_fast16_t m = 0;
    uint_fast32_t vec_size = 0;

    /* When true, lock_point/unlock_point skip the per-node SHARED (reader)
     * stripe lock. GraphIndex::SearchANN/SearchPQ hold an index-level shared
     * rwlock (graph_rwlock_) for the whole query while writers (Append/Insert/
     * Delete/CommitDrop) take it exclusive, so the striped reader-vs-writer
     * per-node lock is redundant during search and only causes contention on
     * the few stripe atomics that hub nodes hash to (cacheline ping-pong that
     * collapsed throughput at high read concurrency). Set true on first search;
     * skipping is safe ONLY while the index-level shared lock is held, which
     * the search path guarantees. Defaults false so the multi-threaded parallel
     * build path (which is not gated by graph_rwlock_) keeps its per-node
     * locks; a stale false merely takes the always-safe locking path. */
    std::atomic<bool> search_lock_free_{false};

    GraphIndexEntryInfo entry_info;
    LayerView base_layer;
    LayerView upper_layer;

    std::vector<point_type> elems;
    std::vector<MirrorVectorSlot> vectors;
    std::vector<BasePointRec> base_points;
    std::vector<UpperPointRec> upper_points;
    std::vector<T> async_ids;

    duckdb::unique_ptr<duckdb::FixedSizeAllocator> node_alloc_;
    duckdb::unique_ptr<duckdb::FixedSizeAllocator> vector_alloc_;
    duckdb::unique_ptr<duckdb::FixedSizeAllocator> upper_alloc_;

    // compact 模式：vector_alloc_ 被 Reset，header->vector_ptr 持失效 buffer_id。
    // get_data/GetNodeHeader 必须短路，否则对空 allocator 调 Get() → SIGSEGV。
    bool compact_mode_ = false;

    // graph_memory_limit：vectors[] 内存镜像(lock-free 热层)的节点上限。预算内的
    // base 节点在 vectors[] 保留一份原始向量镜像；超出上限的节点 vectors[] 槽留空，
    // 只存 vector_alloc_(buffer manager 兜底，可分页落盘 / 受 DuckDB memory_limit 约束)。
    // get_data_unlocked 的逐 id fallback 天然处理：vectors[id] 为空 → 走 vector_alloc_->Get()。
    // 镜像与 vector_alloc_ 在 full 模式下本是 2× 冗余，收紧上限即砍掉超出部分的冗余 RAM，
    // 热层仍享 lock-free 快路径，冷层由 buffer manager 的 LRU 决定常驻/换出。
    //
    // 预算是“全库镜像总量”——所有索引共享 GlobalMirrorUsedBytes() 这一个池(见上方 free
    // functions)。本 store 按需向池申领并记账，析构时归还。三个量：
    //   mirror_max_nodes_     : 派生——本 store 已获准镜像的节点数(id < 此值才镜像)。
    //                           SIZE_MAX = 不限(无全局上限 / 无 vector_alloc_，旧全镜像行为)。
    //   mirror_limit_bytes_   : 全局上限快照(0=不限)。ApplyMirrorBudget 设；仅 vector_alloc_
    //                           存在时才非 0(否则超预算节点无处安放，必须全镜像)。
    //   mirror_claimed_bytes_ : 本 store 已从全局池申领的字节，~MemStore 归还。
    size_t mirror_max_nodes_ = SIZE_MAX;
    size_t mirror_limit_bytes_ = 0;
    size_t mirror_claimed_bytes_ = 0;
    size_t mirror_allocated_bytes_ = 0;
    duckdb::Allocator *mirror_allocator_ = nullptr;

    ~MemStore() {
        ClearMirrorVectors();
        ReleaseMirrorClaim();
    }

    void SetMirrorAllocator(duckdb::Allocator &allocator) {
        mirror_allocator_ = &allocator;
    }

    // 向全局镜像池申领，使本 store 累计可镜像到 want_nodes 个节点。返回实际可镜像节点数
    // (池满时 < want_nodes)。mirror_limit_bytes_==0(不限)→ 直接返回 want_nodes，不计账。
    // 幂等增量：重复调用只补申领差额，故 incremental Append 的逐批扩容可反复调用。
    size_t ClaimMirrorNodes(size_t want_nodes) {
        if (mirror_limit_bytes_ == 0 || vec_size == 0) {
            return want_nodes;
        }
        size_t have = mirror_claimed_bytes_ / vec_size;
        if (want_nodes <= have) {
            return have;
        }
        size_t want_more = (want_nodes - have) * static_cast<size_t>(vec_size);
        size_t granted = ClaimGlobalMirrorBytes(want_more, mirror_limit_bytes_);
        mirror_claimed_bytes_ += granted;
        return mirror_claimed_bytes_ / vec_size;
    }

    void ReleaseMirrorClaim() {
        ReleaseGlobalMirrorBytes(mirror_claimed_bytes_);
        mirror_claimed_bytes_ = 0;
        if (mirror_limit_bytes_ != 0) {
            mirror_max_nodes_ = 0;
        }
    }

    duckdb::Allocator &MirrorAllocator() {
        if (!mirror_allocator_) {
            throw duckdb::InternalException("GRAPH_INDEX mirror allocator is not initialized");
        }
        return *mirror_allocator_;
    }

    void FreeMirrorSlot(MirrorVectorSlot &slot) {
        const size_t old_size = static_cast<size_t>(slot.size());
        VEXDB_DUCK_ASSERT(old_size <= mirror_allocated_bytes_);
        mirror_allocated_bytes_ = old_size <= mirror_allocated_bytes_
            ? mirror_allocated_bytes_ - old_size : 0;
        slot.Reset();
    }

    void AllocateMirrorSlot(MirrorVectorSlot &slot, duckdb::idx_t bytes) {
        const size_t old_size = static_cast<size_t>(slot.size());
        VEXDB_DUCK_ASSERT(old_size <= mirror_allocated_bytes_);
        mirror_allocated_bytes_ = old_size <= mirror_allocated_bytes_
            ? mirror_allocated_bytes_ - old_size : 0;
        slot.Allocate(MirrorAllocator(), bytes);
        mirror_allocated_bytes_ += static_cast<size_t>(slot.size());
    }

    void AssignMirrorSlot(MirrorVectorSlot &slot, const char *data, duckdb::idx_t bytes) {
        AllocateMirrorSlot(slot, bytes);
        if (slot.ptr && bytes > 0) {
            std::memcpy(slot.ptr, data, bytes);
        }
    }

    void ResizeMirrorSlots(size_t new_size) {
        if (new_size < vectors.size()) {
            for (size_t i = new_size; i < vectors.size(); i++) {
                FreeMirrorSlot(vectors[i]);
            }
        }
        vectors.resize(new_size);
    }

    void ClearMirrorVectors(bool shrink = false) {
        for (auto &slot : vectors) {
            FreeMirrorSlot(slot);
        }
        vectors.clear();
        if (shrink) {
            vectors.shrink_to_fit();
        }
    }

    size_t GetTrackedInMemorySize() const {
        size_t bytes = mirror_allocated_bytes_;
        bytes += elems.capacity() * sizeof(point_type);
        bytes += vectors.capacity() * sizeof(MirrorVectorSlot);
        bytes += base_points.capacity() * sizeof(BasePointRec);
        bytes += upper_points.capacity() * sizeof(UpperPointRec);
        bytes += async_ids.capacity() * sizeof(T);
        bytes += id_to_node_ptr_.capacity() * sizeof(duckdb::IndexPointer);
        bytes += upper_idx_to_ptr_.capacity() * sizeof(duckdb::IndexPointer);
        bytes += node_ptr_to_id_.size() *
                 (sizeof(duckdb::idx_t) + sizeof(T) + 2 * sizeof(void *));
        // Base/upper neighbor arrays are fixed-width after construction. Count
        // their separately allocated payload without walking every node.
        bytes += base_points.size() *
                 (static_cast<size_t>(m) * 2 * (sizeof(T) + sizeof(float)) +
                  ((static_cast<size_t>(m) * 2 + 31) / 32) * sizeof(uint32));
        bytes += upper_points.size() *
                 (static_cast<size_t>(m) * 2 * sizeof(T) +
                  static_cast<size_t>(m) * sizeof(float) +
                  ((static_cast<size_t>(m) + 31) / 32) * sizeof(uint32));
        return bytes;
    }

    std::vector<duckdb::IndexPointer> id_to_node_ptr_;
    std::vector<duckdb::IndexPointer> upper_idx_to_ptr_;
    duckdb::unordered_map<duckdb::idx_t, T> node_ptr_to_id_;

    std::vector<std::vector<float>> level0_dists_;
    std::vector<std::vector<float>> upper_dists_;
    bool normalize_vectors_ = false;

    //   entry_lock      : 保护 entry_info 的读写；HNSW search/insert 的 entry 点
    //   entry_waitlock  : 防写者饥饿——升级前先抢 wait gate 阻拦后续 reader
    //   elems_veclock   : 保护 elems / vectors / base_points / upper_points 的扩容
    //   async_ids_lock  : 保护 async_ids 列表 push
    mutable LWLock entry_lock;
    mutable LWLock entry_waitlock;
    mutable LWLock elems_veclock;
    mutable LWLock async_ids_lock;

    // build-only 加锁开关。并行 BuildBulk 期间 = true：多 worker 并发读写 MemStore
    // 且不走 graph_rwlock_，get_data 此时取 elems_veclock SHARED。查询 / runtime 写
    // 期间 = false：graph_rwlock_ 已让 search(SHARED) 与 writer(EXCLUSIVE) 互斥，查询
    // 执行时没有并发写，get_data 走免锁快路径，保住 56ac149f4b 的 ARM QPS 优化。
    std::atomic<bool> parallel_build_active_{false};

    // Striped per-element 锁，用 idx & MASK 寻址。LWLockPadded 64-byte align
    // 避免 cache line false sharing。STRIPE_COUNT 是 power-of-2 让 mask 高效。
    // 64 stripe × 2 layer × 64B = 8KB，能装住 build/search 阶段的并发热点。
    static constexpr size_t STRIPE_COUNT = 64;
    static constexpr size_t STRIPE_MASK = STRIPE_COUNT - 1;
    mutable LWLockPadded base_point_locks_[STRIPE_COUNT];
    mutable LWLockPadded upper_point_locks_[STRIPE_COUNT];

    // Atomic id counters: assign_vector_id 用 fetch_add 拿独占 id。
    // 注意：next_*_id_ 跟 elems/upper_points 的 size 是松耦合的——id 由 atomic
    // 决定，outer vector 扩容用 elems_veclock 串行化。
    std::atomic<T> next_base_id_{0};
    std::atomic<T> next_upper_id_{0};

    MemStore() {
        InitLocks();
    }
    MemStore(uint_fast16_t dim_in, uint_fast16_t m_in, uint_fast32_t vec_size_in)
        : dim(dim_in), m(m_in), vec_size(vec_size_in) {
        entry_info.set(INVALID_VECTOR_ID, INVALID_VECTOR_ID, -1);
        InitLocks();
    }

private:
    void InitLocks() {
        LWLockInitialize(&entry_lock, LWTRANCHE_EXTEND);
        LWLockInitialize(&entry_waitlock, LWTRANCHE_EXTEND);
        LWLockInitialize(&elems_veclock, LWTRANCHE_EXTEND);
        LWLockInitialize(&async_ids_lock, LWTRANCHE_EXTEND);
        for (size_t i = 0; i < STRIPE_COUNT; i++) {
            LWLockInitialize(&base_point_locks_[i].lock, LWTRANCHE_EXTEND);
            LWLockInitialize(&upper_point_locks_[i].lock, LWTRANCHE_EXTEND);
        }
    }

public:
    // 并行 build 启动前 caller 应调一次。两件事：
    //   1. 预留 outer vector 容量避免并行阶段 realloc 让 raw ptr 失效
    //   2. 预 New() 所有节点的 FixedSizeAllocator 槽位（FixedSizeAllocator 内
    //      部 buffers map 不是 thread-safe，concurrent New/Get 会 race）。
    //   预 New 后并行阶段的 assign_vector_id 只 atomic fetch_add 不碰 allocator。
    void ReserveCapacity(size_t base_n, size_t upper_n) {
        LWLockAcquire(&elems_veclock, LW_EXCLUSIVE);
        const size_t cur_base = elems.size();
        const size_t cur_upper = upper_points.size();
        elems.reserve(base_n);
        vectors.reserve(base_n);
        base_points.reserve(base_n);
        upper_points.reserve(upper_n);
        id_to_node_ptr_.reserve(base_n);
        upper_idx_to_ptr_.reserve(upper_n);

        // Pre-allocate base: resize outer vectors + node_alloc_/vector_alloc_ New.
        if (base_n > cur_base) {
            elems.resize(base_n);
            ResizeMirrorSlots(base_n);
            /* Pre-size each mirror buffer to vec_size so add_vector() can memcpy into a
             * STABLE pointer. get_data()'s fast path reads vectors[id].data() lock-free
             * during parallel BuildBulk; allocating/repointing the buffer in add_vector()
             * would let a reader see a dangling/NULL data()
             * → SEGV at 0x0 in search_upper_layer (the parallel-build publish race). */
            // 全局池：申领使本索引可镜像到 base_n 个节点(增量 Append 反复调用只补差额)。
            // 池满则 mirror_max_nodes_ 停在已获准处，其余节点走 vector_alloc_。
            if (mirror_limit_bytes_ != 0) {
                mirror_max_nodes_ = ClaimMirrorNodes(base_n);
            }
            for (size_t i = cur_base; i < base_n; i++) {
                // 预算内才铺内存镜像；超出 mirror_max_nodes_ 的 slot 留空(size 0)，
                // add_vector 跳过镜像写入、get_data 自动 fallback 到 vector_alloc_。
                if (i < mirror_max_nodes_) {
                    AllocateMirrorSlot(vectors[i], vec_size);
                }
            }
            base_points.resize(base_n, MakeBasePoint());
            base_layer.current_size = base_n;
            id_to_node_ptr_.resize(base_n);
            if (node_alloc_ && vector_alloc_) {
                for (size_t i = cur_base; i < base_n; i++) {
                    auto node_ptr = node_alloc_->New();
                    auto *header = reinterpret_cast<duckdb::vex::HNSWNodeHeader<T> *>(node_alloc_->Get(node_ptr));
                    std::memset(header, 0, duckdb::vex::HNSWNodeHeader<T>::SegmentSize(m));
                    if (!compact_mode_) {
                        header->vector_ptr = vector_alloc_->New();
                    }
                    id_to_node_ptr_[i] = node_ptr;
                    node_ptr_to_id_[node_ptr.Get()] = static_cast<T>(i);
                }
            }
        }
        // Pre-allocate upper: same pattern.
        if (upper_n > cur_upper) {
            upper_points.resize(upper_n, MakeUpperPoint());
            upper_layer.current_size = upper_n;
            upper_idx_to_ptr_.resize(upper_n);
            if (upper_alloc_) {
                for (size_t i = cur_upper; i < upper_n; i++) {
                    auto upper_ptr = upper_alloc_->New();
                    auto *upper = reinterpret_cast<duckdb::vex::HNSWUpperLevel<T> *>(upper_alloc_->Get(upper_ptr));
                    std::memset(upper, 0, duckdb::vex::HNSWUpperLevel<T>::SegmentSize(m));
                    upper_idx_to_ptr_[i] = upper_ptr;
                }
            }
        }
        LWLockRelease(&elems_veclock);
    }

    // get_entry: 主库风格双锁协议（参考 graph_index_storage.h:434）。
    //   - shared 路径：拿 entry_lock SHARED 读 entry_info；成功返回 shared=true
    //   - 升级路径：当 caller 准备插入更高 level 时（new_level > entry.level）
    //     或图为空 (entry.level < 0)，需要升级为 EXCLUSIVE。先抢 entry_waitlock
    //     阻拦后续 reader（防饥饿），再 release shared、acquire exclusive。
    //
    // exclusive=true 表示 caller 跳过升级判断，强制 SHARED 模式（部分 query 路径）
    template <bool exclusive = false, bool bottom_only = false>
    std::pair<GraphIndexEntryInfo, bool> get_entry(int_fast8_t insert_level = 0) {
        (void)bottom_only;
        LWLockAcquire(&entry_waitlock, LW_EXCLUSIVE);
        LWLockRelease(&entry_waitlock);

        LWLockAcquire(&entry_lock, LW_SHARED);
        GraphIndexEntryInfo entry = entry_info;
        bool shared = true;
        if ((!exclusive && insert_level > entry.level) || entry.level < 0) {
            LWLockRelease(&entry_lock);
            LWLockAcquire(&entry_waitlock, LW_EXCLUSIVE);
            LWLockAcquire(&entry_lock, LW_EXCLUSIVE);
            LWLockRelease(&entry_waitlock);
            entry = entry_info;
            shared = false;
        }
        return {entry, shared};
    }

    void release_entry_lock(bool /*shared*/) {
        LWLockRelease(&entry_lock);
    }

    // assign_vector_id: 用 atomic counter 拿独占 id。如果 caller 已经调过
    // ReserveCapacity 预 New 了足够多的槽位（base_n >= id+1），fast path
    // 完全 lock-free。否则 fall back 到 EXCLUSIVE 锁下扩容 + New。
    //
    // Fast path: 单次 fetch_add，零 lock，零 allocator 接触。
    // Slow path: caller 没预留（如 Append 增量）才走，触发 EXCLUSIVE。
    template <bool is_base_layer>
    T assign_vector_id() {
        if constexpr (is_base_layer) {
            // acq_rel：与其他 worker 的 set_neighbor (stripe lock 内) 之间已有
            // happens-before；这里需要 release，因为后续在 elems_veclock 释放前
            // 完成的写入（resize、New segment）必须对 fast-path 读 size() 的
            // worker 可见。
            T id = next_base_id_.fetch_add(1, std::memory_order_acq_rel);
            if (id < base_layer.size()) {
                return id;
            }
            // Slow path: grow under lock. Race: another worker may have grown
            // already; re-check size after acquire.
            LWLockAcquire(&elems_veclock, LW_EXCLUSIVE);
            if (id + 1 > elems.size()) {
                elems.resize(id + 1);
                ResizeMirrorSlots(id + 1);
                base_points.resize(id + 1, MakeBasePoint());
                base_layer.current_size.store(base_points.size(), std::memory_order_release);
                if (id >= id_to_node_ptr_.size()) {
                    id_to_node_ptr_.resize(id + 1);
                }
            }
            if (node_alloc_ && !id_to_node_ptr_[id].Get()) {
                auto node_ptr = node_alloc_->New();
                auto *header = reinterpret_cast<duckdb::vex::HNSWNodeHeader<T> *>(node_alloc_->Get(node_ptr));
                std::memset(header, 0, duckdb::vex::HNSWNodeHeader<T>::SegmentSize(m));
                // ReleaseRawVectors resets vector_alloc_ and marks the store
                // compact. Quantized incremental inserts have no raw payload,
                // so allocating an unused vector segment here both wastes space
                // and eventually exhausts a reset allocator whose sentinel was
                // intentionally removed.
                if (vector_alloc_ && !compact_mode_) {
                    header->vector_ptr = vector_alloc_->New();
                }
                id_to_node_ptr_[id] = node_ptr;
                node_ptr_to_id_[node_ptr.Get()] = id;
            }
            LWLockRelease(&elems_veclock);
            return id;
        } else {
            T idx = next_upper_id_.fetch_add(1, std::memory_order_acq_rel);
            if (idx < upper_layer.size()) {
                return idx;
            }
            LWLockAcquire(&elems_veclock, LW_EXCLUSIVE);
            if (idx + 1 > upper_points.size()) {
                upper_points.resize(idx + 1, MakeUpperPoint());
                upper_layer.current_size.store(upper_points.size(), std::memory_order_release);
                if (idx >= upper_idx_to_ptr_.size()) {
                    upper_idx_to_ptr_.resize(idx + 1);
                }
            }
            if (upper_alloc_ && !upper_idx_to_ptr_[idx].Get()) {
                auto upper_ptr = upper_alloc_->New();
                auto *upper = reinterpret_cast<duckdb::vex::HNSWUpperLevel<T> *>(upper_alloc_->Get(upper_ptr));
                std::memset(upper, 0, duckdb::vex::HNSWUpperLevel<T>::SegmentSize(m));
                upper_idx_to_ptr_[idx] = upper_ptr;
            }
            LWLockRelease(&elems_veclock);
            return idx;
        }
    }

    void add_async_id(T id) {
        LWLockAcquire(&async_ids_lock, LW_EXCLUSIVE);
        async_ids.push_back(id);
        LWLockRelease(&async_ids_lock);
    }

    // add_elem / add_vector: 同一个 id 只会被一个 worker 调用（id 由
    // assign_vector_id 独占分配），写入 elems[id]/vectors[id] 的 slot 字段
    // 在 id 维度上不竞争。需要的是 SHARED elems_veclock 防止外层 realloc
    // 让 elems[id] 的 reference 失效。
    void add_elem(PointExtensionContext &ctx, T id, const ItemPointerData &tid) {
        (void)ctx;
        LWLockAcquire(&elems_veclock, LW_SHARED);
        {
            // Same coarse lock as GraphIndexPoint::insert_tid — add_elem writes
            // elems[id].tids directly (bypassing insert_tid), so dedup routing
            // multiple workers to the same id would race on push_back without this.
            std::unique_lock<std::shared_mutex> _tl(GraphIndexPoint::tid_lock());
            elems[id].tids.push_back(tid);
        }
        if (node_alloc_) {
            auto ptr = GetNodePtr(id);
            if (ptr.Get()) {
                auto *header = reinterpret_cast<duckdb::vex::HNSWNodeHeader<T> *>(node_alloc_->Get(ptr));
                header->row_id = tid.row_id;
            }
        }
        LWLockRelease(&elems_veclock);
    }
    void add_elem(PointExtensionContext &ctx, T id, Span<const ItemPointerData> tids) {
        (void)ctx;
        LWLockAcquire(&elems_veclock, LW_SHARED);
        {
            std::unique_lock<std::shared_mutex> _tl(GraphIndexPoint::tid_lock());
            for (const auto &tid : tids) {
                elems[id].tids.push_back(tid);
                if (node_alloc_) {
                    auto ptr = GetNodePtr(id);
                    if (ptr.Get()) {
                        auto *header = reinterpret_cast<duckdb::vex::HNSWNodeHeader<T> *>(node_alloc_->Get(ptr));
                        header->row_id = tid.row_id;
                    }
                }
            }
        }
        LWLockRelease(&elems_veclock);
    }

    void add_vector(T id, const char *query) {
        const char *store_data = query;
        std::vector<char> normalized;
        if (normalize_vectors_) {
            normalized.resize(vec_size);
            auto *dst = reinterpret_cast<float *>(normalized.data());
            auto *src = reinterpret_cast<const float *>(query);
            float norm2 = 0.0f;
            for (uint_fast16_t i = 0; i < dim; i++) {
                norm2 += src[i] * src[i];
            }
            if (norm2 > 0.0f) {
                float inv_norm = 1.0f / std::sqrt(norm2);
                for (uint_fast16_t i = 0; i < dim; i++) {
                    dst[i] = src[i] * inv_norm;
                }
            } else {
                std::memcpy(dst, src, vec_size);
            }
            store_data = normalized.data();
        }

        LWLockAcquire(&elems_veclock, LW_SHARED);
        /* Hot path (parallel build): ReserveCapacity pre-sized vectors[id] to vec_size,
         * so memcpy into the stable buffer — no realloc, no data()-pointer change, safe
         * against concurrent lock-free get_data() readers. Cold path (unreserved id,
         * e.g. incremental Append) falls back to assign. */
        // 预算内的节点写内存镜像(lock-free 热层)。超出 mirror_max_nodes_ 的节点
        // 跳过镜像、只落 vector_alloc_(下方)——get_data 对其走 buffer manager fallback。
        if (id < mirror_max_nodes_) {
            auto &slot = vectors[id];
            if (slot.size() == (size_t)vec_size) {
                std::memcpy(slot.data(), store_data, vec_size);
            } else {
                AssignMirrorSlot(slot, store_data, vec_size);
            }
        }
        if (node_alloc_ && vector_alloc_) {
            auto ptr = GetNodePtr(id);
            if (ptr.Get()) {
                auto *header = reinterpret_cast<duckdb::vex::HNSWNodeHeader<T> *>(node_alloc_->Get(ptr));
                if (header->vector_ptr.Get()) {
                    auto *vec_data = reinterpret_cast<float *>(vector_alloc_->Get(header->vector_ptr));
                    std::memcpy(vec_data, store_data, vec_size);
                }
            }
        }
        LWLockRelease(&elems_veclock);
    }

    template <typename Distancer>
    void add_vector(Distancer &, T id, const char *query) {
        add_vector(id, query);
    }

    void set_entrypoint(T id, T cur_layer_idx, int_fast8_t level) {
        // caller (algorithm) is already holding entry_lock EXCLUSIVE
        entry_info.set(id, cur_layer_idx, level);
    }

    void *get_index() const {
        return nullptr;
    }
    DistPrecisionType get_precision() const {
        return static_cast<DistPrecisionType>(0);
    }
    uint16 get_dim() const {
        return uint16(dim);
    }
    uint32 get_vecsize() const {
        return uint32(vec_size);
    }
    uint32 get_elemsize() const {
        return uint32(vec_size);
    }
    size_t get_vector_num() const {
        return elems.size();
    }

    // max_id<is_base_layer>(): upper bound on node ids on a given layer.
    // Used by VisitedListPool to size the epoch-tag array.
    //
    // assign_vector_id fast-path does next_base_id_.fetch_add(1) BEFORE the
    // slow-path grows base_layer.size(), so during parallel build the published
    // next_id_ may exceed layer.size(). A reentrant search_layer (via
    // update_reverse_edges) can visit such an id and overflow the visited slab.
    // Use the atomic counter here; +16 slack covers any further outstanding
    // fetch_add.
    template <bool is_base_layer>
    size_t max_id() const {
        if constexpr (is_base_layer) {
            size_t n_atomic = static_cast<size_t>(next_base_id_.load(std::memory_order_acquire));
            size_t n_layer = base_layer.size();
            return (n_atomic > n_layer ? n_atomic : n_layer) + 16;
        } else {
            size_t n_atomic = static_cast<size_t>(next_upper_id_.load(std::memory_order_acquire));
            size_t n_layer = upper_layer.size();
            return (n_atomic > n_layer ? n_atomic : n_layer) + 16;
        }
    }

    void ResizeForReload(size_t base_n, size_t upper_n) {
        elems.resize(base_n);
        ResizeMirrorSlots(base_n);
        base_points.resize(base_n, MakeBasePoint());
        base_layer.current_size = base_n;
        upper_points.resize(upper_n, MakeUpperPoint());
        upper_layer.current_size = upper_n;
    }

    void CreateAllocators(duckdb::BlockManager &block_manager) {
        using namespace duckdb;
        node_alloc_ = make_uniq<FixedSizeAllocator>(vex::HNSWNodeHeader<T>::SegmentSize(m), block_manager);
        vector_alloc_ = make_uniq<FixedSizeAllocator>(static_cast<idx_t>(dim) * sizeof(float), block_manager);
        upper_alloc_ = make_uniq<FixedSizeAllocator>(vex::HNSWUpperLevel<T>::SegmentSize(m), block_manager);
    }

    void ReserveSentinelSlot() {
        auto p0 = node_alloc_->New();
        auto p1 = vector_alloc_->New();
        auto p2 = upper_alloc_->New();
        node_alloc_->Get(p0);
        vector_alloc_->Get(p1);
        upper_alloc_->Get(p2);
    }

    void InitAllocators(duckdb::BlockManager &block_manager) {
        CreateAllocators(block_manager);
        ReserveSentinelSlot();
    }

    duckdb::IndexPointer GetNodePtr(T id) const {
        if (id >= id_to_node_ptr_.size()) return duckdb::IndexPointer();
        return id_to_node_ptr_[id];
    }

    T GetNodeId(duckdb::IndexPointer ptr) const {
        auto it = node_ptr_to_id_.find(ptr.Get());
        return it != node_ptr_to_id_.end() ? it->second : T(INVALID_VECTOR_ID);
    }

    duckdb::vex::HNSWNodeHeader<T> *GetNodeHeader(T id) {
        auto ptr = GetNodePtr(id);
        if (!ptr.Get()) return nullptr;
        return reinterpret_cast<duckdb::vex::HNSWNodeHeader<T> *>(node_alloc_->Get(ptr));
    }

    float *GetVectorData(T id) {
        if (compact_mode_) return nullptr;
        auto *header = GetNodeHeader(id);
        if (!header || !header->vector_ptr.Get()) return nullptr;
        return reinterpret_cast<float *>(vector_alloc_->Get(header->vector_ptr));
    }

    char *get_data_unlocked(T id) {
        // Defensive bound check: after reload, neighbor slots past header->level0_count
        // can hold stale data from a previously freed segment. is_valid() only rejects
        // INVALID_VECTOR_ID; any other garbage value (e.g. 0xfffffff7) is treated as a
        // real id and routed here. Without this check we'd OOB into vectors[id].
        if (id >= vectors.size() && id >= elems.size()) {
            return nullptr;
        }
        // Fast path: in-memory raw vector copy. In non-compact (full) mode
        // vectors[] is populated by add_vector and only released in compact
        // mode (ReleaseRawVectors), so it stays valid for the whole search.
        // Returning its stable pointer avoids two FixedSizeAllocator::Get calls
        // that EACH take a per-buffer std::mutex (FixedSizeBuffer::GetDeprecated)
        // plus a buffers hash lookup — the dominant per-query cost on ARM (see
        // docs/reports/2026-05-27_duck-arm-perquery-flamegraph.md). Falls through
        // to the allocator path post-reload (vectors empty) or in compact mode.
        if (id < vectors.size() && !vectors[id].empty()) {
            return vectors[id].data();
        }
        if (!compact_mode_ && node_alloc_ && vector_alloc_) {
            auto ptr = GetNodePtr(id);
            if (ptr.Get()) {
                auto *hdr = reinterpret_cast<duckdb::vex::HNSWNodeHeader<T> *>(node_alloc_->Get(ptr, /*dirty=*/false));
                if (hdr->vector_ptr.Get()) {
                    return reinterpret_cast<char *>(vector_alloc_->Get(hdr->vector_ptr, /*dirty=*/false));
                }
            }
        }
        // 镜像超预算的 id：vectors[id] 为空，上面的 vector_alloc_ 路径已返回。
        // 若 allocator 缺失(理论上 ApplyMirrorBudget 已保证不会收紧)则返回 nullptr，
        // 绝不对空 slot 取 .data() 后按 vec_size memcpy 造成越界读。
        if (id >= vectors.size() || vectors[id].empty()) {
            return nullptr;
        }
        return vectors[id].data();
    }
    /* Lock wrapper. get_data() reads vectors[id] (std::vector) which the main repo's
     * custom Vector serves lock-free via a split-push_back back-door that std::vector
     * cannot replicate. Take elems_veclock SHARED so the read is safe against concurrent
     * inner writes (also SHARED, non-conflicting per-id) and excluded during EXCLUSIVE
     * resizes (outer realloc) — the parallel-build read-vs-realloc race. */
    char *get_data(T id) {
        // Lock only during parallel build (concurrent lock-free writers, no graph_rwlock_).
        // Queries hold graph_rwlock_ SHARED (writers EXCLUSIVE) → no concurrent writer →
        // lock-free fast path preserves QPS.
        if (!parallel_build_active_.load(std::memory_order_acquire)) {
            return get_data_unlocked(id);
        }
        LWLockAcquire(&elems_veclock, LW_SHARED);
        char *r = get_data_unlocked(id);
        LWLockRelease(&elems_veclock);
        return r;
    }
    const char *get_data_unlocked(T id) const {
        if (id >= vectors.size() && id >= elems.size()) {
            return nullptr;
        }
        // Fast path: see non-const overload above.
        if (id < vectors.size() && !vectors[id].empty()) {
            return vectors[id].data();
        }
        if (!compact_mode_ && node_alloc_ && vector_alloc_) {
            auto ptr = GetNodePtr(id);
            if (ptr.Get()) {
                auto &alloc = *const_cast<duckdb::FixedSizeAllocator *>(node_alloc_.get());
                auto &valloc = *const_cast<duckdb::FixedSizeAllocator *>(vector_alloc_.get());
                auto &ptr_mutable = const_cast<duckdb::IndexPointer &>(ptr);
                auto *hdr = reinterpret_cast<duckdb::vex::HNSWNodeHeader<T> *>(alloc.Get(ptr_mutable, /*dirty=*/false));
                if (hdr->vector_ptr.Get()) {
                    auto &vptr = const_cast<duckdb::IndexPointer &>(hdr->vector_ptr);
                    return reinterpret_cast<const char *>(valloc.Get(vptr, /*dirty=*/false));
                }
            }
        }
        // 镜像超预算的 id：vectors[id] 为空，上面的 vector_alloc_ 路径已返回。
        // 若 allocator 缺失(理论上 ApplyMirrorBudget 已保证不会收紧)则返回 nullptr，
        // 绝不对空 slot 取 .data() 后按 vec_size memcpy 造成越界读。
        if (id >= vectors.size() || vectors[id].empty()) {
            return nullptr;
        }
        return vectors[id].data();
    }
    const char *get_data(T id) const {
        // Build-only lock — see non-const overload.
        if (!parallel_build_active_.load(std::memory_order_acquire)) {
            return get_data_unlocked(id);
        }
        LWLockAcquire(const_cast<LWLock *>(&elems_veclock), LW_SHARED);
        const char *r = get_data_unlocked(id);
        LWLockRelease(const_cast<LWLock *>(&elems_veclock));
        return r;
    }

    struct my_buf {
        const char *c;
        explicit my_buf(const char *c) : c(c) {}
        char *get_vecbuf() const { return (char *)c; }
        static constexpr void release() {}
    };

    my_buf read_data(T id) { return my_buf{get_data(id)}; }
    my_buf read_data(T id) const { return my_buf{get_data(id)}; }

    void reset_neighbors_val_pool() {
    }

    template <typename Distancer, typename IdVec>
    void get_distance_batch(const Distancer &distancer, const char *query, const IdVec &ids, float *dists) {
        std::vector<void *> vals;
        vals.reserve(ids.size());
        for (auto id : ids) {
            vals.push_back(get_data(id));
        }
        distancer.get_distance_batch2(query, vals.data(), uint16(dim), uint16(vals.size()), dists);
    }

    template <typename Distancer>
    float get_distance(const Distancer &distancer, const char *query, T id) {
        return distancer.get_distance_single(query, get_data(id), uint16(dim));
    }
    template <typename Distancer>
    float get_distance(const Distancer &distancer, const char *query, const char *val) {
        return distancer.get_distance_single(query, val, uint16(dim));
    }
    template <typename Distancer>
    float get_distance_precise(const Distancer &distancer, const char *query, const char *val) {
        return distancer.get_distance_single(query, val, uint16(dim));
    }
    template <typename Distancer>
    float get_distance_est(const Distancer &distancer, const char *query, T id) {
        return get_distance(distancer, query, id);
    }

    // lock_point / unlock_point: 并发 build 时 algorithm 用来锁单个节点的
    // 邻居数组。用 striped LWLock：idx & STRIPE_MASK 命中 64 把锁里某一把。
    // unified unlock 让 caller 不需要传 shared/exclusive 标志。
    template <bool is_base_layer, bool shared_lock>
    void lock_point(T idx) {
        if constexpr (shared_lock) {
            if (search_lock_free_.load(std::memory_order_acquire)) return;
        }
        auto &lock = (is_base_layer ? base_point_locks_ : upper_point_locks_)[idx & STRIPE_MASK].lock;
        if constexpr (shared_lock) {
            LWLockAcquire(&lock, LW_SHARED);
        } else {
            LWLockAcquire(&lock, LW_EXCLUSIVE);
        }
    }
    template <bool is_base_layer, bool shared_lock>
    void unlock_point(T idx) {
        if constexpr (shared_lock) {
            if (search_lock_free_.load(std::memory_order_acquire)) return;
        }
        auto &lock = (is_base_layer ? base_point_locks_ : upper_point_locks_)[idx & STRIPE_MASK].lock;
        LWLockRelease(&lock);
    }

    template <bool is_base_layer>
    auto get_point_info(T idx) {
        if constexpr (is_base_layer) {
            if (node_alloc_) {
                auto ptr = GetNodePtr(idx);
                if (ptr.Get()) {
                    auto *header = reinterpret_cast<duckdb::vex::HNSWNodeHeader<T> *>(node_alloc_->Get(ptr));
                    return std::make_tuple(header->GetLevel0Neighbors(), idx, idx);
                }
            }
            auto &bp = base_points[idx];
            return std::make_tuple(bp.neighbors.data(), idx, idx);
        } else {
            // Same caveat as get_neighbors<false>: HNSWUpperLevel's on-disk segment
            // only stores neighbor IDs (no cur_layer_idxs), but the algorithm reads
            // `neighbors + m` as cur_layer_idxs. up.neighbors_info has the full
            // [IDs | cur_layer_idxs] layout the algorithm expects, so read there.
            auto &up = upper_points[idx];
            return std::make_tuple(up.neighbors_info.data(), up.lower_layer_idx, up.id);
        }
    }

    template <bool is_base_layer, typename CandVec, typename CandType>
    void get_neighbors(CandVec &out, const CandType &cand) {
        if constexpr (is_base_layer) {
            T *neighbors = nullptr;
            float *dists = nullptr;
            size_t max_count = m * 2;
            
            if (node_alloc_) {
                auto ptr = GetNodePtr(cand.cur_layer_idx);
                if (ptr.Get()) {
                    auto *header = reinterpret_cast<duckdb::vex::HNSWNodeHeader<T> *>(node_alloc_->Get(ptr));
                    neighbors = header->GetLevel0Neighbors();
                    max_count = header->level0_count > 0 ? header->level0_count : m * 2;
                }
            }
            
            auto &bp = base_points[cand.cur_layer_idx];
            if (!neighbors) {
                neighbors = bp.neighbors.data();
            }
            dists = bp.dists.data();
            
            out.reserve(max_count);
            for (size_t i = 0; i < max_count; ++i) {
                auto id = neighbors[i];
                if (id == T(INVALID_VECTOR_ID)) {
                    break;
                }
                out.emplace_back(id, id, dists[i]);
            }
        } else {
            // HNSWUpperLevel's on-disk segment only stores neighbor IDs per layer
            // slot (no cur_layer_idxs); cur_layer_idxs live exclusively in the
            // in-memory up.neighbors_info second half. Reading from upper_alloc_
            // and treating "neighbors + m" as cur_layer_idxs would land in the
            // *next* layer's id array — bogus values that may collide with
            // self.id and trip Assert(nbr.id != self.id) downstream. Always read
            // from the in-memory authoritative copy.
            auto &up = upper_points[cand.cur_layer_idx];
            const T *neighbors = up.neighbors_info.data();
            const T *cur_layer_idxs = neighbors + m;
            const float *dists = up.dists.data();

            out.reserve(m);
            for (size_t i = 0; i < size_t(m); ++i) {
                if (neighbors[i] == T(INVALID_VECTOR_ID)) {
                    break;
                }
                out.emplace_back(neighbors[i], cur_layer_idxs[i], dists[i]);
            }
        }
    }

    template <bool is_base_layer>
    auto get_neighbor_stats(T idx) {
        if constexpr (is_base_layer) {
            auto &bp = base_points[idx];
            return std::make_pair(bp.dists.data(), BitSpan<uint32>(bp.stat_words.data(), bp.neighbors.size()));
        } else {
            auto &up = upper_points[idx];
            return std::make_pair(up.dists.data(), BitSpan<uint32>(up.stat_words.data(), size_t(m)));
        }
    }

    bool has_stat(BitSpan<uint32>) const {
        return false;
    }
    void set_stat(BitSpan<uint32>) {
    }

    template <bool is_base_layer>
    void set_neighbor(T cur_layer_idx, int16 pruned, T newpoint_id, T newpoint_cur_layer_idx) {
        if (pruned < 0) {
            return;
        }
        if constexpr (is_base_layer) {
            auto &bp = base_points[cur_layer_idx];
            if (size_t(pruned) < bp.neighbors.size()) {
                bp.neighbors[pruned] = newpoint_id;
            }

            if (node_alloc_) {
                auto ptr = GetNodePtr(cur_layer_idx);
                if (ptr.Get()) {
                    auto *header = reinterpret_cast<duckdb::vex::HNSWNodeHeader<T> *>(node_alloc_->Get(ptr));
                    auto *neighbors = header->GetLevel0Neighbors();
                    if (size_t(pruned) < size_t(m) * 2) {
                        neighbors[pruned] = newpoint_id;
                        // Update level0_count to track the highest occupied slot.
                        // Without this, reload-time logic that trusts level0_count
                        // (P8' patch + get_neighbors max_count) misses neighbors
                        // added via set_neighbor and overwrites them as INVALID.
                        if (newpoint_id != T(INVALID_VECTOR_ID) &&
                            uint16_t(pruned + 1) > header->level0_count) {
                            header->level0_count = uint16_t(pruned + 1);
                        }
                    }
                }
            }
        } else {
            auto &up = upper_points[cur_layer_idx];
            if (size_t(pruned) < size_t(m)) {
                up.neighbors_info[pruned] = newpoint_id;
                up.neighbors_info[size_t(m) + size_t(pruned)] = newpoint_cur_layer_idx;
            }

            if (upper_alloc_ && cur_layer_idx < upper_idx_to_ptr_.size()) {
                auto ptr = upper_idx_to_ptr_[cur_layer_idx];
                if (ptr.Get()) {
                    auto *upper = reinterpret_cast<duckdb::vex::HNSWUpperLevel<T> *>(upper_alloc_->Get(ptr));
                    auto *neighbors = upper->GetNeighbors(0, m);
                    if (size_t(pruned) < size_t(m)) {
                        neighbors[pruned] = newpoint_id;
                        if (newpoint_id != T(INVALID_VECTOR_ID) &&
                            uint16_t(pruned + 1) > upper->counts[0]) {
                            upper->counts[0] = uint16_t(pruned + 1);
                        }
                    }
                }
            }
        }
    }

    void set_base_neighbors(T id, const T *neighbors_id) {
        auto &bp = base_points[id];
        bp.neighbors.assign(neighbors_id, neighbors_id + m * 2);

        if (node_alloc_) {
            auto ptr = GetNodePtr(id);
            if (ptr.Get()) {
                auto *header = reinterpret_cast<duckdb::vex::HNSWNodeHeader<T> *>(node_alloc_->Get(ptr));
                std::memcpy(header->GetLevel0Neighbors(), neighbors_id, m * 2 * sizeof(T));
                header->level0_count = 0;
                for (int i = 0; i < m * 2; ++i) {
                    if (neighbors_id[i] != T(INVALID_VECTOR_ID)) {
                        header->level0_count = i + 1;
                    }
                }
            }
        }
    }
    void set_upper_neighbors(T idx, const T *neighbors_info) {
        auto &up = upper_points[idx];
        up.neighbors_info.assign(neighbors_info, neighbors_info + m * 2);

        if (upper_alloc_ && idx < upper_idx_to_ptr_.size()) {
            auto ptr = upper_idx_to_ptr_[idx];
            if (ptr.Get()) {
                auto *upper = reinterpret_cast<duckdb::vex::HNSWUpperLevel<T> *>(upper_alloc_->Get(ptr));
                std::memcpy(upper->GetNeighbors(0, m), neighbors_info, m * sizeof(T));
                upper->counts[0] = 0;
                for (int i = 0; i < m; ++i) {
                    if (neighbors_info[i] != T(INVALID_VECTOR_ID)) {
                        upper->counts[0] = i + 1;
                    }
                }
            }
        }
    }

    void add_basepoint(T id, const T *neighbors_id) {
        if (id >= base_points.size()) {
            base_points.resize(id + 1, MakeBasePoint());
        }
        auto &bp = base_points[id];
        bp.neighbors.assign(neighbors_id, neighbors_id + m * 2);
        base_layer.current_size = base_points.size();

        if (node_alloc_) {
            auto ptr = GetNodePtr(id);
            if (ptr.Get()) {
                auto *header = reinterpret_cast<duckdb::vex::HNSWNodeHeader<T> *>(node_alloc_->Get(ptr));
                std::memcpy(header->GetLevel0Neighbors(), neighbors_id, m * 2 * sizeof(T));
                header->level0_count = 0;
                for (int i = 0; i < m * 2; ++i) {
                    if (neighbors_id[i] != T(INVALID_VECTOR_ID)) {
                        header->level0_count = i + 1;
                    }
                }
            }
        }
    }

    void add_upperpoint(T cur_layer_idx, T lower_layer_idx, T id, const T *neighbors_info) {
        if (cur_layer_idx >= upper_points.size()) {
            upper_points.resize(cur_layer_idx + 1, MakeUpperPoint());
        }
        /* Publish this point's own slot under its stripe lock. During parallel
         * BuildBulk another worker can reach cur_layer_idx via a reverse edge
         * (update_reverse_edges<false> in insert_new_point runs right after this)
         * and read up.lower_layer_idx / up.id / up.neighbors_info via the readers'
         * lock_point<false,true> (search_upper_layer / search_layer). Those fields
         * default to INVALID_VECTOR_ID in MakeUpperPoint; without a matching
         * exclusive lock here the read has no happens-before and may observe the
         * stale INVALID lower_layer_idx, which is then used as a raw base-layer
         * index → out-of-bounds SEGV. (Base layer is immune: its lower_layer_idx
         * is the identity id, never a stored field.) */
        lock_point<false, false>(cur_layer_idx);
        auto &up = upper_points[cur_layer_idx];
        up.lower_layer_idx = lower_layer_idx;
        up.id = id;
        up.neighbors_info.assign(neighbors_info, neighbors_info + m * 2);
        upper_layer.current_size = upper_points.size();

        if (upper_alloc_ && cur_layer_idx < upper_idx_to_ptr_.size()) {
            auto ptr = upper_idx_to_ptr_[cur_layer_idx];
            if (ptr.Get()) {
                auto *upper = reinterpret_cast<duckdb::vex::HNSWUpperLevel<T> *>(upper_alloc_->Get(ptr));
                upper->lower_layer_idx = lower_layer_idx;
                upper->id = id;
                std::memcpy(upper->GetNeighbors(0, m), neighbors_info, m * sizeof(T));
                upper->counts[0] = 0;
                for (int i = 0; i < m; ++i) {
                    if (neighbors_info[i] != T(INVALID_VECTOR_ID)) {
                        upper->counts[0] = i + 1;
                    }
                }
            }
        }
        unlock_point<false, false>(cur_layer_idx);
    }

    template <typename Func>
    bool apply_elem(T id, Func &&func) {
        return func(elems[id]);
    }

    template <typename Func>
    void get_itempointer(T id, Func &&func) {
        func(&elems[id]);
    }

    template <typename Func>
    void for_each_async_id(Func &&func) {
        for (auto id : async_ids) {
            func(id);
        }
    }

    bool fetch_vec_from_heap(ItemPointerData tid, char *dest) {
        auto id = static_cast<size_t>(tid.row_id);
        if (id >= vectors.size()) {
            return false;
        }
        std::memcpy(dest, get_data(id), vec_size);
        return true;
    }
    bool fetch_vec_from_heap(PointExtensionContext &, T id, char *dest) {
        if (id >= vectors.size()) {
            return false;
        }
        std::memcpy(dest, get_data(id), vec_size);
        return true;
    }

private:
    BasePointRec MakeBasePoint() const {
        BasePointRec bp;
        bp.neighbors.assign(m * 2, T(INVALID_VECTOR_ID));
        bp.dists.assign(m * 2, INVALID_DIST);
        bp.stat_words.assign((m * 2 + 31) / 32, 0);
        return bp;
    }
    UpperPointRec MakeUpperPoint() const {
        UpperPointRec up;
        up.neighbors_info.assign(m * 2, T(INVALID_VECTOR_ID));
        up.dists.assign(m, INVALID_DIST);
        up.stat_words.assign((m + 31) / 32, 0);
        return up;
    }
};
