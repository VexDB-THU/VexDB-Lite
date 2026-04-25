#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/execution/index/fixed_size_allocator.hpp"
#include "duckdb/execution/index/index_pointer.hpp"
#include "vex/vex_adapter_duckdb_stub.hpp"
#include "vex/vex_adapter_graph_state.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace duckdb {

struct IndexStorageInfo;
using DuckDBCoreGraphState = ::vex::AdapterGraphState;

struct DuckDBStorageGraphState {
    bool has_entry_point = false;
    uint64_t entry_point_raw = 0;
    int max_level = 0;
    uint64_t node_count = 0;
};

struct DuckDBCoreLiveStorage {
    FixedSizeAllocator *node_alloc = nullptr;
    FixedSizeAllocator *vector_alloc = nullptr;
    FixedSizeAllocator *upper_alloc = nullptr;
    FixedSizeAllocator *meta_alloc = nullptr;
    unordered_map<row_t, IndexPointer> *row_id_map = nullptr;
};

struct DuckDBCoreBindingConfig {
    void *block_manager = nullptr;
    void *buffer_manager = nullptr;
    std::string index_name;
    uint32_t dimension = 0;
    int m = 16;
    uint32_t metadata_size = 0;
    IndexStorageInfo *storage_info = nullptr;
    DuckDBCoreLiveStorage live_storage;

    // Optional callbacks for backend meta-page graph-state persistence.
    // If unset, skeleton uses in-memory fallback state.
    std::function<bool(DuckDBCoreGraphState &out)> load_graph_state_cb;
    std::function<bool(const DuckDBCoreGraphState &in)> store_graph_state_cb;
};

// Skeleton for direct DuckDB page/buffer binding.
// This is intentionally a non-functional placeholder for incremental integration.
class DuckDBCoreLowLevelBindingSkeleton final : public ::vex::DuckDBLowLevelBinding {
public:
    explicit DuckDBCoreLowLevelBindingSkeleton(const DuckDBCoreBindingConfig &cfg);

    ::vex::node_id_t AllocateNode(::vex::row_id_t row_id, const float *vec, uint32_t dim, uint8_t level) override;
    void FreeNode(::vex::node_id_t node_id) override;

    bool PinNode(::vex::node_id_t node_id, bool for_update, ::vex::DuckDBNodeLayoutView &out) override;
    void UnpinNode(::vex::DuckDBNodeLayoutView &view) override;

    uint32_t GetDimension() const override;
    int GetM() const override;
    uint64_t GetNodeCount() const override;
    void ForEachNode(std::function<void(::vex::node_id_t)> cb) const override;

    bool LoadGraphState(bool &has_entry_point, ::vex::node_id_t &entry_point, int &max_level, uint64_t &node_count) const override;
    bool StoreGraphState(bool has_entry_point, ::vex::node_id_t entry_point, int max_level, uint64_t node_count) override;
    bool ResolveNodeIdByRowId(::vex::row_id_t row_id, ::vex::node_id_t &node_id) const override;
    bool ResolveStorageNodeKey(::vex::node_id_t node_id, uint64_t &key) const override;

private:
    DuckDBCoreBindingConfig cfg_;
    DuckDBCoreGraphState state_;
    uint64_t active_node_count_ = 0;
    std::vector<IndexPointer> node_id_to_ptr_;
    unordered_map<idx_t, ::vex::node_id_t> ptr_to_node_id_;
};

std::shared_ptr<::vex::DuckDBNodeStore> CreateDuckDBCoreNodeStoreSkeleton(const DuckDBCoreBindingConfig &cfg);
bool LoadDuckDBGraphStateFromStorage(const IndexStorageInfo &storage, DuckDBCoreGraphState &out);
bool StoreDuckDBGraphStateToStorage(IndexStorageInfo &storage, const DuckDBCoreGraphState &in);
bool LoadDuckDBRawGraphStateFromStorage(const IndexStorageInfo &storage, DuckDBStorageGraphState &out);
bool StoreDuckDBRawGraphStateToStorage(IndexStorageInfo &storage, const DuckDBStorageGraphState &in);
void BindDuckDBGraphStateStorage(DuckDBCoreBindingConfig &cfg, IndexStorageInfo &storage);
void BindDuckDBLiveStorage(DuckDBCoreBindingConfig &cfg, FixedSizeAllocator &node_alloc,
                           FixedSizeAllocator &vector_alloc, FixedSizeAllocator &upper_alloc,
                           FixedSizeAllocator *meta_alloc,
                           unordered_map<row_t, IndexPointer> &row_id_map);

} // namespace duckdb
