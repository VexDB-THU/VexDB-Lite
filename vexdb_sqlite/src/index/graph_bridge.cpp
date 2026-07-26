// GraphBridge 实现：GraphIndexAlgorithm × SQLite 单线程 MemStore。
//
// include 顺序约定（duck 同款）：宿主依赖头必须先于 algorithm.h。
#include "vex_graph_index_depend_sqlite.hpp"
#include "graph_index/graph_index_algorithm.h"

#include "distance/core/distance.h"
#include "distance/core/distance_dispatcher.h"
#include "distance/core/distance_utils_core.h"
#include "quantizer/annkmeans.h"
#include "quantizer/product_quantizer.h"
#include "rabitq/code_distancer.h"

#include "index/graph_bridge.h"

#include <cmath>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace vexdb_sqlite {

namespace {

using SqliteStore = MemStore<uint32, GraphIndexPoint>;
using SqliteDiskStore = DiskStore<uint32, GraphIndexPoint>;
using SqMetricList = MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::COSINE>;

template <typename T>
class VtlDestroyGuard {
public:
    explicit VtlDestroyGuard(T &value) : value_(value) {}
    VtlDestroyGuard(const VtlDestroyGuard &) = delete;
    VtlDestroyGuard &operator=(const VtlDestroyGuard &) = delete;
    ~VtlDestroyGuard() { ann_helper::optional_destroy(value_); }

private:
    T &value_;
};
using SqDTypeList = DistPrecisionTypeList<DistPrecisionType::FLOAT>;

Metric ToCoreMetric(VexMetric m) {
    switch (m) {
    case VexMetric::L2: return Metric::L2;
    case VexMetric::COSINE: return Metric::COSINE;
    case VexMetric::INNER_PRODUCT: return Metric::INNER_PRODUCT;
    }
    return Metric::L2;
}

Metric ToRaBitQMetric(VexMetric m) {
    return m == VexMetric::L2 ? Metric::L2 : Metric::INNER_PRODUCT;
}

template <typename Store>
class SqliteQuantizedSearchStore {
public:
    using T = typename Store::T;
    using point_type = typename Store::point_type;
    static constexpr bool use_dist_cache = false;

    SqliteQuantizedSearchStore(Store &store, const std::vector<uint8_t> &codes,
                               size_t code_size)
        : store_(store), codes_(codes), mutable_codes_(nullptr), code_size_(code_size) {}

    SqliteQuantizedSearchStore(Store &store, std::vector<uint8_t> &codes,
                               size_t code_size)
        : store_(store), codes_(codes), mutable_codes_(&codes), code_size_(code_size) {}

    template <bool exclusive = false, bool bottom_only = false>
    auto get_entry(int_fast8_t level = 0) {
        return store_.template get_entry<exclusive, bottom_only>(level);
    }
    void release_entry_lock(bool shared) { store_.release_entry_lock(shared); }

    template <bool base, bool shared>
    void lock_point(T id) { store_.template lock_point<base, shared>(id); }
    template <bool base, bool shared>
    void unlock_point(T id) { store_.template unlock_point<base, shared>(id); }
    template <bool base>
    auto get_point_info(T id) { return store_.template get_point_info<base>(id); }

    template <bool base>
    T assign_vector_id() { return store_.template assign_vector_id<base>(); }

    void add_async_id(T id) { store_.add_async_id(id); }
    template <typename Func>
    void for_each_async_id(Func &&func) {
        store_.for_each_async_id(std::forward<Func>(func));
    }

    void add_elem(PointExtensionContext &ctx, T id, const ItemPointerData &tid) {
        store_.add_elem(ctx, id, tid);
    }
    void add_elem(PointExtensionContext &ctx, T id, Span<const ItemPointerData> tids) {
        store_.add_elem(ctx, id, tids);
    }

    template <typename Distancer>
    void add_vector(Distancer &, T id, const char *code) {
        if constexpr (std::is_same_v<Store, SqliteDiskStore>) {
            store_.set_quantizer_code(id, reinterpret_cast<const uint8_t *>(code));
        } else {
            if (!mutable_codes_) {
                throw std::runtime_error("quantized search store is read-only");
            }
            const size_t offset = static_cast<size_t>(id) * code_size_;
            if (mutable_codes_->size() < offset + code_size_) {
                mutable_codes_->resize(offset + code_size_);
            }
            std::memcpy(mutable_codes_->data() + offset, code, code_size_);
        }
    }

    void set_entrypoint(T id, T cur_layer_idx, int_fast8_t level) {
        store_.set_entrypoint(id, cur_layer_idx, level);
    }

    void *get_index() const { return store_.get_index(); }
    DistPrecisionType get_precision() const { return store_.get_precision(); }
    uint16 get_dim() const { return store_.get_dim(); }
    uint32 get_vecsize() const { return static_cast<uint32>(code_size_); }
    uint32 get_elemsize() const { return static_cast<uint32>(code_size_); }
    size_t get_vector_num() const { return store_.get_vector_num(); }
    template <bool base>
    size_t max_id() const { return store_.template max_id<base>(); }

    char *get_data(T id) { return static_cast<char *>(CodeFor(id)); }
    const char *get_data(T id) const { return static_cast<const char *>(CodeFor(id)); }
    struct my_buf {
        const char *data;
        char *get_vecbuf() const { return const_cast<char *>(data); }
        static constexpr void release() {}
    };
    my_buf read_data(T id) { return my_buf{get_data(id)}; }
    void reset_neighbors_val_pool() { store_.reset_neighbors_val_pool(); }

    template <typename Distancer, typename IdVec>
    void get_distance_batch(const Distancer &distancer, const char *query,
                            const IdVec &ids, float *out) {
        if constexpr (std::is_same_v<Store, SqliteDiskStore>) {
            // DiskStore 的 cache miss 共用一条 code scratch；收集多个指针会让
            // 前面的指针被后一次读取覆盖。逐 id 即取即算，保证指针生命期。
            for (size_t i = 0; i < ids.size(); ++i) {
                out[i] = distancer.get_distance_single(query, CodeFor(ids[i]), 0);
            }
        } else {
            std::vector<void *> ptrs;
            ptrs.reserve(ids.size());
            for (auto id : ids) ptrs.push_back(CodeFor(id));
            distancer.get_distance_batch2(query, ptrs.data(), 0,
                                          static_cast<uint16_t>(ptrs.size()), out);
        }
    }
    template <typename Distancer>
    float get_distance(const Distancer &distancer, const char *query, T id) {
        return distancer.get_distance_single(query, CodeFor(id), 0);
    }
    template <typename Distancer>
    float get_distance_est(const Distancer &distancer, const char *query, T id) {
        return distancer.get_distance_est_single(query, CodeFor(id), 0);
    }
    template <typename Distancer>
    float get_distance(const Distancer &distancer, const char *query, const char *code) {
        return distancer.get_distance_single(query, code, 0);
    }
    template <typename Distancer>
    float get_distance_precise(const Distancer &distancer, const char *query, const char *code) {
        return get_distance(distancer, query, code);
    }

    template <bool base, typename CandVec, typename CandType>
    void get_neighbors(CandVec &out, const CandType &cand) {
        store_.template get_neighbors<base>(out, cand);
    }
    template <bool base>
    auto get_neighbor_stats(T id) { return store_.template get_neighbor_stats<base>(id); }
    template <typename Bits>
    bool has_stat(Bits bits) const { return store_.has_stat(bits); }
    template <typename Bits>
    void set_stat(Bits bits) { store_.set_stat(bits); }
    template <bool base>
    void set_neighbor(T idx, int16 pruned, T new_id, T new_upper_idx) {
        store_.template set_neighbor<base>(idx, pruned, new_id, new_upper_idx);
    }
    void set_base_neighbors(T id, const T *neighbors) { store_.set_base_neighbors(id, neighbors); }
    void set_upper_neighbors(T id, const T *neighbors) { store_.set_upper_neighbors(id, neighbors); }
    void add_basepoint(T id, const T *neighbors) { store_.add_basepoint(id, neighbors); }
    void add_upperpoint(T idx, T lower, T id, const T *neighbors) {
        store_.add_upperpoint(idx, lower, id, neighbors);
    }
    template <typename Func>
    bool apply_elem(T id, Func &&func) {
        return store_.apply_elem(id, std::forward<Func>(func));
    }
    template <typename Func>
    void get_itempointer(T id, Func &&func) {
        store_.get_itempointer(id, std::forward<Func>(func));
    }

private:
    void *CodeFor(T id) const {
        if constexpr (std::is_same_v<Store, SqliteDiskStore>) {
            if (store_.quantizer_code_size() != code_size_) {
                throw std::runtime_error("quantizer code size does not match disk cache");
            }
            return const_cast<char *>(store_.get_quantizer_code(id));
        }
        size_t offset = static_cast<size_t>(id) * code_size_;
        if (!code_size_ || offset + code_size_ > codes_.size()) {
            throw std::runtime_error("quantizer code coverage does not match graph nodes");
        }
        return const_cast<uint8_t *>(codes_.data() + offset);
    }

    Store &store_;
    const std::vector<uint8_t> &codes_;
    std::vector<uint8_t> *mutable_codes_;
    size_t code_size_;
};

struct SqlitePQState {
    ::vex::quantizer::ProductQuantizer quantizer;

    ~SqlitePQState() {
        if (quantizer.centroids != nullptr) {
            ::vex::quantizer::PQContext ctx;
            quantizer.free_resources(ctx);
        }
    }
};

class SqlitePQDistancer {
public:
    static constexpr bool has_estimation_func = false;
    // 图内只算 ADC；vtab 会扩大候选并按 rowid 从 %_vectors 读取业务原向量
    // 做精确重排。不能从 compact store 取 raw，因为那里按契约没有 kind=4。
    static constexpr bool need_refine = false;

