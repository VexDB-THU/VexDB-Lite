#include "vex_graph_index.hpp"

#include <cmath>
#include <set>
#include <limits>
#include <thread>
#include <atomic>
#include <exception>
#include <mutex>
#include <vector>

#include "graph_index/graph_index_algorithm.h"
#include "distance/core/distance_dispatcher.h"
#include "quantizer/annkmeans.h"
#include "rabitq/code_distancer.h"

#include "vex/vex_disk_block_store.hpp"
#include "vex_distance.hpp"
#include "vex_fetch_utils.hpp"

#include "duckdb/parallel/task_scheduler.hpp"
#include "vex_hnsw_node.hpp"
#include "vex_physical_create_index.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/planner/operator/logical_create_index.hpp"
#include "duckdb/storage/table_io_manager.hpp"

namespace duckdb {

namespace {

using DuckMetricList = MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::COSINE>;
using DuckDTypeList = DistPrecisionTypeList<DistPrecisionType::FLOAT>;
constexpr uint32_t PQ_INVALID_CODE_POSITION = std::numeric_limits<uint32_t>::max();

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

static Metric ToDuckMetric(VexMetric metric) {
    switch (metric) {
    case VexMetric::L2:
        return Metric::L2;
    case VexMetric::INNER_PRODUCT:
        return Metric::INNER_PRODUCT;
    case VexMetric::COSINE:
        return Metric::COSINE;
    }
    throw InternalException("Unknown VexMetric");
}

static Metric ToRaBitQMetric(VexMetric metric) {
    return metric == VexMetric::L2 ? Metric::L2 : Metric::INNER_PRODUCT;
}

// Code view over DuckStore. Graph topology and row-id ownership stay in
// DuckStore; distance reads/writes are redirected to quantizer codes. RaBitQ
// codes are aligned by node id. PQ keeps row-oriented persistence and supplies
// node_code_positions to bridge node ids to code slots.
class DuckQuantizedSearchStore {
public:
    using T = DuckStore::T;
    using point_type = DuckStore::point_type;
    static constexpr bool use_dist_cache = false;

    DuckQuantizedSearchStore(DuckStore &store, const std::vector<uint8_t> &codes,
                             size_t code_size,
                             const std::vector<uint32_t> *node_code_positions = nullptr)
        : store_(store), codes_(codes), mutable_codes_(nullptr), code_size_(code_size),
          node_code_positions_(node_code_positions), mutable_node_code_positions_(nullptr) {}

    DuckQuantizedSearchStore(DuckStore &store, std::vector<uint8_t> &codes,
                             size_t code_size,
                             std::vector<uint32_t> *node_code_positions = nullptr)
        : store_(store), codes_(codes), mutable_codes_(&codes), code_size_(code_size),
          node_code_positions_(node_code_positions),
          mutable_node_code_positions_(node_code_positions) {}

    void SetPendingCodePosition(uint32_t position) { pending_code_position_ = position; }

    template <bool exclusive = false, bool bottom_only = false>
    auto get_entry(int_fast8_t insert_level = 0) {
        return store_.template get_entry<exclusive, bottom_only>(insert_level);
    }

    void release_entry_lock(bool shared) {
        store_.release_entry_lock(shared);
    }

    template <bool is_base_layer, bool shared_lock>
    void lock_point(T idx) {
        store_.template lock_point<is_base_layer, shared_lock>(idx);
    }

    template <bool is_base_layer, bool shared_lock>
    void unlock_point(T idx) {
        store_.template unlock_point<is_base_layer, shared_lock>(idx);
    }

    template <bool is_base_layer>
    auto get_point_info(T idx) {
        return store_.template get_point_info<is_base_layer>(idx);
    }

    template <bool is_base_layer>
    T assign_vector_id() {
        return store_.template assign_vector_id<is_base_layer>();
    }

    void add_elem(PointExtensionContext &ctx, T id, const ItemPointerData &tid) {
        store_.add_elem(ctx, id, tid);
    }

    void add_elem(PointExtensionContext &ctx, T id, Span<const ItemPointerData> tids) {
        store_.add_elem(ctx, id, tids);
    }

    template <typename Distancer>
    void add_vector(Distancer &, T id, const char *code) {
        if (!mutable_codes_) {
            throw InternalException("quantized search store is read-only");
        }
        if (mutable_node_code_positions_) {
            if (pending_code_position_ == PQ_INVALID_CODE_POSITION ||
                (pending_code_position_ + 1) * code_size_ > mutable_codes_->size()) {
                throw InternalException("PQ pending code position is invalid");
            }
            if (mutable_node_code_positions_->size() <= static_cast<size_t>(id)) {
                mutable_node_code_positions_->resize(static_cast<size_t>(id) + 1,
                                                     PQ_INVALID_CODE_POSITION);
            }
            std::memcpy(mutable_codes_->data() + pending_code_position_ * code_size_,
                        code, code_size_);
            (*mutable_node_code_positions_)[id] = pending_code_position_;
            return;
        }
        const size_t offset = static_cast<size_t>(id) * code_size_;
        if (mutable_codes_->size() < offset + code_size_) {
            mutable_codes_->resize(offset + code_size_);
        }
        std::memcpy(mutable_codes_->data() + offset, code, code_size_);
    }

    void set_entrypoint(T id, T cur_layer_idx, int_fast8_t level) {
        store_.set_entrypoint(id, cur_layer_idx, level);
    }

    uint32 get_elemsize() const { return static_cast<uint32>(code_size_); }
    uint16 get_dim() const { return store_.get_dim(); }
    uint32 get_vecsize() const { return static_cast<uint32>(code_size_); }
    size_t get_vector_num() const { return store_.get_vector_num(); }

    char *get_data(T id) { return static_cast<char *>(CodeFor(id)); }
    const char *get_data(T id) const { return static_cast<const char *>(CodeFor(id)); }

    struct my_buf {
        const char *data;
        char *get_vecbuf() const { return const_cast<char *>(data); }
        static constexpr void release() {}
    };

    my_buf read_data(T id) { return my_buf{get_data(id)}; }
    void reset_neighbors_val_pool() {}

    template <typename Distancer, typename IdVec>
    void get_distance_batch(const Distancer &distancer, const char *query,
                            const IdVec &ids, float *dists) {
        std::vector<void *> code_ptrs;
        code_ptrs.reserve(ids.size());
        for (auto id : ids) {
            code_ptrs.push_back(CodeFor(id));
        }
        distancer.get_distance_batch2(query, code_ptrs.data(), 0,
                                      static_cast<uint16_t>(code_ptrs.size()), dists);
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
        if (!code) {
            throw InternalException("quantized raw-code distance received a null code");
        }
        return distancer.get_distance_single(query, code, 0);
    }

    template <typename Distancer>
    float get_distance_precise(const Distancer &distancer, const char *query, const char *code) {
        return get_distance(distancer, query, code);
    }

    template <bool is_base_layer, typename CandVec, typename CandType>
    void get_neighbors(CandVec &out, const CandType &cand) {
        store_.template get_neighbors<is_base_layer>(out, cand);
    }

    template <bool is_base_layer>
    auto get_neighbor_stats(T idx) {
        return store_.template get_neighbor_stats<is_base_layer>(idx);
    }

    template <typename Bits>
    bool has_stat(Bits bits) const { return store_.has_stat(bits); }
    template <typename Bits>
    void set_stat(Bits bits) { store_.set_stat(bits); }

    template <bool is_base_layer>
    void set_neighbor(T idx, int16 pruned, T new_id, T new_upper_idx) {
        store_.template set_neighbor<is_base_layer>(idx, pruned, new_id, new_upper_idx);
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
        uint32_t code_position = id;
        if (node_code_positions_) {
            if (static_cast<size_t>(id) >= node_code_positions_->size() ||
                (*node_code_positions_)[id] == PQ_INVALID_CODE_POSITION) {
                throw InternalException("PQ code mapping does not cover graph node %u", id);
            }
            code_position = (*node_code_positions_)[id];
        }
        const size_t offset = static_cast<size_t>(code_position) * code_size_;
        if (code_size_ == 0 || offset + code_size_ > codes_.size()) {
            throw InternalException("quantizer code coverage does not match graph nodes");
        }
        return const_cast<uint8_t *>(codes_.data() + offset);
    }

    DuckStore &store_;
    const std::vector<uint8_t> &codes_;
    std::vector<uint8_t> *mutable_codes_;
    size_t code_size_;
    const std::vector<uint32_t> *node_code_positions_;
    std::vector<uint32_t> *mutable_node_code_positions_;
    uint32_t pending_code_position_ = PQ_INVALID_CODE_POSITION;
};

class DuckPQDistancer {
public:
    static constexpr bool has_estimation_func = false;
    static constexpr bool need_refine = false;

    explicit DuckPQDistancer(::vex::quantizer::ProductQuantizer &quantizer)
        : quantizer_(quantizer), dist_table_(quantizer.M * quantizer.ksub) {}

    void process(const float *query) {
        quantizer_.compute_distance_table(query, dist_table_.data());
    }

