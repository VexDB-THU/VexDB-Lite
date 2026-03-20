#include "vex_distance.hpp"

#include <cmath>
#include <cstring>
#include <stdexcept>

// MSVC does not support __attribute__((target(...))); on MSVC x86-64
// SSE2 is always available and AVX2 is enabled via /arch:AVX2 compiler flag.
#ifndef _MSC_VER
#define VEX_TARGET_SSE  __attribute__((target("sse4.1")))
#define VEX_TARGET_AVX2 __attribute__((target("avx2,fma")))
#else
#define VEX_TARGET_SSE
#define VEX_TARGET_AVX2
#endif

// Platform detection for SIMD intrinsics
#if defined(__EMSCRIPTEN__)
// WebAssembly: use WASM SIMD128 when available, otherwise Generic
#if defined(__wasm_simd128__)
#define VEX_WASM_SIMD
#include <wasm_simd128.h>
#endif
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define VEX_X86
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#define VEX_ARM
#include <arm_neon.h>
#endif

namespace duckdb {
namespace vex {

// ============================================================
// CPU feature detection
// ============================================================

#ifdef VEX_X86
static void cpuid(int info[4], int level) {
#ifdef _MSC_VER
	__cpuid(info, level);
#else
	__cpuid_count(level, 0, info[0], info[1], info[2], info[3]);
#endif
}

static bool HasSSE() {
	int info[4];
	cpuid(info, 1);
	return (info[3] & (1 << 25)) != 0; // SSE
}

static bool HasAVX2() {
	int info[4];
	// Check OSXSAVE and AVX support
	cpuid(info, 1);
	bool has_osxsave = (info[2] & (1 << 27)) != 0;
	bool has_avx = (info[2] & (1 << 28)) != 0;
	if (!has_osxsave || !has_avx) {
		return false;
	}
	// Check AVX2
	cpuid(info, 7);
	return (info[1] & (1 << 5)) != 0;
}

static bool HasAVX512F() {
	int info[4];
	cpuid(info, 1);
	bool has_osxsave = (info[2] & (1 << 27)) != 0;
	if (!has_osxsave) {
		return false;
	}
	cpuid(info, 7);
	return (info[1] & (1 << 16)) != 0; // AVX-512F
}
#endif

SimdArch GetBestArch() {
#ifdef VEX_WASM_SIMD
	return SimdArch::WASM_SIMD;
#elif defined(VEX_X86)
	if (HasAVX512F()) {
		return SimdArch::AVX512;
	}
	if (HasAVX2()) {
		return SimdArch::AVX2;
	}
	if (HasSSE()) {
		return SimdArch::SSE;
	}
#elif defined(VEX_ARM)
	return SimdArch::NEON;
#endif
	return SimdArch::GENERIC;
}

const char *ArchName(SimdArch arch) {
	switch (arch) {
	case SimdArch::AVX512:    return "AVX-512";
	case SimdArch::AVX2:      return "AVX2";
	case SimdArch::SSE:       return "SSE";
	case SimdArch::NEON:      return "NEON";
	case SimdArch::WASM_SIMD: return "WASM-SIMD";
	default:                  return "Generic";
	}
}

// Cached best architecture
static const SimdArch g_best_arch = GetBestArch();

// ============================================================
// Generic (scalar) implementations
// ============================================================

static float L2SqrGeneric(const float *a, const float *b, uint32_t dim) {
	float sum = 0.0f;
	for (uint32_t i = 0; i < dim; i++) {
		float diff = a[i] - b[i];
		sum += diff * diff;
	}
	return sum;
}

static float InnerProductGeneric(const float *a, const float *b, uint32_t dim) {
	float sum = 0.0f;
	for (uint32_t i = 0; i < dim; i++) {
		sum += a[i] * b[i];
	}
	return sum;
}

// ============================================================
// WASM SIMD128 implementations
// ============================================================

#ifdef VEX_WASM_SIMD

static float L2SqrWasmSimd(const float *a, const float *b, uint32_t dim) {
	v128_t sum0 = wasm_f32x4_splat(0.0f);
	v128_t sum1 = wasm_f32x4_splat(0.0f);
	v128_t sum2 = wasm_f32x4_splat(0.0f);
	v128_t sum3 = wasm_f32x4_splat(0.0f);

	uint32_t i = 0;
	for (; i + 16 <= dim; i += 16) {
		v128_t a0 = wasm_v128_load(a + i);
		v128_t b0 = wasm_v128_load(b + i);
		v128_t d0 = wasm_f32x4_sub(a0, b0);
		sum0 = wasm_f32x4_add(sum0, wasm_f32x4_mul(d0, d0));

		v128_t a1 = wasm_v128_load(a + i + 4);
		v128_t b1 = wasm_v128_load(b + i + 4);
		v128_t d1 = wasm_f32x4_sub(a1, b1);
		sum1 = wasm_f32x4_add(sum1, wasm_f32x4_mul(d1, d1));

		v128_t a2 = wasm_v128_load(a + i + 8);
		v128_t b2 = wasm_v128_load(b + i + 8);
		v128_t d2 = wasm_f32x4_sub(a2, b2);
		sum2 = wasm_f32x4_add(sum2, wasm_f32x4_mul(d2, d2));

		v128_t a3 = wasm_v128_load(a + i + 12);
		v128_t b3 = wasm_v128_load(b + i + 12);
		v128_t d3 = wasm_f32x4_sub(a3, b3);
		sum3 = wasm_f32x4_add(sum3, wasm_f32x4_mul(d3, d3));
	}
	for (; i + 4 <= dim; i += 4) {
		v128_t av = wasm_v128_load(a + i);
		v128_t bv = wasm_v128_load(b + i);
		v128_t d = wasm_f32x4_sub(av, bv);
		sum0 = wasm_f32x4_add(sum0, wasm_f32x4_mul(d, d));
	}

	sum0 = wasm_f32x4_add(sum0, sum1);
	sum2 = wasm_f32x4_add(sum2, sum3);
	sum0 = wasm_f32x4_add(sum0, sum2);

	// Horizontal sum: extract 4 lanes
	float result = wasm_f32x4_extract_lane(sum0, 0) + wasm_f32x4_extract_lane(sum0, 1) +
	               wasm_f32x4_extract_lane(sum0, 2) + wasm_f32x4_extract_lane(sum0, 3);

	for (; i < dim; i++) {
		float diff = a[i] - b[i];
		result += diff * diff;
	}
	return result;
}

static float InnerProductWasmSimd(const float *a, const float *b, uint32_t dim) {
	v128_t sum0 = wasm_f32x4_splat(0.0f);
	v128_t sum1 = wasm_f32x4_splat(0.0f);
	v128_t sum2 = wasm_f32x4_splat(0.0f);
	v128_t sum3 = wasm_f32x4_splat(0.0f);

	uint32_t i = 0;
	for (; i + 16 <= dim; i += 16) {
		sum0 = wasm_f32x4_add(sum0, wasm_f32x4_mul(wasm_v128_load(a + i),      wasm_v128_load(b + i)));
		sum1 = wasm_f32x4_add(sum1, wasm_f32x4_mul(wasm_v128_load(a + i + 4),  wasm_v128_load(b + i + 4)));
		sum2 = wasm_f32x4_add(sum2, wasm_f32x4_mul(wasm_v128_load(a + i + 8),  wasm_v128_load(b + i + 8)));
		sum3 = wasm_f32x4_add(sum3, wasm_f32x4_mul(wasm_v128_load(a + i + 12), wasm_v128_load(b + i + 12)));
	}
	for (; i + 4 <= dim; i += 4) {
		sum0 = wasm_f32x4_add(sum0, wasm_f32x4_mul(wasm_v128_load(a + i), wasm_v128_load(b + i)));
	}

	sum0 = wasm_f32x4_add(sum0, sum1);
	sum2 = wasm_f32x4_add(sum2, sum3);
	sum0 = wasm_f32x4_add(sum0, sum2);

	float result = wasm_f32x4_extract_lane(sum0, 0) + wasm_f32x4_extract_lane(sum0, 1) +
	               wasm_f32x4_extract_lane(sum0, 2) + wasm_f32x4_extract_lane(sum0, 3);

	for (; i < dim; i++) {
		result += a[i] * b[i];
	}
	return result;
}

#endif // VEX_WASM_SIMD

// ============================================================
// SSE implementations
// ============================================================

#ifdef VEX_X86

VEX_TARGET_SSE
static float L2SqrSSE(const float *a, const float *b, uint32_t dim) {
	__m128 sum0 = _mm_setzero_ps();
	__m128 sum1 = _mm_setzero_ps();
	__m128 sum2 = _mm_setzero_ps();
	__m128 sum3 = _mm_setzero_ps();

	uint32_t i = 0;
	// Process 16 floats per iteration (4 SSE registers x 4 floats)
	for (; i + 16 <= dim; i += 16) {
		__m128 a0 = _mm_loadu_ps(a + i);
		__m128 b0 = _mm_loadu_ps(b + i);
		__m128 d0 = _mm_sub_ps(a0, b0);
		sum0 = _mm_add_ps(sum0, _mm_mul_ps(d0, d0));

		__m128 a1 = _mm_loadu_ps(a + i + 4);
		__m128 b1 = _mm_loadu_ps(b + i + 4);
		__m128 d1 = _mm_sub_ps(a1, b1);
		sum1 = _mm_add_ps(sum1, _mm_mul_ps(d1, d1));

		__m128 a2 = _mm_loadu_ps(a + i + 8);
		__m128 b2 = _mm_loadu_ps(b + i + 8);
		__m128 d2 = _mm_sub_ps(a2, b2);
		sum2 = _mm_add_ps(sum2, _mm_mul_ps(d2, d2));

		__m128 a3 = _mm_loadu_ps(a + i + 12);
		__m128 b3 = _mm_loadu_ps(b + i + 12);
		__m128 d3 = _mm_sub_ps(a3, b3);
		sum3 = _mm_add_ps(sum3, _mm_mul_ps(d3, d3));
	}
	// Process 4 floats per iteration
	for (; i + 4 <= dim; i += 4) {
		__m128 av = _mm_loadu_ps(a + i);
		__m128 bv = _mm_loadu_ps(b + i);
		__m128 d = _mm_sub_ps(av, bv);
		sum0 = _mm_add_ps(sum0, _mm_mul_ps(d, d));
	}

	// Horizontal sum
	sum0 = _mm_add_ps(sum0, sum1);
	sum2 = _mm_add_ps(sum2, sum3);
	sum0 = _mm_add_ps(sum0, sum2);

	// SSE3 hadd or manual reduction
	__m128 shuf = _mm_movehdup_ps(sum0);
	__m128 sums = _mm_add_ps(sum0, shuf);
	shuf = _mm_movehl_ps(shuf, sums);
	sums = _mm_add_ss(sums, shuf);

	float result = _mm_cvtss_f32(sums);

	// Scalar tail
	for (; i < dim; i++) {
		float diff = a[i] - b[i];
		result += diff * diff;
	}
	return result;
}

VEX_TARGET_SSE
static float InnerProductSSE(const float *a, const float *b, uint32_t dim) {
	__m128 sum0 = _mm_setzero_ps();
	__m128 sum1 = _mm_setzero_ps();
	__m128 sum2 = _mm_setzero_ps();
	__m128 sum3 = _mm_setzero_ps();

	uint32_t i = 0;
	for (; i + 16 <= dim; i += 16) {
		sum0 = _mm_add_ps(sum0, _mm_mul_ps(_mm_loadu_ps(a + i),      _mm_loadu_ps(b + i)));
		sum1 = _mm_add_ps(sum1, _mm_mul_ps(_mm_loadu_ps(a + i + 4),  _mm_loadu_ps(b + i + 4)));
		sum2 = _mm_add_ps(sum2, _mm_mul_ps(_mm_loadu_ps(a + i + 8),  _mm_loadu_ps(b + i + 8)));
		sum3 = _mm_add_ps(sum3, _mm_mul_ps(_mm_loadu_ps(a + i + 12), _mm_loadu_ps(b + i + 12)));
	}
	for (; i + 4 <= dim; i += 4) {
		sum0 = _mm_add_ps(sum0, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
	}

	sum0 = _mm_add_ps(sum0, sum1);
	sum2 = _mm_add_ps(sum2, sum3);
	sum0 = _mm_add_ps(sum0, sum2);

	__m128 shuf = _mm_movehdup_ps(sum0);
	__m128 sums = _mm_add_ps(sum0, shuf);
	shuf = _mm_movehl_ps(shuf, sums);
	sums = _mm_add_ss(sums, shuf);

	float result = _mm_cvtss_f32(sums);
	for (; i < dim; i++) {
		result += a[i] * b[i];
	}
	return result;
}

// ============================================================
// AVX2 implementations
// ============================================================

VEX_TARGET_AVX2
static float L2SqrAVX2(const float *a, const float *b, uint32_t dim) {
	__m256 sum0 = _mm256_setzero_ps();
	__m256 sum1 = _mm256_setzero_ps();
	__m256 sum2 = _mm256_setzero_ps();
	__m256 sum3 = _mm256_setzero_ps();

	uint32_t i = 0;
	// Process 32 floats per iteration (4 AVX registers x 8 floats)
	for (; i + 32 <= dim; i += 32) {
		__m256 a0 = _mm256_loadu_ps(a + i);
		__m256 b0 = _mm256_loadu_ps(b + i);
		__m256 d0 = _mm256_sub_ps(a0, b0);
		sum0 = _mm256_fmadd_ps(d0, d0, sum0);

		__m256 a1 = _mm256_loadu_ps(a + i + 8);
		__m256 b1 = _mm256_loadu_ps(b + i + 8);
		__m256 d1 = _mm256_sub_ps(a1, b1);
		sum1 = _mm256_fmadd_ps(d1, d1, sum1);

		__m256 a2 = _mm256_loadu_ps(a + i + 16);
		__m256 b2 = _mm256_loadu_ps(b + i + 16);
		__m256 d2 = _mm256_sub_ps(a2, b2);
		sum2 = _mm256_fmadd_ps(d2, d2, sum2);

		__m256 a3 = _mm256_loadu_ps(a + i + 24);
		__m256 b3 = _mm256_loadu_ps(b + i + 24);
		__m256 d3 = _mm256_sub_ps(a3, b3);
		sum3 = _mm256_fmadd_ps(d3, d3, sum3);
	}
	for (; i + 8 <= dim; i += 8) {
		__m256 av = _mm256_loadu_ps(a + i);
		__m256 bv = _mm256_loadu_ps(b + i);
		__m256 d = _mm256_sub_ps(av, bv);
		sum0 = _mm256_fmadd_ps(d, d, sum0);
	}

	sum0 = _mm256_add_ps(sum0, sum1);
	sum2 = _mm256_add_ps(sum2, sum3);
	sum0 = _mm256_add_ps(sum0, sum2);

	// Horizontal sum: 8 -> 4 -> 2 -> 1
	__m128 hi = _mm256_extractf128_ps(sum0, 1);
	__m128 lo = _mm256_castps256_ps128(sum0);
	__m128 s = _mm_add_ps(lo, hi);
	__m128 shuf = _mm_movehdup_ps(s);
	__m128 sums = _mm_add_ps(s, shuf);
	shuf = _mm_movehl_ps(shuf, sums);
	sums = _mm_add_ss(sums, shuf);

	float result = _mm_cvtss_f32(sums);
	for (; i < dim; i++) {
		float diff = a[i] - b[i];
		result += diff * diff;
	}
	return result;
}

VEX_TARGET_AVX2
static float InnerProductAVX2(const float *a, const float *b, uint32_t dim) {
	__m256 sum0 = _mm256_setzero_ps();
	__m256 sum1 = _mm256_setzero_ps();
	__m256 sum2 = _mm256_setzero_ps();
	__m256 sum3 = _mm256_setzero_ps();

	uint32_t i = 0;
	for (; i + 32 <= dim; i += 32) {
		sum0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i),      _mm256_loadu_ps(b + i),      sum0);
		sum1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8),  _mm256_loadu_ps(b + i + 8),  sum1);
		sum2 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16), sum2);
		sum3 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24), sum3);
	}
	for (; i + 8 <= dim; i += 8) {
		sum0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), sum0);
	}

	sum0 = _mm256_add_ps(sum0, sum1);
	sum2 = _mm256_add_ps(sum2, sum3);
	sum0 = _mm256_add_ps(sum0, sum2);

	__m128 hi = _mm256_extractf128_ps(sum0, 1);
	__m128 lo = _mm256_castps256_ps128(sum0);
	__m128 s = _mm_add_ps(lo, hi);
	__m128 shuf = _mm_movehdup_ps(s);
	__m128 sums = _mm_add_ps(s, shuf);
	shuf = _mm_movehl_ps(shuf, sums);
	sums = _mm_add_ss(sums, shuf);

	float result = _mm_cvtss_f32(sums);
	for (; i < dim; i++) {
		result += a[i] * b[i];
	}
	return result;
}

