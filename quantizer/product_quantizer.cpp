// Backend-neutral port of openGauss's pq.cpp. Algorithm preserved verbatim;
// scaffolding swapped to PQContext (see pq_alloc.h):
//
//   palloc / palloc_extended / pfree -> ctx.allocator
//   ereport(ERROR, ...)              -> VEX_QUANT_ERROR
//   FloatVectorArray (training)      -> PQFloatArray + AllocFloatArray helper
//   TaskRunner                       -> ctx.parallel.Run
//   memcpy_s / securec_check         -> std::memcpy
//
// SIMD distance kernels come from src/distance/core/ via the existing
// get_fvec_L2sqr_ny_nearest_func / get_fvec_ny_distance_func /
// get_distance_*_code_func helpers (already shared between duck and PG).
#include "quantizer/product_quantizer.h"

#include "quantizer/annkmeans.h"
#include "quantizer/pq_endecode.h"
#include "distance/core/distance.h"

#include <cstring>
#include <vector>

namespace vex {
namespace quantizer {

uint32_t ProductQuantizer::AutoSelectM(uint32_t dim) {
    // Prefer 4-8 dims per subquantizer (good recall/code-size tradeoff).
    for (uint32_t target_dsub : {4u, 8u, 3u, 6u, 2u, 5u, 1u}) {
        if (dim % target_dsub == 0) {
            uint32_t candidate_m = dim / target_dsub;
            if (candidate_m >= 1 && candidate_m <= dim) {
                return candidate_m;
            }
        }
    }
    return dim;
}

void ProductQuantizer::set_basic_values(size_t dim, size_t m, size_t nbits_in) {
    d     = dim;
    M     = m;
    nbits = nbits_in;
    // set_derived_values() must be called separately so the caller can pass
    // PQContext (centroid allocation goes through ctx.allocator).
}

void ProductQuantizer::set_derived_values(const PQContext &ctx) {
    if (d % M != 0) {
        VEX_QUANT_ERROR("PQ: dimension (" + std::to_string(d) +
                        ") must be a multiple of subquantizer count (" + std::to_string(M) + ")");
    }
    dsub      = d / M;
    code_size = (nbits * M + 7) / 8;
    ksub      = 1ULL << nbits;
    // Centroid table can exceed 1GB (e.g. d=1024, M=64, ksub=256 -> ~1GB).
    // Route through AllocHugeZero so PG can pass MCXT_ALLOC_HUGE; std::malloc
    // already supports >1GB so duck falls through unchanged.
    centroids = static_cast<float *>(ctx.allocator.AllocHugeZero(d * ksub * sizeof(float)));
    if (centroids == nullptr) {
        VEX_QUANT_ERROR("PQ: failed to allocate " +
                        std::to_string(d * ksub * sizeof(float)) + " bytes for centroids");
    }
}

void ProductQuantizer::set_fvec_L2sqr_ny_nearest_func() {
    _fvec_L2sqr_ny_nearest_func = ann_helper::get_fvec_L2sqr_ny_nearest_func();
}

void ProductQuantizer::set_fvec_ny_distance_func(::Metric metric) {
    // PQ training is L2-only. Cosine indexes pre-normalize so L2 over unit
    // vectors gives the right ranking; INNER_PRODUCT gets its own kernel.
    _fvec_ny_distance_func = ann_helper::get_fvec_ny_distance_func(
        metric == ::Metric::INNER_PRODUCT ? ::Metric::INNER_PRODUCT : ::Metric::L2);
}

void ProductQuantizer::set_dist_code_func() {
    _distance_single_code_func = ann_helper::get_distance_single_code_func(static_cast<uint32_t>(nbits));
    _distance_four_codes_func  = ann_helper::get_distance_four_codes_func(static_cast<uint32_t>(nbits));
}

void ProductQuantizer::free_resources(const PQContext &ctx) {
    ctx.allocator.Free(centroids);
    centroids = nullptr;
    trained   = false;
}

void ProductQuantizer::set_params(const PQFloatArray &subcenters, size_t m) {
    for (size_t i = 0; i < ksub; i++) {
        std::memcpy(get_centroids(m, i), subcenters.Get(i), dsub * sizeof(float));
    }
}

void ProductQuantizer::train(const KMeansState &kmeans_state,
                             PQFloatArray samples,
                             int avg_work_mem_kb,
                             const PQContext &ctx) {
    size_t n = samples.length;
    if (n == 0) {
        VEX_QUANT_ERROR("PQ: train called with empty sample set");
    }

    // Each subquantizer runs independent K-means on the m-th `dsub`-slice.
    // openGauss runs these in parallel across `M` workers; we pass the same
    // task to ctx.parallel which serializes when no driver is provided.
    ctx.progress.Report(0, M, "kmeans subq");
    ctx.parallel.Run(M, [&](size_t m) {
        // Subvector buffer: pack samples[*][m*dsub : (m+1)*dsub] into a
        // contiguous float array K-means expects.
        PQFloatArray subvecs;
        subvecs.maxlen = n;
        subvecs.length = n;
        subvecs.dim    = dsub;
        subvecs.data   = static_cast<float *>(ctx.allocator.Alloc(n * dsub * sizeof(float)));

        for (size_t j = 0; j < n; j++) {
            const float *src = samples.Get(j);
            std::memcpy(subvecs.Get(j), src + m * dsub, dsub * sizeof(float));
        }

        PQFloatArray subcenters;
        subcenters.maxlen = ksub;
        subcenters.length = 0;
        subcenters.dim    = dsub;
        subcenters.data   = static_cast<float *>(ctx.allocator.AllocZero(ksub * dsub * sizeof(float)));

        AnnKmeans(kmeans_state, subvecs, subcenters, avg_work_mem_kb, ctx);

        // Copy trained centroids into the global table at slot m.
        set_params(subcenters, m);

        ctx.allocator.Free(subvecs.data);
        ctx.allocator.Free(subcenters.data);

        // Report progress per finished subquantizer. With ctx.parallel.Run
        // serialized (default), `m` increments monotonically; with a real
        // parallel driver this becomes a "n done so far" counter without any
        // ordering guarantee — fine for a NOTICE-style progress signal.
        ctx.progress.Report(m + 1, M, "kmeans subq");
    });

    trained = true;
}

namespace {

// Generic encoder template — same pattern as openGauss compute_code_generic.
// Picks the nearest centroid per subquantizer, then bit-packs the indices.
template <class Encoder>
void compute_code_generic(const ProductQuantizer &pq, const float *x, uint8_t *code,
                          float *distances_scratch) {
    Encoder encoder(code, static_cast<int>(pq.nbits));
    for (size_t m = 0; m < pq.M; m++) {
        const float *xsub = x + m * pq.dsub;
        uint64_t idxm = pq._fvec_L2sqr_ny_nearest_func(
            distances_scratch, xsub, pq.get_centroids(m, 0),
            static_cast<uint32_t>(pq.dsub), static_cast<uint32_t>(pq.ksub));
        encoder.encode(idxm);
    }
    encoder.restore_code();
}

} // namespace

void ProductQuantizer::compute_code(const float *x, uint8_t *code) const {
    // Scratch buffer for the SIMD nearest-neighbor function. Stack-allocated
    // since ksub is bounded (typically 256). Fall back to heap for huge nbits.
    if (ksub <= 4096) {
        float distances[4096];
        switch (nbits) {
            case 8:  compute_code_generic<PQEncoder8>(*this, x, code, distances); break;
            case 16: compute_code_generic<PQEncoder16>(*this, x, code, distances); break;
            default: compute_code_generic<PQEncoderGeneric>(*this, x, code, distances); break;
        }
        return;
    }
    std::vector<float> distances(ksub);
    switch (nbits) {
        case 8:  compute_code_generic<PQEncoder8>(*this, x, code, distances.data()); break;
        case 16: compute_code_generic<PQEncoder16>(*this, x, code, distances.data()); break;
        default: compute_code_generic<PQEncoderGeneric>(*this, x, code, distances.data()); break;
    }
}

void ProductQuantizer::compute_distance_table(const float *x, float *dist_table) const {
    for (size_t m = 0; m < M; m++) {
        _fvec_ny_distance_func(dist_table + m * ksub, x + m * dsub,
                               get_centroids(m, 0),
                               static_cast<uint32_t>(dsub),
                               static_cast<uint32_t>(ksub));
    }
}

float ProductQuantizer::distance_to_code(const uint8_t *code, const float *dist_table) const {
    return _distance_single_code_func(static_cast<uint32_t>(M),
                                      static_cast<uint32_t>(nbits),
                                      dist_table, code);
}

void ProductQuantizer::distance_to_four_code(const float *dist_table,
                                             const uint8_t *code0, const uint8_t *code1,
                                             const uint8_t *code2, const uint8_t *code3,
                                             float &result0, float &result1,
                                             float &result2, float &result3) const {
    _distance_four_codes_func(static_cast<uint32_t>(M), static_cast<uint32_t>(nbits),
                              dist_table, code0, code1, code2, code3,
                              result0, result1, result2, result3);
}

} // namespace quantizer
} // namespace vex
