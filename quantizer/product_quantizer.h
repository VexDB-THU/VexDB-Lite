// Product Quantizer ported from openGauss
// src/include/access/annvector/pq.h. Backend-neutral; uses our shared
// PQContext (allocator + random + parallel) instead of palloc / RandomInt /
// TaskRunner. Distance kernels come from src/distance/core/.
//
// Implemented for METRIC_L2 today; the cosine path goes through pre-normalized
// vectors at the backend layer (matches what PG and duck adapters already do).
#pragma once

#include "quantizer/annkmeans.h"
#include "quantizer/pq_alloc.h"

#include "distance/core/distance.h"
#include "distance/core/distance_func.h"

#include <cstdint>

namespace vex {
namespace quantizer {

struct ProductQuantizer {
    size_t d         = 0;  // input dimension
    size_t M         = 0;  // number of subquantizers
    size_t nbits     = 8;  // bits per subquantizer index (8, 16, or arbitrary)
    size_t dsub      = 0;  // d / M (set in set_derived_values)
    size_t ksub      = 0;  // 1 << nbits
    size_t code_size = 0;  // bytes per encoded vector

    // SIMD function pointers from src/distance/core/. Wired in
    // set_fvec_L2sqr_ny_nearest_func / set_fvec_ny_distance_func / set_dist_code_func.
    ann_helper::fvec_L2sqr_ny_nearest_func _fvec_L2sqr_ny_nearest_func = nullptr;
    ann_helper::fvec_ny_distance_func      _fvec_ny_distance_func      = nullptr;
    ann_helper::distance_single_code_func  _distance_single_code_func  = nullptr;
    ann_helper::distance_four_codes_func   _distance_four_codes_func   = nullptr;

    // Centroid table. Size = M * ksub * dsub floats. Layout: (M, ksub, dsub).
    float *centroids = nullptr;
    bool   trained   = false;

    size_t get_centroids_size() const { return d * ksub; }

    float *get_centroids(size_t m, size_t i) {
        return &centroids[(m * ksub + i) * dsub];
    }
    const float *get_centroids(size_t m, size_t i) const {
        return &centroids[(m * ksub + i) * dsub];
    }

    // Pick a subquantizer count that divides `dim` and gives ~4-8 dims per
    // subquantizer. Falls back to dim (1 dim per subquantizer) if no nice
    // divisor exists. Ported from main's product_quantizer.cpp.
    static uint32_t AutoSelectM(uint32_t dim);

    void set_basic_values(size_t dim, size_t m, size_t nbits_in);
    void set_derived_values(const PQContext &ctx);
    void set_fvec_L2sqr_ny_nearest_func();
    void set_fvec_ny_distance_func(::Metric metric);
    void set_dist_code_func();

    // Free centroids via ctx.allocator. Caller is responsible for invocation
    // (no destructor — backends own the lifecycle).
    void free_resources(const PQContext &ctx);

    // Train the codebook from `samples`. Each subquantizer is trained
    // independently; if ctx.parallel is non-trivial they run in parallel.
    void train(const KMeansState &kmeans_state,
               PQFloatArray samples,
               int avg_work_mem_kb,
               const PQContext &ctx);

    // Copy ksub centroids for the m-th subquantizer from an external buffer
    // into this quantizer's centroid table. Mirrors openGauss
    // ProductQuantizer::set_params; used both internally by train() and by
    // reload-from-disk paths that materialize codebooks subquantizer by
    // subquantizer.
    void set_params(const PQFloatArray &subcenters, size_t m);

    // Quantize one vector into `code` (must point to code_size bytes).
    void compute_code(const float *x, uint8_t *code) const;

    // Distance from a code to a query, using a precomputed M*ksub table.
    float distance_to_code(const uint8_t *code, const float *dist_table) const;
    void  distance_to_four_code(const float *dist_table,
                                const uint8_t *code0, const uint8_t *code1,
                                const uint8_t *code2, const uint8_t *code3,
                                float &result0, float &result1,
                                float &result2, float &result3) const;

    // dist_table[m * ksub + j] = ||x_m - centroid(m, j)||^2.
    void compute_distance_table(const float *x, float *dist_table) const;
};

} // namespace quantizer
} // namespace vex
