#ifndef DISTANCE_FUNC_NAME
static_assert(false, "don't use the file without definition DISTANCE_FUNC_NAME");
#endif

#include <algorithm>
#include <cmath>

/** Abstractions for 256-bit registers
 *
 * The objective is to separate the different interpretations of the same
 * registers (as a vector of uint8, uint16 or uint32), to provide printing
 * functions.
 */

#ifdef __SSE_SUPPORT__
#include <immintrin.h>
#include "distance/pq/horizontal_sum128.h"
#endif

#ifdef __AVX_SUPPORT__
#include "distance/pq/transpose_avx2_inl.h"
#include "distance/pq/horizontal_sum256.h"
#endif

#ifdef __AVX512_SUPPORT__ 
#include "distance/pq/transpose_avx512_inl.h"
#include "distance/pq/horizontal_sum512.h"
#endif

#ifdef __SVE_SUPPORT__
#include <arm_sve.h>
#endif 

#if COMPILER_TARGET_AARCH64
#include <arm_neon.h>
#endif

/*********************************************************
 * Optimized distance computations
 *********************************************************/

/* Functions to compute:
   - L2 distance between 2 vectors
   - inner product between 2 vectors
   - L2 norm of a vector

   The functions should probably not be invoked when a large number of
   vectors are be processed in batch (in which case Matrix multiply
   is faster), but may be useful for comparing vectors isolated in
   memory.

   Works with any vectors of any dimension, even unaligned (in which
   case they are slower).

*/

/*********************************************************
 * Autovectorized implementations
 */
float DISTANCE_FUNC_NAME(fvec_inner_product)(const float* x, const float* y, uint32 d) {
    float res = 0.F;
    for (uint32 i = 0; i != d; ++i) {
        res += x[i] * y[i];
    }
    return res;
}

float DISTANCE_FUNC_NAME(fvec_L2sqr)(const float* x, const float* y, uint32 d) {
    uint32 i;
    float res = 0;
    for (i = 0; i < d; i++) {
        const float tmp = x[i] - y[i];
        res += tmp * tmp;
    }
    return res;
}


/*********************************************************
 * Reference implementations
 */

void DISTANCE_FUNC_NAME(fvec_L2sqr_ny_ref)(
        float* dis,
        const float* x,
        const float* y,
        uint32 d,
        uint32 ny) {
    for (uint32 i = 0; i < ny; i++) {
        dis[i] = DISTANCE_FUNC_NAME(fvec_L2sqr)(x, y, d);
        y += d;
    }
}

void DISTANCE_FUNC_NAME(fvec_inner_products_ny_ref)(
        float* ip,
        const float* x,
        const float* y,
        uint32 d,
        uint32 ny) {
    for (uint32 i = 0; i < ny; i++) {
        ip[i] = DISTANCE_FUNC_NAME(fvec_inner_product)(x, y, d);
        y += d;
    }
}

/*********************************************************
 * SSE and AVX implementations
 */

