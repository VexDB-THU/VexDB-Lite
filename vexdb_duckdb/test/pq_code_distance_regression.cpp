#include "vex_graph_index_depend_duck.hpp"
#include "distance/core/distance.h"

#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

// Exercise the actual dispatched kernels with independently packed codes and
// long-double table sums. The binary scores permit old/new bitwise comparison.
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    std::ofstream dump(argv[1], std::ios::binary);
    if (!dump) return 2;
    std::mt19937 random(392701);
    size_t checked = 0;
    for (const uint32_t bits : {1u, 2u, 4u, 7u, 8u, 10u, 12u, 16u}) {
        const uint32_t ksub = uint32_t(1) << bits;
        const auto one = ann_helper::get_distance_single_code_func(bits);
        const auto four = ann_helper::get_distance_four_codes_func(bits);
        for (const uint32_t m : {0u, 1u, 3u, 4u, 7u, 8u, 15u, 16u, 17u, 31u, 32u, 128u, 384u, 385u}) {
            if (bits == 16 && m > 32) continue;
            // Offset the LUT by one float: SIMD cannot assume wider alignment.
            std::vector<float> storage(size_t(m) * ksub + 1);
            auto *table = storage.data() + 1;
            for (size_t j = 0; j < size_t(m) * ksub; ++j) {
                table[j] = (int(random() % 200001) - 100000) / 8192.0f;
            }
            for (int repeat = 0; repeat < 64; ++repeat) {
                std::array<std::vector<uint8_t>, 4> codes;
                std::array<long double, 4> expected {}, magnitude {};
                for (int row = 0; row < 4; ++row) {
                    codes[row].resize(std::max<size_t>(1, (size_t(m) * bits + 7) / 8), 0);
                    for (uint32_t part = 0; part < m; ++part) {
                        const uint32_t index = random() % ksub;
                        for (uint32_t b = 0; b < bits; ++b) {
                            const size_t offset = size_t(part) * bits + b;
                            if (index & (uint32_t(1) << b)) codes[row][offset / 8] |= uint8_t(1u << (offset % 8));
                        }
                        expected[row] += table[size_t(part) * ksub + index];
                        magnitude[row] += std::abs(static_cast<long double>(table[size_t(part) * ksub + index]));
                    }
                }
                std::array<float, 4> scores {};
                four(m, bits, table, codes[0].data(), codes[1].data(), codes[2].data(), codes[3].data(),
                     scores[0], scores[1], scores[2], scores[3]);
                for (int row = 0; row < 4; ++row) {
                    const auto single = one(m, bits, table, codes[row].data());
                    const long double tolerance = (2 * m + 2) * std::numeric_limits<float>::epsilon() * magnitude[row];
                    if (!std::isfinite(scores[row]) || !std::isfinite(single) ||
                        std::abs(scores[row] - expected[row]) > tolerance ||
                        std::abs(single - expected[row]) > tolerance) {
                        throw std::runtime_error("PQ code distance disagrees with independent table sum");
                    }
                    dump.write(reinterpret_cast<const char *>(&scores[row]), sizeof(float));
                    dump.write(reinterpret_cast<const char *>(&single), sizeof(float));
                    checked += 2;
                }
            }
        }
    }
    std::cout << "PASS " << checked << " dispatched single/four PQ distances, varied bit widths and remainders\n";
}
