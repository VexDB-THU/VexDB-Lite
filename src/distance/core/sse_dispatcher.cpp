#include <immintrin.h>

#include "distance/core/distance_utils_core.h"
#include "half.h"
#include "distance/core/halfutils_core.h"
#include "distance/core/transform_template_core.h"

class SseDistancePatcher {
    static constexpr uint16 unroll_factor = 4u;
public:
    template <DistPrecisionType dt>
    static constexpr RemainderSituation get_remainder_situation(uint16 dim)
    {
        if (dt == DistPrecisionType::HALF) {
            return RemainderSituation::Unknown;
        }
        constexpr uint16 k_per_iter = 16 / get_dtype_size(dt);
        if (dim % k_per_iter == 0) {
            if (dim % (unroll_factor * k_per_iter) == 0) {
                return RemainderSituation::NoPartial;
            }
            return RemainderSituation::NoTail;
        }
        return RemainderSituation::Unknown;
    }

private:
    template <Metric m, DistPrecisionType dt, RemainderSituation rs, bool aligned>
    struct SsePolicy;

    template <TransformOp op, DistPrecisionType dt, RemainderSituation rs, bool aligned>
    struct SseTransformPolicy;

    template <Metric m, RemainderSituation rs, bool aligned>
    struct SsePolicy<m, DistPrecisionType::FLOAT, rs, aligned> {
        using PlainT = float;
        using VecT = __m128;
        using IntmT = float;
        using AccT = __m128;
        static constexpr uint16 k = unroll_factor;
        static constexpr uint16 k_per_iter = 4u;
        static constexpr bool use_asm_code = false;
        static constexpr bool use_custom_code = true;
        static constexpr bool is_aligned = aligned;
        static constexpr bool rem_before_reduce = false;
        static constexpr bool rem_after_reduce = true;
        static constexpr RemainderSituation RS = rs;
        static constexpr Metric M = m;

        template <bool aligned_input>
        static INLINE_PROP VecT load(const float *v)
        {
            if constexpr (aligned_input) {
                return _mm_load_ps(v);
            } else {
                return _mm_loadu_ps(v);
            }
        }

        template <bool aligned_input>
        static INLINE_PROP Array<VecT, k> loadk(const float *v)
        {
            Array<VecT, k> res;
            ann_helper::unroll<k>([&](auto i) -> void {
                res[i] = load<aligned_input>(v + k_per_iter * i);
            });
            return res;
        }

        static INLINE_PROP AccT get_zero_vectors() { return _mm_setzero_ps(); }
        static INLINE_PROP VecT sub_vectors(VecT a, VecT b) { return _mm_sub_ps(a, b); }
        static INLINE_PROP AccT madd_vectors(VecT a, VecT b, AccT s) { return _mm_fmadd_ps(a, b, s); }
        static INLINE_PROP AccT madd_square(VecT a, AccT s) { return _mm_fmadd_ps(a, a, s); }
        static INLINE_PROP VecT add_vectors(VecT a, VecT b) { return _mm_add_ps(a, b); }

        static INLINE_PROP float reduce(const Array<VecT, k> &accs)
        {
            VecT acc0 = add_vectors(add_vectors(accs[0], accs[1]), add_vectors(accs[2], accs[3]));
            VecT acc1 = _mm_hadd_ps(acc0, acc0);
            return _mm_cvtss_f32(_mm_hadd_ps(acc1, acc1));
        }

        static INLINE_PROP float deal_remainder(const float *x, const float *y, uint16 dim, uint16 n_tail)
        {
            x += k_per_iter * n_tail;
            y += k_per_iter * n_tail;
            float res = 0.0f;
            switch (dim % k_per_iter) {
                case 3u: {
                    if constexpr (m == Metric::L2) {
                        const float diff2 = x[2] - y[2];
                        res += diff2 * diff2;
                    } else {
                        res += x[2] * y[2];
                    }
                }
                case 2u: {
                    if constexpr (m == Metric::L2) {
                        const float diff1 = x[1] - y[1];
                        res += diff1 * diff1;
                    } else {
                        res += x[1] * y[1];
                    }
                }
                case 1u: {
                    if constexpr (m == Metric::L2) {
                        const float diff0 = x[0] - y[0];
                        res += diff0 * diff0;
                    } else {
                        res += x[0] * y[0];
                    }
                } break;
            }
            return res;
        }
    };

