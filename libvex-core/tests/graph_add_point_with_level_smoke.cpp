#include "vex/vex_adapter_duckdb_mock.hpp"
#include "vex/vex_adapter_duckdb_stub.hpp"
#include "vex/vex_graph_algo.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

int main() {
    auto binding = std::make_shared<vex::DuckDBLowLevelBindingMock>(4, 8, 0);

    vex::DuckDBNodeStoreConfig cfg{};
    cfg.dimension = 4;
    cfg.m = 8;
    cfg.metadata_size = 0;
    cfg.index_name = "graph_add_point_with_level";
    cfg.low_level_binding = binding;

    vex::DuckDBNodeStore store(cfg);
    vex::HNSWGraph graph(store, vex::Metric::L2, cfg.m, 32);

    const std::vector<float> data = {
        0.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
    };

    auto node0 = graph.AddPointWithLevel(300, &data[0], cfg.dimension, 3);
    auto node1 = graph.AddPointWithLevel(301, &data[4], cfg.dimension, 1);

    auto h0 = store.PinNode(node0);
    auto h1 = store.PinNode(node1);
    if (!h0 || !h1 || !h0->Header() || !h1->Header()) {
        std::cerr << "failed to pin inserted nodes" << std::endl;
        return 1;
    }
    if (h0->Header()->level != 3 || h1->Header()->level != 1) {
        std::cerr << "inserted node levels do not match requested levels" << std::endl;
        return 2;
    }
    if (!graph.HasEntryPoint() || graph.EntryPoint() != node0 || graph.MaxLevel() != 3) {
        std::cerr << "graph state does not preserve caller-controlled levels" << std::endl;
        return 3;
    }

    float query[4] = {1.1f, 1.1f, 1.1f, 1.1f};
    std::vector<vex::row_id_t> rows;
    std::vector<float> dists;
    graph.Search(query, 1, 16, rows, dists);
    if (rows.size() != 1 || rows[0] != 301) {
        std::cerr << "search result mismatch after AddPointWithLevel" << std::endl;
        return 4;
    }

    return 0;
}