    float get_distance_single(const void *, const void *code, uint16) const {
        return quantizer_.distance_to_code(static_cast<const uint8_t *>(code),
                                           dist_table_.data());
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
        }
        for (; i < count; i++) {
            out[i] = get_distance_single(nullptr, codes[i], 0);
        }
    }

private:
    ::vex::quantizer::ProductQuantizer &quantizer_;
    std::vector<float> dist_table_;
};

template <typename Fn>
static auto RunWithDuckAlgo(VexMetric metric, idx_t dim, int ef_construction, int m, DuckStore &store, Fn &&fn) {
    return DispatchRunner<false, DuckMetricList, DuckDTypeList, DispatcherMode::NO_QUANT>::call(
        ToDuckMetric(metric), DistPrecisionType::FLOAT, static_cast<uint16>(dim), QuantizerType::NONE,
        [&](auto &distancer) -> decltype(auto) {
            using DistT = std::decay_t<decltype(distancer)>;
            using AlgoT = GraphIndexAlgorithm<DuckStore, DistT>;
            AlgoT algo(uint_fast16_t(ef_construction), uint_fast16_t(m), store, distancer);
            return fn(algo);
        });
}

static uint64_t MixCoverage64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

static uint64_t HashCoverageBytes(const_data_ptr_t data, idx_t size) {
    uint64_t h = 1469598103934665603ULL;
    for (idx_t i = 0; i < size; i++) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t HashCoverageRowId(row_t row_id) {
    return MixCoverage64(static_cast<uint64_t>(row_id));
}

static uint64_t HashCoverageRowBytes(row_t row_id, const_data_ptr_t bytes, idx_t size) {
    uint64_t h = HashCoverageRowId(row_id);
    h ^= HashCoverageBytes(bytes, size) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return MixCoverage64(h);
}

static uint64_t HashCoverageRowVector(row_t row_id, const float *vec, idx_t dim) {
    auto bytes = const_data_ptr_cast(reinterpret_cast<const char *>(vec));
    return HashCoverageRowBytes(row_id, bytes, dim * sizeof(float));
}

} // namespace

VexMetric ParseMetric(const string &metric_name) {
    auto name = StringUtil::Lower(metric_name);
    if (name == "l2") {
        return VexMetric::L2;
    }
    if (name == "ip" || name == "inner_product") {
        return VexMetric::INNER_PRODUCT;
    }
    if (name == "cos" || name == "cosine") {
        return VexMetric::COSINE;
    }
    throw InvalidInputException("Unsupported GRAPH_INDEX metric: %s", metric_name);
}

GraphIndex::GraphIndex(const string &name, IndexConstraintType constraint_type, const vector<column_t> &column_ids,
                       TableIOManager &table_io_manager, const vector<unique_ptr<Expression>> &unbound_expressions,
                       AttachedDatabase &db, idx_t dimension, int m, int ef_construction, VexMetric metric,
                       idx_t vec_column_index, uint32_t pq_m, bool compact_mode, int build_threads,
                       bool rabitq_requested)
    : BoundIndex(name, TYPE_NAME, constraint_type, column_ids, table_io_manager, unbound_expressions, db),
      dimension_(dimension), m_(m), ef_construction_(ef_construction),
      build_threads_(build_threads), metric_(metric),
      vec_column_index_(vec_column_index), pq_m_(pq_m),
      runtime_(make_uniq<GraphIndexRuntimeState>(dimension, m, Allocator::Get(db))),
      rabitq_requested_(rabitq_requested), compact_mode_(compact_mode) {
    runtime_->store.normalize_vectors_ = (metric_ == VexMetric::COSINE);
}

GraphIndex::~GraphIndex() {
    // ProductQuantizer deliberately has no destructor because PostgreSQL may
    // allocate it from a MemoryContext. DuckDB uses the default malloc/free
    // allocator, so this adapter owns and must release the codebook.
    ::vex::quantizer::PQContext ctx;
    pq_quantizer_.free_resources(ctx);
}

void GraphIndex::ApplyMirrorBudget() {
    auto &store = runtime_->store;
    // Only tighten when the buffer-manager-backed copy exists: over-budget nodes skip
    // the vectors[] mirror and rely on vector_alloc_ as their sole home. Without it
    // they'd have nowhere to live, so fall back to the unlimited (full-mirror) path.
    // (The disk-image load path never InitAllocators → vector_alloc_ is null → it stays
    // full-mirror and outside the global pool by construction.)
    if (graph_memory_limit_bytes_ == 0 || !store.vector_alloc_ || store.vec_size == 0) {
        store.mirror_limit_bytes_ = 0;
        store.mirror_max_nodes_ = SIZE_MAX;
        return;
    }
    // vexdb_graph_memory_limit is a GLOBAL budget shared by all indexes' mirrors. Record
    // the limit here; the actual byte claim against the shared pool happens lazily in
    // ReserveCapacity / DeserializeFromStorage once this store's node count is known.
    // Until claimed, nothing is mirrored (mirror_max_nodes_ = 0).
    store.mirror_limit_bytes_ = static_cast<size_t>(graph_memory_limit_bytes_);
    store.mirror_max_nodes_ = 0;
}

unique_ptr<BoundIndex> GraphIndex::Create(CreateIndexInput &input) {
    if (input.unbound_expressions.empty()) {
        throw InvalidInputException("GRAPH_INDEX requires at least one indexed expression");
    }

    // CREATE INDEX syntax requires the vector column to be the first listed, e.g.
    // GRAPH_INDEX(vec, scalar1, scalar2). Reject any other layout up-front: silently
    // accepting duplicate or extra-vector columns corrupts the per-node metadata
    // segment layout, which assumes a fixed schema with one vector at slot 0.
    auto &first_type = input.unbound_expressions[0]->return_type;
    if (first_type.id() != LogicalTypeId::ARRAY ||
        ArrayType::GetChildType(first_type).id() != LogicalTypeId::FLOAT) {
        throw InvalidInputException("GRAPH_INDEX first column must be FLOAT[N], got %s",
                                    first_type.ToString());
    }
    auto &vec_type = first_type;
    // Hybrid (multi-column) GRAPH_INDEX is disabled for this release: the
    // per-partition graph build path is not yet stable. Single-column form
    // `GRAPH_INDEX(vec)` is supported; reject anything else up-front.
    if (input.unbound_expressions.size() > 1) {
        throw InvalidInputException(
            "GRAPH_INDEX currently supports only a single FLOAT[N] column; "
            "multi-column (hybrid/filtered) form is disabled in this release");
    }

    idx_t dimension = ArrayType::GetSize(vec_type);
    int m = 16;
    int ef_construction = 64;
    VexMetric metric = VexMetric::L2;

    // On lazy bind after restart DuckDB may not repopulate input.options with
    // the original WITH clause. Fall back to our persisted storage options so
    // allocator segment sizes (which depend on m) exactly match the bytes on
    // disk. Reading an m=12 image with the default m=16 layout shifts every
    // node header and can turn a valid vector pointer into zero.
    auto find_option = [&](const char *name) -> const Value * {
        auto input_it = input.options.find(name);
        if (input_it != input.options.end()) {
            return &input_it->second;
        }
        auto storage_it = input.storage_info.options.find(name);
        if (storage_it != input.storage_info.options.end()) {
            return &storage_it->second;
        }
        return nullptr;
    };

    if (auto m_value = find_option("m")) {
        try {
            m = m_value->DefaultCastAs(LogicalType::INTEGER).GetValue<int>();
        } catch (...) {
            throw InvalidInputException("GRAPH_INDEX option 'm' must be a valid integer");
        }
        if (m < 2 || m > 128) {
            throw InvalidInputException("GRAPH_INDEX option 'm' must be in [2, 128], got %d", m);
        }
    }
    if (auto ef_value = find_option("ef_construction")) {
        try {
            ef_construction = ef_value->DefaultCastAs(LogicalType::INTEGER).GetValue<int>();
        } catch (...) {
            throw InvalidInputException("GRAPH_INDEX option 'ef_construction' must be a valid integer");
        }
        if (ef_construction < 1 || ef_construction > 10000) {
            throw InvalidInputException(
                "GRAPH_INDEX option 'ef_construction' must be in [1, 10000], got %d", ef_construction);
        }
    }
    if (auto metric_value = find_option("metric")) {
        metric = ParseMetric(metric_value->GetValue<string>());
    }
    /* Default build_threads = available scheduler threads (matches `SET threads`).
     * Override via WITH (parallel_workers=N) — the name unified with PG / openGauss —
     * or its DuckDB-native alias WITH (threads=N). parallel_workers takes precedence. */
    int build_threads;
    auto pw_it = input.options.find("parallel_workers");
    auto threads_it = (pw_it != input.options.end()) ? pw_it : input.options.find("threads");
    if (threads_it != input.options.end()) {
        const char *opt_name = (pw_it != input.options.end()) ? "parallel_workers" : "threads";
        try {
            build_threads = threads_it->second.DefaultCastAs(LogicalType::INTEGER).GetValue<int>();
        } catch (...) {
            throw InvalidInputException("GRAPH_INDEX option '%s' must be a valid integer in [1, 1024]", opt_name);
        }
        if (build_threads < 1 || build_threads > 1024) {
            throw InvalidInputException("GRAPH_INDEX option '%s' must be in [1, 1024], got %d", opt_name, build_threads);
        }
    } else {
        idx_t nthreads = TaskScheduler::GetScheduler(input.context).NumberOfThreads();
        build_threads = static_cast<int>(std::min<idx_t>(nthreads, 1024));
        if (build_threads < 1) build_threads = 1;
    }
    uint32_t pq_m = 0;
    bool rabitq_requested = false;
    if (auto pq_m_value = find_option("pq_m")) {
        int pq_m_val = 0;
        try {
            pq_m_val = pq_m_value->DefaultCastAs(LogicalType::INTEGER).GetValue<int>();
        } catch (...) {
            throw InvalidInputException("GRAPH_INDEX option 'pq_m' must be a valid integer");
        }
        if (pq_m_val < 0) {
            throw InvalidInputException("GRAPH_INDEX option 'pq_m' must be >= 0, got %d", pq_m_val);
        }
        pq_m = static_cast<uint32_t>(pq_m_val);
    }
    if (auto quantizer_value = find_option("quantizer")) {
        auto qstr = StringUtil::Lower(quantizer_value->ToString());
        if (qstr == "pq") {
            if (pq_m == 0) {
                pq_m = ::vex::quantizer::ProductQuantizer::AutoSelectM(static_cast<uint32_t>(dimension));
            }
        } else if (qstr == "rabitq") {
            if (pq_m != 0) {
                throw InvalidInputException(
                    "GRAPH_INDEX quantizer='rabitq' cannot be combined with pq_m");
            }
            rabitq_requested = true;
        } else if (qstr == "none" || qstr.empty()) {
            pq_m = 0;
        } else {
            throw InvalidInputException("GRAPH_INDEX got unknown quantizer '%s' (expected 'pq', 'rabitq' or 'none')",
                                        quantizer_value->ToString());
        }
    }
    if (pq_m > 0 && dimension % pq_m != 0) {
        throw InvalidInputException("GRAPH_INDEX: pq_m (%u) must divide dimension (%llu)",
                                    pq_m, static_cast<unsigned long long>(dimension));
    }
    bool compact_mode = false;
    if (auto memory_mode_value = find_option("memory_mode")) {
        auto mstr = StringUtil::Lower(memory_mode_value->ToString());
        if (mstr == "compact") {
            compact_mode = true;
        } else if (mstr != "full" && !mstr.empty()) {
            throw InvalidInputException(
                "GRAPH_INDEX option 'memory_mode' must be 'full' or 'compact', got '%s'", mstr);
        }
    }
    if (compact_mode && pq_m == 0 && !rabitq_requested) {
        // Compact mode releases raw vectors post-train, so search must go
        // through PQ codes — auto-pick pq_m via the same heuristic used when
        // the user passes quantizer='pq' alone.
        pq_m = ::vex::quantizer::ProductQuantizer::AutoSelectM(static_cast<uint32_t>(dimension));
    }
    static const char *known_options[] = {"m", "ef_construction", "metric", "parallel_workers", "threads",
                                          "quantizer", "pq_m", "memory_mode"};
    for (auto &kv : input.options) {
        bool ok = false;
        for (auto *known : known_options) {
            if (StringUtil::CIEquals(kv.first, known)) { ok = true; break; }
        }
        if (!ok) {
            throw InvalidInputException("GRAPH_INDEX got unknown option '%s'", kv.first);
        }
    }

    auto graph_index = make_uniq<GraphIndex>(input.name, input.constraint_type, input.column_ids, input.table_io_manager,
                                             input.unbound_expressions, input.db, dimension, m, ef_construction,
                                             metric, 0, pq_m, compact_mode, build_threads, rabitq_requested);

    // Capture the mirror budget once here (ClientContext available). It persists on the
    // GraphIndex member across Create → PhysicalVexCreateIndex::Finalize → BuildBulk
    // (which builds a fresh store but reuses this object), and is applied to whichever
    // store is live after each InitAllocators via ApplyMirrorBudget().
    graph_index->graph_memory_limit_bytes_ = GetGraphMemoryLimitBytes(input.context);

    if (input.storage_info.allocator_infos.size() >= 3) {
        // Reload path: create allocators WITHOUT slot-0 reservation. The serialized
        // bitmask already has slot 0 reserved from the original InitAllocators().
        // Reserving it again would corrupt buffers_with_free_space tracking.
        graph_index->runtime_->store.CreateAllocators(input.table_io_manager.GetIndexBlockManager());
        graph_index->ApplyMirrorBudget();
        graph_index->DeserializeFromStorage(input.storage_info);
        graph_index->runtime_->store.normalize_vectors_ = (graph_index->metric_ == VexMetric::COSINE);
        return std::move(graph_index);
    }

    graph_index->runtime_->store.InitAllocators(input.table_io_manager.GetIndexBlockManager());
    graph_index->ApplyMirrorBudget();

    auto manifest_it = input.storage_info.options.find("vex_graph_manifest");
    if (manifest_it != input.storage_info.options.end()) {
        auto manifest_blob = StringValue::Get(manifest_it->second.DefaultCastAs(LogicalType::BLOB));
        auto manifest = vex_disk::DeserializeManifest(manifest_blob);
        if (!manifest.segments.empty()) {
            auto &seg = manifest.segments[0];
            auto disk_blob = vex_disk::ReadBlobFromBlocks(input.table_io_manager.GetIndexBlockManager(),
                                                          QueryContext(input.context), seg.blocks, seg.size);
            graph_index->LoadFromDiskImage(disk_blob);
            graph_index->DeserializePQAndModeFromStorage(input.storage_info);
            graph_index->runtime_->store.normalize_vectors_ = (graph_index->metric_ == VexMetric::COSINE);
            return std::move(graph_index);
        }
    }
    auto blob_it = input.storage_info.options.find("vex_graph_blob");
    if (blob_it != input.storage_info.options.end()) {
        auto blob = StringValue::Get(blob_it->second.DefaultCastAs(LogicalType::BLOB));
        graph_index->LoadFromDiskImage(blob);
        graph_index->DeserializePQAndModeFromStorage(input.storage_info);
    }
    graph_index->runtime_->store.normalize_vectors_ = (graph_index->metric_ == VexMetric::COSINE);
    return std::move(graph_index);
}

PhysicalOperator &GraphIndex::CreatePlan(PlanIndexInput &input) {
    auto &op = input.op;
    auto &planner = input.planner;

    vector<LogicalType> proj_types;
    vector<unique_ptr<Expression>> select_list;
    for (idx_t i = 0; i < op.expressions.size(); i++) {
        proj_types.push_back(op.expressions[i]->return_type);
        select_list.push_back(std::move(op.expressions[i]));
    }
    proj_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
    select_list.push_back(
        make_uniq<BoundReferenceExpression>(LogicalType(LogicalTypeId::BIGINT), op.info->scan_types.size() - 1));

    auto &proj = planner.Make<PhysicalProjection>(proj_types, std::move(select_list), op.estimated_cardinality);
    proj.children.push_back(input.table_scan);

    auto &create_idx = planner.Make<PhysicalVexCreateIndex>(op, op.table, op.info->column_ids, std::move(op.info),
                                                            std::move(op.unbound_expressions),
                                                            op.estimated_cardinality,
                                                            std::move(op.alter_table_info));
    create_idx.children.push_back(proj);
    return create_idx;
}

// Normalize a single vector in-place to unit L2 length. Used for cosine indexes
// so that the algorithm's apply_arrangement byte-equality check (used to detect
// duplicate vectors for dedup) sees the same form as what MemStore::add_vector
// stores — without this, raw input bytes never match the stored normalized bytes
// and dedup fails for cosine metric.
static void NormalizeInPlace(float *vec, idx_t dim) {
    float norm2 = 0.0f;
    for (idx_t i = 0; i < dim; i++) {
        norm2 += vec[i] * vec[i];
    }
    if (norm2 > 0.0f) {
        float inv = 1.0f / std::sqrt(norm2);
        for (idx_t i = 0; i < dim; i++) {
            vec[i] *= inv;
        }
    }
}

void GraphIndex::BuildBulk(const std::vector<float> &vectors, const std::vector<row_t> &row_ids) {
    runtime_ = make_uniq<GraphIndexRuntimeState>(dimension_, m_, Allocator::Get(db));
    runtime_->store.InitAllocators(table_io_manager.GetIndexBlockManager());
    // Fresh store for the bulk build — re-apply the budget captured in Create().
    ApplyMirrorBudget();

    // For cosine: normalize once at the adapter, then tell store NOT to normalize
    // again. Double-normalizing causes float-precision drift in the rounding of
    // sqrt(sum-of-squares) back through float32, so already-unit input ends up
    // byte-different from its second-pass copy. apply_arrangement's memcmp-based
    // dedup needs ctx.query and the stored vector to be byte-identical.
    std::vector<float> normalized;
    if (metric_ == VexMetric::COSINE) {
        runtime_->store.normalize_vectors_ = false;
        normalized = vectors;
        for (idx_t i = 0; i < row_ids.size(); i++) {
            NormalizeInPlace(normalized.data() + i * dimension_, dimension_);
        }
    }
    const float *src = (metric_ == VexMetric::COSINE) ? normalized.data() : vectors.data();

    // Pre-reserve outer vectors so concurrent assign_vector_id during the
    // parallel phase doesn't realloc and invalidate raw pointers held by
    // other workers' reads.
    // - base: exact count, every row gets one base node
    // - upper: expected ≈ base/(m-1) per HNSW theory, but the level distribution
    //   has a long tail and parallel timing variance can land more upper points
    //   per chunk. Reserve same as base to eliminate realloc risk; wastes some
    //   memory on small datasets but keeps the build safe.
    const size_t base_n = runtime_->store.get_vector_num() + row_ids.size();
    const size_t upper_n = base_n;
    runtime_->store.ReserveCapacity(base_n, upper_n);

    const idx_t n = row_ids.size();
    const int n_workers = std::clamp(build_threads_, 1, static_cast<int>(std::max<idx_t>(n, 1)));

    // Enable build-only locking in get_data for the parallel build span: workers mutate
    // MemStore concurrently without graph_rwlock_. RAII restores false on all paths
    // (including exceptions) — and only after all workers have joined inside RunWithDuckAlgo.
    // Also force search_lock_free_ off for the build: the per-node stripe locks
    // (lock_point) that synchronize concurrent worker reads against add_upperpoint's
    // exclusive publish are skipped when search_lock_free_ is set. A prior SearchANN
    // latches that flag true and never resets it, so a rebuild on the same store
    // would otherwise let build-time readers skip the lock → the upper-point publish
    // race resurfaces. Save/restore so search keeps its lock-free fast path afterward.
    struct BuildActiveGuard {
        std::atomic<bool> &flag;
        std::atomic<bool> &lock_free;
        bool saved_lock_free;
        BuildActiveGuard(std::atomic<bool> &f, std::atomic<bool> &lf)
            : flag(f), lock_free(lf),
              saved_lock_free(lf.load(std::memory_order_acquire)) {
            lock_free.store(false, std::memory_order_release);
            flag.store(true, std::memory_order_release);
        }
        ~BuildActiveGuard() {
            flag.store(false, std::memory_order_release);
            lock_free.store(saved_lock_free, std::memory_order_release);
        }
    } _build_active_guard(runtime_->store.parallel_build_active_, runtime_->store.search_lock_free_);

    RunWithDuckAlgo(metric_, dimension_, ef_construction_, m_, runtime_->store, [&](auto &algo) {
        using AlgoT = std::decay_t<decltype(algo)>;

        auto insert_one = [&](idx_t i) {
            PointExtensionContext point_ctx;
            ItemPointerData tid;
            tid.row_id = row_ids[i];
            const char *query = reinterpret_cast<const char *>(src + i * dimension_);
            typename AlgoT::InsertContextBase insert_ctx(point_ctx, query, &tid);
            algo.insert(insert_ctx);
        };

        // Phase A: serial first point if graph is empty. Multiple workers
        // racing on get_entry<>(level=-1) would all enter the empty-graph
        // branch and concurrently set_entrypoint, corrupting the entry.
        idx_t start_index = 0;
        if (runtime_->store.get_vector_num() == 0 && n > 0) {
            insert_one(0);
            start_index = 1;
        }

        // Phase B: serial loop if single-threaded or trivial remainder.
        if (n_workers <= 1 || start_index >= n) {
            for (idx_t i = start_index; i < n; i++) {
                insert_one(i);
            }
            return;
        }

        // Phase C: std::thread pool, contiguous slices. First exception
        // wins (HNSW build errors are usually OOM / NULL deref — one msg suffices).
        std::vector<std::thread> workers;
        std::vector<std::exception_ptr> errors(n_workers);
        const idx_t remaining = n - start_index;
        const idx_t per = remaining / n_workers;
        const idx_t rem = remaining % n_workers;
        idx_t offset = start_index;
        workers.reserve(n_workers);
        try {
            for (int t = 0; t < n_workers; t++) {
                const idx_t count = per + (t < static_cast<int>(rem) ? 1 : 0);
                const idx_t s = offset;
                const idx_t e = offset + count;
                offset = e;
                workers.emplace_back([t, s, e, &errors, &insert_one]() {
                    try {
                        for (idx_t i = s; i < e; i++) {
                            insert_one(i);
                        }
                    } catch (...) {
                        errors[t] = std::current_exception();
                    }
                });
            }
        } catch (...) {
            // emplace_back 中途 bad_alloc：必须 join 已 spawn 的，否则析构未 join
            // 的 std::thread → std::terminate。
            for (auto &w : workers) {
                if (w.joinable()) w.join();
            }
            throw;
        }
        for (auto &w : workers) {
            w.join();
        }
        for (auto &ep : errors) {
            if (ep) {
                std::rethrow_exception(ep);
            }
        }
    });
    runtime_->store.normalize_vectors_ = (metric_ == VexMetric::COSINE);
    RebuildRowIdNodeMap();

    // When samples ≤ ksub (256 for nbits=8) the shared AnnKmeans takes the
    // QuickCenters fast-path (every sample becomes a centroid), so PQ works
    // at any non-empty row count. Compact mode in particular needs PQ to
    // function regardless of how few rows the user has.
    if (pq_m_ > 0 && !row_ids.empty()) {
        TrainAndEncodePQ(src, row_ids);
    }
    if (rabitq_requested_ && !row_ids.empty()) {
        TrainAndEncodeRaBitQ();
    }
    if (compact_mode_ && (pq_use_ || rabitq_use_)) {
        ReleaseRawVectors();
    }
}

bool GraphIndex::UsesRaBitQ() const {
    vex_duck::SharedLockGuard _rg(graph_rwlock_);
    return rabitq_use_;
}

const char *GraphIndex::GetQuantizerName() const {
    vex_duck::SharedLockGuard _rg(graph_rwlock_);
    return rabitq_requested_ ? "rabitq" : (pq_use_ ? "pq" : "none");
}

idx_t GraphIndex::GetRaBitQCodesBytes() const {
    vex_duck::SharedLockGuard _rg(graph_rwlock_);
    return rabitq_codes_.size();
}

idx_t GraphIndex::GetRaBitQFixedBytes() const {
    vex_duck::SharedLockGuard _rg(graph_rwlock_);
    if (!rabitq_use_ || !rabitq_quantizer_) {
        return 0;
    }
    const idx_t padded_dim = RABITQ_PADDED_DIM(dimension_);
    return rabitq_quantizer_->get_random_matrix_size() +
           HNSW_RABITQ_NUM_CLUSTERS * dimension_ * sizeof(float) +
           HNSW_RABITQ_NUM_CLUSTERS * padded_dim * sizeof(float);
}

void GraphIndex::TrainAndEncodeRaBitQ() {
    if (!runtime_ || runtime_->store.elems.empty()) {
        return;
    }

    auto &store = runtime_->store;
    const idx_t node_count = store.elems.size();
    const int padded_dim = RABITQ_PADDED_DIM(static_cast<int>(dimension_));
    auto quantizer = std::make_unique<::rabitq::RaBitQuantizer>(
        static_cast<int>(dimension_), padded_dim, ToRaBitQMetric(metric_));

    // The graph may deduplicate equal vectors, so train and encode by internal
    // node id rather than table row order. This is also the stable search and
    // persistence layout used by PostgreSQL's graph store.
    constexpr size_t kMaxTrainingBytes = 64ULL * 1024 * 1024;
    constexpr idx_t kMaxTrainingSamples = 65536;
    const idx_t samples_by_bytes = std::max<idx_t>(
        HNSW_RABITQ_NUM_CLUSTERS,
        kMaxTrainingBytes / std::max<size_t>(size_t(dimension_) * sizeof(float), 1));
    const idx_t sample_count = std::min(node_count,
        std::min(kMaxTrainingSamples, samples_by_bytes));
    std::vector<float> samples(sample_count * dimension_);
    for (idx_t sample_id = 0; sample_id < sample_count; sample_id++) {
        const idx_t node_id = sample_id * node_count / sample_count;
        const auto *src = reinterpret_cast<const float *>(store.get_data(node_id));
        if (!src) {
            throw InternalException("RaBitQ training found a graph node without a vector");
        }
        std::memcpy(samples.data() + sample_id * dimension_, src,
                    dimension_ * sizeof(float));
    }

    ::vex::quantizer::KMeansState state;
    if (metric_ == VexMetric::L2) {
        state.distance_fn = ann_helper::get_general_distance_func(
            Metric::L2_SQRT, static_cast<uint32>(dimension_));
        state.norm_fn = nullptr;
    } else {
        state.distance_fn = ann_helper::get_general_distance_func(
            Metric::SPHERICAL, static_cast<uint32>(dimension_));
        state.norm_fn = ann_helper::get_general_distance_func(
            Metric::L2_NORM, static_cast<uint32>(dimension_));
    }
    state.skip_check_duplicate = false;

    ::vex::quantizer::PQFloatArray sample_view;
    sample_view.data = samples.data();
    sample_view.length = sample_count;
    sample_view.maxlen = sample_count;
    sample_view.dim = dimension_;

    ::vex::quantizer::PQFloatArray centers;
    centers.data = quantizer->get_centroids();
    centers.length = 0;
    centers.maxlen = HNSW_RABITQ_NUM_CLUSTERS;
    centers.dim = dimension_;

    ::vex::quantizer::PQContext ctx;
    try {
        ::vex::quantizer::AnnKmeans(state, sample_view, centers,
                                    /*avg_work_mem_kb=*/128 * 1024, ctx);
        quantizer->train();
    } catch (const ::vex::quantizer::VexQuantizerError &e) {
        throw InvalidInputException("RaBitQ training failed: %s", e.what());
    } catch (const std::exception &e) {
        throw InvalidInputException("RaBitQ training failed: %s", e.what());
    }

    rabitq_query_rescaling_factor_ = quantizer->get_query_rescaling_factor();
    quantizer->set_rescaling_factor(rabitq_query_rescaling_factor_);
    ::rabitq::CodeDistancer encoder(*quantizer, static_cast<int>(dimension_),
                                    ToRaBitQMetric(metric_),
                                    rabitq_query_rescaling_factor_);
    const size_t code_size = encoder.code_size();
    rabitq_codes_.assign(node_count * code_size, 0);
    for (idx_t id = 0; id < node_count; id++) {
        const auto *src = reinterpret_cast<const float *>(store.get_data(id));
        encoder.compute_code(src,
                             rabitq_codes_.data() + id * code_size);
    }

    rabitq_quantizer_ = std::move(quantizer);
    rabitq_use_ = true;
}

void GraphIndex::ReleaseRawVectors() {
    if (!runtime_) {
        return;
    }
    auto &store = runtime_->store;
    if (store.vector_alloc_) {
        store.vector_alloc_->Reset();
    }
    store.ClearMirrorVectors(/*shrink=*/true);
    store.ReleaseMirrorClaim();
    store.compact_mode_ = true;
}

void GraphIndex::TrainAndEncodePQ(const float *vec_data, const std::vector<row_t> &row_ids) {
    // Default PQContext: per-context deterministic SplitMix64 random,
    // std::malloc/free allocator, serial parallel executor. The shared PQ never sees any
    // duck-specific types — backend swap is purely at this construction site.
    ::vex::quantizer::PQContext ctx;
    struct ParallelTrainConfig {
        size_t max_workers = 1;
    } parallel_config;
    // Inject a thread-pool driver so each of the M sub-quantizer K-means runs
    // on its own std::thread. The shared algorithm writes only into its own
    // [m*ksub*dsub : (m+1)*ksub*dsub] slice of the centroids buffer and uses
    // std::malloc plus a per-task deterministic random state, so no further
    // synchronization is needed. Exceptions from any worker are captured and the first is
    // rethrown after all threads join.
    //
    // Disable knob: set environment variable VEX_PQ_TRAIN_SERIAL=1 to force
    // the serial fallback (debug aid).
    static const bool pq_parallel_train = []() {
        const char *env = std::getenv("VEX_PQ_TRAIN_SERIAL");
        return !(env && env[0] == '1');
    }();
    if (pq_parallel_train) {
        ctx.parallel.run_fn = [](size_t n,
                                  const ::vex::quantizer::PQParallelExecutor::TaskFn &body,
                                  void *user) {
            // K-means inside each subquantizer also calls ctx.parallel.Run with
            // num_samples (~50K) and num_centers (256). Spawning a thread per
            // such task is catastrophic. Only parallelise the small outer loop
            // (M subquantizers, typically ≤ 64). Anything larger runs serial.
            constexpr size_t kMaxParallel = 64;
            if (n == 0) return;
            auto *config = static_cast<ParallelTrainConfig *>(user);
            const size_t max_workers = config ? std::max<size_t>(config->max_workers, 1) : 1;
            if (n == 1 || n > kMaxParallel || max_workers == 1) {
                for (size_t i = 0; i < n; i++) body(i);
                return;
            }
            const size_t worker_count = std::min(n, max_workers);
            std::atomic<size_t> next_task{0};
            std::vector<std::thread> workers;
            workers.reserve(worker_count);
            std::vector<std::exception_ptr> errors(n);
            try {
                for (size_t worker = 0; worker < worker_count; worker++) {
                    workers.emplace_back([&body, &errors, &next_task, n]() {
                        while (true) {
                            const size_t i = next_task.fetch_add(1, std::memory_order_relaxed);
                            if (i >= n) break;
                            try {
                                body(i);
                            } catch (...) {
                                errors[i] = std::current_exception();
                            }
                        }
                    });
                }
            } catch (...) {
                for (auto &t : workers) {
                    if (t.joinable()) t.join();
                }
                throw;
            }
            for (auto &t : workers) t.join();
            for (auto &ep : errors) {
                if (ep) std::rethrow_exception(ep);
            }
        };
        ctx.parallel.user = &parallel_config;
    }
    ::vex::quantizer::KMeansState kmeans_state;
    // Elkan K-means stores metric bounds and relies on the triangle
    // inequality, so it must receive Euclidean distance rather than squared
    // L2.  ProductQuantizer still uses squared L2 for encoding and ADC below.
    kmeans_state.distance_fn = ann_helper::get_general_distance_func(
        Metric::L2_SQRT, static_cast<uint32>(dimension_ / pq_m_));
    kmeans_state.norm_fn     = nullptr;
    // Skip duplicate-center error for tiny test datasets — pq_m=4 + 8-dim
    // synthetic data trips it spuriously and adds no recall protection.
    kmeans_state.skip_check_duplicate = true;

    pq_quantizer_.free_resources(ctx);
    pq_quantizer_.set_basic_values(static_cast<size_t>(dimension_), pq_m_, /*nbits*/8);
    pq_quantizer_.set_derived_values(ctx);
    pq_quantizer_.set_fvec_L2sqr_ny_nearest_func();
    pq_quantizer_.set_fvec_ny_distance_func(Metric::L2);
    pq_quantizer_.set_dist_code_func();

    // Training on every row makes Elkan's N*256 lower-bound table grow without
    // limit. It is also multiplied by the number of concurrently trained
    // subquantizers. Use the same deterministic, byte-bounded sampling contract
    // as SQLite/RaBitQ and cap aggregate scratch memory across workers.
    constexpr size_t kMaxTrainingBytes = 64ULL * 1024 * 1024;
    constexpr idx_t kMaxTrainingSamples = 65536;
    // Keep the process peak bounded as well as the training sample itself.
    // At 65,536 samples every Elkan worker needs about 64 MiB for its
    // N*ksub bound table. Four workers made a 100k-vector PQ build retain
    // roughly 500 MiB RSS after training on common allocators. Two workers
    // keep useful subquantizer parallelism without letting scratch dominate
    // the graph and code storage.
    constexpr size_t kParallelScratchBudget = 128ULL * 1024 * 1024;
    const idx_t samples_by_bytes = std::max<idx_t>(
        256, kMaxTrainingBytes /
                 std::max<size_t>(size_t(dimension_) * sizeof(float), 1));
    const idx_t sample_count = std::min<idx_t>(
        row_ids.size(), std::min<idx_t>(kMaxTrainingSamples, samples_by_bytes));
    std::vector<float> training_samples(sample_count * dimension_);
    for (idx_t sample_id = 0; sample_id < sample_count; sample_id++) {
        const idx_t source_id = sample_id * row_ids.size() / sample_count;
        std::memcpy(training_samples.data() + sample_id * dimension_,
                    vec_data + source_id * dimension_,
                    dimension_ * sizeof(float));
    }
    const size_t per_worker_scratch =
        std::max<size_t>(sample_count * pq_quantizer_.ksub * sizeof(float), 1);
    parallel_config.max_workers = std::max<size_t>(
        1, std::min<size_t>(pq_quantizer_.M,
                            kParallelScratchBudget / per_worker_scratch));

    // Wrap the sampled raw-float buffer as PQFloatArray (non-owning).
    ::vex::quantizer::PQFloatArray samples;
    samples.data   = training_samples.data();
    samples.length = sample_count;
    samples.maxlen = sample_count;
    samples.dim    = dimension_;

    try {
        pq_quantizer_.train(kmeans_state, samples, /*avg_work_mem_kb=*/128 * 1024, ctx);
    } catch (const ::vex::quantizer::VexQuantizerError &e) {
        pq_quantizer_.free_resources(ctx);
        throw InvalidInputException("PQ training failed: %s", e.what());
    }
    if (!pq_quantizer_.trained) {
        return;
    }
    pq_use_ = true;

    // Index codes by ascending row_id so the layout is stable across reloads.
    pq_row_id_order_ = row_ids;
    std::sort(pq_row_id_order_.begin(), pq_row_id_order_.end());

    std::unordered_map<row_t, idx_t> rid_to_idx;
    rid_to_idx.reserve(row_ids.size());
    for (idx_t i = 0; i < row_ids.size(); i++) {
        rid_to_idx[row_ids[i]] = i;
    }

    auto code_size = pq_quantizer_.code_size;
    pq_codes_.assign(pq_row_id_order_.size() * code_size, 0);
    pq_vector_coverage_hashes_.assign(pq_row_id_order_.size(), 0);
    for (idx_t i = 0; i < pq_row_id_order_.size(); i++) {
        auto src_idx = rid_to_idx[pq_row_id_order_[i]];
        auto rid = pq_row_id_order_[i];
        const auto *vec = vec_data + src_idx * dimension_;
        pq_quantizer_.compute_code(vec, pq_codes_.data() + i * code_size);
        pq_vector_coverage_hashes_[i] = HashCoverageRowVector(rid, vec, dimension_);
    }
    RebuildPQLatestCodePositions();
    RebuildPQNodeCodePositions();
}

void GraphIndex::RebuildPQLatestCodePositions() {
    pq_latest_code_positions_.clear();
    pq_latest_code_positions_.reserve(pq_row_id_order_.size());
    for (idx_t position = 0; position < pq_row_id_order_.size(); position++) {
        pq_latest_code_positions_[pq_row_id_order_[position]] =
            static_cast<uint32_t>(position);
    }
}

void GraphIndex::RecordRowIdNode(row_t row_id, uint32_t node_id) {
    if (!runtime_ || node_id >= runtime_->store.elems.size()) {
        throw InternalException("graph insert did not publish a valid node id");
    }
    auto [it, inserted] = rowid_node_map_.emplace(row_id, node_id);
    if (inserted || it->second == node_id) {
        return;
    }
    auto &duplicates = duplicate_rowid_nodes_[row_id];
    if (std::find(duplicates.begin(), duplicates.end(), it->second) == duplicates.end()) {
        duplicates.push_back(it->second);
    }
    it->second = node_id;
}

void GraphIndex::RebuildRowIdNodeMap() {
    rowid_node_map_.clear();
    duplicate_rowid_nodes_.clear();
    if (!runtime_) {
        return;
    }
    const auto &elems = runtime_->store.elems;
    rowid_node_map_.reserve(elems.size());
    for (idx_t node_id = 0; node_id < elems.size(); node_id++) {
        for (const auto &tid : elems[node_id].tids) {
            RecordRowIdNode(tid.row_id, static_cast<uint32_t>(node_id));
        }
    }
}

bool GraphIndex::IsLatestPQCodePosition(idx_t position) const {
    if (position >= pq_row_id_order_.size()) return false;
    auto it = pq_latest_code_positions_.find(pq_row_id_order_[position]);
    return it != pq_latest_code_positions_.end() && it->second == position;
}

void GraphIndex::RetireDeletedRowVersion(row_t row_id) {
    if (!runtime_ || deleted_rids_.find(row_id) == deleted_rids_.end()) {
        return;
    }

    auto current = rowid_node_map_.find(row_id);
    if (current == rowid_node_map_.end()) {
        // A NULL vector has no graph node, so NULL -> vector UPDATE reaches
        // this branch legitimately. Bulk build and recovery eagerly rebuild
        // the map; an absent key therefore means there is nothing to retire,
        // not that an O(N) repair scan is needed.
        return;
    }

    std::vector<uint32_t> node_ids;
    node_ids.push_back(current->second);
    auto duplicates = duplicate_rowid_nodes_.find(row_id);
    if (duplicates != duplicate_rowid_nodes_.end()) {
        node_ids.insert(node_ids.end(), duplicates->second.begin(), duplicates->second.end());
    }

    auto &store = runtime_->store;
    LWLockAcquire(&store.elems_veclock, LW_SHARED);
    {
        std::unique_lock<std::shared_mutex> tids_lock(GraphIndexPoint::tid_lock());
        for (auto node_id : node_ids) {
            if (node_id >= store.elems.size()) {
                continue;
            }
            auto &tids = store.elems[node_id].tids;
            const auto old_size = tids.size();
            tids.erase(std::remove_if(tids.begin(), tids.end(), [&](const auto &tid) {
                return tid.row_id == row_id;
            }), tids.end());
            if (tids.size() == old_size || !store.node_alloc_) {
                continue;
            }
            auto *header = store.GetNodeHeader(static_cast<uint32_t>(node_id));
            if (!header) continue;
            if (tids.empty()) {
                header->deleted = 1;
                header->extra_row_count = 0;
            } else {
                header->deleted = 0;
                header->row_id = tids.back().row_id;
                header->extra_row_count = static_cast<uint16_t>(
                    std::min<size_t>(tids.size() - 1,
                                     std::numeric_limits<uint16_t>::max()));
            }
        }
    }
    LWLockRelease(&store.elems_veclock);
    rowid_node_map_.erase(row_id);
    duplicate_rowid_nodes_.erase(row_id);
}

void GraphIndex::RebuildPQNodeCodePositions() {
    pq_node_code_positions_.clear();
    if (!runtime_ || !pq_use_ || pq_quantizer_.code_size == 0) {
        return;
    }
    if (pq_codes_.size() != pq_row_id_order_.size() * pq_quantizer_.code_size) {
        throw InvalidInputException("PQ code bytes do not match row order");
    }

    if (pq_row_id_order_.size() >= PQ_INVALID_CODE_POSITION) {
        throw InvalidInputException("PQ code count exceeds the supported 32-bit graph mapping");
    }
    std::unordered_map<row_t, uint32_t> code_by_row_id;
    code_by_row_id.reserve(pq_row_id_order_.size());
    for (idx_t position = 0; position < pq_row_id_order_.size(); position++) {
        code_by_row_id[pq_row_id_order_[position]] = static_cast<uint32_t>(position);
    }

    const auto &elems = runtime_->store.elems;
    pq_node_code_positions_.assign(elems.size(), PQ_INVALID_CODE_POSITION);
    for (idx_t node_id = 0; node_id < elems.size(); node_id++) {
        for (const auto &tid : elems[node_id].tids) {
            auto it = code_by_row_id.find(tid.row_id);
            if (it != code_by_row_id.end()) {
                pq_node_code_positions_[node_id] = it->second;
                break;
            }
        }
        if (pq_node_code_positions_[node_id] == PQ_INVALID_CODE_POSITION) {
            throw InvalidInputException(
                "PQ row/code metadata does not cover graph node %llu",
                static_cast<unsigned long long>(node_id));
        }
    }
}

void GraphIndex::SearchPQ(const float *query_vec, idx_t k, int ef,
                          std::vector<row_t> &row_ids, std::vector<float> &distances,
                          double refine_factor) const {
    row_ids.clear();
    distances.clear();
    if (!pq_use_ || pq_codes_.empty() || !runtime_ || k == 0) {
        return;
    }
    vex_duck::SharedLockGuard _rg(graph_rwlock_);
    auto &store = runtime_->store;
    if (pq_node_code_positions_.size() != store.elems.size()) {
        throw InternalException("PQ node/code mapping is unavailable; rebuild the index");
    }

    const idx_t target_k = k;
    const bool refine = refine_factor > 1.0 && !compact_mode_;
    const idx_t candidate_k = refine
        ? std::min<idx_t>(static_cast<idx_t>(std::ceil(static_cast<double>(k) * refine_factor)),
                          pq_row_id_order_.size())
        : k;

    std::vector<float> query_buf;
    const float *query = query_vec;
    if (metric_ == VexMetric::COSINE) {
        query_buf.assign(query_vec, query_vec + dimension_);
        NormalizeInPlace(query_buf.data(), dimension_);
        query = query_buf.data();
    }

    auto *self = const_cast<::vex::quantizer::ProductQuantizer *>(&pq_quantizer_);
    DuckPQDistancer distancer(*self);
    distancer.process(query);
    const bool has_deleted = !deleted_rids_.empty();
    std::vector<std::pair<float, row_t>> candidates;
    idx_t estimated_live_codes = pq_latest_code_positions_.size();
    if (has_deleted) {
        for (auto rid : deleted_rids_) {
            if (pq_latest_code_positions_.find(rid) != pq_latest_code_positions_.end() &&
                estimated_live_codes > 0) {
                estimated_live_codes--;
            }
        }
    }
    if (k >= estimated_live_codes) {
        // Returning the whole relation is necessarily O(N). Scan the persisted
        // row-oriented codes here so a large LIMIT cannot lose graph nodes that
        // are not reached by the ANN walk. Normal top-k queries stay on the
        // bounded graph path below.
        candidates.reserve(pq_row_id_order_.size());
        for (idx_t code_position = 0; code_position < pq_row_id_order_.size(); code_position++) {
            if (!IsLatestPQCodePosition(code_position)) {
                continue;
            }
            const row_t rid = pq_row_id_order_[code_position];
            if (has_deleted && deleted_rids_.find(rid) != deleted_rids_.end()) {
                continue;
            }
            const auto *code = pq_codes_.data() + code_position * pq_quantizer_.code_size;
            candidates.emplace_back(
                distancer.get_distance_single(nullptr, code, 0), rid);
        }
    } else {
        DuckQuantizedSearchStore quantized_store(
            store, pq_codes_, pq_quantizer_.code_size, &pq_node_code_positions_);
        GraphIndexAlgorithm<DuckQuantizedSearchStore, DuckPQDistancer> algo(
            static_cast<uint_fast16_t>(ef_construction_), static_cast<uint_fast16_t>(m_),
            quantized_store, distancer);

        store.search_lock_free_.store(true, std::memory_order_release);
        const idx_t base_search_width =
            std::max<idx_t>(candidate_k, std::max<idx_t>(k, static_cast<idx_t>(ef)));
        // This topology was built with raw-vector distances while traversal uses
        // noisier ADC distances. Keep a bounded 2x candidate window so the two
        // orderings can diverge without dropping close neighbours at the boundary.
        // The work remains O(ef), independent of the total number of PQ codes.
        idx_t needed = base_search_width * 2;
        if (has_deleted) {
            needed += deleted_rids_.size();
        }
        const auto search_k = static_cast<uint_fast16_t>(
            std::min<idx_t>(needed, std::numeric_limits<uint_fast16_t>::max()));
        PointExtensionContext point_ctx;
        auto results = algo.search(point_ctx, reinterpret_cast<const char *>(query), search_k);
        VtlDestroyGuard results_guard(results);

        candidates.reserve(results.size());
        for (idx_t i = 0; i < results.size(); i++) {
            const row_t rid = results[i].tid.row_id;
            if (has_deleted && deleted_rids_.find(rid) != deleted_rids_.end()) {
                continue;
            }
            candidates.emplace_back(results[i].dist, rid);
        }
    }
    // PQ commonly gives adjacent vectors the same code distance. Graph walk
    // order is not a stable tie-breaker, so make equal-distance results
    // deterministic and consistent with the former row-oriented ADC scan.
    std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
        return a.first < b.first || (a.first == b.first && a.second < b.second);
    });
    if (candidates.size() > candidate_k) {
        candidates.resize(candidate_k);
    }

    if (refine && !candidates.empty()) {
        // Re-rank top k*factor candidates using exact raw-vector distance.
        // The same eager row_id → node map used by UPDATE supplies the raw
        // vector location, so refine never pays a first-query O(N) scan.
        std::vector<float> refine_query;
        const float *rq = query_vec;
        if (metric_ == VexMetric::COSINE) {
            refine_query.assign(query_vec, query_vec + dimension_);
            NormalizeInPlace(refine_query.data(), dimension_);
            rq = refine_query.data();
        }
        std::vector<std::pair<float, row_t>> exact;
        exact.reserve(candidates.size());
        for (auto &e : candidates) {
            auto it = rowid_node_map_.find(e.second);
            if (it == rowid_node_map_.end()) continue;
            const auto *raw = reinterpret_cast<const float *>(runtime_->store.get_data(it->second));
            if (!raw) continue;
            float d = 0.0f;
            for (idx_t j = 0; j < dimension_; j++) {
                float diff = rq[j] - raw[j];
                d += diff * diff;
            }
            exact.emplace_back(d, e.second);
        }
        std::partial_sort(exact.begin(),
                          exact.begin() + std::min<size_t>(target_k, exact.size()),
                          exact.end(),
                          [](const auto &a, const auto &b) { return a.first < b.first; });
        const size_t out = std::min<size_t>(target_k, exact.size());
        row_ids.reserve(out);
        distances.reserve(out);
        for (size_t i = 0; i < out; i++) {
            row_ids.push_back(exact[i].second);
            distances.push_back(exact[i].first);
        }
        return;
    }

    const size_t out = std::min<size_t>(target_k, candidates.size());
    row_ids.reserve(out);
    distances.reserve(out);
    for (size_t i = 0; i < out; i++) {
        auto &e = candidates[i];
        row_ids.push_back(e.second);
        distances.push_back(e.first);
    }
}

