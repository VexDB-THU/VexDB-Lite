#include <immintrin.h>

#include "distance/core/distance_utils_core.h"
#include "half.h"
#include "distance/core/halfutils_core.h"
#include "distance/core/transform_template_core.h"

class Avx512DistancePatcher {
    static constexpr uint16 unroll_factor = 4u;
public:
    template <DistPrecisionType dt>
    static constexpr RemainderSituation get_remainder_situation(uint16 dim)
    {
        constexpr uint16 k_per_iter = 64 /
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
    struct Avx512Policy;

    template <TransformOp op, DistPrecisionType dt, RemainderSituation rs, bool aligned>
    struct Avx512TransformPolicy;

    template <Metric m, RemainderSituation rs, bool aligned>
    struct Avx512FloatHalfBase {
        using VecT = __m512;
        using IntmT = float;
        using AccT = __m512;
        static constexpr uint16 k = unroll_factor;
        static constexpr uint16 k_per_iter = 16u;
        static constexpr bool use_asm_code = false;
        static constexpr bool use_custom_code = true;
        static constexpr bool is_aligned = aligned;
        static constexpr bool rem_before_reduce = true;
        static constexpr bool rem_after_reduce = false;
        static constexpr RemainderSituation RS = rs;
        static constexpr Metric M = m;

        static INLINE_PROP AccT get_zero_vectors() { return _mm512_setzero_ps(); }
        static INLINE_PROP VecT sub_vectors(VecT a, VecT b) { return _mm512_sub_ps(a, b); }
        static INLINE_PROP AccT madd_vectors(VecT a, VecT b, AccT s) { return _mm512_fmadd_ps(a, b, s); }
        static INLINE_PROP AccT madd_square(VecT a, AccT s) { return _mm512_fmadd_ps(a, a, s); }
        static INLINE_PROP VecT add_vectors(VecT a, VecT b) { return _mm512_add_ps(a, b); }

        static INLINE_PROP float reduce(const Array<VecT, k> &accs)
        {
            VecT sum0 = add_vectors(accs[0], accs[1]);
            VecT sum1 = add_vectors(accs[2], accs[3]);
            return _mm512_reduce_add_ps(add_vectors(sum0, sum1));
        }
    };

    template <Metric m, RemainderSituation rs, bool aligned>
    struct Avx512Policy<m, DistPrecisionType::FLOAT, rs, aligned> 
        : public Avx512FloatHalfBase<m, rs, aligned> {
        using PlainT = float;
        using VecT = typename Avx512FloatHalfBase<m, rs, aligned>::VecT;
        using AccT = typename Avx512FloatHalfBase<m, rs, aligned>::AccT;
        constexpr static auto k = Avx512FloatHalfBase<m, rs, aligned>::k;
        constexpr static auto k_per_iter = Avx512FloatHalfBase<m, rs, aligned>::k_per_iter;

        static INLINE_PROP float transform(float v) { return v; }

        template <bool aligned_input>
        static INLINE_PROP VecT load(const float *v)
        {
            return aligned_input ? _mm512_load_ps(v) : _mm512_loadu_ps(v);
        }

        template <bool aligned_input>
        static INLINE_PROP Array<VecT, k> loadk(const float *v)
        {
            Array<VecT, k> res;
            ann_helper::unroll<k>([&](auto i) -> void {
                res[i] = load<aligned_input>(v + 16 * i);
            });
            return res;
        }

        static INLINE_PROP VecT deal_remainder(const float *x, const float *y, uint16 dim, uint16 n_tail, VecT acc)
        {
            const uint16 left = dim % 16;
            if (left) {
                x += 16 * n_tail;
                y += 16 * n_tail;
                __mmask16 mask = (__mmask16)((1u << left) - 1u);
                VecT xs = _mm512_maskz_loadu_ps(mask, x);
                VecT ys = _mm512_maskz_loadu_ps(mask, y);
                if constexpr (m == Metric::L2) {
                    VecT diff = _mm512_sub_ps(xs, ys);
                    acc = _mm512_mask3_fmadd_ps(diff, diff, acc, mask);
                } else {
                    acc = _mm512_mask3_fmadd_ps(xs, ys, acc, mask);
                }
            }
            return acc;
        }
    };

    template <Metric m, RemainderSituation rs, bool aligned>
    struct Avx512Policy<m, DistPrecisionType::HALF, rs, aligned> 
        : public Avx512FloatHalfBase<m, rs, aligned> {
        using PlainT = half;
        using VecT = typename Avx512FloatHalfBase<m, rs, aligned>::VecT;
        using AccT = typename Avx512FloatHalfBase<m, rs, aligned>::AccT;
        constexpr static auto k = Avx512FloatHalfBase<m, rs, aligned>::k;
        constexpr static auto k_per_iter = Avx512FloatHalfBase<m, rs, aligned>::k_per_iter;

        template <bool aligned_input>
        static INLINE_PROP VecT load(const half *v)
        {
            return aligned_input
                ? _mm512_cvtph_ps(_mm256_load_si256((const __m256i *)v))
                : _mm512_cvtph_ps(_mm256_loadu_si256((const __m256i *)v));
        }

        template <bool aligned_input>
        static INLINE_PROP Array<VecT, k> loadk(const half *v)
        {
            Array<VecT, k> res;
            ann_helper::unroll<k>([&](auto i) -> void {
                res[i] = load<aligned_input>(v + 16 * i);
            });
            return res;
        }

        static INLINE_PROP VecT deal_remainder(const half *x, const half *y, uint16 dim, uint16 n_tail, VecT acc)
        {
            const uint16 left = dim % 16;
            if (left) {
                x += 16 * n_tail;
                y += 16 * n_tail;
                __mmask16 mask = (__mmask16)((1u << left) - 1u);
                __m256i vx = _mm256_maskz_loadu_epi16(mask, x);
                __m256i vy = _mm256_maskz_loadu_epi16(mask, y);
                __m512 vxs = _mm512_cvtph_ps(vx);
                __m512 vys = _mm512_cvtph_ps(vy);
                if constexpr (m == Metric::L2) {
                    __m512 diff = _mm512_sub_ps(vxs, vys);
                    acc = _mm512_mask3_fmadd_ps(diff, diff, acc, mask);
                } else {
                    acc = _mm512_mask3_fmadd_ps(vxs, vys, acc, mask);
                }
            }
            return acc;
        }
    };

    template <Metric m, RemainderSituation rs, bool aligned>
    struct Avx512Policy<m, DistPrecisionType::INT8, rs, aligned> {
        using PlainT = int8;
        using VecT = __m512i;
        using IntmT = int16;
        using AccT = __m512i;
        static constexpr uint16 k = unroll_factor;
        static constexpr uint16 k_per_iter = 64u;
        static constexpr bool use_asm_code = false;
        static constexpr bool use_custom_code = true;
        static constexpr bool is_aligned = aligned;
        static constexpr bool rem_before_reduce = true;
        static constexpr bool rem_after_reduce = false;
        static constexpr RemainderSituation RS = rs;
        static constexpr Metric M = m;

        static INLINE_PROP int8 transform(int8 v) { return v; }
        static INLINE_PROP AccT get_zero_vectors() { return _mm512_setzero_si512(); }

        static INLINE_PROP VecT sub_vectors(VecT a, VecT b) { return _mm512_sub_epi8(a, b); }

        static INLINE_PROP AccT sub_madd_int8s(VecT a, VecT b, AccT s) {
            __m512i d_lo = _mm512_sub_epi16(_mm512_cvtepi8_epi16(_mm512_extracti64x4_epi64(a, 0)),
                                            _mm512_cvtepi8_epi16(_mm512_extracti64x4_epi64(b, 0)));
            __m512i d_hi = _mm512_sub_epi16(_mm512_cvtepi8_epi16(_mm512_extracti64x4_epi64(a, 1)),
                                            _mm512_cvtepi8_epi16(_mm512_extracti64x4_epi64(b, 1)));
            s = _mm512_add_epi32(s, _mm512_madd_epi16(d_lo, d_lo));
            return _mm512_add_epi32(s, _mm512_madd_epi16(d_hi, d_hi));
        }

        static INLINE_PROP AccT madd_square(VecT a, AccT s) {
            __m512i a16 = _mm512_cvtepi8_epi16(_mm512_extracti64x4_epi64(a, 0));
            __m512i sq = _mm512_mullo_epi16(a16, a16);
            // TD: fix bug
            return _mm512_add_epi32(sq, s);
        }

        static INLINE_PROP AccT madd_vectors(VecT a, VecT b, AccT s) {
            s = _mm512_add_epi32(s,
                _mm512_madd_epi16(_mm512_cvtepi8_epi16(_mm512_extracti64x4_epi64(a, 0)), 
                    _mm512_cvtepi8_epi16(_mm512_extracti64x4_epi64(b, 0))));
            return _mm512_add_epi32(s,
                _mm512_madd_epi16(_mm512_cvtepi8_epi16(_mm512_extracti64x4_epi64(a, 1)), 
                    _mm512_cvtepi8_epi16(_mm512_extracti64x4_epi64(b, 1))));
        }

        static INLINE_PROP VecT add_vectors(VecT a, VecT b) { return _mm512_add_epi32(a, b); }

        template <bool aligned_input>
        static INLINE_PROP VecT load(const int8 *v) {
            if constexpr (aligned_input) {
                return _mm512_load_si512(v);
            } else {
                return _mm512_loadu_si512(v);
            }
        }

        template <bool aligned_input>
        static INLINE_PROP Array<VecT, k> loadk(const int8 *v) {
            Array<VecT, k> res;
            ann_helper::unroll<k>([&](auto i) -> void {
                res[i] = load<aligned_input>(v + 64 * i);
            });
            return res;
        }

        static INLINE_PROP AccT deal_remainder(const int8 *x, const int8 *y, uint16 dim, uint16 n_tail, AccT acc) {
            const uint16 left = dim % 64;
            if (left) {
                x += 64 * n_tail;
                y += 64 * n_tail;
                __mmask64 mask = (__mmask64)((1u << left) - 1u);
                VecT xs = _mm512_maskz_loadu_epi8(mask, x);
                VecT ys = _mm512_maskz_loadu_epi8(mask, y);
                if constexpr (m == Metric::L2) {
                    return sub_madd_int8s(xs, ys, acc);
                } else {
                    return madd_vectors(xs, ys, acc);
                }
            }
            return acc;
        }

        static INLINE_PROP float reduce(const Array<AccT, k> &accs) {
            AccT sum0 = _mm512_add_epi32(_mm512_add_epi32(accs[0], accs[1]),
                                         _mm512_add_epi32(accs[2], accs[3]));
            return (float)_mm512_reduce_add_epi32(sum0);
        }
    };

public:
    template <Metric m, DistPrecisionType dt, RemainderSituation rs, bool aligned>
    using Distancer = DistanceDispatcher<Avx512Policy, m, dt, rs, aligned>;

private:
    template <RemainderSituation rs, bool aligned>
    struct Avx512TransformPolicyBase {
        using VecT = __m512;
        using IntmT = float;
        static constexpr uint16 k = unroll_factor;
        static constexpr uint16 k_per_iter = 16u;
        static constexpr bool use_custom_code = true;
        static constexpr bool is_aligned = aligned;
        static constexpr RemainderSituation RS = rs;

        static INLINE_PROP VecT add(VecT a, VecT b) { return _mm512_add_ps(a, b); }
        static INLINE_PROP VecT sub(VecT a, VecT b) { return _mm512_sub_ps(a, b); }
        static INLINE_PROP VecT mul(VecT a, VecT b) { return _mm512_mul_ps(a, b); }
        static INLINE_PROP VecT div(VecT a, VecT b) { return _mm512_div_ps(a, b); }

        template <bool aligned_input>
        static INLINE_PROP VecT load(const float *v)
        {
            return aligned_input ? _mm512_load_ps(v) : _mm512_loadu_ps(v);
        }

        template <bool aligned_input>
        static INLINE_PROP Array<VecT, k> loadk(const float *v)
        {
            Array<VecT, k> res;
            ann_helper::unroll<k>([&](auto i) -> void {
                res[i] = load<aligned_input>(v + 16 * i);
            });
            return res;
        }

        template <bool aligned_input>
        static INLINE_PROP void store(float *v, VecT x)
        {
            if constexpr (aligned_input) {
                _mm512_store_ps(v, x);
            } else {
                _mm512_storeu_ps(v, x);
            }
        }

        static INLINE_PROP VecT broadcast_scalar(float v) { return _mm512_set1_ps(v); }

        static INLINE_PROP float to_interm(float v) { return v; }
        static INLINE_PROP float from_interm(float v) { return v; }
    };

    template <TransformOp op, RemainderSituation rs, bool aligned>
    struct Avx512TransformPolicy<op, DistPrecisionType::FLOAT, rs, aligned>
        : public Avx512TransformPolicyBase<rs, aligned> {
        using PlainT = float;
    };

    template <TransformOp op, RemainderSituation rs, bool aligned>
    struct Avx512TransformPolicy<op, DistPrecisionType::HALF, rs, aligned>
        : public Avx512TransformPolicyBase<rs, aligned> {
        using PlainT = half;
        using VecT = typename Avx512TransformPolicyBase<rs, aligned>::VecT;
        using IntmT = float;

        constexpr static auto k = Avx512TransformPolicyBase<rs, aligned>::k;
        constexpr static auto k_per_iter = Avx512TransformPolicyBase<rs, aligned>::k_per_iter;

        template <bool aligned_input>
        static INLINE_PROP VecT load(const half *v)
        {
            return aligned_input
                ? _mm512_cvtph_ps(_mm256_load_si256((const __m256i *)v))
                : _mm512_cvtph_ps(_mm256_loadu_si256((const __m256i *)v));
        }

        template <bool aligned_input>
        static INLINE_PROP Array<VecT, k> loadk(const half *v)
        {
            Array<VecT, k> res;
            ann_helper::unroll<k>([&](auto i) -> void {
                res[i] = load<aligned_input>(v + 16 * i);
            });
            return res;
        }

        template <bool aligned_input>
        static INLINE_PROP void store(half *v, VecT x)
        {
            __m256i hv = _mm512_cvtps_ph(x, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
            if constexpr (aligned_input) {
                _mm256_store_si256((__m256i *)v, hv);
            } else {
                _mm256_storeu_si256((__m256i *)v, hv);
            }
        }

        static INLINE_PROP float to_interm(half v) { return half_to_float(v); }
        static INLINE_PROP float to_interm(float v) { return v; }
        static INLINE_PROP half from_interm(float v) { return float_to_half(v); }
    };

    template <TransformOp op, RemainderSituation rs, bool aligned>
    struct Avx512TransformPolicy<op, DistPrecisionType::INT8, rs, aligned>
        : public Avx512TransformPolicyBase<rs, aligned> {
        using PlainT = int16;
        using VecT = typename Avx512TransformPolicyBase<rs, aligned>::VecT;
        using IntmT = float;

        constexpr static auto k = Avx512TransformPolicyBase<rs, aligned>::k;
        constexpr static auto k_per_iter = Avx512TransformPolicyBase<rs, aligned>::k_per_iter;

        template <bool aligned_input>
        static INLINE_PROP VecT load(const int16 *v)
        {
            alignas(64) float buf[k_per_iter];
            for (uint16 i = 0; i < k_per_iter; ++i) {
                buf[i] = (float)v[i];
            }
            return _mm512_load_ps(buf);
        }

        template <bool aligned_input>
        static INLINE_PROP Array<VecT, k> loadk(const int16 *v)
        {
            Array<VecT, k> res;
            ann_helper::unroll<k>([&](auto i) -> void {
                res[i] = load<aligned_input>(v + 16 * i);
            });
            return res;
        }

        template <bool aligned_input>
        static INLINE_PROP void store(int16 *v, VecT x)
        {
            alignas(64) float buf[k_per_iter];
            _mm512_store_ps(buf, x);
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
    using Transformer = TransformDispatcher<Avx512TransformPolicy, op, dt, rs, aligned>;
};

#define PatcherName Avx512DistancePatcher
#define CUR_ARCH Arch::AVX512
#include "distance/core/distance.templ"
#undef CUR_ARCH
#undef PatcherName