    template <Metric m, RemainderSituation rs, bool aligned>
    struct SsePolicy<m, DistPrecisionType::HALF, rs, aligned> {
        using PlainT = half;
        using IntmT = float;
        using VecT = void;
        using AccT = void;
        static constexpr bool use_asm_code = false;
        static constexpr bool use_custom_code = false;
        static constexpr bool is_aligned = aligned;
        static constexpr uint16 k = 1;
        static constexpr uint16 k_per_iter = 1;
        static constexpr RemainderSituation RS = rs;
        static INLINE_PROP float transform(half v) { return half_to_float(v); }
    };

    template <Metric m, RemainderSituation rs, bool aligned>
    struct SsePolicy<m, DistPrecisionType::INT8, rs, aligned> {
        using PlainT = int8;
        using VecT = __m128i;
        using IntmT = int16;
        using AccT = __m128i;
        static constexpr uint16 k = unroll_factor;
        static constexpr uint16 k_per_iter = 16u;
        static constexpr bool use_asm_code = false;
        static constexpr bool use_custom_code = true;
        static constexpr bool is_aligned = aligned;
        static constexpr bool rem_before_reduce = true;
        static constexpr bool rem_after_reduce = false;
        static constexpr RemainderSituation RS = rs;
        static constexpr Metric M = m;

        static INLINE_PROP AccT get_zero_vectors() { return _mm_setzero_si128(); }

        static INLINE_PROP VecT sub_vectors(VecT a, VecT b) { return _mm_sub_epi8(a, b); }

        static INLINE_PROP AccT sub_madd_int8s(VecT a, VecT b, AccT s) {
            VecT d_lo = _mm_sub_epi16(_mm_cvtepi8_epi16(a), _mm_cvtepi8_epi16(b));
            VecT d_hi = _mm_sub_epi16(_mm_cvtepi8_epi16(_mm_srli_si128(a, 8)),
                                      _mm_cvtepi8_epi16(_mm_srli_si128(b, 8)));
            s = _mm_add_epi32(s, _mm_madd_epi16(d_lo, d_lo));
            return _mm_add_epi32(s, _mm_madd_epi16(d_hi, d_hi));
        }

        static INLINE_PROP AccT madd_square(VecT a, AccT s) {
            __m128i a16 = _mm_cvtepi8_epi16(a);
            __m128i sq = _mm_mullo_epi16(a16, a16);
            return _mm_add_epi32(sq, s);
        }

        static INLINE_PROP AccT madd_vectors(VecT a, VecT b, AccT s) {
            __m128i d_lo = _mm_sub_epi16(_mm_cvtepi8_epi16(a), _mm_cvtepi8_epi16(b));
            __m128i d_hi = _mm_sub_epi16(_mm_cvtepi8_epi16(_mm_srli_si128(a, 8)),
                                         _mm_cvtepi8_epi16(_mm_srli_si128(b, 8)));
            s = _mm_add_epi32(s, _mm_madd_epi16(d_lo, d_lo));
            return _mm_add_epi32(s, _mm_madd_epi16(d_hi, d_hi));
        }

        static INLINE_PROP AccT add_vectors(AccT a, AccT b) { return _mm_add_epi32(a, b); }

        template <bool aligned_input>
        static INLINE_PROP VecT load(const int8 *v) {
            if constexpr (aligned_input) {
                return _mm_load_si128((const __m128i *)v);
            } else {
                return _mm_loadu_si128((const __m128i *)v);
            }
        }

