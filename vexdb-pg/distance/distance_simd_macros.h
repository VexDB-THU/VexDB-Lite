/**
 * Copyright ...
 * Definitions for distance macros
 */

#ifndef DISTANCE_MACROS_H
#define DISTANCE_MACROS_H

#ifdef USE_AVX512
#undef USE_AVX512
#endif
#ifdef USE_AVX
#undef USE_AVX
#endif
#ifdef USE_SSE
#undef USE_SSE
#endif

#ifdef __AVX512_SUPPORT__
#define USE_AVX512
#elif defined(__AVX_SUPPORT__)
#define USE_AVX
#elif defined(__SSE_SUPPORT__)
#define USE_SSE
#endif
#if defined(__NEON_SUPPORT__) && !defined(__SVE_SUPPORT__) && !defined(__SME_SUPPORT__)
#include <arm_neon.h>
#endif

#if defined(USE_AVX512) || defined(USE_AVX)
#include <immintrin.h>
#endif
#ifdef USE_SSE
#include <immintrin.h>
#include <xmmintrin.h>
#endif

#ifdef USE_AVX512
#define OPTIMIZE1
#define OPTIMIZE_HALF
constexpr uint32 k_per_iter = 16u;
constexpr uint32 half_k_per_iter = 16u;
constexpr uint32 int8_k_per_iter = 64u;
using vectorize_floats = __m512;
using vectorize_half_repr = vectorize_floats;
using vectorize_int8s = __m512i;
using vectorize_int8s_acc_repr = vectorize_int8s;
 constexpr uint32 half_k_per_iter_f = half_k_per_iter;
 using vectorize_half_repr_f = vectorize_half_repr;
static FORCE_INLINE vectorize_floats get_zero_vectors() { return _mm512_setzero_ps(); }
static FORCE_INLINE vectorize_floats load_vectors(const float *v) { return _mm512_load_ps(v); }
static FORCE_INLINE vectorize_floats loadu_vectors(const float *v) { return _mm512_loadu_ps(v); }
static FORCE_INLINE vectorize_floats sub_vectors(vectorize_floats a, vectorize_floats b)
    { return _mm512_sub_ps(a, b); }
[[maybe_unused]]
static FORCE_INLINE vectorize_floats add_vectors(vectorize_floats a, vectorize_floats b)
    { return _mm512_add_ps(a, b); }
static FORCE_INLINE vectorize_floats madd_vectors(vectorize_floats a, vectorize_floats b, vectorize_floats s)
    { return _mm512_fmadd_ps(a, b, s); }

static FORCE_INLINE vectorize_half_repr get_zero_halfs() { return _mm512_setzero_ps(); }
static FORCE_INLINE vectorize_half_repr load_halfs(const half *v)
    { return _mm512_cvtph_ps(_mm256_load_si256((const __m256i *)v)); }
static FORCE_INLINE vectorize_half_repr loadu_halfs(const half *v)
    { return _mm512_cvtph_ps(_mm256_loadu_si256((const __m256i *)v)); }
static FORCE_INLINE vectorize_half_repr sub_halfs(vectorize_half_repr a, vectorize_half_repr b)
    { return _mm512_sub_ps(a, b); }
[[maybe_unused]]
static FORCE_INLINE vectorize_half_repr add_halfs(vectorize_half_repr a, vectorize_half_repr b)
    { return _mm512_add_ps(a, b); }
static FORCE_INLINE vectorize_half_repr madd_halfs(vectorize_half_repr a, vectorize_half_repr b, vectorize_half_repr s)
    { return _mm512_fmadd_ps(a, b, s); }

 static FORCE_INLINE vectorize_half_repr_f get_zero_halfs_f() { return get_zero_halfs(); }
 static FORCE_INLINE vectorize_half_repr_f load_halfs_f(const half *v) { return load_halfs(v); }
 static FORCE_INLINE vectorize_half_repr_f loadu_halfs_f(const half *v)
 {
     return loadu_halfs(v);
 }
 static FORCE_INLINE vectorize_half_repr_f sub_halfs_f(vectorize_half_repr_f a, vectorize_half_repr_f b)
     { return sub_halfs(a, b); }      
 static FORCE_INLINE vectorize_half_repr_f madd_halfs_f(vectorize_half_repr_f a, vectorize_half_repr_f b, vectorize_half_repr_f s)
     { return madd_halfs(a, b, s); }      
 
