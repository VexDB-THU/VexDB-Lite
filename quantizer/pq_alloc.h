// Backend-neutral helpers for the shared Product Quantizer / K-means port.
//
// openGauss's reference implementation uses palloc / pfree, ereport, and
// FloatVectorArray throughout. We can't pull those into the duck adapter (no
// MemoryContext, no Postgres exception system) but we still want to share the
// algorithm. This header defines the small surface each backend implements:
//
//   - PQAllocator:        allocator callback, replaces palloc / pfree
//   - VexQuantizerError:  exception type, replaces ereport(ERROR, ...)
//   - PQFloatArray:       POD layout used by Train/Encode, replaces FloatVectorArray
//   - PQParallelExecutor: optional task fan-out, replaces TaskRunner.
//                         Single-threaded fallback is provided.
//
// Backends register a PQAllocator at quantizer-construction time. The shared
// PQ / K-means code never calls palloc directly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>

namespace vex {
namespace quantizer {

// Allocator callback. Default implementation uses std::malloc / std::free; a
// PG-side wrapper passes palloc / pfree (with the appropriate MemoryContext
// captured by the caller).
struct PQAllocator {
    using AllocFn = void *(*)(size_t bytes, void *user);
    using FreeFn  = void (*)(void *p, void *user);

    AllocFn alloc_fn      = nullptr;
    // Optional huge-allocation hook. Used for large codebooks (e.g. PQ
    // centroids at d=1024+, M=64+, ksub=256 can exceed 1GB). On PG this maps
    // to palloc_extended(MCXT_ALLOC_HUGE); on duck std::malloc already
    // handles >1GB so leaving this null falls back to alloc_fn.
    AllocFn alloc_huge_fn = nullptr;
    FreeFn  free_fn       = nullptr;
    void   *user          = nullptr;

    void *Alloc(size_t bytes) const {
        return alloc_fn ? alloc_fn(bytes, user) : std::malloc(bytes);
    }
    void *AllocHuge(size_t bytes) const {
        if (alloc_huge_fn) return alloc_huge_fn(bytes, user);
        return Alloc(bytes);
    }
    void *AllocZero(size_t bytes) const {
        void *p = Alloc(bytes);
        if (p) std::memset(p, 0, bytes);
        return p;
    }
    void *AllocHugeZero(size_t bytes) const {
        void *p = AllocHuge(bytes);
        if (p) std::memset(p, 0, bytes);
        return p;
    }
    void Free(void *p) const {
        if (!p) return;
        if (free_fn) free_fn(p, user);
        else std::free(p);
    }
};

// All quantizer-side fatal errors raise this. The duck adapter lets it
// propagate as a regular C++ exception (DuckDB extension catches and converts);
// the PG adapter catches it at the boundary and calls ereport(ERROR, ...).
class VexQuantizerError : public std::runtime_error {
public:
    explicit VexQuantizerError(const std::string &what) : std::runtime_error(what) {}
};

#define VEX_QUANT_ERROR(msg) throw ::vex::quantizer::VexQuantizerError(msg)

// Drop-in for openGauss FloatVectorArray. A PQFloatArray is a non-owning view:
// `data` is a flat float buffer of length `maxlen * dim`. `length` is the
// current populated count (mutable, used by K-means as it appends centers).
// Allocation is the caller's responsibility (use PQAllocator).
struct PQFloatArray {
    float *data   = nullptr;
    size_t length = 0;
    size_t maxlen = 0;
    size_t dim    = 0;

    float *Get(size_t i)             { return data + i * dim; }
    const float *Get(size_t i) const { return data + i * dim; }

    void Set(size_t i, const float *src) {
        std::memcpy(data + i * dim, src, dim * sizeof(float));
    }
};

// Random number callbacks. K-means and PQ use these for centroid init.
// Default implementation seeds with std::mt19937(42) — deterministic, matches
// openGauss's pinned-seed behavior for reproducible builds.
struct PQRandom {
    using IntFn    = uint32_t (*)(void *user);
    using DoubleFn = double   (*)(void *user);

    IntFn    int_fn    = nullptr;
    DoubleFn double_fn = nullptr;
    void    *user      = nullptr;

    uint32_t RandomInt() const;
    double   RandomDouble() const;
};

// Parallel task executor. Backends supply an implementation that can fan out
// `body(i)` for i in [0, n). Default is a serial loop — adequate for duck and
// for first-cut PG integration; PG can later swap in a worker-pool driver.
struct PQParallelExecutor {
    using TaskFn = std::function<void(size_t /*idx*/)>;
    using RunFn  = void (*)(size_t n, const TaskFn &body, void *user);

    RunFn run_fn = nullptr;
    void *user   = nullptr;

    void Run(size_t n, const TaskFn &body) const {
        if (run_fn) {
            run_fn(n, body, user);
            return;
        }
        for (size_t i = 0; i < n; i++) body(i);
    }
};

// Optional progress reporter. openGauss train() reports per-subquantizer
// progress via Timer::inc_loop_count_forground_report; we expose a
// std::function so backends can forward (PG -> ereport(NOTICE), duck -> noop).
struct PQProgress {
    std::function<void(size_t done, size_t total, const char *stage)> fn = nullptr;
    void Report(size_t done, size_t total, const char *stage) const {
        if (fn) fn(done, total, stage);
    }
};

// Bundled context passed through the algorithm so each callsite knows where to
// allocate, how to throw, how to randomize, and (optionally) how to parallelize.
struct PQContext {
    PQAllocator        allocator;
    PQRandom           random;
    PQParallelExecutor parallel;
    PQProgress         progress;
};

} // namespace quantizer
} // namespace vex