#ifdef __SSE_SUPPORT__
namespace {
#ifndef __AVX_SUPPORT__
struct ElementOp128L2 {
    static float op(float x, float y) {
        float tmp = x - y;
        return tmp * tmp;
    }
    static __m128 op(__m128 x, __m128 y) {
        __m128 tmp = _mm_sub_ps(x, y);
        return _mm_mul_ps(tmp, tmp);
    }
};

struct ElementOp128IP {
    static float op(float x, float y) { return x * y; }
    static __m128 op(__m128 x, __m128 y) { return _mm_mul_ps(x, y); }
};
#endif

#if defined(__AVX_SUPPORT__) && !defined(__AVX512_SUPPORT__)
struct ElementOp256L2 {
    static float op(float x, float y) {
        float tmp = x - y;
        return tmp * tmp;
    }
    static __m128 op(__m128 x, __m128 y) {
        __m128 tmp = _mm_sub_ps(x, y);
        return _mm_mul_ps(tmp, tmp);
    }
    static __m256 op(__m256 x, __m256 y) {
        __m256 tmp = _mm256_sub_ps(x, y);
        return _mm256_mul_ps(tmp, tmp);
    }
};

struct ElementOp256IP {
    static float op(float x, float y) { return x * y; }
    static __m128 op(__m128 x, __m128 y) { return _mm_mul_ps(x, y); }
    static __m256 op(__m256 x, __m256 y) { return _mm256_mul_ps(x, y); }
};
#endif

#ifdef __AVX512_SUPPORT__
struct ElementOp512L2 {
    static float op(float x, float y) {
        float tmp = x - y;
        return tmp * tmp;
    }
    static __m128 op(__m128 x, __m128 y) {
        __m128 tmp = _mm_sub_ps(x, y);
        return _mm_mul_ps(tmp, tmp);
    }
    static __m256 op(__m256 x, __m256 y) {
        __m256 tmp = _mm256_sub_ps(x, y);
        return _mm256_mul_ps(tmp, tmp);
    }
    static __m512 op(__m512 x, __m512 y) {
        __m512 tmp = _mm512_sub_ps(x, y);
        return _mm512_mul_ps(tmp, tmp);
    }
};
struct ElementOp512IP {
    static float op(float x, float y) { return x * y; }
    static __m128 op(__m128 x, __m128 y) { return _mm_mul_ps(x, y); }
    static __m256 op(__m256 x, __m256 y) { return _mm256_mul_ps(x, y); }
    static __m512 op(__m512 x, __m512 y) { return _mm512_mul_ps(x, y); }
};
#endif

template <class ElementOp>
void fvec_op_ny_D1(float* dis, const float* x, const float* y, uint32 ny) {

    float x0s = x[0];
    __m128 x0 = _mm_set_ps(x0s, x0s, x0s, x0s);

    uint32 i;
    for (i = 0; i + 3 < ny; i += 4) {
        __m128 accu = ElementOp::op(x0, _mm_loadu_ps(y));
        y += 4;
        dis[i] = _mm_cvtss_f32(accu);
        __m128 tmp = _mm_shuffle_ps(accu, accu, 1);
        dis[i + 1] = _mm_cvtss_f32(tmp);
        tmp = _mm_shuffle_ps(accu, accu, 2);
        dis[i + 2] = _mm_cvtss_f32(tmp);
        tmp = _mm_shuffle_ps(accu, accu, 3);
        dis[i + 3] = _mm_cvtss_f32(tmp);
    }
    while (i < ny) { // handle non-multiple-of-4 case
        dis[i++] = ElementOp::op(x0s, *y++);
    }
}

template <class ElementOp>
void fvec_op_ny_D2(float* dis, const float* x, const float* y, uint32 ny) {
    __m128 x0 = _mm_set_ps(x[1], x[0], x[1], x[0]);
    uint32 i;
    for (i = 0; i + 1 < ny; i += 2) {
        __m128 accu = ElementOp::op(x0, _mm_loadu_ps(y));
        y += 4;
        accu = _mm_hadd_ps(accu, accu);
        dis[i] = _mm_cvtss_f32(accu);
        accu = _mm_shuffle_ps(accu, accu, 3);
        dis[i + 1] = _mm_cvtss_f32(accu);
    }
    if (i < ny) { // handle odd case
        dis[i] = ElementOp::op(x[0], y[0]) + ElementOp::op(x[1], y[1]);
    }
}

#if defined(__AVX512_SUPPORT__)
template <>
void fvec_op_ny_D2<ElementOp512IP>(
        float* dis,
        const float* x,
        const float* y,
        uint32 ny) {
    const uint32 ny16 = ny / 16;
    uint32 i = 0;

    if (ny16 > 0) {
        // process 16 D2-vectors per loop.
        _mm_prefetch((const char*)y, _MM_HINT_T0);
        _mm_prefetch((const char*)(y + 32), _MM_HINT_T0);

        const __m512 m0 = _mm512_set1_ps(x[0]);
        const __m512 m1 = _mm512_set1_ps(x[1]);

        for (i = 0; i < ny16 * 16; i += 16) {
            _mm_prefetch((const char*)(y + 64), _MM_HINT_T0);

            // load 16x2 matrix and transpose it in registers.
            // the typical bottleneck is memory access, so
            // let's trade instructions for the bandwidth.

            __m512 v0;
            __m512 v1;

            transpose_16x2(
                    _mm512_loadu_ps(y + 0 * 16),
                    _mm512_loadu_ps(y + 1 * 16),
                    v0,
                    v1);

            // compute distances (dot product)
            __m512 distances = _mm512_mul_ps(m0, v0);
            distances = _mm512_fmadd_ps(m1, v1, distances);

            // store
            _mm512_storeu_ps(dis + i, distances);

            y += 32; // move to the next set of 16x2 elements
        }
    }

    if (i < ny) {
        // process leftovers
        float x0 = x[0];
        float x1 = x[1];

        for (; i < ny; i++) {
            float distance = x0 * y[0] + x1 * y[1];
            y += 2;
            dis[i] = distance;
        }
    }
}

template <>
void fvec_op_ny_D2<ElementOp512L2>(
        float* dis,
        const float* x,
        const float* y,
        uint32 ny) {
    const uint32 ny16 = ny / 16;
    uint32 i = 0;

    if (ny16 > 0) {
        // process 16 D2-vectors per loop.
        _mm_prefetch((const char*)y, _MM_HINT_T0);
        _mm_prefetch((const char*)(y + 32), _MM_HINT_T0);

        const __m512 m0 = _mm512_set1_ps(x[0]);
        const __m512 m1 = _mm512_set1_ps(x[1]);

        for (i = 0; i < ny16 * 16; i += 16) {
            _mm_prefetch((const char*)(y + 64), _MM_HINT_T0);

            // load 16x2 matrix and transpose it in registers.
            // the typical bottleneck is memory access, so
            // let's trade instructions for the bandwidth.

            __m512 v0;
            __m512 v1;

            transpose_16x2(
                    _mm512_loadu_ps(y + 0 * 16),
                    _mm512_loadu_ps(y + 1 * 16),
                    v0,
                    v1);

            // compute differences
            const __m512 d0 = _mm512_sub_ps(m0, v0);
            const __m512 d1 = _mm512_sub_ps(m1, v1);

            // compute squares of differences
            __m512 distances = _mm512_mul_ps(d0, d0);
            distances = _mm512_fmadd_ps(d1, d1, distances);

            // store
            _mm512_storeu_ps(dis + i, distances);

            y += 32; // move to the next set of 16x2 elements
        }
    }

    if (i < ny) {
        // process leftovers
        float x0 = x[0];
        float x1 = x[1];

        for (; i < ny; i++) {
            float sub0 = x0 - y[0];
            float sub1 = x1 - y[1];
            float distance = sub0 * sub0 + sub1 * sub1;

            y += 2;
            dis[i] = distance;
        }
    }
}

#elif defined(__AVX_SUPPORT__)

template <>
void fvec_op_ny_D2<ElementOp256IP>(
        float* dis,
        const float* x,
        const float* y,
        uint32 ny) {
    const uint32 ny8 = ny / 8;
    uint32 i = 0;

    if (ny8 > 0) {
        // process 8 D2-vectors per loop.
        _mm_prefetch((const char*)y, _MM_HINT_T0);
        _mm_prefetch((const char*)(y + 16), _MM_HINT_T0);

        const __m256 m0 = _mm256_set1_ps(x[0]);
        const __m256 m1 = _mm256_set1_ps(x[1]);

        for (i = 0; i < ny8 * 8; i += 8) {
            _mm_prefetch((const char*)(y + 32), _MM_HINT_T0);

            // load 8x2 matrix and transpose it in registers.
            // the typical bottleneck is memory access, so
            // let's trade instructions for the bandwidth.

            __m256 v0;
            __m256 v1;

            transpose_8x2(
                    _mm256_loadu_ps(y + 0 * 8),
                    _mm256_loadu_ps(y + 1 * 8),
                    v0,
                    v1);

            // compute distances
            __m256 distances = _mm256_mul_ps(m0, v0);
            distances = _mm256_fmadd_ps(m1, v1, distances);

            // store
            _mm256_storeu_ps(dis + i, distances);

            y += 16;
        }
    }

    if (i < ny) {
        // process leftovers
        float x0 = x[0];
        float x1 = x[1];

        for (; i < ny; i++) {
            float distance = x0 * y[0] + x1 * y[1];
            y += 2;
            dis[i] = distance;
        }
    }
}

template <>
void fvec_op_ny_D2<ElementOp256L2>(
        float* dis,
        const float* x,
        const float* y,
        uint32 ny) {
    const uint32 ny8 = ny / 8;
    uint32 i = 0;

    if (ny8 > 0) {
        // process 8 D2-vectors per loop.
        _mm_prefetch((const char*)y, _MM_HINT_T0);
        _mm_prefetch((const char*)(y + 16), _MM_HINT_T0);

        const __m256 m0 = _mm256_set1_ps(x[0]);
        const __m256 m1 = _mm256_set1_ps(x[1]);

        for (i = 0; i < ny8 * 8; i += 8) {
            _mm_prefetch((const char*)(y + 32), _MM_HINT_T0);

            // load 8x2 matrix and transpose it in registers.
            // the typical bottleneck is memory access, so
            // let's trade instructions for the bandwidth.

            __m256 v0;
            __m256 v1;

            transpose_8x2(
                    _mm256_loadu_ps(y + 0 * 8),
                    _mm256_loadu_ps(y + 1 * 8),
                    v0,
                    v1);

            // compute differences
            const __m256 d0 = _mm256_sub_ps(m0, v0);
            const __m256 d1 = _mm256_sub_ps(m1, v1);

            // compute squares of differences
            __m256 distances = _mm256_mul_ps(d0, d0);
            distances = _mm256_fmadd_ps(d1, d1, distances);

            // store
            _mm256_storeu_ps(dis + i, distances);

            y += 16;
        }
    }

    if (i < ny) {
        // process leftovers
        float x0 = x[0];
        float x1 = x[1];

        for (; i < ny; i++) {
            float sub0 = x0 - y[0];
            float sub1 = x1 - y[1];
            float distance = sub0 * sub0 + sub1 * sub1;

            y += 2;
            dis[i] = distance;
        }
    }
}
#endif

template <class ElementOp>
void fvec_op_ny_D4(float* dis, const float* x, const float* y, uint32 ny) {
    __m128 x0 = _mm_loadu_ps(x);

    for (uint32 i = 0; i < ny; i++) {
        __m128 accu = ElementOp::op(x0, _mm_loadu_ps(y));
        y += 4;
        dis[i] = horizontal_sum(accu);
    }
}


#if defined(__AVX512_SUPPORT__)
template <>
void fvec_op_ny_D4<ElementOp512IP>(
        float* dis,
        const float* x,
        const float* y,
        uint32 ny) {
    const uint32 ny16 = ny / 16;
    uint32 i = 0;

    if (ny16 > 0) {
        // process 16 D4-vectors per loop.
        const __m512 m0 = _mm512_set1_ps(x[0]);
        const __m512 m1 = _mm512_set1_ps(x[1]);
        const __m512 m2 = _mm512_set1_ps(x[2]);
        const __m512 m3 = _mm512_set1_ps(x[3]);

        for (i = 0; i < ny16 * 16; i += 16) {
            // load 16x4 matrix and transpose it in registers.
            // the typical bottleneck is memory access, so
            // let's trade instructions for the bandwidth.

            __m512 v0;
            __m512 v1;
            __m512 v2;
            __m512 v3;

            transpose_16x4(
                    _mm512_loadu_ps(y + 0 * 16),
                    _mm512_loadu_ps(y + 1 * 16),
                    _mm512_loadu_ps(y + 2 * 16),
                    _mm512_loadu_ps(y + 3 * 16),
                    v0,
                    v1,
                    v2,
                    v3);

            // compute distances
            __m512 distances = _mm512_mul_ps(m0, v0);
            distances = _mm512_fmadd_ps(m1, v1, distances);
            distances = _mm512_fmadd_ps(m2, v2, distances);
            distances = _mm512_fmadd_ps(m3, v3, distances);

            // store
            _mm512_storeu_ps(dis + i, distances);

            y += 64; // move to the next set of 16x4 elements
        }
    }

    if (i < ny) {
        // process leftovers
        __m128 x0 = _mm_loadu_ps(x);

        for (; i < ny; i++) {
            __m128 accu = ElementOp512IP::op(x0, _mm_loadu_ps(y));
            y += 4;
            dis[i] = horizontal_sum(accu);
        }
    }
}

template <>
void fvec_op_ny_D4<ElementOp512L2>(
        float* dis,
        const float* x,
        const float* y,
        uint32 ny) {
    const uint32 ny16 = ny / 16;
    uint32 i = 0;

    if (ny16 > 0) {
        // process 16 D4-vectors per loop.
        const __m512 m0 = _mm512_set1_ps(x[0]);
        const __m512 m1 = _mm512_set1_ps(x[1]);
        const __m512 m2 = _mm512_set1_ps(x[2]);
        const __m512 m3 = _mm512_set1_ps(x[3]);

        for (i = 0; i < ny16 * 16; i += 16) {
            // load 16x4 matrix and transpose it in registers.
            // the typical bottleneck is memory access, so
            // let's trade instructions for the bandwidth.

            __m512 v0;
            __m512 v1;
            __m512 v2;
            __m512 v3;

            transpose_16x4(
                    _mm512_loadu_ps(y + 0 * 16),
                    _mm512_loadu_ps(y + 1 * 16),
                    _mm512_loadu_ps(y + 2 * 16),
                    _mm512_loadu_ps(y + 3 * 16),
                    v0,
                    v1,
                    v2,
                    v3);

            // compute differences
            const __m512 d0 = _mm512_sub_ps(m0, v0);
            const __m512 d1 = _mm512_sub_ps(m1, v1);
            const __m512 d2 = _mm512_sub_ps(m2, v2);
            const __m512 d3 = _mm512_sub_ps(m3, v3);

            // compute squares of differences
            __m512 distances = _mm512_mul_ps(d0, d0);
            distances = _mm512_fmadd_ps(d1, d1, distances);
            distances = _mm512_fmadd_ps(d2, d2, distances);
            distances = _mm512_fmadd_ps(d3, d3, distances);

            // store
            _mm512_storeu_ps(dis + i, distances);

            y += 64; // move to the next set of 16x4 elements
        }
    }

    if (i < ny) {
        // process leftovers
        __m128 x0 = _mm_loadu_ps(x);

        for (; i < ny; i++) {
            __m128 accu = ElementOp512L2::op(x0, _mm_loadu_ps(y));
            y += 4;
            dis[i] = horizontal_sum(accu);
        }
    }
}

#elif  defined(__AVX_SUPPORT__)

template <>
void fvec_op_ny_D4<ElementOp256IP>(
        float* dis,
        const float* x,
        const float* y,
        uint32 ny) {
    const uint32 ny8 = ny / 8;
    uint32 i = 0;

    if (ny8 > 0) {
        // process 8 D4-vectors per loop.
        const __m256 m0 = _mm256_set1_ps(x[0]);
        const __m256 m1 = _mm256_set1_ps(x[1]);
        const __m256 m2 = _mm256_set1_ps(x[2]);
        const __m256 m3 = _mm256_set1_ps(x[3]);

        for (i = 0; i < ny8 * 8; i += 8) {
            // load 8x4 matrix and transpose it in registers.
            // the typical bottleneck is memory access, so
            // let's trade instructions for the bandwidth.

            __m256 v0;
            __m256 v1;
            __m256 v2;
            __m256 v3;

            transpose_8x4(
                    _mm256_loadu_ps(y + 0 * 8),
                    _mm256_loadu_ps(y + 1 * 8),
                    _mm256_loadu_ps(y + 2 * 8),
                    _mm256_loadu_ps(y + 3 * 8),
                    v0,
                    v1,
                    v2,
                    v3);

            // compute distances
            __m256 distances = _mm256_mul_ps(m0, v0);
            distances = _mm256_fmadd_ps(m1, v1, distances);
            distances = _mm256_fmadd_ps(m2, v2, distances);
            distances = _mm256_fmadd_ps(m3, v3, distances);

            // store
            _mm256_storeu_ps(dis + i, distances);

            y += 32;
        }
    }

    if (i < ny) {
        // process leftovers
        __m128 x0 = _mm_loadu_ps(x);

        for (; i < ny; i++) {
            __m128 accu = ElementOp256IP::op(x0, _mm_loadu_ps(y));
            y += 4;
            dis[i] = horizontal_sum(accu);
        }
    }
}

template <>
void fvec_op_ny_D4<ElementOp256L2>(
        float* dis,
        const float* x,
        const float* y,
        uint32 ny) {
    const uint32 ny8 = ny / 8;
    uint32 i = 0;

    if (ny8 > 0) {
        // process 8 D4-vectors per loop.
        const __m256 m0 = _mm256_set1_ps(x[0]);
        const __m256 m1 = _mm256_set1_ps(x[1]);
        const __m256 m2 = _mm256_set1_ps(x[2]);
        const __m256 m3 = _mm256_set1_ps(x[3]);

        for (i = 0; i < ny8 * 8; i += 8) {
            // load 8x4 matrix and transpose it in registers.
            // the typical bottleneck is memory access, so
            // let's trade instructions for the bandwidth.

            __m256 v0;
            __m256 v1;
            __m256 v2;
            __m256 v3;

            transpose_8x4(
                    _mm256_loadu_ps(y + 0 * 8),
                    _mm256_loadu_ps(y + 1 * 8),
                    _mm256_loadu_ps(y + 2 * 8),
                    _mm256_loadu_ps(y + 3 * 8),
                    v0,
                    v1,
                    v2,
                    v3);

            // compute differences
            const __m256 d0 = _mm256_sub_ps(m0, v0);
            const __m256 d1 = _mm256_sub_ps(m1, v1);
            const __m256 d2 = _mm256_sub_ps(m2, v2);
            const __m256 d3 = _mm256_sub_ps(m3, v3);

            // compute squares of differences
            __m256 distances = _mm256_mul_ps(d0, d0);
            distances = _mm256_fmadd_ps(d1, d1, distances);
            distances = _mm256_fmadd_ps(d2, d2, distances);
            distances = _mm256_fmadd_ps(d3, d3, distances);

            // store
            _mm256_storeu_ps(dis + i, distances);

            y += 32;
        }
    }

    if (i < ny) {
        // process leftovers
        __m128 x0 = _mm_loadu_ps(x);

        for (; i < ny; i++) {
            __m128 accu = ElementOp256L2::op(x0, _mm_loadu_ps(y));
            y += 4;
            dis[i] = horizontal_sum(accu);
        }
    }
}

#endif

template <class ElementOp>
void fvec_op_ny_D8(float* dis, const float* x, const float* y, uint32 ny) {
    __m128 x0 = _mm_loadu_ps(x);
    __m128 x1 = _mm_loadu_ps(x + 4);

    for (uint32 i = 0; i < ny; i++) {
        __m128 accu = ElementOp::op(x0, _mm_loadu_ps(y));
        y += 4;
        accu = _mm_add_ps(accu, ElementOp::op(x1, _mm_loadu_ps(y)));
        y += 4;
        accu = _mm_hadd_ps(accu, accu);
        accu = _mm_hadd_ps(accu, accu);
        dis[i] = _mm_cvtss_f32(accu);
    }
}

#if defined(__AVX512_SUPPORT__)
template <>
void fvec_op_ny_D8<ElementOp512IP>(
        float* dis,
        const float* x,
        const float* y,
        uint32 ny) {
    const uint32 ny16 = ny / 16;
    uint32 i = 0;

    if (ny16 > 0) {
        // process 16 D16-vectors per loop.
        const __m512 m0 = _mm512_set1_ps(x[0]);
        const __m512 m1 = _mm512_set1_ps(x[1]);
        const __m512 m2 = _mm512_set1_ps(x[2]);
        const __m512 m3 = _mm512_set1_ps(x[3]);
        const __m512 m4 = _mm512_set1_ps(x[4]);
        const __m512 m5 = _mm512_set1_ps(x[5]);
        const __m512 m6 = _mm512_set1_ps(x[6]);
        const __m512 m7 = _mm512_set1_ps(x[7]);

        for (i = 0; i < ny16 * 16; i += 16) {
            // load 16x8 matrix and transpose it in registers.
            // the typical bottleneck is memory access, so
            // let's trade instructions for the bandwidth.

            __m512 v0;
            __m512 v1;
            __m512 v2;
            __m512 v3;
            __m512 v4;
            __m512 v5;
            __m512 v6;
            __m512 v7;

            transpose_16x8(
                    _mm512_loadu_ps(y + 0 * 16),
                    _mm512_loadu_ps(y + 1 * 16),
                    _mm512_loadu_ps(y + 2 * 16),
                    _mm512_loadu_ps(y + 3 * 16),
                    _mm512_loadu_ps(y + 4 * 16),
                    _mm512_loadu_ps(y + 5 * 16),
                    _mm512_loadu_ps(y + 6 * 16),
                    _mm512_loadu_ps(y + 7 * 16),
                    v0,
                    v1,
                    v2,
                    v3,
                    v4,
                    v5,
                    v6,
                    v7);

            // compute distances
            __m512 distances = _mm512_mul_ps(m0, v0);
            distances = _mm512_fmadd_ps(m1, v1, distances);
            distances = _mm512_fmadd_ps(m2, v2, distances);
            distances = _mm512_fmadd_ps(m3, v3, distances);
            distances = _mm512_fmadd_ps(m4, v4, distances);
            distances = _mm512_fmadd_ps(m5, v5, distances);
            distances = _mm512_fmadd_ps(m6, v6, distances);
            distances = _mm512_fmadd_ps(m7, v7, distances);

            // store
            _mm512_storeu_ps(dis + i, distances);

            y += 128; // 16 floats * 8 rows
        }
    }

    if (i < ny) {
        // process leftovers
        __m256 x0 = _mm256_loadu_ps(x);

        for (; i < ny; i++) {
            __m256 accu = ElementOp512IP::op(x0, _mm256_loadu_ps(y));
            y += 8;
            dis[i] = horizontal_sum(accu);
        }
    }
}

template <>
void fvec_op_ny_D8<ElementOp512L2>(
        float* dis,
        const float* x,
        const float* y,
        uint32 ny) {
    const uint32 ny16 = ny / 16;
    uint32 i = 0;

    if (ny16 > 0) {
        // process 16 D16-vectors per loop.
        const __m512 m0 = _mm512_set1_ps(x[0]);
        const __m512 m1 = _mm512_set1_ps(x[1]);
        const __m512 m2 = _mm512_set1_ps(x[2]);
        const __m512 m3 = _mm512_set1_ps(x[3]);
        const __m512 m4 = _mm512_set1_ps(x[4]);
        const __m512 m5 = _mm512_set1_ps(x[5]);
        const __m512 m6 = _mm512_set1_ps(x[6]);
        const __m512 m7 = _mm512_set1_ps(x[7]);

        for (i = 0; i < ny16 * 16; i += 16) {
            // load 16x8 matrix and transpose it in registers.
            // the typical bottleneck is memory access, so
            // let's trade instructions for the bandwidth.

            __m512 v0;
            __m512 v1;
            __m512 v2;
            __m512 v3;
            __m512 v4;
            __m512 v5;
            __m512 v6;
            __m512 v7;

            transpose_16x8(
                    _mm512_loadu_ps(y + 0 * 16),
                    _mm512_loadu_ps(y + 1 * 16),
                    _mm512_loadu_ps(y + 2 * 16),
                    _mm512_loadu_ps(y + 3 * 16),
                    _mm512_loadu_ps(y + 4 * 16),
                    _mm512_loadu_ps(y + 5 * 16),
                    _mm512_loadu_ps(y + 6 * 16),
                    _mm512_loadu_ps(y + 7 * 16),
                    v0,
                    v1,
                    v2,
                    v3,
                    v4,
                    v5,
                    v6,
                    v7);

            // compute differences
            const __m512 d0 = _mm512_sub_ps(m0, v0);
            const __m512 d1 = _mm512_sub_ps(m1, v1);
            const __m512 d2 = _mm512_sub_ps(m2, v2);
            const __m512 d3 = _mm512_sub_ps(m3, v3);
            const __m512 d4 = _mm512_sub_ps(m4, v4);
            const __m512 d5 = _mm512_sub_ps(m5, v5);
            const __m512 d6 = _mm512_sub_ps(m6, v6);
            const __m512 d7 = _mm512_sub_ps(m7, v7);

            // compute squares of differences
            __m512 distances = _mm512_mul_ps(d0, d0);
            distances = _mm512_fmadd_ps(d1, d1, distances);
            distances = _mm512_fmadd_ps(d2, d2, distances);
            distances = _mm512_fmadd_ps(d3, d3, distances);
            distances = _mm512_fmadd_ps(d4, d4, distances);
            distances = _mm512_fmadd_ps(d5, d5, distances);
            distances = _mm512_fmadd_ps(d6, d6, distances);
            distances = _mm512_fmadd_ps(d7, d7, distances);

            // store
            _mm512_storeu_ps(dis + i, distances);

            y += 128; // 16 floats * 8 rows
        }
    }

    if (i < ny) {
        // process leftovers
        __m256 x0 = _mm256_loadu_ps(x);

        for (; i < ny; i++) {
            __m256 accu = ElementOp512L2::op(x0, _mm256_loadu_ps(y));
            y += 8;
            dis[i] = horizontal_sum(accu);
        }
    }
}


#elif defined(__AVX_SUPPORT__)

template <>
void fvec_op_ny_D8<ElementOp256IP>(
        float* dis,
        const float* x,
        const float* y,
        uint32 ny) {
    const uint32 ny8 = ny / 8;
    uint32 i = 0;

    if (ny8 > 0) {
        // process 8 D8-vectors per loop.
        const __m256 m0 = _mm256_set1_ps(x[0]);
        const __m256 m1 = _mm256_set1_ps(x[1]);
        const __m256 m2 = _mm256_set1_ps(x[2]);
        const __m256 m3 = _mm256_set1_ps(x[3]);
        const __m256 m4 = _mm256_set1_ps(x[4]);
        const __m256 m5 = _mm256_set1_ps(x[5]);
        const __m256 m6 = _mm256_set1_ps(x[6]);
        const __m256 m7 = _mm256_set1_ps(x[7]);

        for (i = 0; i < ny8 * 8; i += 8) {
            // load 8x8 matrix and transpose it in registers.
            // the typical bottleneck is memory access, so
            // let's trade instructions for the bandwidth.

            __m256 v0;
            __m256 v1;
            __m256 v2;
            __m256 v3;
            __m256 v4;
            __m256 v5;
            __m256 v6;
            __m256 v7;

            transpose_8x8(
                    _mm256_loadu_ps(y + 0 * 8),
                    _mm256_loadu_ps(y + 1 * 8),
                    _mm256_loadu_ps(y + 2 * 8),
                    _mm256_loadu_ps(y + 3 * 8),
                    _mm256_loadu_ps(y + 4 * 8),
                    _mm256_loadu_ps(y + 5 * 8),
                    _mm256_loadu_ps(y + 6 * 8),
                    _mm256_loadu_ps(y + 7 * 8),
                    v0,
                    v1,
                    v2,
                    v3,
                    v4,
                    v5,
                    v6,
                    v7);

            // compute distances
            __m256 distances = _mm256_mul_ps(m0, v0);
            distances = _mm256_fmadd_ps(m1, v1, distances);
            distances = _mm256_fmadd_ps(m2, v2, distances);
            distances = _mm256_fmadd_ps(m3, v3, distances);
            distances = _mm256_fmadd_ps(m4, v4, distances);
            distances = _mm256_fmadd_ps(m5, v5, distances);
            distances = _mm256_fmadd_ps(m6, v6, distances);
            distances = _mm256_fmadd_ps(m7, v7, distances);

            // store
            _mm256_storeu_ps(dis + i, distances);

            y += 64;
        }
    }

    if (i < ny) {
        // process leftovers
        __m256 x0 = _mm256_loadu_ps(x);

        for (; i < ny; i++) {
            __m256 accu = ElementOp256IP::op(x0, _mm256_loadu_ps(y));
            y += 8;
            dis[i] = horizontal_sum(accu);
        }
    }
}

template <>
void fvec_op_ny_D8<ElementOp256L2>(
        float* dis,
        const float* x,
        const float* y,
        uint32 ny) {
    const uint32 ny8 = ny / 8;
    uint32 i = 0;

    if (ny8 > 0) {
        // process 8 D8-vectors per loop.
        const __m256 m0 = _mm256_set1_ps(x[0]);
        const __m256 m1 = _mm256_set1_ps(x[1]);
        const __m256 m2 = _mm256_set1_ps(x[2]);
        const __m256 m3 = _mm256_set1_ps(x[3]);
        const __m256 m4 = _mm256_set1_ps(x[4]);
        const __m256 m5 = _mm256_set1_ps(x[5]);
        const __m256 m6 = _mm256_set1_ps(x[6]);
        const __m256 m7 = _mm256_set1_ps(x[7]);

        for (i = 0; i < ny8 * 8; i += 8) {
            // load 8x8 matrix and transpose it in registers.
            // the typical bottleneck is memory access, so
            // let's trade instructions for the bandwidth.

            __m256 v0;
            __m256 v1;
            __m256 v2;
            __m256 v3;
            __m256 v4;
            __m256 v5;
            __m256 v6;
            __m256 v7;

            transpose_8x8(
                    _mm256_loadu_ps(y + 0 * 8),
                    _mm256_loadu_ps(y + 1 * 8),
                    _mm256_loadu_ps(y + 2 * 8),
                    _mm256_loadu_ps(y + 3 * 8),
                    _mm256_loadu_ps(y + 4 * 8),
                    _mm256_loadu_ps(y + 5 * 8),
                    _mm256_loadu_ps(y + 6 * 8),
                    _mm256_loadu_ps(y + 7 * 8),
                    v0,
                    v1,
                    v2,
                    v3,
                    v4,
                    v5,
                    v6,
                    v7);

            // compute differences
            const __m256 d0 = _mm256_sub_ps(m0, v0);
            const __m256 d1 = _mm256_sub_ps(m1, v1);
            const __m256 d2 = _mm256_sub_ps(m2, v2);
            const __m256 d3 = _mm256_sub_ps(m3, v3);
            const __m256 d4 = _mm256_sub_ps(m4, v4);
            const __m256 d5 = _mm256_sub_ps(m5, v5);
            const __m256 d6 = _mm256_sub_ps(m6, v6);
            const __m256 d7 = _mm256_sub_ps(m7, v7);

            // compute squares of differences
            __m256 distances = _mm256_mul_ps(d0, d0);
            distances = _mm256_fmadd_ps(d1, d1, distances);
            distances = _mm256_fmadd_ps(d2, d2, distances);
            distances = _mm256_fmadd_ps(d3, d3, distances);
            distances = _mm256_fmadd_ps(d4, d4, distances);
            distances = _mm256_fmadd_ps(d5, d5, distances);
            distances = _mm256_fmadd_ps(d6, d6, distances);
            distances = _mm256_fmadd_ps(d7, d7, distances);

            // store
            _mm256_storeu_ps(dis + i, distances);

            y += 64;
        }
    }

    if (i < ny) {
        // process leftovers
        __m256 x0 = _mm256_loadu_ps(x);

        for (; i < ny; i++) {
            __m256 accu = ElementOp256L2::op(x0, _mm256_loadu_ps(y));
            y += 8;
            dis[i] = horizontal_sum(accu);
        }
    }
}

#endif

template <class ElementOp>
void fvec_op_ny_D12(float* dis, const float* x, const float* y, uint32 ny) {
    __m128 x0 = _mm_loadu_ps(x);
    __m128 x1 = _mm_loadu_ps(x + 4);
    __m128 x2 = _mm_loadu_ps(x + 8);

    for (uint32 i = 0; i < ny; i++) {
        __m128 accu = ElementOp::op(x0, _mm_loadu_ps(y));
        y += 4;
        accu = _mm_add_ps(accu, ElementOp::op(x1, _mm_loadu_ps(y)));
        y += 4;
        accu = _mm_add_ps(accu, ElementOp::op(x2, _mm_loadu_ps(y)));
        y += 4;
        dis[i] = horizontal_sum(accu);
    }
}
} // anonymous namespace

