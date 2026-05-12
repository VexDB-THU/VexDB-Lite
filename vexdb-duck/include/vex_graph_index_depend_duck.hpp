#pragma once

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

enum class QuantizerType : uint8 {
    NONE = 0,
    PQ = 1,
    RABITQ = 2
};

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

    bool empty() const {
        return tids.empty();
    }
    bool insert_tid(PointExtensionContext &, Span<const Data> data, bool &overwritten) {
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

struct GraphIndexCluster : public GraphIndexPoint {
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

template <typename IdType = uint32, typename elem_type = GraphIndexPoint>
class MemStore;

template <typename IdType, typename elem_type>
class MemStore {
public:
    using T = IdType;
    using point_type = elem_type;

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
    GraphIndexEntryInfo entry_info;
    LayerView base_layer;
    LayerView upper_layer;

    std::vector<point_type> elems;
    std::vector<std::vector<char>> vectors;
    std::vector<BasePointRec> base_points;
    std::vector<UpperPointRec> upper_points;
    std::vector<T> async_ids;

    duckdb::unique_ptr<duckdb::FixedSizeAllocator> node_alloc_;
    duckdb::unique_ptr<duckdb::FixedSizeAllocator> vector_alloc_;
    duckdb::unique_ptr<duckdb::FixedSizeAllocator> upper_alloc_;

    // compact 模式：vector_alloc_ 被 Reset，header->vector_ptr 持失效 buffer_id。
    // get_data/GetNodeHeader 必须短路，否则对空 allocator 调 Get() → SIGSEGV。
    bool compact_mode_ = false;

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
            vectors.resize(base_n);
            base_points.resize(base_n, MakeBasePoint());
            base_layer.current_size = base_n;
            id_to_node_ptr_.resize(base_n);
            if (node_alloc_ && vector_alloc_) {
                for (size_t i = cur_base; i < base_n; i++) {
                    auto node_ptr = node_alloc_->New();
                    auto vec_ptr = vector_alloc_->New();
                    auto *header = reinterpret_cast<duckdb::vex::HNSWNodeHeader<T> *>(node_alloc_->Get(node_ptr));
                    std::memset(header, 0, duckdb::vex::HNSWNodeHeader<T>::SegmentSize(m));
                    header->vector_ptr = vec_ptr;
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
                vectors.resize(id + 1);
                base_points.resize(id + 1, MakeBasePoint());
                base_layer.current_size.store(base_points.size(), std::memory_order_release);
                if (id >= id_to_node_ptr_.size()) {
                    id_to_node_ptr_.resize(id + 1);
                }
            }
            if (node_alloc_ && vector_alloc_ && !id_to_node_ptr_[id].Get()) {
                auto node_ptr = node_alloc_->New();
                auto vec_ptr = vector_alloc_->New();
                auto *header = reinterpret_cast<duckdb::vex::HNSWNodeHeader<T> *>(node_alloc_->Get(node_ptr));
                std::memset(header, 0, duckdb::vex::HNSWNodeHeader<T>::SegmentSize(m));
                header->vector_ptr = vec_ptr;
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
    // assign_vector_id 独占分配），写入 elems[id]/vectors[id] 的 inner 字段
    // 在 id 维度上不竞争。需要的是 SHARED elems_veclock 防止外层 realloc
    // 让 elems[id] 的 reference 失效。
    void add_elem(PointExtensionContext &ctx, T id, const ItemPointerData &tid) {
        (void)ctx;
        LWLockAcquire(&elems_veclock, LW_SHARED);
        elems[id].tids.push_back(tid);
        if (node_alloc_) {
            auto ptr = GetNodePtr(id);
            if (ptr.Get()) {
                auto *header = reinterpret_cast<duckdb::vex::HNSWNodeHeader<T> *>(node_alloc_->Get(ptr));
                header->row_id = tid.row_id;
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
        vectors[id].assign(store_data, store_data + vec_size);
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

    void ResizeForReload(size_t base_n, size_t upper_n) {
        elems.resize(base_n);
        vectors.resize(base_n);
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

    char *get_data(T id) {
        // Defensive bound check: after reload, neighbor slots past header->level0_count
        // can hold stale data from a previously freed segment. is_valid() only rejects
        // INVALID_VECTOR_ID; any other garbage value (e.g. 0xfffffff7) is treated as a
        // real id and routed here. Without this check we'd OOB into vectors[id].
        if (id >= vectors.size() && id >= elems.size()) {
            return nullptr;
        }
        if (!compact_mode_ && node_alloc_ && vector_alloc_) {
            auto ptr = GetNodePtr(id);
            if (ptr.Get()) {
                auto *hdr = reinterpret_cast<duckdb::vex::HNSWNodeHeader<T> *>(node_alloc_->Get(ptr));
                if (hdr->vector_ptr.Get()) {
                    return reinterpret_cast<char *>(vector_alloc_->Get(hdr->vector_ptr));
                }
            }
        }
        if (id >= vectors.size()) {
            return nullptr;
        }
        return vectors[id].data();
    }
    const char *get_data(T id) const {
        if (id >= vectors.size() && id >= elems.size()) {
            return nullptr;
        }
        if (!compact_mode_ && node_alloc_ && vector_alloc_) {
            auto ptr = GetNodePtr(id);
            if (ptr.Get()) {
                auto &alloc = *const_cast<duckdb::FixedSizeAllocator *>(node_alloc_.get());
                auto &valloc = *const_cast<duckdb::FixedSizeAllocator *>(vector_alloc_.get());
                auto &ptr_mutable = const_cast<duckdb::IndexPointer &>(ptr);
                auto *hdr = reinterpret_cast<duckdb::vex::HNSWNodeHeader<T> *>(alloc.Get(ptr_mutable));
                if (hdr->vector_ptr.Get()) {
                    auto &vptr = const_cast<duckdb::IndexPointer &>(hdr->vector_ptr);
                    return reinterpret_cast<const char *>(valloc.Get(vptr));
                }
            }
        }
        if (id >= vectors.size()) {
            return nullptr;
        }
        return vectors[id].data();
    }

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
        auto &lock = (is_base_layer ? base_point_locks_ : upper_point_locks_)[idx & STRIPE_MASK].lock;
        if constexpr (shared_lock) {
            LWLockAcquire(&lock, LW_SHARED);
        } else {
            LWLockAcquire(&lock, LW_EXCLUSIVE);
        }
    }
    template <bool is_base_layer, bool /*shared_lock*/>
    void unlock_point(T idx) {
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