        template <bool aligned_input>
        static INLINE_PROP Array<VecT, k> loadk(const int8 *v) {
            Array<VecT, k> res;
            ann_helper::unroll<k>([&](auto i) -> void {
                res[i] = load<aligned_input>(v + k_per_iter * i);
            });
            return res;
        }

        static INLINE_PROP AccT deal_remainder(const int8 *x, const int8 *y, uint16 dim, uint16 n_tail, AccT acc) {
            const uint16 left = dim % k_per_iter;
            if (left) {
                x += k_per_iter * n_tail;
                y += k_per_iter * n_tail;
                alignas(16) int8 x_buf[k_per_iter] = {0};
                alignas(16) int8 y_buf[k_per_iter] = {0};
                memcpy(x_buf, x, left * sizeof(int8));
                memcpy(y_buf, y, left * sizeof(int8));
                VecT xs = _mm_load_si128((const __m128i *)x_buf);
                VecT ys = _mm_load_si128((const __m128i *)y_buf);
                if constexpr (m == Metric::L2) {
                    return sub_madd_int8s(xs, ys, acc);
                } else {
                    return madd_vectors(xs, ys, acc);
                }
            }
            return acc;
        }

        static INLINE_PROP float reduce(const Array<AccT, k> &accs) {
            AccT acc_sum = _mm_add_epi32(_mm_add_epi32(accs[0], accs[1]),
                                         _mm_add_epi32(accs[2], accs[3]));
            __m128i sum = _mm_hadd_epi32(acc_sum, acc_sum);
            sum = _mm_hadd_epi32(sum, sum);
            return (float)_mm_cvtsi128_si32(_mm_hadd_epi32(sum, sum));
        }
    };

public:
    template <Metric m, DistPrecisionType dt, RemainderSituation rs, bool aligned>
    using Distancer = DistanceDispatcher<SsePolicy, m, dt, rs, aligned>;

private:
    template <RemainderSituation rs, bool aligned>
    struct SseTransformPolicyBase {
        using VecT = __m128;
        using IntmT = float;
        static constexpr uint16 k = unroll_factor;
        static constexpr uint16 k_per_iter = 4u;
        static constexpr bool use_custom_code = true;
        static constexpr bool is_aligned = aligned;
        static constexpr RemainderSituation RS = rs;

        static INLINE_PROP VecT add(VecT a, VecT b) { return _mm_add_ps(a, b); }
        static INLINE_PROP VecT sub(VecT a, VecT b) { return _mm_sub_ps(a, b); }
        static INLINE_PROP VecT mul(VecT a, VecT b) { return _mm_mul_ps(a, b); }
        static INLINE_PROP VecT div(VecT a, VecT b) { return _mm_div_ps(a, b); }

        template <bool aligned_input>
        static INLINE_PROP VecT load(const float *v)
        {
            if constexpr (aligned_input) {
                return _mm_load_ps(v);
            } else {
                return _mm_loadu_ps(v);
            }
        }

        template <bool aligned_input>
        static INLINE_PROP Array<VecT, k> loadk(const float *v)
        {
            Array<VecT, k> res;
            ann_helper::unroll<k>([&](auto i) -> void {
                res[i] = load<aligned_input>(v + k_per_iter * i);
            });
            return res;
        }

        template <bool aligned_input>
        static INLINE_PROP void store(float *v, VecT x)
        {
            if constexpr (aligned_input) {
                _mm_store_ps(v, x);
            } else {
                _mm_storeu_ps(v, x);
            }
        }

        static INLINE_PROP VecT broadcast_scalar(float v) { return _mm_set1_ps(v); }

        static INLINE_PROP float to_interm(float v) { return v; }
        static INLINE_PROP float from_interm(float v) { return v; }
    };

    template <TransformOp op, RemainderSituation rs, bool aligned>
    struct SseTransformPolicy<op, DistPrecisionType::FLOAT, rs, aligned>
        : public SseTransformPolicyBase<rs, aligned> {
        using PlainT = float;
    };

