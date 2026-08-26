#pragma once

#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/execution/index/index_type.hpp"
#include "duckdb/storage/table/scan_state.hpp"

#include "vex_graph_index_depend_duck.hpp"
#include "vex_distance.hpp"
#include "quantizer/product_quantizer.h"

#include <unordered_set>

namespace duckdb {

class PhysicalOperator;
using DuckStore = MemStore<uint32, GraphIndexPoint>;

struct GraphIndexRuntimeState {
    explicit GraphIndexRuntimeState(idx_t dimension, int m, Allocator &mirror_allocator)
        : store(uint_fast16_t(dimension), uint_fast16_t(m), uint_fast32_t(dimension * sizeof(float))) {
        store.SetMirrorAllocator(mirror_allocator);
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

struct GraphIndexRowIdCoverage {
    idx_t live_count = 0;
    idx_t rowid_upper_bound = 0;
    uint64_t rowid_checksum = 0;
    uint64_t vector_checksum = 0;
    bool has_vector_checksum = false;
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
               AttachedDatabase &db, idx_t dimension, int m, int ef_construction, VexMetric metric,
               idx_t vec_column_index, uint32_t pq_m = 0, bool compact_mode = false,
               int build_threads = 1);

    void BuildBulk(const std::vector<float> &vectors, const std::vector<row_t> &row_ids);
    void SearchANN(const float *query_vec, idx_t k, int ef, std::vector<row_t> &row_ids,
                   std::vector<float> &distances) const;
    // Brute-force scan over PQ codes using a precomputed distance table. Skips
    // the HNSW graph entirely; result is approximate but the per-row cost is
    // an M-byte lookup vs a dim-float dot product, so this is faster than a
    // raw seq_scan for indexes that fit in memory.
    // refine_factor > 1.0 takes top k*factor by PQ distance then re-ranks via
    // raw vector. Ignored in compact_mode_ (no raw vec). 1.0 = no refine.
    void SearchPQ(const float *query_vec, idx_t k, std::vector<row_t> &row_ids,
                  std::vector<float> &distances, double refine_factor = 1.0) const;

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
    idx_t GetRowIdCount() const;
    GraphIndexRowIdCoverage GetRowIdCoverage() const;
    bool HasVectorCoverageChecksum() const;
    bool UsesPQCoverageChecksum() const;
    uint64_t HashPQVectorForCoverage(row_t row_id, const float *vec) const;
    bool HasRowIdCoverageCheck() const { return rowid_coverage_checked_; }
    bool IsRowIdCoverageStale() const { return rowid_coverage_stale_; }
    void MarkRowIdCoverageChecked(bool stale) {
        rowid_coverage_checked_ = true;
        rowid_coverage_stale_ = stale;
    }
    // HNSW entry-point level (top layer). -1 when the index has no nodes.
    int GetMaxLevel() const;
    bool UsesPQ() const { return pq_use_; }
    uint32_t GetPQM() const { return pq_use_ ? static_cast<uint32_t>(pq_quantizer_.M) : 0u; }
    idx_t GetPQCodesBytes() const { return pq_codes_.size(); }
    idx_t GetPQCodebookBytes() const {
        return pq_use_ ? pq_quantizer_.get_centroids_size() * sizeof(float) : 0u;
    }
    bool IsCompactMode() const { return compact_mode_; }

public:
    ErrorData Append(IndexLock &l, DataChunk &chunk, Vector &row_ids) override;
    ErrorData Append(IndexLock &l, DataChunk &chunk, Vector &row_ids, IndexAppendInfo &info) override;
    void VerifyAppend(DataChunk &chunk, IndexAppendInfo &info, optional_ptr<ConflictManager> manager) override;
    void VerifyConstraint(DataChunk &chunk, IndexAppendInfo &info, ConflictManager &manager) override;
    void Delete(IndexLock &state, DataChunk &entries, Vector &row_identifiers) override;
    // DuckDB renamed this hook from CommitDrop (v1.5.2) to ResetStorage
    // (v1.5.5). Keep both signatures and mark the active API as the override.
#ifdef VEXDB_DUCKDB_HAS_RESET_STORAGE
    void CommitDrop(IndexLock &index_lock);
    void ResetStorage(IndexLock &index_lock) override;
#else
    void CommitDrop(IndexLock &index_lock) override;
    void ResetStorage(IndexLock &index_lock);
#endif
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

    // graph_memory_limit (bytes) — byte budget for MemStore's in-memory raw-vector
    // mirror (vectors[]). Captured from the vexdb_graph_memory_limit setting where a
    // ClientContext is available (Create / PhysicalVexCreateIndex::Finalize) and applied
    // to the store after each InitAllocators via ApplyMirrorBudget(). 0 = unlimited.
    idx_t graph_memory_limit_bytes_ = 0;
    // Translate graph_memory_limit_bytes_ → store.mirror_max_nodes_ for the current
    // runtime_->store. Only tightens when vector_alloc_ exists (over-budget nodes need
    // the buffer-manager-backed copy as their home); otherwise leaves it unlimited.
    void ApplyMirrorBudget();

private:
    IndexStorageInfo ExportStorageInfo() const;
    std::string BuildDiskImage() const;
    void LoadFromDiskImage(const std::string &blob);
    void DeserializeFromStorage(const IndexStorageInfo &info);
    void DeserializePQAndModeFromStorage(const IndexStorageInfo &info);
    void TrainAndEncodePQ(const float *vec_data, const std::vector<row_t> &row_ids);

    idx_t dimension_;
    int m_;
    int ef_construction_;
    // Parsed from WITH (threads=N). Default 1 = serial. >1 enables std::thread
    // pool in BuildBulk (P5'). Must respect MemStore thread-safety contract.
    int build_threads_ = 1;
    VexMetric metric_;
    idx_t vec_column_index_;

    std::unique_ptr<GraphIndexRuntimeState> runtime_;
    std::unordered_set<row_t> deleted_rids_;

    // Index-level reader/writer lock. SearchANN/SearchPQ take it SHARED for the
    // whole query; Append/Insert/Delete/index reset take it EXCLUSIVE. This makes
    // the per-node reader lock inside the HNSW walk redundant during search
    // (no writer can run concurrently), so search sets store.search_lock_free_
    // and skips it — eliminating hub-node reader-byte cacheline contention that
    // collapsed throughput at high read concurrency. mutable: the search
    // methods are const. The parallel BuildBulk path runs before any query and
    // is not gated by this lock (it relies on the per-node locks internally).
    mutable vex_duck::SimpleRWLock graph_rwlock_;

    // Product Quantization state. pq_m_ = 0 / pq_use_ = false means PQ disabled.
    // Once Train() runs (after BuildBulk completes), pq_quantizer_.trained = true
    // and pq_codes_ holds m bytes per row, indexed by row_id sort order so reload
    // can restore the alignment.
    uint32_t pq_m_ = 0;
    bool pq_use_ = false;
    ::vex::quantizer::ProductQuantizer pq_quantizer_;
    std::vector<uint8_t> pq_codes_;
    std::vector<row_t> pq_row_id_order_;
    std::vector<uint64_t> pq_vector_coverage_hashes_;

    // memory_mode='compact' (PQ-only). After BuildBulk + TrainAndEncodePQ
    // releases the raw vector tier, SearchANN refuses to run and post-build
    // INSERTs encode into pq_codes_ without traversing the (now-broken) HNSW
    // graph. Persisted across index reset / Vacuum / Reload.
    bool compact_mode_ = false;

    // Lazy row_id → store_id index used by SearchPQ refine. Built on first
    // refine query, invalidated on Append / Delete / index reset.
    mutable std::unordered_map<row_t, uint32_t> pq_refine_rid_map_;
    mutable bool pq_refine_rid_map_dirty_ = true;

    bool rowid_coverage_checked_ = false;
    bool rowid_coverage_stale_ = false;

    // Free the raw vector tier and clear the in-memory copy. Idempotent.
    void ReleaseRawVectors();
};

} // namespace duckdb
