// SQLite 宿主依赖头 —— GraphIndexAlgorithm 的注入点（对标
// vex_graph_index_depend_duck.hpp 的角色，经 common/include/graph_index/
// graph_index_depend.h 的 PG_VEXDB_TARGET_SQLITE 分支进入）。
//
// 与 DuckDB 版的差异：
//   - 纯内存 MemStore：无 FixedSizeAllocator/BlockManager 双写路径（SQLite 的
//     持久化走 %_graph shadow 表序列化，见 M3 桥接层），数组即唯一事实源。
//   - 单线程：M3 全部锁为 no-op（SQLite 串行模型 + 构建期独占）。M3+ 并行建图
//     时在此补 striped lock 与 publish fence（接口位已保留，语义见 duck 版
//     add_upperpoint 的 publish race 注释——那是必须正面解决的唯一真险点）。
//   - ItemPointerData 用 SQLite rowid（int64）。
//
// store 命名沿用 MemStore：algorithm.h 用 std::is_same<Store, MemStore<T,pt>>
// 判定 mem_store 特化分支（need_refine/has_est 关断），同名让分支自动正确。
#pragma once

#include <atomic>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <stdexcept>
#include <tuple>
#include <unordered_set>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "vtl/span"
// 注意：vtl/bitvector 依赖下方的 Assert/unlikely 宏，在宏定义之后才 include
// （对齐 duck.hpp 的顺序约定）。

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

class BaseObject {};

struct PointExtensionContext;

enum class DistPrecisionType : uint8_t;

#ifndef Assert
#define Assert(cond) ((void)0)
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

// PG elog shim（algorithm.h 的 REPORT_PERF(NOTICE) 等需要；对齐 duck_pg_shim）。
enum {
    DEBUG5 = 10, DEBUG4 = 11, DEBUG3 = 12, DEBUG2 = 13, DEBUG1 = 14,
    LOG = 15, INFO = 17, NOTICE = 18, WARNING = 19, ERROR = 20, FATAL = 21, PANIC = 22,
};

#ifndef elog
#include <cstdio>
#include <stdexcept>
#define elog(level, ...) \
    do { \
        if ((level) >= ERROR) { \
            char _buf[512]; \
            std::snprintf(_buf, sizeof(_buf), __VA_ARGS__); \
            throw std::runtime_error(_buf); \
        } else { \
            std::fprintf(stderr, "[vexdb-sqlite] " __VA_ARGS__); \
            std::fprintf(stderr, "\n"); \
        } \
    } while (0)
#endif

using Oid = uint32_t;

inline bool OidIsValid(Oid oid) { return oid != 0; }

inline Oid index_getprocid(void *, int, int) { return 0; }

#include "quantizer_type.h"

using Relation = void *;

// SQLite 行标识：rowid（int64）。保留 PG TID 字段形状以兼容算法层可能的
// 字段触碰，但语义只有 row_id。
struct BlockIdData {
    uint16 bi_hi = 0;
    uint16 bi_lo = 0;
};

struct ItemPointerData {
    BlockIdData ip_blkid;
    uint16 ip_posid = 0;
    int64_t row_id = 0;
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
    bool operator<(const GraphIndexSearchRes &other) const { return dist < other.dist; }
};

template <typename T>
struct GraphIndexCandidate {
    T id;
    T cur_layer_idx;
    T lower_layer_idx;
    float dist;
    const char *val;

    GraphIndexCandidate()
        : id((T)INVALID_VECTOR_ID), cur_layer_idx((T)INVALID_VECTOR_ID),
          lower_layer_idx((T)INVALID_VECTOR_ID), dist(INVALID_DIST), val(nullptr) {}
    GraphIndexCandidate(T id_val, T cur_idxx, float dist_val)
        : id(id_val), cur_layer_idx(cur_idxx), lower_layer_idx((T)INVALID_VECTOR_ID),
          dist(dist_val), val(nullptr) {}
    GraphIndexCandidate(T id_val, T cur_idxx, T lower_idx, float dist_val, const char *val_ptr)
        : id(id_val), cur_layer_idx(cur_idxx), lower_layer_idx(lower_idx), dist(dist_val),
          val(val_ptr) {}
};

struct GraphIndexEntryInfo {
    size_t id;
    size_t cur_layer_idx;
    int_fast8_t level;

    GraphIndexEntryInfo() : id(INVALID_VECTOR_ID), cur_layer_idx(INVALID_VECTOR_ID), level(-1) {}
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

    // 并行 BuildBulk 时 dedup 可把多个 worker 路由到同一 point 的 tids；
    // 进程级粗锁串行化（duck 同款取舍：build 一次性，正确性优先）。
    static std::shared_mutex &tid_lock() {
        static std::shared_mutex m;
        return m;
    }