static FORCE_INLINE vectorize_half_repr initialize_halfs(float norm)
    { return _mm512_set1_ps(norm); }
static FORCE_INLINE vectorize_half_repr mul_halfs(vectorize_half_repr a, vectorize_half_repr b)
    { return _mm512_mul_ps(a, b); }
[[maybe_unused]]     
static FORCE_INLINE void store_halfs(half *out, vectorize_half_repr x)
    { _mm256_store_si256((__m256i*)out, _mm512_cvtps_ph(x, _MM_FROUND_TO_NEAREST_INT|_MM_FROUND_NO_EXC)); }
static FORCE_INLINE void storeu_halfs(half *out, vectorize_half_repr x)
    { _mm256_storeu_si256((__m256i*)out, _mm512_cvtps_ph(x, _MM_FROUND_TO_NEAREST_INT|_MM_FROUND_NO_EXC)); }      
    

static FORCE_INLINE vectorize_int8s get_zero_int8s() { return _mm512_setzero_si512(); }
static FORCE_INLINE vectorize_int8s load_int8s(const int8_t *v)
    { return _mm512_load_si512((__m512i*)(v)); }
static FORCE_INLINE vectorize_int8s loadu_int8s(const int8_t *v)
    { return _mm512_loadu_si512((__m512i*)(v)); }

static FORCE_INLINE vectorize_int8s sub_madd_int8s(vectorize_int8s a, vectorize_int8s b,
                                                   vectorize_int8s s)
{
    // Subtract
    vectorize_int8s d_lo = _mm512_sub_epi16(_mm512_cvtepi8_epi16(_mm512_extracti64x4_epi64(a, 0)),
     _mm512_cvtepi8_epi16(_mm512_extracti64x4_epi64(b, 0)));
    vectorize_int8s d_hi = _mm512_sub_epi16(_mm512_cvtepi8_epi16(_mm512_extracti64x4_epi64(a, 1)), 
    _mm512_cvtepi8_epi16(_mm512_extracti64x4_epi64(b, 1)));

    //_mm512_madd_epi16(a, b): Square and pairwise sum: (a0*b0+a1*b1, a2*b2+a3*b3, ...) → 16 x int32
    s = _mm512_add_epi32(s, _mm512_madd_epi16(d_lo, d_lo));
    return _mm512_add_epi32(s, _mm512_madd_epi16(d_hi, d_hi));
}
static FORCE_INLINE vectorize_int8s madd_int8s(vectorize_int8s a, vectorize_int8s b, vectorize_int8s s)
{         
    //_mm512_madd_epi16(a, b): Square and pairwise sum: (a0*b0+a1*b1, a2*b2+a3*b3, ...) → 16 x int32
    s = _mm512_add_epi32(s, _mm512_madd_epi16( _mm512_cvtepi8_epi16(_mm512_extracti64x4_epi64(a, 0)), 
        _mm512_cvtepi8_epi16(_mm512_extracti64x4_epi64(b, 0))));
    return _mm512_add_epi32(s, _mm512_madd_epi16(_mm512_cvtepi8_epi16(_mm512_extracti64x4_epi64(a, 1)), 
        _mm512_cvtepi8_epi16(_mm512_extracti64x4_epi64(b, 1))));
}

#elif defined(USE_AVX)
#define OPTIMIZE1
#define OPTIMIZE_HALF
constexpr uint32 k_per_iter = 8u;
constexpr uint32 half_k_per_iter = 8u;
constexpr uint32 int8_k_per_iter = 32u;
using vectorize_floats = __m256;
using vectorize_half_repr = vectorize_floats;
using vectorize_int8s = __m256i;
using vectorize_int8s_acc_repr = vectorize_int8s;
 constexpr uint32 half_k_per_iter_f = half_k_per_iter;
 using vectorize_half_repr_f = vectorize_half_repr;
