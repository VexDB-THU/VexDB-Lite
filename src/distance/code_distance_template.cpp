#ifndef DISTANCE_FUNC_NAME
static_assert(false, "don't use the file without definition DISTANCE_FUNC_NAME");
#endif

#include "distance/pq/pq_endecode.h"


/// Returns the distance to a single code.
template <typename PQDecoderT>
float distance_single_code_generic(
        // number of subquantizers
        uint32 M,
        // number of bits per quantization index
        uint32 nbits,
        // precomputed distances, layout (M, ksub)
        const float* sim_table,//sim_table is combination of query x with all codes in one list;
        // the code
        const uint8* code) {
    PQDecoderT decoder(code, nbits);
    const uint32 ksub = 1 << nbits;

    const float* tab = sim_table;
    float result = 0;
    
    for (uint32 m = 0; m < M; m++) {
        result += tab[decoder.decode()];
        tab += ksub;
    }

    return result;
}

/// Combines 4 operations of distance_single_code()
/// General-purpose version.
template <typename PQDecoderT>
void distance_four_codes_generic(
        // number of subquantizers
        uint32 M,
        // number of bits per quantization index
        uint32 nbits,
        // precomputed distances, layout (M, ksub)
        const float* sim_table,
        // codes
        const uint8* __restrict code0,
        const uint8* __restrict code1,
        const uint8* __restrict code2,
        const uint8* __restrict code3,
        // computed distances
        float& result0,
        float& result1,
        float& result2,
        float& result3) {
    PQDecoderT decoder0(code0, nbits);
    PQDecoderT decoder1(code1, nbits);
    PQDecoderT decoder2(code2, nbits);
    PQDecoderT decoder3(code3, nbits);
    const uint32 ksub = 1 << nbits;

    const float* tab = sim_table;
    result0 = 0;
    result1 = 0;
    result2 = 0;
    result3 = 0;

    for (uint32 m = 0; m < M; m++) {
        result0 += tab[decoder0.decode()];
        result1 += tab[decoder1.decode()];
        result2 += tab[decoder2.decode()];
        result3 += tab[decoder3.decode()];
        tab += ksub;
    }
}

// This directory contains functions to compute a distance
// from a given PQ code to a query vector, given that the
// distances to a query vector for pq.M codebooks are precomputed.
//
// The code was originally the part of IndexIVFPQ.cpp.
// The baseline implementation can be found in
//   code_distance-generic.h, distance_single_code_generic().

// The reason for this somewhat unusual structure is that
// custom implementations may need to fall off to generic
// implementation in certain cases. So, say, avx2 header file
// needs to reference the generic header file. This is
// why the names of the functions for custom implementations
// have this _generic or _avx2 suffix.

#ifdef __AVX_SUPPORT__
#include <immintrin.h>
#include <type_traits>
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=78782
#if ANN_COMPILER_GCC && ANN_GCC_MAJOR < 9
#define _mm_loadu_si64(x) (_mm_loadl_epi64((__m128i_u*)x))
#endif

