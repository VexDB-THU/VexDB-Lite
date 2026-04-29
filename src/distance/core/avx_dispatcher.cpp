#include <immintrin.h>

#include "distance/core/distance_utils_core.h"
#include "half.h"
#include "distance/core/halfutils_core.h"
#include "distance/core/transform_template_core.h"

class AvxDistancePatcher {
    static constexpr uint16 unroll_factor = 4u;
public:
    template <DistPrecisionType dt>
    static constexpr RemainderSituation get_remainder_situation(uint16 dim)
    {
        constexpr uint16 k_per_iter = 32 /
            get_dtype_size(dt == DistPrecisionType::HALF ? DistPrecisionType::FLOAT : dt);
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
    struct AvxPolicy;

    template <TransformOp op, DistPrecisionType dt, RemainderSituation rs, bool aligned>
    struct AvxTransformPolicy;

    template <Metric m, RemainderSituation rs, bool aligned>
    struct AvxFloatHalfBase {
        using VecT = __m256;
        using IntmT = float;
        using AccT = __m256;
        static constexpr uint16 k = unroll_factor;
        static constexpr uint16 k_per_iter = 8u;
        static constexpr bool use_asm_code = false;
        static constexpr bool use_custom_code = true;
        static constexpr bool is_aligned = aligned;
        static constexpr bool rem_before_reduce = true;
        static constexpr bool rem_after_reduce = false;
        static constexpr RemainderSituation RS = rs;
        static constexpr Metric M = m;

        static INLINE_PROP void turn_off() { _mm256_zeroupper(); }

        static INLINE_PROP AccT get_zero_vectors() { return _mm256_setzero_ps(); }
        static INLINE_PROP VecT sub_vectors(VecT a, VecT b) { return _mm256_sub_ps(a, b); }
        static INLINE_PROP AccT madd_vectors(VecT a, VecT b, AccT s) { return _mm256_fmadd_ps(a, b, s); }
        static INLINE_PROP AccT madd_square(VecT a, AccT s) { return _mm256_fmadd_ps(a, a, s); }
        static INLINE_PROP VecT add_vectors(VecT a, VecT b) { return _mm256_add_ps(a, b); }

        static INLINE_PROP float reduce(const Array<VecT, k> &accs)
        {
            VecT sum = add_vectors(add_vectors(accs[0], accs[1]), add_vectors(accs[2], accs[3]));
            __m128 sum128 = _mm_add_ps(_mm256_extractf128_ps(sum, 1), _mm256_castps256_ps128(sum));
            sum128 = _mm_hadd_ps(sum128, sum128);
            return _mm_cvtss_f32(_mm_hadd_ps(sum128, sum128));
        }
    };

    template <Metric m, RemainderSituation rs, bool aligned>
    struct AvxPolicy<m, DistPrecisionType::FLOAT, rs, aligned> 
        : public AvxFloatHalfBase<m, rs, aligned> {
        using Base = AvxFloatHalfBase<m, rs, aligned>;
        using PlainT = float;
        using VecT = typename AvxFloatHalfBase<m, rs, aligned>::VecT;
        using AccT = typename AvxFloatHalfBase<m, rs, aligned>::AccT;
        constexpr static auto k = AvxFloatHalfBase<m, rs, aligned>::k;
        constexpr static auto k_per_iter = AvxFloatHalfBase<m, rs, aligned>::k_per_iter;

        template <bool aligned_input>
        static INLINE_PROP VecT load(const float *v)
        {
            if constexpr (aligned_input) {
                return _mm256_load_ps(v);
            } else {
                return _mm256_loadu_ps(v);
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

        static INLINE_PROP VecT deal_remainder(const float *x, const float *y, uint16 dim, uint16 n_tail, VecT acc)
        {
            const uint16 left = dim % k_per_iter;
            if (left) {
                x += k_per_iter * n_tail;
                y += k_per_iter * n_tail;
                alignas(32) float x_buf[k_per_iter] = {0};
                alignas(32) float y_buf[k_per_iter] = {0};
                for (uint16 i = 0; i < left; ++i) {
                    x_buf[i] = x[i];
                    y_buf[i] = y[i];
                }
                VecT xs = load<true>(x_buf);
                VecT ys = load<true>(y_buf);
                if constexpr (m == Metric::L2) {
                    VecT diff = Base::sub_vectors(xs, ys);
                    acc = Base::madd_vectors(diff, diff, acc);
                } else {
                    acc = Base::madd_vectors(xs, ys, acc);
                }
            }
            return acc;
        }
    };

    template <Metric m, RemainderSituation rs, bool aligned>
    struct AvxPolicy<m, DistPrecisionType::HALF, rs, aligned> 
        : public AvxFloatHalfBase<m, rs, aligned> {
        using PlainT = half;
        using VecT = typename AvxFloatHalfBase<m, rs, aligned>::VecT;
        using AccT = typename AvxFloatHalfBase<m, rs, aligned>::AccT;
        constexpr static auto k = AvxFloatHalfBase<m, rs, aligned>::k;
        constexpr static auto k_per_iter = AvxFloatHalfBase<m, rs, aligned>::k_per_iter;

        template <bool aligned_input>
        static INLINE_PROP VecT load(const half *v)
        {
            if constexpr (aligned_input) {
                return _mm256_cvtph_ps(_mm_load_si128((const __m128i *)v));
            } else {
                return _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)v));
            }
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

        static INLINE_PROP VecT deal_remainder(const half *x, const half *y, uint16 dim, uint16 n_tail, VecT acc)
        {
            const uint16 left = dim % k_per_iter;
            if (left) {
                x += k_per_iter * n_tail;
                y += k_per_iter * n_tail;
                alignas(16) half x_buf[k_per_iter] = {0};
                alignas(16) half y_buf[k_per_iter] = {0};
                for (uint16 i = 0; i < left; ++i) {
                    x_buf[i] = x[i];
                    y_buf[i] = y[i];
                }
                __m256 vxs = _mm256_cvtph_ps(_mm_load_si128((__m128i *)x_buf));
                __m256 vys = _mm256_cvtph_ps(_mm_load_si128((__m128i *)y_buf));
                if constexpr (m == Metric::L2) {
                    __m256 diff = _mm256_sub_ps(vxs, vys);
                    acc = _mm256_fmadd_ps(diff, diff, acc);
                } else {
                    acc = _mm256_fmadd_ps(vxs, vys, acc);
                }
            }
            return acc;
        }
    };

    template <Metric m, RemainderSituation rs, bool aligned>
    struct AvxPolicy<m, DistPrecisionType::INT8, rs, aligned> {
        using PlainT = int8;
        using VecT = __m256i;
        using IntmT = int16;
        using AccT = __m256i;
        static constexpr uint16 k = unroll_factor;
        static constexpr uint16 k_per_iter = 32u;
        static constexpr bool use_asm_code = false;
        static constexpr bool use_custom_code = true;
        static constexpr bool is_aligned = aligned;
        static constexpr bool rem_before_reduce = true;
        static constexpr bool rem_after_reduce = false;
        static constexpr RemainderSituation RS = rs;
        static constexpr Metric M = m;

        static INLINE_PROP void turn_off() { _mm256_zeroupper(); }
        static INLINE_PROP int8 transform(int8 v) { return v; }
        static INLINE_PROP AccT get_zero_vectors() { return _mm256_setzero_si256(); }

        static INLINE_PROP VecT sub_vectors(VecT a, VecT b) { return _mm256_sub_epi8(a, b); }

        static INLINE_PROP AccT sub_madd_int8s(VecT a, VecT b, AccT s) {
            __m256i d_lo = _mm256_sub_epi16(
                _mm256_cvtepi8_epi16(_mm256_extracti128_si256(a, 0)),
                _mm256_cvtepi8_epi16(_mm256_extracti128_si256(b, 0)));
            __m256i d_hi = _mm256_sub_epi16(
                _mm256_cvtepi8_epi16(_mm256_extracti128_si256(a, 1)),
                _mm256_cvtepi8_epi16(_mm256_extracti128_si256(b, 1)));
            s = _mm256_add_epi32(s, _mm256_madd_epi16(d_lo, d_lo));
            return _mm256_add_epi32(s, _mm256_madd_epi16(d_hi, d_hi));
        }

        static INLINE_PROP AccT madd_square(VecT a, AccT s) {
            __m256i a16 = _mm256_cvtepi8_epi16(_mm256_extracti128_si256(a, 0));
            __m256i sq = _mm256_mullo_epi16(a16, a16);
            // TD: fix bug
            return _mm256_add_epi32(sq, s);
        }

        static INLINE_PROP AccT madd_vectors(VecT a, VecT b, AccT s) {
            s = _mm256_add_epi32(s,
                    _mm256_madd_epi16(_mm256_cvtepi8_epi16(_mm256_extracti128_si256(a, 0)),
                        _mm256_cvtepi8_epi16(_mm256_extracti128_si256(b, 0))));
            return _mm256_add_epi32(s,
                    _mm256_madd_epi16(_mm256_cvtepi8_epi16(_mm256_extracti128_si256(a, 1)), 
                        _mm256_cvtepi8_epi16(_mm256_extracti128_si256(b, 1))));
        }

        static INLINE_PROP AccT add_vectors(AccT a, AccT b) { return _mm256_add_epi32(a, b); }

        template <bool aligned_input>
        static INLINE_PROP VecT load(const int8 *v) {
            if constexpr (aligned_input) {
                return _mm256_load_si256((const __m256i *)v);
            } else {
                return _mm256_loadu_si256((const __m256i *)v);
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
                alignas(32) int8 x_buf[k_per_iter] = {0};
                alignas(32) int8 y_buf[k_per_iter] = {0};
                memcpy(x_buf, x, left * sizeof(int8));
                memcpy(y_buf, y, left * sizeof(int8));
                VecT xs = _mm256_load_si256((const __m256i *)x_buf);
                VecT ys = _mm256_load_si256((const __m256i *)y_buf);
                if constexpr (m == Metric::L2) {
                    return sub_madd_int8s(xs, ys, acc);
                } else {
                    return madd_vectors(xs, ys, acc);
                }
            }
            return acc;
        }

        static INLINE_PROP float reduce(const Array<AccT, k> &accs) {
            AccT acc_sum = _mm256_add_epi32(
                _mm256_add_epi32(accs[0], accs[1]),
                _mm256_add_epi32(accs[2], accs[3]));
            return hsum_epi32_256(acc_sum);
        }
    };

public:
    template <Metric m, DistPrecisionType dt, RemainderSituation rs, bool aligned>
    using Distancer = DistanceDispatcher<AvxPolicy, m, dt, rs, aligned>;

private:
    template <RemainderSituation rs, bool aligned>
    struct AvxTransformPolicyBase {
        using VecT = __m256;
        using IntmT = float;
        static constexpr uint16 k = unroll_factor;
        static constexpr uint16 k_per_iter = 8u;
        static constexpr bool use_custom_code = true;
        static constexpr bool is_aligned = aligned;
        static constexpr RemainderSituation RS = rs;

        static INLINE_PROP VecT add(VecT a, VecT b) { return _mm256_add_ps(a, b); }
        static INLINE_PROP VecT sub(VecT a, VecT b) { return _mm256_sub_ps(a, b); }
        static INLINE_PROP VecT mul(VecT a, VecT b) { return _mm256_mul_ps(a, b); }
        static INLINE_PROP VecT div(VecT a, VecT b) { return _mm256_div_ps(a, b); }
        static INLINE_PROP void turn_off() { _mm256_zeroupper(); }

        template <bool aligned_input>
        static INLINE_PROP VecT load(const float *v)
        {
            if constexpr (aligned_input) {
                return _mm256_load_ps(v);
            } else {
                return _mm256_loadu_ps(v);
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
                _mm256_store_ps(v, x);
            } else {
                _mm256_storeu_ps(v, x);
            }
        }

        static INLINE_PROP VecT broadcast_scalar(float v) { return _mm256_set1_ps(v); }

        static INLINE_PROP float to_interm(float v) { return v; }
        static INLINE_PROP float from_interm(float v) { return v; }
    };

    template <TransformOp op, RemainderSituation rs, bool aligned>
    struct AvxTransformPolicy<op, DistPrecisionType::FLOAT, rs, aligned>
        : public AvxTransformPolicyBase<rs, aligned> {
        using PlainT = float;
    };

    template <TransformOp op, RemainderSituation rs, bool aligned>
    struct AvxTransformPolicy<op, DistPrecisionType::HALF, rs, aligned>
        : public AvxTransformPolicyBase<rs, aligned> {
        using PlainT = half;
        using VecT = typename AvxTransformPolicyBase<rs, aligned>::VecT;
        using IntmT = float;

        constexpr static auto k = AvxTransformPolicyBase<rs, aligned>::k;
        constexpr static auto k_per_iter = AvxTransformPolicyBase<rs, aligned>::k_per_iter;

        template <bool aligned_input>
        static INLINE_PROP VecT load(const half *v)
        {
            alignas(32) float buf[k_per_iter];
            for (uint16 i = 0; i < k_per_iter; ++i) {
                buf[i] = half_to_float(v[i]);
            }
            return _mm256_load_ps(buf);
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
            alignas(32) float buf[k_per_iter];
            _mm256_store_ps(buf, x);
            for (uint16 i = 0; i < k_per_iter; ++i) {
                v[i] = float_to_half(buf[i]);
            }
        }

        static INLINE_PROP float to_interm(half v) { return half_to_float(v); }
        static INLINE_PROP float to_interm(float v) { return v; }
        static INLINE_PROP half from_interm(float v) { return float_to_half(v); }
    };

    template <TransformOp op, RemainderSituation rs, bool aligned>
    struct AvxTransformPolicy<op, DistPrecisionType::INT8, rs, aligned>
        : public AvxTransformPolicyBase<rs, aligned> {
        using PlainT = int16;
        using VecT = typename AvxTransformPolicyBase<rs, aligned>::VecT;
        using IntmT = float;

        constexpr static auto k = AvxTransformPolicyBase<rs, aligned>::k;
        constexpr static auto k_per_iter = AvxTransformPolicyBase<rs, aligned>::k_per_iter;

        template <bool aligned_input>
        static INLINE_PROP VecT load(const int16 *v)
        {
            alignas(32) float buf[k_per_iter];
            for (uint16 i = 0; i < k_per_iter; ++i) {
                buf[i] = (float)v[i];
            }
            return _mm256_load_ps(buf);
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
            alignas(32) float buf[k_per_iter];
            _mm256_store_ps(buf, x);
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
    using Transformer = TransformDispatcher<AvxTransformPolicy, op, dt, rs, aligned>;

private:
    static FORCE_INLINE float hsum_epi32_256(__m256i v) {
        __m128i lo = _mm256_castsi256_si128(v);
        __m128i hi = _mm256_extracti128_si256(v, 1);
        __m128i sum = _mm_add_epi32(lo, hi);
        sum = _mm_hadd_epi32(sum, sum);
        sum = _mm_hadd_epi32(sum, sum);
        return (float)_mm_cvtsi128_si32(sum);
    }
};

#define PatcherName AvxDistancePatcher
#define CUR_ARCH Arch::AVX
#include "distance/core/distance.templ"
#undef CUR_ARCH
#undef PatcherName
