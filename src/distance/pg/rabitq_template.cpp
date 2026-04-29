#ifndef DISTANCE_FUNC_NAME
static_assert(false, "don't use the file without definition DISTANCE_FUNC_NAME");
#endif

#if defined(__AVX_SUPPORT__) || defined(__AVX512_SUPPORT__)
#include <emmintrin.h>
#include <immintrin.h>
#elif defined(__SSE_SUPPORT__)
#include <emmintrin.h>
#endif

#ifdef __NEON_SUPPORT__
#include <arm_neon.h>
#endif
#if defined(__SVE_SUPPORT__) || defined(__SVE2_SUPPORT__) || defined(__SME_SUPPORT__) || defined(__SME2_SUPPORT__)
#define DO_SVE_OPTIMIZE
#include <arm_sve.h>
#endif

#include <vtl/array>
#include <vtl/expr_helper>

#include "pg_compat.h"
#include "distance/core/distance_utils_core.h"
#include "distance/core/transform_template_core.h"

template<uint32 LOG_N>
static inline void DISTANCE_FUNC_NAME(helper_float)(float *buf) {
    CONSTEXPR_IF(LOG_N == 1u) {
        float u = buf[0];
        float v = buf[1];
        buf[0] = u + v;
        buf[1] = u - v;
        return;
    }

    constexpr uint32 N = 1 << LOG_N;
    constexpr uint32 HALF_N = N >> 1;
    CONSTEXPR_IF(LOG_N > 1) {
        DISTANCE_FUNC_NAME(helper_float)<LOG_N - 1>(buf);
        DISTANCE_FUNC_NAME(helper_float)<LOG_N - 1>(buf + HALF_N);
    }
    uint32 k = 0;
#ifdef __AVX512_SUPPORT__
    CONSTEXPR_IF(HALF_N >= 16) {
        for (; k + 16 <= HALF_N; k += 16) {
            __m512 first = _mm512_loadu_ps(buf + k);
            __m512 second = _mm512_loadu_ps(buf + k + HALF_N);
            __m512 add_result = _mm512_add_ps(first, second);
            __m512 sub_result = _mm512_sub_ps(first, second);
            _mm512_storeu_ps(buf + k, add_result);
            _mm512_storeu_ps(buf + k + HALF_N, sub_result);
        }
    }
#endif
#ifdef __AVX_SUPPORT__
    CONSTEXPR_IF(HALF_N >= 8) {
        for (; k + 8 <= HALF_N; k += 8) {
            __m256 first = _mm256_loadu_ps(buf + k);
            __m256 second = _mm256_loadu_ps(buf + k + HALF_N);
            __m256 add_result = _mm256_add_ps(first, second);
            __m256 sub_result = _mm256_sub_ps(first, second);
            _mm256_storeu_ps(buf + k, add_result);
            _mm256_storeu_ps(buf + k + HALF_N, sub_result);
        }
    }
#endif
#ifdef __SSE_SUPPORT__
    CONSTEXPR_IF(HALF_N >= 4) {
        for (; k + 4 <= HALF_N; k += 4) {
            __m128 first = _mm_loadu_ps(buf + k);
            __m128 second = _mm_loadu_ps(buf + k + HALF_N);
            __m128 add_result = _mm_add_ps(first, second);
            __m128 sub_result = _mm_sub_ps(first, second);
            _mm_storeu_ps(buf + k, add_result);
            _mm_storeu_ps(buf + k + HALF_N, sub_result);
        }
    }
#endif
#ifdef DO_SVE_OPTIMIZE
    static const int vec_len = svcntw();
    for (; k + vec_len <= HALF_N; k += vec_len) {
        svbool_t pg = svptrue_b32();
        svfloat32_t first = svld1_f32(pg, buf + k);
        svfloat32_t second = svld1_f32(pg, buf + k + HALF_N);
        svfloat32_t add_result = svadd_f32_z(pg, first, second);
        svfloat32_t sub_result = svsub_f32_z(pg, first, second);
        svst1_f32(pg, buf + k, add_result);
        svst1_f32(pg, buf + k + HALF_N, sub_result);
    }
#elif defined(__NEON_SUPPORT__)
    CONSTEXPR_IF(HALF_N >= 4) {
        for (; k + 4 <= HALF_N; k += 4) {
            float32x4_t first = vld1q_f32(buf + k);
            float32x4_t second = vld1q_f32(buf + k + HALF_N);
            float32x4_t add_result = vaddq_f32(first, second);
            float32x4_t sub_result = vsubq_f32(first, second);
            vst1q_f32(buf + k, add_result);
            vst1q_f32(buf + k + HALF_N, sub_result);
        }
    }
#endif

#ifdef DO_SVE_OPTIMIZE
    if (k < HALF_N) {
        svbool_t pg = svwhilelt_b32(k, HALF_N);
        svfloat32_t first = svld1_f32(pg, buf + k);
        svfloat32_t second = svld1_f32(pg, buf + k + HALF_N);
        svfloat32_t add_result = svadd_f32_z(pg, first, second);
        svfloat32_t sub_result = svsub_f32_z(pg, first, second);
        svst1_f32(pg, buf + k, add_result);
        svst1_f32(pg, buf + k + HALF_N, sub_result);
    }
#else
    for (; k < HALF_N; ++k) {
        float u = buf[k];
        float v = buf[k + HALF_N];
        buf[k] = u + v;
        buf[k + HALF_N] = u - v;
    }
#endif
}