static FORCE_INLINE vectorize_floats get_zero_vectors() { return _mm256_setzero_ps(); }
static FORCE_INLINE vectorize_floats load_vectors(const float *v) { return _mm256_load_ps(v); }
static FORCE_INLINE vectorize_floats loadu_vectors(const float *v) { return _mm256_loadu_ps(v); }
static FORCE_INLINE vectorize_floats sub_vectors(vectorize_floats a, vectorize_floats b)
    { return _mm256_sub_ps(a, b); }
[[maybe_unused]]
static FORCE_INLINE vectorize_floats add_vectors(vectorize_floats a, vectorize_floats b)
    { return _mm256_add_ps(a, b); }
static FORCE_INLINE vectorize_floats madd_vectors(vectorize_floats a, vectorize_floats b, vectorize_floats s)
    { return _mm256_fmadd_ps(a, b, s); }

static FORCE_INLINE vectorize_half_repr get_zero_halfs() { return _mm256_setzero_ps(); }
static FORCE_INLINE vectorize_half_repr load_halfs(const half *v)
    { return _mm256_cvtph_ps(_mm_load_si128((const __m128i *)v)); }
static FORCE_INLINE vectorize_half_repr loadu_halfs(const half *v)
    { return _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)v)); }
static FORCE_INLINE vectorize_half_repr sub_halfs(vectorize_half_repr a, vectorize_half_repr b)
    { return _mm256_sub_ps(a, b); }
[[maybe_unused]]
static FORCE_INLINE vectorize_half_repr add_halfs(vectorize_half_repr a, vectorize_half_repr b)
    { return _mm256_add_ps(a, b); }
static FORCE_INLINE vectorize_half_repr madd_halfs(vectorize_half_repr a, vectorize_half_repr b, vectorize_half_repr s)
    { return _mm256_fmadd_ps(a, b, s); }

 static FORCE_INLINE vectorize_half_repr_f get_zero_halfs_f() { return get_zero_halfs(); }
 static FORCE_INLINE vectorize_half_repr_f load_halfs_f(const half *v) { return load_halfs(v); }
 static FORCE_INLINE vectorize_half_repr_f loadu_halfs_f(const half *v)
 {
     return loadu_halfs(v);
 }
 static FORCE_INLINE vectorize_half_repr_f sub_halfs_f(vectorize_half_repr_f a, vectorize_half_repr_f b)
     { return sub_halfs(a, b); }      
 static FORCE_INLINE vectorize_half_repr_f madd_halfs_f(vectorize_half_repr_f a, vectorize_half_repr_f b, vectorize_half_repr_f s)
     { return madd_halfs(a, b, s); }        
 
static FORCE_INLINE vectorize_half_repr initialize_halfs(float norm)
    { return _mm256_set1_ps(norm); }
static FORCE_INLINE vectorize_half_repr mul_halfs(vectorize_half_repr a, vectorize_half_repr b)
    { return _mm256_mul_ps(a, b); }
[[maybe_unused]]    
static FORCE_INLINE void store_halfs(half *out, vectorize_half_repr x)
    { _mm_store_si128((__m128i*)out, _mm256_cvtps_ph(x, _MM_FROUND_TO_NEAREST_INT|_MM_FROUND_NO_EXC)); } 
static FORCE_INLINE void storeu_halfs(half *out, vectorize_half_repr x)
    { _mm_storeu_si128((__m128i*)out, _mm256_cvtps_ph(x, _MM_FROUND_TO_NEAREST_INT|_MM_FROUND_NO_EXC)); }     

static FORCE_INLINE vectorize_int8s get_zero_int8s() { return _mm256_setzero_si256(); }
static FORCE_INLINE vectorize_int8s load_int8s(const int8_t *v)
    { return _mm256_load_si256((__m256i*)(v)); }