void GraphIndex::SearchRaBitQ(const float *query_vec, idx_t k, int ef,
                              std::vector<row_t> &row_ids,
                              std::vector<float> &distances) const {
    row_ids.clear();
    distances.clear();
    const size_t code_size = ::rabitq::CodeSize(static_cast<int>(dimension_));

    std::vector<float> normalized_query;
    const float *query = query_vec;
    if (metric_ == VexMetric::COSINE) {
        normalized_query.assign(query_vec, query_vec + dimension_);
        NormalizeInPlace(normalized_query.data(), dimension_);
        query = normalized_query.data();
    }

    auto convert_distance = [&](float distance) {
        if (metric_ == VexMetric::L2) {
            return std::sqrt(std::max(0.0f, distance));
        }
        if (metric_ == VexMetric::INNER_PRODUCT) {
            return distance - 1.0f;
        }
        return distance;
    };

    auto do_full_scan = [&](auto &store, bool has_deleted) {
        ::rabitq::CodeDistancer distancer(*rabitq_quantizer_,
                                          static_cast<int>(dimension_),
                                          ToRaBitQMetric(metric_),
                                          rabitq_query_rescaling_factor_);
        distancer.process(query);
        std::vector<std::pair<float, row_t>> candidates;
        candidates.reserve(store.elems.size());
        for (idx_t node_id = 0; node_id < store.elems.size(); node_id++) {
            const auto *code = rabitq_codes_.data() + node_id * code_size;
            const float distance = distancer.get_distance_single(nullptr, code, 0);
            for (const auto &tid : store.elems[node_id].tids) {
                if (has_deleted && deleted_rids_.find(tid.row_id) != deleted_rids_.end()) {
                    continue;
                }
                candidates.emplace_back(distance, tid.row_id);
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
            return a.first < b.first || (a.first == b.first && a.second < b.second);
        });
        const size_t out = std::min<size_t>(k, candidates.size());
        row_ids.reserve(out);
        distances.reserve(out);
        for (size_t i = 0; i < out; i++) {
            row_ids.push_back(candidates[i].second);
            distances.push_back(convert_distance(candidates[i].first));
        }
    };

    auto do_search = [&](auto &store, uint_fast16_t search_k, bool has_deleted) {
        if (rabitq_codes_.size() != store.elems.size() * code_size) {
            throw InternalException("RaBitQ code coverage does not match graph nodes");
        }
        ::rabitq::CodeDistancer distancer(*rabitq_quantizer_,
                                          static_cast<int>(dimension_),
                                          ToRaBitQMetric(metric_),
                                          rabitq_query_rescaling_factor_);
        distancer.process(query);
        DuckQuantizedSearchStore quantized_store(store, rabitq_codes_, code_size);
        GraphIndexAlgorithm<DuckQuantizedSearchStore, ::rabitq::CodeDistancer> algo(
            static_cast<uint_fast16_t>(ef_construction_),
            static_cast<uint_fast16_t>(m_), quantized_store, distancer);

        store.search_lock_free_.store(true, std::memory_order_release);
        PointExtensionContext point_ctx;
        auto results = algo.search(point_ctx, reinterpret_cast<const char *>(query), search_k);
        VtlDestroyGuard results_guard(results);
        for (idx_t i = 0; i < results.size() && row_ids.size() < k; i++) {
            const row_t rid = results[i].tid.row_id;
            if (has_deleted && deleted_rids_.find(rid) != deleted_rids_.end()) {
                continue;
            }
            float distance = convert_distance(results[i].dist);
            // Cosine inputs are normalized before encoding/search, so 1-dot is
            // already DuckDB's public cosine distance.
            row_ids.push_back(rid);
            distances.push_back(distance);
        }
    };

    UnorderedSet<size_t> deleted_internal;
    {
        vex_duck::SharedLockGuard _rg(graph_rwlock_);
        if (!runtime_ || !rabitq_use_ || !rabitq_quantizer_ || rabitq_codes_.empty()) {
            return;
        }
        auto &store = runtime_->store;
        const bool has_deleted = !deleted_rids_.empty();
        const idx_t live_upper_bound =
            store.elems.size() > deleted_rids_.size()
                ? store.elems.size() - deleted_rids_.size()
                : 0;
        if (k >= live_upper_bound) {
            do_full_scan(store, has_deleted);
            return;
        }
        idx_t needed = std::max<idx_t>(k, static_cast<idx_t>(ef));
        if (has_deleted) {
            needed += deleted_rids_.size();
        }
        bool entry_deleted = false;
        if (has_deleted && store.entry_info.id != INVALID_VECTOR_ID &&
            store.entry_info.id < store.elems.size()) {
            for (auto &tid : store.elems[store.entry_info.id].tids) {
                if (deleted_rids_.find(tid.row_id) != deleted_rids_.end()) {
                    entry_deleted = true;
                    break;
                }
            }
        }
        const auto search_k = static_cast<uint_fast16_t>(
            std::min<idx_t>(needed, std::numeric_limits<uint_fast16_t>::max()));
        if (!entry_deleted) {
            do_search(store, search_k, has_deleted);
            return;
        }
        for (size_t id = 0; id < store.elems.size(); id++) {
            for (auto &tid : store.elems[id].tids) {
                if (deleted_rids_.find(tid.row_id) != deleted_rids_.end()) {
                    deleted_internal.insert(id);
                    break;
                }
            }
        }
    }

    {
        vex_duck::ExclusiveLockGuard _wg(graph_rwlock_);
        if (!runtime_) {
            return;
        }
        auto &store = runtime_->store;
        RunWithDuckAlgo(metric_, dimension_, ef_construction_, m_, store, [&](auto &algo) {
            algo.repair_entry(deleted_internal);
        });
    }
    {
        vex_duck::SharedLockGuard _rg(graph_rwlock_);
        if (!runtime_ || !rabitq_use_ || !rabitq_quantizer_ || rabitq_codes_.empty()) {
            return;
        }
        auto &store = runtime_->store;
        const bool has_deleted = !deleted_rids_.empty();
        const idx_t live_upper_bound =
            store.elems.size() > deleted_rids_.size()
                ? store.elems.size() - deleted_rids_.size()
                : 0;
        if (k >= live_upper_bound) {
            do_full_scan(store, has_deleted);
            return;
        }
        idx_t needed = std::max<idx_t>(k, static_cast<idx_t>(ef));
        if (has_deleted) {
            needed += deleted_rids_.size();
        }
        const auto search_k = static_cast<uint_fast16_t>(
            std::min<idx_t>(needed, std::numeric_limits<uint_fast16_t>::max()));
        do_search(store, search_k, has_deleted);
    }
}

