/**
 * Copyright ...
 */

#ifndef DISTANCE_TEMPLATE_H
#define DISTANCE_TEMPLATE_H

#include <math.h>
#include <vtl/expr_helper>
#include <vtl/array>

#include "distance/core/distance.h"

#if defined(PG_VEXDB_TARGET_DUCK)
#ifndef Assert
#define Assert(cond) ((void)0)
#endif
#ifndef Assume
#define Assume(cond) ((void)0)
#endif
#endif

#define ASSUME_ALIGNED(v) v = (const plaint *)__builtin_assume_aligned(v, 64) // TD vector_aligned_size

template <std::size_t N, typename T, std::size_t... i>
constexpr Array<T, N> initialize_array_with_value(T v, std::index_sequence<i...> = {}) {
    const auto get_rid_of_index = [](T v, auto index) { return v; };
    if constexpr (sizeof...(i) != N) {
        return initialize_array_with_value<N, T>(v, std::make_index_sequence<N>());
    } else {
        return Array<T, N>{get_rid_of_index(v, i)...};
    }
}

/* Policy interface requirements for DistanceHelper operations:
 *
 * Required types:
 *   - PlainT: Plain data type (e.g., float, half, int8_t)
 *   - VecT: SIMD vector type (e.g., __m256, __m512, __m256i, __m512i)
 *   - IntmT: Intermediate type for computations (e.g., float for FLOAT/HALF, int16_t for INT8)
 *   - AccT: Accumulator type (same as VecT or different for INT8)
 *
 * Required static constexpr members:
 *   - k: uint16 - unroll factor (number of SIMD vectors per iteration)
 *   - k_per_iter: uint16 - SIMD width in elements (e.g., 8 for AVX, 16 for AVX512)
 *   - use_asm_code: bool - whether to use assembly fallback
 *   - use_custom_code: bool - whether to use custom SIMD implementation
 *   - is_aligned: bool - whether memory is aligned
 *   - rem_before_reduce: bool - whether remainder is handled before reduction
 *   - rem_after_reduce: bool - whether remainder is handled after reduction
 *   - RS: RemainderSituation - remainder handling strategy (Unknown, NoPartial, NoTail)
 *
 * Required vector arithmetic methods:
 *   - static INLINE_PROP AccT get_zero_vectors() - return zero accumulator vector
 *   - static INLINE_PROP VecT sub_vectors(VecT a, VecT b) - subtract vector b from a
 *   - static INLINE_PROP AccT madd_vectors(VecT a, VecT b, AccT s) - compute a*b + s
 *   - static INLINE_PROP AccT madd_square(VecT a, AccT s) - compute a*a + s
 *   - static INLINE_PROP VecT add_vectors(VecT a, VecT b) - add two vectors element-wise
 *
 * Required load methods:
 *   - template<bool aligned_input> static INLINE_PROP Array<VecT, k> loadk(const PlainT *v) - load k vectors
 *   - template<bool aligned_input> static INLINE_PROP VecT load(const PlainT *v) - load single vector
 *
 * Remainder handling (optional, used when RS == RemainderSituation::Unknown):
 *   - static INLINE_PROP AccT deal_remainder(const PlainT *x, const PlainT *y, uint16 dim, uint16 n_tail, AccT acc) - handle tail elements
 *   - static INLINE_PROP float deal_remainder(const PlainT *x, const PlainT *y, uint16 dim, uint16 n_tail) - if !rem_before_reduce
 *
 * Reduction method:
 *   - static INLINE_PROP float reduce(const Array<AccT, k> &accs) - reduce k accumulators to single float
 *
 * Assembly fallback (optional, only when use_asm_code == true):
 *   - static INLINE_PROP float asm_code(const PlainT *x, const PlainT *y, uint16 dim) - assembly implementation
 * Required only if not use_custom_code:
 *   - static INLINE_PROP IntmT transform(PlainT v) - transform plain type to intermediate type
 */
template <typename Policy>
struct DistanceHelper {
    using plaint = typename Policy::PlainT;
    using vect = typename Policy::VecT;
    using intm = typename Policy::IntmT;
    using acc = typename Policy::AccT;
    static constexpr uint16 k = Policy::k;
    static constexpr uint16 k_per_iter = Policy::k_per_iter;
    static constexpr uint16 k_step = k_per_iter * k;
    static constexpr uint16 k_bytes = sizeof(plaint) * k_step;
    static constexpr uint16 skip_step = 4;
    static constexpr uint16 cache_size = 64;
    static constexpr bool has_partial_tail = Policy::RS != RemainderSituation::NoPartial;
    static constexpr bool has_rem_tail = Policy::RS == RemainderSituation::Unknown;