#define FHT_HELPER(z, i, x)  \
void DISTANCE_FUNC_NAME(fht_helper_##i)(float *buf) { DISTANCE_FUNC_NAME(helper_float)<i>(buf); }
namespace ann_helper {
BOOST_PP_REPEAT_FROM_TO(1, BOOST_PP_ADD(max_vector_bottom_dim, 1), FHT_HELPER, x)
}   /* namespace ann_helper */

static constexpr Array<Array<uint32, 8>, 256> generate_avx2_mask_patterns() {
    Array<Array<uint32, 8>, 256> patterns{};
    for (uint32 mask_val = 0; mask_val < 256; ++mask_val) {
        for (int bit = 0; bit < 8; ++bit) {
            patterns[mask_val][bit] = (mask_val & (1 << bit)) ? 0xFFFFFFFF : 0;
        }
    }
    return patterns;
}
static constexpr Array<Array<float, 8>, 256> generate_avx_mask_table() {
    Array<Array<float, 8>, 256> mask_table{};
    for (int m = 0; m < 256; m++) {
        for (int i = 0; i < 8; ++i) {
            mask_table[m][i] = (m & (1 << i)) ? 1.0f : 0.0f;
        }
    }
    return mask_table;
}
static constexpr Array<Array<float, 4>, 16> generate_neon_mask4_table() {
    Array<Array<float, 4>, 16> mask_table{};
    for (int m = 0; m < 16; ++m) {
        for (int i = 0; i < 4; ++i) {
            mask_table[m][i] = (m & (1 << i)) ? 1.0f : 0.0f;
        }
    }
    return mask_table;
}
static uint64 reverse_bits_u64(uint64 n)
{
    n = ((n >> 1) & 0x5555555555555555) | ((n << 1) & 0xaaaaaaaaaaaaaaaa);
    n = ((n >> 2) & 0x3333333333333333) | ((n << 2) & 0xcccccccccccccccc);
    n = ((n >> 4) & 0x0f0f0f0f0f0f0f0f) | ((n << 4) & 0xf0f0f0f0f0f0f0f0);
    n = ((n >> 8) & 0x00ff00ff00ff00ff) | ((n << 8) & 0xff00ff00ff00ff00);
    n = ((n >> 16) & 0x0000ffff0000ffff) | ((n << 16) & 0xffff0000ffff0000);
    n = ((n >> 32) & 0x00000000ffffffff) | ((n << 32) & 0xffffffff00000000);
    return n;
}

namespace ann_helper {
void DISTANCE_FUNC_NAME(flip_sign)(const uint8 *flip, float *data, size_t dim)
{
    size_t i = 0;
#ifdef __AVX512_SUPPORT__
    constexpr size_t kFloatsPerChunk = 64;
    const __m512 sign_flip = _mm512_castsi512_ps(_mm512_set1_epi32(0x80000000));
    for (; i + kFloatsPerChunk <= dim; i += kFloatsPerChunk) {
        uint64 mask_batch;
        memcpy(&mask_batch, &flip[i / 8], sizeof(mask_batch));
        uint16 mask_bits0 = (mask_batch) & 0xFFFF;
        uint16 mask_bits1 = (mask_batch >> 16) & 0xFFFF;
        uint16 mask_bits2 = (mask_batch >> (2 * 16)) & 0xFFFF;
        uint16 mask_bits3 = (mask_batch >> (3 * 16)) & 0xFFFF;
        __m512 data_vec0 = _mm512_loadu_ps(&data[i]);
        __m512 data_vec1 = _mm512_loadu_ps(&data[i + 16]);
        __m512 data_vec2 = _mm512_loadu_ps(&data[i + 2 * 16]);
        __m512 data_vec3 = _mm512_loadu_ps(&data[i + 3 * 16]);
        data_vec0 = _mm512_mask_xor_ps(data_vec0, _cvtu32_mask16(mask_bits0), data_vec0, sign_flip);
        data_vec1 = _mm512_mask_xor_ps(data_vec1, _cvtu32_mask16(mask_bits1), data_vec1, sign_flip);
        data_vec2 = _mm512_mask_xor_ps(data_vec2, _cvtu32_mask16(mask_bits2), data_vec2, sign_flip);
        data_vec3 = _mm512_mask_xor_ps(data_vec3, _cvtu32_mask16(mask_bits3), data_vec3, sign_flip);
        _mm512_storeu_ps(&data[i], data_vec0);
        _mm512_storeu_ps(&data[i + 16], data_vec1);
        _mm512_storeu_ps(&data[i + 2 * 16], data_vec2);
        _mm512_storeu_ps(&data[i + 3 * 16], data_vec3);
    }
    for (; i + 16 <= dim; i += 16) {
        uint16 mask_bits;
        memcpy(&mask_bits, &flip[i / 8], sizeof(mask_bits));
        __m512 data_vec = _mm512_loadu_ps(&data[i]);
        __mmask16 mask = _cvtu32_mask16(mask_bits);
        __m512 result = _mm512_mask_xor_ps(data_vec, mask, data_vec, sign_flip);
        _mm512_storeu_ps(&data[i], result);
    }
#elif defined(__AVX2_SUPPORT__) || defined(__AVX_SUPPORT__)
    constexpr size_t kFloatsPerChunk = 32;
    const __m256 sign_flip = _mm256_castsi256_ps(_mm256_set1_epi32(0x80000000));
    alignas(32) static constexpr auto mask_patterns_avx2 = generate_avx2_mask_patterns();
    for (; i + kFloatsPerChunk <= dim; i += kFloatsPerChunk) {
        uint32 mask_bits;
        memcpy(&mask_bits, &flip[i / 8], sizeof(mask_bits));

        __m256 data01 = _mm256_loadu_ps(&data[i]);
        __m256 data23 = _mm256_loadu_ps(&data[i + 8]);
        __m256 data45 = _mm256_loadu_ps(&data[i + 16]);
        __m256 data67 = _mm256_loadu_ps(&data[i + 24]);

        const uint8 mask01 = mask_bits & 0xFF;
        const uint8 mask23 = (mask_bits >> 8) & 0xFF;
        const uint8 mask45 = (mask_bits >> 16) & 0xFF;
        const uint8 mask67 = (mask_bits >> 24) & 0xFF;

        __m256i pattern01 = _mm256_load_si256(reinterpret_cast<const __m256i *>(mask_patterns_avx2[mask01].data()));
        __m256i pattern23 = _mm256_load_si256(reinterpret_cast<const __m256i *>(mask_patterns_avx2[mask23].data()));
        __m256i pattern45 = _mm256_load_si256(reinterpret_cast<const __m256i *>(mask_patterns_avx2[mask45].data()));
        __m256i pattern67 = _mm256_load_si256(reinterpret_cast<const __m256i *>(mask_patterns_avx2[mask67].data()));

        __m256 flipped01 = _mm256_xor_ps(data01, sign_flip);
        __m256 flipped23 = _mm256_xor_ps(data23, sign_flip);
        __m256 flipped45 = _mm256_xor_ps(data45, sign_flip);
        __m256 flipped67 = _mm256_xor_ps(data67, sign_flip);

        data01 = _mm256_blendv_ps(data01, flipped01, _mm256_castsi256_ps(pattern01));
        data23 = _mm256_blendv_ps(data23, flipped23, _mm256_castsi256_ps(pattern23));
        data45 = _mm256_blendv_ps(data45, flipped45, _mm256_castsi256_ps(pattern45));
        data67 = _mm256_blendv_ps(data67, flipped67, _mm256_castsi256_ps(pattern67));

        _mm256_storeu_ps(&data[i], data01);
        _mm256_storeu_ps(&data[i + 8], data23);
        _mm256_storeu_ps(&data[i + 16], data45);
        _mm256_storeu_ps(&data[i + 24], data67);
    }
#elif defined(__SSE_SUPPORT__)
    constexpr size_t kFloatsPerChunk = 16;
    static constexpr uint32 mask_patterns[16][4] __attribute__((aligned(16))) = {
        {0,0,0,0}, {0xFFFFFFFF,0,0,0}, {0,0xFFFFFFFF,0,0}, {0xFFFFFFFF,0xFFFFFFFF,0,0},
        {0,0,0xFFFFFFFF,0}, {0xFFFFFFFF,0,0xFFFFFFFF,0}, {0,0xFFFFFFFF,0xFFFFFFFF,0}, 
        {0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0}, {0,0,0,0xFFFFFFFF}, {0xFFFFFFFF,0,0,0xFFFFFFFF},
        {0,0xFFFFFFFF,0,0xFFFFFFFF}, {0xFFFFFFFF,0xFFFFFFFF,0,0xFFFFFFFF},
        {0,0,0xFFFFFFFF,0xFFFFFFFF}, {0xFFFFFFFF,0,0xFFFFFFFF,0xFFFFFFFF},
        {0,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF}, {0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF}
    };

    const __m128 sign_flip = _mm_castsi128_ps(_mm_set1_epi32(0x80000000));
    for (; i + kFloatsPerChunk <= dim; i += kFloatsPerChunk) {
        uint16 mask_bits;
        memcpy(&mask_bits, &flip[i / 8], sizeof(mask_bits));

        __m128 data0 = _mm_loadu_ps(&data[i]);
        __m128 data1 = _mm_loadu_ps(&data[i + 4]);
        __m128 data2 = _mm_loadu_ps(&data[i + 8]);
        __m128 data3 = _mm_loadu_ps(&data[i + 12]);

        const uint32 mask0 = mask_bits & 0xF;
        const uint32 mask1 = (mask_bits >> 4) & 0xF;
        const uint32 mask2 = (mask_bits >> 8) & 0xF;
        const uint32 mask3 = (mask_bits >> 12) & 0xF;

        __m128i pattern0 = _mm_load_si128(reinterpret_cast<const __m128i *>(mask_patterns[mask0]));
        __m128i pattern1 = _mm_load_si128(reinterpret_cast<const __m128i *>(mask_patterns[mask1]));
        __m128i pattern2 = _mm_load_si128(reinterpret_cast<const __m128i *>(mask_patterns[mask2]));
        __m128i pattern3 = _mm_load_si128(reinterpret_cast<const __m128i *>(mask_patterns[mask3]));

        __m128 flipped0 = _mm_xor_ps(data0, sign_flip);
        __m128 flipped1 = _mm_xor_ps(data1, sign_flip);
        __m128 flipped2 = _mm_xor_ps(data2, sign_flip);
        __m128 flipped3 = _mm_xor_ps(data3, sign_flip);

        data0 = _mm_blendv_ps(data0, flipped0, _mm_castsi128_ps(pattern0));
        data1 = _mm_blendv_ps(data1, flipped1, _mm_castsi128_ps(pattern1));
        data2 = _mm_blendv_ps(data2, flipped2, _mm_castsi128_ps(pattern2));
        data3 = _mm_blendv_ps(data3, flipped3, _mm_castsi128_ps(pattern3));

        _mm_storeu_ps(&data[i], data0);
        _mm_storeu_ps(&data[i + 4], data1);
        _mm_storeu_ps(&data[i + 8], data2);
        _mm_storeu_ps(&data[i + 12], data3);
    }
#elif defined(__SVE2_SUPPORT__) || defined(__SME_SUPPORT__) || defined(__SME2_SUPPORT__)
    static const size_t sve_len = svcntw();
    const svuint32_t sign_flip = svdup_n_u32(0x80000000u);
    for (size_t i = 0; i < dim; i += sve_len) {
        svbool_t pg = svwhilelt_b32(i, dim);

        svfloat32_t vec = svld1_f32(pg, &data[i]);

        const size_t bytes_needed = (sve_len + 7) / 8;
        svuint8_t flip_bytes = svld1_u8(svwhilelt_b8(size_t(0), bytes_needed), &flip[i / 8]);

        svbool_t flip_mask = svpfalse_b();
        for (size_t j = 0; j < sve_len && (i + j) < dim; ++j) {
            const size_t bit_idx = j;
            const size_t byte_idx = bit_idx / 8;
            const size_t bit_in_byte = bit_idx % 8;

            uint8 byte_val = svlasta_u8(svwhilelt_b8(byte_idx, byte_idx + 1), flip_bytes);
            if ((byte_val >> bit_in_byte) & 1) {
                flip_mask = svpnext_b32(flip_mask, flip_mask);
            }
        }

        svuint32_t vec_int = svreinterpret_u32_f32(vec);
        svuint32_t flipped = sveor_u32_m(flip_mask, vec_int, sign_flip);
        vec = svreinterpret_f32_u32(flipped);

        svst1_f32(pg, &data[i], vec);
    }
#elif defined(__SVE_SUPPORT__)
    static const size_t sve_len = svcntw();
    const svuint32_t sign_flip = svdup_n_u32(0x80000000u);
    for (size_t i = 0; i < dim; i += sve_len) {
        svbool_t pg = svwhilelt_b32(i, dim);

        svfloat32_t vec = svld1_f32(pg, &data[i]);

        const size_t byte_idx = i / 8;
        const size_t bit_offset = i % 8;

        svbool_t flip_mask = svpfalse_b();
        for (size_t j = 0; j < sve_len && (i + j) < dim; ++j) {
            const size_t global_idx = i + j;
            const size_t flip_byte_idx = global_idx / 8;
            const size_t flip_bit_idx = global_idx % 8;
            if ((flip[flip_byte_idx] >> flip_bit_idx) & 1) {
                flip_mask = svpnext_b32(flip_mask, flip_mask);
            }
        }

        svuint32_t vec_int = svreinterpret_u32_f32(vec);
        svuint32_t flipped = sveor_u32_m(flip_mask, vec_int, sign_flip);
        vec = svreinterpret_f32_u32(flipped);

        svst1_f32(pg, &data[i], vec);
    }
#elif defined(__NEON_SUPPORT__)
    constexpr size_t kFloatsPerChunk = 16;
    const uint32x4_t sign_flip = vdupq_n_u32(0x80000000u);
    for (; i + kFloatsPerChunk <= dim; i += kFloatsPerChunk) {
        uint16 mask_bits;
        memcpy(&mask_bits, &flip[i / 8], sizeof(mask_bits));

        const uint32 mask0 = mask_bits & 0xF;
        const uint32 mask1 = (mask_bits >> 4) & 0xF;
        const uint32 mask2 = (mask_bits >> 8) & 0xF;
        const uint32 mask3 = (mask_bits >> 12) & 0xF;

        float32x4_t vec0 = vld1q_f32(&data[i]);
        float32x4_t vec1 = vld1q_f32(&data[i + 4]);
        float32x4_t vec2 = vld1q_f32(&data[i + 8]);
        float32x4_t vec3 = vld1q_f32(&data[i + 12]);

        uint32x4_t blend_mask0 = {
            (mask0 >> 0) & 1 ? 0xFFFFFFFF : 0,
            (mask0 >> 1) & 1 ? 0xFFFFFFFF : 0,
            (mask0 >> 2) & 1 ? 0xFFFFFFFF : 0,
            (mask0 >> 3) & 1 ? 0xFFFFFFFF : 0
        };

        uint32x4_t blend_mask1 = {
            (mask1 >> 0) & 1 ? 0xFFFFFFFF : 0,
            (mask1 >> 1) & 1 ? 0xFFFFFFFF : 0,
            (mask1 >> 2) & 1 ? 0xFFFFFFFF : 0,
            (mask1 >> 3) & 1 ? 0xFFFFFFFF : 0
        };

        uint32x4_t blend_mask2 = {
            (mask2 >> 0) & 1 ? 0xFFFFFFFF : 0,
            (mask2 >> 1) & 1 ? 0xFFFFFFFF : 0,
            (mask2 >> 2) & 1 ? 0xFFFFFFFF : 0,
            (mask2 >> 3) & 1 ? 0xFFFFFFFF : 0
        };

        uint32x4_t blend_mask3 = {
            (mask3 >> 0) & 1 ? 0xFFFFFFFF : 0,
            (mask3 >> 1) & 1 ? 0xFFFFFFFF : 0,
            (mask3 >> 2) & 1 ? 0xFFFFFFFF : 0,
            (mask3 >> 3) & 1 ? 0xFFFFFFFF : 0
        };

        uint32x4_t vec0_int = vreinterpretq_u32_f32(vec0);
        uint32x4_t vec1_int = vreinterpretq_u32_f32(vec1);
        uint32x4_t vec2_int = vreinterpretq_u32_f32(vec2);
        uint32x4_t vec3_int = vreinterpretq_u32_f32(vec3);

        uint32x4_t flipped0 = veorq_u32(vec0_int, sign_flip);
        uint32x4_t flipped1 = veorq_u32(vec1_int, sign_flip);
        uint32x4_t flipped2 = veorq_u32(vec2_int, sign_flip);
        uint32x4_t flipped3 = veorq_u32(vec3_int, sign_flip);

        vec0_int = vbslq_u32(blend_mask0, flipped0, vec0_int);
        vec1_int = vbslq_u32(blend_mask1, flipped1, vec1_int);
        vec2_int = vbslq_u32(blend_mask2, flipped2, vec2_int);
        vec3_int = vbslq_u32(blend_mask3, flipped3, vec3_int);

        vec0 = vreinterpretq_f32_u32(vec0_int);
        vec1 = vreinterpretq_f32_u32(vec1_int);
        vec2 = vreinterpretq_f32_u32(vec2_int);
        vec3 = vreinterpretq_f32_u32(vec3_int);

        vst1q_f32(&data[i], vec0);
        vst1q_f32(&data[i + 4], vec1);
        vst1q_f32(&data[i + 8], vec2);
        vst1q_f32(&data[i + 12], vec3);
    }
#endif
    for (; i < dim; ++i) {
        const size_t byte_idx = i / 8;
        const size_t bit_idx = i % 8;
        if ((flip[byte_idx] >> bit_idx) & 1) {
            uint32 *int_data = reinterpret_cast<uint32 *>(&data[i]);
            *int_data ^= 0x80000000u;
        }
    }
}

void DISTANCE_FUNC_NAME(kacs_walk)(float *data, size_t len)
{
    const size_t half_len = len / 2;
    Assume(len % 32 == 0);
    for (size_t i = 0; i < half_len; ++i) {
        const float x = data[i];
        const float y = data[i + half_len];
        data[i] = x + y;
        data[i + half_len] = x - y;
    }
}
}   /* namespace ann_helper */

#include "rabitq.h"
namespace ann_helper {
float DISTANCE_FUNC_NAME(warmup_ip_x0_q)(uint64 *data, const uint64 *query, float delta, float vl, size_t dim)
{
    const size_t num_blk = dim / 64;
    size_t ip_scalar = 0;
    size_t ppc_scalar = 0;

#if defined(__AVX512_SUPPORT__) || defined(__AVX2_SUPPORT__) || defined(__AVX_SUPPORT__)
    // Process 8 blocks at a time for maximum ILP (works for AVX/AVX2/AVX512)
    size_t i = 0;

    // Use 8 independent accumulators to minimize dependency chains and hide popcount latency
    size_t ip_acc0 = 0, ip_acc1 = 0, ip_acc2 = 0, ip_acc3 = 0;
    size_t ip_acc4 = 0, ip_acc5 = 0, ip_acc6 = 0, ip_acc7 = 0;
    size_t ppc_acc0 = 0, ppc_acc1 = 0, ppc_acc2 = 0, ppc_acc3 = 0;
    size_t ppc_acc4 = 0, ppc_acc5 = 0, ppc_acc6 = 0, ppc_acc7 = 0;

    for (; i + 8 <= num_blk; i += 8) {
        // Load all data blocks first to improve memory throughput
        const uint64 x0 = data[i];
        const uint64 x1 = data[i + 1];
        const uint64 x2 = data[i + 2];
        const uint64 x3 = data[i + 3];
        const uint64 x4 = data[i + 4];
        const uint64 x5 = data[i + 5];
        const uint64 x6 = data[i + 6];
        const uint64 x7 = data[i + 7];

        // Load all query pointers early for better prefetching
        const uint64 * const q0 = &query[i * 4];
        const uint64 * const q1 = &query[(i + 1) * 4];
        const uint64 * const q2 = &query[(i + 2) * 4];
        const uint64 * const q3 = &query[(i + 3) * 4];
        const uint64 * const q4 = &query[(i + 4) * 4];
        const uint64 * const q5 = &query[(i + 5) * 4];
        const uint64 * const q6 = &query[(i + 6) * 4];
        const uint64 * const q7 = &query[(i + 7) * 4];

        // Compute popcounts for all blocks (independent operations)
        ppc_acc0 += __builtin_popcountll(x0);
        ppc_acc1 += __builtin_popcountll(x1);
        ppc_acc2 += __builtin_popcountll(x2);
        ppc_acc3 += __builtin_popcountll(x3);
        ppc_acc4 += __builtin_popcountll(x4);
        ppc_acc5 += __builtin_popcountll(x5);
        ppc_acc6 += __builtin_popcountll(x6);
        ppc_acc7 += __builtin_popcountll(x7);

        // Process block 0: compute weighted popcount with 4-bit encoding (0-15 range)
        ip_acc0 += __builtin_popcountll(x0 & q0[0]);
        ip_acc0 += __builtin_popcountll(x0 & q0[1]) << 1;
        ip_acc0 += __builtin_popcountll(x0 & q0[2]) << 2;
        ip_acc0 += __builtin_popcountll(x0 & q0[3]) << 3;

        // Process block 1
        ip_acc1 += __builtin_popcountll(x1 & q1[0]);
        ip_acc1 += __builtin_popcountll(x1 & q1[1]) << 1;
        ip_acc1 += __builtin_popcountll(x1 & q1[2]) << 2;
        ip_acc1 += __builtin_popcountll(x1 & q1[3]) << 3;

        // Process block 2
        ip_acc2 += __builtin_popcountll(x2 & q2[0]);
        ip_acc2 += __builtin_popcountll(x2 & q2[1]) << 1;
        ip_acc2 += __builtin_popcountll(x2 & q2[2]) << 2;
        ip_acc2 += __builtin_popcountll(x2 & q2[3]) << 3;

        // Process block 3
        ip_acc3 += __builtin_popcountll(x3 & q3[0]);
        ip_acc3 += __builtin_popcountll(x3 & q3[1]) << 1;
        ip_acc3 += __builtin_popcountll(x3 & q3[2]) << 2;
        ip_acc3 += __builtin_popcountll(x3 & q3[3]) << 3;

        // Process block 4
        ip_acc4 += __builtin_popcountll(x4 & q4[0]);
        ip_acc4 += __builtin_popcountll(x4 & q4[1]) << 1;
        ip_acc4 += __builtin_popcountll(x4 & q4[2]) << 2;
        ip_acc4 += __builtin_popcountll(x4 & q4[3]) << 3;

        // Process block 5
        ip_acc5 += __builtin_popcountll(x5 & q5[0]);
        ip_acc5 += __builtin_popcountll(x5 & q5[1]) << 1;
        ip_acc5 += __builtin_popcountll(x5 & q5[2]) << 2;
        ip_acc5 += __builtin_popcountll(x5 & q5[3]) << 3;

        // Process block 6
        ip_acc6 += __builtin_popcountll(x6 & q6[0]);
        ip_acc6 += __builtin_popcountll(x6 & q6[1]) << 1;
        ip_acc6 += __builtin_popcountll(x6 & q6[2]) << 2;
        ip_acc6 += __builtin_popcountll(x6 & q6[3]) << 3;

        // Process block 7
        ip_acc7 += __builtin_popcountll(x7 & q7[0]);
        ip_acc7 += __builtin_popcountll(x7 & q7[1]) << 1;
        ip_acc7 += __builtin_popcountll(x7 & q7[2]) << 2;
        ip_acc7 += __builtin_popcountll(x7 & q7[3]) << 3;
    }

    // Combine accumulators in a fully balanced binary tree to minimize dependency depth
    ip_scalar = ((ip_acc0 + ip_acc1) + (ip_acc2 + ip_acc3)) + ((ip_acc4 + ip_acc5) + (ip_acc6 + ip_acc7));
    ppc_scalar = ((ppc_acc0 + ppc_acc1) + (ppc_acc2 + ppc_acc3)) + ((ppc_acc4 + ppc_acc5) + (ppc_acc6 + ppc_acc7));

    // Process remaining blocks (0-7 blocks)
    for (; i < num_blk; ++i) {
        const uint64 x = data[i];
        ppc_scalar += __builtin_popcountll(x);
        const uint64 * const q = &query[i * 4];
        ip_scalar += __builtin_popcountll(x & q[0]);
        ip_scalar += __builtin_popcountll(x & q[1]) << 1;
        ip_scalar += __builtin_popcountll(x & q[2]) << 2;
        ip_scalar += __builtin_popcountll(x & q[3]) << 3;
    }
#elif defined(__NEON_SUPPORT__) || defined(DO_SVE_OPTIMIZE)
    // ARM NEON/SVE version - process 8 blocks at a time for maximum ILP
    size_t i = 0;
    size_t ip_acc0 = 0, ip_acc1 = 0, ip_acc2 = 0, ip_acc3 = 0;
    size_t ip_acc4 = 0, ip_acc5 = 0, ip_acc6 = 0, ip_acc7 = 0;
    size_t ppc_acc0 = 0, ppc_acc1 = 0, ppc_acc2 = 0, ppc_acc3 = 0;
    size_t ppc_acc4 = 0, ppc_acc5 = 0, ppc_acc6 = 0, ppc_acc7 = 0;

    for (; i + 8 <= num_blk; i += 8) {
        // Load all data blocks
        const uint64 x0 = data[i];
        const uint64 x1 = data[i + 1];
        const uint64 x2 = data[i + 2];
        const uint64 x3 = data[i + 3];
        const uint64 x4 = data[i + 4];
        const uint64 x5 = data[i + 5];
        const uint64 x6 = data[i + 6];
        const uint64 x7 = data[i + 7];

        // Load all query pointers
        const uint64 * const q0 = &query[i * 4];
        const uint64 * const q1 = &query[(i + 1) * 4];
        const uint64 * const q2 = &query[(i + 2) * 4];
        const uint64 * const q3 = &query[(i + 3) * 4];
        const uint64 * const q4 = &query[(i + 4) * 4];
        const uint64 * const q5 = &query[(i + 5) * 4];
        const uint64 * const q6 = &query[(i + 6) * 4];
        const uint64 * const q7 = &query[(i + 7) * 4];

        // Compute popcounts
        ppc_acc0 += __builtin_popcountll(x0);
        ppc_acc1 += __builtin_popcountll(x1);
        ppc_acc2 += __builtin_popcountll(x2);
        ppc_acc3 += __builtin_popcountll(x3);
        ppc_acc4 += __builtin_popcountll(x4);
        ppc_acc5 += __builtin_popcountll(x5);
        ppc_acc6 += __builtin_popcountll(x6);
        ppc_acc7 += __builtin_popcountll(x7);

        // Process all 8 blocks
        ip_acc0 += __builtin_popcountll(x0 & q0[0]);
        ip_acc0 += __builtin_popcountll(x0 & q0[1]) << 1;
        ip_acc0 += __builtin_popcountll(x0 & q0[2]) << 2;
        ip_acc0 += __builtin_popcountll(x0 & q0[3]) << 3;

        ip_acc1 += __builtin_popcountll(x1 & q1[0]);
        ip_acc1 += __builtin_popcountll(x1 & q1[1]) << 1;
        ip_acc1 += __builtin_popcountll(x1 & q1[2]) << 2;
        ip_acc1 += __builtin_popcountll(x1 & q1[3]) << 3;

        ip_acc2 += __builtin_popcountll(x2 & q2[0]);
        ip_acc2 += __builtin_popcountll(x2 & q2[1]) << 1;
        ip_acc2 += __builtin_popcountll(x2 & q2[2]) << 2;
        ip_acc2 += __builtin_popcountll(x2 & q2[3]) << 3;

        ip_acc3 += __builtin_popcountll(x3 & q3[0]);
        ip_acc3 += __builtin_popcountll(x3 & q3[1]) << 1;
        ip_acc3 += __builtin_popcountll(x3 & q3[2]) << 2;
        ip_acc3 += __builtin_popcountll(x3 & q3[3]) << 3;

        ip_acc4 += __builtin_popcountll(x4 & q4[0]);
        ip_acc4 += __builtin_popcountll(x4 & q4[1]) << 1;
        ip_acc4 += __builtin_popcountll(x4 & q4[2]) << 2;
        ip_acc4 += __builtin_popcountll(x4 & q4[3]) << 3;

        ip_acc5 += __builtin_popcountll(x5 & q5[0]);
        ip_acc5 += __builtin_popcountll(x5 & q5[1]) << 1;
        ip_acc5 += __builtin_popcountll(x5 & q5[2]) << 2;
        ip_acc5 += __builtin_popcountll(x5 & q5[3]) << 3;

        ip_acc6 += __builtin_popcountll(x6 & q6[0]);
        ip_acc6 += __builtin_popcountll(x6 & q6[1]) << 1;
        ip_acc6 += __builtin_popcountll(x6 & q6[2]) << 2;
        ip_acc6 += __builtin_popcountll(x6 & q6[3]) << 3;

        ip_acc7 += __builtin_popcountll(x7 & q7[0]);
        ip_acc7 += __builtin_popcountll(x7 & q7[1]) << 1;
        ip_acc7 += __builtin_popcountll(x7 & q7[2]) << 2;
        ip_acc7 += __builtin_popcountll(x7 & q7[3]) << 3;
    }

    // Combine accumulators in a fully balanced binary tree to minimize dependency depth
    ip_scalar = ((ip_acc0 + ip_acc1) + (ip_acc2 + ip_acc3)) + ((ip_acc4 + ip_acc5) + (ip_acc6 + ip_acc7));
    ppc_scalar = ((ppc_acc0 + ppc_acc1) + (ppc_acc2 + ppc_acc3)) + ((ppc_acc4 + ppc_acc5) + (ppc_acc6 + ppc_acc7));

    // Process remaining blocks
    for (; i < num_blk; ++i) {
        const uint64 x = data[i];
        ppc_scalar += __builtin_popcountll(x);
        const uint64 * const q = &query[i * 4];
        ip_scalar += __builtin_popcountll(x & q[0]);
        ip_scalar += __builtin_popcountll(x & q[1]) << 1;
        ip_scalar += __builtin_popcountll(x & q[2]) << 2;
        ip_scalar += __builtin_popcountll(x & q[3]) << 3;
    }
#else
    // Optimized scalar version with 8-way unrolling for maximum ILP
    size_t i = 0;
    size_t ip_acc0 = 0, ip_acc1 = 0, ip_acc2 = 0, ip_acc3 = 0;
    size_t ip_acc4 = 0, ip_acc5 = 0, ip_acc6 = 0, ip_acc7 = 0;
    size_t ppc_acc0 = 0, ppc_acc1 = 0, ppc_acc2 = 0, ppc_acc3 = 0;
    size_t ppc_acc4 = 0, ppc_acc5 = 0, ppc_acc6 = 0, ppc_acc7 = 0;

    // Process 8 blocks at a time
    for (; i + 8 <= num_blk; i += 8) {
        // Load all data blocks
        const uint64 x0 = data[i];
        const uint64 x1 = data[i + 1];
        const uint64 x2 = data[i + 2];
        const uint64 x3 = data[i + 3];
        const uint64 x4 = data[i + 4];
        const uint64 x5 = data[i + 5];
        const uint64 x6 = data[i + 6];
        const uint64 x7 = data[i + 7];

        // Load all query pointers
        const uint64 * const q0 = &query[i * 4];
        const uint64 * const q1 = &query[(i + 1) * 4];
        const uint64 * const q2 = &query[(i + 2) * 4];
        const uint64 * const q3 = &query[(i + 3) * 4];
        const uint64 * const q4 = &query[(i + 4) * 4];
        const uint64 * const q5 = &query[(i + 5) * 4];
        const uint64 * const q6 = &query[(i + 6) * 4];
        const uint64 * const q7 = &query[(i + 7) * 4];

        // Compute popcounts
        ppc_acc0 += __builtin_popcountll(x0);
        ppc_acc1 += __builtin_popcountll(x1);
        ppc_acc2 += __builtin_popcountll(x2);
        ppc_acc3 += __builtin_popcountll(x3);
        ppc_acc4 += __builtin_popcountll(x4);
        ppc_acc5 += __builtin_popcountll(x5);
        ppc_acc6 += __builtin_popcountll(x6);
        ppc_acc7 += __builtin_popcountll(x7);

        // Process all 8 blocks
        ip_acc0 += __builtin_popcountll(x0 & q0[0]);
        ip_acc0 += __builtin_popcountll(x0 & q0[1]) << 1;
        ip_acc0 += __builtin_popcountll(x0 & q0[2]) << 2;
        ip_acc0 += __builtin_popcountll(x0 & q0[3]) << 3;

        ip_acc1 += __builtin_popcountll(x1 & q1[0]);
        ip_acc1 += __builtin_popcountll(x1 & q1[1]) << 1;
        ip_acc1 += __builtin_popcountll(x1 & q1[2]) << 2;
        ip_acc1 += __builtin_popcountll(x1 & q1[3]) << 3;

        ip_acc2 += __builtin_popcountll(x2 & q2[0]);
        ip_acc2 += __builtin_popcountll(x2 & q2[1]) << 1;
        ip_acc2 += __builtin_popcountll(x2 & q2[2]) << 2;
        ip_acc2 += __builtin_popcountll(x2 & q2[3]) << 3;

        ip_acc3 += __builtin_popcountll(x3 & q3[0]);
        ip_acc3 += __builtin_popcountll(x3 & q3[1]) << 1;
        ip_acc3 += __builtin_popcountll(x3 & q3[2]) << 2;
        ip_acc3 += __builtin_popcountll(x3 & q3[3]) << 3;

        ip_acc4 += __builtin_popcountll(x4 & q4[0]);
        ip_acc4 += __builtin_popcountll(x4 & q4[1]) << 1;
        ip_acc4 += __builtin_popcountll(x4 & q4[2]) << 2;
        ip_acc4 += __builtin_popcountll(x4 & q4[3]) << 3;

        ip_acc5 += __builtin_popcountll(x5 & q5[0]);
        ip_acc5 += __builtin_popcountll(x5 & q5[1]) << 1;
        ip_acc5 += __builtin_popcountll(x5 & q5[2]) << 2;
        ip_acc5 += __builtin_popcountll(x5 & q5[3]) << 3;

        ip_acc6 += __builtin_popcountll(x6 & q6[0]);
        ip_acc6 += __builtin_popcountll(x6 & q6[1]) << 1;
        ip_acc6 += __builtin_popcountll(x6 & q6[2]) << 2;
        ip_acc6 += __builtin_popcountll(x6 & q6[3]) << 3;

        ip_acc7 += __builtin_popcountll(x7 & q7[0]);
        ip_acc7 += __builtin_popcountll(x7 & q7[1]) << 1;
        ip_acc7 += __builtin_popcountll(x7 & q7[2]) << 2;
        ip_acc7 += __builtin_popcountll(x7 & q7[3]) << 3;
    }

    // Combine accumulators in a fully balanced binary tree to minimize dependency depth
    ip_scalar = ((ip_acc0 + ip_acc1) + (ip_acc2 + ip_acc3)) + ((ip_acc4 + ip_acc5) + (ip_acc6 + ip_acc7));
    ppc_scalar = ((ppc_acc0 + ppc_acc1) + (ppc_acc2 + ppc_acc3)) + ((ppc_acc4 + ppc_acc5) + (ppc_acc6 + ppc_acc7));

    // Process remaining blocks
    for (; i < num_blk; ++i) {
        const uint64 x = data[i];
        ppc_scalar += __builtin_popcountll(x);
        const uint64 * const q = &query[i * 4];
        ip_scalar += __builtin_popcountll(x & q[0]);
        ip_scalar += __builtin_popcountll(x & q[1]) << 1;
        ip_scalar += __builtin_popcountll(x & q[2]) << 2;
        ip_scalar += __builtin_popcountll(x & q[3]) << 3;
    }
#endif

    return (delta * static_cast<float>(ip_scalar)) + (vl * static_cast<float>(ppc_scalar));
}

float DISTANCE_FUNC_NAME(ip_fxi)(float *query, uint8 *compact_code, size_t dim)
{
    float result = 0.0f;
    Assume(dim % 64 == 0);
    size_t i = 0;

#ifdef __AVX512_SUPPORT__
    __m512 sum0 = _mm512_setzero_ps();
    __m512 sum1 = _mm512_setzero_ps();
    __m512 sum2 = _mm512_setzero_ps();
    __m512 sum3 = _mm512_setzero_ps();
    // Unroll loop by 4 for better instruction-level parallelism
    for (; i + 64 <= dim; i += 64) {
        // Load 64 uint8 values and convert to float (4 iterations of 16 elements)
        __m128i codes_u8_0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&compact_code[i]));
        __m128i codes_u8_1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&compact_code[i + 16]));
        __m128i codes_u8_2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&compact_code[i + 32]));
        __m128i codes_u8_3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&compact_code[i + 48]));

        __m512i codes_u32_0 = _mm512_cvtepu8_epi32(codes_u8_0);
        __m512i codes_u32_1 = _mm512_cvtepu8_epi32(codes_u8_1);
        __m512i codes_u32_2 = _mm512_cvtepu8_epi32(codes_u8_2);
        __m512i codes_u32_3 = _mm512_cvtepu8_epi32(codes_u8_3);

        __m512 codes_f32_0 = _mm512_cvtepi32_ps(codes_u32_0);
        __m512 codes_f32_1 = _mm512_cvtepi32_ps(codes_u32_1);
        __m512 codes_f32_2 = _mm512_cvtepi32_ps(codes_u32_2);
        __m512 codes_f32_3 = _mm512_cvtepi32_ps(codes_u32_3);

        // Load query values
        __m512 query_vec_0 = _mm512_loadu_ps(&query[i]);
        __m512 query_vec_1 = _mm512_loadu_ps(&query[i + 16]);
        __m512 query_vec_2 = _mm512_loadu_ps(&query[i + 32]);
        __m512 query_vec_3 = _mm512_loadu_ps(&query[i + 48]);

        // Multiply and accumulate
        sum0 = _mm512_fmadd_ps(query_vec_0, codes_f32_0, sum0);
        sum1 = _mm512_fmadd_ps(query_vec_1, codes_f32_1, sum1);
        sum2 = _mm512_fmadd_ps(query_vec_2, codes_f32_2, sum2);
        sum3 = _mm512_fmadd_ps(query_vec_3, codes_f32_3, sum3);
    }
    // Combine accumulators in balanced tree for minimal dependency depth
    sum0 = _mm512_add_ps(_mm512_add_ps(sum0, sum1), _mm512_add_ps(sum2, sum3));

    // Handle remainder in chunks of 16
    for (; i + 16 <= dim; i += 16) {
        __m128i codes_u8 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&compact_code[i]));
        __m512i codes_u32 = _mm512_cvtepu8_epi32(codes_u8);
        __m512 codes_f32 = _mm512_cvtepi32_ps(codes_u32);
        __m512 query_vec = _mm512_loadu_ps(&query[i]);
        sum0 = _mm512_fmadd_ps(query_vec, codes_f32, sum0);
    }
    result = _mm512_reduce_add_ps(sum0);
