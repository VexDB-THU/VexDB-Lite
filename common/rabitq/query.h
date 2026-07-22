/**
 * Copyright (c) 2026 VexDB-THU
 * RaBitQ Query Wrapper
 */

#ifndef RABITQ_QUERY_H
#define RABITQ_QUERY_H

#include <numeric>
#include "distance/core/distance.h"
#include "rabitq/rabitq.h"

namespace rabitq {

class QueryWrapper {
public:
    static constexpr size_t kNumBits = query_kNumBits;

    QueryWrapper(int padded_dim, Metric metric)
        : _padded_dim(padded_dim),
          _metric(metric)
    {
        _query_bin = alloc_array_zero<uint64>(padded_dim * kNumBits / 64);
        _centroid = alloc_array_zero<float>(_padded_dim);
        _quant_query = alloc_array_zero<uint16>(_padded_dim);
    }
    QueryWrapper() = delete;
    ~QueryWrapper() { destroy(); }

    void preprocess(float *rotated_query, RaBitQuantizer *quantizer)
    {
        float c_1 = -static_cast<float>((1 << 1) - 1) / 2.0f;
        float c_b = -static_cast<float>((1 << (HNSW_RABITQ_EX_BITS + 1)) - 1) / 2.0f;

        float sumq = std::accumulate(rotated_query, rotated_query + _padded_dim, static_cast<float>(0));

        _G_k1xSumq = sumq * c_1;
        _G_kbxSumq = sumq * c_b;

        quantizer->quantize_scalar(rotated_query, _centroid, kNumBits, _quant_query, _delta, _vl);

        /* represent quantized query as u64 */
        new_transpose_bin(_quant_query, _query_bin, _padded_dim);
    }

    float delta() const { return _delta; }
    float vl() const { return _vl; }
    float k1xsumq() const { return _G_k1xSumq; }
    float kbxsumq() const { return _G_kbxSumq; }
    float g_add() const { return _G_add; }
    float g_error() const { return _G_error; }

    void set_g_add(float norm, float ip = 0) {
        if (_metric == Metric::L2) {
            _G_add = norm * norm;
            _G_error = norm;
        } else if (_metric == Metric::INNER_PRODUCT) {
            _G_add = -ip;
            _G_error = norm;
        }
    }

    void set_g_error(float norm) { _G_error = norm; }

    const uint64 *query_bin() const { return _query_bin; }

    void destroy()
    {
        free_mem(_query_bin);
        free_mem(_centroid);
        free_mem(_quant_query);
        _query_bin = nullptr;
        _centroid = nullptr;
        _quant_query = nullptr;
    }

private:
    float _G_add;
    float _G_k1xSumq;
    float _G_kbxSumq;
    float _G_error;
    float _delta;
    float _vl;
    int _padded_dim;
    Metric _metric;
    uint64 *_query_bin;
    float *_centroid;
    uint16 *_quant_query;

    void new_transpose_bin(uint16 *q, uint64 *tq, size_t padded_dim)
    {
        for (size_t i = 0; i < padded_dim; i += 64) {
            for (size_t j = 0; j < kNumBits; ++j) {
                uint64 result = 0;
                for (size_t k = 0; k < 64; ++k) {
                    uint16 val = q[k];
                    uint16 bit = (val >> (kNumBits - 1 - j)) & 1;
                    result |= (static_cast<uint64>(bit) << (63 - k));
                }
                tq[kNumBits - j - 1] = result;
            }
            tq += kNumBits;
            q += 64;
        }
    }
};

} /* namespace rabitq */

#endif /* RABITQ_QUERY_H */