void GraphIndex::SearchANN(const float *query_vec, idx_t k, int ef, std::vector<row_t> &row_ids,
                           std::vector<float> &distances) const {
    row_ids.clear();
    distances.clear();
    PointExtensionContext point_ctx;

    // The actual HNSW walk + deleted-row filtering. MUST be called with
    // graph_rwlock_ held shared (reads store + deleted_rids_, both mutated by
    // writers under the exclusive lock). Factored out so the common path can run
    // it under the SAME shared lock as the deleted-entry detection — keeping the
    // hot search path at a single index-lock acquire (QPS-neutral vs the original).
    auto do_search = [&](auto &store, uint_fast16_t search_k, bool has_deleted) {
        // searches hold graph_rwlock_ shared, writers take it exclusive, so the
        // per-node reader lock inside the HNSW walk is redundant. Skip it to avoid
        // hub-node reader-byte cacheline contention under high read concurrency.
        // Safe only while the shared lock is held; the flag is set under it too.
        store.search_lock_free_.store(true, std::memory_order_release);
        RunWithDuckAlgo(metric_, dimension_, ef_construction_, m_, store, [&](auto &algo) {
            auto res = algo.search(point_ctx, reinterpret_cast<const char *>(query_vec), search_k);
            VtlDestroyGuard results_guard(res);
            for (idx_t i = 0; i < res.size() && row_ids.size() < k; i++) {
                row_t rid = res[i].tid.row_id;
                if (has_deleted && deleted_rids_.find(rid) != deleted_rids_.end()) {
                    continue;
                }
                row_ids.push_back(rid);
                distances.push_back(res[i].dist);
            }
        });
    };

    // deleted_rids_ + store.elems are mutated by writers (Append/Delete) under
    // graph_rwlock_ exclusive, so every read of them must hold the lock shared.
    // Common path: ONE shared lock spans deleted detection AND the search.
    UnorderedSet<size_t> deleted_internal;  // only filled on the rare repair path
    {
        vex_duck::SharedLockGuard _rg(graph_rwlock_);
        if (!runtime_) {
            return;
        }
        if (compact_mode_) {
            throw InvalidInputException(
                "GRAPH_INDEX memory_mode='compact': the raw-vector search path is unavailable; "
                "use the active quantizer search path");
        }
        auto &store = runtime_->store;
        const bool has_deleted = !deleted_rids_.empty();
        idx_t needed = std::max<idx_t>(k, static_cast<idx_t>(ef));
        if (has_deleted) {
            needed += deleted_rids_.size();
        }
        // If the graph entry node has been deleted, search starting from it can wander
        // into a stale subgraph (its neighbor links may all point to other deleted
        // nodes). Detect that case and let the algorithm pick a fresh entry from the
        // upper layers before searching. Building the internal-id deleted set is O(N)
        // so we only do it when the entry is actually deleted, which is rare.
        bool entry_deleted = false;
        if (has_deleted && store.entry_info.id != INVALID_VECTOR_ID &&
            store.entry_info.id < store.elems.size()) {
            for (auto &tid : store.elems[store.entry_info.id].tids) {
                if (deleted_rids_.find(tid.row_id) != deleted_rids_.end()) {
                    entry_deleted = true;
                    break;
                }
            }
        }
        auto search_k = uint_fast16_t(std::min<idx_t>(needed, std::numeric_limits<uint_fast16_t>::max()));
        if (!entry_deleted) {
            // Common path: detection + search under the same single shared lock.
            do_search(store, search_k, has_deleted);
            return;
        }
        // Rare path: entry node deleted — collect the internal-id deleted set under
        // the shared lock, then repair under exclusive (below).
        for (size_t id = 0; id < store.elems.size(); id++) {
            for (auto &tid : store.elems[id].tids) {
                if (deleted_rids_.find(tid.row_id) != deleted_rids_.end()) {
                    deleted_internal.insert(id);
                    break;
                }
            }
        }
    }

    // Rare path only: repair the entry under the exclusive lock, then search.
    {
        vex_duck::ExclusiveLockGuard _wg(graph_rwlock_);
        if (!runtime_) {
            return;
        }
        auto &store = runtime_->store;
        RunWithDuckAlgo(metric_, dimension_, ef_construction_, m_, store, [&](auto &algo) {
            algo.repair_entry(deleted_internal);
        });
    }
    {
        vex_duck::SharedLockGuard _rg(graph_rwlock_);
        if (!runtime_) {
            return;
        }
        if (compact_mode_) {
            throw InvalidInputException(
                "GRAPH_INDEX memory_mode='compact': the raw-vector search path is unavailable; "
                "use the active quantizer search path");
        }
        auto &store = runtime_->store;
        const bool has_deleted = !deleted_rids_.empty();
        idx_t needed = std::max<idx_t>(k, static_cast<idx_t>(ef));
        if (has_deleted) {
            needed += deleted_rids_.size();
        }
        auto search_k = uint_fast16_t(std::min<idx_t>(needed, std::numeric_limits<uint_fast16_t>::max()));
        do_search(store, search_k, has_deleted);
    }
}