#elif defined(__AVX_SUPPORT__)
    __m256 sum0 = _mm256_setzero_ps();
    __m256 sum1 = _mm256_setzero_ps();
    __m256 sum2 = _mm256_setzero_ps();
    __m256 sum3 = _mm256_setzero_ps();
    // Unroll loop by 4 for better instruction-level parallelism
    for (; i + 32 <= dim; i += 32) {
        // Load 32 uint8 values and convert to float (4 iterations)
        __m128i codes_u8_0 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(&compact_code[i]));
        __m128i codes_u8_1 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(&compact_code[i + 8]));
        __m128i codes_u8_2 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(&compact_code[i + 16]));
        __m128i codes_u8_3 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(&compact_code[i + 24]));
        __m256i codes_u32_0 = _mm256_cvtepu8_epi32(codes_u8_0);
        __m256i codes_u32_1 = _mm256_cvtepu8_epi32(codes_u8_1);
        __m256i codes_u32_2 = _mm256_cvtepu8_epi32(codes_u8_2);
        __m256i codes_u32_3 = _mm256_cvtepu8_epi32(codes_u8_3);
        __m256 codes_f32_0 = _mm256_cvtepi32_ps(codes_u32_0);
        __m256 codes_f32_1 = _mm256_cvtepi32_ps(codes_u32_1);
        __m256 codes_f32_2 = _mm256_cvtepi32_ps(codes_u32_2);
        __m256 codes_f32_3 = _mm256_cvtepi32_ps(codes_u32_3);

        // Load query values
        __m256 query_vec_0 = _mm256_loadu_ps(&query[i]);
        __m256 query_vec_1 = _mm256_loadu_ps(&query[i + 8]);
        __m256 query_vec_2 = _mm256_loadu_ps(&query[i + 16]);
        __m256 query_vec_3 = _mm256_loadu_ps(&query[i + 24]);

        // Multiply and accumulate