namespace {
#include "distance/pq/horizontal_sum128.h"
#include "distance/pq/horizontal_sum256.h"

// processes a single code for M=4, ksub=256, nbits=8
float distance_single_code_avx2_pqdecoder8_m4(
        // precomputed distances, layout (4, 256)
        const float* sim_table,
        const uint8* code) {
    float result = 0;

    const float* tab = sim_table;
    constexpr uint32 ksub = 1 << 8;

    const __m128i vksub = _mm_set1_epi32(ksub);
    __m128i offsets_0 = _mm_setr_epi32(0, 1, 2, 3);
    offsets_0 = _mm_mullo_epi32(offsets_0, vksub);

    // accumulators of partial sums
    __m128 partialSum;

    // load 4 uint8 values
    const __m128i mm1 = _mm_cvtsi32_si128(*((const int32_t*)code));
    {
        // convert uint8 values (low part of __m128i) to int32
        // values
        const __m128i idx1 = _mm_cvtepu8_epi32(mm1);

        // add offsets
        const __m128i indices_to_read_from = _mm_add_epi32(idx1, offsets_0);

        // gather 8 values, similar to 8 operations of tab[idx]
        __m128 collected =
                _mm_i32gather_ps(tab, indices_to_read_from, sizeof(float));

        // collect partial sums
        partialSum = collected;
    }

    // horizontal sum for partialSum
    result = horizontal_sum(partialSum);
    return result;
}

// processes a single code for M=8, ksub=256, nbits=8
float distance_single_code_avx2_pqdecoder8_m8(
        // precomputed distances, layout (8, 256)
        const float* sim_table,
        const uint8* code) {
    float result = 0;

    const float* tab = sim_table;
    constexpr uint32 ksub = 1 << 8;

    const __m256i vksub = _mm256_set1_epi32(ksub);
    __m256i offsets_0 = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    offsets_0 = _mm256_mullo_epi32(offsets_0, vksub);

    // accumulators of partial sums
    __m256 partialSum;

    // load 8 uint8 values
    const __m128i mm1 = _mm_loadu_si64((const __m128i_u*)code);
    {
        // convert uint8 values (low part of __m128i) to int32
        // values
        const __m256i idx1 = _mm256_cvtepu8_epi32(mm1);

        // add offsets
        const __m256i indices_to_read_from = _mm256_add_epi32(idx1, offsets_0);

        // gather 8 values, similar to 8 operations of tab[idx]
        __m256 collected =
                _mm256_i32gather_ps(tab, indices_to_read_from, sizeof(float));

        // collect partial sums
        partialSum = collected;
    }

    // horizontal sum for partialSum
    result = horizontal_sum(partialSum);
    return result;
}

// processes four codes for M=4, ksub=256, nbits=8
void distance_four_codes_avx2_pqdecoder8_m4(
        // precomputed distances, layout (4, 256)
        const float* sim_table,
        // codes
        const uint8* __restrict code0,
        const uint8* __restrict code1,
        const uint8* __restrict code2,
        const uint8* __restrict code3,
        // computed distances
        float& result0,
        float& result1,
        float& result2,
        float& result3) {
    constexpr intptr_t N = 4;

    const float* tab = sim_table;
    constexpr uint32 ksub = 1 << 8;

    // process 8 values
    const __m128i vksub = _mm_set1_epi32(ksub);
    __m128i offsets_0 = _mm_setr_epi32(0, 1, 2, 3);
    offsets_0 = _mm_mullo_epi32(offsets_0, vksub);

    // accumulators of partial sums
    __m128 partialSums[N];

    // load 4 uint8 values
    __m128i mm1[N];
    mm1[0] = _mm_cvtsi32_si128(*((const int32_t*)code0));
    mm1[1] = _mm_cvtsi32_si128(*((const int32_t*)code1));
    mm1[2] = _mm_cvtsi32_si128(*((const int32_t*)code2));
    mm1[3] = _mm_cvtsi32_si128(*((const int32_t*)code3));

    for (intptr_t j = 0; j < N; j++) {
        // convert uint8 values (low part of __m128i) to int32
        // values
        const __m128i idx1 = _mm_cvtepu8_epi32(mm1[j]);

        // add offsets
        const __m128i indices_to_read_from = _mm_add_epi32(idx1, offsets_0);

        // gather 4 values, similar to 4 operations of tab[idx]
        __m128 collected =
                _mm_i32gather_ps(tab, indices_to_read_from, sizeof(float));

        // collect partial sums
        partialSums[j] = collected;
    }

    // horizontal sum for partialSum
    result0 = horizontal_sum(partialSums[0]);
    result1 = horizontal_sum(partialSums[1]);
    result2 = horizontal_sum(partialSums[2]);
    result3 = horizontal_sum(partialSums[3]);
}

// processes four codes for M=8, ksub=256, nbits=8
void distance_four_codes_avx2_pqdecoder8_m8(
        // precomputed distances, layout (8, 256)
        const float* sim_table,
        // codes
        const uint8* __restrict code0,
        const uint8* __restrict code1,
        const uint8* __restrict code2,
        const uint8* __restrict code3,
        // computed distances
        float& result0,
        float& result1,
        float& result2,
        float& result3) {
    constexpr intptr_t N = 4;

    const float* tab = sim_table;
    constexpr uint32 ksub = 1 << 8;

    // process 8 values
    const __m256i vksub = _mm256_set1_epi32(ksub);
    __m256i offsets_0 = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    offsets_0 = _mm256_mullo_epi32(offsets_0, vksub);

    // accumulators of partial sums
    __m256 partialSums[N];

    // load 8 uint8 values
    __m128i mm1[N];
    mm1[0] = _mm_loadu_si64((const __m128i_u*)code0);
    mm1[1] = _mm_loadu_si64((const __m128i_u*)code1);
    mm1[2] = _mm_loadu_si64((const __m128i_u*)code2);
    mm1[3] = _mm_loadu_si64((const __m128i_u*)code3);

    for (intptr_t j = 0; j < N; j++) {
        // convert uint8 values (low part of __m128i) to int32
        // values
        const __m256i idx1 = _mm256_cvtepu8_epi32(mm1[j]);

        // add offsets
        const __m256i indices_to_read_from = _mm256_add_epi32(idx1, offsets_0);

        // gather 8 values, similar to 8 operations of tab[idx]
        __m256 collected =
                _mm256_i32gather_ps(tab, indices_to_read_from, sizeof(float));

        // collect partial sums
        partialSums[j] = collected;
    }

    // horizontal sum for partialSum
    result0 = horizontal_sum(partialSums[0]);
    result1 = horizontal_sum(partialSums[1]);
    result2 = horizontal_sum(partialSums[2]);
    result3 = horizontal_sum(partialSums[3]);
}

} // namespace