ErrorData GraphIndex::Append(IndexLock &l, DataChunk &chunk, Vector &row_ids) {
    (void)l;
    auto count = chunk.size();
    if (count == 0) {
        return ErrorData();
    }

    // Mutates the graph: exclude all concurrent (shared) searches so the
    // lock-free search read path stays safe.
    vex_duck::ExclusiveLockGuard _wg(graph_rwlock_);

    if (!runtime_) {
        runtime_ = make_uniq<GraphIndexRuntimeState>(dimension_, m_, Allocator::Get(db));
        runtime_->store.normalize_vectors_ = (metric_ == VexMetric::COSINE);
    }
    if (!runtime_->store.node_alloc_ || !runtime_->store.upper_alloc_ ||
        (!compact_mode_ && !runtime_->store.vector_alloc_)) {
        runtime_->store.InitAllocators(table_io_manager.GetIndexBlockManager());
        ApplyMirrorBudget();
    }

    if (column_ids.empty()) {
        return ErrorData(ExceptionType::INTERNAL, "GRAPH_INDEX has no indexed columns");
    }
    if (vec_column_index_ >= column_ids.size()) {
        return ErrorData(ExceptionType::INTERNAL, "GRAPH_INDEX vec column index out of range");
    }
    auto vec_col_idx = static_cast<idx_t>(column_ids[vec_column_index_]);
    if (vec_col_idx >= chunk.ColumnCount()) {
        return ErrorData(ExceptionType::INTERNAL,
                         StringUtil::Format("GRAPH_INDEX column index out of range: %llu (chunk columns=%llu)",
                                            static_cast<unsigned long long>(vec_col_idx),
                                            static_cast<unsigned long long>(chunk.ColumnCount())));
    }

    auto &vec_vector = chunk.data[vec_col_idx];
    auto &vec_type = vec_vector.GetType();
    if (vec_type.id() != LogicalTypeId::ARRAY || ArrayType::GetChildType(vec_type).id() != LogicalTypeId::FLOAT) {
        return ErrorData(ExceptionType::INVALID_INPUT,
                         StringUtil::Format("GRAPH_INDEX column must be FLOAT[N], got %s", vec_type.ToString()));
    }

    auto dim = ArrayType::GetSize(vec_type);
    if (dim != dimension_) {
        return ErrorData(ExceptionType::INVALID_INPUT,
                         StringUtil::Format("GRAPH_INDEX dimension mismatch: expected %llu, got %llu",
                                            static_cast<unsigned long long>(dimension_),
                                            static_cast<unsigned long long>(dim)));
    }

    vec_vector.Flatten(count);
    row_ids.Flatten(count);

    auto &vec_validity = FlatVector::Validity(vec_vector);
    auto &child_vec = ArrayVector::GetEntry(vec_vector);
    child_vec.Flatten(count * dim);
    auto vec_data = FlatVector::GetData<float>(child_vec);
    auto row_id_data = FlatVector::GetData<row_t>(row_ids);

    const bool pq_active = pq_use_;
    const bool pq_normalize = pq_active && metric_ == VexMetric::COSINE;
    const auto pq_code_size = pq_active ? pq_quantizer_.code_size : 0;
    const bool track_pq_vector_hashes = pq_active && pq_vector_coverage_hashes_.size() == pq_row_id_order_.size();
    const bool rabitq_active = rabitq_use_ && rabitq_quantizer_;
    const size_t rabitq_code_size = rabitq_active
        ? ::rabitq::CodeSize(static_cast<int>(dim))
        : 0;

    std::vector<float> pq_norm_buf;
    if (pq_normalize || (rabitq_active && metric_ == VexMetric::COSINE)) {
        pq_norm_buf.resize(dim);
    }
    if (pq_active) {
        pq_codes_.reserve(pq_codes_.size() + count * pq_code_size);
        pq_row_id_order_.reserve(pq_row_id_order_.size() + count);
        if (track_pq_vector_hashes) {
            pq_vector_coverage_hashes_.reserve(pq_vector_coverage_hashes_.size() + count);
        }
    }
    if (rabitq_active) {
        rabitq_codes_.reserve(rabitq_codes_.size() + count * rabitq_code_size);
    }

    // From this point on the chunk can mutate graph/code state. Keep the stale
    // marker set until every row and its side metadata have been published.
    MarkRowIdCoverageChecked(true);

    if (compact_mode_ && pq_active) {
        auto &store = runtime_->store;
        if (pq_node_code_positions_.size() != store.elems.size()) {
            return ErrorData(ExceptionType::INTERNAL,
                             "PQ node/code mapping is unavailable; rebuild the index");
        }
        DuckPQDistancer distancer(pq_quantizer_);
        DuckQuantizedSearchStore code_store(
            store, pq_codes_, pq_code_size, &pq_node_code_positions_);
        GraphIndexAlgorithm<DuckQuantizedSearchStore, DuckPQDistancer> algo(
            static_cast<uint_fast16_t>(ef_construction_),
            static_cast<uint_fast16_t>(m_), code_store, distancer);

        for (idx_t i = 0; i < count; i++) {
            if (!vec_validity.RowIsValid(i)) {
                continue;
            }
            PointExtensionContext point_ctx;
            ItemPointerData tid;
            tid.row_id = row_id_data[i];
            RetireDeletedRowVersion(tid.row_id);
            if (!deleted_rids_.empty()) {
                deleted_rids_.erase(tid.row_id);
            }
            const float *encode_src = vec_data + i * dim;
            if (pq_normalize) {
                std::memcpy(pq_norm_buf.data(), encode_src, dim * sizeof(float));
                NormalizeInPlace(pq_norm_buf.data(), dim);
                encode_src = pq_norm_buf.data();
            }
            std::vector<uint8_t> pending_code(pq_code_size);
            pq_quantizer_.compute_code(encode_src, pending_code.data());
            if (pq_row_id_order_.size() >= PQ_INVALID_CODE_POSITION) {
                return ErrorData(ExceptionType::INVALID_INPUT,
                                 "PQ code count exceeds the supported 32-bit graph mapping");
            }
            const auto code_position = static_cast<uint32_t>(pq_row_id_order_.size());
            pq_codes_.insert(pq_codes_.end(), pending_code.begin(), pending_code.end());
            pq_row_id_order_.push_back(tid.row_id);
            pq_latest_code_positions_[tid.row_id] = code_position;
            if (track_pq_vector_hashes) {
                pq_vector_coverage_hashes_.push_back(
                    HashCoverageRowVector(tid.row_id, encode_src, dim));
            }
            code_store.SetPendingCodePosition(code_position);
            distancer.process(encode_src);
            typename decltype(algo)::InsertContextBase insert_ctx(
                point_ctx, reinterpret_cast<const char *>(pending_code.data()), &tid);
            algo.insert(insert_ctx);
            RecordRowIdNode(tid.row_id, static_cast<uint32_t>(insert_ctx.result_id));
        }
    } else if (compact_mode_ && rabitq_active) {
        auto &store = runtime_->store;
        ::rabitq::CodeDistancer distancer(*rabitq_quantizer_,
                                          static_cast<int>(dim),
                                          ToRaBitQMetric(metric_),
                                          rabitq_query_rescaling_factor_);
        DuckQuantizedSearchStore code_store(store, rabitq_codes_, rabitq_code_size);
        GraphIndexAlgorithm<DuckQuantizedSearchStore, ::rabitq::CodeDistancer> algo(
            static_cast<uint_fast16_t>(ef_construction_),
            static_cast<uint_fast16_t>(m_), code_store, distancer);

        for (idx_t i = 0; i < count; i++) {
            if (!vec_validity.RowIsValid(i)) {
                continue;
            }
            PointExtensionContext point_ctx;
            ItemPointerData tid;
            tid.row_id = row_id_data[i];
            RetireDeletedRowVersion(tid.row_id);
            if (!deleted_rids_.empty()) {
                deleted_rids_.erase(tid.row_id);
            }
            const float *raw_vec = vec_data + i * dim;
            const float *encode_src = raw_vec;
            if (metric_ == VexMetric::COSINE) {
                std::memcpy(pq_norm_buf.data(), raw_vec, dim * sizeof(float));
                NormalizeInPlace(pq_norm_buf.data(), dim);
                encode_src = pq_norm_buf.data();
            }
            std::vector<uint8_t> pending_code(rabitq_code_size);
            ::rabitq::EncodeCode(*rabitq_quantizer_, static_cast<int>(dim),
                                 encode_src, pending_code.data());
            distancer.process(encode_src);
            typename decltype(algo)::InsertContextBase insert_ctx(
                point_ctx, reinterpret_cast<const char *>(pending_code.data()), &tid);
            algo.insert(insert_ctx);
            RecordRowIdNode(tid.row_id, static_cast<uint32_t>(insert_ctx.result_id));
        }
    } else {
    RunWithDuckAlgo(metric_, dim, ef_construction_, m_, runtime_->store, [&](auto &algo) {
        using AlgoT = std::decay_t<decltype(algo)>;
        for (idx_t i = 0; i < count; i++) {
            if (!vec_validity.RowIsValid(i)) {
                continue;
            }
            PointExtensionContext point_ctx;
            ItemPointerData tid;
            tid.row_id = row_id_data[i];
            RetireDeletedRowVersion(tid.row_id);
            if (!deleted_rids_.empty()) {
                deleted_rids_.erase(tid.row_id);
            }
            const float *vec_ptr = vec_data + i * dim;
            std::vector<uint8_t> pending_rabitq_code;
            if (rabitq_active) {
                const float *encode_src = vec_ptr;
                if (metric_ == VexMetric::COSINE) {
                    std::memcpy(pq_norm_buf.data(), vec_ptr, dim * sizeof(float));
                    NormalizeInPlace(pq_norm_buf.data(), dim);
                    encode_src = pq_norm_buf.data();
                }
                pending_rabitq_code.resize(rabitq_code_size);
                ::rabitq::EncodeCode(*rabitq_quantizer_, static_cast<int>(dim),
                                     encode_src, pending_rabitq_code.data());
            }
            const size_t old_node_count = runtime_->store.elems.size();
            const char *query = reinterpret_cast<const char *>(vec_ptr);
            typename AlgoT::InsertContextBase insert_ctx(point_ctx, query, &tid);
            algo.insert(insert_ctx);
            RecordRowIdNode(tid.row_id, static_cast<uint32_t>(insert_ctx.result_id));
            if (rabitq_active && runtime_->store.elems.size() > old_node_count) {
                rabitq_codes_.insert(rabitq_codes_.end(),
                                    pending_rabitq_code.begin(), pending_rabitq_code.end());
            }

            if (pq_active) {
                // Historical slots stay append-only for persistence
                // compatibility. pq_latest_code_positions_ decides visibility.
                const float *encode_src = vec_ptr;
                if (pq_normalize) {
                    std::memcpy(pq_norm_buf.data(), vec_ptr, dim * sizeof(float));
                    NormalizeInPlace(pq_norm_buf.data(), dim);
                    encode_src = pq_norm_buf.data();
                }
                if (pq_row_id_order_.size() >= PQ_INVALID_CODE_POSITION) {
                    throw InvalidInputException(
                        "PQ code count exceeds the supported 32-bit graph mapping");
                }
                const auto code_position = static_cast<uint32_t>(pq_row_id_order_.size());
                size_t off = pq_codes_.size();
                pq_codes_.resize(off + pq_code_size);
                pq_quantizer_.compute_code(encode_src, pq_codes_.data() + off);
                pq_row_id_order_.push_back(tid.row_id);
                pq_latest_code_positions_[tid.row_id] = code_position;
                if (track_pq_vector_hashes) {
                    pq_vector_coverage_hashes_.push_back(HashCoverageRowVector(tid.row_id, encode_src, dim));
                }
                if (runtime_->store.elems.size() > old_node_count) {
                    pq_node_code_positions_.resize(runtime_->store.elems.size(),
                                                   PQ_INVALID_CODE_POSITION);
                    pq_node_code_positions_[old_node_count] = code_position;
                }
            }
        }
    });
    }
    if (rabitq_requested_ && !rabitq_use_ && !runtime_->store.elems.empty()) {
        TrainAndEncodeRaBitQ();
    }
    rowid_coverage_checked_.store(false, std::memory_order_release);
    rowid_coverage_stale_.store(false, std::memory_order_relaxed);
    return ErrorData();
}

ErrorData GraphIndex::Append(IndexLock &l, DataChunk &chunk, Vector &row_ids, IndexAppendInfo &info) {
    (void)info;
    return Append(l, chunk, row_ids);
}

void GraphIndex::VerifyAppend(DataChunk &chunk, IndexAppendInfo &info, optional_ptr<ConflictManager> manager) {
    (void)chunk;
    (void)info;
    (void)manager;
}

void GraphIndex::VerifyConstraint(DataChunk &chunk, IndexAppendInfo &info, ConflictManager &manager) {
    (void)chunk;
    (void)info;
    (void)manager;
}

void GraphIndex::Delete(IndexLock &state, DataChunk &entries, Vector &row_identifiers) {
    (void)state;
    auto count = entries.size();
    if (count == 0) {
        return;
    }
    // Mutates deleted_rids_ which SearchANN reads: exclude concurrent searches.
    vex_duck::ExclusiveLockGuard _wg(graph_rwlock_);
    UnifiedVectorFormat rid_format;
    row_identifiers.ToUnifiedFormat(count, rid_format);
    auto rid_data = UnifiedVectorFormat::GetData<row_t>(rid_format);
    for (idx_t i = 0; i < count; i++) {
        auto idx = rid_format.sel->get_index(i);
        if (!rid_format.validity.RowIsValid(idx)) {
            continue;
        }
        deleted_rids_.insert(rid_data[idx]);
    }
    rowid_coverage_checked_.store(false, std::memory_order_release);
    rowid_coverage_stale_.store(false, std::memory_order_relaxed);
}

void GraphIndex::CommitDrop(IndexLock &index_lock) {
    (void)index_lock;
    vex_duck::ExclusiveLockGuard _wg(graph_rwlock_);
    if (runtime_) {
        ReleaseRawVectors();
        if (runtime_->store.node_alloc_) {
            runtime_->store.node_alloc_->Reset();
        }
        if (runtime_->store.upper_alloc_) {
            runtime_->store.upper_alloc_->Reset();
        }
    }
    runtime_.reset();
    rowid_node_map_.clear();
    duplicate_rowid_nodes_.clear();
    rowid_coverage_checked_.store(false, std::memory_order_release);
    rowid_coverage_stale_.store(false, std::memory_order_relaxed);
}

ErrorData GraphIndex::Insert(IndexLock &l, DataChunk &chunk, Vector &row_ids) {
    return Append(l, chunk, row_ids);
}

ErrorData GraphIndex::Insert(IndexLock &l, DataChunk &chunk, Vector &row_ids, IndexAppendInfo &info) {
    return Append(l, chunk, row_ids, info);
}

bool GraphIndex::MergeIndexes(IndexLock &state, BoundIndex &other_index) {
    (void)state;
    (void)other_index;
    return false;
}

bool GraphIndex::HasExcessHistory() const {
    vex_duck::SharedLockGuard _rg(graph_rwlock_);
    if (!runtime_) {
        return false;
    }
    size_t live_rows = 0;
    for (const auto &entry : rowid_node_map_) {
        if (deleted_rids_.find(entry.first) == deleted_rids_.end()) {
            live_rows++;
        }
    }
    size_t history_rows = runtime_->store.elems.size();
    if (pq_use_) {
        history_rows = std::max(history_rows, pq_row_id_order_.size());
    }
    if (live_rows > std::numeric_limits<size_t>::max() / 2) {
        return false;
    }
    return history_rows > live_rows * 2;
}

