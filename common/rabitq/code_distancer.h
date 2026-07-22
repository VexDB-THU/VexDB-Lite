/**
 * Backend-neutral RaBitQ code encoder and query distancer.
 *
 * Host adapters own training and persistence. This class owns only the
 * query-local estimator and the stable on-disk code layout:
 *   [8-byte header: uint16 cluster id + zero padding]
 *   [binary code + factors][extended code + factors]
 */
#ifndef VEX_RABITQ_CODE_DISTANCER_H
#define VEX_RABITQ_CODE_DISTANCER_H

#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "rabitq/estimator.h"
#include "rabitq/rabitq.h"

namespace rabitq {

inline size_t CodeSize(int dim) {
    const int padded_dim = RABITQ_PADDED_DIM(dim);
    return kCodeHeaderSize + RABITQ_BIN_DATA_SIZE(padded_dim) +
           RABITQ_EXT_DATA_SIZE(padded_dim);
}

inline bool CodeHasValidCluster(const void *raw_code) {
    if (!raw_code) return false;
    uint16_t cluster_id = 0;
    std::memcpy(&cluster_id, raw_code, sizeof(cluster_id));
    return cluster_id < HNSW_RABITQ_NUM_CLUSTERS;
}

inline bool CodeHasFiniteFactors(const void *raw_code, int dim) {
    if (!raw_code) return false;
    const int padded_dim = RABITQ_PADDED_DIM(dim);
    const size_t bin_code_size = RABITQ_BIN_CODE_SIZE(padded_dim);
    const size_t bin_size = RABITQ_BIN_DATA_SIZE(padded_dim);
    const size_t ext_code_size = RABITQ_EXT_CODE_SIZE(padded_dim);
    const auto *code = static_cast<const uint8_t *>(raw_code);
    const auto *bin_factors = code + kCodeHeaderSize + bin_code_size;
    const auto *ext_factors = code + kCodeHeaderSize + bin_size + ext_code_size;
    for (size_t i = 0; i < 3; i++) {
        float bin_factor = 0.0f;
        float ext_factor = 0.0f;
        std::memcpy(&bin_factor, bin_factors + i * sizeof(float), sizeof(float));
        std::memcpy(&ext_factor, ext_factors + i * sizeof(float), sizeof(float));
        if (!std::isfinite(bin_factor) || !std::isfinite(ext_factor)) return false;
    }
    return true;
}

inline void EncodeCode(RaBitQuantizer &quantizer, int dim, const float *vec,
                       uint8_t *code) {
    const size_t cid_size = kCodeHeaderSize;
    const size_t bin_size = RABITQ_BIN_DATA_SIZE(RABITQ_PADDED_DIM(dim));
    auto *bin_data = reinterpret_cast<char *>(code + cid_size);
    auto *ext_data = bin_data + bin_size;
    uint16_t cluster_id = static_cast<uint16_t>(
        quantizer.quantize(const_cast<float *>(vec), bin_data, ext_data));
    std::memset(code, 0, cid_size);
    std::memcpy(code, &cluster_id, sizeof(cluster_id));
    if (!CodeHasValidCluster(code) || !CodeHasFiniteFactors(code, dim)) {
        fail("encoded code contains invalid values");
    }
}

class CodeDistancer {
public:
    static constexpr bool has_estimation_func = true;
    static constexpr bool need_refine = false;

    CodeDistancer(RaBitQuantizer &quantizer, int dim, Metric metric,
                  double query_rescaling_factor)
        : quantizer_(quantizer),
          dim_(dim),
          padded_dim_(RABITQ_PADDED_DIM(dim)),
          cid_size_(kCodeHeaderSize),
          bin_size_(RABITQ_BIN_DATA_SIZE(padded_dim_)),
          code_size_(CodeSize(dim)),
          estimator_(padded_dim_, metric, query_rescaling_factor) {
        estimator_.set_quantizer(&quantizer_);
    }

    CodeDistancer(const CodeDistancer &) = delete;
    CodeDistancer &operator=(const CodeDistancer &) = delete;

    size_t code_size() const {
        return code_size_;
    }

    void process(const float *query) {
        estimator_.preprocess(const_cast<float *>(query));
    }

    void compute_code(const float *vec, uint8_t *code) {
        EncodeCode(quantizer_, dim_, vec, code);
    }

    float get_distance_est_single(const void *, const void *code, uint16_t) const {
        if (!code) fail("estimate code is missing");
        auto parts = Decode(code);
        return estimator_.get_bin_dist(parts.cluster_id, parts.bin_data);
    }

    float get_distance_single(const void *, const void *code, uint16_t) const {
        if (!code) fail("full code is missing");
        auto parts = Decode(code);
        return estimator_.get_full_dist(parts.cluster_id, parts.bin_data, parts.ext_data);
    }

    void get_distance_est_batch2(const void *query, void *const *codes, uint16_t dim,
                                 uint16_t count, float *out) const {
        if (!codes && count) fail("estimate code batch is missing");
        for (uint16_t i = 0; i < count; i++) {
            out[i] = get_distance_est_single(query, codes[i], dim);
        }
    }

    void get_distance_batch2(const void *query, void *const *codes, uint16_t dim,
                             uint16_t count, float *out) const {
        if (!codes && count) fail("full code batch is missing");
        for (uint16_t i = 0; i < count; i++) {
            out[i] = get_distance_single(query, codes[i], dim);
        }
    }

    void destroy() {}

private:
    struct CodeParts {
        uint16_t cluster_id;
        char *bin_data;
        char *ext_data;
    };

    CodeParts Decode(const void *raw_code) const {
        if (!raw_code) {
            fail("code is missing");
        }
        auto *code = const_cast<uint8_t *>(static_cast<const uint8_t *>(raw_code));
        uint16_t cluster_id = 0;
        std::memcpy(&cluster_id, code, sizeof(cluster_id));
        if (!CodeHasValidCluster(code) || !CodeHasFiniteFactors(code, dim_)) {
            fail("code contains invalid values");
        }
        auto *bin_data = reinterpret_cast<char *>(code + cid_size_);
        return {cluster_id, bin_data, bin_data + bin_size_};
    }

    RaBitQuantizer &quantizer_;
    int dim_;
    int padded_dim_;
    size_t cid_size_;
    size_t bin_size_;
    size_t code_size_;
    mutable RaBitQEstimator estimator_;
};

} // namespace rabitq

#endif // VEX_RABITQ_CODE_DISTANCER_H
