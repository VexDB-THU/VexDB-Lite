#pragma once

#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/execution/index/index_type.hpp"
#include "duckdb/storage/table/scan_state.hpp"

#include "vex_graph_index_depend_duck.hpp"
#include "vex_distance.hpp"
#include "quantizer/product_quantizer.h"
#include "rabitq/rabitq.h"

#include <atomic>
#include <functional>
#include <unordered_map>
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
               int build_threads = 1, bool rabitq_requested = false);
    ~GraphIndex() override;

    void BuildBulk(const std::vector<float> &vectors, const std::vector<row_t> &row_ids);
    // Build from a read-only, random-access vector source. Rebuild can use a
    // memory-mapped temporary file here so the full table vector payload does
    // not have to live in the process heap.
    void BuildBulk(const float *vectors, idx_t vector_count, const std::vector<row_t> &row_ids,
                   bool vectors_are_normalized = false);
    void RebuildBulk(const std::vector<float> &vectors, const std::vector<row_t> &row_ids);
    void RebuildBulk(const float *vectors, idx_t vector_count, const std::vector<row_t> &row_ids,
                     bool vectors_are_normalized = false,
                     std::function<void()> release_vector_source = nullptr);
    void SearchANN(const float *query_vec, idx_t k, int ef, std::vector<row_t> &row_ids,
                   std::vector<float> &distances) const;
    // PQ ADC search over the shared graph topology. The graph keeps query cost
    // bounded by ef instead of scanning every code as the table grows.
    // refine_factor > 1.0 takes top k*factor by PQ distance then re-ranks via
    // raw vector. Ignored in compact_mode_ (no raw vec). 1.0 = no refine.
    void SearchPQ(const float *query_vec, idx_t k, int ef, std::vector<row_t> &row_ids,
                  std::vector<float> &distances, double refine_factor = 1.0) const;
    void SearchRaBitQ(const float *query_vec, idx_t k, int ef,
                      std::vector<row_t> &row_ids, std::vector<float> &distances) const;

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
    bool UsesRaBitQCoverageChecksum() const;
    uint64_t HashRaBitQVectorForCoverage(row_t row_id, const float *vec) const;
    bool HasRowIdCoverageCheck() const {
        return rowid_coverage_checked_.load(std::memory_order_acquire);
    }
    bool IsRowIdCoverageStale() const {
        return rowid_coverage_stale_.load(std::memory_order_acquire);
    }
    bool HasStoragePointerCorruption() const {
        return storage_pointer_corruption_.load(std::memory_order_acquire);
    }
    void MarkRowIdCoverageChecked(bool stale) {
        // Publish the result before the checked bit. Optimizer threads may read
        // this without graph_rwlock_, so the opposite order can expose a stale
        // checked=true / stale=false pair during concurrent mutation.
        rowid_coverage_stale_.store(stale, std::memory_order_relaxed);
        rowid_coverage_checked_.store(true, std::memory_order_release);
    }
    // HNSW entry-point level (top layer). -1 when the index has no nodes.
    int GetMaxLevel() const;
    bool UsesPQ() const { return pq_use_; }
    bool UsesRaBitQ() const;
    const char *GetQuantizerName() const;
    uint32_t GetPQM() const { return pq_use_ ? static_cast<uint32_t>(pq_quantizer_.M) : 0u; }
    idx_t GetPQCodesBytes() const { return pq_codes_.size(); }
    idx_t GetPQCodebookBytes() const {
        return pq_use_ ? pq_quantizer_.get_centroids_size() * sizeof(float) : 0u;
    }
    idx_t GetRaBitQCodesBytes() const;
    idx_t GetRaBitQFixedBytes() const;
    bool IsCompactMode() const { return compact_mode_; }

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

    // graph_memory_limit (bytes) — byte budget for MemStore's in-memory raw-vector
    // mirror (vectors[]). Captured from the vexdb_graph_memory_limit setting where a
    // ClientContext is available (Create / PhysicalVexCreateIndex::Finalize) and applied
    // to the store after each InitAllocators via ApplyMirrorBudget(). 0 = unlimited.
    idx_t graph_memory_limit_bytes_ = 0;
    // Rebuild keeps the live index resident until the staged build succeeds.
    // Disable the staged raw-vector mirror explicitly so it cannot temporarily
    // claim a second copy from the global mirror budget.
    bool suppress_mirror_for_build_ = false;
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
    void TrainAndEncodePQFromStore();
    void TrainAndEncodeRaBitQ();

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
    // whole query; Append/Insert/Delete/CommitDrop take it EXCLUSIVE. This makes
    // the per-node reader lock inside the HNSW walk redundant during search
    // (no writer can run concurrently), so search sets store.search_lock_free_
    // and skips it — eliminating hub-node reader-byte cacheline contention that
    // collapsed throughput at high read concurrency. mutable: the search
    // methods are const. The parallel BuildBulk path runs before any query and
    // is not gated by this lock (it relies on the per-node locks internally).
    mutable vex_duck::SimpleRWLock graph_rwlock_;

    // Product Quantization state. pq_m_ = 0 / pq_use_ = false means PQ disabled.
    // Once Train() runs (after BuildBulk completes), pq_quantizer_.trained = true
    // and pq_codes_ holds m bytes per row. Graph search addresses nodes by
    // internal id, so pq_node_code_positions_ maps nodes to code slots.
    uint32_t pq_m_ = 0;
    bool pq_use_ = false;
    ::vex::quantizer::ProductQuantizer pq_quantizer_;
    std::vector<uint8_t> pq_codes_;
    std::vector<row_t> pq_row_id_order_;
    std::vector<uint64_t> pq_vector_coverage_hashes_;
    std::vector<uint32_t> pq_node_code_positions_;
    // pq_row_id_order_ is append-only so persisted indexes remain compatible.
    // UPDATE/reinsert can therefore leave historical code slots for the same
    // physical row_id. Only the newest slot is live; older slots remain useful
    // to historical graph nodes as navigation anchors but must never be emitted.
    std::unordered_map<row_t, uint32_t> pq_latest_code_positions_;

    // RaBitQ shares the graph topology with the raw-vector index, but replaces
    // distance reads during search with compact codes aligned by internal node
    // id. Training and estimation live in common/rabitq; this class only adapts
    // DuckDB build, persistence and row-id filtering.
    bool rabitq_requested_ = false;
    bool rabitq_use_ = false;
    std::unique_ptr<::rabitq::RaBitQuantizer> rabitq_quantizer_;
    double rabitq_query_rescaling_factor_ = 0.0;
    std::vector<uint8_t> rabitq_codes_;

    // memory_mode='compact' releases only the index-side raw-vector mirror; the
    // table column remains unchanged. PQ searches its code array, while RaBitQ
    // keeps using the graph through a code-aware store. Post-build RaBitQ INSERTs
    // are encoded before graph insertion. Persisted across CommitDrop / Vacuum /
    // Reload.
    bool compact_mode_ = false;

    // Eager row_id → graph node map, rebuilt once after bulk load/recovery and
    // maintained by every incremental insert. UPDATE is DELETE+INSERT in
    // DuckDB; keeping this map avoids scanning every graph node to retire the
    // previous row version. Duplicate-node history only exists in indexes
    // written by older builds, so it is isolated in the sparse side map.
    std::unordered_map<row_t, uint32_t> rowid_node_map_;
    std::unordered_map<row_t, std::vector<uint32_t>> duplicate_rowid_nodes_;

    void RebuildPQNodeCodePositions();
    void RebuildPQLatestCodePositions();
    void RebuildRowIdNodeMap();
    void RecordRowIdNode(row_t row_id, uint32_t node_id);
    bool IsLatestPQCodePosition(idx_t position) const;
    void RetireDeletedRowVersion(row_t row_id);
    // Checkpoint/WAL snapshots invoke maintenance once physical history grows
    // beyond 2x the live row set. Explicit VACUUM remains the exact compactor.
    bool HasExcessHistory() const;

    std::atomic<bool> rowid_coverage_checked_{false};
    std::atomic<bool> rowid_coverage_stale_{false};
    // Hard process termination can leave index catalog metadata referring to an
    // allocator buffer whose block was not recovered. Never pass such a pointer
    // to FixedSizeAllocator::Get: release builds would dereference buffers.end().
    // The optimizer treats this index as stale so exact scan remains available;
    // PRAGMA rebuild can then replace it from the table.
    std::atomic<bool> storage_pointer_corruption_{false};

    // Free the raw vector tier and clear the in-memory copy. Idempotent.
    void ReleaseRawVectors();
    void SwapRebuildState(GraphIndex &staged) noexcept;
    // Refill the optional hot mirror after an atomic rebuild has released the
    // previous runtime and returned its share of the global mirror budget.
    void WarmRawVectorMirror();
};

} // namespace duckdb
