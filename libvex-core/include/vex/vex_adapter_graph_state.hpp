#ifndef VEX_ADAPTER_GRAPH_STATE_HPP
#define VEX_ADAPTER_GRAPH_STATE_HPP

#include "vex/vex_adapter_node_store_common.hpp"
#include "vex/vex_graph_algo.hpp"

namespace vex {

struct AdapterGraphState {
    bool has_entry_point = false;
    node_id_t entry_point = INVALID_NODE_ID;
    int max_level = 0;
    uint64_t node_count = 0;
};

inline AdapterGraphState CaptureGraphState(const HNSWGraph &graph) {
    AdapterGraphState out{};
    out.has_entry_point = graph.HasEntryPoint();
    out.entry_point = graph.EntryPoint();
    out.max_level = graph.MaxLevel();
    out.node_count = graph.NodeCount();
    return out;
}

inline void ApplyGraphState(HNSWGraph &graph, const AdapterGraphState &state) {
    graph.LoadState(state.has_entry_point, state.entry_point, state.max_level, state.node_count);
}

inline bool LoadGraphStateFromBinding(const AdapterLowLevelBinding &binding, AdapterGraphState &out) {
    bool has_entry_point = false;
    node_id_t entry_point = INVALID_NODE_ID;
    int max_level = 0;
    uint64_t node_count = 0;
    if (!binding.LoadGraphState(has_entry_point, entry_point, max_level, node_count)) {
        return false;
    }
    out.has_entry_point = has_entry_point;
    out.entry_point = entry_point;
    out.max_level = max_level;
    out.node_count = node_count;
    return true;
}

inline bool StoreGraphStateToBinding(AdapterLowLevelBinding &binding, const AdapterGraphState &state) {
    return binding.StoreGraphState(state.has_entry_point, state.entry_point, state.max_level, state.node_count);
}

inline bool StoreGraphStateToBinding(AdapterLowLevelBinding &binding, const HNSWGraph &graph) {
    return StoreGraphStateToBinding(binding, CaptureGraphState(graph));
}

} // namespace vex

#endif // VEX_ADAPTER_GRAPH_STATE_HPP