    SqlitePQDistancer(::vex::quantizer::ProductQuantizer &quantizer, Metric metric)
        : quantizer_(quantizer),
          dist_table_(quantizer.M * quantizer.ksub),
          flag_(metric == Metric::INNER_PRODUCT ? -1.0f : 1.0f) {}

    void process(const float *query) {
        quantizer_.compute_distance_table(query, dist_table_.data());
    }

    float get_distance_single(const void *, const void *code, uint16) const {
        return quantizer_.distance_to_code(static_cast<const uint8_t *>(code),
                                           dist_table_.data()) * flag_;
    }

    float get_distance_est_single(const void *query, const void *code, uint16 dim) const {
        return get_distance_single(query, code, dim);
    }

    void get_distance_batch2(const void *, void *const *codes, uint16, uint16 count,
                             float *out) const {
        uint16 i = 0;
        for (; i + 4 <= count; i += 4) {
            quantizer_.distance_to_four_code(
                dist_table_.data(), static_cast<const uint8_t *>(codes[i]),
                static_cast<const uint8_t *>(codes[i + 1]),
                static_cast<const uint8_t *>(codes[i + 2]),
                static_cast<const uint8_t *>(codes[i + 3]),
                out[i], out[i + 1], out[i + 2], out[i + 3]);
            out[i] *= flag_;
            out[i + 1] *= flag_;
            out[i + 2] *= flag_;
            out[i + 3] *= flag_;
        }
        for (; i < count; i++) {
            out[i] = get_distance_single(nullptr, codes[i], 0);
        }
    }

private:
    ::vex::quantizer::ProductQuantizer &quantizer_;
    std::vector<float> dist_table_;
    float flag_;
};

// 照抄 duck RunWithDuckAlgo（graph_index.cpp:53）：dispatcher 选出 distancer，
// 就地实例化算法对象执行 fn。Store 参数化：MemStore（全内存）/ DiskStore（段式）。
template <typename Store, typename Fn>
auto RunWithAlgo(Metric metric, uint16_t dim, int ef_construction, int m, Store &store,
                 Fn &&fn) {
    return DispatchRunner<false, SqMetricList, SqDTypeList, DispatcherMode::NO_QUANT>::call(
        metric, DistPrecisionType::FLOAT, dim, QuantizerType::NONE,
        [&](auto &distancer) -> decltype(auto) {
            using DistT = std::decay_t<decltype(distancer)>;
            using AlgoT = GraphIndexAlgorithm<Store, DistT>;
            AlgoT algo(uint_fast16_t(ef_construction), uint_fast16_t(m), store, distancer);
            return fn(algo);
        });
}

// ---- 序列化格式 v4（段式，M9'：%_graph(kind, seg, data)；全部小端定长记录）----
// v1（全量镜像分块）已废弃：EnsureGraph 读不出 meta 段 → fall through 重建。
// v3 = elems 支持空壳节点（tid 摘除：DELETE/UPDATE 增量化）；
// v4 = elems/upper 也按 64 条记录分段，避免单行写重写 O(N) 元数据。
// 读侧接受 v2/v3；写侧统一 v4。
constexpr uint32_t kGraphBlobMagic = 0x47535856;  // 'VXSG'
constexpr uint32_t kGraphBlobVersion = 4;
constexpr uint32_t kGraphBlobMinVersion = 2;

struct BlobHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t dim;
    uint32_t m;
    uint32_t ef_construction;
    uint32_t metric;
    uint64_t base_count;
    uint64_t upper_count;
    uint64_t entry_id;
    uint64_t entry_cur_layer_idx;
    int32_t entry_level;
    uint32_t seg_records;  // v2：base/vec 每段记录数（写入时固定，读侧以此为准）
};

template <typename T>
void AppendPod(std::vector<char> &out, const T &v) {
    const char *p = reinterpret_cast<const char *>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

template <typename T>
bool ReadPod(const char *&p, const char *end, T &v) {
    if (p + sizeof(T) > end) return false;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return true;
}

}  // namespace

struct GraphBridge::Impl {
    uint16_t dim;
    int m;
    int ef_construction;
    VexMetric metric;
    Metric core_metric;
    SqliteStore store;                          // 全内存模式（disk 为空时生效）
    std::unique_ptr<SqliteDiskStore> disk;      // DiskStore 模式（非空=段式懒加载）
    // tid 摘除（DELETE/UPDATE 增量化）：rowid→节点 id 反查 map 懒构建（首次
    // RemoveTid 时从 elems 扫描，之后随 Insert/RemoveTid 维护）；dead_nodes=
    // tids 摘空的空壳节点数（占 ef 槽位，Search 补偿+阈值重建判据）。
    std::unordered_map<int64_t, uint32_t> rowid_to_node;
    bool rowid_map_built = false;
    size_t dead_nodes = 0;
    bool elems_dirty = false;  // tid 摘除待落盘（disk 模式 HasDirty 用）
    // true=段与持久化不一致，需全量写元数据段。mem 重建时调用方会先清旧
    // 段；从 v2/v3 打开的图只需把旧的单 BLOB elems/upper 迁移成 v4 分段，
    // base/vec/code 的既有段格式不变。
    bool full_dirty = false;
    QuantizerType quantizer_type = QuantizerType::NONE;
    bool quantizer_use = false;
    uint32_t pq_m = 0;
    bool compact_mode = false;
    std::unique_ptr<SqlitePQState> pq_state;
    std::unique_ptr<::rabitq::RaBitQuantizer> rabitq_quantizer;
    double rabitq_query_rescaling_factor = 0.0;
    std::vector<uint8_t> quantizer_codes;
    bool quantizer_fixed_dirty = false;
    std::unordered_set<size_t> quantizer_dirty_code_segs;

    bool uses_pq() const {
        return quantizer_use && quantizer_type == QuantizerType::PQ;
    }

    bool uses_rabitq() const {
        return quantizer_use && quantizer_type == QuantizerType::RABITQ;
    }

    size_t quantizer_code_size() const {
        if (quantizer_type == QuantizerType::PQ) {
            return pq_state ? pq_state->quantizer.code_size : size_t(pq_m);
        }
        if (quantizer_type == QuantizerType::RABITQ) {
            return ::rabitq::CodeSize(dim);
        }
        return 0;
    }

    template <typename Store>
    void release_raw_vectors(Store &active_store) {
        if (!compact_mode) return;
        active_store.compact_mode_ = true;
        if constexpr (std::is_same_v<Store, SqliteStore>) {
            active_store.vectors.clear();
            active_store.vectors.shrink_to_fit();
            active_store.dirty_vec_segs_.clear();
        }
    }

    template <typename Store>
    void train_rabitq(Store &active_store) {
        const size_t node_count = active_store.elems.size();
        if (quantizer_type != QuantizerType::RABITQ || node_count == 0) return;

        const int padded = RABITQ_PADDED_DIM(dim);
        auto quantizer = std::make_unique<::rabitq::RaBitQuantizer>(
            dim, padded, ToRaBitQMetric(metric));
        constexpr size_t kMaxTrainingBytes = 64ULL * 1024 * 1024;
        constexpr size_t kMaxTrainingSamples = 65536;
        const size_t samples_by_bytes = std::max<size_t>(
            HNSW_RABITQ_NUM_CLUSTERS,
            kMaxTrainingBytes / std::max<size_t>(size_t(dim) * sizeof(float), 1));
        const size_t sample_count = std::min(
            node_count, std::min(kMaxTrainingSamples, samples_by_bytes));
        std::vector<float> samples(sample_count * dim);
        for (size_t sample_id = 0; sample_id < sample_count; sample_id++) {
            const size_t node_id = sample_id * node_count / sample_count;
            auto vec_buf = active_store.read_data(node_id);
            const auto *src = reinterpret_cast<const float *>(vec_buf.get_vecbuf());
            if (!src) throw std::runtime_error("RaBitQ training found a node without a vector");
            std::memcpy(samples.data() + sample_id * dim, src, size_t(dim) * sizeof(float));
            vec_buf.release();
        }

        ::vex::quantizer::KMeansState state;
        if (metric == VexMetric::L2) {
            state.distance_fn = ann_helper::get_general_distance_func(Metric::L2_SQRT, dim);
        } else {
            state.distance_fn = ann_helper::get_general_distance_func(Metric::SPHERICAL, dim);
            state.norm_fn = ann_helper::get_general_distance_func(Metric::L2_NORM, dim);
        }

        ::vex::quantizer::PQFloatArray sample_view;
        sample_view.data = samples.data();
        sample_view.length = sample_count;
        sample_view.maxlen = sample_count;
        sample_view.dim = dim;
        ::vex::quantizer::PQFloatArray centers;
        centers.data = quantizer->get_centroids();
        centers.length = 0;
        centers.maxlen = HNSW_RABITQ_NUM_CLUSTERS;
        centers.dim = dim;
        ::vex::quantizer::PQContext ctx;
        ::vex::quantizer::AnnKmeans(state, sample_view, centers,
                                    /*avg_work_mem_kb=*/128 * 1024, ctx);
        quantizer->train();

        rabitq_query_rescaling_factor = quantizer->get_query_rescaling_factor();
        quantizer->set_rescaling_factor(rabitq_query_rescaling_factor);
        const size_t code_size = ::rabitq::CodeSize(dim);
        if constexpr (std::is_same_v<Store, SqliteDiskStore>) {
            std::vector<uint8_t> code(code_size);
            for (size_t id = 0; id < node_count; id++) {
                auto vec_buf = active_store.read_data(id);
                const auto *src = reinterpret_cast<const float *>(vec_buf.get_vecbuf());
                ::rabitq::EncodeCode(*quantizer, dim, src, code.data());
                vec_buf.release();
                active_store.set_quantizer_code(static_cast<typename Store::T>(id), code.data());
            }
            quantizer_codes.clear();
        } else {
            quantizer_codes.assign(node_count * code_size, 0);
            for (size_t id = 0; id < node_count; id++) {
                const auto *src = reinterpret_cast<const float *>(active_store.get_data(id));
                ::rabitq::EncodeCode(*quantizer, dim, src,
                                     quantizer_codes.data() + id * code_size);
            }
        }
        rabitq_quantizer = std::move(quantizer);
        quantizer_use = true;
        quantizer_fixed_dirty = true;
        if constexpr (!std::is_same_v<Store, SqliteDiskStore>) {
            const size_t n_segs =
                (node_count + SqliteDiskStore::SEG_RECORDS - 1) / SqliteDiskStore::SEG_RECORDS;
            for (size_t seg = 0; seg < n_segs; seg++) {
                quantizer_dirty_code_segs.insert(seg);
            }
        }
    }