#endif // VEX_X86

// ============================================================
// ARM NEON implementations
// ============================================================

#ifdef VEX_ARM

static float L2SqrNEON(const float *a, const float *b, uint32_t dim) {
	float32x4_t sum0 = vdupq_n_f32(0);
	float32x4_t sum1 = vdupq_n_f32(0);
	float32x4_t sum2 = vdupq_n_f32(0);
	float32x4_t sum3 = vdupq_n_f32(0);

	uint32_t i = 0;
	for (; i + 16 <= dim; i += 16) {
		float32x4_t a0 = vld1q_f32(a + i);
		float32x4_t b0 = vld1q_f32(b + i);
		float32x4_t d0 = vsubq_f32(a0, b0);
		sum0 = vmlaq_f32(sum0, d0, d0);

		float32x4_t a1 = vld1q_f32(a + i + 4);
		float32x4_t b1 = vld1q_f32(b + i + 4);
		float32x4_t d1 = vsubq_f32(a1, b1);
		sum1 = vmlaq_f32(sum1, d1, d1);

		float32x4_t a2 = vld1q_f32(a + i + 8);
		float32x4_t b2 = vld1q_f32(b + i + 8);
		float32x4_t d2 = vsubq_f32(a2, b2);
		sum2 = vmlaq_f32(sum2, d2, d2);

		float32x4_t a3 = vld1q_f32(a + i + 12);
		float32x4_t b3 = vld1q_f32(b + i + 12);
		float32x4_t d3 = vsubq_f32(a3, b3);
		sum3 = vmlaq_f32(sum3, d3, d3);
	}
	for (; i + 4 <= dim; i += 4) {
		float32x4_t av = vld1q_f32(a + i);
		float32x4_t bv = vld1q_f32(b + i);
		float32x4_t d = vsubq_f32(av, bv);
		sum0 = vmlaq_f32(sum0, d, d);
	}

	sum0 = vaddq_f32(sum0, sum1);
	sum2 = vaddq_f32(sum2, sum3);
	sum0 = vaddq_f32(sum0, sum2);

	float result = vaddvq_f32(sum0);
	for (; i < dim; i++) {
		float diff = a[i] - b[i];
		result += diff * diff;
	}
	return result;
}

