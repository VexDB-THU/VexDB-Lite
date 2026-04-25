#include "graph_index/core_node_store_bridge.h"

#include "graph_index/core_node_store_bridge_live.hpp"
#include "graph_index/core_node_store_bridge_utils.h"
#include "graph_index/graph_index_xlog.h"

#include <algorithm>
#include <cstring>

static bool TryBootstrapPgGraphState(const PgCoreBindingConfig &cfg, PgCoreGraphState &state) {
    if (!cfg.load_graph_state_cb) {
        return false;
    }

    PgCoreGraphState loaded{};
    if (!cfg.load_graph_state_cb(loaded)) {
        return false;
    }
    state = loaded;
    return true;
}

bool LoadPgGraphStateFromMetaPage(const PgCoreBindingConfig &cfg, PgCoreGraphState &out) {
    if (!cfg.index_rel) {
        return false;
    }

    Buffer metabuf = ReadBufferExtended(cfg.index_rel, cfg.fork_num, cfg.metablkno, RBM_NORMAL, NULL);
    LockBuffer(metabuf, BUFFER_LOCK_SHARE);
    Page metapage = BufferGetPage(metabuf);
    GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(metapage);

    if (metap->magic_number != GRAPH_INDEX_MAGIC_NUMBER) {
        UnlockReleaseBuffer(metabuf);
        return false;
    }

    PgBridgeFillStateFromMeta(metap, out);

    UnlockReleaseBuffer(metabuf);
    return true;
}

bool StorePgGraphStateToMetaPage(const PgCoreBindingConfig &cfg, const PgCoreGraphState &in) {
    if (!cfg.index_rel) {
        return false;
    }

    Buffer metabuf = ReadBufferExtended(cfg.index_rel, cfg.fork_num, cfg.metablkno, RBM_NORMAL, NULL);
    LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
    Page metapage = BufferGetPage(metabuf);
    GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(metapage);

    if (metap->magic_number != GRAPH_INDEX_MAGIC_NUMBER) {
        LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
        ReleaseBuffer(metabuf);
        return false;
    }

    PgBridgeApplyStateToMeta(metap, in);

    MarkBufferDirty(metabuf);

    GraphIndexXlog xlog;
    xlog.init(cfg.index_rel, metabuf, metapage);
    xlog.update_num_vector(metap->num_vectors);
    GraphIndexEntryInfo entry{};
    entry.id = metap->entrypoint_id;
    entry.cur_layer_idx = metap->entry_cur_layer_idx;
    entry.level = metap->entry_level;
    xlog.update_entry(entry);

    LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
    ReleaseBuffer(metabuf);
    return true;
}

bool RoundtripPgGraphStateOnMetaPage(const PgCoreBindingConfig &cfg, const PgCoreGraphState &in, PgCoreGraphState &out) {
    if (!StorePgGraphStateToMetaPage(cfg, in)) {
        return false;
    }
    return LoadPgGraphStateFromMetaPage(cfg, out);
}

PgCoreLowLevelBindingSkeleton::PgCoreLowLevelBindingSkeleton(const PgCoreBindingConfig &cfg)
    : cfg_(cfg) {
    if (cfg_.index_rel && !cfg_.load_graph_state_cb) {
        cfg_.load_graph_state_cb = [cfg](PgCoreGraphState &out) {
            return LoadPgGraphStateFromMetaPage(cfg, out);
        };
    }
    if (cfg_.index_rel && !cfg_.store_graph_state_cb) {
        cfg_.store_graph_state_cb = [cfg](const PgCoreGraphState &in) {
            return StorePgGraphStateToMetaPage(cfg, in);
        };
    }
    (void)TryBootstrapPgGraphState(cfg_, state_);
}

bool PgCoreLowLevelBindingSkeleton::UseLocalFallbackStorage() const {
    return cfg_.index_rel == NULL;
}

PgCoreLowLevelBindingSkeleton::LocalNodeStorage *PgCoreLowLevelBindingSkeleton::TryGetLocalNode(vex::node_id_t node_id) {
    if (node_id >= local_nodes_.size()) {
        return nullptr;
    }
    auto &node = local_nodes_[node_id];
    return node.allocated ? &node : nullptr;
}

