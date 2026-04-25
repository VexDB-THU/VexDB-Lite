#ifndef VEX_GRAPH_MAINTENANCE_HPP
#define VEX_GRAPH_MAINTENANCE_HPP

#include "vex/vex_adapter_graph_state.hpp"
#include "vex/vex_node_store.hpp"

#include <vector>

namespace vex {

void DeleteNodesFromGraph(NodeStore &store, AdapterGraphState &state,
                          const std::vector<node_id_t> &deleted_nodes, int m);

} // namespace vex

#endif // VEX_GRAPH_MAINTENANCE_HPP