static float InnerProductNEON(const float *a, const float *b, uint32_t dim) {
	float32x4_t sum0 = vdupq_n_f32(0);
	float32x4_t sum1 = vdupq_n_f32(0);
	float32x4_t sum2 = vdupq_n_f32(0);
	float32x4_t sum3 = vdupq_n_f32(0);

	uint32_t i = 0;
	for (; i + 16 <= dim; i += 16) {
		sum0 = vmlaq_f32(sum0, vld1q_f32(a + i),      vld1q_f32(b + i));
		sum1 = vmlaq_f32(sum1, vld1q_f32(a + i + 4),  vld1q_f32(b + i + 4));
		sum2 = vmlaq_f32(sum2, vld1q_f32(a + i + 8),  vld1q_f32(b + i + 8));
		sum3 = vmlaq_f32(sum3, vld1q_f32(a + i + 12), vld1q_f32(b + i + 12));
	}
	for (; i + 4 <= dim; i += 4) {
		sum0 = vmlaq_f32(sum0, vld1q_f32(a + i), vld1q_f32(b + i));
	}

	sum0 = vaddq_f32(sum0, sum1);
	sum2 = vaddq_f32(sum2, sum3);
	sum0 = vaddq_f32(sum0, sum2);

	float result = vaddvq_f32(sum0);
	for (; i < dim; i++) {
		result += a[i] * b[i];
	}
	return result;
}