template <typename PQDecoderT>
typename std::enable_if<!std::is_same<PQDecoderT, PQDecoder8>::value, float>::
        type distance_single_code_avx2(
                // number of subquantizers
                uint32 M,
                // number of bits per quantization index
                uint32 nbits,
                // precomputed distances, layout (M, ksub)
                const float* sim_table,
                const uint8* code) {
    // default implementation
    return distance_single_code_generic<PQDecoderT>(M, nbits, sim_table, code);
}

template <typename PQDecoderT>
typename std::enable_if<std::is_same<PQDecoderT, PQDecoder8>::value, float>::
        type distance_single_code_avx2(
                // number of subquantizers
                uint32 M,
                // number of bits per quantization index
                uint32 nbits,
                // precomputed distances, layout (M, ksub)
                const float* sim_table,
                const uint8* code) {
    if (M == 4) {
        return distance_single_code_avx2_pqdecoder8_m4(sim_table, code);
    }
    if (M == 8) {
        return distance_single_code_avx2_pqdecoder8_m8(sim_table, code);
    }

    float result = 0;
    constexpr uint32 ksub = 1 << 8;

    uint32 m = 0;
    const uint32 pqM16 = M / 16;

    const float* tab = sim_table;

    if (pqM16 > 0) {
        // process 16 values per loop

        const __m256i vksub = _mm256_set1_epi32(ksub);
        __m256i offsets_0 = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
        offsets_0 = _mm256_mullo_epi32(offsets_0, vksub);

        // accumulators of partial sums
        __m256 partialSum = _mm256_setzero_ps();

        // loop
        for (m = 0; m < pqM16 * 16; m += 16) {
            // load 16 uint8 values
            const __m128i mm1 = _mm_loadu_si128((const __m128i_u*)(code + m));
            {
                // convert uint8 values (low part of __m128i) to int32
                // values
                const __m256i idx1 = _mm256_cvtepu8_epi32(mm1);

                // add offsets
                const __m256i indices_to_read_from =
                        _mm256_add_epi32(idx1, offsets_0);

                // gather 8 values, similar to 8 operations of tab[idx]
                __m256 collected = _mm256_i32gather_ps(
                        tab, indices_to_read_from, sizeof(float));
                tab += ksub * 8;

                // collect partial sums
                partialSum = _mm256_add_ps(partialSum, collected);
            }

            // move high 8 uint8 to low ones
            const __m128i mm2 = _mm_unpackhi_epi64(mm1, _mm_setzero_si128());
            {
                // convert uint8 values (low part of __m128i) to int32
                // values
                const __m256i idx1 = _mm256_cvtepu8_epi32(mm2);

                // add offsets
                const __m256i indices_to_read_from =
                        _mm256_add_epi32(idx1, offsets_0);

                // gather 8 values, similar to 8 operations of tab[idx]
                __m256 collected = _mm256_i32gather_ps(
                        tab, indices_to_read_from, sizeof(float));
                tab += ksub * 8;

                // collect partial sums
                partialSum = _mm256_add_ps(partialSum, collected);
            }
        }

        // horizontal sum for partialSum
        result += horizontal_sum(partialSum);
    }

    //
    if (m < M) {
        // process leftovers
        PQDecoder8 decoder(code + m, nbits);

        for (; m < M; m++) {
            result += tab[decoder.decode()];
            tab += ksub;
        }
    }

    return result;
}

template <typename PQDecoderT>
typename std::enable_if<!std::is_same<PQDecoderT, PQDecoder8>::value, void>::
        type
        distance_four_codes_avx2(
                // number of subquantizers
                uint32 M,
                // number of bits per quantization index
                uint32 nbits,
                // precomputed distances, layout (M, ksub)
                const float* sim_table,
                // codes
                const uint8* __restrict code0,
                const uint8* __restrict code1,
                const uint8* __restrict code2,
                const uint8* __restrict code3,
                // computed distances
                float& result0,
                float& result1,
                float& result2,
                float& result3) {
    distance_four_codes_generic<PQDecoderT>(
            M,
            nbits,
            sim_table,
            code0,
            code1,
            code2,
            code3,
            result0,
            result1,
            result2,
            result3);
}

