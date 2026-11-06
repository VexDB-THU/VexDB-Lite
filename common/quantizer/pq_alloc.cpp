// Default implementations for PQRandom. Backends that want deterministic
// reproducibility (matching openGauss's pinned seed=42 behavior) get it for
// free without supplying their own callback.
#include "quantizer/pq_alloc.h"

namespace vex {
namespace quantizer {

static uint64_t NextSplitMix64(uint64_t &state) {
    uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

uint32_t PQRandom::RandomInt() const {
    if (int_fn) {
        return int_fn(user);
    }
    return static_cast<uint32_t>(NextSplitMix64(fallback_state) >> 32);
}

double PQRandom::RandomDouble() const {
    if (double_fn) {
        return double_fn(user);
    }
    // Use the top 53 bits so results are identical across standard-library
    // implementations (unlike uniform_real_distribution).
    return static_cast<double>(NextSplitMix64(fallback_state) >> 11) *
           (1.0 / 9007199254740992.0);
}

} // namespace quantizer
} // namespace vex