static FORCE_INLINE vectorize_int8s loadu_int8s(const int8_t *v)
    { return _mm256_loadu_si256((__m256i*)(v)); }

static FORCE_INLINE vectorize_int8s sub_madd_int8s(vectorize_int8s a, vectorize_int8s b,
                                                    vectorize_int8s s)
{
    // Subtract
    vectorize_int8s d_lo = _mm256_sub_epi16( _mm256_cvtepi8_epi16(_mm256_extracti128_si256(a, 0)),
         _mm256_cvtepi8_epi16(_mm256_extracti128_si256(b, 0)));
    vectorize_int8s d_hi = _mm256_sub_epi16(_mm256_cvtepi8_epi16(_mm256_extracti128_si256(a, 1)), 
                _mm256_cvtepi8_epi16(_mm256_extracti128_si256(b, 1)));

    s = _mm256_add_epi32(s, _mm256_madd_epi16(d_lo, d_lo));
    return _mm256_add_epi32(s, _mm256_madd_epi16(d_hi, d_hi));
}

static FORCE_INLINE vectorize_int8s madd_int8s(vectorize_int8s a, vectorize_int8s b, vectorize_int8s s)
{
    s = _mm256_add_epi32(s, _mm256_madd_epi16(_mm256_cvtepi8_epi16(_mm256_extracti128_si256(a, 0)),
         _mm256_cvtepi8_epi16(_mm256_extracti128_si256(b, 0))));
    return _mm256_add_epi32(s, _mm256_madd_epi16(_mm256_cvtepi8_epi16(_mm256_extracti128_si256(a, 1)), 
        _mm256_cvtepi8_epi16(_mm256_extracti128_si256(b, 1))));
}
#elif defined(USE_SSE)
#define OPTIMIZE1
constexpr uint32 k_per_iter = 4u;
using vectorize_floats = __m128;
constexpr uint32 int8_k_per_iter = 16u;
using vectorize_int8s = __m128i;
using vectorize_int8s_acc_repr = vectorize_int8s;
static FORCE_INLINE vectorize_floats get_zero_vectors() { return _mm_setzero_ps(); }
static FORCE_INLINE vectorize_floats load_vectors(const float *v) { return _mm_load_ps(v); }
static FORCE_INLINE vectorize_floats loadu_vectors(const float *v) { return _mm_loadu_ps(v); }
static FORCE_INLINE vectorize_floats sub_vectors(vectorize_floats a, vectorize_floats b)
    { return _mm_sub_ps(a, b); }
[[maybe_unused]]
static FORCE_INLINE vectorize_floats add_vectors(vectorize_floats a, vectorize_floats b)
    { return _mm_add_ps(a, b); }
static FORCE_INLINE vectorize_floats madd_vectors(vectorize_floats a, vectorize_floats b, vectorize_floats s)
    { return _mm_fmadd_ps(a, b, s); }

static FORCE_INLINE vectorize_int8s get_zero_int8s() { return _mm_setzero_si128(); }
static FORCE_INLINE vectorize_int8s load_int8s(const int8_t *v)
    { return _mm_load_si128((__m128i*)(v)); }
static FORCE_INLINE vectorize_int8s loadu_int8s(const int8_t *v)
    { return _mm_loadu_si128((__m128i*)(v)); }

static FORCE_INLINE vectorize_int8s sub_madd_int8s(vectorize_int8s a, vectorize_int8s b,
    vectorize_int8s s)
{
    // Subtract
    vectorize_int8s d_lo = _mm_sub_epi16(_mm_cvtepi8_epi16(a), _mm_cvtepi8_epi16(b));
    vectorize_int8s d_hi = _mm_sub_epi16( _mm_cvtepi8_epi16(_mm_srli_si128(a, 8)),  _mm_cvtepi8_epi16(_mm_srli_si128(b, 8)));

    s = _mm_add_epi32(s, _mm_madd_epi16(d_lo, d_lo));
    return _mm_add_epi32(s, _mm_madd_epi16(d_hi, d_hi));
}

