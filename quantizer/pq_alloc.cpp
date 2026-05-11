// Default implementations for PQRandom. Backends that want deterministic
// reproducibility (matching openGauss's pinned seed=42 behavior) get it for
// free without supplying their own callback.
#include "quantizer/pq_alloc.h"

#include <random>

namespace vex {
namespace quantizer {

namespace {
// Process-wide RNG. K-means / PQ training is single-threaded and serialized at
// a higher level (one CREATE INDEX at a time per backend), so a non-locking
// thread_local engine is sufficient. Seed is pinned (42) so two runs over the
// same data produce identical codebooks — useful for regression tests and for
// caching codebooks across CHECKPOINT/restart.
thread_local std::mt19937 g_default_rng{42};
} // namespace

uint32_t PQRandom::RandomInt() const {
    if (int_fn) {
        return int_fn(user);
    }
    return static_cast<uint32_t>(g_default_rng());
}

double PQRandom::RandomDouble() const {
    if (double_fn) {
        return double_fn(user);
    }
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(g_default_rng);
}

} // namespace quantizer
} // namespace vex
