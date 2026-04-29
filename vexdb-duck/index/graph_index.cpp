#include "vex_graph_index.hpp"

#include "graph_index/graph_index_algorithm.h"

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

using DuckDistancer =
    Distancer<Arch::GENERAL, Metric::L2, DistPrecisionType::FLOAT, RemainderSituation::Unknown, false>;
using DuckAlgo = GraphIndexAlgorithm<DuckStore, DuckDistancer>;

VexMetric ParseMetric(const string &metric_name) {
    if (StringUtil::Lower(metric_name) == "l2") {
        return VexMetric::L2;
    }
    throw InvalidInputException("Unsupported GRAPH_INDEX metric: %s", metric_name);
}

GraphIndex::GraphIndex(const string &name, IndexConstraintType constraint_type, const vector<column_t> &column_ids,
                       TableIOManager &table_io_manager, const vector<unique_ptr<Expression>> &unbound_expressions,
                       AttachedDatabase &db, idx_t dimension, int m, int ef_construction, VexMetric metric)
    : BoundIndex(name, TYPE_NAME, constraint_type, column_ids, table_io_manager, unbound_expressions, db),
      dimension_(dimension), m_(m), ef_construction_(ef_construction), metric_(metric),
      runtime_(make_uniq<GraphIndexRuntimeState>(dimension, m)) {
}

unique_ptr<BoundIndex> GraphIndex::Create(CreateIndexInput &input) {
    if (input.unbound_expressions.empty()) {
        throw InvalidInputException("GRAPH_INDEX requires at least one indexed expression");
    }

    auto &vec_type = input.unbound_expressions[0]->return_type;
    if (vec_type.id() != LogicalTypeId::ARRAY || ArrayType::GetChildType(vec_type).id() != LogicalTypeId::FLOAT) {
        throw InvalidInputException("GRAPH_INDEX first column must be FLOAT[N], got %s", vec_type.ToString());
    }

    idx_t dimension = ArrayType::GetSize(vec_type);
    int m = 16;
    int ef_construction = 64;
    VexMetric metric = VexMetric::L2;

    auto m_it = input.options.find("m");
    if (m_it != input.options.end()) {
        m = m_it->second.DefaultCastAs(LogicalType::INTEGER).GetValue<int>();
    }
    auto ef_it = input.options.find("ef_construction");
    if (ef_it != input.options.end()) {
        ef_construction = ef_it->second.DefaultCastAs(LogicalType::INTEGER).GetValue<int>();
    }
    auto metric_it = input.options.find("metric");
    if (metric_it != input.options.end()) {
        metric = ParseMetric(metric_it->second.GetValue<string>());
    }

    auto graph_index = make_uniq<GraphIndex>(input.name, input.constraint_type, input.column_ids, input.table_io_manager,
                                             input.unbound_expressions, input.db, dimension, m, ef_construction,
                                             metric);
    auto manifest_it = input.storage_info.options.find("vex_graph_manifest");
    if (manifest_it != input.storage_info.options.end()) {
        auto manifest_blob = StringValue::Get(manifest_it->second.DefaultCastAs(LogicalType::BLOB));
        auto manifest = vex_disk::DeserializeManifest(manifest_blob);
        if (!manifest.segments.empty()) {
            auto &seg = manifest.segments[0];
            auto disk_blob = vex_disk::ReadBlobFromBlocks(input.table_io_manager.GetIndexBlockManager(),
                                                          QueryContext(input.context), seg.blocks, seg.size);
            graph_index->LoadFromDiskImage(disk_blob);
            return std::move(graph_index);
        }
    }
    auto blob_it = input.storage_info.options.find("vex_graph_blob");
    if (blob_it != input.storage_info.options.end()) {
        auto blob = StringValue::Get(blob_it->second.DefaultCastAs(LogicalType::BLOB));
        graph_index->LoadFromDiskImage(blob);
    }
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

void GraphIndex::BuildBulk(const std::vector<float> &vectors, const std::vector<row_t> &row_ids, idx_t dimension) {
    if (dimension_ != dimension) {
        throw InvalidInputException("GRAPH_INDEX dimension mismatch: expected %llu, got %llu",
                                    static_cast<unsigned long long>(dimension_),
                                    static_cast<unsigned long long>(dimension));
    }
    vectors_ = vectors;
    row_ids_ = row_ids;

    runtime_ = make_uniq<GraphIndexRuntimeState>(dimension_, m_);
    DuckDistancer distancer;
    DuckAlgo algo(uint_fast16_t(ef_construction_), uint_fast16_t(m_), runtime_->store, distancer);

    for (idx_t i = 0; i < row_ids.size(); i++) {
        PointExtensionContext point_ctx;
        ItemPointerData tid;
        tid.row_id = row_ids[i];
        const char *query = reinterpret_cast<const char *>(vectors.data() + i * dimension_);
        DuckAlgo::InsertContextBase insert_ctx(point_ctx, query, &tid);
        algo.insert(insert_ctx);
    }
}

void GraphIndex::SearchANN(const float *query_vec, idx_t k, int ef, std::vector<row_t> &row_ids,
                           std::vector<float> &distances) const {
    row_ids.clear();
    distances.clear();
    if (!runtime_) {
        return;
    }
    if (metric_ != VexMetric::L2) {
        throw NotImplementedException("Only L2 metric is implemented in GRAPH_INDEX");
    }

    DuckDistancer distancer;
    auto &store = runtime_->store;
    DuckAlgo algo(uint_fast16_t(ef_construction_), uint_fast16_t(m_), store, distancer);
    PointExtensionContext point_ctx;
    auto res = algo.search(point_ctx, reinterpret_cast<const char *>(query_vec), uint_fast16_t(std::max<idx_t>(k, ef)));
    for (auto &tid : res.first) {
        row_ids.push_back(tid.row_id);
    }
    for (auto &dist : res.second) {
        distances.push_back(dist);
    }
}

ErrorData GraphIndex::Append(IndexLock &l, DataChunk &chunk, Vector &row_ids) {
    (void)l;
    (void)chunk;
    (void)row_ids;
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
    vectors_.clear();
    row_ids_.clear();
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

idx_t GraphIndex::GetInMemorySize(IndexLock &state) {
    (void)state;
    return static_cast<idx_t>(vectors_.size() * sizeof(float) + row_ids_.size() * sizeof(row_t));
}

void GraphIndex::Verify(IndexLock &l) {
    (void)l;
}

string GraphIndex::ToString(IndexLock &l, bool display_ascii) {
    (void)l;
    (void)display_ascii;
    return StringUtil::Format("GRAPH_INDEX(dim=%llu, m=%d, ef_construction=%d, rows=%llu)",
                              static_cast<unsigned long long>(dimension_), m_, ef_construction_,
                              static_cast<unsigned long long>(row_ids_.size()));
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

} // namespace duckdb