static FORCE_INLINE vectorize_int8s madd_int8s(vectorize_int8s a, vectorize_int8s b, vectorize_int8s s)
{
    s = _mm_add_epi32(s, _mm_madd_epi16(_mm_cvtepi8_epi16(a), _mm_cvtepi8_epi16(b)));
    return _mm_add_epi32(s, _mm_madd_epi16(_mm_cvtepi8_epi16(_mm_srli_si128(a, 8)), _mm_cvtepi8_epi16(_mm_srli_si128(b, 8))));
}
#elif defined(__NEON_SUPPORT__) && !defined(__SVE_SUPPORT__) && !defined(__SME_SUPPORT__)
#define OPTIMIZE1
#define OPTIMIZE_HALF
constexpr uint32 k_per_iter = 4u;
constexpr uint32 half_k_per_iter = 8u;
constexpr uint32 int8_k_per_iter = 16u;
using vectorize_floats = float32x4_t;
using vectorize_half_repr = float16x8_t;
 constexpr uint32 half_k_per_iter_f = 4u;
 using vectorize_half_repr_f = float32x4_t;
using vectorize_int8s = int8x16_t;
using vectorize_int8s_acc_repr = int32x4_t;
static FORCE_INLINE vectorize_floats get_zero_vectors() { return vdupq_n_f32(0.0f); }
static FORCE_INLINE vectorize_floats load_vectors(const float *v) { return vld1q_f32(v); }
static FORCE_INLINE vectorize_floats loadu_vectors(const float *v)
{
#if defined(__ARM_FEATURE_UNALIGNED) || defined(__aarch64__)
    return vld1q_f32(v);
#else
    float32x4_t result;
    memcpy(&result, v, sizeof(result));
    return result;
#endif
}
static FORCE_INLINE vectorize_floats sub_vectors(vectorize_floats a, vectorize_floats b)
    { return vsubq_f32(a, b); }
static FORCE_INLINE vectorize_floats add_vectors(vectorize_floats a, vectorize_floats b)
    { return vaddq_f32(a, b); }
static FORCE_INLINE vectorize_floats madd_vectors(vectorize_floats a, vectorize_floats b, vectorize_floats s)
    { return vmlaq_f32(s, a, b); }

static FORCE_INLINE vectorize_half_repr get_zero_halfs() { return vdupq_n_f16((float16_t)0.0f); }
static FORCE_INLINE vectorize_half_repr load_halfs(const half *v) { return vld1q_f16(v); }
static FORCE_INLINE vectorize_half_repr loadu_halfs(const half *v)
{
#if defined(__ARM_FEATURE_UNALIGNED) || defined(__aarch64__)
    return vld1q_f16(v);
#else
    vectorize_half_repr result;
    memcpy(&result, v, sizeof(result));
    return result;
#endif
}
 [[maybe_unused]]
static FORCE_INLINE vectorize_half_repr sub_halfs(vectorize_half_repr a, vectorize_half_repr b)
    { return vsubq_f16(a, b); }
[[maybe_unused]]
static FORCE_INLINE vectorize_half_repr add_halfs(vectorize_half_repr a, vectorize_half_repr b)
    { return vaddq_f16(a, b); }
static FORCE_INLINE vectorize_half_repr madd_halfs(vectorize_half_repr a, vectorize_half_repr b, vectorize_half_repr s)
    { return vfmaq_f16(s, a, b); }
 static FORCE_INLINE vectorize_half_repr_f get_zero_halfs_f() { return vdupq_n_f32(0.0f); }
 static FORCE_INLINE vectorize_half_repr_f load_halfs_f(const half *v) { return vcvt_f32_f16(vld1_f16(v)); }
 static FORCE_INLINE vectorize_half_repr_f loadu_halfs_f(const half *v)
 {
 #if defined(__ARM_FEATURE_UNALIGNED) || defined(__aarch64__)
     return vcvt_f32_f16(vld1_f16(v));
 #else
     vectorize_half_repr result;
     float16x4_t temp;
     memcpy(&temp, v, sizeof(temp));
     return vcvt_f32_f16(temp);
 #endif
 }
 static FORCE_INLINE vectorize_half_repr_f sub_halfs_f(vectorize_half_repr_f a, vectorize_half_repr_f b)
     { return vsubq_f32(a, b); }      
 static FORCE_INLINE vectorize_half_repr_f madd_halfs_f(vectorize_half_repr_f a, vectorize_half_repr_f b, vectorize_half_repr_f s)
     { return vfmaq_f32(s, a, b); }    

