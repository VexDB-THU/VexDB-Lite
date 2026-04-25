#ifndef VEX_QUANTIZER_HPP
#define VEX_QUANTIZER_HPP

#include "vex/vex_distance.hpp"

#include <cstdint>
#include <vector>

namespace vex {

struct ProductQuantizer {
    uint32_t d = 0;
    uint32_t m = 0;
    uint32_t dsub = 0;
    bool trained = false;

    static constexpr uint32_t KSUB = 256;
    static constexpr uint32_t MAX_KMEANS_ITERS = 25;
    static constexpr uint32_t MIN_TRAINING_POINTS = 256;

    std::vector<float> centroids;

    void Init(uint32_t dim, uint32_t num_sub);
    static uint32_t AutoSelectM(uint32_t dim);
    void Train(const float *vectors, uint32_t n);

    void Encode(const float *x, uint8_t *code) const;
    void Decode(const uint8_t *code, float *x) const;

    void ComputeDistanceTable(const float *query, float *dist_table) const;
    static float DistanceFromTable(const uint8_t *code, const float *dist_table, uint32_t m);

    uint32_t CodeSize() const { return m; }

    void SerializeTo(std::vector<char> &out) const;
    bool DeserializeFrom(const char *&ptr, const char *end);
};

} // namespace vex

#endif // VEX_QUANTIZER_HPP
