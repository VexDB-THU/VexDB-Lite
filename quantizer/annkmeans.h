// K-means clustering for codebook training, ported from openGauss
// src/include/access/annvector/annkmeans.h.
//
// Backend-neutral. The PG-only sampling helpers (setupKmeansState,
// ann_sample_rows, GetSampleNumbers) are NOT ported — the caller supplies a
// PQFloatArray of samples already (each backend has its own way to fetch
// rows from the table).
//
// Algorithm: Elkan's accelerated K-means with k-means++ initialization, plus
// the QuickCenters fast path when samples.length <= centers.maxlen.
//
// Reference:
//   Elkan, "Using the Triangle Inequality to Accelerate k-Means", ICML 2003
//   Arthur & Vassilvitskii, "k-means++", SODA 2007
#pragma once

#include "quantizer/pq_alloc.h"

#include <cstdint>

namespace vex {
namespace quantizer {

// Distance function compatible with the existing src/distance/core/ dispatcher.
// Wraps the function pointer so K-means stays decoupled from the dispatcher's
// exact symbol names (the duck adapter currently aliases this to
// `ann_helper::distance_func`; PG aliases to its own typedef).
using KMeansDistanceFn = float (*)(const void *a, const void *b, uint16_t dim);

struct KMeansState {
    KMeansDistanceFn distance_fn = nullptr;
    KMeansDistanceFn norm_fn     = nullptr;  // optional: apply unit-norm to centers (cosine)
    bool             skip_check_duplicate = false;
};

// Run k-means on `samples`, write `centers->maxlen` centroids into `centers`.
// On entry: centers->data must point to caller-allocated buffer of size
//           centers->maxlen * centers->dim, centers->length must be 0.
// On exit:  centers->length == centers->maxlen.
// `avg_work_mem_kb` caps internal scratch allocations; openGauss passes
// maintenance_work_mem.
void AnnKmeans(const KMeansState &state,
               PQFloatArray samples,
               PQFloatArray &centers,
               int avg_work_mem_kb,
               const PQContext &ctx);

} // namespace quantizer
} // namespace vex