void GraphIndex::Vacuum(IndexLock &l) {
    (void)l;

    // DuckDB calls BoundIndex::Vacuum with the index mutex held. Search and
    // mutation use a separate lock in this adapter, so exclude them for the
    // complete snapshot/build/swap interval as well.
    vex_duck::ExclusiveLockGuard _wg(graph_rwlock_);
    if (!runtime_) {
        return;
    }

    auto &old_store = runtime_->store;
    bool has_stale_history = !deleted_rids_.empty();
    for (idx_t node_id = 0; node_id < old_store.elems.size() && !has_stale_history;
         node_id++) {
        const auto &tids = old_store.elems[node_id].tids;
        if (tids.empty()) {
            has_stale_history = true;
            break;
        }
        for (const auto &tid : tids) {
            auto current = rowid_node_map_.find(tid.row_id);
            if (current != rowid_node_map_.end() && current->second != node_id) {
                has_stale_history = true;
                break;
            }
        }
    }
    if (pq_use_ && pq_row_id_order_.size() != pq_latest_code_positions_.size()) {
        has_stale_history = true;
    }
    if (!has_stale_history) {
        return;
    }

    const bool pq_active = pq_use_ && pq_quantizer_.trained &&
                           pq_quantizer_.code_size > 0;
    const bool rabitq_active = rabitq_use_ && rabitq_quantizer_;
    if (pq_active && rabitq_active) {
        throw InternalException("GRAPH_INDEX cannot compact PQ and RaBitQ simultaneously");
    }
    const size_t pq_code_size = pq_active ? pq_quantizer_.code_size : 0;
    const size_t rabitq_code_size = rabitq_active
        ? ::rabitq::CodeSize(static_cast<int>(dimension_)) : 0;
    if (pq_active &&
        pq_codes_.size() != pq_row_id_order_.size() * pq_code_size) {
        throw InvalidInputException("PQ code bytes do not match row order before VACUUM");
    }
    if (rabitq_active &&
        rabitq_codes_.size() != old_store.elems.size() * rabitq_code_size) {
        throw InvalidInputException("RaBitQ code bytes do not match graph nodes before VACUUM");
    }

    // Build into an independent runtime. Its raw-vector mirror is deliberately
    // disabled: compact indexes reconstruct one approximation at a time, and
    // full indexes use DuckDB's buffer-managed vector allocator during rebuild.
    // This avoids an unbounded N*dimension temporary array and avoids holding
    // both the old and new hot mirrors at the same time.
    auto new_runtime = make_uniq<GraphIndexRuntimeState>(dimension_, m_, Allocator::Get(db));
    auto &new_store = new_runtime->store;
    new_store.InitAllocators(table_io_manager.GetIndexBlockManager());
    new_store.mirror_limit_bytes_ = 1;
    new_store.mirror_max_nodes_ = 0;
    new_store.normalize_vectors_ = false;

    std::unordered_set<row_t> seen_row_ids;
    seen_row_ids.reserve(rowid_node_map_.size());
    std::unordered_map<row_t, uint32_t> new_rowid_node_map;
    new_rowid_node_map.reserve(rowid_node_map_.size());
    std::unordered_map<row_t, std::vector<uint32_t>> new_duplicate_rowid_nodes;

    std::vector<uint8_t> new_pq_codes;
    std::vector<row_t> new_pq_row_id_order;
    std::vector<uint64_t> new_pq_vector_hashes;
    std::vector<uint32_t> new_pq_node_positions;
    std::unordered_map<row_t, uint32_t> new_pq_latest_positions;
    if (pq_active) {
        new_pq_codes.reserve(pq_latest_code_positions_.size() * pq_code_size);
        new_pq_row_id_order.reserve(pq_latest_code_positions_.size());
        new_pq_latest_positions.reserve(pq_latest_code_positions_.size());
        if (HasVectorCoverageChecksum()) {
            new_pq_vector_hashes.reserve(pq_latest_code_positions_.size());
        }
    }

    std::vector<uint8_t> new_rabitq_codes;
    if (rabitq_active) {
        new_rabitq_codes.reserve(old_store.elems.size() * rabitq_code_size);
    }

    std::vector<float> vector_scratch(dimension_);
    std::unique_ptr<float, void (*)(void *)> rabitq_reconstruct_scratch(
        nullptr, free_vector);
    if (compact_mode_ && rabitq_active) {
        rabitq_reconstruct_scratch.reset(
            alloc_floatvector(RABITQ_PADDED_DIM(dimension_), 1));
    }
    RunWithDuckAlgo(metric_, dimension_, ef_construction_, m_, new_store, [&](auto &algo) {
        using AlgoT = std::decay_t<decltype(algo)>;
        for (idx_t old_node_id = 0; old_node_id < old_store.elems.size(); old_node_id++) {
            const auto &old_elem = old_store.elems[old_node_id];
            for (const auto &old_tid : old_elem.tids) {
                const row_t row_id = old_tid.row_id;
                if (row_id < 0 || row_id >= MAX_ROW_ID ||
                    deleted_rids_.find(row_id) != deleted_rids_.end()) {
                    continue;
                }
                auto current = rowid_node_map_.find(row_id);
                if (current != rowid_node_map_.end() && current->second != old_node_id) {
                    continue;
                }
                if (!seen_row_ids.insert(row_id).second) {
                    continue;
                }

                uint32_t old_pq_position = PQ_INVALID_CODE_POSITION;
                const uint8_t *old_pq_code = nullptr;
                if (pq_active) {
                    auto latest = pq_latest_code_positions_.find(row_id);
                    if (latest == pq_latest_code_positions_.end() ||
                        latest->second >= pq_row_id_order_.size()) {
                        throw InvalidInputException(
                            "PQ metadata does not cover live row %lld during VACUUM",
                            static_cast<long long>(row_id));
                    }
                    old_pq_position = latest->second;
                    old_pq_code = pq_codes_.data() +
                                  static_cast<size_t>(old_pq_position) * pq_code_size;
                }
                const uint8_t *old_rabitq_code = rabitq_active
                    ? rabitq_codes_.data() + old_node_id * rabitq_code_size : nullptr;

                if (compact_mode_ && pq_active) {
                    pq_quantizer_.decode_code(old_pq_code, vector_scratch.data());
                } else if (compact_mode_ && rabitq_active) {
                    rabitq_quantizer_->reconstruct(old_rabitq_code,
                                                   vector_scratch.data(),
                                                   rabitq_reconstruct_scratch.get());
                } else {
                    const char *raw = old_store.get_data_unlocked(
                        static_cast<uint32_t>(old_node_id));
                    if (!raw) {
                        throw InvalidInputException(
                            "raw vector is unavailable for live graph node %llu during VACUUM",
                            static_cast<unsigned long long>(old_node_id));
                    }
                    std::memcpy(vector_scratch.data(), raw,
                                dimension_ * sizeof(float));
                }
                if (metric_ == VexMetric::COSINE) {
                    NormalizeInPlace(vector_scratch.data(), dimension_);
                }

                PointExtensionContext point_ctx;
                ItemPointerData new_tid;
                new_tid.row_id = row_id;
                typename AlgoT::InsertContextBase insert_ctx(
                    point_ctx,
                    reinterpret_cast<const char *>(vector_scratch.data()),
                    &new_tid);
                algo.insert(insert_ctx);
                const auto new_node_id = static_cast<uint32_t>(insert_ctx.result_id);
                if (new_node_id >= new_store.elems.size()) {
                    throw InternalException("VACUUM graph rebuild returned an invalid node id");
                }
                new_rowid_node_map[row_id] = new_node_id;

                if (pq_active) {
                    if (new_pq_row_id_order.size() >= PQ_INVALID_CODE_POSITION) {
                        throw InvalidInputException(
                            "PQ code count exceeds the supported 32-bit graph mapping");
                    }
                    const auto new_position =
                        static_cast<uint32_t>(new_pq_row_id_order.size());
                    new_pq_codes.insert(new_pq_codes.end(), old_pq_code,
                                        old_pq_code + pq_code_size);
                    new_pq_row_id_order.push_back(row_id);
                    new_pq_latest_positions[row_id] = new_position;
                    if (HasVectorCoverageChecksum()) {
                        new_pq_vector_hashes.push_back(
                            pq_vector_coverage_hashes_[old_pq_position]);
                    }
                    if (new_pq_node_positions.size() <= new_node_id) {
                        new_pq_node_positions.resize(
                            static_cast<size_t>(new_node_id) + 1,
                            PQ_INVALID_CODE_POSITION);
                    }
                    auto &mapped_position = new_pq_node_positions[new_node_id];
                    if (mapped_position == PQ_INVALID_CODE_POSITION) {
                        mapped_position = new_position;
                    } else if (std::memcmp(
                                   new_pq_codes.data() +
                                       static_cast<size_t>(mapped_position) * pq_code_size,
                                   old_pq_code, pq_code_size) != 0) {
                        throw InvalidInputException(
                            "PQ VACUUM reconstruction merged rows with different codes");
                    }
                }

                if (rabitq_active) {
                    const size_t node_offset =
                        static_cast<size_t>(new_node_id) * rabitq_code_size;
                    if (new_rabitq_codes.size() == node_offset) {
                        new_rabitq_codes.insert(new_rabitq_codes.end(),
                                                old_rabitq_code,
                                                old_rabitq_code + rabitq_code_size);
                    } else if (node_offset + rabitq_code_size <=
                                   new_rabitq_codes.size()) {
                        if (std::memcmp(new_rabitq_codes.data() + node_offset,
                                        old_rabitq_code, rabitq_code_size) != 0) {
                            throw InvalidInputException(
                                "RaBitQ VACUUM reconstruction merged rows with different codes");
                        }
                    } else {
                        throw InternalException(
                            "RaBitQ VACUUM code layout is not node-aligned");
                    }
                }
            }
        }
    });

    new_store.normalize_vectors_ = (metric_ == VexMetric::COSINE);
    if (pq_active) {
        if (new_pq_node_positions.size() != new_store.elems.size() ||
            std::find(new_pq_node_positions.begin(), new_pq_node_positions.end(),
                      PQ_INVALID_CODE_POSITION) != new_pq_node_positions.end()) {
            throw InternalException("PQ VACUUM code mapping does not cover rebuilt graph");
        }
    }
    if (rabitq_active &&
        new_rabitq_codes.size() != new_store.elems.size() * rabitq_code_size) {
        throw InternalException("RaBitQ VACUUM codes do not cover rebuilt graph");
    }

    if (compact_mode_ && (pq_active || rabitq_active)) {
        if (new_store.vector_alloc_) {
            new_store.vector_alloc_->Reset();
        }
        new_store.ClearMirrorVectors(/*shrink=*/true);
        new_store.ReleaseMirrorClaim();
        new_store.compact_mode_ = true;
    }

    // Commit with noexcept moves/swaps only. Until this point every failure
    // destroys the temporary runtime and leaves the old index untouched.
    auto retired_runtime = std::move(runtime_);
    runtime_ = std::move(new_runtime);
    rowid_node_map_.swap(new_rowid_node_map);
    duplicate_rowid_nodes_.swap(new_duplicate_rowid_nodes);
    deleted_rids_.clear();
    if (pq_active) {
        pq_codes_.swap(new_pq_codes);
        pq_row_id_order_.swap(new_pq_row_id_order);
        pq_vector_coverage_hashes_.swap(new_pq_vector_hashes);
        pq_node_code_positions_.swap(new_pq_node_positions);
        pq_latest_code_positions_.swap(new_pq_latest_positions);
    }
    if (rabitq_active) {
        rabitq_codes_.swap(new_rabitq_codes);
    }
    rowid_coverage_checked_.store(false, std::memory_order_release);
    rowid_coverage_stale_.store(false, std::memory_order_relaxed);

    // Release the old mirror before warming the rebuilt full-mode mirror. A
    // failed mirror allocation is only a cache miss: vector_alloc_ remains the
    // authoritative buffer-managed copy, so VACUUM correctness is unaffected.
    retired_runtime.reset();
    if (!compact_mode_) {
        auto &store = runtime_->store;
        store.ReleaseMirrorClaim();
        size_t mirror_nodes = store.elems.size();
        if (graph_memory_limit_bytes_ == 0) {
            store.mirror_limit_bytes_ = 0;
            store.mirror_max_nodes_ = SIZE_MAX;
        } else {
            store.mirror_limit_bytes_ =
                static_cast<size_t>(graph_memory_limit_bytes_);
            mirror_nodes = store.ClaimMirrorNodes(store.elems.size());
            store.mirror_max_nodes_ = mirror_nodes;
        }
        try {
            store.ResizeMirrorSlots(store.elems.size());
            for (size_t node_id = 0; node_id < mirror_nodes; node_id++) {
                if (!store.vectors[node_id].empty()) {
                    continue;
                }
                auto *raw = store.GetVectorData(static_cast<uint32_t>(node_id));
                if (raw) {
                    store.AssignMirrorSlot(
                        store.vectors[node_id], reinterpret_cast<const char *>(raw),
                        store.vec_size);
                }
            }
        } catch (...) {
            // Buffer-managed vectors remain complete; an incomplete hot mirror
            // is safe because get_data_unlocked falls back per node.
        }
    }
}

int GraphIndex::GetMaxLevel() const {
    if (!runtime_) {
        return -1;
    }
    return static_cast<int>(runtime_->store.entry_info.level);
}

idx_t GraphIndex::GetNodeCount() const {
    if (!runtime_) {
        return 0;
    }
    // deleted_rids_ is a std::unordered_set guarded by graph_rwlock_ — writers
    // (Append/Delete) mutate it under the exclusive lock. The optimizer path
    // (TryOptimizeANN) calls this on every query plan, so the deleted_rids_ reads
    // below must hold graph_rwlock_ shared or a concurrent INSERT/DELETE rehash
    // dangles the bucket array → use-after-free SEGV in unordered_set::find.
    vex_duck::SharedLockGuard _rg(graph_rwlock_);
    // elems / elem.tids are std::vector, read here lock-free on the optimizer path
    // (TryOptimizeANN). Under concurrent INSERT (add_elem/add_vector/BuildBulk) they
    // realloc, so a lock-free traversal dangles → use-after-free SEGV. Take the same
    // SHARED locks the build/search paths use: elems_veclock for the outer vector,
    // and GraphIndexPoint::tid_lock() for the per-node tids. (Same class of fix as
    // get_data — MemStore's STL containers can't use the main repo's lock-free
    // back-door, so reads must be locked.)
    LWLockAcquire(&runtime_->store.elems_veclock, LW_SHARED);
    idx_t result;
    if (deleted_rids_.empty()) {
        result = runtime_->store.elems.size();
    } else {
        // A node is "live" if at least one tracked row_id is not deleted. With dedup a
        // node carries many row_ids and survives until the last is deleted.
        std::shared_lock<std::shared_mutex> _tl(GraphIndexPoint::tid_lock());
        idx_t live = 0;
        for (auto &elem : runtime_->store.elems) {
            for (auto &tid : elem.tids) {
                if (deleted_rids_.find(tid.row_id) == deleted_rids_.end()) {
                    live++;
                    break;
                }
            }
        }
        result = live;
    }
    LWLockRelease(&runtime_->store.elems_veclock);
    return result;
}

idx_t GraphIndex::GetRowIdCount() const {
    if (!runtime_) {
        return 0;
    }
    // deleted_rids_ + store.elems are mutated by writers under graph_rwlock_
    // exclusive; read them shared so the unordered_set traversal can't race a
    // concurrent INSERT/DELETE rehash/realloc.
    vex_duck::SharedLockGuard _rg(graph_rwlock_);
    idx_t total = 0;
    for (auto &elem : runtime_->store.elems) {
        for (auto &tid : elem.tids) {
            if (deleted_rids_.find(tid.row_id) == deleted_rids_.end()) {
                total++;
            }
        }
    }
    return total;
}

