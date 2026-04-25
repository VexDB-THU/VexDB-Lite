/**
 * Shared helpers for PG libvex-core bridge graph-state synchronization.
 */
#ifndef GRAPH_INDEX_CORE_NODE_STORE_BRIDGE_UTILS_H
#define GRAPH_INDEX_CORE_NODE_STORE_BRIDGE_UTILS_H

#include "pg_compat.h"
#include "graph_index/core_node_store_bridge.h"
#include "graph_index/graph_index_struct.h"

inline void PgBridgeFillStateFromMeta(GraphIndexMetaPage metap, PgCoreGraphState &state)
{
    state.has_entry_point = (metap->entrypoint_id != INVALID_VECTOR_ID);
    state.entry_point = state.has_entry_point
        ? static_cast<vex::node_id_t>(metap->entrypoint_id)
        : vex::INVALID_NODE_ID;
    state.max_level = static_cast<int>(metap->entry_level);
    state.node_count = static_cast<uint64_t>(metap->num_vectors);
}

inline void PgBridgeApplyStateToMeta(GraphIndexMetaPage metap, const PgCoreGraphState &state)
{
    metap->num_vectors = static_cast<size_t>(state.node_count);
    metap->entry_level = static_cast<int8>(state.max_level);
    if (state.has_entry_point) {
        metap->entrypoint_id = static_cast<size_t>(state.entry_point);
        metap->entry_cur_layer_idx = static_cast<size_t>(state.entry_point);
    } else {
        metap->entrypoint_id = INVALID_VECTOR_ID;
        metap->entry_cur_layer_idx = INVALID_VECTOR_ID;
    }
}

inline PgCoreBindingConfig PgBridgeMakeBindingConfig(
    Relation index,
    ForkNumber fork_num,
    BlockNumber metablkno,
    GraphIndexMetaPage metap)
{
    PgCoreBindingConfig cfg{};
    cfg.index_rel = index;
    cfg.fork_num = fork_num;
    cfg.metablkno = metablkno;
    cfg.dimension = metap->dimension;
    cfg.m = metap->m;
    return cfg;
}

#endif // GRAPH_INDEX_CORE_NODE_STORE_BRIDGE_UTILS_H
