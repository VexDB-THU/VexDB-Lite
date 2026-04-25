#include "vex/vex_adapter_pg_mock.hpp"
#include "vex/vex_adapter_pg_stub.hpp"
#include "vex/vex_graph_algo.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

int main() {
    auto binding = std::make_shared<vex::PGLowLevelBindingMock>(4, 8, 0);

    vex::PGNodeStoreConfig cfg{};
    cfg.dimension = 4;
    cfg.m = 8;
    cfg.metadata_size = 0;
    cfg.index_name = "pg_direct_mutation_smoke";
    cfg.low_level_binding = binding;

    vex::PGNodeStore store(cfg);
    vex::HNSWGraph graph(store, vex::Metric::L2, cfg.m, 32);

    std::vector<float> data = {
        0.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
        2.0f, 2.0f, 2.0f, 2.0f,
    };

    graph.AddPoint(400, &data[0], cfg.dimension);
    graph.AddPoint(401, &data[4], cfg.dimension);
    graph.AddPoint(402, &data[8], cfg.dimension);

    vex::node_id_t node_id = vex::INVALID_NODE_ID;
    if (!binding->ResolveNodeIdByRowId(401, node_id)) {
        std::cerr << "resolve row_id -> node_id failed" << std::endl;
        return 1;
    }

    uint64_t storage_key = 0;
    if (!binding->ResolveStorageNodeKey(node_id, storage_key) ||
        storage_key != static_cast<uint64_t>(node_id)) {
        std::cerr << "resolve storage key failed" << std::endl;
        return 2;
    }

    auto handle = store.PinNodeForUpdate(node_id);
    if (!handle || !handle->MutableHeader()) {
        std::cerr << "mutable pin failed" << std::endl;
        return 3;
    }
    handle->MutableHeader()->deleted = 1;

    float query[4] = {1.1f, 1.1f, 1.1f, 1.1f};
    std::vector<vex::row_id_t> rows;
    std::vector<float> dists;
    graph.Search(query, 2, 16, rows, dists);

    if (rows.empty()) {
        std::cerr << "search after mutation empty" << std::endl;
        return 4;
    }

    if (rows[0] == 401) {
        std::cerr << "deleted node still visible in search" << std::endl;
        return 5;
    }

    return 0;
}