    template <typename Store>
    void train_pq(Store &active_store) {
        const size_t node_count = active_store.elems.size();
        if (quantizer_type != QuantizerType::PQ || node_count == 0) return;
        if (pq_m == 0 || size_t(dim) % size_t(pq_m) != 0) {
            throw std::runtime_error("PQ subquantizer count must divide the vector dimension");
        }

        constexpr size_t kMaxTrainingBytes = 64ULL * 1024 * 1024;
        constexpr size_t kMaxTrainingSamples = 65536;
        const size_t samples_by_bytes = std::max<size_t>(
            1, kMaxTrainingBytes / std::max<size_t>(size_t(dim) * sizeof(float), 1));
        const size_t sample_count = std::min(
            node_count, std::min(kMaxTrainingSamples, samples_by_bytes));
        std::vector<float> samples(sample_count * dim);
        for (size_t sample_id = 0; sample_id < sample_count; sample_id++) {
            const size_t node_id = sample_id * node_count / sample_count;
            auto vec_buf = active_store.read_data(node_id);
            const auto *src = reinterpret_cast<const float *>(vec_buf.get_vecbuf());
            if (!src) throw std::runtime_error("PQ training found a node without a vector");
            std::memcpy(samples.data() + sample_id * dim, src, size_t(dim) * sizeof(float));
            vec_buf.release();
        }

        auto state = std::make_unique<SqlitePQState>();
        auto &pq = state->quantizer;
        ::vex::quantizer::PQContext ctx;
        pq.set_basic_values(dim, pq_m, 8);
        pq.set_derived_values(ctx);
        pq.set_fvec_L2sqr_ny_nearest_func();
        pq.set_fvec_ny_distance_func(
            metric == VexMetric::INNER_PRODUCT ? Metric::INNER_PRODUCT : Metric::L2);
        pq.set_dist_code_func();

        ::vex::quantizer::KMeansState kmeans_state;
        kmeans_state.distance_fn = ann_helper::get_general_distance_func(
            metric == VexMetric::INNER_PRODUCT ? Metric::INNER_PRODUCT : Metric::L2_SQRT,
            uint32_t(pq.dsub));
        kmeans_state.skip_check_duplicate = true;
        ::vex::quantizer::PQFloatArray sample_view;
        sample_view.data = samples.data();
        sample_view.length = sample_count;
        sample_view.maxlen = sample_count;
        sample_view.dim = dim;
        pq.train(kmeans_state, sample_view, /*avg_work_mem_kb=*/128 * 1024, ctx);

        const size_t code_size = pq.code_size;
        if constexpr (std::is_same_v<Store, SqliteDiskStore>) {
            std::vector<uint8_t> code(code_size);
            for (size_t id = 0; id < node_count; id++) {
                auto vec_buf = active_store.read_data(id);
                const auto *src = reinterpret_cast<const float *>(vec_buf.get_vecbuf());
                pq.compute_code(src, code.data());
                vec_buf.release();
                active_store.set_quantizer_code(static_cast<typename Store::T>(id), code.data());
            }
            quantizer_codes.clear();
        } else {
            quantizer_codes.assign(node_count * code_size, 0);
            for (size_t id = 0; id < node_count; id++) {
                const auto *src = reinterpret_cast<const float *>(active_store.get_data(id));
                pq.compute_code(src, quantizer_codes.data() + id * code_size);
            }
        }
        pq_state = std::move(state);
        quantizer_use = true;
        quantizer_fixed_dirty = true;
        if constexpr (!std::is_same_v<Store, SqliteDiskStore>) {
            const size_t n_segs =
                (node_count + SqliteDiskStore::SEG_RECORDS - 1) / SqliteDiskStore::SEG_RECORDS;
            for (size_t seg = 0; seg < n_segs; seg++) {
                quantizer_dirty_code_segs.insert(seg);
            }
        }
    }

    template <typename Store>
    void train_quantizer(Store &active_store) {
        if (quantizer_type == QuantizerType::PQ) {
            train_pq(active_store);
        } else if (quantizer_type == QuantizerType::RABITQ) {
            train_rabitq(active_store);
        }
    }

    template <typename Store>
    void append_rabitq_code(Store &active_store, size_t old_node_count) {
        if (!uses_rabitq() || active_store.elems.size() == old_node_count) return;
        if (active_store.elems.size() != old_node_count + 1) {
            throw std::runtime_error("RaBitQ incremental node allocation is not contiguous");
        }
        auto vec_buf = active_store.read_data(old_node_count);
        const auto *vec = reinterpret_cast<const float *>(vec_buf.get_vecbuf());
        const size_t code_size = quantizer_code_size();
        if constexpr (std::is_same_v<Store, SqliteDiskStore>) {
            std::vector<uint8_t> code(code_size);
            ::rabitq::EncodeCode(*rabitq_quantizer, dim, vec, code.data());
            vec_buf.release();
            active_store.set_quantizer_code(static_cast<typename Store::T>(old_node_count), code.data());
        } else {
            const size_t offset = quantizer_codes.size();
            quantizer_codes.resize(offset + code_size);
            ::rabitq::EncodeCode(*rabitq_quantizer, dim, vec,
                                 quantizer_codes.data() + offset);
            vec_buf.release();
            quantizer_dirty_code_segs.insert(old_node_count / SqliteDiskStore::SEG_RECORDS);
        }
    }

    template <typename Store>
    void append_pq_code(Store &active_store, size_t old_node_count) {
        if (!uses_pq() || active_store.elems.size() == old_node_count) return;
        if (active_store.elems.size() != old_node_count + 1) {
            throw std::runtime_error("PQ incremental node allocation is not contiguous");
        }
        auto vec_buf = active_store.read_data(old_node_count);
        const auto *vec = reinterpret_cast<const float *>(vec_buf.get_vecbuf());
        const size_t code_size = quantizer_code_size();
        if constexpr (std::is_same_v<Store, SqliteDiskStore>) {
            std::vector<uint8_t> code(code_size);
            pq_state->quantizer.compute_code(vec, code.data());
            vec_buf.release();
            active_store.set_quantizer_code(static_cast<typename Store::T>(old_node_count),
                                            code.data());
        } else {
            const size_t offset = quantizer_codes.size();
            quantizer_codes.resize(offset + code_size);
            pq_state->quantizer.compute_code(vec, quantizer_codes.data() + offset);
            vec_buf.release();
            quantizer_dirty_code_segs.insert(old_node_count / SqliteDiskStore::SEG_RECORDS);
        }
    }

    template <typename Store>
    void append_quantizer_code(Store &active_store, size_t old_node_count) {
        if (uses_pq()) {
            append_pq_code(active_store, old_node_count);
        } else if (uses_rabitq()) {
            append_rabitq_code(active_store, old_node_count);
        }
    }

    template <typename ElemsVec>
    void build_rowid_map(const ElemsVec &elems) {
        rowid_to_node.clear();
        rowid_to_node.reserve(elems.size() * 2);
        dead_nodes = 0;
        for (size_t i = 0; i < elems.size(); i++) {
            if (elems[i].tids.empty()) {
                dead_nodes++;
                continue;
            }
            for (const auto &t : elems[i].tids) rowid_to_node[t.row_id] = uint32_t(i);
        }
        rowid_map_built = true;
    }

    Impl(uint16_t dim_in, int m_in, int efc_in, VexMetric metric_in,
         QuantizerType quantizer_type_in, uint32_t pq_m_in, bool compact_mode_in)
        : dim(dim_in), m(m_in), ef_construction(efc_in), metric(metric_in),
          core_metric(ToCoreMetric(metric_in)),
          store(dim_in, uint_fast16_t(m_in), uint_fast32_t(dim_in) * sizeof(float)),
          quantizer_type(quantizer_type_in), pq_m(pq_m_in), compact_mode(compact_mode_in) {
        store.normalize_vectors_ = (metric == VexMetric::COSINE);
    }
};

GraphBridge::GraphBridge(uint16_t dim, int m, int ef_construction, VexMetric metric,
                         QuantizerType quantizer, uint32_t pq_m, bool compact_mode)
    : impl_(new Impl(dim, m, ef_construction, metric, quantizer, pq_m, compact_mode)) {}

GraphBridge::~GraphBridge() = default;

