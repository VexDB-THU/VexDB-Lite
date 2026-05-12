#include "vex_graph_index.hpp"

#include <set>
#include <limits>
#include <thread>
#include <atomic>
#include <exception>
#include <mutex>
#include <vector>

#include "graph_index/graph_index_algorithm.h"
#include "distance/core/distance_dispatcher.h"

#include "vex/vex_disk_block_store.hpp"
#include "vex_distance.hpp"
#include "vex_hnsw_node.hpp"
#include "vex_physical_create_index.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/vector/array_vector.hpp"
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
                       idx_t vec_column_index, uint32_t pq_m, bool compact_mode, int build_threads)
    : BoundIndex(name, TYPE_NAME, constraint_type, column_ids, table_io_manager, unbound_expressions, db),
      dimension_(dimension), m_(m), ef_construction_(ef_construction),
      build_threads_(build_threads), metric_(metric),
      vec_column_index_(vec_column_index), pq_m_(pq_m),
      runtime_(make_uniq<GraphIndexRuntimeState>(dimension, m)),
      compact_mode_(compact_mode) {
    runtime_->store.normalize_vectors_ = (metric_ == VexMetric::COSINE);
}

unique_ptr<BoundIndex> GraphIndex::Create(CreateIndexInput &input) {
    if (input.unbound_expressions.empty()) {
        throw InvalidInputException("GRAPH_INDEX requires at least one indexed expression");
    }

    // CREATE INDEX syntax requires the vector column to be the first listed, e.g.
    // GRAPH_INDEX(vec, scalar1, scalar2). Reject any other layout up-front: silently
    // accepting duplicate or extra-vector columns corrupts the per-node metadata
    // segment layout, which assumes a fixed schema with one vector at slot 0.
    auto &first_type = input.unbound_expressions[0]->GetReturnType();
    if (first_type.id() != LogicalTypeId::ARRAY ||
        ArrayType::GetChildType(first_type).id() != LogicalTypeId::FLOAT) {
        throw InvalidInputException("GRAPH_INDEX first column must be FLOAT[N], got %s",
                                    first_type.ToString());
    }
    auto &vec_type = first_type;
    // DuckDB dedups `column_ids` at bind time but keeps one unbound_expression per
    // user-written reference, so `GRAPH_INDEX(v, c1, c1)` arrives as 3 expressions
    // with 2 unique column_ids. Walk expressions to catch both duplicates and any
    // sneaky second vector column past slot 0.
    if (input.unbound_expressions.size() > 1) {
        unordered_set<string> seen_expr_sigs;
        seen_expr_sigs.insert(input.unbound_expressions[0]->ToString());
        for (idx_t i = 1; i < input.unbound_expressions.size(); i++) {
            auto &expr = *input.unbound_expressions[i];
            auto &col_type = expr.GetReturnType();
            if (col_type.id() == LogicalTypeId::ARRAY &&
                ArrayType::GetChildType(col_type).id() == LogicalTypeId::FLOAT) {
                throw InvalidInputException(
                    "GRAPH_INDEX: only the first column may be a vector (FLOAT[N]); "
                    "column at position %llu has type %s",
                    static_cast<unsigned long long>(i + 1), col_type.ToString());
            }
            auto sig = expr.ToString();
            if (!seen_expr_sigs.insert(sig).second) {
                throw InvalidInputException(
                    "GRAPH_INDEX: duplicate column at position %llu (%s)",
                    static_cast<unsigned long long>(i + 1), sig);
            }
        }
    }

    idx_t dimension = ArrayType::GetSize(vec_type);
    int m = 16;
    int ef_construction = 64;
    VexMetric metric = VexMetric::L2;

    auto m_it = input.options.find("m");
    if (m_it != input.options.end()) {
        try {
            m = m_it->second.DefaultCastAs(LogicalType::INTEGER).GetValue<int>();
        } catch (...) {
            throw InvalidInputException("GRAPH_INDEX option 'm' must be a valid integer");
        }
        if (m < 2 || m > 128) {
            throw InvalidInputException("GRAPH_INDEX option 'm' must be in [2, 128], got %d", m);
        }
    }
    auto ef_it = input.options.find("ef_construction");
    if (ef_it != input.options.end()) {
        try {
            ef_construction = ef_it->second.DefaultCastAs(LogicalType::INTEGER).GetValue<int>();
        } catch (...) {
            throw InvalidInputException("GRAPH_INDEX option 'ef_construction' must be a valid integer");
        }
        if (ef_construction < 1 || ef_construction > 10000) {
            throw InvalidInputException(
                "GRAPH_INDEX option 'ef_construction' must be in [1, 10000], got %d", ef_construction);
        }
    }
    auto metric_it = input.options.find("metric");
    if (metric_it != input.options.end()) {
        metric = ParseMetric(metric_it->second.GetValue<string>());
    }
    int build_threads = 1;
    auto threads_it = input.options.find("threads");
    if (threads_it != input.options.end()) {
        try {
            build_threads = threads_it->second.DefaultCastAs(LogicalType::INTEGER).GetValue<int>();
        } catch (...) {
            throw InvalidInputException("GRAPH_INDEX option 'threads' must be a valid integer in [1, 1024]");
        }
        if (build_threads < 1 || build_threads > 1024) {
            throw InvalidInputException("GRAPH_INDEX option 'threads' must be in [1, 1024], got %d", build_threads);
        }
    }
    uint32_t pq_m = 0;
    auto pq_m_it = input.options.find("pq_m");
    if (pq_m_it != input.options.end()) {
        int pq_m_val = 0;
        try {
            pq_m_val = pq_m_it->second.DefaultCastAs(LogicalType::INTEGER).GetValue<int>();
        } catch (...) {
            throw InvalidInputException("GRAPH_INDEX option 'pq_m' must be a valid integer");
        }
        if (pq_m_val < 0) {
            throw InvalidInputException("GRAPH_INDEX option 'pq_m' must be >= 0, got %d", pq_m_val);
        }
        pq_m = static_cast<uint32_t>(pq_m_val);
    }
    auto quantizer_it = input.options.find("quantizer");
    if (quantizer_it != input.options.end()) {
        auto qstr = StringUtil::Lower(quantizer_it->second.ToString());
        if (qstr == "pq") {
            if (pq_m == 0) {
                pq_m = ::vex::quantizer::ProductQuantizer::AutoSelectM(static_cast<uint32_t>(dimension));
            }
        } else if (qstr == "none" || qstr.empty()) {
            pq_m = 0;
        } else {
            throw InvalidInputException("GRAPH_INDEX got unknown quantizer '%s' (expected 'pq' or 'none')",
                                        quantizer_it->second.ToString());
        }
    }
    if (pq_m > 0 && dimension % pq_m != 0) {
        throw InvalidInputException("GRAPH_INDEX: pq_m (%u) must divide dimension (%llu)",
                                    pq_m, static_cast<unsigned long long>(dimension));
    }
    bool compact_mode = false;
    auto memmode_it = input.options.find("memory_mode");
    if (memmode_it != input.options.end()) {
        auto mstr = StringUtil::Lower(memmode_it->second.ToString());
        if (mstr == "compact") {
            compact_mode = true;
        } else if (mstr != "full" && !mstr.empty()) {
            throw InvalidInputException(
                "GRAPH_INDEX option 'memory_mode' must be 'full' or 'compact', got '%s'", mstr);
        }
    }
    if (compact_mode && pq_m == 0) {
        // Compact mode releases raw vectors post-train, so search must go
        // through PQ codes — auto-pick pq_m via the same heuristic used when
        // the user passes quantizer='pq' alone.
        pq_m = ::vex::quantizer::ProductQuantizer::AutoSelectM(static_cast<uint32_t>(dimension));
    }
    static const char *known_options[] = {"m", "ef_construction", "metric", "threads",
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
                                             metric, 0, pq_m, compact_mode, build_threads);

    if (input.storage_info.allocator_infos.size() >= 3) {
        // Reload path: create allocators WITHOUT slot-0 reservation. The serialized
        // bitmask already has slot 0 reserved from the original InitAllocators().
        // Reserving it again would corrupt buffers_with_free_space tracking.
        graph_index->runtime_->store.CreateAllocators(input.table_io_manager.GetIndexBlockManager());
        graph_index->DeserializeFromStorage(input.storage_info);
        graph_index->runtime_->store.normalize_vectors_ = (graph_index->metric_ == VexMetric::COSINE);
        return std::move(graph_index);
    }

    graph_index->runtime_->store.InitAllocators(input.table_io_manager.GetIndexBlockManager());

    auto manifest_it = input.storage_info.options.find("vex_graph_manifest");
    if (manifest_it != input.storage_info.options.end()) {
        auto manifest_blob = StringValue::Get(manifest_it->second.DefaultCastAs(LogicalType::BLOB));
        auto manifest = vex_disk::DeserializeManifest(manifest_blob);
        if (!manifest.segments.empty()) {
            auto &seg = manifest.segments[0];
            auto disk_blob = vex_disk::ReadBlobFromBlocks(input.table_io_manager.GetIndexBlockManager(),
                                                          QueryContext(input.context), seg.blocks, seg.size);
            graph_index->LoadFromDiskImage(disk_blob);
            graph_index->runtime_->store.normalize_vectors_ = (graph_index->metric_ == VexMetric::COSINE);
            return std::move(graph_index);
        }
    }
    auto blob_it = input.storage_info.options.find("vex_graph_blob");
    if (blob_it != input.storage_info.options.end()) {
        auto blob = StringValue::Get(blob_it->second.DefaultCastAs(LogicalType::BLOB));
        graph_index->LoadFromDiskImage(blob);
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
        proj_types.push_back(op.expressions[i]->GetReturnType());
        select_list.push_back(std::move(op.expressions[i]));
    }
    proj_types.emplace_back(LogicalType::ROW_TYPE);
    select_list.push_back(
        make_uniq<BoundReferenceExpression>(LogicalType::ROW_TYPE, op.info->scan_types.size() - 1));

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
    runtime_ = make_uniq<GraphIndexRuntimeState>(dimension_, m_);
    runtime_->store.InitAllocators(table_io_manager.GetIndexBlockManager());

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
        for (auto &w : workers) {
            w.join();
        }
        for (auto &ep : errors) {
            if (ep) {
                std::rethrow_exception(ep);
            }
        }
    });

    // When samples ≤ ksub (256 for nbits=8) the shared AnnKmeans takes the
    // QuickCenters fast-path (every sample becomes a centroid), so PQ works
    // at any non-empty row count. Compact mode in particular needs PQ to
    // function regardless of how few rows the user has.
    if (pq_m_ > 0 && !row_ids.empty()) {
        TrainAndEncodePQ(src, row_ids);
    }
    if (compact_mode_ && pq_use_) {
        ReleaseRawVectors();
    }
}