    bool empty() const { return tids.empty(); }
    // 非模板重载不可删：algorithm.h 以 braced initializer {ptr, size} 调用，
    // 模板形参推导不了 initializer list。
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
    uint32 get_tids(Vec &out, struct PointExtensionContext &) const {
        std::shared_lock<std::shared_mutex> _lk(tid_lock());
        for (const auto &tid : this->tids) {
            out.push_back(tid);
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
    void destroy() {}
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

// base/vec 持久化段的每段记录数（MemStore 段级 dirty 追踪与 DiskStore 段缓存
// 共用；序列化格式 v2/v3 的段切分粒度）。
constexpr size_t VEX_SEG_RECORDS = 64;

// L2 归一化（cosine 存储形态；零范数原样拷贝）。MemStore/DiskStore 的
// add_vector 与 GraphBridge 的查询归一化共用——存储侧与查询侧的数值规则
// 必须逐 bit 一致（cosine raw=-cos 的数值一致性契约），单一实现防漂移。
inline void VexNormalizeVec(float *dst, const float *src, size_t dim) {
    float norm2 = 0.0f;
    for (size_t i = 0; i < dim; i++) norm2 += src[i] * src[i];
    if (norm2 > 0.0f) {
        float inv = 1.0f / std::sqrt(norm2);
        for (size_t i = 0; i < dim; i++) dst[i] = src[i] * inv;
    } else if (dst != src) {
        std::memcpy(dst, src, dim * sizeof(float));
    }
}

// base 记录的 INVALID 填充（neighbors=INVALID_VECTOR_ID, dists=INVALID_DIST）。
// DiskStore 段 miss 模板、序列化尾段 padding 共用——三处独立编码会让
// "无邻居"的磁盘表示漂移（读侧按它判断邻居表结束）。
inline void VexFillInvalidBaseRec(char *rec, size_t m) {
    const size_t nb = m * 2;
    auto *neighbors = reinterpret_cast<uint32 *>(rec);
    auto *dists = reinterpret_cast<float *>(rec + nb * sizeof(uint32));
    for (size_t i = 0; i < nb; i++) {
        neighbors[i] = uint32(INVALID_VECTOR_ID);
        dists[i] = INVALID_DIST;
    }
}

// 空 upper 记录工厂（MemStore/DiskStore 共用——两 store 的"空记录"形状必须
// 一致，serialize/reload 在两模式间互通）。
template <typename Rec, typename T>
Rec VexMakeUpperPoint(size_t m) {
    Rec up;
    up.neighbors_info.assign(m * 2, T(INVALID_VECTOR_ID));
    up.dists.assign(m, INVALID_DIST);
    up.stat_words.assign((m + 31) / 32, 0);
    return up;
}

inline void vacuum_delay_point(bool) {}

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
        std::vector<T> neighbors_info;  // [m 个 ID | m 个 cur_layer_idx]
        std::vector<float> dists;
        std::vector<uint32> stat_words;
    };

    uint_fast16_t dim = 0;
    uint_fast16_t m = 0;
    uint_fast32_t vec_size = 0;

    GraphIndexEntryInfo entry_info;

    std::vector<point_type> elems;
    std::vector<std::vector<char>> vectors;
    std::vector<BasePointRec> base_points;
    std::vector<UpperPointRec> upper_points;
    std::vector<T> async_ids;

    bool normalize_vectors_ = false;
    // compact 模式只保留图结构与量化码，索引侧不再保存原始向量副本。
    bool compact_mode_ = false;

    // ---- 并发原语（M3+ 并行 BuildBulk）。语义对照 duck 版：----
    //   entry_mutex_      : 保护 entry_info（升级协议见 get_entry）
    //   entry_wait_mutex_ : 升级前抢 wait gate 阻拦后续 reader（防写者饥饿）
    //   elems_mutex_      : 保护 elems/vectors/base_points/upper_points 外层扩容
    //   striped locks     : 64 路按 idx&MASK 寻址的 per-node 锁（base/upper 两组）
    //   parallel_build_active_ : true=并行建图中（get_data 走 SHARED 锁）；
    //                            false=单线程/查询期（免锁快路径）
    // 单线程路径上这些锁全部 uncontended（~ns 级），不设跳锁开关。
    mutable std::shared_mutex entry_mutex_;
    mutable std::mutex entry_wait_mutex_;
    mutable std::shared_mutex elems_mutex_;
    static constexpr size_t STRIPE_COUNT = 64;
    static constexpr size_t STRIPE_MASK = STRIPE_COUNT - 1;
    mutable std::shared_mutex base_point_locks_[STRIPE_COUNT];
    mutable std::shared_mutex upper_point_locks_[STRIPE_COUNT];
    std::atomic<bool> parallel_build_active_{false};

    // 原子层 size：assign_vector_id fast-path 不持锁读（acquire），slow-path 在
    // EXCLUSIVE elems_mutex_ 下扩容后 release 写——建立 happens-before。
    std::atomic<size_t> base_size_{0};
    std::atomic<size_t> upper_size_{0};

    // 段级 dirty 追踪（mem 模式增量落盘：xSync 只 UPSERT 变更段，免每 commit
    // 清段+全量重写——1M 驻留图单行 insert 原要重写 ~0.8GB）。仅单线程增量
    // 写期启用；并行 BuildBulk 期关闭（防 8 线程标记 race），构建后由 bridge
    // 标 full_dirty 走全量。v4 也把 elems/upper 切成 64 条记录的小段，避免
    // 单行 INSERT/DELETE 在百万行图上重写 O(N) row-id/upper 元数据。
    bool track_dirty_ = true;
    std::unordered_set<uint32> dirty_elem_segs_, dirty_upper_segs_;
    std::unordered_set<uint32> dirty_base_segs_, dirty_vec_segs_;
    void mark_elem_dirty(T id) {
        if (track_dirty_) dirty_elem_segs_.insert(uint32(size_t(id) / VEX_SEG_RECORDS));
    }
    void mark_upper_dirty(T id) {
        if (track_dirty_) dirty_upper_segs_.insert(uint32(size_t(id) / VEX_SEG_RECORDS));
    }
    void mark_base_dirty(T id) {
        if (track_dirty_) dirty_base_segs_.insert(uint32(size_t(id) / VEX_SEG_RECORDS));
    }
    void mark_vec_dirty(T id) {
        if (track_dirty_) dirty_vec_segs_.insert(uint32(size_t(id) / VEX_SEG_RECORDS));
    }

    std::atomic<T> next_base_id_{0};
    std::atomic<T> next_upper_id_{0};

    MemStore() = default;
    MemStore(uint_fast16_t dim_in, uint_fast16_t m_in, uint_fast32_t vec_size_in)
        : dim(dim_in), m(m_in), vec_size(vec_size_in) {
        entry_info.set(INVALID_VECTOR_ID, INVALID_VECTOR_ID, -1);
    }

    // 并行 build 前调用：预留外层容量 + 预 resize 内层向量 buffer，使并行期
    // add_vector 只 memcpy 进稳定指针（防 get_data 读侧 dangling——duck 的
    // parallel-build publish race 教训）。
    void ReserveCapacity(size_t base_n, size_t upper_n) {
        std::unique_lock<std::shared_mutex> _lk(elems_mutex_);
        elems.reserve(base_n);
        vectors.reserve(base_n);
        base_points.reserve(base_n);
        // upper 容量按 base_n 全额预留（duck 同款取舍）：upper 记录数期望
        // n/(m-1)，但 level 分布长尾使"均值+常数 slack"的 resize 在 ~12%
        // (N=4 万) 到 ~41%(N=100 万) 的构建中被超出——超出时 slow path 的
        // upper_points.resize 触发 realloc，与只持 stripe 锁的读者
        // (get_point_info/get_neighbors/set_neighbor) racing = heap corruption。
        // 容量内的 resize 增长不迁移既有元素（引用稳定），全额 reserve 把
        // realloc 风险归零；代价 = base_n × sizeof(UpperPointRec) 原始容量
        // (~80B/条，1M 行 ~80MB 构建期一次性)，初始构造仍按 upper_n 估算。
        upper_points.reserve(std::max(base_n, upper_n));
        if (base_n > elems.size()) {
            size_t cur = vectors.size();
            elems.resize(base_n);
            vectors.resize(base_n);
            for (size_t i = cur; i < base_n; i++) {
                vectors[i].resize(vec_size);
            }
            base_points.resize(base_n, MakeBasePoint());
            base_size_.store(base_n, std::memory_order_release);
        }
        if (upper_n > upper_points.size()) {
            upper_points.resize(upper_n, MakeUpperPoint());
            upper_size_.store(upper_n, std::memory_order_release);
        }
    }

    // 双锁升级协议（照 duck/主库）：SHARED 读 entry；需要更高 level 或图空时
    // 先抢 wait gate 再升 EXCLUSIVE，防写者饥饿。
    template <bool exclusive = false, bool bottom_only = false>
    std::pair<GraphIndexEntryInfo, bool> get_entry(int_fast8_t insert_level = 0) {
        (void)bottom_only;
        entry_wait_mutex_.lock();
        entry_wait_mutex_.unlock();

        entry_mutex_.lock_shared();
        GraphIndexEntryInfo entry = entry_info;
        bool shared = true;
        if ((!exclusive && insert_level > entry.level) || entry.level < 0) {
            entry_mutex_.unlock_shared();
            entry_wait_mutex_.lock();
            entry_mutex_.lock();
            entry_wait_mutex_.unlock();
            entry = entry_info;
            shared = false;
        }
        return {entry, shared};
    }

    void release_entry_lock(bool shared) {
        if (shared) {
            entry_mutex_.unlock_shared();
        } else {
            entry_mutex_.unlock();
        }
    }

    // fast path：atomic fetch_add 拿独占 id，已在预留容量内则零锁返回；
    // slow path：EXCLUSIVE elems_mutex_ 下扩容（增量 Append 或未预留时）。
    template <bool is_base_layer>
    T assign_vector_id() {
        if constexpr (is_base_layer) {
            T id = next_base_id_.fetch_add(1, std::memory_order_acq_rel);
            if (size_t(id) < base_size_.load(std::memory_order_acquire)) {
                return id;
            }
            std::unique_lock<std::shared_mutex> _lk(elems_mutex_);
            if (size_t(id) + 1 > elems.size()) {
                elems.resize(id + 1);
                if (!compact_mode_) vectors.resize(id + 1);
                base_points.resize(id + 1, MakeBasePoint());
            }
            if (!compact_mode_ && vectors[id].size() != (size_t)vec_size) {
                vectors[id].resize(vec_size);
            }
            base_size_.store(elems.size(), std::memory_order_release);
            return id;
        } else {
            T idx = next_upper_id_.fetch_add(1, std::memory_order_acq_rel);
            if (size_t(idx) < upper_size_.load(std::memory_order_acquire)) {
                return idx;
            }
            std::unique_lock<std::shared_mutex> _lk(elems_mutex_);
            if (size_t(idx) + 1 > upper_points.size()) {
                upper_points.resize(idx + 1, MakeUpperPoint());
            }
            upper_size_.store(upper_points.size(), std::memory_order_release);
            return idx;
        }
    }

    void add_async_id(T id) {
        std::unique_lock<std::shared_mutex> _lk(elems_mutex_);
        async_ids.push_back(id);
    }

    // elems[id]/vectors[id] 的 id 维度由 assign_vector_id 独占分配不竞争；
    // SHARED elems_mutex_ 防外层 realloc 让引用失效；tid_lock 串行化 dedup
    // 路由到同一 id 的并发 push_back（duck 同款双锁）。
    void add_elem(PointExtensionContext &, T id, const ItemPointerData &tid) {
        std::shared_lock<std::shared_mutex> _lk(elems_mutex_);
        std::unique_lock<std::shared_mutex> _tl(GraphIndexPoint::tid_lock());
        elems[id].tids.push_back(tid);
        mark_elem_dirty(id);
    }
    void add_elem(PointExtensionContext &, T id, Span<const ItemPointerData> tids) {
        std::shared_lock<std::shared_mutex> _lk(elems_mutex_);
        std::unique_lock<std::shared_mutex> _tl(GraphIndexPoint::tid_lock());
        for (const auto &tid : tids) {
            elems[id].tids.push_back(tid);
        }
        mark_elem_dirty(id);
    }

    void add_vector(T id, const char *query) {
        const char *store_data = query;
        std::vector<char> normalized;
        if (normalize_vectors_) {
            normalized.resize(vec_size);
            VexNormalizeVec(reinterpret_cast<float *>(normalized.data()),
                            reinterpret_cast<const float *>(query), dim);
            store_data = normalized.data();
        }
        // SHARED：防外层 realloc。ReserveCapacity 已预 resize 内层 buffer →
        // 热路径 memcpy 进稳定指针（不 realloc，不挪 data() 指针）。
        std::shared_lock<std::shared_mutex> _lk(elems_mutex_);
        auto &slot = vectors[id];
        if (slot.size() == (size_t)vec_size) {
            std::memcpy(slot.data(), store_data, vec_size);
        } else {
            slot.assign(store_data, store_data + vec_size);
        }
        mark_vec_dirty(id);
    }

    template <typename Distancer>
    void add_vector(Distancer &, T id, const char *query) {
        add_vector(id, query);
    }

    void set_entrypoint(T id, T cur_layer_idx, int_fast8_t level) {
        entry_info.set(id, cur_layer_idx, level);
    }

    void *get_index() const { return nullptr; }
    DistPrecisionType get_precision() const { return static_cast<DistPrecisionType>(0); }
    uint16 get_dim() const { return uint16(dim); }
    uint32 get_vecsize() const { return uint32(vec_size); }
    uint32 get_elemsize() const { return uint32(vec_size); }
    size_t get_vector_num() const { return elems.size(); }

    template <bool is_base_layer>
    size_t max_id() const {
        if constexpr (is_base_layer) {
            return std::max<size_t>(next_base_id_.load(std::memory_order_relaxed),
                                    base_points.size()) + 16;
        } else {
            return std::max<size_t>(next_upper_id_.load(std::memory_order_relaxed),
                                    upper_points.size()) + 16;
        }
    }

    void ResizeForReload(size_t base_n, size_t upper_n) {
        elems.resize(base_n);
        if (!compact_mode_) vectors.resize(base_n);
        base_points.resize(base_n, MakeBasePoint());
        upper_points.resize(upper_n, MakeUpperPoint());
        next_base_id_.store(T(base_n), std::memory_order_relaxed);
        next_upper_id_.store(T(upper_n), std::memory_order_relaxed);
    }

    char *get_data_unlocked(T id) {
        if (id >= vectors.size()) return nullptr;
        return vectors[id].data();
    }
    const char *get_data_unlocked(T id) const {
        if (id >= vectors.size()) return nullptr;
        return vectors[id].data();
    }
    // 并行 build 期取 SHARED（防外层 realloc 撕裂读）；其余时间免锁快路径
    //（SQLite 查询是单线程 xFilter，无并发写）。
    char *get_data(T id) {
        if (!parallel_build_active_.load(std::memory_order_acquire)) {
            return get_data_unlocked(id);
        }
        std::shared_lock<std::shared_mutex> _lk(elems_mutex_);
        return get_data_unlocked(id);
    }
    const char *get_data(T id) const {
        if (!parallel_build_active_.load(std::memory_order_acquire)) {
            return get_data_unlocked(id);
        }
        std::shared_lock<std::shared_mutex> _lk(elems_mutex_);
        return get_data_unlocked(id);
    }

    struct my_buf {
        const char *c;
        explicit my_buf(const char *c) : c(c) {}
        char *get_vecbuf() const { return (char *)c; }
        static constexpr void release() {}
    };
    my_buf read_data(T id) { return my_buf{get_data(id)}; }
    my_buf read_data(T id) const { return my_buf{get_data(id)}; }

    void reset_neighbors_val_pool() {}

    template <typename Distancer, typename IdVec>
    void get_distance_batch(const Distancer &distancer, const char *query, const IdVec &ids,
                            float *dists) {
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

    // striped per-node 锁：idx & MASK 命中 64 路之一。查询期（非并行 build）
    // 无并发写，锁 uncontended，不设跳锁开关（duck 的 search_lock_free_ 是
    // 高并发查询优化，SQLite 串行查询不需要）。
    template <bool is_base_layer, bool shared_lock>
    void lock_point(T idx) {
        auto &lk = (is_base_layer ? base_point_locks_ : upper_point_locks_)[idx & STRIPE_MASK];
        if constexpr (shared_lock) {
            lk.lock_shared();
        } else {
            lk.lock();
        }
    }
    template <bool is_base_layer, bool shared_lock>
    void unlock_point(T idx) {
        auto &lk = (is_base_layer ? base_point_locks_ : upper_point_locks_)[idx & STRIPE_MASK];
        if constexpr (shared_lock) {
            lk.unlock_shared();
        } else {
            lk.unlock();
        }
    }

    template <bool is_base_layer>
    auto get_point_info(T idx) {
        if constexpr (is_base_layer) {
            auto &bp = base_points[idx];
            return std::make_tuple(bp.neighbors.data(), idx, idx);
        } else {
            auto &up = upper_points[idx];
            return std::make_tuple(up.neighbors_info.data(), up.lower_layer_idx, up.id);
        }
    }

    template <bool is_base_layer, typename CandVec, typename CandType>
    void get_neighbors(CandVec &out, const CandType &cand) {
        if constexpr (is_base_layer) {
            auto &bp = base_points[cand.cur_layer_idx];
            const T *neighbors = bp.neighbors.data();
            const float *dists = bp.dists.data();
            out.reserve(m * 2);
            for (size_t i = 0; i < size_t(m) * 2; ++i) {
                if (neighbors[i] == T(INVALID_VECTOR_ID)) break;
                out.emplace_back(neighbors[i], neighbors[i], dists[i]);
            }
        } else {
            auto &up = upper_points[cand.cur_layer_idx];
            const T *neighbors = up.neighbors_info.data();
            const T *cur_layer_idxs = neighbors + m;
            const float *dists = up.dists.data();
            out.reserve(m);
            for (size_t i = 0; i < size_t(m); ++i) {
                if (neighbors[i] == T(INVALID_VECTOR_ID)) break;
                out.emplace_back(neighbors[i], cur_layer_idxs[i], dists[i]);
            }
        }
    }

    template <bool is_base_layer>
    auto get_neighbor_stats(T idx) {
        if constexpr (is_base_layer) {
            auto &bp = base_points[idx];
            return std::make_pair(bp.dists.data(),
                                  BitSpan<uint32>(bp.stat_words.data(), bp.neighbors.size()));
        } else {
            auto &up = upper_points[idx];
            mark_upper_dirty(idx);
            return std::make_pair(up.dists.data(), BitSpan<uint32>(up.stat_words.data(), size_t(m)));
        }
    }

    bool has_stat(BitSpan<uint32>) const { return false; }
    void set_stat(BitSpan<uint32>) {}

    template <bool is_base_layer>
    void set_neighbor(T cur_layer_idx, int16 pruned, T newpoint_id, T newpoint_cur_layer_idx) {
        if (pruned < 0) return;
        if constexpr (is_base_layer) {
            auto &bp = base_points[cur_layer_idx];
            if (size_t(pruned) < bp.neighbors.size()) {
                bp.neighbors[pruned] = newpoint_id;
                // 同段的 dists 直写（update_reverse_edges 经 get_neighbor_stats
                // 指针）由此标记一并覆盖
                mark_base_dirty(cur_layer_idx);
            }
        } else {
            auto &up = upper_points[cur_layer_idx];
            if (size_t(pruned) < size_t(m)) {
                up.neighbors_info[pruned] = newpoint_id;
                up.neighbors_info[size_t(m) + size_t(pruned)] = newpoint_cur_layer_idx;
                mark_upper_dirty(cur_layer_idx);
            }
        }
    }

    void set_base_neighbors(T id, const T *neighbors_id) {
        base_points[id].neighbors.assign(neighbors_id, neighbors_id + m * 2);
        mark_base_dirty(id);
    }
    void set_upper_neighbors(T idx, const T *neighbors_info) {
        upper_points[idx].neighbors_info.assign(neighbors_info, neighbors_info + m * 2);
        mark_upper_dirty(idx);
    }

    void add_basepoint(T id, const T *neighbors_id) {
        if (id >= base_points.size()) {
            std::unique_lock<std::shared_mutex> _lk(elems_mutex_);
            if (id >= base_points.size()) {
                base_points.resize(id + 1, MakeBasePoint());
                base_size_.store(base_points.size(), std::memory_order_release);
            }
        }
        base_points[id].neighbors.assign(neighbors_id, neighbors_id + m * 2);
        mark_base_dirty(id);
    }

    // publish fence（duck #26 forward-publish race 的根治同款）：另一 worker 可
    // 经反向边在本函数返回前到达 cur_layer_idx，读 up.lower_layer_idx/id；不持
    // EXCLUSIVE stripe 锁写则无 happens-before，读侧可能看到 MakeUpperPoint 的
    // INVALID 默认值并当裸 base 索引使用 → 越界。base 层免疫（lower=identity）。
    void add_upperpoint(T cur_layer_idx, T lower_layer_idx, T id, const T *neighbors_info) {
        if (cur_layer_idx >= upper_points.size()) {
            std::unique_lock<std::shared_mutex> _lk(elems_mutex_);
            if (cur_layer_idx >= upper_points.size()) {
                upper_points.resize(cur_layer_idx + 1, MakeUpperPoint());
                upper_size_.store(upper_points.size(), std::memory_order_release);
            }
        }
        lock_point<false, false>(cur_layer_idx);
        auto &up = upper_points[cur_layer_idx];
        up.lower_layer_idx = lower_layer_idx;
        up.id = id;
        up.neighbors_info.assign(neighbors_info, neighbors_info + m * 2);
        mark_upper_dirty(cur_layer_idx);
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
        if (id >= vectors.size()) return false;
        std::memcpy(dest, get_data(T(id)), vec_size);
        return true;
    }
    bool fetch_vec_from_heap(PointExtensionContext &, T id, char *dest) {
        if (id >= vectors.size()) return false;
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
        return VexMakeUpperPoint<UpperPointRec, T>(size_t(m));
    }
};

// ---------------------------------------------------------------------------
// DiskStore（M9'：内存有界的段式磁盘 store，对齐 PG 双层设计的 SQLite 版）。
//
// 数据分层（1M 行 128 维 m=16 量化）：
//   常驻：meta/entry（KB）、elems/rowid 映射（~16MB）、upper 层（~13MB）
//   段式 LRU：base 邻居+dists（~256MB）、向量（~512MB）—— 按 SEG_RECORDS 条
//   定长记录切段，经注入的 PageIO 回调读写 %_graph(kind, seg, data) shadow 行。
//
// 设计要点：
//   - 单线程（SQLite 串行模型）：锁全 no-op；不支持并行 BuildBulk。
//   - store 不持 sqlite3 句柄：I/O 经 PageIO std::function 注入，全程主线程。
//   - 指针生命期：get_distance_batch/get_distance 内部直取段指针即取即算
//     （零拷贝、绝不跨段存活）；get_data（喂 cand.val 的 get_neighbors_data
//     路径）走 val pool 拷贝，reset_neighbors_val_pool 整体释放——对齐 PG
//     DiskStore 的 val pool 语义。get_point_info 返回的段内指针靠"每 cache
//     pin 最近访问段"保护（算法层在下一次段访问前必已消费完，见 search_layer
//     拷贝邻居的调用序）。
//   - dirty 段只在写事务内产生（构建/增量插入）；只读打开 write 回调为空，
//     evict dirty 段时 write 缺失视为协议违规（抛错）。
// ---------------------------------------------------------------------------
template <typename IdType = uint32, typename elem_type = GraphIndexPoint>
class DiskStore {
public:
    using T = IdType;
    using point_type = elem_type;
    using BasePointRec = typename MemStore<IdType, elem_type>::BasePointRec;
    using UpperPointRec = typename MemStore<IdType, elem_type>::UpperPointRec;

    static constexpr bool use_dist_cache = false;
    static constexpr bool has_occlusion_cache = true;
    static constexpr bool clustered = false;

    // %_graph(kind, seg, data) 的 kind 取值（格式 v2/v3，GraphBridge 序列化共用）。
    // v3 = elems 支持空壳节点（tid 摘除后 tids 为空，DELETE/UPDATE 增量化）。
    static constexpr int KIND_META = 0;
    static constexpr int KIND_ELEMS = 1;
    static constexpr int KIND_UPPER = 2;
    static constexpr int KIND_BASE = 3;
    static constexpr int KIND_VEC = 4;
    static constexpr int KIND_QUANTIZER_CODES = 6;
    // 旧名字保留为源码兼容别名；持久化 kind 值不变。
    static constexpr int KIND_RABITQ_CODES = KIND_QUANTIZER_CODES;
    // 段粒度：HNSW 查询是随机点查，段越大 I/O 放大越狠（读整段只用一条记录，
    // miss 成本=整段 blob 读）。64 条 → vec 段 32KB（128 维）/ base 段 16KB
    //（m=16），向 PG 8KB page 的取向靠拢；1M 行 ≈ 15625 段/类，%_graph 行数
    // 与 SegCache map 均无压力。实测 4096 条（2MB 段）在 8MB 预算下换页
    // 放大到 QPS≈1 不可用。
    static constexpr size_t SEG_RECORDS = VEX_SEG_RECORDS;

    // read 返回 false=段不存在（构建中的新段，填 INVALID 模板）。
    // read_rec（M9'b 记录粒度读）：只读段 blob 的 [offset, offset+len) 进 dst
    //（blob 增量 I/O 只触达 offset 所在页，免整段拷贝）；缺失时退化整段 read。
    struct PageIO {
        std::function<bool(int kind, uint32 seg, std::vector<char> &out)> read;
        std::function<bool(int kind, uint32 seg, const std::vector<char> &data)> write;
        std::function<bool(int kind, uint32 seg, size_t offset, size_t len, char *dst)> read_rec;
    };

    uint_fast16_t dim = 0;
    uint_fast16_t m = 0;
    uint_fast32_t vec_size = 0;
    bool normalize_vectors_ = false;
    bool compact_mode_ = false;

    GraphIndexEntryInfo entry_info;
    std::vector<point_type> elems;          // 常驻：id → rowids
    std::vector<UpperPointRec> upper_points;  // 常驻：upper 层全量
    bool upper_dirty = false;               // entry/meta 有改动
    std::unordered_set<uint32> dirty_elem_segs_, dirty_upper_segs_;

    size_t next_base_id_ = 0;
    size_t next_upper_id_ = 0;

    DiskStore(uint_fast16_t dim_in, uint_fast16_t m_in, uint_fast32_t vec_size_in,
              PageIO io, size_t cache_budget_bytes, uint_fast32_t quantizer_code_size_in = 0,
              bool compact_mode_in = false)
        : dim(dim_in), m(m_in), vec_size(vec_size_in), io_(std::move(io)),
          cache_budget_(cache_budget_bytes), quantizer_code_size_(quantizer_code_size_in) {
        compact_mode_ = compact_mode_in;
        entry_info.set(INVALID_VECTOR_ID, INVALID_VECTOR_ID, -1);
        stat_scratch_.assign((size_t(m) * 2 + 31) / 32, 0);
        // 预算下限：每种启用的数据各至少 2 段（pin 最近段 + 载入新段），
        // 防写 miss 无处驱逐。量化 code 与 base/vec 共用同一预算。
        size_t min_budget = 2 * seg_bytes(KIND_BASE);
        if (!compact_mode_) min_budget += 2 * seg_bytes(KIND_VEC);
        if (quantizer_code_size_ > 0) min_budget += 2 * seg_bytes(KIND_QUANTIZER_CODES);
        if (cache_budget_ < min_budget) cache_budget_ = min_budget;
    }

    size_t base_rec_bytes() const { return size_t(m) * 2 * (sizeof(T) + sizeof(float)); }
    size_t seg_bytes(int kind) const {
        if (kind == KIND_BASE) return SEG_RECORDS * base_rec_bytes();
        if (kind == KIND_QUANTIZER_CODES) return SEG_RECORDS * size_t(quantizer_code_size_);
        return SEG_RECORDS * size_t(vec_size);
    }

    size_t cache_bytes_used() const { return total_cache_bytes(); }
    size_t cache_budget_bytes() const { return cache_budget_; }
    size_t quantizer_code_size() const { return size_t(quantizer_code_size_); }
    size_t rabitq_code_size() const { return quantizer_code_size(); }
    void mark_elem_dirty(T id) {
        dirty_elem_segs_.insert(uint32(size_t(id) / SEG_RECORDS));
    }
    void mark_upper_dirty(T id) {
        dirty_upper_segs_.insert(uint32(size_t(id) / SEG_RECORDS));
        upper_dirty = true;
    }

    const char *get_quantizer_code(T id) {
        if (quantizer_code_size_ == 0) {
            throw std::runtime_error("vexdb-sqlite: quantizer code cache is not configured");
        }
        if (size_t(id) >= next_base_id_) {
            throw std::runtime_error("quantizer code coverage does not match graph nodes");
        }
        return quantizer_code_rec(id, false);
    }
    void set_quantizer_code(T id, const uint8_t *code) {
        if (quantizer_code_size_ == 0 || code == nullptr) {
            throw std::runtime_error("vexdb-sqlite: invalid quantizer code write");
        }
        std::memcpy(quantizer_code_rec(id, true), code, quantizer_code_size_);
    }
    const char *get_rabitq_code(T id) { return get_quantizer_code(id); }
    void set_rabitq_code(T id, const uint8_t *code) { set_quantizer_code(id, code); }

    // ---- entry（常驻，单线程无锁） ----
    template <bool exclusive = false, bool bottom_only = false>
    std::pair<GraphIndexEntryInfo, bool> get_entry(int_fast8_t = 0) {
        return {entry_info, true};
    }
    void release_entry_lock(bool) {}
    void set_entrypoint(T id, T cur_layer_idx, int_fast8_t level) {
        entry_info.set(id, cur_layer_idx, level);
        upper_dirty = true;
    }

    // ---- id 分配 / 元素（写路径，M9'b/c） ----
    template <bool is_base_layer>
    T assign_vector_id() {
        if constexpr (is_base_layer) {
            T id = T(next_base_id_++);
            if (elems.size() < next_base_id_) elems.resize(next_base_id_);
            return id;
        } else {
            T idx = T(next_upper_id_++);
            if (upper_points.size() < next_upper_id_)
                upper_points.resize(next_upper_id_, MakeUpperPoint());
            return idx;
        }
    }
    void add_async_id(T) {}
    template <typename Func>
    void for_each_async_id(Func &&) {}

    void add_elem(PointExtensionContext &, T id, const ItemPointerData &tid) {
        if (elems.size() <= size_t(id)) elems.resize(size_t(id) + 1);
        elems[id].tids.push_back(tid);
        mark_elem_dirty(id);
        upper_dirty = true;
    }
    void add_elem(PointExtensionContext &, T id, Span<const ItemPointerData> tids) {
        if (elems.size() <= size_t(id)) elems.resize(size_t(id) + 1);
        for (const auto &tid : tids) elems[id].tids.push_back(tid);
        mark_elem_dirty(id);
        upper_dirty = true;
    }
    template <typename Func>
    bool apply_elem(T id, Func &&func) { return func(elems[id]); }
    template <typename Func>
    void get_itempointer(T id, Func &&func) { func(&elems[id]); }

    // ---- 向量（vec 段） ----
    void add_vector(T id, const char *query) {
        const char *store_data = query;
        std::vector<char> normalized;
        if (normalize_vectors_) {
            normalized.resize(vec_size);
            VexNormalizeVec(reinterpret_cast<float *>(normalized.data()),
                            reinterpret_cast<const float *>(query), dim);
            store_data = normalized.data();
        }
        char *rec = vec_rec(id, /*mark_dirty=*/true);
        std::memcpy(rec, store_data, vec_size);
    }
    template <typename Distancer>
    void add_vector(Distancer &, T id, const char *query) { add_vector(id, query); }

    // get_data：喂 cand.val（get_neighbors_data 路径，指针跨多次段访问存活）
    // → val pool 拷贝。热路径距离计算不走这里（见 get_distance*）。
    char *get_data(T id) {
        char *buf = pool_alloc(vec_size);
        std::memcpy(buf, vec_rec(id, false), vec_size);
        return buf;
    }
    void reset_neighbors_val_pool() { val_pool_.clear(); }

    struct my_buf {
        std::vector<char> owned;
        char *get_vecbuf() const { return const_cast<char *>(owned.data()); }
        void release() {}
    };
    my_buf read_data(T id) {
        my_buf b;
        b.owned.assign(vec_rec(id, false), vec_rec(id, false) + vec_size);
        return b;
    }

    template <typename Distancer, typename IdVec>
    void get_distance_batch(const Distancer &distancer, const char *query, const IdVec &ids,
                            float *dists) {
        // 逐 id 即取即算：段指针绝不跨次存活（vals 收集式的 MemStore 模式在
        // 段 LRU 下会 dangling——第二段载入可能 evict 第一段）。
        for (size_t i = 0; i < ids.size(); ++i) {
            dists[i] = distancer.get_distance_single(query, vec_rec(ids[i], false), uint16(dim));
        }
    }
    template <typename Distancer>
    float get_distance(const Distancer &distancer, const char *query, T id) {
        return distancer.get_distance_single(query, vec_rec(id, false), uint16(dim));
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

    bool fetch_vec_from_heap(ItemPointerData tid, char *dest) {
        auto id = static_cast<size_t>(tid.row_id);
        if (id >= next_base_id_) return false;
        std::memcpy(dest, vec_rec(T(id), false), vec_size);
        return true;
    }
    bool fetch_vec_from_heap(PointExtensionContext &, T id, char *dest) {
        if (size_t(id) >= next_base_id_) return false;
        std::memcpy(dest, vec_rec(id, false), vec_size);
        return true;
    }

    // ---- 锁（单线程全 no-op） ----
    template <bool is_base_layer, bool shared_lock>
    void lock_point(T) {}
    template <bool is_base_layer, bool shared_lock>
    void unlock_point(T) {}

    // ---- 图结构访问 ----
    template <bool is_base_layer>
    auto get_point_info(T idx) {
        if constexpr (is_base_layer) {
            char *rec = base_rec(idx, false);
            return std::make_tuple(reinterpret_cast<T *>(rec), idx, idx);
        } else {
            auto &up = upper_points[idx];
            return std::make_tuple(up.neighbors_info.data(), up.lower_layer_idx, up.id);
        }
    }

    template <bool is_base_layer, typename CandVec, typename CandType>
    void get_neighbors(CandVec &out, const CandType &cand) {
        if constexpr (is_base_layer) {
            char *rec = base_rec(cand.cur_layer_idx, false);
            const T *neighbors = reinterpret_cast<const T *>(rec);
            const float *dists = reinterpret_cast<const float *>(rec + size_t(m) * 2 * sizeof(T));
            out.reserve(m * 2);
            for (size_t i = 0; i < size_t(m) * 2; ++i) {
                if (neighbors[i] == T(INVALID_VECTOR_ID)) break;
                out.emplace_back(neighbors[i], neighbors[i], dists[i]);
            }
        } else {
            auto &up = upper_points[cand.cur_layer_idx];
            const T *neighbors = up.neighbors_info.data();
            const T *cur_layer_idxs = neighbors + m;
            const float *dists = up.dists.data();
            out.reserve(m);
            for (size_t i = 0; i < size_t(m); ++i) {
                if (neighbors[i] == T(INVALID_VECTOR_ID)) break;
                out.emplace_back(neighbors[i], cur_layer_idxs[i], dists[i]);
            }
        }
    }

    // stat_words 是 dead 写（has_stat 恒 false，select_neighbors 把它当 scratch
    // 写但从不回读跨调用），给共享 scratch buffer，不进段格式。
    template <bool is_base_layer>
    auto get_neighbor_stats(T idx) {
        if constexpr (is_base_layer) {
            char *rec = base_rec(idx, true);  // update_reverse_edges 会写 dists[pruned]
            auto *dists = reinterpret_cast<float *>(rec + size_t(m) * 2 * sizeof(T));
            return std::make_pair(dists, BitSpan<uint32>(stat_scratch_.data(), size_t(m) * 2));
        } else {
            auto &up = upper_points[idx];
            dirty_upper_segs_.insert(uint32(size_t(idx) / SEG_RECORDS));
            upper_dirty = true;
            return std::make_pair(up.dists.data(), BitSpan<uint32>(stat_scratch_.data(), size_t(m)));
        }
    }
    bool has_stat(BitSpan<uint32>) const { return false; }
    void set_stat(BitSpan<uint32>) {}

    template <bool is_base_layer>
    void set_neighbor(T cur_layer_idx, int16 pruned, T newpoint_id, T newpoint_cur_layer_idx) {
        if (pruned < 0) return;
        if constexpr (is_base_layer) {
            if (size_t(pruned) < size_t(m) * 2) {
                char *rec = base_rec(cur_layer_idx, true);
                reinterpret_cast<T *>(rec)[pruned] = newpoint_id;
            }
        } else {
            if (size_t(pruned) < size_t(m)) {
                auto &up = upper_points[cur_layer_idx];
                up.neighbors_info[pruned] = newpoint_id;
                up.neighbors_info[size_t(m) + size_t(pruned)] = newpoint_cur_layer_idx;
                dirty_upper_segs_.insert(uint32(size_t(cur_layer_idx) / SEG_RECORDS));
                upper_dirty = true;
            }
        }
    }
    void set_base_neighbors(T id, const T *neighbors_id) {
        char *rec = base_rec(id, true);
        std::memcpy(rec, neighbors_id, size_t(m) * 2 * sizeof(T));
    }
    void set_upper_neighbors(T idx, const T *neighbors_info) {
        upper_points[idx].neighbors_info.assign(neighbors_info, neighbors_info + size_t(m) * 2);
        dirty_upper_segs_.insert(uint32(size_t(idx) / SEG_RECORDS));
        upper_dirty = true;
    }
    void add_basepoint(T id, const T *neighbors_id) {
        if (size_t(id) >= next_base_id_) next_base_id_ = size_t(id) + 1;
        set_base_neighbors(id, neighbors_id);
    }
    void add_upperpoint(T cur_layer_idx, T lower_layer_idx, T id, const T *neighbors_info) {
        if (size_t(cur_layer_idx) >= upper_points.size()) {
            upper_points.resize(size_t(cur_layer_idx) + 1, MakeUpperPoint());
            next_upper_id_ = upper_points.size();
        }
        auto &up = upper_points[cur_layer_idx];
        up.lower_layer_idx = lower_layer_idx;
        up.id = id;
        up.neighbors_info.assign(neighbors_info, neighbors_info + size_t(m) * 2);
        dirty_upper_segs_.insert(uint32(size_t(cur_layer_idx) / SEG_RECORDS));
        upper_dirty = true;
    }

    // ---- 杂项接口（对齐 MemStore） ----
    void *get_index() const { return nullptr; }
    DistPrecisionType get_precision() const { return static_cast<DistPrecisionType>(0); }
    uint16 get_dim() const { return uint16(dim); }
    uint32 get_vecsize() const { return uint32(vec_size); }
    uint32 get_elemsize() const { return uint32(vec_size); }
    size_t get_vector_num() const { return elems.size(); }
    template <bool is_base_layer>
    size_t max_id() const {
        return (is_base_layer ? next_base_id_ : next_upper_id_) + 16;
    }
    void reset_capacity(size_t base_n, size_t upper_n) {
        next_base_id_ = base_n;
        next_upper_id_ = upper_n;
        if (elems.size() < base_n) elems.resize(base_n);
        if (upper_points.size() < upper_n) upper_points.resize(upper_n, MakeUpperPoint());
    }
    void destroy() {}

    // ---- dirty 段写回（xSync 调用；写事务内才合法） ----
    template <typename WriteFn>
    void flush_dirty_segs(WriteFn &&write_fn) {
        // 写失败的段保留 dirty：清了就永不再 flush——失败 COMMIT 被应用重试
        // 时会带着新 meta/旧段提交，静默损坏。
        for (auto &kv : base_cache_.segs) {
            if (kv.second.dirty && write_fn(KIND_BASE, kv.first, kv.second.data)) {
                kv.second.dirty = false;
            }
        }
        for (auto &kv : vec_cache_.segs) {
            if (kv.second.dirty && write_fn(KIND_VEC, kv.first, kv.second.data)) {
                kv.second.dirty = false;
            }
        }
        for (auto &kv : quantizer_code_cache_.segs) {
            if (kv.second.dirty &&
                write_fn(KIND_QUANTIZER_CODES, kv.first, kv.second.data)) {
                kv.second.dirty = false;
            }
        }
    }
    bool has_dirty() const {
        if (upper_dirty || !dirty_elem_segs_.empty() || !dirty_upper_segs_.empty()) return true;
        for (auto &kv : base_cache_.segs)
            if (kv.second.dirty) return true;
        for (auto &kv : vec_cache_.segs)
            if (kv.second.dirty) return true;
        for (auto &kv : quantizer_code_cache_.segs)
            if (kv.second.dirty) return true;
        return false;
    }

private:
    struct Seg {
        std::vector<char> data;
        bool dirty = false;
        uint64 tick = 0;
    };
    struct SegCache {
        std::unordered_map<uint32, Seg> segs;
        size_t bytes = 0;
    };

    PageIO io_;
    size_t cache_budget_ = 0;
    uint_fast32_t quantizer_code_size_ = 0;
    SegCache base_cache_, vec_cache_, quantizer_code_cache_;
    uint64 tick_ = 0;
    std::vector<uint32> stat_scratch_;
    std::vector<std::vector<char>> val_pool_;
    // 直读单记录的暂存（per-kind 各一条：算法层在下一次同 kind 访问前必已
    // 消费完指针——search_layer 拷邻居/逐 id 即取即算的调用序保证）。
    std::vector<char> base_scratch_, vec_scratch_, quantizer_code_scratch_;

    char *pool_alloc(size_t n) {
        val_pool_.emplace_back(n);
        return val_pool_.back().data();
    }

    char *base_rec(T idx, bool mark_dirty) {
        return rec_ptr(base_cache_, KIND_BASE, idx, base_rec_bytes(), base_scratch_, mark_dirty);
    }
    char *vec_rec(T idx, bool mark_dirty) {
        return rec_ptr(vec_cache_, KIND_VEC, idx, size_t(vec_size), vec_scratch_, mark_dirty);
    }
    char *quantizer_code_rec(T idx, bool mark_dirty) {
        return rec_ptr(quantizer_code_cache_, KIND_QUANTIZER_CODES, idx,
                       size_t(quantizer_code_size_), quantizer_code_scratch_, mark_dirty);
    }

    size_t total_cache_bytes() const {
        return base_cache_.bytes + vec_cache_.bytes + quantizer_code_cache_.bytes;
    }

    // 记录访问入口。读 miss 的 admission＝缓存冻结策略：预算未满才整段进
    // 缓存（HNSW 首查从 entry 出发的访问序天然热度相关——先进缓存的就是
    // 入口邻域热区），满了之后一律直读单记录到 per-kind scratch——彻底
    // 消灭 thrash（段式 LRU 在"工作集>预算"的 HNSW 随机访问下每查询反复
    // 驱逐重读，实测 QPS 崩到个位数）。写 miss 必整段载入（dirty 是段粒度，
    // evict 时写回），不受冻结限制。
    char *rec_ptr(SegCache &c, int kind, T idx, size_t rec_size, std::vector<char> &scratch,
                  bool mark_dirty) {
        const uint32 seg = uint32(size_t(idx) / SEG_RECORDS);
        const size_t off = (size_t(idx) % SEG_RECORDS) * rec_size;
        auto it = c.segs.find(seg);
        if (it != c.segs.end()) {
            it->second.tick = ++tick_;
            if (mark_dirty) it->second.dirty = true;
            return it->second.data.data() + off;
        }
        const size_t want = seg_bytes(kind);
        const bool cache_full = total_cache_bytes() + want > cache_budget_;
        if (!mark_dirty && cache_full && io_.read_rec) {
            scratch.resize(rec_size);
            if (!io_.read_rec(kind, seg, off, rec_size, scratch.data())) {
                fill_invalid_rec(kind, scratch.data(), rec_size);  // 段不存在（构建中）
            }
            return scratch.data();
        }
        Seg &s = admit_seg(c, kind, seg, mark_dirty);
        return s.data.data() + off;
    }

    Seg &admit_seg(SegCache &c, int kind, uint32 seg, bool mark_dirty) {
        const size_t want = seg_bytes(kind);
        evict_if_needed(want);
        Seg s;
        const bool found = io_.read && io_.read(kind, seg, s.data);
        if (found && kind == KIND_QUANTIZER_CODES && !s.data.empty() &&
            s.data.size() <= want && s.data.size() % size_t(quantizer_code_size_) == 0) {
            // v2 兼容：历史格式的最后一个 code 段只保存实际记录，不补齐 64 条。
            // cache 内统一补零成定长段；一旦变脏，按新格式整段写回。
            s.data.resize(want, 0);
        } else if (!found || s.data.size() != want) {
            fill_invalid(kind, s.data, want);
        }
        auto it = c.segs.emplace(seg, std::move(s)).first;
        c.bytes += want;
        it->second.tick = ++tick_;
        if (mark_dirty) it->second.dirty = true;
        return it->second;
    }

    void fill_invalid_rec(int kind, char *rec, size_t rec_size) {
        if (kind == KIND_BASE) {
            VexFillInvalidBaseRec(rec, size_t(m));
        } else {
            std::memset(rec, 0, rec_size);
        }
    }

    void fill_invalid(int kind, std::vector<char> &data, size_t want) {
        data.assign(want, 0);
        if (kind == KIND_BASE) {
            const size_t rec = base_rec_bytes();
            for (size_t r = 0; r < SEG_RECORDS; r++) {
                fill_invalid_rec(kind, data.data() + r * rec, rec);
            }
        }
    }

    void evict_if_needed(size_t incoming) {
        if (total_cache_bytes() + incoming <= cache_budget_) return;
        // 一次扫描收集 (dirty, tick, cache, seg)，clean 段排前、再按 tick 升序，
        // 批量驱逐到预算 75%——dirty 段写回（UPSERT 整段）是构建期写放大的
        // 大头（反向边更新随机散布全图 base 段，驱逐后马上被重载再改 dirty），
        // clean 段驱逐零成本，优先丢弃；clean 不够时才动 dirty（buffer manager
        // 的脏页优先保留惯例）。批量化避免逐段驱逐每次全扫 map 的 O(n²)。
        std::vector<std::tuple<bool, uint64, SegCache *, uint32>> order;
        order.reserve(base_cache_.segs.size() + vec_cache_.segs.size() +
                      quantizer_code_cache_.segs.size());
        for (SegCache *c : {&base_cache_, &vec_cache_, &quantizer_code_cache_}) {
            uint64 newest = 0;
            for (auto &kv : c->segs) newest = std::max(newest, kv.second.tick);
            for (auto &kv : c->segs) {
                if (kv.second.tick == newest) continue;  // pin 最近段：保护
                // get_point_info 返回的段内指针在算法消费窗口内有效
                order.emplace_back(kv.second.dirty, kv.second.tick, c, kv.first);
            }
        }
        std::sort(order.begin(), order.end());
        const size_t target = cache_budget_ - std::min(cache_budget_, cache_budget_ / 4);
        for (const auto &[dirty, tick, c, seg] : order) {
            if (total_cache_bytes() + incoming <= target) break;
            auto vit = c->segs.find(seg);
            if (vit == c->segs.end()) continue;
            if (vit->second.dirty) {
                // dirty 段只在写事务内产生；只读打开不可能到达这里
                int kind = c == &base_cache_ ? KIND_BASE
                    : (c == &vec_cache_ ? KIND_VEC : KIND_QUANTIZER_CODES);
                if (!io_.write || !io_.write(kind, seg, vit->second.data)) {
                    throw std::runtime_error("vexdb-sqlite: dirty segment evict without write IO");
                }
            }
            c->bytes -= vit->second.data.size();
            c->segs.erase(vit);
        }
    }

    UpperPointRec MakeUpperPoint() const {
        return VexMakeUpperPoint<UpperPointRec, T>(size_t(m));
    }
};