void GraphBridge::Insert(const float *vec, int64_t rowid) {
    auto &im = *impl_;
    const size_t before = im.disk ? im.disk->elems.size() : im.store.elems.size();
    auto run = [&](auto &store) {
        if (im.compact_mode && im.quantizer_use) {
            const float *encode_src = vec;
            std::vector<float> normalized;
            if (im.metric == VexMetric::COSINE) {
                normalized.resize(im.dim);
                VexNormalizeVec(normalized.data(), vec, im.dim);
                encode_src = normalized.data();
            }
            const size_t code_size = im.quantizer_code_size();
            std::vector<uint8_t> pending_code(code_size);
            auto insert_code = [&](auto &distancer) {
                SqliteQuantizedSearchStore<std::decay_t<decltype(store)>> code_store(
                    store, im.quantizer_codes, code_size);
                GraphIndexAlgorithm<decltype(code_store), std::decay_t<decltype(distancer)>> algo(
                    uint_fast16_t(im.ef_construction), uint_fast16_t(im.m),
                    code_store, distancer);
                PointExtensionContext pctx;
                ItemPointerData tid{};
                tid.row_id = rowid;
                typename decltype(algo)::InsertContextBase ctx(
                    pctx, reinterpret_cast<const char *>(pending_code.data()), &tid);
                algo.insert(ctx);
            };
            if (im.uses_pq()) {
                im.pq_state->quantizer.compute_code(encode_src, pending_code.data());
                SqlitePQDistancer distancer(im.pq_state->quantizer, im.core_metric);
                distancer.process(encode_src);
                insert_code(distancer);
            } else if (im.uses_rabitq() && im.rabitq_quantizer) {
                ::rabitq::EncodeCode(*im.rabitq_quantizer, im.dim, encode_src,
                                     pending_code.data());
                ::rabitq::CodeDistancer distancer(*im.rabitq_quantizer, im.dim,
                                                  ToRaBitQMetric(im.metric),
                                                  im.rabitq_query_rescaling_factor);
                distancer.process(encode_src);
                insert_code(distancer);
            } else {
                throw std::runtime_error("compact quantizer state is unavailable");
            }
            return;
        }
        RunWithAlgo(im.core_metric, im.dim, im.ef_construction, im.m, store, [&](auto &algo) {
            using AlgoT = std::decay_t<decltype(algo)>;
            PointExtensionContext pctx;
            ItemPointerData tid{};
            tid.row_id = rowid;
            typename AlgoT::InsertContextBase ctx(pctx, reinterpret_cast<const char *>(vec), &tid);
            algo.insert(ctx);
            return 0;
        });
        if (im.quantizer_use) {
            im.append_quantizer_code(store, before);
        } else if (im.quantizer_type != QuantizerType::NONE && !store.elems.empty()) {
            im.train_quantizer(store);
            im.release_raw_vectors(store);
        }
    };
    if (im.disk) {
        run(*im.disk);
    } else {
        run(im.store);
    }
    // rowid→node 懒 map 增量维护：节点数 +1 = 新节点（assign_vector_id 顺序
    // 分配，必在尾部）→ 追加一条；节点数不变 = dedup 把 tid 挂到既有节点
    //（节点 id 不可知）→ 才保守作废。原先无条件作废让交替 INSERT/DELETE
    // 工作负载每次删除付 O(N) 全 elems 重扫。
    if (im.rowid_map_built) {
        const size_t after = im.disk ? im.disk->elems.size() : im.store.elems.size();
        if (after == before + 1) {
            im.rowid_to_node[rowid] = uint32_t(after - 1);
        } else {
            im.rowid_map_built = false;
        }
    }
}

// 批量建图（duck BuildBulk 三段式）：首点串行立 entry → 单线程回退 → 连续
// 切片 spawn std::thread。worker 是纯计算（只碰 store/算法），异常聚合后抛。
// 仅全内存模式（DiskStore 模式的两阶段构建见 M9'c）。
void GraphBridge::BuildBulk(const float *vecs, const int64_t *rowids, size_t n, int n_threads) {
    auto &im = *impl_;
    if (n == 0) return;
    if (im.disk) {
        // 仅全内存模式（disk 模式两阶段构建经 vtab 组装）。误调用会建进
        // 空置的 mem store 而 Search/Count 读 disk store——静默数据丢失。
        throw std::logic_error("BuildBulk is mem-mode only (graph opened via OpenV2Disk)");
    }
    // 构建期关闭段级 dirty 追踪（8 线程标记会 race 且无意义——全新图首次
    // 落盘必然全量）；构建后标 full_dirty 走全量重写协议。异常路径（worker
    // rethrow）同样必须恢复追踪并标 full——RAII 确保。
    im.store.track_dirty_ = false;
    struct RestoreGuard {
        Impl &im;
        ~RestoreGuard() {
            im.store.track_dirty_ = true;
            im.store.dirty_elem_segs_.clear();
            im.store.dirty_upper_segs_.clear();
            im.store.dirty_base_segs_.clear();
            im.store.dirty_vec_segs_.clear();
            im.full_dirty = true;
            im.rowid_map_built = false;
        }
    } guard{im};

    RunWithAlgo(im.core_metric, im.dim, im.ef_construction, im.m, im.store, [&](auto &algo) {
        using AlgoT = std::decay_t<decltype(algo)>;
        auto insert_one = [&](size_t i) {
            PointExtensionContext pctx;
            ItemPointerData tid{};
            tid.row_id = rowids[i];
            typename AlgoT::InsertContextBase ctx(
                pctx, reinterpret_cast<const char *>(vecs + i * im.dim), &tid);
            algo.insert(ctx);
        };

        // Phase A：首点串行（建 entry，避免空图竞争升级协议）
        insert_one(0);
        if (n == 1) return 0;

        size_t rest = n - 1;
        int workers = n_threads;
        if (workers > int(rest)) workers = int(rest);
        // Phase B：单线程回退
        if (workers <= 1) {
            for (size_t i = 1; i < n; i++) insert_one(i);
            return 0;
        }

        // Phase C：并行。预留容量（内层向量 buffer 预 resize=publish 安全的根基；
        // upper 预留按 1/(m-1) 几何级数的期望上界放大）。
        im.store.ReserveCapacity(n, n / size_t(im.m > 2 ? im.m - 1 : 1) + 64);
        im.store.parallel_build_active_.store(true, std::memory_order_release);

        std::atomic<size_t> next{1};
        std::atomic<bool> failed{false};
        std::exception_ptr first_error = nullptr;
        std::mutex err_mu;
        auto worker = [&]() {
            constexpr size_t kBatch = 64;
            while (!failed.load(std::memory_order_relaxed)) {
                size_t begin = next.fetch_add(kBatch, std::memory_order_relaxed);
                if (begin >= n) break;
                size_t end = begin + kBatch < n ? begin + kBatch : n;
                try {
                    for (size_t i = begin; i < end; i++) insert_one(i);
                } catch (...) {
                    std::lock_guard<std::mutex> g(err_mu);
                    if (!first_error) first_error = std::current_exception();
                    failed.store(true, std::memory_order_relaxed);
                    break;
                }
            }
        };
        std::vector<std::thread> threads;
        threads.reserve(size_t(workers));
        for (int t = 0; t < workers; t++) threads.emplace_back(worker);
        for (auto &th : threads) th.join();
        im.store.parallel_build_active_.store(false, std::memory_order_release);
        if (first_error) std::rethrow_exception(first_error);
        return 0;
    });
    im.train_quantizer(im.store);
    im.release_raw_vectors(im.store);
}

void GraphBridge::Search(const float *query, size_t k, uint32_t ef_search,
                         std::vector<std::pair<double, int64_t>> &out) {
    Search(query, k, ef_search, nullptr, out);
}

void GraphBridge::Search(const float *query, size_t k, uint32_t ef_search,
                         const std::function<bool(int64_t)> &filter,
                         std::vector<std::pair<double, int64_t>> &out) {
    auto &im = *impl_;
    out.clear();
    if (Count() == 0 || k == 0) return;

    // cosine：归一化 query 副本，使 raw=-cos_sim、1+raw 与 M2 暴力路径数值一致
    //（不只排序一致）。L2/IP 直接用原始 query。
    const float *q = query;
    std::vector<float> normalized;
    if (im.metric == VexMetric::COSINE) {
        normalized.resize(im.dim);
        VexNormalizeVec(normalized.data(), query, im.dim);
        q = normalized.data();
    }

    uint32_t ef = std::max<uint32_t>(uint32_t(k), ef_search);
    // 空壳（tid 摘空）节点占据遍历与结果槽位但产不出行，按存活比放大 ef
    //（上限 2×）。空壳节点的零输出由 tids 展开天然保证，无需显式过滤。
    if (im.dead_nodes > 0) {
        size_t n = Count();
        size_t live = n > im.dead_nodes ? n - im.dead_nodes : 1;
        ef = uint32_t(std::min<double>(double(ef) * double(n) / double(live), double(ef) * 2.0));
    }
    auto run = [&](auto &store) {
        PointExtensionContext pctx;
        auto node_filter = [&](uint32 id) -> bool {
            for (const auto &t : store.elems[id].tids) {
                if (filter(t.row_id)) return true;
            }
            return false;
        };
        auto emit = [&](const auto &res) {
            for (size_t i = 0; i < res.size() && out.size() < k; i++) {
                if (filter && !filter(res[i].tid.row_id)) continue;
                double d = res[i].dist;
                switch (im.metric) {
                case VexMetric::L2: d = std::sqrt(std::max(0.0, d)); break;
                case VexMetric::COSINE:
                    // PQ 在单位向量上返回 squared-L2，即 2*(1-cos)。
                    // RaBitQ/原图返回负内积，沿用 1+raw。
                    d = im.uses_pq() ? std::max(0.0, d * 0.5) : 1.0 + d;
                    break;
                case VexMetric::INNER_PRODUCT: break;
                }
                out.emplace_back(d, res[i].tid.row_id);
            }
        };
        auto search = [&](auto &algo) {
            if (filter) {
                auto results = algo.search(pctx, reinterpret_cast<const char *>(q), ef, node_filter);
                VtlDestroyGuard results_guard(results);
                emit(results);
            } else {
                auto results = algo.search(pctx, reinterpret_cast<const char *>(q), ef);
                VtlDestroyGuard results_guard(results);
                emit(results);
            }
        };

        if (im.quantizer_use) {
            const size_t code_size = im.quantizer_code_size();
            const bool code_coverage_ok = [&]() {
                if constexpr (std::is_same_v<std::decay_t<decltype(store)>, SqliteDiskStore>) {
                    return store.quantizer_code_size() == code_size;
                }
                return im.quantizer_codes.size() == store.elems.size() * code_size;
            }();
            if (!code_coverage_ok) {
                throw std::runtime_error("quantizer code coverage does not match graph nodes");
            }
            auto search_codes = [&](auto &distancer) {
                SqliteQuantizedSearchStore<std::decay_t<decltype(store)>> quantized_store(
                    store, im.quantizer_codes, code_size);
                GraphIndexAlgorithm<decltype(quantized_store), std::decay_t<decltype(distancer)>> algo(
                    uint_fast16_t(im.ef_construction), uint_fast16_t(im.m),
                    quantized_store, distancer);
                search(algo);
            };
            if (im.uses_pq() && im.pq_state) {
                SqlitePQDistancer distancer(im.pq_state->quantizer, im.core_metric);
                distancer.process(q);
                search_codes(distancer);
            } else if (im.uses_rabitq() && im.rabitq_quantizer) {
                ::rabitq::CodeDistancer distancer(*im.rabitq_quantizer, im.dim,
                                                  ToRaBitQMetric(im.metric),
                                                  im.rabitq_query_rescaling_factor);
                distancer.process(q);
                search_codes(distancer);
            } else {
                throw std::runtime_error("quantizer state is unavailable");
            }
        } else {
            RunWithAlgo(im.core_metric, im.dim, im.ef_construction, im.m, store, [&](auto &algo) {
                search(algo);
                return 0;
            });
        }
    };
    if (im.disk) {
        run(*im.disk);
    } else {
        run(im.store);
    }
}

