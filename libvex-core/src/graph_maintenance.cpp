#include "vex/vex_graph_maintenance.hpp"

#include "vex/vex_config.hpp"

#include <algorithm>
#include <unordered_set>

namespace vex {

namespace {

void RefreshEntryPoint(NodeStore &store, AdapterGraphState &state) {
    state.has_entry_point = false;
    state.entry_point = INVALID_NODE_ID;
    state.max_level = 0;

    store.ForEachNode([&](node_id_t node_id) {
        auto handle = store.PinNode(node_id);
        if (!handle || !handle->Header() || handle->Header()->deleted) {
            return;
        }
        const int level = static_cast<int>(handle->Header()->level);
        if (!state.has_entry_point || level > state.max_level) {
            state.entry_point = node_id;
            state.has_entry_point = true;
            state.max_level = level;
        }
    });
}

} // namespace

void DeleteNodesFromGraph(NodeStore &store, AdapterGraphState &state,
                          const std::vector<node_id_t> &deleted_nodes, int m) {
    if (deleted_nodes.empty()) {
        return;
    }

    std::unordered_set<node_id_t> deleted_node_set;
    deleted_node_set.reserve(deleted_nodes.size());
    for (auto node_id : deleted_nodes) {
        deleted_node_set.insert(node_id);
    }

    if (deleted_nodes.size() <= state.node_count) {
        state.node_count -= deleted_nodes.size();
    } else {
        state.node_count = 0;
    }

    if (state.has_entry_point && deleted_node_set.find(state.entry_point) != deleted_node_set.end()) {
        RefreshEntryPoint(store, state);
    }

    store.ForEachNode([&](node_id_t node_id) {
        auto handle = store.PinNode(node_id);
        if (!handle || !handle->Header() || handle->Header()->deleted) {
            return;
        }

        auto update = store.PinNodeForUpdate(node_id);
        if (!update || !update->MutableHeader()) {
            return;
        }

        for (int level = 0; level <= static_cast<int>(handle->Header()->level); ++level) {
            node_id_t *neighbors = nullptr;
            uint16_t current_count = 0;
            if (level == 0) {
                neighbors = update->MutableLevel0Neighbors();
                current_count = update->Level0Count();
            } else {
                neighbors = update->MutableUpperNeighbors(level - 1);
                current_count = update->UpperCount(level - 1);
            }
            if (!neighbors) {
                continue;
            }

            uint16_t write_idx = 0;
            for (uint16_t read_idx = 0; read_idx < current_count; ++read_idx) {
                const auto neighbor_id = neighbors[read_idx];
                if (neighbor_id == INVALID_NODE_ID || deleted_node_set.find(neighbor_id) != deleted_node_set.end()) {
                    continue;
                }

                auto neighbor_handle = store.PinNode(neighbor_id);
                if (!neighbor_handle || !neighbor_handle->Header() || neighbor_handle->Header()->deleted) {
                    continue;
                }
                neighbors[write_idx++] = neighbor_id;
            }

            const auto capacity = static_cast<uint16_t>(GraphIndexConfig::GetLayerM(m, level));
            for (uint16_t idx = write_idx; idx < capacity; ++idx) {
                neighbors[idx] = INVALID_NODE_ID;
            }

            if (level == 0) {
                update->SetLevel0Count(write_idx);
            } else {
                update->SetUpperCount(level - 1, write_idx);
            }
        }
    });

    for (auto node_id : deleted_nodes) {
        store.FreeNode(node_id);
    }
}

} // namespace vex