const PgCoreLowLevelBindingSkeleton::LocalNodeStorage *PgCoreLowLevelBindingSkeleton::TryGetLocalNode(
    vex::node_id_t node_id) const {
    if (node_id >= local_nodes_.size()) {
        return nullptr;
    }
    auto const &node = local_nodes_[node_id];
    return node.allocated ? &node : nullptr;
}

vex::node_id_t PgCoreLowLevelBindingSkeleton::AllocateNode(vex::row_id_t row_id, const float *vec, uint32_t dim,
                                                           uint8_t level) {
    if (!UseLocalFallbackStorage()) {
        ereport(ERROR, (errmsg("PG core direct binding skeleton: AllocateNode is not wired to page layout yet")));
        return vex::INVALID_NODE_ID;
    }

    if (!vec || dim != cfg_.dimension) {
        ereport(ERROR, (errmsg("PG core direct binding skeleton: invalid vector payload")));
        return vex::INVALID_NODE_ID;
    }

    vex::node_id_t node_id = vex::INVALID_NODE_ID;
    if (!local_free_list_.empty()) {
        node_id = local_free_list_.back();
        local_free_list_.pop_back();
    } else {
        node_id = static_cast<vex::node_id_t>(local_nodes_.size());
        local_nodes_.emplace_back();
    }

    auto &node = local_nodes_[node_id];
    node.allocated = true;
    node.header = {};
    node.header.row_id = row_id;
    node.header.level = level;
    node.header.deleted = 0;
    node.header.level0_count = 0;
    node.header.extra_row_count = 0;
    node.header.reserved = 0;
    node.header.upper_offset = 0;
    node.header.metadata_offset = 0;
    node.vector.assign(vec, vec + dim);
    node.level0_neighbors.assign(static_cast<size_t>(cfg_.m) * 2, vex::INVALID_NODE_ID);
    node.upper_neighbors.assign(static_cast<size_t>(vex::HNSW_MAX_UPPER_LEVELS) * static_cast<size_t>(cfg_.m),
                                vex::INVALID_NODE_ID);
    node.upper_counts.fill(0);
    node.metadata.assign(cfg_.metadata_size, 0);

    row_to_node_id_[row_id] = node_id;
    local_active_count_++;
    return node_id;
}

void PgCoreLowLevelBindingSkeleton::FreeNode(vex::node_id_t node_id) {
    if (!UseLocalFallbackStorage()) {
        ereport(ERROR, (errmsg("PG core direct binding skeleton: FreeNode is not wired to page layout yet")));
        return;
    }

    auto *node = TryGetLocalNode(node_id);
    if (!node) {
        return;
    }

    row_to_node_id_.erase(node->header.row_id);
    node->allocated = false;
    node->header = {};
    node->vector.clear();
    node->level0_neighbors.clear();
    node->upper_neighbors.clear();
    node->upper_counts.fill(0);
    node->metadata.clear();
    local_free_list_.push_back(node_id);
    if (local_active_count_ > 0) {
        local_active_count_--;
    }
}

bool PgCoreLowLevelBindingSkeleton::PinNode(vex::node_id_t node_id, bool, vex::PGNodeLayoutView &out) {
    out = {};
    if (!UseLocalFallbackStorage()) {
        return false;
    }

    auto *node = TryGetLocalNode(node_id);
    if (!node) {
        return false;
    }

    out.header = &node->header;
    out.vector = node->vector.empty() ? nullptr : node->vector.data();
    out.level0_neighbors = node->level0_neighbors.empty() ? nullptr : node->level0_neighbors.data();
    out.level0_count = &node->header.level0_count;
    out.upper_neighbors_base = node->upper_neighbors.empty() ? nullptr : node->upper_neighbors.data();
    out.upper_counts = node->upper_counts.data();
    out.metadata = node->metadata.empty() ? nullptr : node->metadata.data();
    out.opaque = nullptr;
    return true;
}