bool GraphBridge::RemoveTid(int64_t rowid) {
    auto &im = *impl_;
    auto with_store = [&](auto &store) -> bool {
        auto &elems = store.elems;
        if (!im.rowid_map_built) im.build_rowid_map(elems);
        auto it = im.rowid_to_node.find(rowid);
        if (it == im.rowid_to_node.end()) return false;
        const auto node_id = it->second;
        auto &tids = elems[node_id].tids;
        for (size_t i = 0; i < tids.size(); i++) {
            if (tids[i].row_id == rowid) {
                tids.erase(tids.begin() + i);
                break;
            }
        }
        if (tids.empty()) im.dead_nodes++;
        store.mark_elem_dirty(node_id);
        im.rowid_to_node.erase(it);
        im.elems_dirty = true;
        return true;
    };
    return im.disk ? with_store(*im.disk) : with_store(im.store);
}

size_t GraphBridge::DeadNodeCount() const {
    return impl_->dead_nodes;
}

size_t GraphBridge::Count() const {
    return impl_->disk ? impl_->disk->get_vector_num() : impl_->store.get_vector_num();
}

QuantizerType GraphBridge::Quantizer() const {
    return impl_->quantizer_use ? impl_->quantizer_type : QuantizerType::NONE;
}

bool GraphBridge::UsesPQ() const {
    return impl_->uses_pq();
}

bool GraphBridge::UsesRaBitQ() const {
    return impl_->uses_rabitq();
}

bool GraphBridge::IsCompactMode() const {
    return impl_->compact_mode;
}

bool GraphBridge::IsDiskMode() const {
    return impl_->disk != nullptr;
}

size_t GraphBridge::CacheBytesUsed() const {
    return impl_->disk ? impl_->disk->cache_bytes_used() : 0;
}

size_t GraphBridge::CacheBudgetBytes() const {
    return impl_->disk ? impl_->disk->cache_budget_bytes() : 0;
}

bool GraphBridge::HasDirty() const {
    return impl_->full_dirty || impl_->elems_dirty || impl_->quantizer_fixed_dirty ||
           !impl_->quantizer_dirty_code_segs.empty() ||
           (impl_->disk && impl_->disk->has_dirty());
}

namespace {

// ---- v2 段式序列化辅助（MemStore 全量 / DiskStore 增量共用） ----

constexpr int kKindMeta = SqliteDiskStore::KIND_META;
constexpr int kKindElems = SqliteDiskStore::KIND_ELEMS;
constexpr int kKindUpper = SqliteDiskStore::KIND_UPPER;
constexpr int kKindBase = SqliteDiskStore::KIND_BASE;
constexpr int kKindVec = SqliteDiskStore::KIND_VEC;
constexpr int kKindQuantizerFixed = 5;
constexpr int kKindQuantizerCodes = SqliteDiskStore::KIND_QUANTIZER_CODES;
constexpr size_t kSegRecords = SqliteDiskStore::SEG_RECORDS;
constexpr uint32_t kRaBitQBlobMagic = 0x51524256; // 'VBRQ'
constexpr uint32_t kRaBitQBlobVersion = 2;
constexpr uint32_t kPQBlobMagic = 0x51504256; // 'VBPQ'
constexpr uint32_t kPQBlobVersion = 1;

struct RaBitQBlobHeader {
    uint32_t magic;
    uint32_t version;
    double query_rescaling_factor;
    uint64_t random_bytes;
    uint64_t centroid_bytes;
    uint64_t rotated_bytes;
};

struct PQBlobHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t dim;
    uint32_t m;
    uint32_t nbits;
    uint32_t reserved;
    uint64_t centroid_bytes;
};

template <typename ImplT>
bool ReadRaBitQ(const GraphBridge::SegReadFn &read, ImplT &im,
                size_t node_count, std::string &err) {
    if (im.quantizer_type != QuantizerType::RABITQ) return true;
    // Empty RaBitQ indexes have no trained quantizer yet, so there is no fixed
    // data or code segment to persist. The first insert trains it.
    if (node_count == 0) {
        im.quantizer_use = false;
        return true;
    }
    std::vector<char> fixed;
    if (!read(kKindQuantizerFixed, 0, fixed)) {
        err = "RaBitQ persistence segments are missing";
        return false;
    }
    const char *p = fixed.data();
    const char *end = p + fixed.size();
    RaBitQBlobHeader header{};
    if (!ReadPod(p, end, header) || header.magic != kRaBitQBlobMagic ||
        header.version != kRaBitQBlobVersion) {
        err = "RaBitQ fixed data has an unsupported format";
        return false;
    }

    const int padded = RABITQ_PADDED_DIM(im.dim);
    auto quantizer = std::make_unique<::rabitq::RaBitQuantizer>(
        im.dim, padded, ToRaBitQMetric(im.metric));
    const size_t random_bytes = quantizer->get_random_matrix_size();
    const size_t centroid_bytes = HNSW_RABITQ_NUM_CLUSTERS * size_t(im.dim) * sizeof(float);
    const size_t rotated_bytes = HNSW_RABITQ_NUM_CLUSTERS * size_t(padded) * sizeof(float);
    if (header.random_bytes != random_bytes || header.centroid_bytes != centroid_bytes ||
        header.rotated_bytes != rotated_bytes ||
        p + random_bytes + centroid_bytes + rotated_bytes != end) {
        err = "RaBitQ fixed data is truncated or incompatible";
        return false;
    }
    quantizer->load(const_cast<char *>(p),
                    reinterpret_cast<float *>(const_cast<char *>(p + random_bytes)),
                    reinterpret_cast<float *>(
                        const_cast<char *>(p + random_bytes + centroid_bytes)));
    quantizer->set_rescaling_factor(header.query_rescaling_factor);

    const size_t code_size = im.quantizer_code_size();
    const size_t expected_codes = node_count * code_size;
    std::vector<uint8_t> codes;
    if (!im.disk) codes.reserve(expected_codes);
    const size_t n_segs = (node_count + kSegRecords - 1) / kSegRecords;
    for (size_t seg = 0; seg < n_segs; seg++) {
        std::vector<char> chunk;
        if (!read(kKindQuantizerCodes, uint32_t(seg), chunk)) {
            err = "RaBitQ persistence segments are missing";
            return false;
        }
        const size_t records = std::min(kSegRecords, node_count - seg * kSegRecords);
        const size_t actual_bytes = records * code_size;
        const size_t full_segment_bytes = kSegRecords * code_size;
        if (chunk.size() != actual_bytes && chunk.size() != full_segment_bytes) {
            err = "RaBitQ code coverage does not match graph nodes";
            return false;
        }
        for (size_t record = 0; record < records; record++) {
            const auto *code = reinterpret_cast<const uint8_t *>(chunk.data()) +
                               record * code_size;
            if (!::rabitq::CodeHasValidCluster(code) ||
                !::rabitq::CodeHasFiniteFactors(code, im.dim)) {
                err = "RaBitQ code contains invalid values";
                return false;
            }
        }
        if (!im.disk) {
            codes.insert(codes.end(), reinterpret_cast<const uint8_t *>(chunk.data()),
                         reinterpret_cast<const uint8_t *>(chunk.data() + actual_bytes));
        }
    }
    im.rabitq_quantizer = std::move(quantizer);
    im.rabitq_query_rescaling_factor = header.query_rescaling_factor;
    if (im.disk) {
        im.quantizer_codes.clear();
    } else {
        im.quantizer_codes = std::move(codes);
    }
    im.quantizer_use = expected_codes != 0;
    return true;
}

