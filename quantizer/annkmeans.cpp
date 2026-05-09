// Backend-neutral port of openGauss's annkmeans.cpp. The Elkan K-means
// algorithm is preserved verbatim; only the surrounding scaffolding changes:
//
//   palloc/pfree                  -> ctx.allocator.Alloc/Free
//   ereport(ERROR, ...)           -> VEX_QUANT_ERROR
//   RandomInt / RandomDouble      -> ctx.random.RandomInt / RandomDouble
//   PARALLEL_BATCH_RUN_TASK_WAIT  -> ctx.parallel.Run (default = serial)
//   CHECK_FOR_INTERRUPTS / IvfBench -> dropped (PG wrapper can re-add via
//                                       parallel callback if needed)
//   FloatVectorArrayInit          -> AllocFloatArray helper using ctx
//   qsort_arg                     -> std::sort with lambda
//   memcpy_s / securec_check      -> std::memcpy (no securec library on duck)
//   palloc_huge                   -> ctx.allocator.Alloc (size limit is the
//                                       caller's avg_work_mem_kb cap)
//
// PG-specific sampling helpers (setupKmeansState, ann_sample_rows,
// GetSampleNumbers) are NOT ported; each backend prepares samples itself.
#include "quantizer/annkmeans.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstring>
#include <vector>

namespace vex {
namespace quantizer {

namespace {

PQFloatArray AllocFloatArray(const PQContext &ctx, size_t maxlen, size_t dim) {
    PQFloatArray arr;
    arr.maxlen = maxlen;
    arr.length = 0;
    arr.dim    = dim;
    arr.data   = static_cast<float *>(ctx.allocator.AllocZero(maxlen * dim * sizeof(float)));
    return arr;
}

void FreeFloatArray(const PQContext &ctx, PQFloatArray &arr) {
    ctx.allocator.Free(arr.data);
    arr.data = nullptr;
}

int CompareVectors(const float *a, const float *b, size_t dim) {
    for (size_t i = 0; i < dim; i++) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

void ApplyNorm(KMeansDistanceFn norm_fn, float *vec, uint16_t dim) {
    double norm = norm_fn(vec, vec, dim);
    if (norm > 0) {
        for (uint16_t i = 0; i < dim; i++) {
            vec[i] /= static_cast<float>(norm);
        }
    }
}

// Initialize centers with k-means++.
//   Arthur & Vassilvitskii, SODA 2007
// `lower_bound` is sized num_samples * num_centers and pre-allocated by caller.
void InitCenters(const KMeansState &state,
                 PQFloatArray samples,
                 PQFloatArray &centers,
                 float *lower_bound,
                 const PQContext &ctx) {
    auto procinfo = state.distance_fn;
    size_t num_centers = centers.maxlen;
    size_t num_samples = samples.length;
    size_t dim         = centers.dim;

    // First center: uniform random pick.
    centers.Set(0, samples.Get(ctx.random.RandomInt() % num_samples));
    centers.length = 1;

    auto *weight = static_cast<float *>(ctx.allocator.Alloc(num_samples * sizeof(float)));
    for (size_t j = 0; j < num_samples; j++) {
        weight[j] = FLT_MAX;
    }

    for (size_t i = 0; i < num_centers; i++) {
        double sum = 0.0;

        ctx.parallel.Run(num_samples, [&](size_t j) {
            double distance = procinfo(samples.Get(j), centers.Get(i), static_cast<uint16_t>(dim));
            lower_bound[j * num_centers + i] = static_cast<float>(distance);
            distance *= distance;
            if (distance < weight[j]) {
                weight[j] = static_cast<float>(distance);
            }
        });

        for (size_t j = 0; j < num_samples; j++) {
            sum += weight[j];
        }

        // Last iteration only computed lower bounds; no new center to pick.
        if (i + 1 == num_centers) {
            break;
        }

        // Choose next center using weighted probability.
        double choice = sum * ctx.random.RandomDouble();
        size_t j;
        for (j = 0; j < num_samples - 1; j++) {
            choice -= weight[j];
            if (choice <= 0) break;
        }

        centers.Set(i + 1, samples.Get(j));
        centers.length++;
    }

    ctx.allocator.Free(weight);
}

// Fast path when samples.length <= centers.maxlen: copy unique samples and
// fill the rest with random vectors (normalized for cosine).
void QuickCenters(const KMeansState &state,
                  PQFloatArray samples,
                  PQFloatArray &centers,
                  const PQContext &ctx) {
    size_t dim = centers.dim;

    if (samples.length > 0) {
        // Sort + dedup samples by lex order.
        std::vector<size_t> order(samples.length);
        for (size_t i = 0; i < samples.length; i++) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return CompareVectors(samples.Get(a), samples.Get(b), dim) < 0;
        });
        for (size_t i = 0; i < order.size(); i++) {
            const float *vec = samples.Get(order[i]);
            if (i == 0 || CompareVectors(vec, samples.Get(order[i - 1]), dim) != 0) {
                centers.Set(centers.length, vec);
                centers.length++;
            }
        }
    }

