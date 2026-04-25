#ifndef HORIZONTAL_SUM256_H
#define HORIZONTAL_SUM256_H




#ifdef __AVX_SUPPORT__

// Computes a horizontal sum over an __m256 register
inline float horizontal_sum(const __m256 v) {
    const __m128 v0 =
            _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
    return horizontal_sum(v0);
}

#endif

#endif