void ann_helper::DISTANCE_FUNC_NAME(fvec_L2sqr_ny)(
        float* dis,
        const float* x,
        const float* y,
        uint32 d,
        uint32 ny) {
    // optimized for a few special cases
#if defined(__AVX512_SUPPORT__) 
#define DISPATCH(dval)                                  \
    case dval:                                          \
        fvec_op_ny_D##dval<ElementOp512L2>(dis, x, y, ny); \
        return;

#elif defined(__AVX_SUPPORT__)
#define DISPATCH(dval)                                  \
    case dval:                                        \
        fvec_op_ny_D##dval<ElementOp256L2>(dis, x, y, ny); \
        return;
#else
#define DISPATCH(dval)                                  \
    case dval:                                           \
        fvec_op_ny_D##dval<ElementOp128L2>(dis, x, y, ny); \
        return;
#endif

    switch (d) {
        DISPATCH(1)
        DISPATCH(2)
        DISPATCH(4)
        DISPATCH(8)
        DISPATCH(12)
        default:
            DISTANCE_FUNC_NAME(fvec_L2sqr_ny_ref)(dis, x, y, d, ny);
            return;
    }
#undef DISPATCH
}

void ann_helper::DISTANCE_FUNC_NAME(fvec_inner_products_ny)(
        float* dis,
        const float* x,
        const float* y,
        uint32 d,
        uint32 ny) {

#if defined(__AVX512_SUPPORT__) 
#define DISPATCH(dval)                                  \
    case dval:                                          \
        fvec_op_ny_D##dval<ElementOp512IP>(dis, x, y, ny); \
        return;

#elif defined(__AVX_SUPPORT__) 
#define DISPATCH(dval)                                  \
    case dval:                                          \
        fvec_op_ny_D##dval<ElementOp256IP>(dis, x, y, ny); \
        return;
#else
#define DISPATCH(dval)                                  \
    case dval:                                          \
        fvec_op_ny_D##dval<ElementOp128IP>(dis, x, y, ny); \
        return;
#endif

    switch (d) {
        DISPATCH(1)
        DISPATCH(2)
        DISPATCH(4)
        DISPATCH(8)
        DISPATCH(12)
        default:
            DISTANCE_FUNC_NAME(fvec_inner_products_ny_ref)(dis, x, y, d, ny);
            return;
    }
#undef DISPATCH
}