static FORCE_INLINE vectorize_half_repr initialize_halfs(float norm)
    { return vdupq_n_f16((float16_t)norm); }
static FORCE_INLINE vectorize_half_repr mul_halfs(vectorize_half_repr a, vectorize_half_repr b)
    { return vmulq_f16(a, b); }
[[maybe_unused]] 
static FORCE_INLINE void store_halfs(half *out, vectorize_half_repr x)
    { vst1q_f16(out, x); }
static FORCE_INLINE void storeu_halfs(half *out, vectorize_half_repr x)
{
#if defined(__ARM_FEATURE_UNALIGNED) || defined(__aarch64__)
    vst1q_f16(out, x);
#else
    memcpy(out, &x, sizeof(vectorize_half_repr));
#endif
}

static FORCE_INLINE vectorize_int8s_acc_repr get_zero_int8s() { return vdupq_n_s32(0); }
static FORCE_INLINE vectorize_int8s load_int8s(const int8_t *v)
    { return vld1q_s8((v)); }
static FORCE_INLINE vectorize_int8s loadu_int8s(const int8_t *v)
{ 
#if defined(__ARM_FEATURE_UNALIGNED) || defined(__aarch64__)
    return vld1q_s8(v);
#else
    vectorize_int8s result;
    memcpy(&result, v, sizeof(result));
    return result;
#endif
}

static FORCE_INLINE vectorize_int8s_acc_repr sub_madd_int8s(vectorize_int8s a, vectorize_int8s b,
    vectorize_int8s_acc_repr s)
{
    int16x8_t d_lo = vsubq_s16(vmovl_s8(vget_low_s8(a)), vmovl_s8(vget_low_s8(b)));
    int16x8_t d_hi = vsubq_s16(vmovl_s8(vget_high_s8(a)), vmovl_s8(vget_high_s8(b)));
    s = vmlal_high_s16(vmlal_s16(s, vget_low_s16(d_lo), vget_low_s16(d_lo)), d_lo, d_lo);
    return vmlal_high_s16(vmlal_s16(s, vget_low_s16(d_hi), vget_low_s16(d_hi)), d_hi, d_hi);
}

static FORCE_INLINE vectorize_int8s_acc_repr madd_int8s(vectorize_int8s a, vectorize_int8s b, vectorize_int8s_acc_repr s)
{
#if COMPILER_SUPPORT_DOTPROD
    return vdotq_s32(s, a, b);
#else
    int16x8_t a_lo = vmovl_s8(vget_low_s8(a));
    int16x8_t a_hi = vmovl_s8(vget_high_s8(a));
    int16x8_t b_lo = vmovl_s8(vget_low_s8(b));
    int16x8_t b_hi = vmovl_s8(vget_high_s8(b));
    s = vmlal_high_s16(vmlal_s16(s, vget_low_s16(a_lo), vget_low_s16(b_lo)), a_lo, b_lo);
    return vmlal_high_s16(vmlal_s16(s, vget_low_s16(a_hi), vget_low_s16(b_hi)), a_hi, b_hi);
#endif
}
#endif

#ifdef OPTIMIZE1
constexpr uint32 k_unroll = 4u;
constexpr uint32 k_step = k_per_iter * k_unroll;
constexpr uint32 int8_k_step = int8_k_per_iter * k_unroll;
#endif

#ifdef OPTIMIZE_HALF
constexpr uint32 half_k_step = half_k_per_iter * k_unroll;
 constexpr uint32 half_k_step_f = half_k_per_iter_f * k_unroll;
#endif

#endif /* DISTANCE_MACROS_H */
