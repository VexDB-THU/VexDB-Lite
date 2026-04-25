#include "vex/vex_core.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    vex_index_config_t cfg{};
    cfg.dimension = 4;
    cfg.m = 8;
    cfg.ef_construction = 32;
    cfg.metric = VEX_METRIC_L2;

    vex_index *idx = vex_index_create(&cfg);
    if (!idx) {
        std::cerr << "create failed\n";
        return 1;
    }

    std::vector<float> base = {
        0.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
        2.0f, 2.0f, 2.0f, 2.0f,
        3.0f, 3.0f, 3.0f, 3.0f,
    };
    std::vector<int64_t> row_ids = {10, 11, 12, 13};

    int rc = vex_index_add_batch(idx, row_ids.data(), base.data(), cfg.dimension,
                                 static_cast<uint32_t>(row_ids.size()), 1);
    if (rc != VEX_OK) {
        std::cerr << "add batch failed: " << rc << "\n";
        vex_index_destroy(idx);
        return 2;
    }

    float query[4] = {1.1f, 1.1f, 1.1f, 1.1f};
    vex_result_t before[2]{};
    uint32_t before_count = 0;
    rc = vex_index_search(idx, query, cfg.dimension, 2, 32, before, &before_count);
    if (rc != VEX_OK || before_count == 0) {
        std::cerr << "search before serialize failed\n";
        vex_index_destroy(idx);
        return 3;
    }

    void *blob = nullptr;
    size_t blob_size = 0;
    rc = vex_index_serialize(idx, &blob, &blob_size);
    if (rc != VEX_OK || blob == nullptr || blob_size == 0) {
        std::cerr << "serialize failed\n";
        vex_index_destroy(idx);
        return 4;
    }

    vex_index *idx2 = vex_index_deserialize(blob, blob_size, &cfg);
    vex_free_buffer(blob);
    vex_index_destroy(idx);

    if (!idx2) {
        std::cerr << "deserialize failed\n";
        return 5;
    }

    vex_result_t after[2]{};
    uint32_t after_count = 0;
    rc = vex_index_search(idx2, query, cfg.dimension, 2, 32, after, &after_count);
    vex_index_destroy(idx2);
    if (rc != VEX_OK || after_count == 0) {
        std::cerr << "search after deserialize failed\n";
        return 6;
    }

    if (before_count != after_count || before[0].row_id != after[0].row_id) {
        std::cerr << "roundtrip mismatch\n";
        return 7;
    }

    // Ensure id 0 slot ambiguity does not exist (first inserted row_id should remain searchable).
    vex_index *idx3 = vex_index_create(&cfg);
    if (!idx3) {
        std::cerr << "create failed for id0 guard\n";
        return 8;
    }
    float v0[4] = {5.0f, 5.0f, 5.0f, 5.0f};
    float v1[4] = {6.0f, 6.0f, 6.0f, 6.0f};
    if (vex_index_add(idx3, 100, v0, cfg.dimension) != VEX_OK ||
        vex_index_add(idx3, 101, v1, cfg.dimension) != VEX_OK) {
        std::cerr << "add failed for id0 guard\n";
        vex_index_destroy(idx3);
        return 9;
    }
    float q2[4] = {5.05f, 5.05f, 5.05f, 5.05f};
    vex_result_t r2[1]{};
    uint32_t c2 = 0;
    if (vex_index_search(idx3, q2, cfg.dimension, 1, 16, r2, &c2) != VEX_OK || c2 != 1 || r2[0].row_id != 100) {
        std::cerr << "id0 guard failed\n";
        vex_index_destroy(idx3);
        return 10;
    }
    vex_index_destroy(idx3);

    return 0;
}
