#pragma once

#include <cstdint>
#include <cstddef>

namespace duckdb {
namespace vex {

// Function pointer types for distance computation
using distance_func_t = float (*)(const float *a, const float *b, uint32_t dim);

// Distance computation functions (runtime-dispatched to best SIMD path)
float L2SqrDistance(const float *a, const float *b, uint32_t dim);
float InnerProductDistance(const float *a, const float *b, uint32_t dim);
float CosineDistance(const float *a, const float *b, uint32_t dim);

// Get function pointers (for hot loops where function call overhead matters)
distance_func_t GetL2SqrFunc();
distance_func_t GetInnerProductFunc();

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
