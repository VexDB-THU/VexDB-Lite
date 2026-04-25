#ifndef VEX_DISTANCE_HPP
#define VEX_DISTANCE_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace vex {

using distance_func_t = float (*)(const float *a, const float *b, uint32_t dim);

enum class Metric : uint8_t {
    L2 = 0,
    COSINE = 1,
    INNER_PRODUCT = 2,
};

float L2SqrDistance(const float *a, const float *b, uint32_t dim);
float InnerProductDistance(const float *a, const float *b, uint32_t dim);
float CosineDistance(const float *a, const float *b, uint32_t dim);

distance_func_t GetL2SqrFunc();
distance_func_t GetInnerProductFunc();
distance_func_t GetDistanceFunc(Metric metric);

Metric ParseMetric(const std::string &metric_str);
const char *MetricName(Metric metric);

void NormalizeVector(float *vec, uint32_t dim);

enum class SimdArch : uint8_t {
    GENERIC = 0,
    SSE = 1,
    AVX2 = 2,
    AVX512 = 3,
    NEON = 4,
    WASM_SIMD = 5,
};

SimdArch GetBestArch();
const char *ArchName(SimdArch arch);

} // namespace vex

#endif // VEX_DISTANCE_HPP
