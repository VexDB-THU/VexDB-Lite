/**
 * Copyright ...
 * OpenBLAS interface.
 */

#ifndef CBLAS_INTERFACE_H
#define CBLAS_INTERFACE_H

#include "pg_config.h"
#include "pg_compat.h"
#ifdef ENABLE_OPENBLAS
#include <cblas.h>

inline void cblas_sgemm_rnt(const int M, const int N, const int K,
    const float alpha, const float *A, const int lda,
    const float *B, const int ldb, const float beta,
    float *C, const int ldc)
{
    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans, M, N, K, alpha, A, lda, B, ldb, beta, C, ldc);
}

inline float snrm_sqr(const int N, const float *X, const int incX)
{
    float res = cblas_snrm2(N, X, incX);
    return res * res;
}

inline void move_sscal(const int N, const float alpha, const float *X, const int incX, float *res)
{
    if (X != res) {   
        cblas_scopy(N, X, incX, res, incX);
    }
    cblas_sscal(N, alpha, res, incX);
}

#else
#include <cmath>

inline float cblas_sdot(const int N, const float *X, const int incX, const float *Y, const int incY)
{
    float dot = 0;
    for (int i = 0; i < N; ++i) {
        dot += *X * *Y;
        X += incX;
        Y += incY;
    }
    return dot;
}

inline void cblas_sgemm_rnt(const int M, const int N, const int K,
                     const float alpha, const float *A, const int lda,
                     const float *B, const int ldb, const float beta,
                     float *C, const int ldc)
{
    for (int i = 0; i < M; ++i) {
        for (int j = 0; j < N; ++j) {
            float sum = 0;
            for (int k = 0; k < K; ++k) {
                sum += A[i * lda + k] * B[k * ldb + j];
            }
            C[i * ldc + j] = alpha * sum + beta * C[i * ldc + j];
        }
    }
}

inline size_t cblas_isamin(const int N, const float *X, const int incX)
{
    size_t min_index = 0;
    float min_val = *X;
    for (int i = 1; i < N; ++i) {
        X += incX;
        if (*X < min_val) {
            min_val = *X;
            min_index = i;
        }
    }
    return min_index;
}

inline float cblas_snrm2(const int N, const float *X, const int incX)
{
    float sum = 0;
    for (int i = 0; i < N; ++i) {
        sum += *X * *X;
        X += incX;
    }
    return std::sqrt(sum);
}

inline float snrm_sqr(const int N, const float *X, const int incX)
{
    float sum = 0;
    for (int i = 0; i < N; ++i) {
        sum += *X * *X;
        X += incX;
    }
    return sum;
}

inline void move_sscal(const int N, const float alpha, const float *X, const int incX, float *res)
{
    for (int i = 0; i < N; ++i) {
        res[i] = alpha * X[i * incX];
    }
}

#endif /* ENABLE_OPENBLAS */
#endif /* CBLAS_INTERFACE_H */