// Combines 4 operations of distance_single_code()
template <typename PQDecoderT>
typename std::enable_if<std::is_same<PQDecoderT, PQDecoder8>::value, void>::type
distance_four_codes_avx2(
        // number of subquantizers
        uint32 M,
        // number of bits per quantization index
        uint32 nbits,
        // precomputed distances, layout (M, ksub)
        const float* sim_table,
        // codes
        const uint8* __restrict code0,
        const uint8* __restrict code1,
        const uint8* __restrict code2,
        const uint8* __restrict code3,
        // computed distances
        float& result0,
        float& result1,
        float& result2,
        float& result3) {
    if (M == 4) {
        distance_four_codes_avx2_pqdecoder8_m4(
                sim_table,
                code0,
                code1,
                code2,
                code3,
                result0,
                result1,
                result2,
                result3);
        return;
    }
    if (M == 8) {
        distance_four_codes_avx2_pqdecoder8_m8(
                sim_table,
                code0,
                code1,
                code2,
                code3,
                result0,
                result1,
                result2,
                result3);
        return;
    }

    result0 = 0;
    result1 = 0;
    result2 = 0;
    result3 = 0;
    constexpr uint32 ksub = 1 << 8;

    uint32 m = 0;
    const uint32 pqM16 = M / 16;

    constexpr intptr_t N = 4;

    const float* tab = sim_table;

    if (pqM16 > 0) {
        // process 16 values per loop
        const __m256i vksub = _mm256_set1_epi32(ksub);
        __m256i offsets_0 = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
        offsets_0 = _mm256_mullo_epi32(offsets_0, vksub);

        // accumulators of partial sums
        __m256 partialSums[N];
        for (intptr_t j = 0; j < N; j++) {
            partialSums[j] = _mm256_setzero_ps();
        }

        // loop
        for (m = 0; m < pqM16 * 16; m += 16) {
            // load 16 uint8 values
            __m128i mm1[N];
            mm1[0] = _mm_loadu_si128((const __m128i_u*)(code0 + m));
            mm1[1] = _mm_loadu_si128((const __m128i_u*)(code1 + m));
            mm1[2] = _mm_loadu_si128((const __m128i_u*)(code2 + m));
            mm1[3] = _mm_loadu_si128((const __m128i_u*)(code3 + m));

            // process first 8 codes
            for (intptr_t j = 0; j < N; j++) {
                // convert uint8 values (low part of __m128i) to int32
                // values
                const __m256i idx1 = _mm256_cvtepu8_epi32(mm1[j]);

                // add offsets
                const __m256i indices_to_read_from =
                        _mm256_add_epi32(idx1, offsets_0);

                // gather 8 values, similar to 8 operations of tab[idx]
                __m256 collected = _mm256_i32gather_ps(
                        tab, indices_to_read_from, sizeof(float));

                // collect partial sums
                partialSums[j] = _mm256_add_ps(partialSums[j], collected);
            }
            tab += ksub * 8;

            // process next 8 codes
            for (intptr_t j = 0; j < N; j++) {
                // move high 8 uint8 to low ones
                const __m128i mm2 =
                        _mm_unpackhi_epi64(mm1[j], _mm_setzero_si128());

                // convert uint8 values (low part of __m128i) to int32
                // values
                const __m256i idx1 = _mm256_cvtepu8_epi32(mm2);

                // add offsets
                const __m256i indices_to_read_from =
                        _mm256_add_epi32(idx1, offsets_0);

                // gather 8 values, similar to 8 operations of tab[idx]
                __m256 collected = _mm256_i32gather_ps(
                        tab, indices_to_read_from, sizeof(float));

                // collect partial sums
                partialSums[j] = _mm256_add_ps(partialSums[j], collected);
            }

            tab += ksub * 8;
        }

        // horizontal sum for partialSum
        result0 += horizontal_sum(partialSums[0]);
        result1 += horizontal_sum(partialSums[1]);
        result2 += horizontal_sum(partialSums[2]);
        result3 += horizontal_sum(partialSums[3]);
    }

    //
    if (m < M) {
        // process leftovers
        PQDecoder8 decoder0(code0 + m, nbits);
        PQDecoder8 decoder1(code1 + m, nbits);
        PQDecoder8 decoder2(code2 + m, nbits);
        PQDecoder8 decoder3(code3 + m, nbits);
        for (; m < M; m++) {
            result0 += tab[decoder0.decode()];
            result1 += tab[decoder1.decode()];
            result2 += tab[decoder2.decode()];
            result3 += tab[decoder3.decode()];
            tab += ksub;
        }
    }
}

#endif

#if defined(__AVX_SUPPORT__)

