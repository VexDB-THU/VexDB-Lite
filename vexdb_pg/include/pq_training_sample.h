#ifndef VEXDB_PQ_TRAINING_SAMPLE_H
#define VEXDB_PQ_TRAINING_SAMPLE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace vexdb_pq {

// Sampling happens before the build scan has counted tuples. Bound its memory
// independently of row-count statistics, which may be absent or stale.
inline std::size_t TrainingSampleCapacity(std::size_t dimensions,
                                         std::size_t maintenance_bytes,
                                         std::size_t allocation_limit,
                                         std::size_t maximum_samples) {
    if (dimensions == 0) return 0;
    const auto budget = std::min(maintenance_bytes / 4, allocation_limit);
    // Divide in this order so dimensions*sizeof(float) cannot overflow.
    return std::min(maximum_samples, budget / sizeof(float) / dimensions);
}

// Algorithm R: every eligible input row, including rows after the initial
// reservoir, has equal probability of being retained. `seen` is zero based;
// draw(upper) must be uniform on the inclusive range [0, upper].
template <typename Draw>
inline std::size_t TrainingSampleSlot(std::uint64_t seen, std::size_t capacity,
                                     Draw &&draw) {
    if (capacity == 0) return 0;
    if (seen < capacity) return static_cast<std::size_t>(seen);
    const auto slot = draw(seen);
    return slot < capacity ? static_cast<std::size_t>(slot) : capacity;
}

} // namespace vexdb_pq

#endif
