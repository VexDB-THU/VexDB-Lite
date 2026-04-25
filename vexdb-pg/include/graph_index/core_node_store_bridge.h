/**
 * NodeStore direct binding bridge skeleton for PG adapter.
 */
#ifndef GRAPH_INDEX_CORE_NODE_STORE_BRIDGE_H
#define GRAPH_INDEX_CORE_NODE_STORE_BRIDGE_H

#include "pg_compat.h"
#include "graph_index/graph_index_param.h"
#include "graph_index/graph_index_struct.h"
#include "vex/vex_adapter_pg_stub.hpp"
#include "vex/vex_adapter_graph_state.hpp"

#include <cstdint>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using PgCoreGraphState = vex::AdapterGraphState;

struct PgCoreBindingConfig {
    Relation index_rel = NULL;
    ForkNumber fork_num = MAIN_FORKNUM;
    uint32_t dimension = 0;
    int m = 16;
    uint32_t metadata_size = 0;
    std::string index_name;
    BlockNumber metablkno = GRAPH_INDEX_METAPAGE_BLKNO;

    // Optional callbacks for backend meta-page graph-state persistence.
    // If unset, skeleton uses in-memory fallback state.
    std::function<bool(PgCoreGraphState &out)> load_graph_state_cb;
    std::function<bool(const PgCoreGraphState &in)> store_graph_state_cb;

    // Optional live-binding hooks for PG-specific payload/header plumbing.
    // These keep TID / metadata / row mapping orchestration in the adapter layer
    // instead of forcing libvex-core to know PG storage details.
    std::function<bool(vex::node_id_t node_id, vex::NodeHeader &header)> load_node_header_cb;
    std::function<bool(vex::node_id_t node_id, const vex::NodeHeader &header)> store_node_header_cb;
    std::function<bool(vex::node_id_t node_id, uint8_t *metadata, uint32_t metadata_size)> load_node_metadata_cb;
    std::function<bool(vex::node_id_t node_id, const uint8_t *metadata, uint32_t metadata_size)> store_node_metadata_cb;
    std::function<bool(vex::node_id_t node_id, vex::row_id_t row_id, uint8_t level)> on_allocate_node_cb;
    std::function<bool(vex::node_id_t node_id)> on_free_node_cb;
    std::function<bool(vex::row_id_t row_id, vex::node_id_t &node_id)> resolve_node_id_by_row_id_cb;
    std::function<bool(vex::node_id_t node_id, uint64_t &storage_key)> resolve_storage_node_key_cb;
    bool trust_live_header_cache_for_read = false;
};

// Skeleton for PG shared-buffer/page direct binding.
// This class is a bridge placeholder and does not yet wire to real page layout.
class PgCoreLowLevelBindingSkeleton final : public vex::PGLowLevelBinding {
public:
    explicit PgCoreLowLevelBindingSkeleton(const PgCoreBindingConfig &cfg);

    vex::node_id_t AllocateNode(vex::row_id_t row_id, const float *vec, uint32_t dim, uint8_t level) override;
    void FreeNode(vex::node_id_t node_id) override;

    bool PinNode(vex::node_id_t node_id, bool for_update, vex::PGNodeLayoutView &out) override;
    void UnpinNode(vex::PGNodeLayoutView &view) override;

    uint32_t GetDimension() const override;
    int GetM() const override;
    uint64_t GetNodeCount() const override;
    void ForEachNode(std::function<void(vex::node_id_t)> cb) const override;

    bool LoadGraphState(bool &has_entry_point, vex::node_id_t &entry_point, int &max_level, uint64_t &node_count) const override;
    bool StoreGraphState(bool has_entry_point, vex::node_id_t entry_point, int max_level, uint64_t node_count) override;
    bool ResolveNodeIdByRowId(vex::row_id_t row_id, vex::node_id_t &node_id) const override;
    bool ResolveStorageNodeKey(vex::node_id_t node_id, uint64_t &storage_key) const override;

private:
    struct LocalNodeStorage {
        bool allocated = false;
        vex::NodeHeader header{};
        std::vector<float> vector;
        std::vector<vex::node_id_t> level0_neighbors;
        std::vector<vex::node_id_t> upper_neighbors;
        std::array<uint16_t, vex::HNSW_MAX_UPPER_LEVELS> upper_counts{};
        std::vector<uint8_t> metadata;
    };

    bool UseLocalFallbackStorage() const;
    LocalNodeStorage *TryGetLocalNode(vex::node_id_t node_id);
    const LocalNodeStorage *TryGetLocalNode(vex::node_id_t node_id) const;

    PgCoreBindingConfig cfg_;
    PgCoreGraphState state_;
    std::vector<LocalNodeStorage> local_nodes_;
    std::vector<vex::node_id_t> local_free_list_;
    std::unordered_map<vex::row_id_t, vex::node_id_t> row_to_node_id_;
    uint64_t local_active_count_ = 0;
};

std::shared_ptr<vex::PGNodeStore> CreatePgCoreNodeStoreSkeleton(const PgCoreBindingConfig &cfg);
bool LoadPgGraphStateFromMetaPage(const PgCoreBindingConfig &cfg, PgCoreGraphState &out);
bool StorePgGraphStateToMetaPage(const PgCoreBindingConfig &cfg, const PgCoreGraphState &in);
bool RoundtripPgGraphStateOnMetaPage(const PgCoreBindingConfig &cfg, const PgCoreGraphState &in, PgCoreGraphState &out);

#endif // GRAPH_INDEX_CORE_NODE_STORE_BRIDGE_H
