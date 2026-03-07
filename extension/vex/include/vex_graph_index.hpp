#pragma once

#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/storage/index.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/common/types/vector.hpp"
#include "vex_graph_index_core.hpp"

#include <random>

namespace duckdb {

// ============================================================
// Index Scan State for Graph Index
// ============================================================
struct GraphIndexScanState : public IndexScanState {
	std::vector<float> query_vector;
	std::vector<row_t> row_ids;
	std::vector<float> distances;
	idx_t current_offset;
	idx_t k;
	int ef;
	bool initialized;

	GraphIndexScanState() : current_offset(0), k(10), ef(0), initialized(false) {}
};

// ============================================================
// Graph Index (HNSW implementation)
// ============================================================
class GraphIndex : public BoundIndex {
public:
	static constexpr const char *TYPE_NAME = "GRAPH_INDEX";

	static unique_ptr<BoundIndex> Create(CreateIndexInput &input);
	static PhysicalOperator &CreatePlan(PlanIndexInput &input);

public:
	GraphIndex(const string &name, IndexConstraintType constraint_type,
	           const vector<column_t> &column_ids, TableIOManager &table_io_manager,
	           const vector<unique_ptr<Expression>> &unbound_expressions,
	           AttachedDatabase &db, int m, int ef_construction,
	           bool use_pq = false, uint32_t pq_m = 0);

	void Build(DataChunk &chunk, Vector &row_ids);

	// BoundIndex interface
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
	string VerifyAndToString(IndexLock &l, const bool only_verify) override;
	void VerifyAllocations(IndexLock &l) override;
	void VerifyBuffers(IndexLock &l) override;
	string GetConstraintViolationMessage(VerifyExistenceType verify_type, idx_t failed_index,
	                                     DataChunk &input) override;

	// Graph Index specific API
	void Search(const float *query_vec, idx_t k, int ef,
	            std::vector<row_t> &out_row_ids, std::vector<float> &out_distances);
	void ANNSearch(const float *query_vec, idx_t k, int ef,
	               std::vector<row_t> &out_row_ids, std::vector<float> &out_distances);

	// Index Scan Interface
	static unique_ptr<IndexScanState> TryInitializeScan(const Expression &expr, const Expression &filter_expr);
	bool Scan(IndexScanState &state, idx_t max_count, set<row_t> &row_ids);

	GraphNode *GetEntryPoint() const {
		return graph_.entry_point;
	}

	int GetMaxLevel() const {
		return graph_.max_level;
	}

private:
	int GetRandomLevel();
	string SerializeToBlob() const;
	bool DeserializeFromBlob(const string &blob);
	void Clear();

private:
	int m_;
	int ef_construction_;
	uint32_t dimension_;
	bool use_pq_;
	uint32_t pq_m_;

	//! Shared graph index core
	GraphIndexCore graph_;

	std::mt19937 rng_;
	std::uniform_real_distribution<double> dist_;
	vex::distance_func_t distance_func_;
};

} // namespace duckdb
