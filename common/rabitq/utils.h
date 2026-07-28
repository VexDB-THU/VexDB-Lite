/**
 * Copyright (c) 2026 VexDB-THU
 * RaBitQ Auxiliary Data Structures and Utilities
 */

#ifndef RABITQ_UTILS_H
#define RABITQ_UTILS_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

#include "rabitq/platform.h"

#define HNSW_RABITQ_EX_BITS 8
#define HNSW_RABITQ_NUM_CLUSTERS 16 /* TODO */
#define RABITQ_EX_BITS HNSW_RABITQ_EX_BITS /* TODO */
#define RABITQ_PADDED_DIM(dim)   ((dim + 63) / 64 * 64)
#define RABITQ_BIN_CODE_SIZE(dim)   ((dim + 63) / 64 * sizeof(uint64))
#define RABITQ_BIN_DATA_SIZE(dim)   (RABITQ_BIN_CODE_SIZE(dim) + 3 * sizeof(float))
#define RABITQ_EXT_CODE_SIZE(dim)  ((dim + 7) / 8 * RABITQ_EX_BITS * sizeof(uint8))
#define RABITQ_EXT_DATA_SIZE(dim)  (RABITQ_EXT_CODE_SIZE(dim) + 3 * sizeof(float))

namespace rabitq {

// Keep the first code payload 8-byte aligned across every host adapter.
inline constexpr size_t kCodeHeaderSize = sizeof(uint64_t);

struct BinDataWithFactors {
    BinDataWithFactors(char *bin_data, int bin_code_size)
    {
        bin_code = (uint64 *)bin_data;
        f_add = (float *)(bin_data + bin_code_size);
        f_rescale = f_add + 1;
        f_error = f_rescale + 1;
    }

    uint64 *bin_code;
    float *f_add;
    float *f_rescale;
    float *f_error;
};

struct ExtDataWithFactors {
    ExtDataWithFactors(char *ext_data, int ext_code_size)
    {
        ex_code = (uint8 *)ext_data;
        f_add_ex = (float *)(ext_data + ext_code_size);
        f_rescale_ex = f_add_ex + 1;
        f_error_ex = f_rescale_ex + 1;
    }

    uint8 *ex_code;
    float *f_add_ex;
    float *f_rescale_ex;
    float *f_error_ex;
};

enum ScalarQuantizerType : uint8 {
    RECONSTRUCTION, UNBIASED_ESTIMATION, PLAIN
};

typedef struct EstimateRecord {
    float ip_x0_qr;
    float est_dist;
    float low_dist;

    bool operator<(const EstimateRecord& other) const {
        return this->est_dist < other.est_dist;
    }
} EstimateRecord;

template <typename T>
inline void generate_random_matrix(T *matrix, int rows, int cols)
{
    /* A fixed local generator makes the query scaling constant reproducible
     * and avoids std::random_device throwing across a PostgreSQL boundary. */
    uint64 state = UINT64_C(0x9e3779b97f4a7c15) ^
        (static_cast<uint64>(rows) << 32) ^ static_cast<uint64>(cols);
    auto next_uniform = [&state]() -> double {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        uint64 value = state * UINT64_C(2685821657736338717);
        return (static_cast<double>(value >> 11) + 1.0) /
            (static_cast<double>(UINT64_C(1) << 53) + 1.0);
    };

    bool has_spare = false;
    double spare = 0.0;
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            if (has_spare) {
                matrix[i * cols + j] = static_cast<T>(spare);
                has_spare = false;
                continue;
            }
            double radius = std::sqrt(-2.0 * std::log(next_uniform()));
            double angle = 6.28318530717958647692 * next_uniform();
            matrix[i * cols + j] = static_cast<T>(radius * std::cos(angle));
            spare = radius * std::sin(angle);
            has_spare = true;
        }
    }
}

constexpr float kTightStart[9] = {
    0.0f, 0.15f, 0.20f, 0.52f, 0.59f, 0.71f, 0.75f, 0.77f, 0.81f,
};

template <typename T>
inline double best_rescale_factor(T* o_abs, int dim, int ex_bits)
{
    constexpr double kEps = 1e-5;
    constexpr int kNEnum = 10;
    double max_o = *std::max_element(o_abs, o_abs + dim);

    if (max_o <= kEps) {
        return 0.0;
    }

    double t_end = static_cast<double>(((1 << ex_bits) - 1) + kNEnum) / max_o;
    double t_start = t_end * kTightStart[ex_bits];

    int *cur_o_bar = alloc_array_zero<int>(dim);
    double sqr_denominator = static_cast<double>(dim) * 0.25;
    double numerator = 0;

    for (int i = 0; i < dim; ++i) {
        int cur = static_cast<int>((t_start * o_abs[i]) + kEps);
        cur_o_bar[i] = cur;
        sqr_denominator += cur * cur + cur;
        numerator += (cur + 0.5) * o_abs[i];
    }

    using QueueItem = std::pair<double, size_t>;
    std::priority_queue<QueueItem,
        std::vector<QueueItem, HostAllocator<QueueItem>>,
        std::greater<QueueItem>> next_t;

    for (int i = 0; i < dim; ++i) {
        if (o_abs[i] > kEps) {
            next_t.emplace(static_cast<double>(cur_o_bar[i] + 1) / o_abs[i], i);
        }
    }

    double max_ip = 0;
    double t = 0;

    while (!next_t.empty()) {
        double cur_t = next_t.top().first;
        size_t update_id = next_t.top().second;
        next_t.pop();

        cur_o_bar[update_id]++;
        int update_o_bar = cur_o_bar[update_id];
        sqr_denominator += 2 * update_o_bar;
        numerator += o_abs[update_id];

        double cur_ip = numerator / std::sqrt(sqr_denominator);
        if (cur_ip > max_ip) {
            max_ip = cur_ip;
            t = cur_t;
        }

        if (update_o_bar < (1 << ex_bits) - 1 && o_abs[update_id] > kEps) {
            double t_next = static_cast<double>(update_o_bar + 1) / o_abs[update_id];
            if (t_next < t_end) {
                next_t.emplace(t_next, update_id);
            }
        }
    }

    free_mem(cur_o_bar);

    return t;
}

template <typename T>
inline void rowwise_norm_abs(T *matrix, long rows, long cols)
{
    for (long i = 0; i < rows; ++i) {
        T norm = 0.0f;
        for (long j = 0; j < cols; ++j) {
            norm += matrix[i * cols + j] * matrix[i * cols + j];
        }
        norm = std::sqrt(norm);
        if (norm > 0.0f) {
            for (long j = 0; j < cols; ++j) {
                matrix[i * cols + j] = std::abs(matrix[i * cols + j]) / norm;
            }
        } else {
            for (long j = 0; j < cols; ++j) {
                matrix[i * cols + j] = 0.0f;
            }
        }
    }
}

// For given dim and ex_bits, use random vectors to get the const rescale factor
inline double get_const_scaling_factors(int dim, int ex_bits) {
    if (dim <= 0) {
        return -1.0f;
    }

    constexpr long kConstNum = 100;

    double *matrix = alloc_array_zero<double>(kConstNum * dim);

    generate_random_matrix<double>(matrix, kConstNum, dim);

    rowwise_norm_abs<double>(matrix, kConstNum, dim);

    double sum = 0;
    for (long i = 0; i < kConstNum; ++i) {
        sum += best_rescale_factor<double>(matrix + i * dim, dim, ex_bits);
    }

    double t_const = sum / kConstNum;

    free_mem(matrix);

    return t_const;
}
} /* namespace rabitq */

#endif /* RABITQ_UTILS_H */