#ifdef __FMA__
        sum0 = _mm256_fmadd_ps(query_vec_0, codes_f32_0, sum0);
        sum1 = _mm256_fmadd_ps(query_vec_1, codes_f32_1, sum1);
        sum2 = _mm256_fmadd_ps(query_vec_2, codes_f32_2, sum2);
        sum3 = _mm256_fmadd_ps(query_vec_3, codes_f32_3, sum3);
#else
        sum0 = _mm256_add_ps(sum0, _mm256_mul_ps(query_vec_0, codes_f32_0));
        sum1 = _mm256_add_ps(sum1, _mm256_mul_ps(query_vec_1, codes_f32_1));
        sum2 = _mm256_add_ps(sum2, _mm256_mul_ps(query_vec_2, codes_f32_2));
        sum3 = _mm256_add_ps(sum3, _mm256_mul_ps(query_vec_3, codes_f32_3));
#endif
    }
    sum0 = _mm256_add_ps(_mm256_add_ps(sum0, sum1), _mm256_add_ps(sum2, sum3));

    // Handle remainder in chunks of 8
    for (; i + 8 <= dim; i += 8) {
        __m128i codes_u8 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(&compact_code[i]));
        __m256i codes_u32 = _mm256_cvtepu8_epi32(codes_u8);
        __m256 codes_f32 = _mm256_cvtepi32_ps(codes_u32);
        __m256 query_vec = _mm256_loadu_ps(&query[i]);
