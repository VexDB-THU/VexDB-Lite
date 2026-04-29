#pragma once

#include "vex/vex_duck_point.hpp"
#include "vex/vex_duck_memstore.hpp"
#include "vex/vex_duckdb_compat.hpp"

#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
        size_t current_size = 0;
        size_t size() const {
            return current_size;
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

    MemStore() = default;
    MemStore(uint_fast16_t dim_in, uint_fast16_t m_in, uint_fast32_t vec_size_in)
        : dim(dim_in), m(m_in), vec_size(vec_size_in) {
        entry_info.set(INVALID_VECTOR_ID, INVALID_VECTOR_ID, -1);
    }

    template <bool exclusive = false, bool bottom_only = false>
    std::pair<GraphIndexEntryInfo, bool> get_entry(int_fast8_t = 0) {
        (void)exclusive;
        (void)bottom_only;
        return {entry_info, false};
    }
    void release_entry_lock(bool) {
    }

    template <bool is_base_layer>
    T assign_vector_id() {
        if constexpr (is_base_layer) {
            T id = T(elems.size());
            elems.emplace_back();
            vectors.emplace_back();
            base_points.push_back(MakeBasePoint());
            base_layer.current_size = base_points.size();
            return id;
        } else {
            T idx = T(upper_points.size());
            upper_points.push_back(MakeUpperPoint());
            upper_layer.current_size = upper_points.size();
            return idx;
        }
    }

    void add_async_id(T id) {
        async_ids.push_back(id);
    }

    void add_elem(PointExtensionContext &ctx, T id, const ItemPointerData &tid) {
        (void)ctx;
        if (id >= elems.size()) {
            elems.resize(id + 1);
        }
        elems[id].tids.push_back(tid);
    }

    void add_vector(T id, const char *query) {
        if (id >= vectors.size()) {
            vectors.resize(id + 1);
        }
        vectors[id].assign(query, query + vec_size);
    }

    void set_entrypoint(T id, T cur_layer_idx, int_fast8_t level) {
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

    char *get_data(T id) {
        return vectors[id].data();
    }
    const char *get_data(T id) const {
        return vectors[id].data();
    }

    void reset_neighbors_val_pool() {
    }

    template <typename Distancer, typename IdVec>
    void get_distance_batch(const Distancer &distancer, const char *query, const IdVec &ids, float *dists) {
        std::vector<void *> vals;
        vals.reserve(ids.size());
        for (auto id : ids) {
            vals.push_back(vectors[id].data());
        }
        distancer.get_distance_batch2(query, vals.data(), uint16(dim), uint16(vals.size()), dists);
    }

    template <typename Distancer>
    float get_distance(const Distancer &distancer, const char *query, T id) {
        return distancer.get_distance_single(query, vectors[id].data(), uint16(dim));
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

    template <bool is_base_layer, bool shared_lock>
    void lock_point(T) {
        (void)shared_lock;
    }
    template <bool is_base_layer, bool shared_lock>
    void unlock_point(T) {
        (void)shared_lock;
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
            out.reserve(bp.neighbors.size());
            for (size_t i = 0; i < bp.neighbors.size(); ++i) {
                auto id = bp.neighbors[i];
                if (id == T(INVALID_VECTOR_ID)) {
                    break;
                }
                out.emplace_back(id, id, bp.dists[i]);
            }
        } else {
            auto &up = upper_points[cand.cur_layer_idx];
            auto *neighbors_id = up.neighbors_info.data();
            auto *neighbors_cur_layer_idx = neighbors_id + m;
            out.reserve(m);
            for (size_t i = 0; i < size_t(m); ++i) {
                if (neighbors_id[i] == T(INVALID_VECTOR_ID)) {
                    break;
                }
                out.emplace_back(neighbors_id[i], neighbors_cur_layer_idx[i], up.dists[i]);
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
        } else {
            auto &up = upper_points[cur_layer_idx];
            if (size_t(pruned) < size_t(m)) {
                up.neighbors_info[pruned] = newpoint_id;
                up.neighbors_info[size_t(m) + size_t(pruned)] = newpoint_cur_layer_idx;
            }
        }
    }

    void set_base_neighbors(T id, const T *neighbors_id) {
        auto &bp = base_points[id];
        bp.neighbors.assign(neighbors_id, neighbors_id + m * 2);
    }
    void set_upper_neighbors(T idx, const T *neighbors_info) {
        auto &up = upper_points[idx];
        up.neighbors_info.assign(neighbors_info, neighbors_info + m * 2);
    }

    void add_basepoint(T id, const T *neighbors_id) {
        if (id >= base_points.size()) {
            base_points.resize(id + 1, MakeBasePoint());
        }
        auto &bp = base_points[id];
        bp.neighbors.assign(neighbors_id, neighbors_id + m * 2);
        base_layer.current_size = base_points.size();
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
        std::memcpy(dest, vectors[id].data(), vec_size);
        return true;
    }
    bool fetch_vec_from_heap(PointExtensionContext &, T id, char *dest) {
        if (id >= vectors.size()) {
            return false;
        }
        std::memcpy(dest, vectors[id].data(), vec_size);
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
