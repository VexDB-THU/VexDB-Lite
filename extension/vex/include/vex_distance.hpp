#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace duckdb {
namespace vex {

// Function pointer types for distance computation
using distance_func_t = float (*)(const float *a, const float *b, uint32_t dim);

// Distance metric types
enum class VexMetric : uint8_t {
	L2 = 0,
	COSINE = 1,
	INNER_PRODUCT = 2,
};

// Distance computation functions (runtime-dispatched to best SIMD path)
float L2SqrDistance(const float *a, const float *b, uint32_t dim);
float InnerProductDistance(const float *a, const float *b, uint32_t dim);
float CosineDistance(const float *a, const float *b, uint32_t dim);

// Get function pointers (for hot loops where function call overhead matters)
distance_func_t GetL2SqrFunc();
distance_func_t GetInnerProductFunc();

// Get distance function for a given metric
// - L2: squared L2 distance
// - COSINE: 1 - dot(a,b) for pre-normalized vectors
// - INNER_PRODUCT: -dot(a,b) so smaller = more similar
distance_func_t GetDistanceFunc(VexMetric metric);

// Parse metric string from WITH options ("l2", "cosine", "ip", etc.)
VexMetric ParseMetric(const std::string &metric_str);

// Metric name for display/serialization
const char *MetricName(VexMetric metric);

// Normalize a vector to unit length (in-place)
void NormalizeVector(float *vec, uint32_t dim);

// CPU feature detection
enum class SimdArch : uint8_t {
	GENERIC = 0,
	SSE     = 1,
	AVX2    = 2,
	AVX512  = 3,
};

SimdArch GetBestArch();
const char *ArchName(SimdArch arch);

} // namespace vex
} // namespace duckdb