    template <TransformOp op, RemainderSituation rs, bool aligned>
    struct SseTransformPolicy<op, DistPrecisionType::HALF, rs, aligned>
        : public SseTransformPolicyBase<rs, aligned> {
        using PlainT = half;
        using VecT = typename SseTransformPolicyBase<rs, aligned>::VecT;
        using IntmT = float;

        constexpr static auto k = SseTransformPolicyBase<rs, aligned>::k;
        constexpr static auto k_per_iter = SseTransformPolicyBase<rs, aligned>::k_per_iter;

        template <bool aligned_input>
        static INLINE_PROP VecT load(const half *v)
        {
            alignas(16) float buf[k_per_iter];
            for (uint16 i = 0; i < k_per_iter; ++i) {
                buf[i] = half_to_float(v[i]);
            }
            return _mm_load_ps(buf);
        }

        template <bool aligned_input>
        static INLINE_PROP Array<VecT, k> loadk(const half *v)
        {
            Array<VecT, k> res;
            ann_helper::unroll<k>([&](auto i) -> void {
                res[i] = load<aligned_input>(v + k_per_iter * i);
            });
            return res;
        }

        template <bool aligned_input>
        static INLINE_PROP void store(half *v, VecT x)
        {
            alignas(16) float buf[k_per_iter];
            _mm_store_ps(buf, x);
            for (uint16 i = 0; i < k_per_iter; ++i) {
                v[i] = float_to_half(buf[i]);
            }
        }

        static INLINE_PROP float to_interm(half v) { return half_to_float(v); }
        static INLINE_PROP float to_interm(float v) { return v; }
        static INLINE_PROP half from_interm(float v) { return float_to_half(v); }
    };

    template <TransformOp op, RemainderSituation rs, bool aligned>
    struct SseTransformPolicy<op, DistPrecisionType::INT8, rs, aligned>
        : public SseTransformPolicyBase<rs, aligned> {
        using PlainT = int16;
        using VecT = typename SseTransformPolicyBase<rs, aligned>::VecT;
        using IntmT = float;

        constexpr static auto k = SseTransformPolicyBase<rs, aligned>::k;
        constexpr static auto k_per_iter = SseTransformPolicyBase<rs, aligned>::k_per_iter;

        template <bool aligned_input>
        static INLINE_PROP VecT load(const int16 *v)
        {
            alignas(16) float buf[k_per_iter];
            for (uint16 i = 0; i < k_per_iter; ++i) {
                buf[i] = (float)v[i];
            }
            return _mm_load_ps(buf);
        }

        template <bool aligned_input>
        static INLINE_PROP Array<VecT, k> loadk(const int16 *v)
        {
            Array<VecT, k> res;
            ann_helper::unroll<k>([&](auto i) -> void {
                res[i] = load<aligned_input>(v + k_per_iter * i);
            });
            return res;
        }

        template <bool aligned_input>
        static INLINE_PROP void store(int16 *v, VecT x)
        {
            alignas(16) float buf[k_per_iter];
            _mm_store_ps(buf, x);
            for (uint16 i = 0; i < k_per_iter; ++i) {
                v[i] = (int16)buf[i];
            }
        }

        static INLINE_PROP float to_interm(int16 v) { return (float)v; }
        static INLINE_PROP float to_interm(float v) { return v; }
        static INLINE_PROP int16 from_interm(float v) { return (int16)v; }
    };

public:
    template <TransformOp op, DistPrecisionType dt, RemainderSituation rs, bool aligned>
    using Transformer = TransformDispatcher<SseTransformPolicy, op, dt, rs, aligned>;
};

#define PatcherName SseDistancePatcher
#define CUR_ARCH Arch::SSE
#include "distance/core/distance.templ"
#undef CUR_ARCH
#undef PatcherName