    static INLINE_PROP float l2_distance(const void *xx, const void *yy, uint16 dim)
    {
        const plaint *x = (const plaint *)xx;
        const plaint *y = (const plaint *)yy;

        CONSTEXPR_IF (Policy::use_asm_code) {
            return Policy::asm_code(x, y, dim);
        } else CONSTEXPR_IF (!Policy::use_custom_code) {
            plaint sum = 0;
            CONSTEXPR_IF (Policy::is_aligned) {
                ASSUME_ALIGNED(x);
                ASSUME_ALIGNED(y);
            }
            for (uint16 i = 0; i != dim; ++i) {
                const intm diff = (intm)Policy::transform(x[i]) - (intm)Policy::transform(y[i]);
                sum += (plaint)(diff * diff);
            }
            return sum;
        } else {
            const uint16 n_full = dim / k_step;
            const uint16 n_tail = (dim % k_step) / k_per_iter;
            Array<acc, k> accs =
                initialize_array_with_value<k>(Policy::get_zero_vectors());
            for (uint16 i = 0; i < n_full; ++i) {
                if (i >= skip_step && (!has_partial_tail || i + skip_step < n_full)) {
                    ann_helper::unroll<k_bytes / cache_size>([&](auto j) -> void {
                        unpolluted_prefetch((char *)y + j * cache_size + skip_step * k_bytes);
                    });
                }
                Array<vect, k> xs = Policy::template loadk<Policy::is_aligned>(x);
                Array<vect, k> ys = Policy::template loadk<Policy::is_aligned>(y);
                ann_helper::unroll<k>([&](auto i) -> void {
                    vect diff = Policy::sub_vectors(xs[i], ys[i]);
                    accs[i] = Policy::madd_vectors(diff, diff, accs[i]);
                });
                x += k_step;
                y += k_step;
            }

            CONSTEXPR_IF (has_partial_tail) {
                /* partial unroll */
                ann_helper::unroll<k - 1>([&](auto i) -> bool {
                    if (i >= n_tail) { return false; }
                    vect x0 = Policy::template load<Policy::is_aligned>(x + k_per_iter * i);
                    vect y0 = Policy::template load<Policy::is_aligned>(y + k_per_iter * i);
                    vect diff = Policy::sub_vectors(x0, y0);
                    accs[i] = Policy::madd_vectors(diff, diff, accs[i]);
                    return true;
                });
            }

            CONSTEXPR_IF (has_rem_tail && Policy::rem_before_reduce) {
                accs[k - 1] = Policy::deal_remainder(x, y, dim, n_tail, accs[k - 1]);
            }
            float res = Policy::reduce(accs);
            CONSTEXPR_IF (has_rem_tail && !Policy::rem_before_reduce) {
                res += Policy::deal_remainder(x, y, dim, n_tail);
            }
            return res;
        }
    }