template <typename PQDecoderT>
float distance_single_code(
        // number of subquantizers
        uint32 M,
        // number of bits per quantization index
        uint32 nbits,
        // precomputed distances, layout (M, ksub)
        const float* sim_table,
        // the code
        const uint8* code) {
    return distance_single_code_avx2<PQDecoderT>(M, nbits, sim_table, code);
}

template <typename PQDecoderT>
void distance_four_codes(
        // number of subquantizers
        uint32 M,
        // number of bits per quantization index
        uint32 nbits,
        // precomputed distances, layout (M, ksub)
        const float* sim_table,
        // codes
        const uint8* __restrict code0,
        const uint8* __restrict code1,
        const uint8* __restrict code2,
        const uint8* __restrict code3,
        // computed distances
        float& result0,
        float& result1,
        float& result2,
        float& result3) {
    distance_four_codes_avx2<PQDecoderT>(
            M,
            nbits,
            sim_table,
            code0,
            code1,
            code2,
            code3,
            result0,
            result1,
            result2,
            result3);
}

#elif defined(__SVE_SUPPORT__)

#include <arm_sve.h>

#include <tuple>
#include <type_traits>

namespace {

template <typename PQDecoderT>
std::enable_if_t<!std::is_same_v<PQDecoderT, PQDecoder8>, float> distance_single_code_sve(
        // the product quantizer
        uint32 M,
        // number of bits per quantization index
        uint32 nbits,
        // precomputed distances, layout (M, ksub)
        const float* sim_table,
        const uint8* code) {
    // default implementation
    return distance_single_code_generic<PQDecoderT>(M, nbits, sim_table, code);
}

static void distance_codes_kernel(
        svbool_t pg,
        svuint32_t idx1,
        svuint32_t offsets_0,
        const float* tab,
        svfloat32_t& partialSum) {
    // add offset
    const auto indices_to_read_from = svadd_u32_x(pg, idx1, offsets_0);

    // gather values, similar to some operations of tab[index]
    const auto collected =
            svld1_gather_u32index_f32(pg, tab, indices_to_read_from);

    // collect partial sum
    partialSum = svadd_f32_m(pg, partialSum, collected);
}

static float distance_single_code_sve_for_small_m(
        // the product quantizer
        uint32 M,
        // precomputed distances, layout (M, ksub)
        const float* sim_table,
        // codes
        const uint8* __restrict code) {
    constexpr uint32 nbits = 8u;

    const uint32 ksub = 1 << nbits;

    const auto offsets_0 = svindex_u32(0, static_cast<uint32_t>(ksub));

    // loop
    const auto pg = svwhilelt_b32_u64(0, M);

    auto mm1 = svld1ub_u32(pg, code);
    mm1 = svadd_u32_x(pg, mm1, offsets_0);
    const auto collected0 = svld1_gather_u32index_f32(pg, sim_table, mm1);
    return svaddv_f32(pg, collected0);
}

template <typename PQDecoderT>
std::enable_if_t<std::is_same_v<PQDecoderT, PQDecoder8>, float> distance_single_code_sve(
        // the product quantizer
        uint32 M,
        // number of bits per quantization index
        uint32 nbits,
        // precomputed distances, layout (M, ksub)
        const float* sim_table,
        const uint8* code) {
    if (M <= svcntw())
        return distance_single_code_sve_for_small_m(M, sim_table, code);

    const float* tab = sim_table;

    const uint32 ksub = 1 << nbits;

    const auto offsets_0 = svindex_u32(0, static_cast<uint32_t>(ksub));

    // accumulators of partial sums
    auto partialSum = svdup_n_f32(0.f);

    const auto lanes = svcntb();
    const auto quad_lanes = lanes / 4;

    // loop
    for (uint32 m = 0; m < M;) {
        const auto pg = svwhilelt_b8_u64(m, M);

        const auto mm1 = svld1_u8(pg, code + m);
        {
            const auto mm1lo = svunpklo_u16(mm1);
            const auto pglo = svunpklo_b(pg);

            {
                // convert uint8 values to uint32 values
                const auto idx1 = svunpklo_u32(mm1lo);
                const auto pglolo = svunpklo_b(pglo);

                distance_codes_kernel(pglolo, idx1, offsets_0, tab, partialSum);
                tab += ksub * quad_lanes;
            }

            m += quad_lanes;
            if (m >= M)
                break;

            {
                // convert uint8 values to uint32 values
                const auto idx1 = svunpkhi_u32(mm1lo);
                const auto pglohi = svunpkhi_b(pglo);

                distance_codes_kernel(pglohi, idx1, offsets_0, tab, partialSum);
                tab += ksub * quad_lanes;
            }

            m += quad_lanes;
            if (m >= M)
                break;
        }

        {
            const auto mm1hi = svunpkhi_u16(mm1);
            const auto pghi = svunpkhi_b(pg);

            {
                // convert uint8 values to uint32 values
                const auto idx1 = svunpklo_u32(mm1hi);
                const auto pghilo = svunpklo_b(pghi);

                distance_codes_kernel(pghilo, idx1, offsets_0, tab, partialSum);
                tab += ksub * quad_lanes;
            }

            m += quad_lanes;
            if (m >= M)
                break;

            {
                // convert uint8 values to uint32 values
                const auto idx1 = svunpkhi_u32(mm1hi);
                const auto pghihi = svunpkhi_b(pghi);

                distance_codes_kernel(pghihi, idx1, offsets_0, tab, partialSum);
                tab += ksub * quad_lanes;
            }

            m += quad_lanes;
        }
    }

    return svaddv_f32(svptrue_b32(), partialSum);
}

template <typename PQDecoderT>
std::enable_if_t<!std::is_same_v<PQDecoderT, PQDecoder8>, void>
distance_four_codes_sve(
        // the product quantizer
        uint32 M,
        // number of bits per quantization index
        uint32 nbits,
        // precomputed distances, layout (M, ksub)
        const float* sim_table,
        // codes
        const uint8* __restrict code0,
        const uint8* __restrict code1,
        const uint8* __restrict code2,
        const uint8* __restrict code3,
        // computed distances
        float& result0,
        float& result1,
        float& result2,
        float& result3) {
    distance_four_codes_generic<PQDecoderT>(
            M,
            nbits,
            sim_table,
            code0,
            code1,
            code2,
            code3,
            result0,
            result1,
            result2,
            result3);
}

static void distance_four_codes_sve_for_small_m(
        // the product quantizer
        uint32 M,
        // precomputed distances, layout (M, ksub)
        const float* sim_table,
        // codes
        const uint8* __restrict code0,
        const uint8* __restrict code1,
        const uint8* __restrict code2,
        const uint8* __restrict code3,
        // computed distances
        float& result0,
        float& result1,
        float& result2,
        float& result3) {
    constexpr uint32 nbits = 8u;

    const uint32 ksub = 1 << nbits;

    const auto offsets_0 = svindex_u32(0, static_cast<uint32_t>(ksub));

    // loop
    const auto pg = svwhilelt_b32_u64(0, M);

    auto mm10 = svld1ub_u32(pg, code0);
    auto mm11 = svld1ub_u32(pg, code1);
    auto mm12 = svld1ub_u32(pg, code2);
    auto mm13 = svld1ub_u32(pg, code3);
    mm10 = svadd_u32_x(pg, mm10, offsets_0);
    mm11 = svadd_u32_x(pg, mm11, offsets_0);
    mm12 = svadd_u32_x(pg, mm12, offsets_0);
    mm13 = svadd_u32_x(pg, mm13, offsets_0);
    const auto collected0 = svld1_gather_u32index_f32(pg, sim_table, mm10);
    const auto collected1 = svld1_gather_u32index_f32(pg, sim_table, mm11);
    const auto collected2 = svld1_gather_u32index_f32(pg, sim_table, mm12);
    const auto collected3 = svld1_gather_u32index_f32(pg, sim_table, mm13);
    result0 = svaddv_f32(pg, collected0);
    result1 = svaddv_f32(pg, collected1);
    result2 = svaddv_f32(pg, collected2);
    result3 = svaddv_f32(pg, collected3);
}

// Combines 4 operations of distance_single_code()
template <typename PQDecoderT>
std::enable_if_t<std::is_same_v<PQDecoderT, PQDecoder8>, void>
distance_four_codes_sve(
        // the product quantizer
        uint32 M,
        // number of bits per quantization index
        uint32 nbits,
        // precomputed distances, layout (M, ksub)
        const float* sim_table,
        // codes
        const uint8* __restrict code0,
        const uint8* __restrict code1,
        const uint8* __restrict code2,
        const uint8* __restrict code3,
        // computed distances
        float& result0,
        float& result1,
        float& result2,
        float& result3) {
    if (M <= svcntw()) {
        distance_four_codes_sve_for_small_m(
                M,
                sim_table,
                code0,
                code1,
                code2,
                code3,
                result0,
                result1,
                result2,
                result3);
        return;
    }

    const float* tab = sim_table;

    const uint32 ksub = 1 << nbits;

    const auto offsets_0 = svindex_u32(0, static_cast<uint32_t>(ksub));

    // accumulators of partial sums
    auto partialSum0 = svdup_n_f32(0.f);
    auto partialSum1 = svdup_n_f32(0.f);
    auto partialSum2 = svdup_n_f32(0.f);
    auto partialSum3 = svdup_n_f32(0.f);

    const auto lanes = svcntb();
    const auto quad_lanes = lanes / 4;

    // loop
    for (uint32 m = 0; m < M;) {
        const auto pg = svwhilelt_b8_u64(m, M);

        const auto mm10 = svld1_u8(pg, code0 + m);
        const auto mm11 = svld1_u8(pg, code1 + m);
        const auto mm12 = svld1_u8(pg, code2 + m);
        const auto mm13 = svld1_u8(pg, code3 + m);
        {
            const auto mm10lo = svunpklo_u16(mm10);
            const auto mm11lo = svunpklo_u16(mm11);
            const auto mm12lo = svunpklo_u16(mm12);
            const auto mm13lo = svunpklo_u16(mm13);
            const auto pglo = svunpklo_b(pg);

            {
                const auto pglolo = svunpklo_b(pglo);
                {
                    const auto idx1 = svunpklo_u32(mm10lo);
                    distance_codes_kernel(
                            pglolo, idx1, offsets_0, tab, partialSum0);
                }
                {
                    const auto idx1 = svunpklo_u32(mm11lo);
                    distance_codes_kernel(
                            pglolo, idx1, offsets_0, tab, partialSum1);
                }
                {
                    const auto idx1 = svunpklo_u32(mm12lo);
                    distance_codes_kernel(
                            pglolo, idx1, offsets_0, tab, partialSum2);
                }
                {
                    const auto idx1 = svunpklo_u32(mm13lo);
                    distance_codes_kernel(
                            pglolo, idx1, offsets_0, tab, partialSum3);
                }
                tab += ksub * quad_lanes;
            }

            m += quad_lanes;
            if (m >= M)
                break;

            {
                const auto pglohi = svunpkhi_b(pglo);
                {
                    const auto idx1 = svunpkhi_u32(mm10lo);
                    distance_codes_kernel(
                            pglohi, idx1, offsets_0, tab, partialSum0);
                }
                {
                    const auto idx1 = svunpkhi_u32(mm11lo);
                    distance_codes_kernel(
                            pglohi, idx1, offsets_0, tab, partialSum1);
                }
                {
                    const auto idx1 = svunpkhi_u32(mm12lo);
                    distance_codes_kernel(
                            pglohi, idx1, offsets_0, tab, partialSum2);
                }
                {
                    const auto idx1 = svunpkhi_u32(mm13lo);
                    distance_codes_kernel(
                            pglohi, idx1, offsets_0, tab, partialSum3);
                }
                tab += ksub * quad_lanes;
            }

            m += quad_lanes;
            if (m >= M)
                break;
        }

        {
            const auto mm10hi = svunpkhi_u16(mm10);
            const auto mm11hi = svunpkhi_u16(mm11);
            const auto mm12hi = svunpkhi_u16(mm12);
            const auto mm13hi = svunpkhi_u16(mm13);
            const auto pghi = svunpkhi_b(pg);

            {
                const auto pghilo = svunpklo_b(pghi);
                {
                    const auto idx1 = svunpklo_u32(mm10hi);
                    distance_codes_kernel(
                            pghilo, idx1, offsets_0, tab, partialSum0);
                }
                {
                    const auto idx1 = svunpklo_u32(mm11hi);
                    distance_codes_kernel(
                            pghilo, idx1, offsets_0, tab, partialSum1);
                }
                {
                    const auto idx1 = svunpklo_u32(mm12hi);
                    distance_codes_kernel(
                            pghilo, idx1, offsets_0, tab, partialSum2);
                }
                {
                    const auto idx1 = svunpklo_u32(mm13hi);
                    distance_codes_kernel(
                            pghilo, idx1, offsets_0, tab, partialSum3);
                }
                tab += ksub * quad_lanes;
            }

            m += quad_lanes;
            if (m >= M)
                break;

            {
                const auto pghihi = svunpkhi_b(pghi);
                {
                    const auto idx1 = svunpkhi_u32(mm10hi);
                    distance_codes_kernel(
                            pghihi, idx1, offsets_0, tab, partialSum0);
                }
                {
                    const auto idx1 = svunpkhi_u32(mm11hi);
                    distance_codes_kernel(
                            pghihi, idx1, offsets_0, tab, partialSum1);
                }
                {
                    const auto idx1 = svunpkhi_u32(mm12hi);
                    distance_codes_kernel(
                            pghihi, idx1, offsets_0, tab, partialSum2);
                }
                {
                    const auto idx1 = svunpkhi_u32(mm13hi);
                    distance_codes_kernel(
                            pghihi, idx1, offsets_0, tab, partialSum3);
                }
                tab += ksub * quad_lanes;
            }

            m += quad_lanes;
        }
    }

    result0 = svaddv_f32(svptrue_b32(), partialSum0);
    result1 = svaddv_f32(svptrue_b32(), partialSum1);
    result2 = svaddv_f32(svptrue_b32(), partialSum2);
    result3 = svaddv_f32(svptrue_b32(), partialSum3);
}

} // namespace