template <typename ImplT>
bool ReadPQ(const GraphBridge::SegReadFn &read, ImplT &im,
            size_t node_count, std::string &err) {
    if (im.quantizer_type != QuantizerType::PQ) return true;
    if (node_count == 0) {
        im.quantizer_use = false;
        return true;
    }

    std::vector<char> fixed;
    if (!read(kKindQuantizerFixed, 0, fixed)) {
        err = "PQ persistence segments are missing";
        return false;
    }
    const char *p = fixed.data();
    const char *end = p + fixed.size();
    PQBlobHeader header{};
    if (!ReadPod(p, end, header) || header.magic != kPQBlobMagic ||
        header.version != kPQBlobVersion || header.dim != im.dim ||
        header.m != im.pq_m || header.nbits != 8) {
        err = "PQ fixed data has an unsupported format";
        return false;
    }

    auto state = std::make_unique<SqlitePQState>();
    auto &pq = state->quantizer;
    ::vex::quantizer::PQContext ctx;
    pq.set_basic_values(header.dim, header.m, header.nbits);
    pq.set_derived_values(ctx);
    pq.set_fvec_L2sqr_ny_nearest_func();
    pq.set_fvec_ny_distance_func(
        im.metric == VexMetric::INNER_PRODUCT ? Metric::INNER_PRODUCT : Metric::L2);
    pq.set_dist_code_func();
    const size_t centroid_bytes = pq.get_centroids_size() * sizeof(float);
    if (header.centroid_bytes != centroid_bytes || p + centroid_bytes != end) {
        err = "PQ fixed data is truncated or incompatible";
        return false;
    }
    std::memcpy(pq.centroids, p, centroid_bytes);
    pq.trained = true;

    const size_t code_size = pq.code_size;
    const size_t expected_codes = node_count * code_size;
    std::vector<uint8_t> codes;
    if (!im.disk) codes.reserve(expected_codes);
    const size_t n_segs = (node_count + kSegRecords - 1) / kSegRecords;
    for (size_t seg = 0; seg < n_segs; seg++) {
        std::vector<char> chunk;
        if (!read(kKindQuantizerCodes, uint32_t(seg), chunk)) {
            err = "PQ persistence segments are missing";
            return false;
        }
        const size_t records = std::min(kSegRecords, node_count - seg * kSegRecords);
        const size_t actual_bytes = records * code_size;
        const size_t full_segment_bytes = kSegRecords * code_size;
        if (chunk.size() != actual_bytes && chunk.size() != full_segment_bytes) {
            err = "PQ code coverage does not match graph nodes";
            return false;
        }
        if (!im.disk) {
            codes.insert(codes.end(), reinterpret_cast<const uint8_t *>(chunk.data()),
                         reinterpret_cast<const uint8_t *>(chunk.data() + actual_bytes));
        }
    }
    im.pq_state = std::move(state);
    if (im.disk) {
        im.quantizer_codes.clear();
    } else {
        im.quantizer_codes = std::move(codes);
    }
    im.quantizer_use = expected_codes != 0;
    return true;
}

template <typename ImplT>
bool ReadQuantizer(const GraphBridge::SegReadFn &read, ImplT &im,
                   size_t node_count, std::string &err) {
    if (im.quantizer_type == QuantizerType::PQ) {
        return ReadPQ(read, im, node_count, err);
    }
    if (im.quantizer_type == QuantizerType::RABITQ) {
        return ReadRaBitQ(read, im, node_count, err);
    }
    return true;
}

// 载入后初始化空壳节点计数（tid 摘除产生的 dead 节点，序列化随 elems 走）。
template <typename ElemsVec>
size_t CountDeadNodes(const ElemsVec &elems) {
    size_t dead = 0;
    for (const auto &e : elems) {
        if (e.tids.empty()) dead++;
    }
    return dead;
}

void BuildMetaBlob(uint16_t dim, int m, int efc, VexMetric metric, size_t base_n,
                   size_t upper_n, const GraphIndexEntryInfo &entry, std::vector<char> &out) {
    out.clear();
    BlobHeader h{};
    h.magic = kGraphBlobMagic;
    h.version = kGraphBlobVersion;
    h.dim = dim;
    h.m = uint32_t(m);
    h.ef_construction = uint32_t(efc);
    h.metric = uint32_t(metric);
    h.base_count = base_n;
    h.upper_count = upper_n;
    h.entry_id = entry.id;
    h.entry_cur_layer_idx = entry.cur_layer_idx;
    h.entry_level = entry.level;
    h.seg_records = uint32_t(kSegRecords);
    AppendPod(out, h);
}

template <typename ElemsVec>
void BuildElemsBlob(const ElemsVec &elems, size_t base_n, std::vector<char> &out) {
    out.clear();
    for (size_t i = 0; i < base_n; i++) {
        const auto &tids = elems[i].tids;
        AppendPod(out, uint32_t(tids.size()));
        for (const auto &t : tids) AppendPod(out, int64_t(t.row_id));
    }
}

bool ParseElemsBlob(const std::vector<char> &blob, size_t base_n,
                    std::vector<GraphIndexPoint> &elems, std::string &err) {
    const char *p = blob.data();
    const char *end = p + blob.size();
    elems.resize(base_n);
    for (size_t i = 0; i < base_n; i++) {
        uint32_t cnt = 0;
        if (!ReadPod(p, end, cnt)) {
            err = "graph v2 elems blob truncated";
            return false;
        }
        auto &tids = elems[i].tids;
        tids.resize(cnt);
        for (uint32_t j = 0; j < cnt; j++) {
            int64_t rid = 0;
            if (!ReadPod(p, end, rid)) {
                err = "graph v2 elems blob truncated in rowids";
                return false;
            }
            tids[j] = ItemPointerData{};
            tids[j].row_id = rid;
        }
    }
    return true;
}

template <typename ElemsVec>
void BuildElemsSegment(const ElemsVec &elems, size_t begin, size_t records,
                       std::vector<char> &out) {
    out.clear();
    for (size_t i = begin; i < begin + records; i++) {
        const auto &tids = elems[i].tids;
        AppendPod(out, uint32_t(tids.size()));
        for (const auto &tid : tids) AppendPod(out, int64_t(tid.row_id));
    }
}

bool ParseElemsSegment(const std::vector<char> &blob, size_t begin, size_t records,
                       std::vector<GraphIndexPoint> &elems, std::string &err) {
    const char *p = blob.data();
    const char *end = p + blob.size();
    for (size_t i = begin; i < begin + records; i++) {
        uint32_t count = 0;
        if (!ReadPod(p, end, count) ||
            count > static_cast<uint32_t>((end - p) / sizeof(int64_t))) {
            err = "graph v4 elems segment is truncated";
            return false;
        }
        auto &tids = elems[i].tids;
        tids.resize(count);
        for (uint32_t j = 0; j < count; j++) {
            int64_t rowid = 0;
            if (!ReadPod(p, end, rowid)) {
                err = "graph v4 elems segment is truncated in rowids";
                return false;
            }
            tids[j] = ItemPointerData{};
            tids[j].row_id = rowid;
        }
    }
    if (p != end) {
        err = "graph v4 elems segment has trailing data";
        return false;
    }
    return true;
}

template <typename UpperVec>
void BuildUpperBlob(const UpperVec &ups, size_t upper_n, int m, std::vector<char> &out) {
    const size_t nb = size_t(m) * 2;
    out.clear();
    for (size_t i = 0; i < upper_n; i++) {
        const auto &up = ups[i];
        AppendPod(out, up.lower_layer_idx);
        AppendPod(out, up.id);
        out.insert(out.end(), reinterpret_cast<const char *>(up.neighbors_info.data()),
                   reinterpret_cast<const char *>(up.neighbors_info.data() + nb));
        out.insert(out.end(), reinterpret_cast<const char *>(up.dists.data()),
                   reinterpret_cast<const char *>(up.dists.data() + size_t(m)));
    }
}

using UpperRec = SqliteStore::UpperPointRec;

bool ParseUpperBlob(const std::vector<char> &blob, size_t upper_n, int m,
                    std::vector<UpperRec> &ups, std::string &err) {
    const size_t nb = size_t(m) * 2;
    const char *p = blob.data();
    const char *end = p + blob.size();
    ups.resize(upper_n);
    for (size_t i = 0; i < upper_n; i++) {
        auto &up = ups[i];
        up.neighbors_info.resize(nb);
        up.dists.resize(size_t(m));
        up.stat_words.assign((size_t(m) + 31) / 32, 0);
        if (!ReadPod(p, end, up.lower_layer_idx) || !ReadPod(p, end, up.id) ||
            p + nb * sizeof(uint32) + size_t(m) * sizeof(float) > end) {
            err = "graph v2 upper blob truncated";
            return false;
        }
        std::memcpy(up.neighbors_info.data(), p, nb * sizeof(uint32));
        p += nb * sizeof(uint32);
        std::memcpy(up.dists.data(), p, size_t(m) * sizeof(float));
        p += size_t(m) * sizeof(float);
    }
    return true;
}

template <typename UpperVec>
void BuildUpperSegment(const UpperVec &ups, size_t begin, size_t records, int m,
                       std::vector<char> &out) {
    const size_t neighbor_count = size_t(m) * 2;
    out.clear();
    out.reserve(records * (sizeof(uint32) * 2 +
                           neighbor_count * sizeof(uint32) + size_t(m) * sizeof(float)));
    for (size_t i = begin; i < begin + records; i++) {
        const auto &up = ups[i];
        AppendPod(out, up.lower_layer_idx);
        AppendPod(out, up.id);
        out.insert(out.end(), reinterpret_cast<const char *>(up.neighbors_info.data()),
                   reinterpret_cast<const char *>(up.neighbors_info.data() + neighbor_count));
        out.insert(out.end(), reinterpret_cast<const char *>(up.dists.data()),
                   reinterpret_cast<const char *>(up.dists.data() + size_t(m)));
    }
}

bool ParseUpperSegment(const std::vector<char> &blob, size_t begin, size_t records,
                       int m, std::vector<UpperRec> &ups, std::string &err) {
    const size_t neighbor_count = size_t(m) * 2;
    const size_t record_bytes = sizeof(uint32) * 2 +
                                neighbor_count * sizeof(uint32) +
                                size_t(m) * sizeof(float);
    if (blob.size() != records * record_bytes) {
        err = "graph v4 upper segment is truncated";
        return false;
    }
    const char *p = blob.data();
    const char *end = p + blob.size();
    for (size_t i = begin; i < begin + records; i++) {
        auto &up = ups[i];
        up.neighbors_info.resize(neighbor_count);
        up.dists.resize(size_t(m));
        up.stat_words.assign((size_t(m) + 31) / 32, 0);
        if (!ReadPod(p, end, up.lower_layer_idx) || !ReadPod(p, end, up.id)) {
            err = "graph v4 upper segment is truncated";
            return false;
        }
        std::memcpy(up.neighbors_info.data(), p, neighbor_count * sizeof(uint32));
        p += neighbor_count * sizeof(uint32);
        std::memcpy(up.dists.data(), p, size_t(m) * sizeof(float));
        p += size_t(m) * sizeof(float);
    }
    return true;
}

