#pragma once

#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/storage/index.hpp"
#include "duckdb/common/types/vector.hpp"
#include "vex_graph_index_core.hpp"

#include <random>
#include <map>

namespace duckdb {

// ============================================================
// Hybrid Index - partitioned vector index with scalar filtering
// ============================================================
class HybridIndex : public BoundIndex {
public:
	static constexpr const char *TYPE_NAME = "HYBRID_INDEX";

	static unique_ptr<BoundIndex> Create(CreateIndexInput &input);
	static PhysicalOperator &CreatePlan(PlanIndexInput &input);

public:
	HybridIndex(const string &name, IndexConstraintType constraint_type,
	            const vector<column_t> &column_ids, TableIOManager &table_io_manager,
	            const vector<unique_ptr<Expression>> &unbound_expressions,
	            AttachedDatabase &db, int m, int ef_construction,
	            vex::VexMetric metric = vex::VexMetric::L2);

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

	// Hybrid Index specific API
	void FilteredSearch(const string &partition_key, const float *query_vec, idx_t k, int ef,
	                    std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
	                    idx_t brute_force_threshold = GraphIndexCore::BRUTE_FORCE_THRESHOLD);
	void GlobalSearch(const float *query_vec, idx_t k, int ef,
	                  std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
	                  idx_t brute_force_threshold = GraphIndexCore::BRUTE_FORCE_THRESHOLD);
	std::vector<string> GetPartitionKeys() const;
	idx_t GetTotalNodeCount() const;
	static string ValueToPartitionKey(const Value &val);

	//! Access partitions (for vex_index_info)
	const std::map<string, GraphIndexCore> &GetPartitions() const {
		return partitions_;
	}

	vex::VexMetric GetMetric() const {
		return metric_;
	}

private:
	string SerializeToBlob();
	bool DeserializeFromBlob(const string &blob);
	void DeserializeFromStorage(const IndexStorageInfo &info);
	void Clear();
	GraphIndexCore &GetOrCreatePartition(const string &key);
	void EnsurePartitionAllocators(GraphIndexCore &partition);
	int GetRandomLevel();

private:
	int m_;
	int ef_construction_;
	uint32_t dimension_;

	//! Partitions: scalar_key_string -> HNSW graph
	std::map<string, GraphIndexCore> partitions_;

	//! Reverse lookup: row_id -> partition key (for O(1) delete)
	unordered_map<row_t, string> row_partition_map_;

	std::mt19937 rng_;
	std::uniform_real_distribution<double> dist_;
	vex::VexMetric metric_;
	vex::distance_func_t distance_func_;
};

} // namespace duckdb
