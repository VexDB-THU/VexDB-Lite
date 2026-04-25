#include "vex/vex_adapter_duckdb_stub.hpp"
#include "vex/vex_graph_algo.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    vex::DuckDBNodeStoreConfig cfg{};
    cfg.dimension = 4;
    cfg.m = 8;
    cfg.metadata_size = 0;
    cfg.index_name = "stub_smoke";

    vex::DuckDBNodeStoreStub store(cfg);
    vex::HNSWGraph graph(store, vex::Metric::L2, cfg.m, 32);

    std::vector<float> data = {
        0.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
        2.0f, 2.0f, 2.0f, 2.0f,
    };

    graph.AddPoint(100, &data[0], cfg.dimension);
    graph.AddPoint(101, &data[4], cfg.dimension);
    graph.AddPoint(102, &data[8], cfg.dimension);

    float query[4] = {1.1f, 1.1f, 1.1f, 1.1f};
    std::vector<vex::row_id_t> rows;
    std::vector<float> dists;
    graph.Search(query, 2, 16, rows, dists);

    if (rows.empty()) {
        std::cerr << "duckdb stub search empty" << std::endl;
        return 1;
    }

    if (rows[0] != 101) {
        std::cerr << "unexpected top-1 row_id: " << rows[0] << std::endl;
        return 2;
    }

    return 0;
}