    // Fill remaining slots with random data.
    while (centers.length < centers.maxlen) {
        float *vec = centers.Get(centers.length);
        for (size_t j = 0; j < dim; j++) {
            vec[j] = static_cast<float>(ctx.random.RandomDouble());
        }
        if (state.norm_fn != nullptr) {
            ApplyNorm(state.norm_fn, vec, static_cast<uint16_t>(dim));
        }
        centers.length++;
    }
}

// Elkan's accelerated K-means. Uses triangle inequality to skip distance
// computations. See ICML 2003 paper.
void ElkanKmeans(const KMeansState &state,
                 PQFloatArray samples,
                 PQFloatArray &centers,
                 int avg_work_mem_kb,
                 const PQContext &ctx) {
    size_t dim         = centers.dim;
    size_t num_centers = centers.maxlen;
    size_t num_samples = samples.length;

    // Memory cap check (matches openGauss's logic).
    size_t total_size = (sizeof(int) * num_centers) +                       // center_counts
                        (sizeof(int) * num_samples) +                       // closest_centers
                        (sizeof(float) * num_samples * num_centers) +       // lower_bound
                        (sizeof(float) * num_samples) +                     // upper_bound
                        (sizeof(float) * num_centers) +                     // s
                        (sizeof(float) * num_centers * num_centers) +       // halfcdist
                        (sizeof(float) * num_centers) +                     // newcdist
                        (sizeof(float) * num_centers * dim);                // newCenters
    if (avg_work_mem_kb > 0 && total_size > static_cast<size_t>(avg_work_mem_kb) * 1024UL) {
        VEX_QUANT_ERROR("k-means: working set " + std::to_string(total_size / (1024 * 1024) + 1) +
                        " MB exceeds avg_work_mem " + std::to_string(avg_work_mem_kb / 1024) + " MB");
    }
    if (num_centers * num_centers > static_cast<size_t>(INT_MAX)) {
        VEX_QUANT_ERROR("k-means: indexing overflow (numCenters^2 > INT_MAX)");
    }

    auto *center_counts   = static_cast<int *>(ctx.allocator.AllocZero(num_centers * sizeof(int)));
    auto *closest_centers = static_cast<int *>(ctx.allocator.AllocZero(num_samples * sizeof(int)));
    auto *lower_bound     = static_cast<float *>(ctx.allocator.AllocZero(num_samples * num_centers * sizeof(float)));
    auto *upper_bound     = static_cast<float *>(ctx.allocator.AllocZero(num_samples * sizeof(float)));
    auto *s               = static_cast<float *>(ctx.allocator.AllocZero(num_centers * sizeof(float)));
    auto *halfcdist       = static_cast<float *>(ctx.allocator.AllocZero(num_centers * num_centers * sizeof(float)));
    auto *newcdist        = static_cast<float *>(ctx.allocator.AllocZero(num_centers * sizeof(float)));

    PQFloatArray new_centers = AllocFloatArray(ctx, num_centers, dim);
    new_centers.length = num_centers;

    auto procinfo    = state.distance_fn;
    auto normprocinfo = state.norm_fn;

    InitCenters(state, samples, centers, lower_bound, ctx);

    // Initial assignment: each sample → closest of the seeded centers, using
    // the lower_bound table populated by InitCenters.
    ctx.parallel.Run(num_samples, [&](size_t j) {
        double min_d = DBL_MAX;
        int    best  = 0;
        for (size_t k = 0; k < num_centers; k++) {
            double d = lower_bound[j * num_centers + k];
            if (d < min_d) {
                min_d = d;
                best  = static_cast<int>(k);
            }
        }
        upper_bound[j] = static_cast<float>(min_d);
        closest_centers[j] = best;
    });

    bool changes = false;
    for (int iteration = 0; iteration < 500; iteration++) {
        changes = false;

        // Step 1a: pairwise center-to-center distances (halved).
        ctx.parallel.Run(num_centers, [&](size_t j) {
            const float *vec = centers.Get(j);
            for (size_t k = j + 1; k < num_centers; k++) {
                double d = 0.5 * procinfo(vec, centers.Get(k), static_cast<uint16_t>(dim));
                halfcdist[j * num_centers + k] = static_cast<float>(d);
                halfcdist[k * num_centers + j] = static_cast<float>(d);
            }
        });

        // Step 1b: s(c) = min over k!=j of halfcdist[j][k].
        ctx.parallel.Run(num_centers, [&](size_t j) {
            double min_d = DBL_MAX;
            for (size_t k = 0; k < num_centers; k++) {
                if (j == k) continue;
                double d = halfcdist[j * num_centers + k];
                if (d < min_d) min_d = d;
            }
            s[j] = static_cast<float>(min_d);
        });

        // Step 2 + 3 combined: triangle-inequality pruning + reassignment.
        bool rjreset = (iteration != 0);
        ctx.parallel.Run(num_samples, [&](size_t j) {
            if (upper_bound[j] <= s[closest_centers[j]]) return;

            bool rj = rjreset;
            for (size_t k = 0; k < num_centers; k++) {
                if (static_cast<int>(k) == closest_centers[j]) continue;
                if (upper_bound[j] <= lower_bound[j * num_centers + k]) continue;
                if (upper_bound[j] <= halfcdist[closest_centers[j] * num_centers + k]) continue;

                const float *vec = samples.Get(j);

                double dxcx;
                if (rj) {
                    dxcx = procinfo(vec, centers.Get(closest_centers[j]), static_cast<uint16_t>(dim));
                    lower_bound[j * num_centers + closest_centers[j]] = static_cast<float>(dxcx);
                    upper_bound[j] = static_cast<float>(dxcx);
                    rj = false;
                } else {
                    dxcx = upper_bound[j];
                }

                if (dxcx > lower_bound[j * num_centers + k] ||
                    dxcx > halfcdist[closest_centers[j] * num_centers + k]) {
                    double dxc = procinfo(vec, centers.Get(k), static_cast<uint16_t>(dim));
                    lower_bound[j * num_centers + k] = static_cast<float>(dxc);
                    if (dxc < dxcx) {
                        closest_centers[j] = static_cast<int>(k);
                        upper_bound[j]     = static_cast<float>(dxc);
                        changes            = true;
                    }
                }
            }
        });

        // Step 4a: zero new_centers + counts.
        ctx.parallel.Run(num_centers, [&](size_t j) {
            float *vec = new_centers.Get(j);
            for (size_t k = 0; k < dim; k++) vec[k] = 0.0f;
            center_counts[j] = 0;
        });

        // Step 4b: accumulate (must be serial — multiple j may map to same center).
        for (size_t j = 0; j < num_samples; j++) {
            const float *vec = samples.Get(j);
            int closest = closest_centers[j];
            float *new_center = new_centers.Get(closest);
            for (size_t k = 0; k < dim; k++) new_center[k] += vec[k];
            center_counts[closest]++;
        }

        // Step 4c: divide accumulated sums by counts (or fill with random for empty).
        ctx.parallel.Run(num_centers, [&](size_t j) {
            float *vec = new_centers.Get(j);
            if (center_counts[j] > 0) {
                for (size_t k = 0; k < dim; k++) {
                    if (std::isinf(vec[k])) {
                        vec[k] = vec[k] > 0 ? FLT_MAX : -FLT_MAX;
                    }
                }
                for (size_t k = 0; k < dim; k++) {
                    vec[k] /= static_cast<float>(center_counts[j]);
                }
            } else {
                for (size_t k = 0; k < dim; k++) {
                    vec[k] = static_cast<float>(ctx.random.RandomDouble());
                }
            }
            if (normprocinfo != nullptr) {
                ApplyNorm(normprocinfo, vec, static_cast<uint16_t>(dim));
            }
        });

        // Step 5a: distance from old to new center per cluster.
        ctx.parallel.Run(num_centers, [&](size_t j) {
            newcdist[j] = procinfo(centers.Get(j), new_centers.Get(j), static_cast<uint16_t>(dim));
        });

        // Step 5b: tighten lower bounds (subtract per-center movement).
        ctx.parallel.Run(num_samples, [&](size_t j) {
            for (size_t k = 0; k < num_centers; k++) {
                double d = lower_bound[j * num_centers + k] - newcdist[k];
                if (d < 0) d = 0;
                lower_bound[j * num_centers + k] = static_cast<float>(d);
            }
        });

        // Step 6: loosen upper bounds (add own-cluster movement).
        ctx.parallel.Run(num_samples, [&](size_t j) {
            upper_bound[j] += newcdist[closest_centers[j]];
        });

        // Step 7: copy new centers in place.
        ctx.parallel.Run(num_centers, [&](size_t j) {
            std::memcpy(centers.Get(j), new_centers.Get(j), dim * sizeof(float));
        });

        if (!changes && iteration != 0) {
            break;
        }
    }

    FreeFloatArray(ctx, new_centers);
    ctx.allocator.Free(center_counts);
    ctx.allocator.Free(closest_centers);
    ctx.allocator.Free(lower_bound);
    ctx.allocator.Free(upper_bound);
    ctx.allocator.Free(s);
    ctx.allocator.Free(halfcdist);
    ctx.allocator.Free(newcdist);
}

void CheckCenters(const KMeansState &state, PQFloatArray centers) {
    size_t dim = centers.dim;
    if (centers.length != centers.maxlen) {
        VEX_QUANT_ERROR("k-means: not enough centers (please report a bug)");
    }
    for (size_t i = 0; i < centers.length; i++) {
        const float *vec = centers.Get(i);
        for (size_t j = 0; j < dim; j++) {
            if (std::isnan(vec[j])) {
                VEX_QUANT_ERROR("k-means: NaN detected in centers");
            }
            if (std::isinf(vec[j])) {
                VEX_QUANT_ERROR("k-means: Inf detected in centers");
            }
        }
    }

    // Duplicate detection (skipped for sparse data per kmeanstate flag).
    std::vector<size_t> order(centers.length);
    for (size_t i = 0; i < centers.length; i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return CompareVectors(centers.Get(a), centers.Get(b), dim) < 0;
    });
    for (size_t i = 1; i < order.size(); i++) {
        if (CompareVectors(centers.Get(order[i]), centers.Get(order[i - 1]), dim) == 0 &&
            !state.skip_check_duplicate) {
            VEX_QUANT_ERROR("k-means: duplicate centers detected");
        }
    }
}

} // namespace

void AnnKmeans(const KMeansState &state,
               PQFloatArray samples,
               PQFloatArray &centers,
               int avg_work_mem_kb,
               const PQContext &ctx) {
    if (samples.length <= centers.maxlen) {
        QuickCenters(state, samples, centers, ctx);
    } else {
        ElkanKmeans(state, samples, centers, avg_work_mem_kb, ctx);
    }
    CheckCenters(state, centers);
}

} // namespace quantizer
} // namespace vex