#if defined(__AVX512_SUPPORT__)

uint32 fvec_L2sqr_ny_nearest_avx512_D2(
        float* distances_tmp_buffer,
        const float* x,
        const float* y,
        uint32 ny) {
    // this implementation does not use distances_tmp_buffer.

    uint32 i = 0;
    float current_min_distance = HUGE_VALF;
    uint32 current_min_index = 0;

    const uint32 ny16 = ny / 16;
    if (ny16 > 0) {
        _mm_prefetch((const char*)y, _MM_HINT_T0);
        _mm_prefetch((const char*)(y + 32), _MM_HINT_T0);

        __m512 min_distances = _mm512_set1_ps(HUGE_VALF);
        __m512i min_indices = _mm512_set1_epi32(0);

        __m512i current_indices = _mm512_setr_epi32(
                0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        const __m512i indices_increment = _mm512_set1_epi32(16);

        const __m512 m0 = _mm512_set1_ps(x[0]);
        const __m512 m1 = _mm512_set1_ps(x[1]);

        for (; i < ny16 * 16; i += 16) {
            _mm_prefetch((const char*)(y + 64), _MM_HINT_T0);

            __m512 v0;
            __m512 v1;

            transpose_16x2(
                    _mm512_loadu_ps(y + 0 * 16),
                    _mm512_loadu_ps(y + 1 * 16),
                    v0,
                    v1);

            const __m512 d0 = _mm512_sub_ps(m0, v0);
            const __m512 d1 = _mm512_sub_ps(m1, v1);

            __m512 distances = _mm512_mul_ps(d0, d0);
            distances = _mm512_fmadd_ps(d1, d1, distances);

            __mmask16 comparison =
                    _mm512_cmp_ps_mask(distances, min_distances, _CMP_LT_OS);

            min_distances = _mm512_min_ps(distances, min_distances);
            min_indices = _mm512_mask_blend_epi32(
                    comparison, min_indices, current_indices);

            current_indices =
                    _mm512_add_epi32(current_indices, indices_increment);

            y += 32;
        }

        alignas(64) float min_distances_scalar[16];
        alignas(64) uint32_t min_indices_scalar[16];
        _mm512_store_ps(min_distances_scalar, min_distances);
        _mm512_store_epi32(min_indices_scalar, min_indices);

        for (uint32 j = 0; j < 16; j++) {
            if (current_min_distance > min_distances_scalar[j]) {
                current_min_distance = min_distances_scalar[j];
                current_min_index = min_indices_scalar[j];
            }
        }
    }

    if (i < ny) {
        float x0 = x[0];
        float x1 = x[1];

        for (; i < ny; i++) {
            float sub0 = x0 - y[0];
            float sub1 = x1 - y[1];
            float distance = sub0 * sub0 + sub1 * sub1;

            y += 2;

            if (current_min_distance > distance) {
                current_min_distance = distance;
                current_min_index = i;
            }
        }
    }

    return current_min_index;
}

uint32 fvec_L2sqr_ny_nearest_avx512_D4(
        float* distances_tmp_buffer,
        const float* x,
        const float* y,
        uint32 ny) {
    // this implementation does not use distances_tmp_buffer.

    uint32 i = 0;
    float current_min_distance = HUGE_VALF;
    uint32 current_min_index = 0;

    const uint32 ny16 = ny / 16;

    if (ny16 > 0) {
        __m512 min_distances = _mm512_set1_ps(HUGE_VALF);
        __m512i min_indices = _mm512_set1_epi32(0);

        __m512i current_indices = _mm512_setr_epi32(
                0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        const __m512i indices_increment = _mm512_set1_epi32(16);

        const __m512 m0 = _mm512_set1_ps(x[0]);
        const __m512 m1 = _mm512_set1_ps(x[1]);
        const __m512 m2 = _mm512_set1_ps(x[2]);
        const __m512 m3 = _mm512_set1_ps(x[3]);

        for (; i < ny16 * 16; i += 16) {
            __m512 v0;
            __m512 v1;
            __m512 v2;
            __m512 v3;

            transpose_16x4(
                    _mm512_loadu_ps(y + 0 * 16),
                    _mm512_loadu_ps(y + 1 * 16),
                    _mm512_loadu_ps(y + 2 * 16),
                    _mm512_loadu_ps(y + 3 * 16),
                    v0,
                    v1,
                    v2,
                    v3);

            const __m512 d0 = _mm512_sub_ps(m0, v0);
            const __m512 d1 = _mm512_sub_ps(m1, v1);
            const __m512 d2 = _mm512_sub_ps(m2, v2);
            const __m512 d3 = _mm512_sub_ps(m3, v3);

            __m512 distances = _mm512_mul_ps(d0, d0);
            distances = _mm512_fmadd_ps(d1, d1, distances);
            distances = _mm512_fmadd_ps(d2, d2, distances);
            distances = _mm512_fmadd_ps(d3, d3, distances);

            __mmask16 comparison =
                    _mm512_cmp_ps_mask(distances, min_distances, _CMP_LT_OS);

            min_distances = _mm512_min_ps(distances, min_distances);
            min_indices = _mm512_mask_blend_epi32(
                    comparison, min_indices, current_indices);

            current_indices =
                    _mm512_add_epi32(current_indices, indices_increment);

            y += 64;
        }

        alignas(64) float min_distances_scalar[16];
        alignas(64) uint32_t min_indices_scalar[16];
        _mm512_store_ps(min_distances_scalar, min_distances);
        _mm512_store_epi32(min_indices_scalar, min_indices);

        for (uint32 j = 0; j < 16; j++) {
            if (current_min_distance > min_distances_scalar[j]) {
                current_min_distance = min_distances_scalar[j];
                current_min_index = min_indices_scalar[j];
            }
        }
    }

    if (i < ny) {
        __m128 x0 = _mm_loadu_ps(x);

        for (; i < ny; i++) {
            __m128 accu = ElementOp512L2::op(x0, _mm_loadu_ps(y));
            y += 4;
            const float distance = horizontal_sum(accu);

            if (current_min_distance > distance) {
                current_min_distance = distance;
                current_min_index = i;
            }
        }
    }

    return current_min_index;
}

uint32 fvec_L2sqr_ny_nearest_avx512_D8(
        float* distances_tmp_buffer,
        const float* x,
        const float* y,
        uint32 ny) {
    // this implementation does not use distances_tmp_buffer.

    uint32 i = 0;
    float current_min_distance = HUGE_VALF;
    uint32 current_min_index = 0;

    const uint32 ny16 = ny / 16;
    if (ny16 > 0) {
        __m512 min_distances = _mm512_set1_ps(HUGE_VALF);
        __m512i min_indices = _mm512_set1_epi32(0);

        __m512i current_indices = _mm512_setr_epi32(
                0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
        const __m512i indices_increment = _mm512_set1_epi32(16);

        const __m512 m0 = _mm512_set1_ps(x[0]);
        const __m512 m1 = _mm512_set1_ps(x[1]);
        const __m512 m2 = _mm512_set1_ps(x[2]);
        const __m512 m3 = _mm512_set1_ps(x[3]);

        const __m512 m4 = _mm512_set1_ps(x[4]);
        const __m512 m5 = _mm512_set1_ps(x[5]);
        const __m512 m6 = _mm512_set1_ps(x[6]);
        const __m512 m7 = _mm512_set1_ps(x[7]);

        for (; i < ny16 * 16; i += 16) {
            __m512 v0;
            __m512 v1;
            __m512 v2;
            __m512 v3;
            __m512 v4;
            __m512 v5;
            __m512 v6;
            __m512 v7;

            transpose_16x8(
                    _mm512_loadu_ps(y + 0 * 16),
                    _mm512_loadu_ps(y + 1 * 16),
                    _mm512_loadu_ps(y + 2 * 16),
                    _mm512_loadu_ps(y + 3 * 16),
                    _mm512_loadu_ps(y + 4 * 16),
                    _mm512_loadu_ps(y + 5 * 16),
                    _mm512_loadu_ps(y + 6 * 16),
                    _mm512_loadu_ps(y + 7 * 16),
                    v0,
                    v1,
                    v2,
                    v3,
                    v4,
                    v5,
                    v6,
                    v7);

            const __m512 d0 = _mm512_sub_ps(m0, v0);
            const __m512 d1 = _mm512_sub_ps(m1, v1);
            const __m512 d2 = _mm512_sub_ps(m2, v2);
            const __m512 d3 = _mm512_sub_ps(m3, v3);
            const __m512 d4 = _mm512_sub_ps(m4, v4);
            const __m512 d5 = _mm512_sub_ps(m5, v5);
            const __m512 d6 = _mm512_sub_ps(m6, v6);
            const __m512 d7 = _mm512_sub_ps(m7, v7);

            __m512 distances = _mm512_mul_ps(d0, d0);
            distances = _mm512_fmadd_ps(d1, d1, distances);
            distances = _mm512_fmadd_ps(d2, d2, distances);
            distances = _mm512_fmadd_ps(d3, d3, distances);
            distances = _mm512_fmadd_ps(d4, d4, distances);
            distances = _mm512_fmadd_ps(d5, d5, distances);
            distances = _mm512_fmadd_ps(d6, d6, distances);
            distances = _mm512_fmadd_ps(d7, d7, distances);

            __mmask16 comparison =
                    _mm512_cmp_ps_mask(distances, min_distances, _CMP_LT_OS);

            min_distances = _mm512_min_ps(distances, min_distances);
            min_indices = _mm512_mask_blend_epi32(
                    comparison, min_indices, current_indices);

            current_indices =
                    _mm512_add_epi32(current_indices, indices_increment);

            y += 128;
        }

        alignas(64) float min_distances_scalar[16];
        alignas(64) uint32_t min_indices_scalar[16];
        _mm512_store_ps(min_distances_scalar, min_distances);
        _mm512_store_epi32(min_indices_scalar, min_indices);

        for (uint32 j = 0; j < 16; j++) {
            if (current_min_distance > min_distances_scalar[j]) {
                current_min_distance = min_distances_scalar[j];
                current_min_index = min_indices_scalar[j];
            }
        }
    }

    if (i < ny) {
        __m256 x0 = _mm256_loadu_ps(x);

        for (; i < ny; i++) {
            __m256 accu = ElementOp512L2::op(x0, _mm256_loadu_ps(y));
            y += 8;
            const float distance = horizontal_sum(accu);

            if (current_min_distance > distance) {
                current_min_distance = distance;
                current_min_index = i;
            }
        }
    }

    return current_min_index;
}

#elif defined(__AVX_SUPPORT__)

uint32 fvec_L2sqr_ny_nearest_D2(
        float* distances_tmp_buffer,
        const float* x,
        const float* y,
        uint32 ny) {
    // this implementation does not use distances_tmp_buffer.

    // current index being processed
    uint32 i = 0;

    // min distance and the index of the closest vector so far
    float current_min_distance = HUGE_VALF;
    uint32 current_min_index = 0;

    // process 8 D2-vectors per loop.
    const uint32 ny8 = ny / 8;
    if (ny8 > 0) {
        _mm_prefetch((const char*)y, _MM_HINT_T0);
        _mm_prefetch((const char*)(y + 16), _MM_HINT_T0);

        // track min distance and the closest vector independently
        // for each of 8 AVX2 components.
        __m256 min_distances = _mm256_set1_ps(HUGE_VALF);
        __m256i min_indices = _mm256_set1_epi32(0);

        __m256i current_indices = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
        const __m256i indices_increment = _mm256_set1_epi32(8);

        // 1 value per register
        const __m256 m0 = _mm256_set1_ps(x[0]);
        const __m256 m1 = _mm256_set1_ps(x[1]);

        for (; i < ny8 * 8; i += 8) {
            _mm_prefetch((const char*)(y + 32), _MM_HINT_T0);

            __m256 v0;
            __m256 v1;

            transpose_8x2(
                    _mm256_loadu_ps(y + 0 * 8),
                    _mm256_loadu_ps(y + 1 * 8),
                    v0,
                    v1);

            // compute differences
            const __m256 d0 = _mm256_sub_ps(m0, v0);
            const __m256 d1 = _mm256_sub_ps(m1, v1);

            // compute squares of differences
            __m256 distances = _mm256_mul_ps(d0, d0);
            distances = _mm256_fmadd_ps(d1, d1, distances);

            // compare the new distances to the min distances
            // for each of 8 AVX2 components.
            __m256 comparison =
                    _mm256_cmp_ps(min_distances, distances, _CMP_LT_OS);

            // update min distances and indices with closest vectors if needed.
            min_distances = _mm256_min_ps(distances, min_distances);
            min_indices = _mm256_castps_si256(_mm256_blendv_ps(
                    _mm256_castsi256_ps(current_indices),
                    _mm256_castsi256_ps(min_indices),
                    comparison));

            // update current indices values. Basically, +8 to each of the
            // 8 AVX2 components.
            current_indices =
                    _mm256_add_epi32(current_indices, indices_increment);

            // scroll y forward (8 vectors 2 DIM each).
            y += 16;
        }

        // dump values and find the minimum distance / minimum index
        float min_distances_scalar[8];
        uint32_t min_indices_scalar[8];
        _mm256_storeu_ps(min_distances_scalar, min_distances);
        _mm256_storeu_si256((__m256i*)(min_indices_scalar), min_indices);

        for (uint32 j = 0; j < 8; j++) {
            if (current_min_distance > min_distances_scalar[j]) {
                current_min_distance = min_distances_scalar[j];
                current_min_index = min_indices_scalar[j];
            }
        }
    }

    if (i < ny) {
        // process leftovers.
        // the following code is not optimal, but it is rarely invoked.
        float x0 = x[0];
        float x1 = x[1];

        for (; i < ny; i++) {
            float sub0 = x0 - y[0];
            float sub1 = x1 - y[1];
            float distance = sub0 * sub0 + sub1 * sub1;

            y += 2;

            if (current_min_distance > distance) {
                current_min_distance = distance;
                current_min_index = i;
            }
        }
    }

    return current_min_index;
}

uint32 fvec_L2sqr_ny_nearest_D4(
        float* distances_tmp_buffer,
        const float* x,
        const float* y,
        uint32 ny) {
    // this implementation does not use distances_tmp_buffer.

    // current index being processed
    uint32 i = 0;

    // min distance and the index of the closest vector so far
    float current_min_distance = HUGE_VALF;
    uint32 current_min_index = 0;

    // process 8 D4-vectors per loop.
    const uint32 ny8 = ny / 8;

    if (ny8 > 0) {
        // track min distance and the closest vector independently
        // for each of 8 AVX2 components.
        __m256 min_distances = _mm256_set1_ps(HUGE_VALF);
        __m256i min_indices = _mm256_set1_epi32(0);

        __m256i current_indices = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
        const __m256i indices_increment = _mm256_set1_epi32(8);

        // 1 value per register
        const __m256 m0 = _mm256_set1_ps(x[0]);
        const __m256 m1 = _mm256_set1_ps(x[1]);
        const __m256 m2 = _mm256_set1_ps(x[2]);
        const __m256 m3 = _mm256_set1_ps(x[3]);

        for (; i < ny8 * 8; i += 8) {
            __m256 v0;
            __m256 v1;
            __m256 v2;
            __m256 v3;

            transpose_8x4(
                    _mm256_loadu_ps(y + 0 * 8),
                    _mm256_loadu_ps(y + 1 * 8),
                    _mm256_loadu_ps(y + 2 * 8),
                    _mm256_loadu_ps(y + 3 * 8),
                    v0,
                    v1,
                    v2,
                    v3);

            // compute differences
            const __m256 d0 = _mm256_sub_ps(m0, v0);
            const __m256 d1 = _mm256_sub_ps(m1, v1);
            const __m256 d2 = _mm256_sub_ps(m2, v2);
            const __m256 d3 = _mm256_sub_ps(m3, v3);

            // compute squares of differences
            __m256 distances = _mm256_mul_ps(d0, d0);
            distances = _mm256_fmadd_ps(d1, d1, distances);
            distances = _mm256_fmadd_ps(d2, d2, distances);
            distances = _mm256_fmadd_ps(d3, d3, distances);

            // compare the new distances to the min distances
            // for each of 8 AVX2 components.
            __m256 comparison =
                    _mm256_cmp_ps(min_distances, distances, _CMP_LT_OS);

            // update min distances and indices with closest vectors if needed.
            min_distances = _mm256_min_ps(distances, min_distances);
            min_indices = _mm256_castps_si256(_mm256_blendv_ps(
                    _mm256_castsi256_ps(current_indices),
                    _mm256_castsi256_ps(min_indices),
                    comparison));

            // update current indices values. Basically, +8 to each of the
            // 8 AVX2 components.
            current_indices =
                    _mm256_add_epi32(current_indices, indices_increment);

            // scroll y forward (8 vectors 4 DIM each).
            y += 32;
        }

        // dump values and find the minimum distance / minimum index
        float min_distances_scalar[8];
        uint32_t min_indices_scalar[8];
        _mm256_storeu_ps(min_distances_scalar, min_distances);
        _mm256_storeu_si256((__m256i*)(min_indices_scalar), min_indices);

        for (uint32 j = 0; j < 8; j++) {
            if (current_min_distance > min_distances_scalar[j]) {
                current_min_distance = min_distances_scalar[j];
                current_min_index = min_indices_scalar[j];
            }
        }
    }

    if (i < ny) {
        // process leftovers
        __m128 x0 = _mm_loadu_ps(x);

        for (; i < ny; i++) {
            __m128 accu = ElementOp256L2::op(x0, _mm_loadu_ps(y));
            y += 4;
            const float distance = horizontal_sum(accu);

            if (current_min_distance > distance) {
                current_min_distance = distance;
                current_min_index = i;
            }
        }
    }

    return current_min_index;
}

uint32 fvec_L2sqr_ny_nearest_D8(
        float* distances_tmp_buffer,
        const float* x,
        const float* y,
        uint32 ny) {
    // this implementation does not use distances_tmp_buffer.

    // current index being processed
    uint32 i = 0;

    // min distance and the index of the closest vector so far
    float current_min_distance = HUGE_VALF;
    uint32 current_min_index = 0;

    // process 8 D8-vectors per loop.
    const uint32 ny8 = ny / 8;
    if (ny8 > 0) {
        // track min distance and the closest vector independently
        // for each of 8 AVX2 components.
        __m256 min_distances = _mm256_set1_ps(HUGE_VALF);
        __m256i min_indices = _mm256_set1_epi32(0);

        __m256i current_indices = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
        const __m256i indices_increment = _mm256_set1_epi32(8);

        // 1 value per register
        const __m256 m0 = _mm256_set1_ps(x[0]);
        const __m256 m1 = _mm256_set1_ps(x[1]);
        const __m256 m2 = _mm256_set1_ps(x[2]);
        const __m256 m3 = _mm256_set1_ps(x[3]);

        const __m256 m4 = _mm256_set1_ps(x[4]);
        const __m256 m5 = _mm256_set1_ps(x[5]);
        const __m256 m6 = _mm256_set1_ps(x[6]);
        const __m256 m7 = _mm256_set1_ps(x[7]);

        for (; i < ny8 * 8; i += 8) {
            __m256 v0;
            __m256 v1;
            __m256 v2;
            __m256 v3;
            __m256 v4;
            __m256 v5;
            __m256 v6;
            __m256 v7;

            transpose_8x8(
                    _mm256_loadu_ps(y + 0 * 8),
                    _mm256_loadu_ps(y + 1 * 8),
                    _mm256_loadu_ps(y + 2 * 8),
                    _mm256_loadu_ps(y + 3 * 8),
                    _mm256_loadu_ps(y + 4 * 8),
                    _mm256_loadu_ps(y + 5 * 8),
                    _mm256_loadu_ps(y + 6 * 8),
                    _mm256_loadu_ps(y + 7 * 8),
                    v0,
                    v1,
                    v2,
                    v3,
                    v4,
                    v5,
                    v6,
                    v7);

            // compute differences
            const __m256 d0 = _mm256_sub_ps(m0, v0);
            const __m256 d1 = _mm256_sub_ps(m1, v1);
            const __m256 d2 = _mm256_sub_ps(m2, v2);
            const __m256 d3 = _mm256_sub_ps(m3, v3);
            const __m256 d4 = _mm256_sub_ps(m4, v4);
            const __m256 d5 = _mm256_sub_ps(m5, v5);
            const __m256 d6 = _mm256_sub_ps(m6, v6);
            const __m256 d7 = _mm256_sub_ps(m7, v7);

            // compute squares of differences
            __m256 distances = _mm256_mul_ps(d0, d0);
            distances = _mm256_fmadd_ps(d1, d1, distances);
            distances = _mm256_fmadd_ps(d2, d2, distances);
            distances = _mm256_fmadd_ps(d3, d3, distances);
            distances = _mm256_fmadd_ps(d4, d4, distances);
            distances = _mm256_fmadd_ps(d5, d5, distances);
            distances = _mm256_fmadd_ps(d6, d6, distances);
            distances = _mm256_fmadd_ps(d7, d7, distances);

            // compare the new distances to the min distances
            // for each of 8 AVX2 components.
            __m256 comparison =
                    _mm256_cmp_ps(min_distances, distances, _CMP_LT_OS);

            // update min distances and indices with closest vectors if needed.
            min_distances = _mm256_min_ps(distances, min_distances);
            min_indices = _mm256_castps_si256(_mm256_blendv_ps(
                    _mm256_castsi256_ps(current_indices),
                    _mm256_castsi256_ps(min_indices),
                    comparison));

            // update current indices values. Basically, +8 to each of the
            // 8 AVX2 components.
            current_indices =
                    _mm256_add_epi32(current_indices, indices_increment);

            // scroll y forward (8 vectors 8 DIM each).
            y += 64;
        }

        // dump values and find the minimum distance / minimum index
        float min_distances_scalar[8];
        uint32_t min_indices_scalar[8];
        _mm256_storeu_ps(min_distances_scalar, min_distances);
        _mm256_storeu_si256((__m256i*)(min_indices_scalar), min_indices);

        for (uint32 j = 0; j < 8; j++) {
            if (current_min_distance > min_distances_scalar[j]) {
                current_min_distance = min_distances_scalar[j];
                current_min_index = min_indices_scalar[j];
            }
        }
    }

    if (i < ny) {
        // process leftovers
        __m256 x0 = _mm256_loadu_ps(x);

        for (; i < ny; i++) {
            __m256 accu = ElementOp256L2::op(x0, _mm256_loadu_ps(y));
            y += 8;
            const float distance = horizontal_sum(accu);

            if (current_min_distance > distance) {
                current_min_distance = distance;
                current_min_index = i;
            }
        }
    }

    return current_min_index;
}

//#else
// uint32 fvec_L2sqr_ny_nearest_D2(
//         float* distances_tmp_buffer,
//         const float* x,
//         const float* y,
//         uint32 ny) {
//     return fvec_L2sqr_ny_nearest_ref(distances_tmp_buffer, x, y, 2, ny);
// }

// uint32 fvec_L2sqr_ny_nearest_D4(
//         float* distances_tmp_buffer,
//         const float* x,
//         const float* y,
//         uint32 ny) {
//     return fvec_L2sqr_ny_nearest_ref(distances_tmp_buffer, x, y, 4, ny);
// }

// uint32 fvec_L2sqr_ny_nearest_D8(
//         float* distances_tmp_buffer,
//         const float* x,
//         const float* y,
//         uint32 ny) {
//     return fvec_L2sqr_ny_nearest_ref(distances_tmp_buffer, x, y, 8, ny);
// }
#endif


uint32 DISTANCE_FUNC_NAME(fvec_L2sqr_ny_nearest_ref)(
        float* distances_tmp_buffer,
        const float* x,
        const float* y,
        uint32 d,
        uint32 ny) {
    ann_helper::DISTANCE_FUNC_NAME(fvec_L2sqr_ny)(distances_tmp_buffer, x, y, d, ny);

    uint32 nearest_idx = 0;
    float min_dis = HUGE_VALF;

    for (uint32 i = 0; i < ny; i++) {
        if (distances_tmp_buffer[i] < min_dis) {
            min_dis = distances_tmp_buffer[i];
            nearest_idx = i;
        }
    }

    return nearest_idx;
}

uint32 ann_helper::DISTANCE_FUNC_NAME(fvec_L2sqr_ny_nearest)(
        float* distances_tmp_buffer,
        const float* x,
        const float* y,
        uint32 d,
        uint32 ny) {

#if defined(__AVX512_SUPPORT__)
#define DISPATCH(dval) \
    case dval:         \
        return fvec_L2sqr_ny_nearest_avx512_D##dval(distances_tmp_buffer, x, y, ny);
    switch (d) {
        DISPATCH(2)
        DISPATCH(4)
        DISPATCH(8)
        default:
            return DISTANCE_FUNC_NAME(fvec_L2sqr_ny_nearest_ref)(distances_tmp_buffer, x, y, d, ny);
    }
#undef DISPATCH

#elif defined(__AVX_SUPPORT__)       
    // optimized for a few special cases
#define DISPATCH(dval) \
    case dval:         \
        return fvec_L2sqr_ny_nearest_D##dval(distances_tmp_buffer, x, y, ny);

    switch (d) {
        DISPATCH(2)
        DISPATCH(4)
        DISPATCH(8)
        default:
            return DISTANCE_FUNC_NAME(fvec_L2sqr_ny_nearest_ref)(distances_tmp_buffer, x, y, d, ny);
    }
#undef DISPATCH

#else
    return DISTANCE_FUNC_NAME(fvec_L2sqr_ny_nearest_ref)(distances_tmp_buffer, x, y, d, ny);
#endif
}

#endif /* __SSE_SUPPORT__ */

#ifdef __AVX_SUPPORT__

#elif defined(__SSE_SUPPORT__) // But not AVX

#elif defined(__SVE_SUPPORT__)

struct ElementOpIP {
    static svfloat32_t op(svbool_t pg, svfloat32_t x, svfloat32_t y) {
        return svmul_f32_x(pg, x, y);
    }
    static svfloat32_t merge(
            svbool_t pg,
            svfloat32_t z,
            svfloat32_t x,
            svfloat32_t y) {
        return svmla_f32_x(pg, z, x, y);
    }
};

template <typename ElementOp>
void fvec_op_ny_sve_d1(float* dis, const float* x, const float* y, uint32 ny) {
    const uint32 lanes = svcntw();
    const uint32 lanes2 = lanes * 2;
    const uint32 lanes3 = lanes * 3;
    const uint32 lanes4 = lanes * 4;
    const svbool_t pg = svptrue_b32();
    const svfloat32_t x0 = svdup_n_f32(x[0]);
    uint32 i = 0;
    for (; i + lanes4 < ny; i += lanes4) {
        svfloat32_t y0 = svld1_f32(pg, y);
        svfloat32_t y1 = svld1_f32(pg, y + lanes);
        svfloat32_t y2 = svld1_f32(pg, y + lanes2);
        svfloat32_t y3 = svld1_f32(pg, y + lanes3);
        y0 = ElementOp::op(pg, x0, y0);
        y1 = ElementOp::op(pg, x0, y1);
        y2 = ElementOp::op(pg, x0, y2);
        y3 = ElementOp::op(pg, x0, y3);
        svst1_f32(pg, dis, y0);
        svst1_f32(pg, dis + lanes, y1);
        svst1_f32(pg, dis + lanes2, y2);
        svst1_f32(pg, dis + lanes3, y3);
        y += lanes4;
        dis += lanes4;
    }
    const svbool_t pg0 = svwhilelt_b32_u64(i, ny);
    const svbool_t pg1 = svwhilelt_b32_u64(i + lanes, ny);
    const svbool_t pg2 = svwhilelt_b32_u64(i + lanes2, ny);
    const svbool_t pg3 = svwhilelt_b32_u64(i + lanes3, ny);
    svfloat32_t y0 = svld1_f32(pg0, y);
    svfloat32_t y1 = svld1_f32(pg1, y + lanes);
    svfloat32_t y2 = svld1_f32(pg2, y + lanes2);
    svfloat32_t y3 = svld1_f32(pg3, y + lanes3);
    y0 = ElementOp::op(pg0, x0, y0);
    y1 = ElementOp::op(pg1, x0, y1);
    y2 = ElementOp::op(pg2, x0, y2);
    y3 = ElementOp::op(pg3, x0, y3);
    svst1_f32(pg0, dis, y0);
    svst1_f32(pg1, dis + lanes, y1);
    svst1_f32(pg2, dis + lanes2, y2);
    svst1_f32(pg3, dis + lanes3, y3);
}

template <typename ElementOp>
void fvec_op_ny_sve_d2(float* dis, const float* x, const float* y, uint32 ny) {
    const uint32 lanes = svcntw();
    const uint32 lanes2 = lanes * 2;
    const uint32 lanes4 = lanes * 4;
    const svbool_t pg = svptrue_b32();
    const svfloat32_t x0 = svdup_n_f32(x[0]);
    const svfloat32_t x1 = svdup_n_f32(x[1]);
    uint32 i = 0;
    for (; i + lanes2 < ny; i += lanes2) {
        const svfloat32x2_t y0 = svld2_f32(pg, y);
        const svfloat32x2_t y1 = svld2_f32(pg, y + lanes2);
        svfloat32_t y00 = svget2_f32(y0, 0);
        const svfloat32_t y01 = svget2_f32(y0, 1);
        svfloat32_t y10 = svget2_f32(y1, 0);
        const svfloat32_t y11 = svget2_f32(y1, 1);
        y00 = ElementOp::op(pg, x0, y00);
        y10 = ElementOp::op(pg, x0, y10);
        y00 = ElementOp::merge(pg, y00, x1, y01);
        y10 = ElementOp::merge(pg, y10, x1, y11);
        svst1_f32(pg, dis, y00);
        svst1_f32(pg, dis + lanes, y10);
        y += lanes4;
        dis += lanes2;
    }
    const svbool_t pg0 = svwhilelt_b32_u64(i, ny);
    const svbool_t pg1 = svwhilelt_b32_u64(i + lanes, ny);
    const svfloat32x2_t y0 = svld2_f32(pg0, y);
    const svfloat32x2_t y1 = svld2_f32(pg1, y + lanes2);
    svfloat32_t y00 = svget2_f32(y0, 0);
    const svfloat32_t y01 = svget2_f32(y0, 1);
    svfloat32_t y10 = svget2_f32(y1, 0);
    const svfloat32_t y11 = svget2_f32(y1, 1);
    y00 = ElementOp::op(pg0, x0, y00);
    y10 = ElementOp::op(pg1, x0, y10);
    y00 = ElementOp::merge(pg0, y00, x1, y01);
    y10 = ElementOp::merge(pg1, y10, x1, y11);
    svst1_f32(pg0, dis, y00);
    svst1_f32(pg1, dis + lanes, y10);
}

template <typename ElementOp>
void fvec_op_ny_sve_d4(float* dis, const float* x, const float* y, uint32 ny) {
    const uint32 lanes = svcntw();
    const uint32 lanes4 = lanes * 4;
    const svbool_t pg = svptrue_b32();
    const svfloat32_t x0 = svdup_n_f32(x[0]);
    const svfloat32_t x1 = svdup_n_f32(x[1]);
    const svfloat32_t x2 = svdup_n_f32(x[2]);
    const svfloat32_t x3 = svdup_n_f32(x[3]);
    uint32 i = 0;
    for (; i + lanes < ny; i += lanes) {
        const svfloat32x4_t y0 = svld4_f32(pg, y);
        svfloat32_t y00 = svget4_f32(y0, 0);
        const svfloat32_t y01 = svget4_f32(y0, 1);
        svfloat32_t y02 = svget4_f32(y0, 2);
        const svfloat32_t y03 = svget4_f32(y0, 3);
        y00 = ElementOp::op(pg, x0, y00);
        y02 = ElementOp::op(pg, x2, y02);
        y00 = ElementOp::merge(pg, y00, x1, y01);
        y02 = ElementOp::merge(pg, y02, x3, y03);
        y00 = svadd_f32_x(pg, y00, y02);
        svst1_f32(pg, dis, y00);
        y += lanes4;
        dis += lanes;
    }
    const svbool_t pg0 = svwhilelt_b32_u64(i, ny);
    const svfloat32x4_t y0 = svld4_f32(pg0, y);
    svfloat32_t y00 = svget4_f32(y0, 0);
    const svfloat32_t y01 = svget4_f32(y0, 1);
    svfloat32_t y02 = svget4_f32(y0, 2);
    const svfloat32_t y03 = svget4_f32(y0, 3);
    y00 = ElementOp::op(pg0, x0, y00);
    y02 = ElementOp::op(pg0, x2, y02);
    y00 = ElementOp::merge(pg0, y00, x1, y01);
    y02 = ElementOp::merge(pg0, y02, x3, y03);
    y00 = svadd_f32_x(pg0, y00, y02);
    svst1_f32(pg0, dis, y00);
}

template <typename ElementOp>
void fvec_op_ny_sve_d8(float* dis, const float* x, const float* y, uint32 ny) {
    const uint32 lanes = svcntw();
    const uint32 lanes4 = lanes * 4;
    const uint32 lanes8 = lanes * 8;
    const svbool_t pg = svptrue_b32();
    const svfloat32_t x0 = svdup_n_f32(x[0]);
    const svfloat32_t x1 = svdup_n_f32(x[1]);
    const svfloat32_t x2 = svdup_n_f32(x[2]);
    const svfloat32_t x3 = svdup_n_f32(x[3]);
    const svfloat32_t x4 = svdup_n_f32(x[4]);
    const svfloat32_t x5 = svdup_n_f32(x[5]);
    const svfloat32_t x6 = svdup_n_f32(x[6]);
    const svfloat32_t x7 = svdup_n_f32(x[7]);
    uint32 i = 0;
    for (; i + lanes < ny; i += lanes) {
        const svfloat32x4_t ya = svld4_f32(pg, y);
        const svfloat32x4_t yb = svld4_f32(pg, y + lanes4);
        const svfloat32_t ya0 = svget4_f32(ya, 0);
        const svfloat32_t ya1 = svget4_f32(ya, 1);
        const svfloat32_t ya2 = svget4_f32(ya, 2);
        const svfloat32_t ya3 = svget4_f32(ya, 3);
        const svfloat32_t yb0 = svget4_f32(yb, 0);
        const svfloat32_t yb1 = svget4_f32(yb, 1);
        const svfloat32_t yb2 = svget4_f32(yb, 2);
        const svfloat32_t yb3 = svget4_f32(yb, 3);
        svfloat32_t y0 = svuzp1(ya0, yb0);
        const svfloat32_t y1 = svuzp1(ya1, yb1);
        svfloat32_t y2 = svuzp1(ya2, yb2);
        const svfloat32_t y3 = svuzp1(ya3, yb3);
        svfloat32_t y4 = svuzp2(ya0, yb0);
        const svfloat32_t y5 = svuzp2(ya1, yb1);
        svfloat32_t y6 = svuzp2(ya2, yb2);
        const svfloat32_t y7 = svuzp2(ya3, yb3);
        y0 = ElementOp::op(pg, x0, y0);
        y2 = ElementOp::op(pg, x2, y2);
        y4 = ElementOp::op(pg, x4, y4);
        y6 = ElementOp::op(pg, x6, y6);
        y0 = ElementOp::merge(pg, y0, x1, y1);
        y2 = ElementOp::merge(pg, y2, x3, y3);
        y4 = ElementOp::merge(pg, y4, x5, y5);
        y6 = ElementOp::merge(pg, y6, x7, y7);
        y0 = svadd_f32_x(pg, y0, y2);
        y4 = svadd_f32_x(pg, y4, y6);
        y0 = svadd_f32_x(pg, y0, y4);
        svst1_f32(pg, dis, y0);
        y += lanes8;
        dis += lanes;
    }
    const svbool_t pg0 = svwhilelt_b32_u64(i, ny);
    const svbool_t pga = svwhilelt_b32_u64(i * 2, ny * 2);
    const svbool_t pgb = svwhilelt_b32_u64(i * 2 + lanes, ny * 2);
    const svfloat32x4_t ya = svld4_f32(pga, y);
    const svfloat32x4_t yb = svld4_f32(pgb, y + lanes4);
    const svfloat32_t ya0 = svget4_f32(ya, 0);
    const svfloat32_t ya1 = svget4_f32(ya, 1);
    const svfloat32_t ya2 = svget4_f32(ya, 2);
    const svfloat32_t ya3 = svget4_f32(ya, 3);
    const svfloat32_t yb0 = svget4_f32(yb, 0);
    const svfloat32_t yb1 = svget4_f32(yb, 1);
    const svfloat32_t yb2 = svget4_f32(yb, 2);
    const svfloat32_t yb3 = svget4_f32(yb, 3);
    svfloat32_t y0 = svuzp1(ya0, yb0);
    const svfloat32_t y1 = svuzp1(ya1, yb1);
    svfloat32_t y2 = svuzp1(ya2, yb2);
    const svfloat32_t y3 = svuzp1(ya3, yb3);
    svfloat32_t y4 = svuzp2(ya0, yb0);
    const svfloat32_t y5 = svuzp2(ya1, yb1);
    svfloat32_t y6 = svuzp2(ya2, yb2);
    const svfloat32_t y7 = svuzp2(ya3, yb3);
    y0 = ElementOp::op(pg0, x0, y0);
    y2 = ElementOp::op(pg0, x2, y2);
    y4 = ElementOp::op(pg0, x4, y4);
    y6 = ElementOp::op(pg0, x6, y6);
    y0 = ElementOp::merge(pg0, y0, x1, y1);
    y2 = ElementOp::merge(pg0, y2, x3, y3);
    y4 = ElementOp::merge(pg0, y4, x5, y5);
    y6 = ElementOp::merge(pg0, y6, x7, y7);
    y0 = svadd_f32_x(pg0, y0, y2);
    y4 = svadd_f32_x(pg0, y4, y6);
    y0 = svadd_f32_x(pg0, y0, y4);
    svst1_f32(pg0, dis, y0);
    y += lanes8;
    dis += lanes;
}

template <typename ElementOp>
void fvec_op_ny_sve_lanes1(
        float* dis,
        const float* x,
        const float* y,
        uint32 ny) {
    const uint32 lanes = svcntw();
    const uint32 lanes2 = lanes * 2;
    const uint32 lanes3 = lanes * 3;
    const uint32 lanes4 = lanes * 4;
    const svbool_t pg = svptrue_b32();
    const svfloat32_t x0 = svld1_f32(pg, x);
    uint32 i = 0;
    for (; i + 3 < ny; i += 4) {
        svfloat32_t y0 = svld1_f32(pg, y);
        svfloat32_t y1 = svld1_f32(pg, y + lanes);
        svfloat32_t y2 = svld1_f32(pg, y + lanes2);
        svfloat32_t y3 = svld1_f32(pg, y + lanes3);
        y += lanes4;
        y0 = ElementOp::op(pg, x0, y0);
        y1 = ElementOp::op(pg, x0, y1);
        y2 = ElementOp::op(pg, x0, y2);
        y3 = ElementOp::op(pg, x0, y3);
        dis[i] = svaddv_f32(pg, y0);
        dis[i + 1] = svaddv_f32(pg, y1);
        dis[i + 2] = svaddv_f32(pg, y2);
        dis[i + 3] = svaddv_f32(pg, y3);
    }
    for (; i < ny; ++i) {
        svfloat32_t y0 = svld1_f32(pg, y);
        y += lanes;
        y0 = ElementOp::op(pg, x0, y0);
        dis[i] = svaddv_f32(pg, y0);
    }
}

template <typename ElementOp>
void fvec_op_ny_sve_lanes2(
        float* dis,
        const float* x,
        const float* y,
        uint32 ny) {
    const uint32 lanes = svcntw();
    const uint32 lanes2 = lanes * 2;
    const uint32 lanes3 = lanes * 3;
    const uint32 lanes4 = lanes * 4;
    const svbool_t pg = svptrue_b32();
    const svfloat32_t x0 = svld1_f32(pg, x);
    const svfloat32_t x1 = svld1_f32(pg, x + lanes);
    uint32 i = 0;
    for (; i + 1 < ny; i += 2) {
        svfloat32_t y00 = svld1_f32(pg, y);
        const svfloat32_t y01 = svld1_f32(pg, y + lanes);
        svfloat32_t y10 = svld1_f32(pg, y + lanes2);
        const svfloat32_t y11 = svld1_f32(pg, y + lanes3);
        y += lanes4;
        y00 = ElementOp::op(pg, x0, y00);
        y10 = ElementOp::op(pg, x0, y10);
        y00 = ElementOp::merge(pg, y00, x1, y01);
        y10 = ElementOp::merge(pg, y10, x1, y11);
        dis[i] = svaddv_f32(pg, y00);
        dis[i + 1] = svaddv_f32(pg, y10);
    }
    if (i < ny) {
        svfloat32_t y0 = svld1_f32(pg, y);
        const svfloat32_t y1 = svld1_f32(pg, y + lanes);
        y0 = ElementOp::op(pg, x0, y0);
        y0 = ElementOp::merge(pg, y0, x1, y1);
        dis[i] = svaddv_f32(pg, y0);
    }
}

template <typename ElementOp>
void fvec_op_ny_sve_lanes3(
        float* dis,
        const float* x,
        const float* y,
        uint32 ny) {
    const uint32 lanes = svcntw();
    const uint32 lanes2 = lanes * 2;
    const uint32 lanes3 = lanes * 3;
    const svbool_t pg = svptrue_b32();
    const svfloat32_t x0 = svld1_f32(pg, x);
    const svfloat32_t x1 = svld1_f32(pg, x + lanes);
    const svfloat32_t x2 = svld1_f32(pg, x + lanes2);
    for (uint32 i = 0; i < ny; ++i) {
        svfloat32_t y0 = svld1_f32(pg, y);
        const svfloat32_t y1 = svld1_f32(pg, y + lanes);
        svfloat32_t y2 = svld1_f32(pg, y + lanes2);
        y += lanes3;
        y0 = ElementOp::op(pg, x0, y0);
        y0 = ElementOp::merge(pg, y0, x1, y1);
        y0 = ElementOp::merge(pg, y0, x2, y2);
        dis[i] = svaddv_f32(pg, y0);
    }
}

template <typename ElementOp>
void fvec_op_ny_sve_lanes4(
        float* dis,
        const float* x,
        const float* y,
        uint32 ny) {
    const uint32 lanes = svcntw();
    const uint32 lanes2 = lanes * 2;
    const uint32 lanes3 = lanes * 3;
    const uint32 lanes4 = lanes * 4;
    const svbool_t pg = svptrue_b32();
    const svfloat32_t x0 = svld1_f32(pg, x);
    const svfloat32_t x1 = svld1_f32(pg, x + lanes);
    const svfloat32_t x2 = svld1_f32(pg, x + lanes2);
    const svfloat32_t x3 = svld1_f32(pg, x + lanes3);
    for (uint32 i = 0; i < ny; ++i) {
        svfloat32_t y0 = svld1_f32(pg, y);
        const svfloat32_t y1 = svld1_f32(pg, y + lanes);
        svfloat32_t y2 = svld1_f32(pg, y + lanes2);
        const svfloat32_t y3 = svld1_f32(pg, y + lanes3);
        y += lanes4;
        y0 = ElementOp::op(pg, x0, y0);
        y2 = ElementOp::op(pg, x2, y2);
        y0 = ElementOp::merge(pg, y0, x1, y1);
        y2 = ElementOp::merge(pg, y2, x3, y3);
        y0 = svadd_f32_x(pg, y0, y2);
        dis[i] = svaddv_f32(pg, y0);
    }
}

void ann_helper::DISTANCE_FUNC_NAME(fvec_L2sqr_ny)(
        float* dis,
        const float* x,
        const float* y,
        uint32 d,
        uint32 ny) {
    DISTANCE_FUNC_NAME(fvec_L2sqr_ny_ref)(dis, x, y, d, ny);
}

uint32 DISTANCE_FUNC_NAME(fvec_L2sqr_ny_nearest_ref)(
        float* distances_tmp_buffer,
        const float* x,
        const float* y,
        uint32 d,
        uint32 ny) {
    ann_helper::DISTANCE_FUNC_NAME(fvec_L2sqr_ny)(distances_tmp_buffer, x, y, d, ny);

    uint32 nearest_idx = 0;
    float min_dis = HUGE_VALF;

    for (uint32 i = 0; i < ny; i++) {
        if (distances_tmp_buffer[i] < min_dis) {
            min_dis = distances_tmp_buffer[i];
            nearest_idx = i;
        }
    }

    return nearest_idx;
}

uint32 ann_helper::DISTANCE_FUNC_NAME(fvec_L2sqr_ny_nearest)(
        float* distances_tmp_buffer,
        const float* x,
        const float* y,
        uint32 d,
        uint32 ny) {
    return DISTANCE_FUNC_NAME(fvec_L2sqr_ny_nearest_ref)(distances_tmp_buffer, x, y, d, ny);
}

void ann_helper::DISTANCE_FUNC_NAME(fvec_inner_products_ny)(
        float* dis,
        const float* x,
        const float* y,
        uint32 d,
        uint32 ny) {
    const uint32 lanes = svcntw();
    switch (d) {
        case 1:
            fvec_op_ny_sve_d1<ElementOpIP>(dis, x, y, ny);
            break;
        case 2:
            fvec_op_ny_sve_d2<ElementOpIP>(dis, x, y, ny);
            break;
        case 4:
            fvec_op_ny_sve_d4<ElementOpIP>(dis, x, y, ny);
            break;
        case 8:
            fvec_op_ny_sve_d8<ElementOpIP>(dis, x, y, ny);
            break;
        default:
            if (d == lanes)
                fvec_op_ny_sve_lanes1<ElementOpIP>(dis, x, y, ny);
            else if (d == lanes * 2)
                fvec_op_ny_sve_lanes2<ElementOpIP>(dis, x, y, ny);
            else if (d == lanes * 3)
                fvec_op_ny_sve_lanes3<ElementOpIP>(dis, x, y, ny);
            else if (d == lanes * 4)
                fvec_op_ny_sve_lanes4<ElementOpIP>(dis, x, y, ny);
            else
                DISTANCE_FUNC_NAME(fvec_inner_products_ny_ref)(dis, x, y, d, ny);
            break;
    }
}


#elif COMPILER_TARGET_AARCH64
// not optimized for ARM
void ann_helper::DISTANCE_FUNC_NAME(fvec_L2sqr_ny)(
        float* dis,
        const float* x,
        const float* y,
        uint32 d,
        uint32 ny) {
    DISTANCE_FUNC_NAME(fvec_L2sqr_ny_ref)(dis, x, y, d, ny);
}

uint32 DISTANCE_FUNC_NAME(fvec_L2sqr_ny_nearest_ref)(
        float* distances_tmp_buffer,
        const float* x,
        const float* y,
        uint32 d,
        uint32 ny) {
    ann_helper::DISTANCE_FUNC_NAME(fvec_L2sqr_ny)(distances_tmp_buffer, x, y, d, ny);

    uint32 nearest_idx = 0;
    float min_dis = HUGE_VALF;

    for (uint32 i = 0; i < ny; i++) {
        if (distances_tmp_buffer[i] < min_dis) {
            min_dis = distances_tmp_buffer[i];
            nearest_idx = i;
        }
    }

    return nearest_idx;
}

uint32 ann_helper::DISTANCE_FUNC_NAME(fvec_L2sqr_ny_nearest)(
        float* distances_tmp_buffer,
        const float* x,
        const float* y,
        uint32 d,
        uint32 ny) {
    return DISTANCE_FUNC_NAME(fvec_L2sqr_ny_nearest_ref)(distances_tmp_buffer, x, y, d, ny);
}

void ann_helper::DISTANCE_FUNC_NAME(fvec_inner_products_ny)(
        float* dis,
        const float* x,
        const float* y,
        uint32 d,
        uint32 ny) {
    DISTANCE_FUNC_NAME(fvec_inner_products_ny_ref)(dis, x, y, d, ny);
}

#else
// scalar implementation

void ann_helper::DISTANCE_FUNC_NAME(fvec_L2sqr_ny)(
        float* dis,
        const float* x,
        const float* y,
        uint32 d,
        uint32 ny) {
    DISTANCE_FUNC_NAME(fvec_L2sqr_ny_ref)(dis, x, y, d, ny);
}

uint32 DISTANCE_FUNC_NAME(fvec_L2sqr_ny_nearest_ref)(
        float* distances_tmp_buffer,
        const float* x,
        const float* y,
        uint32 d,
        uint32 ny) {
    ann_helper::DISTANCE_FUNC_NAME(fvec_L2sqr_ny)(distances_tmp_buffer, x, y, d, ny);

    uint32 nearest_idx = 0;
    float min_dis = HUGE_VALF;

    for (uint32 i = 0; i < ny; i++) {
        if (distances_tmp_buffer[i] < min_dis) {
            min_dis = distances_tmp_buffer[i];
            nearest_idx = i;
        }
    }

    return nearest_idx;
}

uint32 ann_helper::DISTANCE_FUNC_NAME(fvec_L2sqr_ny_nearest)(
        float* distances_tmp_buffer,
        const float* x,
        const float* y,
        uint32 d,
        uint32 ny) {       
    return DISTANCE_FUNC_NAME(fvec_L2sqr_ny_nearest_ref)(distances_tmp_buffer, x, y, d, ny);
}

void ann_helper::DISTANCE_FUNC_NAME(fvec_inner_products_ny)(
        float* dis,
        const float* x,
        const float* y,
        uint32 d,
        uint32 ny) {
    DISTANCE_FUNC_NAME(fvec_inner_products_ny_ref)(dis, x, y, d, ny);
}
#endif
