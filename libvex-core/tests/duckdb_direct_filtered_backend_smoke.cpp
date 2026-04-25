#include "vex/vex_adapter_duckdb_mock.hpp"
#include "vex/vex_adapter_duckdb_stub.hpp"
#include "vex/vex_filter.hpp"
#include "vex/vex_graph_algo.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

int main() {
    auto binding = std::make_shared<vex::DuckDBLowLevelBindingMock>(4, 8, sizeof(int32_t));

    vex::DuckDBNodeStoreConfig cfg{};
    cfg.dimension = 4;
    cfg.m = 8;
    cfg.metadata_size = sizeof(int32_t);
    cfg.index_name = "direct_filtered_mock_smoke";
    cfg.low_level_binding = binding;

    vex::DuckDBNodeStore store(cfg);
    vex::HNSWGraph graph(store, vex::Metric::L2, cfg.m, 32);

    std::vector<float> data = {
        0.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
        2.0f, 2.0f, 2.0f, 2.0f,
    };

    auto node0 = graph.AddPoint(300, &data[0], cfg.dimension);
    auto node1 = graph.AddPoint(301, &data[4], cfg.dimension);
    auto node2 = graph.AddPoint(302, &data[8], cfg.dimension);

    auto set_meta = [&](vex::node_id_t node_id, int32_t value) {
        vex::AdapterNodeLayoutView view{};
        if (!binding->PinNode(node_id, true, view) || !view.metadata) {
            std::cerr << "failed to pin metadata for node " << node_id << std::endl;
            std::exit(10);
        }
        std::memcpy(view.metadata, &value, sizeof(value));
        binding->UnpinNode(view);
    };

    set_meta(node0, 10);
    set_meta(node1, 20);
    set_meta(node2, 10);

    int32_t wanted = 10;
    vex::EqualityFilter filter(0, sizeof(wanted), &wanted, 0.5);

    float query[4] = {1.9f, 1.9f, 1.9f, 1.9f};
    std::vector<vex::row_id_t> rows;
    std::vector<float> dists;
    graph.FilteredSearch(query, 2, 16, rows, dists, filter);

    if (rows.size() != 2) {
        std::cerr << "unexpected filtered result count: " << rows.size() << std::endl;
        return 1;
    }
    if (rows[0] != 302 || rows[1] != 300) {
        std::cerr << "unexpected filtered row order: " << rows[0] << ", " << rows[1] << std::endl;
        return 2;
    }

    return 0;
}
