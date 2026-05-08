#include <arm_neon.h>

#include "distance/core/distance_utils_core.h"
#include "half.h"
#include "distance/core/halfutils_core.h"
#include "distance/core/transform_template_core.h"

class NeonDistancePatcher {
    static constexpr uint16 unroll_factor = 4u;
public:
    template <DistPrecisionType dt>
    static constexpr RemainderSituation get_remainder_situation(uint16 dim)
    {
        constexpr uint16 k_per_iter = 16 /
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
    struct NeonPolicy;

    template <TransformOp op, DistPrecisionType dt, RemainderSituation rs, bool aligned>
    struct NeonTransformPolicy;

    template <Metric m, RemainderSituation rs, bool aligned>
    struct NeonFloatHalfBase {
        using VecT = float32x4_t;
        using IntmT = float;
        using AccT = float32x4_t;
        static constexpr uint16 k = unroll_factor;
        static constexpr uint16 k_per_iter = 4u;
        static constexpr bool use_asm_code = false;

        static constexpr bool use_custom_code = true;
        static constexpr bool is_aligned = aligned;
        static constexpr bool rem_before_reduce = false;
        static constexpr bool rem_after_reduce = true;
        static constexpr RemainderSituation RS = rs;
        static constexpr Metric M = m;

        static INLINE_PROP AccT get_zero_vectors() { return vdupq_n_f32(0.0f); }
        static INLINE_PROP VecT sub_vectors(VecT a, VecT b) { return vsubq_f32(a, b); }
        static INLINE_PROP AccT madd_vectors(VecT a, VecT b, AccT s) { return vmlaq_f32(s, a, b); }
        static INLINE_PROP AccT madd_square(VecT a, AccT s) { return vmlaq_f32(s, a, a); }
        static INLINE_PROP VecT add_vectors(VecT a, VecT b) { return vaddq_f32(a, b); }

        static INLINE_PROP float reduce(const Array<VecT, k> &accs)
        {
            VecT acc0 = add_vectors(add_vectors(accs[0], accs[1]), add_vectors(accs[2], accs[3]));
#if COMPILER_TARGET_AARCH64
            return vaddvq_f32(acc0);
#else
            float32x2_t sum64 = vpadd_f32(vget_low_f32(acc0), vget_high_f32(acc0));
            sum64 = vpadd_f32(sum64, sum64);
            return vget_lane_f32(sum64, 0);
#endif
        }
    };

    template <Metric m, RemainderSituation rs, bool aligned>
    struct NeonPolicy<m, DistPrecisionType::FLOAT, rs, aligned> 
        : NeonFloatHalfBase<m, rs, aligned> {
        using PlainT = float;
        using VecT = typename NeonFloatHalfBase<m, rs, aligned>::VecT;
        using AccT = typename NeonFloatHalfBase<m, rs, aligned>::AccT;
        constexpr static auto k = NeonFloatHalfBase<m, rs, aligned>::k;
        constexpr static auto k_per_iter = NeonFloatHalfBase<m, rs, aligned>::k_per_iter;
        static constexpr bool use_asm_code = false;

        static INLINE_PROP float transform(float v) { return v; }

        template <bool aligned_input>
        static INLINE_PROP VecT load(const float *v)
        {
            return vld1q_f32(v);
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
                } /* fall through */
                case 2u: {
                    if constexpr (m == Metric::L2) {
                        const float diff1 = x[1] - y[1];
                        res += diff1 * diff1;
                    } else {
                        res += x[1] * y[1];
                    }
                } /* fall through */
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
    struct NeonPolicy<m, DistPrecisionType::HALF, rs, aligned> 
        : NeonFloatHalfBase<m, rs, aligned> {
        using PlainT = half;
        using VecT = typename NeonFloatHalfBase<m, rs, aligned>::VecT;
        using AccT = typename NeonFloatHalfBase<m, rs, aligned>::AccT;
        constexpr static auto k = NeonFloatHalfBase<m, rs, aligned>::k;
        constexpr static auto k_per_iter = NeonFloatHalfBase<m, rs, aligned>::k_per_iter;

        static INLINE_PROP float transform(half v) { return half_to_float(v); }
        
        template <bool aligned_input>
        static INLINE_PROP VecT load(const half *v)
        {
            float16x4_t hvec = vld1_f16((const __fp16 *)v);
            return vcvt_f32_f16(hvec);
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

        static INLINE_PROP float deal_remainder(const half *x, const half *y, uint16 dim, uint16 n_tail)
        {
            x += k_per_iter * n_tail;
            y += k_per_iter * n_tail;
            float res = 0.0f;
            switch (dim % k_per_iter) {
                case 3u: {
                    if constexpr (m == Metric::L2) {
                        float diff2 = half_to_float(x[2]) - half_to_float(y[2]);
                        res += diff2 * diff2;
                    } else {
                        res += half_to_float(x[2]) * half_to_float(y[2]);
                    }
                } /* fall through */
                case 2u: {
                    if constexpr (m == Metric::L2) {
                        float diff1 = half_to_float(x[1]) - half_to_float(y[1]);
                        res += diff1 * diff1;
                    } else {
                        res += half_to_float(x[1]) * half_to_float(y[1]);
                    }
                } /* fall through */
                case 1u: {
                    if constexpr (m == Metric::L2) {
                        float diff0 = half_to_float(x[0]) - half_to_float(y[0]);
                        res += diff0 * diff0;
                    } else {
                        res += half_to_float(x[0]) * half_to_float(y[0]);
                    }
                } break;
            }
            return res;
        }
    };

    template <Metric m, RemainderSituation rs, bool aligned>
    struct NeonPolicy<m, DistPrecisionType::INT8, rs, aligned> {
        using PlainT = int8;
        using VecT = int8x16_t;
        using IntmT = int16;
        using AccT = int32x4_t;
        static constexpr uint16 k = unroll_factor;
        static constexpr uint16 k_per_iter = 16u;
        static constexpr bool use_asm_code = false;
        static constexpr bool use_custom_code = true;
        static constexpr bool is_aligned = aligned;
        static constexpr bool rem_before_reduce = true;
        static constexpr bool rem_after_reduce = false;
        static constexpr RemainderSituation RS = rs;
        static constexpr Metric M = m;

        static INLINE_PROP int8 transform(int8 v) { return v; }
        static INLINE_PROP AccT get_zero_vectors() { return vdupq_n_s32(0); }

        static INLINE_PROP VecT sub_vectors(VecT a, VecT b) { return vsubq_s8(a, b); }

        static INLINE_PROP AccT sub_madd_int8s(VecT a, VecT b, AccT s) {
            int16x8_t d_lo = vsubq_s16(vmovl_s8(vget_low_s8(a)), vmovl_s8(vget_low_s8(b)));
            int16x8_t d_hi = vsubq_s16(vmovl_s8(vget_high_s8(a)), vmovl_s8(vget_high_s8(b)));
            s = vmlal_high_s16(vmlal_s16(s, vget_low_s16(d_lo), vget_low_s16(d_lo)), d_lo, d_lo);
            return vmlal_high_s16(vmlal_s16(s, vget_low_s16(d_hi), vget_low_s16(d_hi)), d_hi, d_hi);
        }

        static INLINE_PROP AccT madd_square(VecT a, AccT s) {
            return madd_vectors(a, a, s);
        }

        static INLINE_PROP AccT madd_vectors(VecT a, VecT b, AccT s) {
#if COMPILER_SUPPORT_DOTPROD
            return vdotq_s32(s, a, b);
#else
            int16x8_t a_lo = vmovl_s8(vget_low_s8(a));
            int16x8_t a_hi = vmovl_s8(vget_high_s8(a));
            int16x8_t b_lo = vmovl_s8(vget_low_s8(b));
            int16x8_t b_hi = vmovl_s8(vget_high_s8(b));
            s = vmlal_high_s16(vmlal_s16(s, vget_low_s16(a_lo), vget_low_s16(b_lo)), a_lo, b_lo);
            return vmlal_high_s16(vmlal_s16(s, vget_low_s16(a_hi), vget_low_s16(b_hi)), a_hi, b_hi);
#endif
        }

        static INLINE_PROP AccT add_vectors(AccT a, AccT b) { return vaddq_s32(a, b); }

        template <bool aligned_input>
        static INLINE_PROP VecT load(const int8 *v) {
            return vld1q_s8(v);
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
                for (uint16 i = 0; i < left; ++i) {
                    x_buf[i] = x[i];
                    y_buf[i] = y[i];
                }
                VecT xs = vld1q_s8(x_buf);
                VecT ys = vld1q_s8(y_buf);
                if constexpr (m == Metric::L2) {
                    acc = sub_madd_int8s(xs, ys, acc);
                } else {
                    acc = madd_vectors(xs, ys, acc);
                }
            }
            return acc;
        }

        static INLINE_PROP float reduce(const Array<AccT, k> &accs) {
            AccT sum0 = vaddq_s32(vaddq_s32(accs[0], accs[1]),
                                     vaddq_s32(accs[2], accs[3]));
#if COMPILER_TARGET_AARCH64
            return (float)vaddvq_s32(sum0);
#else
            int32x2_t sum64 = vadd_s32(vget_low_s32(sum0), vget_high_s32(sum0));
            sum64 = vpadd_s32(sum64, sum64);
            return (float)vget_lane_s32(sum64, 0);
#endif
        }
    };

public:
    template <Metric m, DistPrecisionType dt, RemainderSituation rs, bool aligned>
    using Distancer = DistanceDispatcher<NeonPolicy, m, dt, rs, aligned>;

private:
    template <RemainderSituation rs, bool aligned>
    struct NeonTransformPolicyBase {
        using VecT = float32x4_t;
        using IntmT = float;
        static constexpr uint16 k = unroll_factor;
        static constexpr uint16 k_per_iter = 4u;
        static constexpr bool use_custom_code = true;
        static constexpr bool is_aligned = aligned;
        static constexpr RemainderSituation RS = rs;

        static INLINE_PROP VecT add(VecT a, VecT b) { return vaddq_f32(a, b); }
        static INLINE_PROP VecT sub(VecT a, VecT b) { return vsubq_f32(a, b); }
        static INLINE_PROP VecT mul(VecT a, VecT b) { return vmulq_f32(a, b); }
        static INLINE_PROP VecT div(VecT a, VecT b) {
#if COMPILER_TARGET_AARCH64
            return vdivq_f32(a, b);
#else
            alignas(16) float ta[4];
            alignas(16) float tb[4];
            vst1q_f32(ta, a);
            vst1q_f32(tb, b);
            for (uint16 i = 0; i < 4; ++i) {
                ta[i] = ta[i] / tb[i];
            }
            return vld1q_f32(ta);
#endif
        }

        template <bool aligned_input>
        static INLINE_PROP VecT load(const float *v)
        {
            return vld1q_f32(v);
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
            vst1q_f32(v, x);
        }

        static INLINE_PROP VecT broadcast_scalar(float v) { return vdupq_n_f32(v); }

        static INLINE_PROP float to_interm(float v) { return v; }
        static INLINE_PROP float from_interm(float v) { return v; }
    };

    template <TransformOp op, RemainderSituation rs, bool aligned>
    struct NeonTransformPolicy<op, DistPrecisionType::FLOAT, rs, aligned>
        : public NeonTransformPolicyBase<rs, aligned> {
        using PlainT = float;
    };

    template <TransformOp op, RemainderSituation rs, bool aligned>
    struct NeonTransformPolicy<op, DistPrecisionType::HALF, rs, aligned>
        : public NeonTransformPolicyBase<rs, aligned> {
        using PlainT = half;
        using VecT = typename NeonTransformPolicyBase<rs, aligned>::VecT;
        using IntmT = float;

        constexpr static auto k = NeonTransformPolicyBase<rs, aligned>::k;
        constexpr static auto k_per_iter = NeonTransformPolicyBase<rs, aligned>::k_per_iter;

        template <bool aligned_input>
        static INLINE_PROP VecT load(const half *v)
        {
            alignas(16) float buf[k_per_iter];
            for (uint16 i = 0; i < k_per_iter; ++i) {
                buf[i] = half_to_float(v[i]);
            }
            return vld1q_f32(buf);
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
            vst1q_f32(buf, x);
            for (uint16 i = 0; i < k_per_iter; ++i) {
                v[i] = float_to_half(buf[i]);
            }
        }

        static INLINE_PROP float to_interm(half v) { return half_to_float(v); }
        static INLINE_PROP float to_interm(float v) { return v; }
        static INLINE_PROP half from_interm(float v) { return float_to_half(v); }
    };

public:
    template <TransformOp op, DistPrecisionType dt, RemainderSituation rs, bool aligned>
    using Transformer = TransformDispatcher<NeonTransformPolicy, op, dt, rs, aligned>;
};

#define PatcherName NeonDistancePatcher
#define CUR_ARCH Arch::NEONV8
#include "distance/core/distance.templ"
#undef CUR_ARCH
#undef PatcherName