#endif // VEX_ARM

// ============================================================
// Runtime dispatch
// ============================================================

static distance_func_t SelectL2SqrFunc() {
#ifdef VEX_WASM_SIMD
	return L2SqrWasmSimd;
#elif defined(VEX_X86)
	if (g_best_arch >= SimdArch::AVX2) {
		return L2SqrAVX2;
	}
	if (g_best_arch >= SimdArch::SSE) {
		return L2SqrSSE;
	}
#elif defined(VEX_ARM)
	return L2SqrNEON;
#endif
	return L2SqrGeneric;
}

static distance_func_t SelectInnerProductFunc() {
#ifdef VEX_WASM_SIMD
	return InnerProductWasmSimd;
#elif defined(VEX_X86)
	if (g_best_arch >= SimdArch::AVX2) {
		return InnerProductAVX2;
	}
	if (g_best_arch >= SimdArch::SSE) {
		return InnerProductSSE;
	}
#elif defined(VEX_ARM)
	return InnerProductNEON;
#endif
	return InnerProductGeneric;
}

// Cached function pointers (initialized once at startup)
static const distance_func_t g_l2sqr_func = SelectL2SqrFunc();
static const distance_func_t g_ip_func = SelectInnerProductFunc();

// ============================================================
// Public API
// ============================================================