#ifdef __FMA__
        sum0 = _mm256_fmadd_ps(query_vec, codes_f32, sum0);
#else
        sum0 = _mm256_add_ps(sum0, _mm256_mul_ps(query_vec, codes_f32));
#endif
    }
    // Horizontal sum - avoid slow hadd instruction
    __m128 sum128 = _mm_add_ps(_mm256_castps256_ps128(sum0), _mm256_extractf128_ps(sum0, 1));
    __m128 shuf = _mm_movehdup_ps(sum128);        // [1, 1, 3, 3]
    sum128 = _mm_add_ps(sum128, shuf);            // [0+1, 1+1, 2+3, 3+3]
    shuf = _mm_movehl_ps(shuf, sum128);           // [2+3, 3+3, ?, ?]
    sum128 = _mm_add_ss(sum128, shuf);            // [0+1+2+3, ?, ?, ?]
    result = _mm_cvtss_f32(sum128);
#elif defined(__SSE_SUPPORT__)
    __m128 sum0 = _mm_setzero_ps();
    __m128 sum1 = _mm_setzero_ps();
    __m128 sum2 = _mm_setzero_ps();
    __m128 sum3 = _mm_setzero_ps();
    // Unroll loop by 4 for better instruction-level parallelism
    for (; i + 16 <= dim; i += 16) {
        // Load 16 uint8 values and convert to float (4 iterations)
        // SSE2 doesn't have direct u8->u32 conversion, so we need to do it in steps
        __m128i codes_u8_01 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(&compact_code[i]));
        __m128i codes_u8_23 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(&compact_code[i + 8]));

        // Zero extend u8 to u16
        __m128i codes_u16_01 = _mm_unpacklo_epi8(codes_u8_01, _mm_setzero_si128());
        __m128i codes_u16_23 = _mm_unpacklo_epi8(codes_u8_23, _mm_setzero_si128());

        // Split into 4 groups of 4 elements and zero extend u16 to u32
        __m128i codes_u32_0 = _mm_unpacklo_epi16(codes_u16_01, _mm_setzero_si128());
        __m128i codes_u32_1 = _mm_unpackhi_epi16(codes_u16_01, _mm_setzero_si128());
        __m128i codes_u32_2 = _mm_unpacklo_epi16(codes_u16_23, _mm_setzero_si128());
        __m128i codes_u32_3 = _mm_unpackhi_epi16(codes_u16_23, _mm_setzero_si128());

        // Convert u32 to float
        __m128 codes_f32_0 = _mm_cvtepi32_ps(codes_u32_0);
        __m128 codes_f32_1 = _mm_cvtepi32_ps(codes_u32_1);
        __m128 codes_f32_2 = _mm_cvtepi32_ps(codes_u32_2);
        __m128 codes_f32_3 = _mm_cvtepi32_ps(codes_u32_3);

        // Load query values
        __m128 query_vec_0 = _mm_loadu_ps(&query[i]);
        __m128 query_vec_1 = _mm_loadu_ps(&query[i + 4]);
        __m128 query_vec_2 = _mm_loadu_ps(&query[i + 8]);
        __m128 query_vec_3 = _mm_loadu_ps(&query[i + 12]);

        // Multiply and accumulate
        sum0 = _mm_add_ps(sum0, _mm_mul_ps(query_vec_0, codes_f32_0));
        sum1 = _mm_add_ps(sum1, _mm_mul_ps(query_vec_1, codes_f32_1));
        sum2 = _mm_add_ps(sum2, _mm_mul_ps(query_vec_2, codes_f32_2));
        sum3 = _mm_add_ps(sum3, _mm_mul_ps(query_vec_3, codes_f32_3));
    }
    sum0 = _mm_add_ps(_mm_add_ps(sum0, sum1), _mm_add_ps(sum2, sum3));

    // Handle remainder in chunks of 4
    for (; i + 4 <= dim; i += 4) {
        // Safe loading of 4 bytes without alignment assumptions
        uint32_t temp;
        memcpy(&temp, &compact_code[i], sizeof(temp));
        __m128i codes_u8 = _mm_cvtsi32_si128(temp);
        __m128i codes_u16 = _mm_unpacklo_epi8(codes_u8, _mm_setzero_si128());
        __m128i codes_u32 = _mm_unpacklo_epi16(codes_u16, _mm_setzero_si128());
        __m128 codes_f32 = _mm_cvtepi32_ps(codes_u32);
        __m128 query_vec = _mm_loadu_ps(&query[i]);
        sum0 = _mm_add_ps(sum0, _mm_mul_ps(query_vec, codes_f32));
    }
    // Horizontal sum - avoid slow hadd instruction
    __m128 shuf = _mm_movehdup_ps(sum0);     // [1, 1, 3, 3]
    sum0 = _mm_add_ps(sum0, shuf);           // [0+1, 1+1, 2+3, 3+3]
    shuf = _mm_movehl_ps(shuf, sum0);        // [2+3, 3+3, ?, ?]
    sum0 = _mm_add_ss(sum0, shuf);           // [0+1+2+3, ?, ?, ?]
    result = _mm_cvtss_f32(sum0);
