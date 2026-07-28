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
#include <atomic>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace vex {
namespace quantizer {

namespace {

template <typename T>
class ScopedPQAllocation {
public:
    ScopedPQAllocation(const PQAllocator &allocator, size_t count, bool zero)
        : allocator_(allocator) {
        if (count > std::numeric_limits<size_t>::max() / sizeof(T)) {
            VEX_QUANT_ERROR("k-means: allocation size overflow");
        }
        const size_t bytes = count * sizeof(T);
        ptr_ = static_cast<T *>(zero ? allocator_.AllocZero(bytes)
                                     : allocator_.Alloc(bytes));
        if (!ptr_ && bytes != 0) {
            VEX_QUANT_ERRORF("k-means: failed to allocate %zu bytes", bytes);
        }
    }

    ScopedPQAllocation(const ScopedPQAllocation &) = delete;
    ScopedPQAllocation &operator=(const ScopedPQAllocation &) = delete;
    ~ScopedPQAllocation() { allocator_.Free(ptr_); }

    T *get() const { return ptr_; }

private:
    const PQAllocator &allocator_;
    T *ptr_ = nullptr;
};

size_t SaturatingAdd(size_t a, size_t b) {
    if (a > std::numeric_limits<size_t>::max() - b) {
        return std::numeric_limits<size_t>::max();
    }
    return a + b;
}

size_t SaturatingMul(size_t a, size_t b) {
    if (a != 0 && b > std::numeric_limits<size_t>::max() / a) {
        return std::numeric_limits<size_t>::max();
    }
    return a * b;
}

PQFloatArray AllocFloatArray(const PQContext &ctx, size_t maxlen, size_t dim) {
    PQFloatArray arr;
    arr.maxlen = maxlen;
    arr.length = 0;
    arr.dim    = dim;
    const size_t count = SaturatingMul(maxlen, dim);
    if (count == std::numeric_limits<size_t>::max() ||
        count > std::numeric_limits<size_t>::max() / sizeof(float)) {
        VEX_QUANT_ERROR("k-means: float array size overflow");
    }
    const size_t bytes = count * sizeof(float);
    arr.data = static_cast<float *>(ctx.allocator.AllocZero(bytes));
    if (!arr.data && bytes != 0) {
        VEX_QUANT_ERRORF("k-means: failed to allocate %zu bytes", bytes);
    }
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

    ScopedPQAllocation<float> weight_owner(ctx.allocator, num_samples, false);
    auto *weight = weight_owner.get();
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

    // Keep the internal guard aligned with the public estimator used by host
    // adapters. It includes InitCenters' weight buffer, which overlaps the
    // other Elkan allocations and was previously missing from the check.
    size_t total_size = EstimateKMeansScratchBytes(num_samples, num_centers, dim);
    if (avg_work_mem_kb > 0 && total_size > static_cast<size_t>(avg_work_mem_kb) * 1024UL) {
        VEX_QUANT_ERRORF("k-means: working set %zu MB exceeds avg_work_mem %d MB",
                         total_size / (1024 * 1024) + 1, avg_work_mem_kb / 1024);
    }
    if (num_centers * num_centers > static_cast<size_t>(INT_MAX)) {
        VEX_QUANT_ERROR("k-means: indexing overflow (numCenters^2 > INT_MAX)");
    }

    ScopedPQAllocation<int> center_counts_owner(ctx.allocator, num_centers, true);
    ScopedPQAllocation<int> closest_centers_owner(ctx.allocator, num_samples, true);
    ScopedPQAllocation<float> lower_bound_owner(
        ctx.allocator, SaturatingMul(num_samples, num_centers), true);
    ScopedPQAllocation<float> upper_bound_owner(ctx.allocator, num_samples, true);
    ScopedPQAllocation<float> s_owner(ctx.allocator, num_centers, true);
    ScopedPQAllocation<float> halfcdist_owner(
        ctx.allocator, SaturatingMul(num_centers, num_centers), true);
    ScopedPQAllocation<float> newcdist_owner(ctx.allocator, num_centers, true);

    auto *center_counts = center_counts_owner.get();
    auto *closest_centers = closest_centers_owner.get();
    auto *lower_bound = lower_bound_owner.get();
    auto *upper_bound = upper_bound_owner.get();
    auto *s = s_owner.get();
    auto *halfcdist = halfcdist_owner.get();
    auto *newcdist = newcdist_owner.get();

    PQFloatArray new_centers = AllocFloatArray(ctx, num_centers, dim);
    // AllocFloatArray owns its allocation; use a tiny local guard without
    // reallocating so exceptions from distance/parallel callbacks clean it.
    struct FloatArrayGuard {
        const PQContext &ctx;
        PQFloatArray &array;
        ~FloatArrayGuard() { FreeFloatArray(ctx, array); }
    } new_centers_guard{ctx, new_centers};
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

    std::atomic<bool> changes{false};
    for (int iteration = 0; iteration < 500; iteration++) {
        changes.store(false, std::memory_order_relaxed);

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
                        changes.store(true, std::memory_order_relaxed);
                    }
                }
            }
        });

        // Step 4a: zero new_centers + counts.
        // Empty-center replacement consumes RNG state. Keep this loop serial
        // so a parallel executor cannot make the codebook depend on task order.
        for (size_t j = 0; j < num_centers; j++) {
            float *vec = new_centers.Get(j);
            for (size_t k = 0; k < dim; k++) vec[k] = 0.0f;
            center_counts[j] = 0;
        }

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

        if (!changes.load(std::memory_order_relaxed) && iteration != 0) {
            break;
        }
    }
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

size_t EstimateKMeansScratchBytes(size_t num_samples,
                                  size_t num_centers,
                                  size_t dim) {
    if (num_samples == 0 || num_centers == 0 || dim == 0) {
        return 0;
    }

    // QuickCenters only owns a sample-order vector; CheckCenters later owns a
    // center-order vector. They do not overlap.
    if (num_samples <= num_centers) {
        return SaturatingMul(std::max(num_samples, num_centers), sizeof(size_t));
    }

    size_t bytes = 0;
    auto add = [&](size_t count, size_t elem_size) {
        bytes = SaturatingAdd(bytes, SaturatingMul(count, elem_size));
    };
    add(num_centers, sizeof(int));                         // center_counts
    add(num_samples, sizeof(int));                         // closest_centers
    add(SaturatingMul(num_samples, num_centers), sizeof(float)); // lower_bound
    add(num_samples, sizeof(float));                       // upper_bound
    add(num_centers, sizeof(float));                       // s
    add(SaturatingMul(num_centers, num_centers), sizeof(float)); // halfcdist
    add(num_centers, sizeof(float));                       // newcdist
    add(SaturatingMul(num_centers, dim), sizeof(float));   // new_centers
    add(num_samples, sizeof(float));                       // InitCenters weight
    return bytes;
}

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