float L2SqrDistance(const float *a, const float *b, uint32_t dim) {
	return g_l2sqr_func(a, b, dim);
}

float InnerProductDistance(const float *a, const float *b, uint32_t dim) {
	return g_ip_func(a, b, dim);
}

float CosineDistance(const float *a, const float *b, uint32_t dim) {
	// Cosine distance = 1 - cosine_similarity
	// cosine_similarity = dot(a,b) / (||a|| * ||b||)
	// We compute dot, norm_a_sq, norm_b_sq in one pass for efficiency
	// But the SIMD inner product helps with the dot product part
	float dot = g_ip_func(a, b, dim);
	float norm_a = g_ip_func(a, a, dim);
	float norm_b = g_ip_func(b, b, dim);
	float denom = std::sqrt(norm_a * norm_b);
	if (denom < 1e-30f) {
		return 1.0f;
	}
	float sim = dot / denom;
	if (sim > 1.0f) sim = 1.0f;
	if (sim < -1.0f) sim = -1.0f;
	return 1.0f - sim;
}

distance_func_t GetL2SqrFunc() {
	return g_l2sqr_func;
}

distance_func_t GetInnerProductFunc() {
	return g_ip_func;
}

// Cosine internal distance for pre-normalized vectors: 1 - dot(a,b)
static float CosineInternalDistance(const float *a, const float *b, uint32_t dim) {
	return 1.0f - g_ip_func(a, b, dim);
}