#elif defined(__NEON_SUPPORT__)
    float32x4_t sum0 = vdupq_n_f32(0.0f);
    float32x4_t sum1 = vdupq_n_f32(0.0f);
    float32x4_t sum2 = vdupq_n_f32(0.0f);
    float32x4_t sum3 = vdupq_n_f32(0.0f);
    // Unroll loop by 4 for better instruction-level parallelism
    for (; i + 16 <= dim; i += 16) {
        // Load 16 uint8 values
        uint8x16_t codes_u8 = vld1q_u8(&compact_code[i]);

        // Split into 2 groups of 8
        uint8x8_t codes_u8_low = vget_low_u8(codes_u8);
        uint8x8_t codes_u8_high = vget_high_u8(codes_u8);

        // Widen u8 to u16
        uint16x8_t codes_u16_0 = vmovl_u8(codes_u8_low);
        uint16x8_t codes_u16_1 = vmovl_u8(codes_u8_high);

        // Widen u16 to u32
        uint32x4_t codes_u32_0 = vmovl_u16(vget_low_u16(codes_u16_0));
        uint32x4_t codes_u32_1 = vmovl_u16(vget_high_u16(codes_u16_0));
        uint32x4_t codes_u32_2 = vmovl_u16(vget_low_u16(codes_u16_1));
        uint32x4_t codes_u32_3 = vmovl_u16(vget_high_u16(codes_u16_1));

        // Convert u32 to float
        float32x4_t codes_f32_0 = vcvtq_f32_u32(codes_u32_0);
        float32x4_t codes_f32_1 = vcvtq_f32_u32(codes_u32_1);
        float32x4_t codes_f32_2 = vcvtq_f32_u32(codes_u32_2);
        float32x4_t codes_f32_3 = vcvtq_f32_u32(codes_u32_3);

        // Load query values
        float32x4_t query_vec_0 = vld1q_f32(&query[i]);
        float32x4_t query_vec_1 = vld1q_f32(&query[i + 4]);
        float32x4_t query_vec_2 = vld1q_f32(&query[i + 8]);
        float32x4_t query_vec_3 = vld1q_f32(&query[i + 12]);

        // Multiply and accumulate
        sum0 = vmlaq_f32(sum0, query_vec_0, codes_f32_0);
        sum1 = vmlaq_f32(sum1, query_vec_1, codes_f32_1);
        sum2 = vmlaq_f32(sum2, query_vec_2, codes_f32_2);
        sum3 = vmlaq_f32(sum3, query_vec_3, codes_f32_3);
    }
    sum0 = vaddq_f32(vaddq_f32(sum0, sum1), vaddq_f32(sum2, sum3));

    // Handle remainder in chunks of 4
    for (; i + 4 <= dim; i += 4) {
        // Safe loading of exactly 4 bytes - create a zero-initialized 8-byte vector
        uint8_t temp[8] = {0};
        memcpy(temp, &compact_code[i], 4);
        uint8x8_t codes_u8 = vld1_u8(temp);
        uint16x4_t codes_u16 = vget_low_u16(vmovl_u8(codes_u8));
        uint32x4_t codes_u32 = vmovl_u16(codes_u16);
        float32x4_t codes_f32 = vcvtq_f32_u32(codes_u32);
        float32x4_t query_vec = vld1q_f32(&query[i]);
        sum0 = vmlaq_f32(sum0, query_vec, codes_f32);
    }
    // Horizontal sum
    float32x2_t sum_low = vget_low_f32(sum0);
    float32x2_t sum_high = vget_high_f32(sum0);
    float32x2_t sum_pair = vadd_f32(sum_low, sum_high);
    result = vget_lane_f32(vpadd_f32(sum_pair, sum_pair), 0);