    static INLINE_PROP float cosine_distance(const void *xx, const void *yy, uint16 dim)
    {
        const plaint *x = (const plaint *)xx;
        const plaint *y = (const plaint *)yy;

        CONSTEXPR_IF (Policy::use_asm_code) {
            return Policy::asm_code(x, y, dim);
        } else CONSTEXPR_IF (!Policy::use_custom_code) {
            intm dot = 0;
            intm norm_x = 0;
            intm norm_y = 0;
            CONSTEXPR_IF (Policy::is_aligned) {
                ASSUME_ALIGNED(x);
                ASSUME_ALIGNED(y);
            }
            for (uint16 i = 0; i < dim; i++) {
                intm xf = (intm)Policy::transform(x[i]);
                intm yf = (intm)Policy::transform(y[i]);
                dot += xf * yf;
                norm_x += xf * xf;
                norm_y += yf * yf;
            }
            return -(float)dot / (sqrtf((float)norm_x * (float)norm_y) + __FLT_EPSILON__);
        } else {
            const uint16 n_full = dim / k_step;
            const uint16 n_tail = (dim % k_step) / k_per_iter;

            Array<acc, k> accs =
                initialize_array_with_value<k>(Policy::get_zero_vectors());
            Array<acc, k> nxs =
                initialize_array_with_value<k>(Policy::get_zero_vectors());
            Array<acc, k> nys =
                initialize_array_with_value<k>(Policy::get_zero_vectors());

            for (uint16 i = 0; i < n_full; ++i) {
                if (i >= skip_step && (!has_partial_tail || i + skip_step < n_full)) {
                    ann_helper::unroll<k_bytes / cache_size>([&](auto j) -> void {
                        unpolluted_prefetch((char *)y + j * cache_size + skip_step * k_bytes);
                    });
                }
                Array<vect, k> xs = Policy::template loadk<Policy::is_aligned>(x);
                Array<vect, k> ys = Policy::template loadk<Policy::is_aligned>(y);
                ann_helper::unroll<k>([&](auto i) -> void {
                    accs[i] = Policy::madd_vectors(xs[i], ys[i], accs[i]);
                    nxs[i] = Policy::madd_square(xs[i], nxs[i]);
                    nys[i] = Policy::madd_square(ys[i], nys[i]);
                });
                x += k_step;
                y += k_step;
            }

            CONSTEXPR_IF (has_partial_tail) {
                /* partial unroll */
                ann_helper::unroll<k - 1>([&](auto i) -> bool {
                    if (i >= n_tail) { return false; }
                    vect x0 = Policy::template load<Policy::is_aligned>(x + k_per_iter * i);
                    vect y0 = Policy::template load<Policy::is_aligned>(y + k_per_iter * i);
                    accs[i] = Policy::madd_vectors(x0, y0, accs[i]);
                    nxs[i] = Policy::madd_square(x0, nxs[i]);
                    nys[i] = Policy::madd_square(y0, nys[i]);
                    return true;
                });
            }

            CONSTEXPR_IF (has_rem_tail && Policy::rem_before_reduce) {
                const uint16 left = dim % k_per_iter;
                if (left) {
                    x += k_per_iter * n_tail;
                    y += k_per_iter * n_tail;
                    // Handle remainder for all three accumulator sets
                    if constexpr (Policy::M == Metric::L2) {
                        accs[k - 1] = Policy::deal_remainder(x, y, dim, n_tail, accs[k - 1]);
                    } else {
                        // COSINE/IP: need to handle dot and both norms
                        accs[k - 1] = Policy::deal_remainder(x, y, dim, n_tail, accs[k - 1]);
                        nxs[k - 1] = Policy::deal_remainder(x, x, dim, n_tail, nxs[k - 1]);
                        nys[k - 1] = Policy::deal_remainder(y, y, dim, n_tail, nys[k - 1]);
                    }
                }
            }
            float res = Policy::reduce(accs);
            float norm_x = Policy::reduce(nxs);
            float norm_y = Policy::reduce(nys);
            CONSTEXPR_IF (has_rem_tail && Policy::rem_after_reduce) {
                const uint16 left = dim % k_per_iter;
                if (left) {
                    x += k_per_iter * n_tail;
                    y += k_per_iter * n_tail;
                    res += Policy::deal_remainder(x, y, dim, n_tail);
                    norm_x += Policy::deal_remainder(x, x, dim, n_tail);
                    norm_y += Policy::deal_remainder(y, y, dim, n_tail);
                }
            }
            return -res / (sqrtf(norm_x * norm_y) + __FLT_EPSILON__);
        }
    }

