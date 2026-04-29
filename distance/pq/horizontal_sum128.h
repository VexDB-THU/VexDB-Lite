#ifndef HORIZONTAL_SUM128_H
#define HORIZONTAL_SUM128_H


#ifdef __SSE_SUPPORT__
#include <immintrin.h>

inline float horizontal_sum(const __m128 v) {
    const __m128 v0 = _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 3, 2));
    const __m128 v1 = _mm_add_ps(v, v0);
    __m128 v2 = _mm_shuffle_ps(v1, v1, _MM_SHUFFLE(0, 0, 0, 1));
    const __m128 v3 = _mm_add_ps(v1, v2);
    return _mm_cvtss_f32(v3);
}

#endif

#endif