GraphIndexRowIdCoverage GraphIndex::GetRowIdCoverage() const {
    GraphIndexRowIdCoverage coverage;
    if (!runtime_) {
        return coverage;
    }

    vex_duck::SharedLockGuard _rg(graph_rwlock_);
    const bool pq_coverage_layout = compact_mode_ && pq_use_ && pq_quantizer_.trained &&
                                    pq_quantizer_.code_size > 0 && !pq_codes_.empty() &&
                                    pq_codes_.size() == pq_row_id_order_.size() * pq_quantizer_.code_size;
    if (pq_coverage_layout) {
        const auto code_size = static_cast<idx_t>(pq_quantizer_.code_size);
        const bool has_vector_hashes = HasVectorCoverageChecksum();
        for (idx_t i = 0; i < pq_row_id_order_.size(); i++) {
            if (!IsLatestPQCodePosition(i)) {
                continue;
            }
            auto rid = pq_row_id_order_[i];
            if (rid < 0 || rid >= MAX_ROW_ID || deleted_rids_.find(rid) != deleted_rids_.end()) {
                continue;
            }
            coverage.live_count++;
            coverage.rowid_upper_bound =
                std::max<idx_t>(coverage.rowid_upper_bound, static_cast<idx_t>(rid) + 1);
            coverage.rowid_checksum += HashCoverageRowId(rid);
            if (has_vector_hashes) {
                coverage.vector_checksum += pq_vector_coverage_hashes_[i];
            } else {
                coverage.vector_checksum += HashCoverageRowBytes(
                    rid, const_data_ptr_cast(reinterpret_cast<const char *>(pq_codes_.data() + i * code_size)),
                    code_size);
            }
            coverage.has_vector_checksum = true;
        }
        return coverage;
    }

    const bool rabitq_coverage_layout = UsesRaBitQCoverageChecksum();
    if (rabitq_coverage_layout) {
        auto &store = runtime_->store;
        const auto code_size = ::rabitq::CodeSize(static_cast<int>(dimension_));
        LWLockAcquire(&store.elems_veclock, LW_SHARED);
        {
            std::shared_lock<std::shared_mutex> _tl(GraphIndexPoint::tid_lock());
            for (idx_t id = 0; id < store.elems.size(); id++) {
                const auto *code = rabitq_codes_.data() + id * code_size;
                for (auto &tid : store.elems[id].tids) {
                    auto rid = tid.row_id;
                    if (rid < 0 || rid >= MAX_ROW_ID || deleted_rids_.find(rid) != deleted_rids_.end()) {
                        continue;
                    }
                    coverage.live_count++;
                    coverage.rowid_upper_bound =
                        std::max<idx_t>(coverage.rowid_upper_bound, static_cast<idx_t>(rid) + 1);
                    coverage.rowid_checksum += HashCoverageRowId(rid);
                    coverage.vector_checksum += HashCoverageRowBytes(
                        rid, const_data_ptr_cast(reinterpret_cast<const char *>(code)), code_size);
                    coverage.has_vector_checksum = true;
                }
            }
        }
        LWLockRelease(&store.elems_veclock);
        return coverage;
    }

    auto &store = runtime_->store;
    LWLockAcquire(&store.elems_veclock, LW_SHARED);
    {
        std::shared_lock<std::shared_mutex> _tl(GraphIndexPoint::tid_lock());
        for (idx_t id = 0; id < store.elems.size(); id++) {
            const char *raw = store.get_data_unlocked(static_cast<uint32>(id));
            const auto *vec = reinterpret_cast<const float *>(raw);
            for (auto &tid : store.elems[id].tids) {
                auto rid = tid.row_id;
                if (rid < 0 || rid >= MAX_ROW_ID || deleted_rids_.find(rid) != deleted_rids_.end()) {
                    continue;
                }
                coverage.live_count++;
                coverage.rowid_upper_bound =
                    std::max<idx_t>(coverage.rowid_upper_bound, static_cast<idx_t>(rid) + 1);
                coverage.rowid_checksum += HashCoverageRowId(rid);
                if (vec) {
                    coverage.vector_checksum += HashCoverageRowVector(rid, vec, dimension_);
                    coverage.has_vector_checksum = true;
                }
            }
        }
    }
    LWLockRelease(&store.elems_veclock);
    return coverage;
}

bool GraphIndex::HasVectorCoverageChecksum() const {
    return !pq_vector_coverage_hashes_.empty() &&
           pq_vector_coverage_hashes_.size() == pq_row_id_order_.size();
}

bool GraphIndex::UsesPQCoverageChecksum() const {
    return compact_mode_ && pq_use_ && pq_quantizer_.trained && pq_quantizer_.code_size > 0 &&
           !pq_codes_.empty() && pq_codes_.size() == pq_row_id_order_.size() * pq_quantizer_.code_size &&
           !HasVectorCoverageChecksum();
}

uint64_t GraphIndex::HashPQVectorForCoverage(row_t row_id, const float *vec) const {
    if (!UsesPQCoverageChecksum()) {
        return 0;
    }
    std::vector<uint8_t> code(pq_quantizer_.code_size);
    pq_quantizer_.compute_code(vec, code.data());
    return HashCoverageRowBytes(row_id, const_data_ptr_cast(reinterpret_cast<const char *>(code.data())),
                                static_cast<idx_t>(code.size()));
}

bool GraphIndex::UsesRaBitQCoverageChecksum() const {
    if (!compact_mode_ || !rabitq_use_ || !rabitq_quantizer_ || !runtime_) {
        return false;
    }
    const auto code_size = ::rabitq::CodeSize(static_cast<int>(dimension_));
    return code_size > 0 &&
           rabitq_codes_.size() == runtime_->store.elems.size() * code_size;
}

uint64_t GraphIndex::HashRaBitQVectorForCoverage(row_t row_id, const float *vec) const {
    if (!UsesRaBitQCoverageChecksum()) {
        return 0;
    }
    std::vector<uint8_t> code(::rabitq::CodeSize(static_cast<int>(dimension_)));
    ::rabitq::EncodeCode(*rabitq_quantizer_, static_cast<int>(dimension_), vec, code.data());
    return HashCoverageRowBytes(row_id,
                                const_data_ptr_cast(reinterpret_cast<const char *>(code.data())),
                                static_cast<idx_t>(code.size()));
}

idx_t GraphIndex::GetInMemorySize(IndexLock &state) {
    (void)state;
    if (!runtime_) {
        return 0;
    }
    vex_duck::SharedLockGuard _rg(graph_rwlock_);
    idx_t size = 0;
    if (runtime_->store.node_alloc_) {
        size += runtime_->store.node_alloc_->GetInMemorySize();
    }
    if (runtime_->store.vector_alloc_) {
        size += runtime_->store.vector_alloc_->GetInMemorySize();
    }
    if (runtime_->store.upper_alloc_) {
        size += runtime_->store.upper_alloc_->GetInMemorySize();
    }
    size += runtime_->store.GetTrackedInMemorySize();
    size += pq_codes_.capacity() * sizeof(pq_codes_[0]);
    size += pq_row_id_order_.capacity() * sizeof(pq_row_id_order_[0]);
    size += pq_vector_coverage_hashes_.capacity() * sizeof(pq_vector_coverage_hashes_[0]);
    size += pq_node_code_positions_.capacity() * sizeof(pq_node_code_positions_[0]);
    size += pq_latest_code_positions_.size() *
            (sizeof(row_t) + sizeof(uint32_t) + 2 * sizeof(void *));
    size += rowid_node_map_.size() *
            (sizeof(row_t) + sizeof(uint32_t) + 2 * sizeof(void *));
    size += rowid_node_map_.size() * sizeof(ItemPointerData);
    size += deleted_rids_.size() * (sizeof(row_t) + 2 * sizeof(void *));
    for (const auto &entry : duplicate_rowid_nodes_) {
        size += sizeof(row_t) + sizeof(std::vector<uint32_t>) + 2 * sizeof(void *);
        size += entry.second.capacity() * sizeof(uint32_t);
    }
    if (pq_use_) {
        size += pq_quantizer_.get_centroids_size() * sizeof(float);
    }
    size += rabitq_codes_.capacity();
    if (rabitq_use_ && rabitq_quantizer_) {
        const idx_t padded_dim = RABITQ_PADDED_DIM(dimension_);
        size += rabitq_quantizer_->get_random_matrix_size();
        size += HNSW_RABITQ_NUM_CLUSTERS * dimension_ * sizeof(float);
        size += HNSW_RABITQ_NUM_CLUSTERS * padded_dim * sizeof(float);
    }
    return size;
}

void GraphIndex::Verify(IndexLock &l) {
    (void)l;
}

string GraphIndex::ToString(IndexLock &l, bool display_ascii) {
    (void)l;
    (void)display_ascii;
    size_t node_count = runtime_ ? runtime_->store.elems.size() : 0;
    return StringUtil::Format("GRAPH_INDEX(dim=%llu, m=%d, ef_construction=%d, rows=%llu)",
                              static_cast<unsigned long long>(dimension_), m_, ef_construction_,
                              static_cast<unsigned long long>(node_count));
}

void GraphIndex::VerifyAllocations(IndexLock &l) {
    (void)l;
}

void GraphIndex::VerifyBuffers(IndexLock &l) {
    (void)l;
}

string GraphIndex::GetConstraintViolationMessage(VerifyExistenceType verify_type, idx_t failed_index,
                                                 DataChunk &input) {
    (void)verify_type;
    (void)failed_index;
    (void)input;
    return "GRAPH_INDEX does not enforce constraints";
}

void GraphIndex::DeserializePQAndModeFromStorage(const IndexStorageInfo &info) {
    auto deleted_it = info.options.find("deleted_rids");
    if (deleted_it != info.options.end()) {
        auto blob = StringValue::Get(deleted_it->second.DefaultCastAs(LogicalType::BLOB));
        const char *p = blob.data();
        const char *end = p + blob.size();
        if (p + sizeof(uint64_t) > end) {
            throw InvalidInputException("GRAPH_INDEX deleted row metadata is truncated");
        }
        uint64_t count = 0;
        std::memcpy(&count, p, sizeof(count));
        p += sizeof(count);
        if (count > static_cast<uint64_t>((end - p) / sizeof(int64_t)) ||
            p + count * sizeof(int64_t) != end) {
            throw InvalidInputException("GRAPH_INDEX deleted row metadata is invalid");
        }
        deleted_rids_.clear();
        for (uint64_t i = 0; i < count; i++) {
            int64_t value = 0;
            std::memcpy(&value, p, sizeof(value));
            p += sizeof(value);
            deleted_rids_.insert(static_cast<row_t>(value));
        }
    }

    auto pq_m_it = info.options.find("pq_m");
    auto pq_dim_it = info.options.find("pq_dim");
    auto pq_codebook_it = info.options.find("pq_codebook");
    auto pq_codes_it = info.options.find("pq_codes");
    auto pq_order_it = info.options.find("pq_row_order");
    if (pq_m_it != info.options.end() && pq_dim_it != info.options.end() &&
        pq_codebook_it != info.options.end() && pq_codes_it != info.options.end() &&
        pq_order_it != info.options.end()) {
        uint32_t pq_storage_version = 1;
        auto pq_version_it = info.options.find("pq_storage_version");
        if (pq_version_it != info.options.end()) {
            pq_storage_version = pq_version_it->second.GetValue<uint32_t>();
        }
        if (pq_storage_version < 1 || pq_storage_version > 2) {
            throw InvalidInputException("Unsupported PQ storage version");
        }
        pq_m_ = pq_m_it->second.GetValue<uint32_t>();
        ::vex::quantizer::PQContext ctx;
        // Lazy binding normally deserializes once, but retries after a failed
        // bind must not orphan the previous malloc-backed codebook.
        pq_quantizer_.free_resources(ctx);
        pq_quantizer_.set_basic_values(pq_dim_it->second.GetValue<uint32_t>(), pq_m_, /*nbits*/8);
        pq_quantizer_.set_derived_values(ctx);
        pq_quantizer_.set_fvec_L2sqr_ny_nearest_func();
        pq_quantizer_.set_fvec_ny_distance_func(Metric::L2);
        pq_quantizer_.set_dist_code_func();

        pq_vector_coverage_hashes_.clear();
        auto checksum_it = info.options.find("pq_vector_coverage_hashes");
        if (pq_storage_version == 2) {
            const auto &cb_blob = StringValue::Get(pq_codebook_it->second);
            const size_t expected_centroid_bytes =
                pq_quantizer_.get_centroids_size() * sizeof(float);
            if (cb_blob.size() != expected_centroid_bytes) {
                throw InvalidInputException("PQ codebook data is truncated or incompatible");
            }
            std::memcpy(pq_quantizer_.centroids, cb_blob.data(), cb_blob.size());
            pq_quantizer_.trained = true;

            const auto &codes_blob = StringValue::Get(pq_codes_it->second);
            if (codes_blob.empty() ||
                codes_blob.size() % pq_quantizer_.code_size != 0) {
                throw InvalidInputException("PQ code data is truncated or incompatible");
            }
            pq_codes_.assign(codes_blob.begin(), codes_blob.end());

            const auto &order_blob = StringValue::Get(pq_order_it->second);
            const size_t code_count = pq_codes_.size() / pq_quantizer_.code_size;
            if (order_blob.size() != code_count * sizeof(int64_t)) {
                throw InvalidInputException("PQ row order does not match code count");
            }
            pq_row_id_order_.resize(code_count);
            std::memcpy(pq_row_id_order_.data(), order_blob.data(), order_blob.size());

            if (checksum_it != info.options.end()) {
                const auto &checksum_blob = StringValue::Get(checksum_it->second);
                if (checksum_blob.size() != code_count * sizeof(uint64_t)) {
                    throw InvalidInputException("PQ coverage checksum count is invalid");
                }
                pq_vector_coverage_hashes_.resize(code_count);
                std::memcpy(pq_vector_coverage_hashes_.data(), checksum_blob.data(),
                            checksum_blob.size());
            }
        } else {
            // v1 stored a uint64 count before every array. Keep this reader so
            // indexes created by earlier builds remain usable.
            const auto &cb_blob = StringValue::Get(pq_codebook_it->second);
            const char *p = cb_blob.data();
            const char *end = p + cb_blob.size();
            if (p + sizeof(uint64_t) <= end) {
                uint64_t cn;
                std::memcpy(&cn, p, sizeof(cn)); p += sizeof(cn);
                if (cn == pq_quantizer_.get_centroids_size() &&
                    p + cn * sizeof(float) == end) {
                    std::memcpy(pq_quantizer_.centroids, p, cn * sizeof(float));
                    pq_quantizer_.trained = true;
                }
            }

            const auto &codes_blob = StringValue::Get(pq_codes_it->second);
            p = codes_blob.data(); end = p + codes_blob.size();
            if (p + sizeof(uint64_t) <= end) {
                uint64_t n;
                std::memcpy(&n, p, sizeof(n)); p += sizeof(n);
                if (p + n == end) pq_codes_.assign(p, p + n);
            }

            const auto &order_blob = StringValue::Get(pq_order_it->second);
            p = order_blob.data(); end = p + order_blob.size();
            if (p + sizeof(uint64_t) <= end) {
                uint64_t n;
                std::memcpy(&n, p, sizeof(n)); p += sizeof(n);
                if (n <= static_cast<uint64_t>((end - p) / sizeof(int64_t)) &&
                    p + n * sizeof(int64_t) == end) {
                    pq_row_id_order_.resize(n);
                    std::memcpy(pq_row_id_order_.data(), p, n * sizeof(int64_t));
                }
            }

            if (checksum_it != info.options.end()) {
                const auto &checksum_blob = StringValue::Get(checksum_it->second);
                p = checksum_blob.data(); end = p + checksum_blob.size();
                if (p + sizeof(uint64_t) <= end) {
                    uint64_t n;
                    std::memcpy(&n, p, sizeof(n)); p += sizeof(n);
                    if (n == pq_row_id_order_.size() &&
                        p + n * sizeof(uint64_t) == end) {
                        pq_vector_coverage_hashes_.resize(n);
                        std::memcpy(pq_vector_coverage_hashes_.data(), p,
                                    n * sizeof(uint64_t));
                    }
                }
            }
        }

        if (!pq_quantizer_.trained || pq_codes_.empty() ||
            pq_codes_.size() != pq_row_id_order_.size() * pq_quantizer_.code_size) {
            throw InvalidInputException("PQ storage metadata is incomplete or inconsistent");
        }
        pq_use_ = true;
        RebuildPQLatestCodePositions();
        pq_node_code_positions_.clear();
        auto node_map_it = info.options.find("pq_node_code_positions");
        if (node_map_it != info.options.end()) {
            const auto &node_map_blob = StringValue::Get(node_map_it->second);
            const char *p = node_map_blob.data();
            const char *end = p + node_map_blob.size();
            uint64_t n = runtime_->store.elems.size();
            if (pq_storage_version == 1) {
                if (p + sizeof(uint64_t) > end) {
                    throw InvalidInputException("PQ node/code mapping is truncated");
                }
                std::memcpy(&n, p, sizeof(n)); p += sizeof(n);
            }
            if (n != runtime_->store.elems.size() ||
                n > static_cast<uint64_t>((end - p) / sizeof(uint32_t)) ||
                p + n * sizeof(uint32_t) != end) {
                throw InvalidInputException("PQ node/code mapping is invalid");
            }
            pq_node_code_positions_.reserve(n);
            for (uint64_t i = 0; i < n; i++) {
                uint32_t position = 0;
                std::memcpy(&position, p, sizeof(position)); p += sizeof(position);
                if (position >= pq_row_id_order_.size()) {
                    throw InvalidInputException("PQ node/code mapping contains an invalid slot");
                }
                pq_node_code_positions_.push_back(position);
            }
        } else {
            // Compatibility with indexes written before the explicit node map.
            RebuildPQNodeCodePositions();
        }
    }

    auto quantizer_it = info.options.find("quantizer");
    if (quantizer_it != info.options.end() &&
        StringUtil::CIEquals(quantizer_it->second.ToString(), "rabitq")) {
        rabitq_requested_ = true;
    }
    auto rabitq_fixed_it = info.options.find("rabitq_fixed");
    auto rabitq_random_it = info.options.find("rabitq_random");
    auto rabitq_centroids_it = info.options.find("rabitq_centroids");
    auto rabitq_rotated_it = info.options.find("rabitq_rotated");
    auto rabitq_codes_it = info.options.find("rabitq_codes");
    auto rabitq_factor_it = info.options.find("rabitq_query_rescaling_factor");
    auto rabitq_version_it = info.options.find("rabitq_version");
    const bool has_rabitq_fixed = rabitq_fixed_it != info.options.end();
    const bool has_rabitq_random = rabitq_random_it != info.options.end();
    const bool has_rabitq_centroids = rabitq_centroids_it != info.options.end();
    const bool has_rabitq_rotated = rabitq_rotated_it != info.options.end();
    const bool has_rabitq_codes = rabitq_codes_it != info.options.end();
    const bool has_rabitq_factor = rabitq_factor_it != info.options.end();
    const bool has_rabitq_version = rabitq_version_it != info.options.end();
    const bool has_any_rabitq_storage =
        has_rabitq_fixed || has_rabitq_random || has_rabitq_centroids ||
        has_rabitq_rotated || has_rabitq_codes || has_rabitq_factor ||
        has_rabitq_version;
    uint32_t rabitq_storage_version = 0;
    if (has_rabitq_version) {
        rabitq_storage_version = rabitq_version_it->second.GetValue<uint32_t>();
        if (rabitq_storage_version != 2 && rabitq_storage_version != 3) {
            throw InvalidInputException("Unsupported RaBitQ storage version");
        }
    }
    const bool has_all_rabitq_storage =
        has_rabitq_version && has_rabitq_codes && has_rabitq_factor &&
        ((rabitq_storage_version == 2 && has_rabitq_fixed) ||
         (rabitq_storage_version == 3 && has_rabitq_random &&
          has_rabitq_centroids && has_rabitq_rotated));
    const size_t persisted_node_count = runtime_ ? runtime_->store.elems.size() : 0;
    if (rabitq_requested_ && persisted_node_count > 0 && !has_all_rabitq_storage) {
        throw InvalidInputException(
            "RaBitQ storage metadata is incomplete; rebuild the index");
    }
    if (!rabitq_requested_ && has_any_rabitq_storage) {
        throw InvalidInputException(
            "RaBitQ storage metadata exists without quantizer='rabitq'");
    }
    if (rabitq_requested_ && has_all_rabitq_storage) {
        const int padded_dim = RABITQ_PADDED_DIM(static_cast<int>(dimension_));
        auto quantizer = std::make_unique<::rabitq::RaBitQuantizer>(
            static_cast<int>(dimension_), padded_dim, ToRaBitQMetric(metric_));
        const size_t random_bytes = quantizer->get_random_matrix_size();
        const size_t centroid_bytes = HNSW_RABITQ_NUM_CLUSTERS * dimension_ * sizeof(float);
        const size_t rotated_bytes = HNSW_RABITQ_NUM_CLUSTERS * padded_dim * sizeof(float);
        if (rabitq_storage_version == 2) {
            const size_t expected_fixed = random_bytes + centroid_bytes + rotated_bytes;
            const auto &fixed_blob = StringValue::Get(rabitq_fixed_it->second);
            if (fixed_blob.size() != expected_fixed) {
                throw InvalidInputException("RaBitQ fixed data is truncated or incompatible");
            }
            auto *fixed = const_cast<char *>(fixed_blob.data());
            quantizer->load(
                fixed, reinterpret_cast<float *>(fixed + random_bytes),
                reinterpret_cast<float *>(fixed + random_bytes + centroid_bytes));
        } else {
            const auto &random_blob = StringValue::Get(rabitq_random_it->second);
            const auto &centroids_blob = StringValue::Get(rabitq_centroids_it->second);
            const auto &rotated_blob = StringValue::Get(rabitq_rotated_it->second);
            if (random_blob.size() != random_bytes ||
                centroids_blob.size() != centroid_bytes ||
                rotated_blob.size() != rotated_bytes) {
                throw InvalidInputException("RaBitQ fixed data is truncated or incompatible");
            }
            quantizer->load(
                const_cast<char *>(random_blob.data()),
                reinterpret_cast<float *>(const_cast<char *>(centroids_blob.data())),
                reinterpret_cast<float *>(const_cast<char *>(rotated_blob.data())));
        }

        rabitq_query_rescaling_factor_ =
            rabitq_factor_it->second.GetValue<double>();
        quantizer->set_rescaling_factor(rabitq_query_rescaling_factor_);

        const auto &codes_blob = StringValue::Get(rabitq_codes_it->second);
        const char *p = codes_blob.data();
        const char *end = p + codes_blob.size();
        uint64_t codes_n = codes_blob.size();
        if (rabitq_storage_version == 2) {
            if (p + sizeof(uint64_t) > end) {
                throw InvalidInputException("RaBitQ code data is truncated");
            }
            std::memcpy(&codes_n, p, sizeof(codes_n));
            p += sizeof(codes_n);
        }
        const size_t code_size = ::rabitq::CodeSize(static_cast<int>(dimension_));
        const size_t node_count = runtime_ ? runtime_->store.elems.size() : 0;
        if (codes_n != node_count * code_size || p + codes_n != end) {
            throw InvalidInputException("RaBitQ code coverage does not match graph nodes");
        }
        rabitq_codes_.assign(reinterpret_cast<const uint8_t *>(p),
                             reinterpret_cast<const uint8_t *>(p + codes_n));
        for (size_t id = 0; id < node_count; id++) {
            const auto *code = rabitq_codes_.data() + id * code_size;
            if (!::rabitq::CodeHasValidCluster(code) ||
                !::rabitq::CodeHasFiniteFactors(code, static_cast<int>(dimension_))) {
                throw InvalidInputException("RaBitQ code contains invalid values");
            }
        }
        rabitq_quantizer_ = std::move(quantizer);
        rabitq_requested_ = true;
        rabitq_use_ = !rabitq_codes_.empty();
    }

    bool compact_mode_flag = false;
    auto compact_it = info.options.find("compact_mode");
    if (compact_it != info.options.end()) {
        compact_mode_flag = compact_it->second.GetValue<bool>();
    }
    compact_mode_ = compact_mode_flag;
    if (compact_mode_) {
        ReleaseRawVectors();
    }
    RebuildRowIdNodeMap();
}