// 读 meta 段并校验参数。失败填 err 返回 false。
bool ReadMetaV2(const GraphBridge::SegReadFn &read, uint16_t dim, int m, VexMetric metric,
                BlobHeader &h, std::string &err) {
    std::vector<char> buf;
    if (!read(kKindMeta, 0, buf)) {
        err = "graph v2 meta segment missing";
        return false;
    }
    const char *p = buf.data();
    const char *end = p + buf.size();
    if (!ReadPod(p, end, h)) {
        err = "graph v2 meta too short";
        return false;
    }
    if (h.magic != kGraphBlobMagic) {
        err = "graph v2 bad magic";
        return false;
    }
    if (h.version < kGraphBlobMinVersion || h.version > kGraphBlobVersion) {
        err = "graph format v" + std::to_string(h.version) + " unsupported";
        return false;
    }
    if (h.dim != dim || int(h.m) != m || VexMetric(h.metric) != metric) {
        err = "graph v2 params mismatch index config";
        return false;
    }
    if (h.seg_records != kSegRecords) {
        err = "graph v2 seg_records mismatch";
        return false;
    }
    return true;
}

}  // namespace

bool GraphBridge::SerializeV2(const SegWriteFn &write) {
    auto &im = *impl_;
    std::vector<char> buf;

    auto write_quantizer = [&]() -> bool {
        if (im.quantizer_type == QuantizerType::NONE) return true;
        const size_t node_count = im.disk ? im.disk->elems.size() : im.store.elems.size();
        if (node_count == 0) return true;
        if (!im.quantizer_use) return false;

        if (im.quantizer_fixed_dirty) {
            std::vector<char> fixed;
            if (im.uses_pq() && im.pq_state) {
                const auto &pq = im.pq_state->quantizer;
                PQBlobHeader header{};
                header.magic = kPQBlobMagic;
                header.version = kPQBlobVersion;
                header.dim = uint32_t(im.dim);
                header.m = uint32_t(pq.M);
                header.nbits = uint32_t(pq.nbits);
                header.centroid_bytes = pq.get_centroids_size() * sizeof(float);
                AppendPod(fixed, header);
                const char *centroids = reinterpret_cast<const char *>(pq.centroids);
                fixed.insert(fixed.end(), centroids, centroids + header.centroid_bytes);
            } else if (im.uses_rabitq() && im.rabitq_quantizer) {
                const int padded = RABITQ_PADDED_DIM(im.dim);
                RaBitQBlobHeader header{};
                header.magic = kRaBitQBlobMagic;
                header.version = kRaBitQBlobVersion;
                header.query_rescaling_factor = im.rabitq_query_rescaling_factor;
                header.random_bytes = im.rabitq_quantizer->get_random_matrix_size();
                header.centroid_bytes = HNSW_RABITQ_NUM_CLUSTERS * size_t(im.dim) * sizeof(float);
                header.rotated_bytes = HNSW_RABITQ_NUM_CLUSTERS * size_t(padded) * sizeof(float);
                AppendPod(fixed, header);
                fixed.insert(fixed.end(), im.rabitq_quantizer->get_random_matrix(),
                             im.rabitq_quantizer->get_random_matrix() + header.random_bytes);
                const char *centroids = reinterpret_cast<const char *>(
                    im.rabitq_quantizer->get_centroids());
                fixed.insert(fixed.end(), centroids, centroids + header.centroid_bytes);
                const char *rotated = reinterpret_cast<const char *>(
                    im.rabitq_quantizer->get_rotated_centroids());
                fixed.insert(fixed.end(), rotated, rotated + header.rotated_bytes);
            } else {
                return false;
            }
            if (!write(kKindQuantizerFixed, 0, fixed)) return false;
            im.quantizer_fixed_dirty = false;
        }

        // DiskStore 的 code 与 base/vec 一样由统一页缓存跟踪 dirty，并已在
        // flush_dirty_segs 中落盘；这里只处理 MemStore 的 code vector。
        if (im.disk) return true;

        const size_t code_size = im.quantizer_code_size();
        for (auto it = im.quantizer_dirty_code_segs.begin();
             it != im.quantizer_dirty_code_segs.end();) {
            const size_t seg = *it;
            const size_t begin_record = seg * kSegRecords;
            const size_t records = std::min(kSegRecords, node_count - begin_record);
            const auto begin = im.quantizer_codes.begin() + begin_record * code_size;
            const auto end = begin + records * code_size;
            std::vector<char> codes(begin, end);
            if (!write(kKindQuantizerCodes, uint32_t(seg), codes)) return false;
            it = im.quantizer_dirty_code_segs.erase(it);
        }
        return true;
    };

    if (im.disk) {
        // DiskStore：meta 很小，始终写；elems/upper/base/vec/code 只写 dirty
        // 段。v2/v3 载入后的首次写会通过 full_dirty 一次性迁移元数据段。
        auto &ds = *im.disk;
        const size_t base_n = ds.elems.size();
        const size_t upper_n = ds.upper_points.size();
        BuildMetaBlob(im.dim, im.m, im.ef_construction, im.metric, base_n, upper_n,
                      ds.entry_info, buf);
        if (!write(kKindMeta, 0, buf)) return false;
        auto write_elem_seg = [&](size_t seg) -> bool {
            const size_t begin = seg * kSegRecords;
            if (begin >= base_n) return true;
            BuildElemsSegment(ds.elems, begin,
                              std::min(kSegRecords, base_n - begin), buf);
            return write(kKindElems, uint32_t(seg), buf);
        };
        auto write_upper_seg = [&](size_t seg) -> bool {
            const size_t begin = seg * kSegRecords;
            if (begin >= upper_n) return true;
            BuildUpperSegment(ds.upper_points, begin,
                              std::min(kSegRecords, upper_n - begin), im.m, buf);
            return write(kKindUpper, uint32_t(seg), buf);
        };
        if (im.full_dirty) {
            const size_t elem_segs = (base_n + kSegRecords - 1) / kSegRecords;
            const size_t upper_segs = (upper_n + kSegRecords - 1) / kSegRecords;
            for (size_t seg = 0; seg < elem_segs; seg++) {
                if (!write_elem_seg(seg)) return false;
            }
            for (size_t seg = 0; seg < upper_segs; seg++) {
                if (!write_upper_seg(seg)) return false;
            }
        } else {
            for (auto it = ds.dirty_elem_segs_.begin();
                 it != ds.dirty_elem_segs_.end();) {
                if (!write_elem_seg(*it)) return false;
                it = ds.dirty_elem_segs_.erase(it);
            }
            for (auto it = ds.dirty_upper_segs_.begin();
                 it != ds.dirty_upper_segs_.end();) {
                if (!write_upper_seg(*it)) return false;
                it = ds.dirty_upper_segs_.erase(it);
            }
        }
        bool ok = true;
        ds.flush_dirty_segs([&](int kind, uint32 seg, const std::vector<char> &data) -> bool {
            bool w = write(kind, seg, data);
            if (!w) ok = false;
            return w;  // 失败段保留 dirty，重试 flush 时重写
        });
        if (ok) {
            im.full_dirty = false;
            ds.dirty_elem_segs_.clear();
            ds.dirty_upper_segs_.clear();
            ds.upper_dirty = false;
            im.elems_dirty = false;
        }
        return ok && write_quantizer();
    }

    // 全内存：meta 始终写，其余全部按 64 条记录分段。full=全部段（调用方
    // 已清旧段）；增量=仅 dirty 集，单行提交与总行数无关。
    auto &st = im.store;
    const size_t base_n = st.base_points.size();
    const size_t upper_n = st.upper_points.size();
    const size_t nb = size_t(im.m) * 2;
    const size_t base_rec = nb * (sizeof(uint32) + sizeof(float));
    const size_t vec_size = st.vec_size;

    BuildMetaBlob(im.dim, im.m, im.ef_construction, im.metric, base_n, upper_n,
                  st.entry_info, buf);
    if (!write(kKindMeta, 0, buf)) return false;

    auto write_elem_seg = [&](size_t seg) -> bool {
        const size_t begin = seg * kSegRecords;
        if (begin >= base_n) return true;
        BuildElemsSegment(st.elems, begin,
                          std::min(kSegRecords, base_n - begin), buf);
        return write(kKindElems, uint32_t(seg), buf);
    };
    auto write_upper_seg = [&](size_t seg) -> bool {
        const size_t begin = seg * kSegRecords;
        if (begin >= upper_n) return true;
        BuildUpperSegment(st.upper_points, begin,
                          std::min(kSegRecords, upper_n - begin), im.m, buf);
        return write(kKindUpper, uint32_t(seg), buf);
    };

    auto write_base_seg = [&](size_t seg) -> bool {
        buf.assign(kSegRecords * base_rec, 0);
        for (size_t r = 0; r < kSegRecords; r++) {
            char *rec = buf.data() + r * base_rec;
            size_t idx = seg * kSegRecords + r;
            if (idx < base_n) {
                const auto &bp = st.base_points[idx];
                std::memcpy(rec, bp.neighbors.data(), nb * sizeof(uint32));
                std::memcpy(rec + nb * sizeof(uint32), bp.dists.data(), nb * sizeof(float));
            } else {
                VexFillInvalidBaseRec(rec, size_t(im.m));
            }
        }
        return write(kKindBase, uint32(seg), buf);
    };
    auto write_vec_seg = [&](size_t seg) -> bool {
        if (im.compact_mode) return true;
        buf.assign(kSegRecords * vec_size, 0);
        for (size_t r = 0; r < kSegRecords; r++) {
            size_t idx = seg * kSegRecords + r;
            if (idx < base_n) {
                std::memcpy(buf.data() + r * vec_size, st.vectors[idx].data(), vec_size);
            }
        }
        return write(kKindVec, uint32(seg), buf);
    };

    if (im.full_dirty) {
        const size_t n_segs = (base_n + kSegRecords - 1) / kSegRecords;
        const size_t upper_segs = (upper_n + kSegRecords - 1) / kSegRecords;
        for (size_t seg = 0; seg < n_segs; seg++) {
            if (!write_elem_seg(seg)) return false;
        }
        for (size_t seg = 0; seg < upper_segs; seg++) {
            if (!write_upper_seg(seg)) return false;
        }
        for (size_t seg = 0; seg < n_segs; seg++) {
            if (!write_base_seg(seg)) return false;
        }
        if (!im.compact_mode) {
            for (size_t seg = 0; seg < n_segs; seg++) {
                if (!write_vec_seg(seg)) return false;
            }
        }
        im.full_dirty = false;
        im.elems_dirty = false;
        st.dirty_elem_segs_.clear();
        st.dirty_upper_segs_.clear();
        st.dirty_base_segs_.clear();
        st.dirty_vec_segs_.clear();
        return write_quantizer();
    }
    // 增量：失败段保留在 dirty 集供重试
    for (auto it = st.dirty_elem_segs_.begin(); it != st.dirty_elem_segs_.end();) {
        if (!write_elem_seg(*it)) return false;
        it = st.dirty_elem_segs_.erase(it);
    }
    for (auto it = st.dirty_upper_segs_.begin(); it != st.dirty_upper_segs_.end();) {
        if (!write_upper_seg(*it)) return false;
        it = st.dirty_upper_segs_.erase(it);
    }
    im.elems_dirty = false;
    for (auto it = st.dirty_base_segs_.begin(); it != st.dirty_base_segs_.end();) {
        if (!write_base_seg(*it)) return false;
        it = st.dirty_base_segs_.erase(it);
    }
    if (im.compact_mode) {
        st.dirty_vec_segs_.clear();
    } else {
        for (auto it = st.dirty_vec_segs_.begin(); it != st.dirty_vec_segs_.end();) {
            if (!write_vec_seg(*it)) return false;
            it = st.dirty_vec_segs_.erase(it);
        }
    }
    return write_quantizer();
}