template <typename PQDecoderT>
float distance_single_code(
        // the product quantizer
        uint32 M,
        // number of bits per quantization index
        uint32 nbits,
        // precomputed distances, layout (M, ksub)
        const float* sim_table,
        // the code
        const uint8* code) {
    return distance_single_code_sve<PQDecoderT>(M, nbits, sim_table, code);
}

template <typename PQDecoderT>
void distance_four_codes(
        // the product quantizer
        uint32 M,
        // number of bits per quantization index
        uint32 nbits,
        // precomputed distances, layout (M, ksub)
        const float* sim_table,
        // codes
        const uint8* __restrict code0,
        const uint8* __restrict code1,
        const uint8* __restrict code2,
        const uint8* __restrict code3,
        // computed distances
        float& result0,
        float& result1,
        float& result2,
        float& result3) {
    distance_four_codes_sve<PQDecoderT>(
            M,
            nbits,
            sim_table,
            code0,
            code1,
            code2,
            code3,
            result0,
            result1,
            result2,
            result3);
}

#else

template <typename PQDecoderT>
float distance_single_code(
        // number of subquantizers
        uint32 M,
        // number of bits per quantization index
        uint32 nbits,
        // precomputed distances, layout (M, ksub)
        const float* sim_table,
        // the code
        const uint8* code) {
    return distance_single_code_generic<PQDecoderT>(M, nbits, sim_table, code);
}