void GraphIndex::DeserializeFromStorage(const IndexStorageInfo &info) {
    if (!runtime_) {
        return;
    }

    auto &store = runtime_->store;

    // compact_mode 模式下 vector_alloc_ 被 Reset，但 header->vector_ptr 仍是旧
    // buffer_id；先识别 compact 标志，后续跳过 vectors mirror 以免对空 allocator
    // 调 Get() 解引用空 buffers 数组导致 SIGSEGV。
    bool compact_mode_flag = false;
    {
        auto compact_it = info.options.find("compact_mode");
        if (compact_it != info.options.end()) {
            compact_mode_flag = compact_it->second.GetValue<bool>();
        }
    }

    if (info.allocator_infos.size() >= 3) {
        store.node_alloc_->Init(info.allocator_infos[0]);
        store.vector_alloc_->Init(info.allocator_infos[1]);
        store.upper_alloc_->Init(info.allocator_infos[2]);
    }

    size_t node_count = 0;
    size_t upper_count = 0;
    auto nc_it = info.options.find("node_count");
    if (nc_it != info.options.end()) {
        node_count = nc_it->second.GetValue<uint64_t>();
    }
    auto uc_it = info.options.find("upper_count");
    if (uc_it != info.options.end()) {
        upper_count = uc_it->second.GetValue<uint64_t>();
    }
    store.ResizeForReload(node_count, upper_count);
    store.id_to_node_ptr_.resize(node_count);
    store.upper_idx_to_ptr_.resize(upper_count);
    // Reset atomic id counters to match restored counts so that the next
    // assign_vector_id<true>() starts from the correct position, not from
    // the default-initialized value of 0.
    store.next_base_id_.store(static_cast<uint32_t>(node_count), std::memory_order_relaxed);
    store.next_upper_id_.store(static_cast<uint32_t>(upper_count), std::memory_order_relaxed);

    auto eid_it = info.options.find("entry_id");
    auto ec_it = info.options.find("entry_cur_layer_idx");
    auto el_it = info.options.find("entry_level");
    if (eid_it != info.options.end() && ec_it != info.options.end() && el_it != info.options.end()) {
        size_t entry_id = eid_it->second.GetValue<uint64_t>();
        size_t entry_cur_idx = ec_it->second.GetValue<uint64_t>();
        int entry_level = el_it->second.GetValue<int>();
        store.entry_info.set(entry_id, entry_cur_idx, entry_level);
    }

    auto id_ptr_it = info.options.find("id_ptr_map");
    if (id_ptr_it != info.options.end()) {
        auto blob = StringValue::Get(id_ptr_it->second.DefaultCastAs(LogicalType::BLOB));
        const char *ptr = blob.data();
        const char *end = ptr + blob.size();
        if (ptr + sizeof(uint64_t) <= end) {
            uint64_t num_entries;
            std::memcpy(&num_entries, ptr, sizeof(num_entries));
            ptr += sizeof(num_entries);
            store.id_to_node_ptr_.resize(num_entries);
            for (uint64_t i = 0; i < num_entries && ptr + sizeof(uint64_t) <= end; i++) {
                uint64_t ptr_val;
                std::memcpy(&ptr_val, ptr, sizeof(ptr_val));
                ptr += sizeof(ptr_val);
                store.id_to_node_ptr_[i].Set(ptr_val);
                store.node_ptr_to_id_[ptr_val] = static_cast<uint32_t>(i);
            }
        }
    }

    auto del_it = info.options.find("deleted_rids");
    if (del_it != info.options.end()) {
        auto blob = StringValue::Get(del_it->second.DefaultCastAs(LogicalType::BLOB));
        const char *ptr = blob.data();
        const char *end = ptr + blob.size();
        deleted_rids_.clear();
        if (ptr + sizeof(uint64_t) <= end) {
            uint64_t num_deleted;
            std::memcpy(&num_deleted, ptr, sizeof(num_deleted));
            ptr += sizeof(num_deleted);
            for (uint64_t i = 0; i < num_deleted && ptr + sizeof(int64_t) <= end; i++) {
                int64_t rid_val;
                std::memcpy(&rid_val, ptr, sizeof(rid_val));
                ptr += sizeof(rid_val);
                deleted_rids_.insert(static_cast<row_t>(rid_val));
            }
        }
    }

    // Repopulate per-node mirrors from disk-backed HNSWNodeHeader. After reload
    // ResizeForReload defaults elems / base_points / vectors to fresh blanks
    // (tids empty, neighbors=INVALID_VECTOR_ID, dists=INVALID_DIST). Code paths
    // that fall back to the in-memory mirrors (get_neighbors reads bp.dists
    // unconditionally; get_data falls back to vectors[id].data() when the
    // node_alloc_ path is missed) would otherwise see those blanks and either
    // return zero results or follow INVALID neighbor IDs into out-of-bounds
    // memory — manifesting as SIGSEGV during the next Append/INSERT that walks
    // the graph from entry_point.
    //
    // Skip nodes whose header->deleted flag was set by a prior Delete() —
    // restoring their tids would resurrect rows the table no longer has.
    const int m_local = static_cast<int>(store.m);
    const uint_fast16_t nbr_slots = static_cast<uint_fast16_t>(m_local * 2);
    // Standard reload participates in the global mirror pool too: vectors live in
    // vector_alloc_, so over-budget nodes simply keep no mirror copy (rebuilt below
    // only for i < mirror_max_nodes_) and are served from the buffer manager.
    if (store.mirror_limit_bytes_ != 0) {
        store.mirror_max_nodes_ =
            store.ClaimMirrorNodes(std::min(store.id_to_node_ptr_.size(), store.elems.size()));
    }
    for (size_t i = 0; i < store.id_to_node_ptr_.size() && i < store.elems.size(); i++) {
        auto ptr = store.id_to_node_ptr_[i];
        if (!ptr.Get() || !store.node_alloc_) {
            continue;
        }
        auto *header = reinterpret_cast<duckdb::vex::HNSWNodeHeader<uint32_t> *>(store.node_alloc_->Get(ptr));
        if (!header) {
            continue;
        }
        // tids (skip deleted)
        if (!header->deleted &&
            deleted_rids_.find(header->row_id) == deleted_rids_.end()) {
            auto &elem = store.elems[i];
            elem.tids.clear();
            ItemPointerData tid;
            tid.row_id = header->row_id;
            elem.tids.push_back(tid);
        }
        // base_points[i].neighbors mirror (search_layer fallback path)
        // ALSO patch the disk-backed header: zero-initialized slots past
        // level0_count can hold garbage if the segment was previously freed
        // and re-allocated. search_layer iterates all m*2 slots regardless
        // of level0_count and uses is_valid(id != INVALID_VECTOR_ID) to skip;
        // garbage like 0xfffffff7 passes is_valid and then indexes into
        // vectors[id] OOB, crashing in Append's commit path.
        {
            uint32_t *header_neighbors = header->GetLevel0Neighbors();
            const uint16_t valid_count = header->level0_count;
            for (uint_fast16_t j = valid_count; j < nbr_slots; j++) {
                header_neighbors[j] = uint32_t(INVALID_VECTOR_ID);
            }
            if (i < store.base_points.size()) {
                auto &bp = store.base_points[i];
                if (bp.neighbors.size() != nbr_slots) {
                    bp.neighbors.assign(nbr_slots, uint32_t(INVALID_VECTOR_ID));
                }
                for (uint_fast16_t j = 0; j < nbr_slots; j++) {
                    bp.neighbors[j] = header_neighbors[j];
                }
            }
        }
        // vectors[i] mirror (get_data fallback path)
        // compact_mode 下原始向量已被 ReleaseRawVectors 清空，header->vector_ptr 是
        // 失效的 buffer_id；跳过避免对空 allocator 调 Get() 解引用 nullptr。
        // 超出 mirror_max_nodes_ 预算的节点不重建镜像，留 get_data 走 vector_alloc_。
        if (!compact_mode_flag && store.vector_alloc_ && header->vector_ptr.Get() &&
            i < store.vectors.size() && i < store.mirror_max_nodes_) {
            auto *vec_data = reinterpret_cast<const char *>(store.vector_alloc_->Get(header->vector_ptr));
            if (vec_data) {
                store.AssignMirrorSlot(store.vectors[i], vec_data, store.vec_size);
            }
        }
    }

    auto upper_ptr_it = info.options.find("upper_ptr_map");
    if (upper_ptr_it != info.options.end()) {
        auto blob = StringValue::Get(upper_ptr_it->second.DefaultCastAs(LogicalType::BLOB));
        const char *ptr = blob.data();
        const char *end = ptr + blob.size();
        if (ptr + sizeof(uint64_t) <= end) {
            uint64_t num_entries;
            std::memcpy(&num_entries, ptr, sizeof(num_entries));
            ptr += sizeof(num_entries);
            store.upper_idx_to_ptr_.resize(num_entries);
            for (uint64_t i = 0; i < num_entries && ptr + sizeof(uint64_t) <= end; i++) {
                uint64_t ptr_val;
                std::memcpy(&ptr_val, ptr, sizeof(ptr_val));
                ptr += sizeof(ptr_val);
                store.upper_idx_to_ptr_[i].Set(ptr_val);
            }
        }
    }

    auto upper_data_it = info.options.find("upper_points_data");
    if (upper_data_it != info.options.end()) {
        auto blob = StringValue::Get(upper_data_it->second.DefaultCastAs(LogicalType::BLOB));
        const char *ptr = blob.data();
        const char *end = ptr + blob.size();
        if (ptr + sizeof(uint64_t) <= end) {
            uint64_t num_entries;
            std::memcpy(&num_entries, ptr, sizeof(num_entries));
            ptr += sizeof(num_entries);
            if (num_entries > store.upper_points.size()) {
                store.upper_points.resize(num_entries);
            }
            for (uint64_t i = 0; i < num_entries; i++) {
                if (ptr + sizeof(uint32_t) * 2 + sizeof(uint64_t) > end) {
                    break;
                }
                uint32_t id_val;
                uint32_t lower_val;
                uint64_t nbr_size;
                std::memcpy(&id_val, ptr, sizeof(id_val)); ptr += sizeof(id_val);
                std::memcpy(&lower_val, ptr, sizeof(lower_val)); ptr += sizeof(lower_val);
                std::memcpy(&nbr_size, ptr, sizeof(nbr_size)); ptr += sizeof(nbr_size);
                if (ptr + nbr_size * sizeof(uint32_t) > end) {
                    break;
                }
                auto &up = store.upper_points[i];
                up.id = id_val;
                up.lower_layer_idx = lower_val;
                up.neighbors_info.resize(nbr_size);
                if (nbr_size) {
                    std::memcpy(up.neighbors_info.data(), ptr, nbr_size * sizeof(uint32_t));
                    ptr += nbr_size * sizeof(uint32_t);
                }
            }
        }
    }

    DeserializePQAndModeFromStorage(info);
}

} // namespace duckdb
