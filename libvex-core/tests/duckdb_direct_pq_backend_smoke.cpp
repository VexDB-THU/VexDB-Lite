#include "vex/vex_adapter_duckdb_mock.hpp"
#include "vex/vex_adapter_duckdb_stub.hpp"
#include "vex/vex_graph_algo.hpp"
#include "vex/vex_quant_distancer.hpp"

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
    cfg.index_name = "direct_pq_mock_smoke";
    cfg.low_level_binding = binding;

    vex::DuckDBNodeStore store(cfg);
    vex::HNSWGraph graph(store, vex::Metric::L2, cfg.m, 32);

    std::vector<float> data = {
        0.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
        2.0f, 2.0f, 2.0f, 2.0f,
    };

    auto node0 = graph.AddPoint(400, &data[0], cfg.dimension);
    auto node1 = graph.AddPoint(401, &data[4], cfg.dimension);
    auto node2 = graph.AddPoint(402, &data[8], cfg.dimension);

    vex::PQDistancerCore distancer(/*pq_m=*/2);
    distancer.Train(data.data(), 3, cfg.dimension);

    std::vector<std::vector<uint8_t>> codes(3, std::vector<uint8_t>(distancer.CodeSize(), 0));
    distancer.ComputeCode(&data[0], codes[0].data());
    distancer.ComputeCode(&data[4], codes[1].data());
    distancer.ComputeCode(&data[8], codes[2].data());

    float query[4] = {1.1f, 1.1f, 1.1f, 1.1f};
    std::vector<vex::row_id_t> rows;
    std::vector<float> dists;
    graph.SearchWithQuantizedCodes(
        query, 2, 16, rows, dists, distancer,
        [&](vex::node_id_t node_id) -> const uint8_t * {
            if (node_id == node0) {
                return codes[0].data();
            }
            if (node_id == node1) {
                return codes[1].data();
            }
            if (node_id == node2) {
                return codes[2].data();
            }
            return nullptr;
        });

    if (rows.empty()) {
        std::cerr << "duckdb direct PQ backend search empty" << std::endl;
        return 1;
    }
    if (rows[0] != 401) {
        std::cerr << "unexpected top-1 row_id for PQ search: " << rows[0] << std::endl;
        return 2;
    }

    return 0;
}
