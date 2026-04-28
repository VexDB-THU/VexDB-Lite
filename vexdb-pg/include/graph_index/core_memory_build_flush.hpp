#ifndef GRAPH_INDEX_CORE_MEMORY_BUILD_FLUSH_HPP
#define GRAPH_INDEX_CORE_MEMORY_BUILD_FLUSH_HPP

#include "graph_index/graph_index.h"
#include "vex/vex_graph_algo.hpp"
#include "vex/vex_node_store_memory.hpp"

#include <vector>

namespace pgvexdb {

struct CoreMemoryBuildFlushInput {
    vex::MemoryNodeStore &node_store;
    const vex::HNSWGraph &core_graph;
    const std::vector<ItemPointerData> &heap_tids_by_node_id;
};

void FlushCoreMemoryBuildToDisk(Relation index, Buffer metabuf, GraphIndexMetaPage metap,
                                uint_fast16_t dimension, uint_fast16_t m, size_t vector_size,
                                const CoreMemoryBuildFlushInput &input);

} // namespace pgvexdb

#endif
