#ifndef GRAPH_INDEX_CORE_MEMORY_BUILD_RUNTIME_HPP
#define GRAPH_INDEX_CORE_MEMORY_BUILD_RUNTIME_HPP

#include "pg_compat.h"

#include <chrono>
#include <memory>
#include <vector>

#include "vex/vex_graph_algo.hpp"
#include "vex/vex_node_store_memory.hpp"

namespace pgvexdb {

struct CoreMemoryBuildRuntime {
    std::unique_ptr<vex::MemoryNodeStore> node_store;
    std::unique_ptr<vex::HNSWGraph> core_graph;
    std::vector<ItemPointerData> heap_tids_by_node_id;
    uint64_t add_point_count = 0;
    double add_point_total_ms = 0.0;
    double update_row_id_total_ms = 0.0;

    bool Init(uint_fast16_t dimension, uint_fast16_t m, uint_fast16_t ef_construction, vex::Metric metric)
    {
        node_store = std::make_unique<vex::MemoryNodeStore>(dimension, static_cast<int>(m), 0);
        core_graph = std::make_unique<vex::HNSWGraph>(*node_store, metric,
                                                      static_cast<int>(m), static_cast<int>(ef_construction));
        heap_tids_by_node_id.clear();
        add_point_count = 0;
        add_point_total_ms = 0.0;
        update_row_id_total_ms = 0.0;
        return true;
    }

    bool AddPoint(const char *query, const ItemPointerData &heap_tid, uint_fast16_t dimension)
    {
        auto add_point_start = std::chrono::high_resolution_clock::now();
        auto node_id = core_graph->AddPoint(static_cast<vex::row_id_t>(0),
                                            reinterpret_cast<const float *>(query), dimension);
        add_point_total_ms += std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - add_point_start).count();
        ++add_point_count;
        if (node_id == vex::INVALID_NODE_ID) {
            return false;
        }
        auto update_row_id_start = std::chrono::high_resolution_clock::now();
        auto handle = node_store->PinNodeForUpdate(node_id);
        if (!handle || !handle->MutableHeader()) {
            return false;
        }
        handle->MutableHeader()->row_id = static_cast<vex::row_id_t>(node_id);
        update_row_id_total_ms += std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - update_row_id_start).count();
        if (heap_tids_by_node_id.size() <= node_id) {
            heap_tids_by_node_id.resize(static_cast<size_t>(node_id) + 1);
        }
        heap_tids_by_node_id[static_cast<size_t>(node_id)] = heap_tid;
        return true;
    }

    void Reset()
    {
        core_graph.reset();
        node_store.reset();
        heap_tids_by_node_id.clear();
        add_point_count = 0;
        add_point_total_ms = 0.0;
        update_row_id_total_ms = 0.0;
    }
};

} // namespace pgvexdb

#endif