template <typename PQDecoderT>
void distance_four_codes(
        // number of subquantizers
        uint32 M,
        // number of bits per quantization index
        uint32 nbits,
        // precomputed distances, layout (M, ksub)
        const float* sim_table,
        // codes
        const uint8* __restrict code0,
        const uint8* __restrict code1,
        const uint8* __restrict code2,
        const uint8* __restrict code3,
        // computed distances
        float& result0,
        float& result1,
        float& result2,
        float& result3) {
            //  int83 a =6;
    distance_four_codes_generic<PQDecoderT>(
            M,
            nbits,
            sim_table,
            code0,
            code1,
            code2,
            code3,
            result0,
            result1,
            result2,
            result3);
}
#endif

void ann_helper::DISTANCE_FUNC_NAME(distance_four_codes_g)(
    uint32 M, uint32 nbits, const float *sim_table,
    const uint8 *__restrict code0, const uint8 *__restrict code1,
    const uint8 *__restrict code2, const uint8 *__restrict code3,
    float &result0, float &result1, float &result2, float &result3)
{
    return distance_four_codes<PQDecoderGeneric>(M, nbits, sim_table, code0, code1, code2, code3,
        result0, result1, result2, result3);
}
void ann_helper::DISTANCE_FUNC_NAME(distance_four_codes_8)(
    uint32 M, uint32 nbits, const float *sim_table,
    const uint8 *__restrict code0, const uint8 *__restrict code1,
    const uint8 *__restrict code2, const uint8 *__restrict code3,
    float &result0, float &result1, float &result2, float &result3)
{
    return distance_four_codes<PQDecoder8>(M, nbits, sim_table, code0, code1, code2, code3,
        result0, result1, result2, result3);
}
void ann_helper::DISTANCE_FUNC_NAME(distance_four_codes_16)(
    uint32 M, uint32 nbits, const float *sim_table,
    const uint8 *__restrict code0, const uint8 *__restrict code1,
    const uint8 *__restrict code2, const uint8 *__restrict code3,
    float &result0, float &result1, float &result2, float &result3)
{
    return distance_four_codes<PQDecoder16>(M, nbits, sim_table, code0, code1, code2, code3,
        result0, result1, result2, result3);
}
float ann_helper::DISTANCE_FUNC_NAME(distance_single_code_g)(
        uint32 M, uint32 nbits, const float *sim_table, const uint8 *code)
{
    return distance_single_code<PQDecoderGeneric>(M, nbits, sim_table, code);
}
float ann_helper::DISTANCE_FUNC_NAME(distance_single_code_8)(
        uint32 M, uint32 nbits, const float *sim_table, const uint8 *code)
{
    return distance_single_code<PQDecoder8>(M, nbits, sim_table, code);
}
float ann_helper::DISTANCE_FUNC_NAME(distance_single_code_16)(
        uint32 M, uint32 nbits, const float *sim_table, const uint8 *code)
{
    return distance_single_code<PQDecoder16>(M, nbits, sim_table, code);
}