void GraphIndex::ReleaseRawVectors() {
    if (!runtime_) {
        return;
    }
    auto &store = runtime_->store;
    if (store.vector_alloc_) {
        store.vector_alloc_->Reset();
    }
    store.vectors.clear();
    store.vectors.shrink_to_fit();
}

void GraphIndex::TrainAndEncodePQ(const float *vec_data, const std::vector<row_t> &row_ids) {
    // Default PQContext: process-default mt19937(seed=42) random, std::malloc/free
    // allocator, serial parallel executor. The shared PQ never sees any
    // duck-specific types — backend swap is purely at this construction site.
    ::vex::quantizer::PQContext ctx;
    // Inject a thread-pool driver so each of the M sub-quantizer K-means runs
    // on its own std::thread. The shared algorithm writes only into its own
    // [m*ksub*dsub : (m+1)*ksub*dsub] slice of the centroids buffer and uses
    // std::malloc / thread_local mt19937, so no further synchronization is
    // needed. Exceptions from any worker are captured and the first is
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
                                  void * /*user*/) {
            // K-means inside each subquantizer also calls ctx.parallel.Run with
            // num_samples (~50K) and num_centers (256). Spawning a thread per
            // such task is catastrophic. Only parallelise the small outer loop
            // (M subquantizers, typically ≤ 64). Anything larger runs serial.
            constexpr size_t kMaxParallel = 64;
            if (n == 0) return;
            if (n == 1 || n > kMaxParallel) {
                for (size_t i = 0; i < n; i++) body(i);
                return;
            }
            std::vector<std::thread> workers;
            workers.reserve(n);
            std::vector<std::exception_ptr> errors(n);
            for (size_t i = 0; i < n; i++) {
                workers.emplace_back([i, &body, &errors]() {
                    try {
                        body(i);
                    } catch (...) {
                        errors[i] = std::current_exception();
                    }
                });
            }
            for (auto &t : workers) t.join();
            for (auto &ep : errors) {
                if (ep) std::rethrow_exception(ep);
            }
        };
    }
    ::vex::quantizer::KMeansState kmeans_state;
    // Distance kernel for K-means: naive L2 squared. Wired here rather than
    // through ann_helper::get_general_distance_func because that getter only
    // exists in the PG-only build target (src/distance/pg/distance.cpp). The
    // shared quantizer code stays backend-neutral; backends can plug in SIMD
    // versions later.
    kmeans_state.distance_fn = [](const void *a, const void *b, uint16_t d) -> float {
        const float *fa = static_cast<const float *>(a);
        const float *fb = static_cast<const float *>(b);
        float acc = 0.0f;
        for (uint16_t i = 0; i < d; i++) {
            float diff = fa[i] - fb[i];
            acc += diff * diff;
        }
        return acc;
    };
    kmeans_state.norm_fn     = nullptr;
    // Skip duplicate-center error for tiny test datasets — pq_m=4 + 8-dim
    // synthetic data trips it spuriously and adds no recall protection.
    kmeans_state.skip_check_duplicate = true;

    pq_quantizer_.set_basic_values(static_cast<size_t>(dimension_), pq_m_, /*nbits*/8);
    pq_quantizer_.set_derived_values(ctx);
    pq_quantizer_.set_fvec_L2sqr_ny_nearest_func();
    pq_quantizer_.set_fvec_ny_distance_func(Metric::L2);
    pq_quantizer_.set_dist_code_func();

    // Wrap incoming raw-float buffer as PQFloatArray (non-owning).
    ::vex::quantizer::PQFloatArray samples;
    samples.data   = const_cast<float *>(vec_data);
    samples.length = row_ids.size();
    samples.maxlen = row_ids.size();
    samples.dim    = dimension_;

    try {
        pq_quantizer_.train(kmeans_state, samples, /*avg_work_mem_kb*/0, ctx);
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
    for (idx_t i = 0; i < pq_row_id_order_.size(); i++) {
        auto src_idx = rid_to_idx[pq_row_id_order_[i]];
        pq_quantizer_.compute_code(vec_data + src_idx * dimension_, pq_codes_.data() + i * code_size);
    }
}

