#include "pq_training_sample.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

int main() {
    using vexdb_pq::TrainingSampleCapacity;
    using vexdb_pq::TrainingSampleSlot;
    constexpr std::size_t MiB = 1024 * 1024;
    assert(TrainingSampleCapacity(0, 16 * MiB, MiB, 50000) == 0);
    assert(TrainingSampleCapacity(64, 16 * MiB, MiB, 50000) == 4096);
    assert(TrainingSampleCapacity(1536, 2048 * MiB, 1024 * MiB, 50000) == 50000);
    assert(TrainingSampleCapacity(std::numeric_limits<std::size_t>::max(),
                                  16 * MiB, MiB, 50000) == 0);

    bool called = false;
    auto last_slot = [&](std::uint64_t upper) { called = true; return upper; };
    assert(TrainingSampleSlot(0, 0, last_slot) == 0 && !called);
    assert(TrainingSampleSlot(3, 8, last_slot) == 3 && !called);
    assert(TrainingSampleSlot(8, 8, last_slot) == 8 && called);
    const auto large = (std::uint64_t{1} << 40) + 17;
    assert(TrainingSampleSlot(large, 8, [&](std::uint64_t upper) {
        assert(upper == large);
        return std::uint64_t{0};
    }) == 0);
    assert(TrainingSampleSlot(large, 8, last_slot) == 8);

    // Ordered input must not reduce training to the first block. Broad bounds
    // detect prefix-only and off-by-one sampling without fixing exact RNG IDs.
    for (unsigned seed = 0; seed < 20; ++seed) {
        std::mt19937_64 random(seed);
        constexpr std::size_t capacity = 128;
        std::vector<std::uint64_t> sample(capacity);
        for (std::uint64_t i = 0; i < 10000; ++i) {
            const auto pos = TrainingSampleSlot(i, capacity, [&](std::uint64_t upper) {
                return std::uniform_int_distribution<std::uint64_t>(0, upper)(random);
            });
            if (pos < capacity) sample[pos] = i;
        }
        std::uint64_t sum = 0;
        unsigned blocks = 0;
        for (auto i : sample) {
            assert(i < 10000);
            sum += i;
            blocks |= 1u << (i / 1000);
        }
        const double mean = double(sum) / capacity;
        assert(3500 < mean && mean < 6500);
        assert(blocks == (1u << 10) - 1);
    }
    std::cout << "PQ training sample tests passed\n";
}
