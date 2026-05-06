#include "vex_graph_index.hpp"

#include <set>

#include "graph_index/graph_index_algorithm.h"
#include "distance/core/distance_dispatcher.h"

#include "vex/vex_disk_block_store.hpp"
#include "vex_distance.hpp"
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
                       AttachedDatabase &db, idx_t dimension, int m, int ef_construction, VexMetric metric)
    : BoundIndex(name, TYPE_NAME, constraint_type, column_ids, table_io_manager, unbound_expressions, db),
      dimension_(dimension), m_(m), ef_construction_(ef_construction), metric_(metric),
      runtime_(make_uniq<GraphIndexRuntimeState>(dimension, m)) {
    runtime_->store.normalize_vectors_ = (metric_ == VexMetric::COSINE);
}

unique_ptr<BoundIndex> GraphIndex::Create(CreateIndexInput &input) {
    if (input.unbound_expressions.empty()) {
        throw InvalidInputException("GRAPH_INDEX requires at least one indexed expression");
    }

    auto &vec_type = input.unbound_expressions[0]->return_type;
    if (vec_type.id() != LogicalTypeId::ARRAY || ArrayType::GetChildType(vec_type).id() != LogicalTypeId::FLOAT) {
        throw InvalidInputException("GRAPH_INDEX first column must be FLOAT[N], got %s", vec_type.ToString());
    }

    // Only the first column may be the FLOAT[N] vector. Any subsequent vector columns
    // are rejected because vexdb-duck does not yet support multi-vector indexes.
    for (idx_t i = 1; i < input.unbound_expressions.size(); i++) {
        auto &t = input.unbound_expressions[i]->return_type;
        if (t.id() == LogicalTypeId::ARRAY && ArrayType::GetChildType(t).id() == LogicalTypeId::FLOAT) {
            throw InvalidInputException(
                "GRAPH_INDEX accepts only the first column may be a vector "
                "(got an additional FLOAT[%llu] at position %llu)",
                static_cast<unsigned long long>(ArrayType::GetSize(t)),
                static_cast<unsigned long long>(i));
        }
    }
    // Reject duplicate columns. DuckDB collapses duplicates in `column_ids`, so the
    // mismatch between unbound_expressions size and column_ids size is the signal.
    if (input.unbound_expressions.size() > input.column_ids.size()) {
        throw InvalidInputException(
            "GRAPH_INDEX rejects duplicate column in expression list (%llu expressions, %llu unique columns)",
            static_cast<unsigned long long>(input.unbound_expressions.size()),
            static_cast<unsigned long long>(input.column_ids.size()));
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
    auto threads_it = input.options.find("threads");
    if (threads_it != input.options.end()) {
        int threads_val = 0;
        try {
            threads_val = threads_it->second.DefaultCastAs(LogicalType::INTEGER).GetValue<int>();
        } catch (...) {
            throw InvalidInputException("GRAPH_INDEX option 'threads' must be a valid integer in [1, 1024]");
        }
        if (threads_val < 1 || threads_val > 1024) {
            throw InvalidInputException("GRAPH_INDEX option 'threads' must be in [1, 1024], got %d", threads_val);
        }
        // vexdb-duck currently builds single-threaded; the option is accepted but ignored.
    }
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
        // vexdb-duck does not yet implement PQ; option is accepted for compatibility but ignored.
    }
    // Reject unknown options.
    static const char *known_options[] = {"m", "ef_construction", "metric", "threads", "quantizer", "pq_m"};
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
                                             metric);

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
        proj_types.push_back(op.expressions[i]->return_type);
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

void GraphIndex::BuildBulk(const std::vector<float> &vectors, const std::vector<row_t> &row_ids) {
    runtime_ = make_uniq<GraphIndexRuntimeState>(dimension_, m_);
    runtime_->store.normalize_vectors_ = (metric_ == VexMetric::COSINE);
    runtime_->store.InitAllocators(table_io_manager.GetIndexBlockManager());

    RunWithDuckAlgo(metric_, dimension_, ef_construction_, m_, runtime_->store, [&](auto &algo) {
        using AlgoT = std::decay_t<decltype(algo)>;
        for (idx_t i = 0; i < row_ids.size(); i++) {
            PointExtensionContext point_ctx;
            ItemPointerData tid;
            tid.row_id = row_ids[i];
            const char *query = reinterpret_cast<const char *>(vectors.data() + i * dimension_);
            typename AlgoT::InsertContextBase insert_ctx(point_ctx, query, &tid);
            algo.insert(insert_ctx);
        }
    });
}

void GraphIndex::SearchANN(const float *query_vec, idx_t k, int ef, std::vector<row_t> &row_ids,
                           std::vector<float> &distances) const {
    row_ids.clear();
    distances.clear();
    if (!runtime_) {
        return;
    }
    auto &store = runtime_->store;
    PointExtensionContext point_ctx;
    auto search_k = uint_fast16_t(std::max<idx_t>(k, ef));

    RunWithDuckAlgo(metric_, dimension_, ef_construction_, m_, store, [&](auto &algo) {
        auto res = algo.search(point_ctx, reinterpret_cast<const char *>(query_vec), search_k);
        for (auto &tid : res.first) row_ids.push_back(tid.row_id);
        for (auto &dist : res.second) distances.push_back(dist);
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
    auto vec_col_idx = static_cast<idx_t>(column_ids[0]);
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

    RunWithDuckAlgo(metric_, dim, ef_construction_, m_, runtime_->store, [&](auto &algo) {
        using AlgoT = std::decay_t<decltype(algo)>;
        for (idx_t i = 0; i < count; i++) {
            if (!vec_validity.RowIsValid(i)) {
                continue;
            }
            PointExtensionContext point_ctx;
            ItemPointerData tid;
            tid.row_id = row_id_data[i];
            const char *query = reinterpret_cast<const char *>(vec_data + i * dim);
            typename AlgoT::InsertContextBase insert_ctx(point_ctx, query, &tid);
            algo.insert(insert_ctx);
        }
    });
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
    (void)entries;
    (void)row_identifiers;
}

void GraphIndex::CommitDrop(IndexLock &index_lock) {
    (void)index_lock;
    if (runtime_) {
        if (runtime_->store.node_alloc_) {
            runtime_->store.node_alloc_->Reset();
        }
        if (runtime_->store.vector_alloc_) {
            runtime_->store.vector_alloc_->Reset();
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
    return runtime_->store.elems.size();
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
}

} // namespace duckdb