// Negative inner product for HNSW: -dot(a,b) so smaller = more similar
static float NegativeIPDistance(const float *a, const float *b, uint32_t dim) {
	return -g_ip_func(a, b, dim);
}

distance_func_t GetDistanceFunc(VexMetric metric) {
	switch (metric) {
	case VexMetric::COSINE:
		return CosineInternalDistance;
	case VexMetric::INNER_PRODUCT:
		return NegativeIPDistance;
	case VexMetric::L2:
	default:
		return g_l2sqr_func;
	}
}

VexMetric ParseMetric(const std::string &metric_str) {
	if (metric_str == "cosine" || metric_str == "cos") {
		return VexMetric::COSINE;
	}
	if (metric_str == "ip" || metric_str == "inner_product") {
		return VexMetric::INNER_PRODUCT;
	}
	if (metric_str == "l2" || metric_str == "euclidean") {
		return VexMetric::L2;
	}
	throw std::invalid_argument("Unknown metric '" + metric_str + "'. Supported: l2, cosine, ip (inner_product)");
}

const char *MetricName(VexMetric metric) {
	switch (metric) {
	case VexMetric::COSINE:
		return "cosine";
	case VexMetric::INNER_PRODUCT:
		return "inner_product";
	case VexMetric::L2:
	default:
		return "l2";
	}
}

void NormalizeVector(float *vec, uint32_t dim) {
	float norm_sq = g_ip_func(vec, vec, dim);
	if (norm_sq < 1e-30f) {
		return;
	}
	float inv_norm = 1.0f / std::sqrt(norm_sq);
	for (uint32_t i = 0; i < dim; i++) {
		vec[i] *= inv_norm;
	}
}

} // namespace vex
} // namespace duckdb