    static INLINE_PROP float ip_distance(const void *xx, const void *yy, uint16 dim)
    {
        const plaint *x = (const plaint *)xx;
        const plaint *y = (const plaint *)yy;
        
        CONSTEXPR_IF (Policy::use_asm_code) {
            return Policy::asm_code(x, y, dim);
        } else CONSTEXPR_IF (!Policy::use_custom_code) {
            intm distance = 0;
            CONSTEXPR_IF (Policy::is_aligned) {
                ASSUME_ALIGNED(x);
                ASSUME_ALIGNED(y);
            }
            for (uint16 i = 0; i < dim; ++i) {
                intm xi = (intm)Policy::transform(x[i]);
                intm yi = (intm)Policy::transform(y[i]);
                distance -= (plaint)(xi * yi);
            }
            return (float)distance;
        } else {
            const uint16 n_full = dim / k_step;
            const uint16 n_tail = (dim % k_step) / k_per_iter;
            Array<vect, k> accs =
                initialize_array_with_value<k>(Policy::get_zero_vectors());
            for (uint16 i = 0; i < n_full; ++i) {
                if (i >= skip_step && (!has_partial_tail || i + skip_step < n_full)) {
                    ann_helper::unroll<k_bytes / cache_size>([&](auto j) -> void {
                        unpolluted_prefetch((char *)y + j * cache_size + skip_step * k_bytes);
                    });
                }
                Array<vect, k> xs = Policy::template loadk<Policy::is_aligned>(x);
                Array<vect, k> ys = Policy::template loadk<Policy::is_aligned>(y);
                ann_helper::unroll<k>([&](auto i) -> void {
                    accs[i] = Policy::madd_vectors(xs[i], ys[i], accs[i]);
                });
                x += k_step;
                y += k_step;
            }

            CONSTEXPR_IF (has_partial_tail) {
                /* partial unroll */
                ann_helper::unroll<k - 1>([&](auto i) -> bool {
                    if (i >= n_tail) { return false; }
                    vect x0 = Policy::template load<Policy::is_aligned>(x + k_per_iter * i);
                    vect y0 = Policy::template load<Policy::is_aligned>(y + k_per_iter * i);
                    accs[i] = Policy::madd_vectors(x0, y0, accs[i]);
                    return true;
                });
            }
            /* deal with remainder */
            CONSTEXPR_IF (has_rem_tail && Policy::rem_before_reduce) {
                const uint16 left = dim % k_per_iter;
                if (left) {
                    x += k_per_iter * n_tail;
                    y += k_per_iter * n_tail;
                    accs[k - 1] = Policy::deal_remainder(x, y, dim, n_tail, accs[k - 1]);
                }
            }
            float res = Policy::reduce(accs);
            CONSTEXPR_IF (has_rem_tail && !Policy::rem_before_reduce) {
                const uint16 left = dim % k_per_iter;
                if (left) {
                    x += k_per_iter * n_tail;
                    y += k_per_iter * n_tail;
                    res += Policy::deal_remainder(x, y, dim, n_tail);
                }
            }
            return -res;
        }
    }

    static INLINE_PROP float sphere_distance(const void *xx, const void *yy, uint16 dim)
    {
        float dist = ip_distance(xx, yy, dim);
        if (dist > 1) {
            dist = 1;
        } else if (dist < -1) {
            dist = -1;
        }
        return (acos(dist) / M_PI);
    }
    static INLINE_PROP float l2_sqrt_distance(const void *xx, const void *yy, uint16 dim)
    {
        float dist = l2_distance(xx, yy, dim);
        return (sqrtf(dist));
    }
    static INLINE_PROP float l2_norm_distance(const void *xx, const void *yy, uint16 dim)
    {
        float dist = -ip_distance(xx, yy, dim);
        return (sqrtf(dist));
    }

    template <Metric m>
    static constexpr auto get_function()
    {
        switch (m) {
            case Metric::L2:
                return l2_distance;
            case Metric::INNER_PRODUCT:
            case Metric::FAST_COSINE:
                return ip_distance;
            case Metric::COSINE:
                return cosine_distance;
            case Metric::SPHERICAL:
                return sphere_distance;
            case Metric::L2_SQRT:
                return l2_sqrt_distance;
            case Metric::L2_NORM:
                return l2_norm_distance;
            default:
                Assert(false);
                __builtin_unreachable();
        }
    }

    /* prefetch seems not working under high concurencies, it won't be better if not worse */
    static constexpr bool use_prefetch = false;
    static FORCE_INLINE void unpolluted_prefetch(void *ptr)
    {
        CONSTEXPR_IF(use_prefetch) {
            /* __builtin_prefetch 0 in arm defaults to L1, and I'm happy about it */
            __builtin_prefetch(ptr, 0, 0);
        }
    }
    static FORCE_INLINE void favored_prefetch(void *ptr)
    {
        CONSTEXPR_IF(use_prefetch) {
#if defined(__arm__) || defined(__arm) || defined(__aarch64__) || defined(__aarch64)
            __builtin_prefetch(ptr, 0, 2);
#else
            /* most arch put NTA mem to L3, so make the favored one there too, I guess... */
            __builtin_prefetch(ptr, 0, 1);
#endif
        }
    }
};

#endif /* DISTANCE_TEMPLATE_H */