#elif defined(__SVE2_SUPPORT__) || defined(__SVE_SUPPORT__)
    svfloat32_t sum0 = svdup_f32(0.0f);
    svfloat32_t sum1 = svdup_f32(0.0f);
    svfloat32_t sum2 = svdup_f32(0.0f);
    svfloat32_t sum3 = svdup_f32(0.0f);
    const size_t sve_len = svcntw();
    svbool_t pg_all = svptrue_b32();
    svbool_t pg_byte = svptrue_b8();
    svbool_t pg_word = svptrue_b32();

    // Efficient SVE: Load full u8 vector, use lo/hi unpacking to process all elements
    // Full u8 vector = 4*sve_len bytes, unpacks to 4*sve_len u32 words via lo/hi combinations
    // For 256-bit SVE: 32 bytes -> {lo,hi} u16 -> {lo,hi} u32 each = 4×8 = 32 elements
    const size_t elems_per_iteration = sve_len * 4;

    for (; i + elems_per_iteration <= dim; i += elems_per_iteration) {
        // Load 4*sve_len bytes (full u8 vector width)
        svuint8_t codes_u8 = svld1_u8(pg_byte, &compact_code[i]);

        // First level: unpack u8 -> u16 (lo and hi)
        svuint16_t codes_u16_lo = svunpklo_u16(codes_u8);
        svuint16_t codes_u16_hi = svunpkhi_u16(codes_u8);

        // Second level: unpack u16 -> u32 (lo and hi of each)
        svuint32_t codes_u32_0 = svunpklo_u32(codes_u16_lo);  // First sve_len elements
        svuint32_t codes_u32_1 = svunpkhi_u32(codes_u16_lo);  // Second sve_len elements
        svuint32_t codes_u32_2 = svunpklo_u32(codes_u16_hi);  // Third sve_len elements
        svuint32_t codes_u32_3 = svunpkhi_u32(codes_u16_hi);  // Fourth sve_len elements

        // Convert to float
        svfloat32_t codes_f32_0 = svcvt_f32_u32_z(pg_word, codes_u32_0);
        svfloat32_t codes_f32_1 = svcvt_f32_u32_z(pg_word, codes_u32_1);
        svfloat32_t codes_f32_2 = svcvt_f32_u32_z(pg_word, codes_u32_2);
        svfloat32_t codes_f32_3 = svcvt_f32_u32_z(pg_word, codes_u32_3);

        // Load corresponding query values
        svfloat32_t query_vec_0 = svld1_f32(pg_word, &query[i]);
        svfloat32_t query_vec_1 = svld1_f32(pg_word, &query[i + sve_len]);
        svfloat32_t query_vec_2 = svld1_f32(pg_word, &query[i + sve_len * 2]);
        svfloat32_t query_vec_3 = svld1_f32(pg_word, &query[i + sve_len * 3]);

        // Multiply and accumulate
        sum0 = svmla_f32_m(pg_word, sum0, query_vec_0, codes_f32_0);
        sum1 = svmla_f32_m(pg_word, sum1, query_vec_1, codes_f32_1);
        sum2 = svmla_f32_m(pg_word, sum2, query_vec_2, codes_f32_2);
        sum3 = svmla_f32_m(pg_word, sum3, query_vec_3, codes_f32_3);
    }

    // Remaining elements (< sve_len*4) handled by scalar loop
    // Note: We can't efficiently process partial SVE vectors with u8->u32 unpacking
    // because unpacking requires specific vector sizes. Scalar fallback is simpler and correct.

    // Reduce all accumulators with balanced tree
    sum0 = svadd_f32_z(pg_all, sum0, sum1);
    sum2 = svadd_f32_z(pg_all, sum2, sum3);
    sum0 = svadd_f32_z(pg_all, sum0, sum2);
    result = svaddv_f32(pg_all, sum0);