void PgCoreLowLevelBindingSkeleton::UnpinNode(vex::PGNodeLayoutView &view) {
    view = {};
}

uint32_t PgCoreLowLevelBindingSkeleton::GetDimension() const {
    return cfg_.dimension;
}

int PgCoreLowLevelBindingSkeleton::GetM() const {
    return cfg_.m;
}

uint64_t PgCoreLowLevelBindingSkeleton::GetNodeCount() const {
    if (UseLocalFallbackStorage()) {
        return local_active_count_;
    }
    return state_.node_count;
}

void PgCoreLowLevelBindingSkeleton::ForEachNode(std::function<void(vex::node_id_t)> cb) const {
    if (!UseLocalFallbackStorage()) {
        return;
    }
    for (vex::node_id_t node_id = 0; node_id < local_nodes_.size(); ++node_id) {
        if (local_nodes_[node_id].allocated) {
            cb(node_id);
        }
    }
}

bool PgCoreLowLevelBindingSkeleton::LoadGraphState(bool &has_entry_point, vex::node_id_t &entry_point, int &max_level,
                                                   uint64_t &node_count) const {
    if (cfg_.load_graph_state_cb) {
        PgCoreGraphState loaded{};
        if (!cfg_.load_graph_state_cb(loaded)) {
            return false;
        }
        has_entry_point = loaded.has_entry_point;
        entry_point = loaded.entry_point;
        max_level = loaded.max_level;
        node_count = loaded.node_count;
        return true;
    }

    has_entry_point = state_.has_entry_point;
    entry_point = state_.entry_point;
    max_level = state_.max_level;
    node_count = state_.node_count;
    return true;
}

bool PgCoreLowLevelBindingSkeleton::StoreGraphState(bool has_entry_point, vex::node_id_t entry_point, int max_level,
                                                    uint64_t node_count) {
    if (cfg_.store_graph_state_cb) {
        PgCoreGraphState out{};
        out.has_entry_point = has_entry_point;
        out.entry_point = entry_point;
        out.max_level = max_level;
        out.node_count = node_count;
        if (!cfg_.store_graph_state_cb(out)) {
            return false;
        }
    }

    state_.has_entry_point = has_entry_point;
    state_.entry_point = entry_point;
    state_.max_level = max_level;
    state_.node_count = node_count;
    return true;
}

bool PgCoreLowLevelBindingSkeleton::ResolveNodeIdByRowId(vex::row_id_t row_id, vex::node_id_t &node_id) const {
    if (cfg_.resolve_node_id_by_row_id_cb && cfg_.resolve_node_id_by_row_id_cb(row_id, node_id)) {
        return true;
    }
    auto it = row_to_node_id_.find(row_id);
    if (it == row_to_node_id_.end()) {
        return false;
    }
    node_id = it->second;
    return true;
}

bool PgCoreLowLevelBindingSkeleton::ResolveStorageNodeKey(vex::node_id_t node_id, uint64_t &storage_key) const {
    if (cfg_.resolve_storage_node_key_cb && cfg_.resolve_storage_node_key_cb(node_id, storage_key)) {
        return true;
    }
    if (UseLocalFallbackStorage()) {
        if (!TryGetLocalNode(node_id)) {
            return false;
        }
    }
    storage_key = static_cast<uint64_t>(node_id);
    return true;
}

std::shared_ptr<vex::PGNodeStore> CreatePgCoreNodeStoreSkeleton(const PgCoreBindingConfig &cfg) {
    auto binding = std::make_shared<PgCoreLowLevelBindingSkeleton>(cfg);

    vex::PGNodeStoreConfig store_cfg{};
    store_cfg.dimension = cfg.dimension;
    store_cfg.m = cfg.m;
    store_cfg.metadata_size = cfg.metadata_size;
    store_cfg.pg_relation = cfg.index_rel;
    store_cfg.index_name = cfg.index_name;
    store_cfg.low_level_binding = binding;

    return std::make_shared<vex::PGNodeStore>(store_cfg);
}
