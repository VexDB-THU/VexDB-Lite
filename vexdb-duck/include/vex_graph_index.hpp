#pragma once

#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/execution/index/index_type.hpp"
#include "duckdb/storage/table/scan_state.hpp"

#include "vex_graph_index_depend_duck.hpp"
#include "vex_distance.hpp"

#include <unordered_set>

namespace duckdb {

class PhysicalOperator;
using DuckStore = MemStore<uint32, GraphIndexPoint>;

struct GraphIndexRuntimeState {
    explicit GraphIndexRuntimeState(idx_t dimension, int m)
        : store(uint_fast16_t(dimension), uint_fast16_t(m), uint_fast32_t(dimension * sizeof(float))) {
    }

    DuckStore store;
};

struct GraphIndexScanState : public IndexScanState {
    std::vector<float> query_vector;
    std::vector<row_t> row_ids;
    std::vector<float> distances;
    idx_t current_offset = 0;
    idx_t k = 0;
};

class GraphIndex : public BoundIndex {
public:
    static constexpr const char *TYPE_NAME = "GRAPH_INDEX";

    static unique_ptr<BoundIndex> Create(CreateIndexInput &input);
    static PhysicalOperator &CreatePlan(PlanIndexInput &input);

public:
    GraphIndex(const string &name, IndexConstraintType constraint_type,
               const vector<column_t> &column_ids, TableIOManager &table_io_manager,
               const vector<unique_ptr<Expression>> &unbound_expressions,
               AttachedDatabase &db, idx_t dimension, int m, int ef_construction, VexMetric metric);

    void BuildBulk(const std::vector<float> &vectors, const std::vector<row_t> &row_ids);
    void SearchANN(const float *query_vec, idx_t k, int ef, std::vector<row_t> &row_ids,
                   std::vector<float> &distances) const;

    idx_t GetDimension() const {
        return dimension_;
    }
    int GetM() const {
        return m_;
    }
    int GetEfConstruction() const {
        return ef_construction_;
    }
    VexMetric GetMetric() const {
        return metric_;
    }

    idx_t GetNodeCount() const;

public:
    ErrorData Append(IndexLock &l, DataChunk &chunk, Vector &row_ids) override;
    ErrorData Append(IndexLock &l, DataChunk &chunk, Vector &row_ids, IndexAppendInfo &info) override;
    void VerifyAppend(DataChunk &chunk, IndexAppendInfo &info, optional_ptr<ConflictManager> manager) override;
    void VerifyConstraint(DataChunk &chunk, IndexAppendInfo &info, ConflictManager &manager) override;
    void Delete(IndexLock &state, DataChunk &entries, Vector &row_identifiers) override;
    void CommitDrop(IndexLock &index_lock) override;
    ErrorData Insert(IndexLock &l, DataChunk &chunk, Vector &row_ids) override;
    ErrorData Insert(IndexLock &l, DataChunk &chunk, Vector &row_ids, IndexAppendInfo &info) override;
    bool MergeIndexes(IndexLock &state, BoundIndex &other_index) override;
    void Vacuum(IndexLock &l) override;
    IndexStorageInfo SerializeToDisk(QueryContext context, const case_insensitive_map_t<Value> &options) override;
    IndexStorageInfo SerializeToWAL(const case_insensitive_map_t<Value> &options) override;
    idx_t GetInMemorySize(IndexLock &state) override;
    void Verify(IndexLock &l) override;
    string ToString(IndexLock &l, bool display_ascii = false) override;
    void VerifyAllocations(IndexLock &l) override;
    void VerifyBuffers(IndexLock &l) override;
    string GetConstraintViolationMessage(VerifyExistenceType verify_type, idx_t failed_index,
                                         DataChunk &input) override;

private:
    IndexStorageInfo ExportStorageInfo() const;
    std::string BuildDiskImage() const;
    void LoadFromDiskImage(const std::string &blob);
    void DeserializeFromStorage(const IndexStorageInfo &info);

    idx_t dimension_;
    int m_;
    int ef_construction_;
    VexMetric metric_;

    std::unique_ptr<GraphIndexRuntimeState> runtime_;
    std::unordered_set<row_t> deleted_rids_;
};

} // namespace duckdb