bool GraphBridge::NeedsFullRewrite() const {
    return !impl_->disk && impl_->full_dirty;
}

int64_t GraphBridge::PeekPersistedNodeCount(const SegReadFn &read) {
    std::vector<char> buf;
    if (!read || !read(kKindMeta, 0, buf)) return -1;
    const char *p = buf.data();
    const char *end = p + buf.size();
    BlobHeader h{};
    if (!ReadPod(p, end, h) || h.magic != kGraphBlobMagic) return -1;
    return int64_t(h.base_count);
}

std::unique_ptr<GraphBridge> GraphBridge::OpenV2(const SegReadFn &read, uint16_t dim, int m,
                                                 int ef_construction, VexMetric metric,
                                                 QuantizerType quantizer, uint32_t pq_m,
                                                 bool compact_mode,
                                                 std::string &err) {
    BlobHeader h{};
    if (!ReadMetaV2(read, dim, m, metric, h, err)) return nullptr;

    auto bridge = std::make_unique<GraphBridge>(dim, m, ef_construction, metric,
                                                quantizer, pq_m, compact_mode);
    auto &st = bridge->impl_->store;
    st.compact_mode_ = compact_mode;
    const size_t base_n = size_t(h.base_count);
    const size_t upper_n = size_t(h.upper_count);
    const size_t nb = size_t(m) * 2;
    const size_t base_rec = nb * (sizeof(uint32) + sizeof(float));

    st.ResizeForReload(base_n, upper_n);

    std::vector<char> buf;
    if (h.version >= 4) {
        const size_t elem_segs = (base_n + kSegRecords - 1) / kSegRecords;
        const size_t upper_segs = (upper_n + kSegRecords - 1) / kSegRecords;
        for (size_t seg = 0; seg < elem_segs; seg++) {
            const size_t begin = seg * kSegRecords;
            const size_t records = std::min(kSegRecords, base_n - begin);
            if (!read(kKindElems, uint32_t(seg), buf) ||
                !ParseElemsSegment(buf, begin, records, st.elems, err)) return nullptr;
        }
        for (size_t seg = 0; seg < upper_segs; seg++) {
            const size_t begin = seg * kSegRecords;
            const size_t records = std::min(kSegRecords, upper_n - begin);
            if (!read(kKindUpper, uint32_t(seg), buf) ||
                !ParseUpperSegment(buf, begin, records, m, st.upper_points, err)) return nullptr;
        }
    } else {
        if (!read(kKindElems, 0, buf) ||
            !ParseElemsBlob(buf, base_n, st.elems, err)) return nullptr;
        if (!read(kKindUpper, 0, buf) ||
            !ParseUpperBlob(buf, upper_n, m, st.upper_points, err)) return nullptr;
        bridge->impl_->full_dirty = true;
    }
    bridge->impl_->dead_nodes = CountDeadNodes(st.elems);

    const size_t n_segs = (base_n + kSegRecords - 1) / kSegRecords;
    for (size_t seg = 0; seg < n_segs; seg++) {
        if (!read(kKindBase, uint32(seg), buf) || buf.size() != kSegRecords * base_rec) {
            err = "graph v2 base segment missing/short";
            return nullptr;
        }
        size_t lo = seg * kSegRecords;
        size_t hi = std::min(base_n, lo + kSegRecords);
        for (size_t idx = lo; idx < hi; idx++) {
            const char *rec = buf.data() + (idx - lo) * base_rec;
            auto &bp = st.base_points[idx];
            std::memcpy(bp.neighbors.data(), rec, nb * sizeof(uint32));
            std::memcpy(bp.dists.data(), rec + nb * sizeof(uint32), nb * sizeof(float));
        }
    }
    if (!compact_mode) {
        for (size_t seg = 0; seg < n_segs; seg++) {
            if (!read(kKindVec, uint32(seg), buf) || buf.size() != kSegRecords * st.vec_size) {
                err = "graph v2 vec segment missing/short";
                return nullptr;
            }
            size_t lo = seg * kSegRecords;
            size_t hi = std::min(base_n, lo + kSegRecords);
            for (size_t idx = lo; idx < hi; idx++) {
                st.vectors[idx].assign(buf.data() + (idx - lo) * st.vec_size,
                                       buf.data() + (idx - lo + 1) * st.vec_size);
            }
        }
    }
    st.entry_info.set(size_t(h.entry_id), size_t(h.entry_cur_layer_idx),
                      int_fast8_t(h.entry_level));
    if (!ReadQuantizer(read, *bridge->impl_, base_n, err)) return nullptr;
    return bridge;
}

std::unique_ptr<GraphBridge> GraphBridge::OpenV2Disk(const SegReadFn &read,
                                                     const SegWriteFn &write,
                                                     const SegRecReadFn &read_rec, uint16_t dim,
                                                     int m, int ef_construction,
                                                     VexMetric metric, QuantizerType quantizer,
                                                     uint32_t pq_m,
                                                     bool compact_mode,
                                                     size_t cache_budget,
                                                     std::string &err) {
    BlobHeader h{};
    if (!ReadMetaV2(read, dim, m, metric, h, err)) return nullptr;

    auto bridge = std::make_unique<GraphBridge>(dim, m, ef_construction, metric,
                                                quantizer, pq_m, compact_mode);
    auto &im = *bridge->impl_;
    SqliteDiskStore::PageIO io;
    io.read = read;
    io.write = write;
    io.read_rec = read_rec;
    im.disk = std::make_unique<SqliteDiskStore>(dim, uint_fast16_t(m),
                                                uint_fast32_t(dim) * sizeof(float),
                                                std::move(io), cache_budget,
                                                uint_fast32_t(im.quantizer_code_size()),
                                                compact_mode);
    auto &ds = *im.disk;
    ds.normalize_vectors_ = (metric == VexMetric::COSINE);

    const size_t base_n = size_t(h.base_count);
    const size_t upper_n = size_t(h.upper_count);
    ds.reset_capacity(base_n, upper_n);
    std::vector<char> buf;
    if (h.version >= 4) {
        const size_t elem_segs = (base_n + kSegRecords - 1) / kSegRecords;
        const size_t upper_segs = (upper_n + kSegRecords - 1) / kSegRecords;
        for (size_t seg = 0; seg < elem_segs; seg++) {
            const size_t begin = seg * kSegRecords;
            const size_t records = std::min(kSegRecords, base_n - begin);
            if (!read(kKindElems, uint32_t(seg), buf) ||
                !ParseElemsSegment(buf, begin, records, ds.elems, err)) return nullptr;
        }
        for (size_t seg = 0; seg < upper_segs; seg++) {
            const size_t begin = seg * kSegRecords;
            const size_t records = std::min(kSegRecords, upper_n - begin);
            if (!read(kKindUpper, uint32_t(seg), buf) ||
                !ParseUpperSegment(buf, begin, records, m, ds.upper_points, err)) return nullptr;
        }
    } else {
        if (!read(kKindElems, 0, buf) ||
            !ParseElemsBlob(buf, base_n, ds.elems, err)) return nullptr;
        if (!read(kKindUpper, 0, buf) ||
            !ParseUpperBlob(buf, upper_n, m, ds.upper_points, err)) return nullptr;
        im.full_dirty = true;
    }
    im.dead_nodes = CountDeadNodes(ds.elems);
    ds.entry_info.set(size_t(h.entry_id), size_t(h.entry_cur_layer_idx),
                      int_fast8_t(h.entry_level));
    ds.upper_dirty = false;
    ds.dirty_elem_segs_.clear();
    ds.dirty_upper_segs_.clear();
    if (!ReadQuantizer(read, im, base_n, err)) return nullptr;
    return bridge;
}

}  // namespace vexdb_sqlite