#endif

    // Handle remaining elements (scalar fallback)
    for (; i < dim; ++i) {
        result += query[i] * static_cast<float>(compact_code[i]);
    }
    return result;
}

float DISTANCE_FUNC_NAME(mask_ip_x0_q)(float *query, uint64 *data, size_t dim)
{
    const size_t num_blk = dim / 64;
#ifdef __AVX512_SUPPORT__
    const uint64 *it_data = data;
    const float *it_query = query;

    __m512 sum0 = _mm512_setzero_ps();
    __m512 sum1 = _mm512_setzero_ps();
    __m512 sum2 = _mm512_setzero_ps();
    __m512 sum3 = _mm512_setzero_ps();

    for (size_t i = 0; i < num_blk; ++i) {
        uint64 bits = reverse_bits_u64(*it_data);
        __mmask16 mask0 = static_cast<__mmask16>(bits);
        __mmask16 mask1 = static_cast<__mmask16>(bits >> 16);
        __mmask16 mask2 = static_cast<__mmask16>(bits >> 32);
        __mmask16 mask3 = static_cast<__mmask16>(bits >> 48);
        const __m512 q0 = _mm512_loadu_ps(it_query);
        const __m512 q1 = _mm512_loadu_ps(it_query + 16);
        const __m512 q2 = _mm512_loadu_ps(it_query + 32);
        const __m512 q3 = _mm512_loadu_ps(it_query + 48);
        sum0 = _mm512_mask_add_ps(sum0, mask0, sum0, q0);
        sum1 = _mm512_mask_add_ps(sum1, mask1, sum1, q1);
        sum2 = _mm512_mask_add_ps(sum2, mask2, sum2, q2);
        sum3 = _mm512_mask_add_ps(sum3, mask3, sum3, q3);
        ++it_data;
        it_query += 64;
    }
    __m512 sum = _mm512_add_ps(_mm512_add_ps(sum0, sum1), _mm512_add_ps(sum2, sum3));
    return _mm512_reduce_add_ps(sum);
#elif defined(__AVX_SUPPORT__)
    alignas(32) static constexpr auto mask_table = generate_avx_mask_table();
    __m256 acc = _mm256_setzero_ps();
    for (size_t i = 0; i < num_blk; ++i) {
        uint64 bits = reverse_bits_u64(data[i]);
        for (int j = 0; j < 64; j += 8) {
            uint8 mask = (bits >> j) & 0xFF;
            if (mask != 0) {
                __m256 mask_vec = _mm256_load_ps(mask_table[mask].data());
                __m256 v = _mm256_loadu_ps(&query[i * 64 + j]);
                acc = _mm256_add_ps(acc, _mm256_mul_ps(v, mask_vec));
            }
        }
    }
    __m128 sum128 = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));
    sum128 = _mm_hadd_ps(sum128, sum128);
    sum128 = _mm_hadd_ps(sum128, sum128);
    return _mm_cvtss_f32(sum128);
#elif defined(__NEON_SUPPORT__)
    static constexpr auto mask4_table = generate_neon_mask4_table();
    float32x4_t acc = vdupq_n_f32(0.0f);
    for (size_t i = 0; i < num_blk; ++i) {
        uint64 bits = reverse_bits_u64(data[i]);
        const float *q_ptr = &query[i * 64];
        // Process 64 bits in 8-bit chunks; each 8-bit chunk becomes two 4-lane masks
        for (int j = 0; j < 64; j += 8) {
            uint8 mask8 = static_cast<uint8>((bits >> j) & 0xFFu);
            if (!mask8) {
                continue;
            }
            uint8 mask_low = mask8 & 0x0Fu;        // bits 0..3
            uint8 mask_high = (mask8 >> 4) & 0x0Fu; // bits 4..7

            // Lower 4 elements
            if (mask_low) {
                float32x4_t mask_vec0 = vld1q_f32(mask4_table[mask_low].data());
                float32x4_t v0 = vld1q_f32(q_ptr + j);
                acc = vmlaq_f32(acc, v0, mask_vec0);
            }
            // Upper 4 elements
            if (mask_high) {
                float32x4_t mask_vec1 = vld1q_f32(mask4_table[mask_high].data());
                float32x4_t v1 = vld1q_f32(q_ptr + j + 4);
                acc = vmlaq_f32(acc, v1, mask_vec1);
            }
        }
    }
    // Horizontal add of acc
    float32x2_t acc_low = vget_low_f32(acc);
    float32x2_t acc_high = vget_high_f32(acc);
    float32x2_t sum_pair = vadd_f32(acc_low, acc_high);
    return vget_lane_f32(vpadd_f32(sum_pair, sum_pair), 0);
#elif defined(__SVE2_SUPPORT__)
    svfloat32_t acc = svdup_f32(0.0f);
    svbool_t all_true = svptrue_b32();
    const size_t sve_vec_len = svcntw();
    for (size_t i = 0; i < num_blk; ++i) {
        uint64 bits = reverse_bits_u64(data[i]);
        svuint64_t bit_vector = svdup_u64(bits);
        for (size_t j = 0; j < 64; j += sve_vec_len) {
            svbool_t pg = svwhilelt_b32((int32)j, (int32)64);
            svuint32_t indices = svindex_u32(j, 1);
            svuint64_t indices_64 = svreinterpret_u64_u32(indices);
            svuint64_t bit_positions = svlsl_x(pg, svdup_u64(1), indices_64);
            svbool_t active_mask = svcmpeq(pg, svand_x(pg, bit_vector, bit_positions), bit_positions);
            if (svptest_any(pg, active_mask)) {
                svfloat32_t data_vec = svld1(pg, &query[i * 64 + j]);
                acc = svadd_m(active_mask, acc, data_vec);
            }
        }
    }
    return svaddv(all_true, acc);
#elif defined(__SVE_SUPPORT__)
    svfloat32_t acc = svdup_f32(0.0f);
    svbool_t all_true = svptrue_b32();
    const size_t sve_vec_len = svcntw();
    for (size_t i = 0; i < num_blk; ++i) {
        uint64 bits = reverse_bits_u64(data[i]);
        for (size_t j = 0; j < 64; j += sve_vec_len) {
            svbool_t pg = svwhilelt_b32(j, (size_t)64);
            uint32_t num_bits_to_take = svcntp_b32(all_true, pg);
            uint32_t mask = (bits >> j) & ((1UL << num_bits_to_take) - 1);
            if (mask != 0) {
                svfloat32_t data_vec = svld1(pg, &query[i * 64 + j]);
                svuint32_t index_vec = svindex_u32(0, 1);
                svuint32_t mask_vec = svdup_u32(mask);
                svuint32_t shifted = svlsr_x(pg, mask_vec, index_vec);
                svbool_t select_mask = svcmpne(pg, svand_x(pg, shifted, 1), 0);
                acc = svadd_m(select_mask, acc, data_vec);
            }
        }
    }
    return svaddv(all_true, acc);
#else
    float sum = 0.0f;
    for (size_t i = 0; i < num_blk; ++i) {
        uint64 bits = reverse_bits_u64(data[i]);
        for (int j = 0; j < 64; ++j) {
            if (bits & (1ul << j)) {
                sum += query[i * 64 + j];
            }
        }
    }
    return sum;
#endif
}
}   /* namespace ann_helper */