void GraphIndex::SearchPQ(const float *query_vec, idx_t k,
                          std::vector<row_t> &row_ids, std::vector<float> &distances,
                          double refine_factor) const {
    row_ids.clear();
    distances.clear();
    if (!pq_use_ || pq_codes_.empty()) {
        return;
    }
    // Refine only meaningful in non-compact (raw vec available) and factor>1.
    const bool refine = refine_factor > 1.0 && !compact_mode_ && runtime_;
    const idx_t pq_k = refine
        ? std::min<idx_t>(static_cast<idx_t>(k * refine_factor), pq_row_id_order_.size())
        : k;
    const idx_t target_k = k;
    k = pq_k;  // expand heap capacity for the PQ pass
    // For cosine indexes the stored codebook was trained on already-normalized
    // vectors (BuildBulk pre-normalizes). To keep the query in the same space
    // we normalize the incoming query too. L2 path uses the query as-is.
    std::vector<float> query_buf;
    const float *query = query_vec;
    if (metric_ == VexMetric::COSINE) {
        query_buf.assign(query_vec, query_vec + dimension_);
        NormalizeInPlace(query_buf.data(), dimension_);
        query = query_buf.data();
    }

    // dist_table[m * KSUB + j] = ||query_m - centroid(m, j)||^2
    auto code_size = pq_quantizer_.code_size;
    std::vector<float> dist_table(pq_quantizer_.M * pq_quantizer_.ksub);
    auto *self = const_cast<::vex::quantizer::ProductQuantizer *>(&pq_quantizer_);
    self->compute_distance_table(query, dist_table.data());

    // Walk all codes, keeping a max-heap of size k by approximate distance.
    // (deleted_rids_ filtered out before insertion so the heap never wastes
    // capacity on rows the table no longer has.)
    using Entry = std::pair<float, row_t>;
    auto cmp = [](const Entry &a, const Entry &b) { return a.first < b.first; };  // max-heap
    std::vector<Entry> heap;
    heap.reserve(k + 1);

    auto push_one = [&](float d, row_t rid) {
        if (heap.size() < k) {
            heap.emplace_back(d, rid);
            std::push_heap(heap.begin(), heap.end(), cmp);
        } else if (d < heap.front().first) {
            std::pop_heap(heap.begin(), heap.end(), cmp);
            heap.back() = {d, rid};
            std::push_heap(heap.begin(), heap.end(), cmp);
        }
    };

    const bool has_deleted = !deleted_rids_.empty();
    const idx_t n = pq_row_id_order_.size();
    const auto *codes_base = pq_codes_.data();

    // ADC fast path: process 4 codes per iteration via the SIMD
    // distance_to_four_code dispatcher (Stage 5 wired up sse/avx/avx512
    // variants of distance_four_codes_8/16/g). Walk in groups of 4 row
    // positions; if any of the 4 hits a deleted row_id fall back to per-row
    // for that group so deleted_rids_ filtering stays correct.
    idx_t i = 0;
    for (; i + 4 <= n; i += 4) {
        if (has_deleted) {
            bool any_deleted = false;
            for (idx_t k4 = 0; k4 < 4; k4++) {
                if (deleted_rids_.find(pq_row_id_order_[i + k4]) != deleted_rids_.end()) {
                    any_deleted = true;
                    break;
                }
            }
            if (any_deleted) {
                for (idx_t k4 = 0; k4 < 4; k4++) {
                    row_t rid = pq_row_id_order_[i + k4];
                    if (deleted_rids_.find(rid) != deleted_rids_.end()) continue;
                    float d = self->distance_to_code(codes_base + (i + k4) * code_size,
                                                     dist_table.data());
                    push_one(d, rid);
                }
                continue;
            }
        }
        float d0, d1, d2, d3;
        self->distance_to_four_code(dist_table.data(),
                                    codes_base + (i + 0) * code_size,
                                    codes_base + (i + 1) * code_size,
                                    codes_base + (i + 2) * code_size,
                                    codes_base + (i + 3) * code_size,
                                    d0, d1, d2, d3);
        push_one(d0, pq_row_id_order_[i + 0]);
        push_one(d1, pq_row_id_order_[i + 1]);
        push_one(d2, pq_row_id_order_[i + 2]);
        push_one(d3, pq_row_id_order_[i + 3]);
    }
    // Remainder (0-3 rows).
    for (; i < n; i++) {
        row_t rid = pq_row_id_order_[i];
        if (has_deleted && deleted_rids_.find(rid) != deleted_rids_.end()) continue;
        float d = self->distance_to_code(codes_base + i * code_size, dist_table.data());
        push_one(d, rid);
    }

    std::sort_heap(heap.begin(), heap.end(), cmp);

    if (refine && !heap.empty()) {
        // Re-rank top k*factor candidates using exact raw-vector distance.
        // Build a lazy row_id → store_id map by scanning store.elems on first
        // refine call, cached and invalidated on Append/Delete.
        if (pq_refine_rid_map_dirty_) {
            pq_refine_rid_map_.clear();
            const auto &elems = runtime_->store.elems;
            pq_refine_rid_map_.reserve(elems.size());
            for (uint32_t sid = 0; sid < elems.size(); sid++) {
                for (auto &tid : elems[sid].tids) {
                    pq_refine_rid_map_[tid.row_id] = sid;
                }
            }
            pq_refine_rid_map_dirty_ = false;
        }
        std::vector<float> refine_query;
        const float *rq = query_vec;
        if (metric_ == VexMetric::COSINE) {
            refine_query.assign(query_vec, query_vec + dimension_);
            NormalizeInPlace(refine_query.data(), dimension_);
            rq = refine_query.data();
        }
        std::vector<std::pair<float, row_t>> exact;
        exact.reserve(heap.size());
        for (auto &e : heap) {
            auto it = pq_refine_rid_map_.find(e.second);
            if (it == pq_refine_rid_map_.end()) continue;
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

    row_ids.reserve(heap.size());
    distances.reserve(heap.size());
    for (auto &e : heap) {
        row_ids.push_back(e.second);
        distances.push_back(e.first);
    }
}

void GraphIndex::SearchANN(const float *query_vec, idx_t k, int ef, std::vector<row_t> &row_ids,
                           std::vector<float> &distances) const {
    row_ids.clear();
    distances.clear();
    if (!runtime_) {
        return;
    }
    if (compact_mode_) {
        throw InvalidInputException(
            "GRAPH_INDEX memory_mode='compact': raw vectors were released after PQ training, "
            "SearchANN is unavailable. Use SET vex_pq_search_mode='pq_only'.");
    }
    auto &store = runtime_->store;
    PointExtensionContext point_ctx;
    idx_t needed = std::max<idx_t>(k, static_cast<idx_t>(ef));
    if (!deleted_rids_.empty()) {
        needed += deleted_rids_.size();
    }
    auto search_k = uint_fast16_t(std::min<idx_t>(needed, std::numeric_limits<uint_fast16_t>::max()));

    bool has_deleted = !deleted_rids_.empty();

    // If the graph entry node has been deleted, search starting from it can wander
    // into a stale subgraph (its neighbor links may all point to other deleted
    // nodes). Detect that case and let the algorithm pick a fresh entry from the
    // upper layers before searching. Building the internal-id deleted set is O(N)
    // so we only do it when the entry is actually deleted, which is rare.
    if (has_deleted && store.entry_info.id != INVALID_VECTOR_ID &&
        store.entry_info.id < store.elems.size()) {
        bool entry_deleted = false;
        for (auto &tid : store.elems[store.entry_info.id].tids) {
            if (deleted_rids_.find(tid.row_id) != deleted_rids_.end()) {
                entry_deleted = true;
                break;
            }
        }
        if (entry_deleted) {
            UnorderedSet<size_t> deleted_internal;
            for (size_t id = 0; id < store.elems.size(); id++) {
                for (auto &tid : store.elems[id].tids) {
                    if (deleted_rids_.find(tid.row_id) != deleted_rids_.end()) {
                        deleted_internal.insert(id);
                        break;
                    }
                }
            }
            RunWithDuckAlgo(metric_, dimension_, ef_construction_, m_, store, [&](auto &algo) {
                algo.repair_entry(deleted_internal);
            });
        }
    }

    RunWithDuckAlgo(metric_, dimension_, ef_construction_, m_, store, [&](auto &algo) {
        auto res = algo.search(point_ctx, reinterpret_cast<const char *>(query_vec), search_k);
        for (idx_t i = 0; i < res.first.size() && row_ids.size() < k; i++) {
            row_t rid = res.first[i].row_id;
            if (has_deleted && deleted_rids_.find(rid) != deleted_rids_.end()) {
                continue;
            }
            row_ids.push_back(rid);
            distances.push_back(res.second[i]);
        }
    });
}

ErrorData GraphIndex::Append(IndexLock &l, DataChunk &chunk, Vector &row_ids) {
    (void)l;
    auto count = chunk.size();
    if (count == 0) {
        return ErrorData();
    }

    if (!runtime_) {
        runtime_ = make_uniq<GraphIndexRuntimeState>(dimension_, m_);
        runtime_->store.normalize_vectors_ = (metric_ == VexMetric::COSINE);
    }
    if (!runtime_->store.node_alloc_ || !runtime_->store.vector_alloc_ || !runtime_->store.upper_alloc_) {
        runtime_->store.InitAllocators(table_io_manager.GetIndexBlockManager());
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
    auto vec_data = FlatVector::GetDataMutable<float>(child_vec);
    auto row_id_data = FlatVector::GetDataMutable<row_t>(row_ids);

    const bool pq_active = pq_use_;
    const bool pq_normalize = pq_active && metric_ == VexMetric::COSINE;
    const auto pq_code_size = pq_active ? pq_quantizer_.code_size : 0;
    // Compact mode released the raw vector tier; HNSW navigation has no
    // anchors so skip algo.insert. New rows reach search via pq_codes_ only.
    const bool skip_hnsw = compact_mode_ && pq_active;

    std::vector<float> pq_norm_buf;
    if (pq_normalize) {
        pq_norm_buf.resize(dim);
    }
    if (pq_active) {
        pq_codes_.reserve(pq_codes_.size() + count * pq_code_size);
        pq_row_id_order_.reserve(pq_row_id_order_.size() + count);
    }

    RunWithDuckAlgo(metric_, dim, ef_construction_, m_, runtime_->store, [&](auto &algo) {
        using AlgoT = std::decay_t<decltype(algo)>;
        for (idx_t i = 0; i < count; i++) {
            if (!vec_validity.RowIsValid(i)) {
                continue;
            }
            PointExtensionContext point_ctx;
            ItemPointerData tid;
            tid.row_id = row_id_data[i];
            if (!deleted_rids_.empty()) {
                deleted_rids_.erase(tid.row_id);
            }
            const float *vec_ptr = vec_data + i * dim;
            if (!skip_hnsw) {
                const char *query = reinterpret_cast<const char *>(vec_ptr);
                typename AlgoT::InsertContextBase insert_ctx(point_ctx, query, &tid);
                algo.insert(insert_ctx);
            }

            if (pq_active) {
                // Re-insert of a previously deleted row_id appends a second
                // entry; SearchPQ will then return both versions until a
                // Stage-8 retrain compacts the duplicates.
                const float *encode_src = vec_ptr;
                if (pq_normalize) {
                    std::memcpy(pq_norm_buf.data(), vec_ptr, dim * sizeof(float));
                    NormalizeInPlace(pq_norm_buf.data(), dim);
                    encode_src = pq_norm_buf.data();
                }
                size_t off = pq_codes_.size();
                pq_codes_.resize(off + pq_code_size);
                pq_quantizer_.compute_code(encode_src, pq_codes_.data() + off);
                pq_row_id_order_.push_back(tid.row_id);
            }
        }
    });
    pq_refine_rid_map_dirty_ = true;
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
    pq_refine_rid_map_dirty_ = true;
}

void GraphIndex::ResetStorage(IndexLock &index_lock) {
    (void)index_lock;
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

void GraphIndex::Vacuum(IndexLock &l) {
    (void)l;
}

idx_t GraphIndex::GetNodeCount() const {
    if (!runtime_) {
        return 0;
    }
    if (deleted_rids_.empty()) {
        return runtime_->store.elems.size();
    }
    // A node is "live" if at least one of its tracked row_ids has not been deleted.
    // Without dedup this collapses to "node_count == row_id_count"; with dedup a
    // node may carry many row_ids and survives until the last one is deleted.
    idx_t live = 0;
    for (auto &elem : runtime_->store.elems) {
        for (auto &tid : elem.tids) {
            if (deleted_rids_.find(tid.row_id) == deleted_rids_.end()) {
                live++;
                break;
            }
        }
    }
    return live;
}

idx_t GraphIndex::GetRowIdCount() const {
    if (!runtime_) {
        return 0;
    }
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

idx_t GraphIndex::GetInMemorySize(IndexLock &state) {
    (void)state;
    if (!runtime_) {
        return 0;
    }
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
    // P8' critical fix: reset atomic id counters to match restored counts.
    // Without this, the next assign_vector_id<true>() returns id=0 (from
    // default-initialized atomic) and clobbers existing node 0's segment.
    // Symptoms: SIGSEGV during the next Append's search_layer after restart.
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
        if (!compact_mode_flag && store.vector_alloc_ && header->vector_ptr.Get() &&
            i < store.vectors.size()) {
            auto *vec_data = reinterpret_cast<const char *>(store.vector_alloc_->Get(header->vector_ptr));
            if (vec_data) {
                store.vectors[i].assign(vec_data, vec_data + store.vec_size);
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

    auto pq_m_it = info.options.find("pq_m");
    auto pq_dim_it = info.options.find("pq_dim");
    auto pq_codebook_it = info.options.find("pq_codebook");
    auto pq_codes_it = info.options.find("pq_codes");
    auto pq_order_it = info.options.find("pq_row_order");
    if (pq_m_it != info.options.end() && pq_dim_it != info.options.end() &&
        pq_codebook_it != info.options.end() && pq_codes_it != info.options.end() &&
        pq_order_it != info.options.end()) {
        pq_m_ = pq_m_it->second.GetValue<uint32_t>();
        ::vex::quantizer::PQContext ctx;  // default allocator/random
        pq_quantizer_.set_basic_values(pq_dim_it->second.GetValue<uint32_t>(), pq_m_, /*nbits*/8);
        pq_quantizer_.set_derived_values(ctx);
        pq_quantizer_.set_fvec_L2sqr_ny_nearest_func();
        pq_quantizer_.set_fvec_ny_distance_func(Metric::L2);
        pq_quantizer_.set_dist_code_func();

        auto cb_blob = StringValue::Get(pq_codebook_it->second.DefaultCastAs(LogicalType::BLOB));
        const char *p = cb_blob.data();
        const char *end = p + cb_blob.size();
        if (p + sizeof(uint64_t) <= end) {
            uint64_t cn;
            std::memcpy(&cn, p, sizeof(cn)); p += sizeof(cn);
            if (cn == pq_quantizer_.get_centroids_size() && p + cn * sizeof(float) <= end) {
                std::memcpy(pq_quantizer_.centroids, p, cn * sizeof(float));
                pq_quantizer_.trained = true;
            }
        }

        auto codes_blob = StringValue::Get(pq_codes_it->second.DefaultCastAs(LogicalType::BLOB));
        p = codes_blob.data(); end = p + codes_blob.size();
        if (p + sizeof(uint64_t) <= end) {
            uint64_t n;
            std::memcpy(&n, p, sizeof(n)); p += sizeof(n);
            if (p + n <= end) {
                pq_codes_.assign(p, p + n);
            }
        }

        auto order_blob = StringValue::Get(pq_order_it->second.DefaultCastAs(LogicalType::BLOB));
        p = order_blob.data(); end = p + order_blob.size();
        if (p + sizeof(uint64_t) <= end) {
            uint64_t n;
            std::memcpy(&n, p, sizeof(n)); p += sizeof(n);
            pq_row_id_order_.clear();
            pq_row_id_order_.reserve(n);
            for (uint64_t i = 0; i < n && p + sizeof(int64_t) <= end; i++) {
                int64_t v;
                std::memcpy(&v, p, sizeof(v)); p += sizeof(v);
                pq_row_id_order_.push_back(static_cast<row_t>(v));
            }
        }

        pq_use_ = pq_quantizer_.trained && !pq_codes_.empty();
    }

    auto compact_it = info.options.find("compact_mode");
    if (compact_it != info.options.end()) {
        compact_mode_ = compact_it->second.GetValue<bool>();
        if (compact_mode_) {
            // Raw vectors weren't persisted (or are stale); make sure the
            // post-reload SearchANN guard fires by clearing any in-memory copy.
            ReleaseRawVectors();
        }
    }
}

} // namespace duckdb
