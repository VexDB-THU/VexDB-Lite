#ifndef HORIZONTAL_SUM512_H
#define HORIZONTAL_SUM512_H


#ifdef __AVX512_SUPPORT__
#include <immintrin.h>

/// helper function for AVX512
inline float horizontal_sum(const __m512 v) {
    // performs better than adding the high and low parts
    return _mm512_reduce_add_ps(v);
}

#endif

#endif
