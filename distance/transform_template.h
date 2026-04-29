/**
 * Copyright ...
 */

#ifndef TRANSFORM_TEMPLATE_H
#define TRANSFORM_TEMPLATE_H

#include <math.h>
#include <string.h>

#include <vtl/expr_helper>

#include "postgres.h"
#include "distance/distance.h"
#include "halfvec.h"
#include "halfutils.h"

template <typename Policy>
struct TransformHelper {
    using plaint = typename Policy::PlainT;
    using vect = typename Policy::VecT;
    using intm = typename Policy::IntmT;
    static constexpr uint16 k = Policy::k;
    static constexpr uint16 k_per_iter = Policy::k_per_iter;
    static constexpr uint16 k_step = k * k_per_iter;
    static constexpr bool has_partial_tail = Policy::RS != RemainderSituation::NoPartial;
    static constexpr bool has_rem_tail = Policy::RS == RemainderSituation::Unknown;

    template <TransformOp op>
    static INLINE_PROP void transform_single(const void *xx, const void *yy, void *oout, uint16 dim)
    {
        const plaint *x = static_cast<const plaint *>(xx);
        plaint *out = static_cast<plaint *>(oout);
        switch (op) {
            case TransformOp::ADD:
                add(x, static_cast<const plaint *>(yy), out, dim);
                break;
            case TransformOp::SUB:
                sub(x, static_cast<const plaint *>(yy), out, dim);
                break;
            case TransformOp::MUL_SCALAR:
                mul(x, scalar_from_ptr(yy), out, dim);
                break;
            case TransformOp::NORMALIZE:
                normalize(x, out, dim);
                break;
            default:
                __builtin_unreachable();
        }
    }

private:
    static INLINE_PROP float scalar_from_ptr(const void *p)
    {
        union {
            float f;
            uint32 u;
        } cvt;
        cvt.u = (uint32)(uintptr_t)p;
        return cvt.f;
    }

    static INLINE_PROP void add(const plaint *x, const plaint *y, plaint *out, uint16 dim)
    {
        CONSTEXPR_IF (!Policy::use_custom_code) {
            for (uint16 i = 0; i < dim; ++i) {
                intm a = Policy::to_interm(x[i]);
                intm b = Policy::to_interm(y[i]);
                out[i] = Policy::from_interm(a + b);
            }
        } else {
            const uint16 n_full = dim / k_step;
            const uint16 n_tail = (dim % k_step) / k_per_iter;
            const uint16 left = dim % k_per_iter;
            for (uint16 i = 0; i < n_full; ++i) {
                auto xs = Policy::template loadk<Policy::is_aligned>(x + i * k_step);
                auto ys = Policy::template loadk<Policy::is_aligned>(y + i * k_step);
                ann_helper::unroll<k>([&](auto j) -> void {
                    Policy::template store<Policy::is_aligned>(out + i * k_step + j * k_per_iter,
                        Policy::add(xs[j], ys[j]));
                });
            }
            CONSTEXPR_IF (has_partial_tail && k > 1) {
                const uint16 off = n_full * k_step;
                ann_helper::unroll<k - 1>([&](auto i) -> bool {
                    if (i >= n_tail) {
                        return false;
                    }
                    vect xv = Policy::template load<Policy::is_aligned>(x + off + i * k_per_iter);
                    vect yv = Policy::template load<Policy::is_aligned>(y + off + i * k_per_iter);
                    Policy::template store<Policy::is_aligned>(out + off + i * k_per_iter,
                        Policy::add(xv, yv));
                    return true;
                });
            }
            CONSTEXPR_IF (has_rem_tail) {
                if (left) {
                    const uint16 off = (n_full * k + n_tail) * k_per_iter;
                    for (uint16 i = 0; i < left; ++i) {
                        intm a = Policy::to_interm(x[off + i]);
                        intm b = Policy::to_interm(y[off + i]);
                        out[off + i] = Policy::from_interm(a + b);
                    }
                }
            }
        }
    }

    static INLINE_PROP void sub(const plaint *x, const plaint *y, plaint *out, uint16 dim)
    {
        CONSTEXPR_IF (!Policy::use_custom_code) {
            for (uint16 i = 0; i < dim; ++i) {
                intm a = Policy::to_interm(x[i]);
                intm b = Policy::to_interm(y[i]);
                out[i] = Policy::from_interm(a - b);
            }
        } else {
            const uint16 n_full = dim / k_step;
            const uint16 n_tail = (dim % k_step) / k_per_iter;
            const uint16 left = dim % k_per_iter;
            for (uint16 i = 0; i < n_full; ++i) {
                auto xs = Policy::template loadk<Policy::is_aligned>(x + i * k_step);
                auto ys = Policy::template loadk<Policy::is_aligned>(y + i * k_step);
                ann_helper::unroll<k>([&](auto j) -> void {
                    Policy::template store<Policy::is_aligned>(out + i * k_step + j * k_per_iter,
                        Policy::sub(xs[j], ys[j]));
                });
            }
            CONSTEXPR_IF (has_partial_tail && k > 1) {
                const uint16 off = n_full * k_step;
                ann_helper::unroll<k - 1>([&](auto i) -> bool {
                    if (i >= n_tail) {
                        return false;
                    }
                    vect xv = Policy::template load<Policy::is_aligned>(x + off + i * k_per_iter);
                    vect yv = Policy::template load<Policy::is_aligned>(y + off + i * k_per_iter);
                    Policy::template store<Policy::is_aligned>(out + off + i * k_per_iter,
                        Policy::sub(xv, yv));
                    return true;
                });
            }
            CONSTEXPR_IF (has_rem_tail) {
                if (left) {
                    const uint16 off = (n_full * k + n_tail) * k_per_iter;
                    for (uint16 i = 0; i < left; ++i) {
                        intm a = Policy::to_interm(x[off + i]);
                        intm b = Policy::to_interm(y[off + i]);
                        out[off + i] = Policy::from_interm(a - b);
                    }
                }
            }
        }
    }

    static INLINE_PROP void mul(const plaint *x, float yy, plaint *out, uint16 dim)
    {
        CONSTEXPR_IF (!Policy::use_custom_code) {
            const intm sv = Policy::to_interm(yy);
            for (uint16 i = 0; i < dim; ++i) {
                intm a = Policy::to_interm(x[i]);
                out[i] = Policy::from_interm(a * sv);
            }
        } else {
            const uint16 n_full = dim / k_step;
            const uint16 n_tail = (dim % k_step) / k_per_iter;
            const uint16 left = dim % k_per_iter;
            vect sc = Policy::broadcast_scalar(yy);
            for (uint16 i = 0; i < n_full; ++i) {
                auto xs = Policy::template loadk<Policy::is_aligned>(x + i * k_step);
                ann_helper::unroll<k>([&](auto j) -> void {
                    Policy::template store<Policy::is_aligned>(out + i * k_step + j * k_per_iter,
                        Policy::mul(xs[j], sc));
                });
            }
            CONSTEXPR_IF (has_partial_tail && k > 1) {
                const uint16 off = n_full * k_step;
                ann_helper::unroll<k - 1>([&](auto i) -> bool {
                    if (i >= n_tail) {
                        return false;
                    }
                    vect xv = Policy::template load<Policy::is_aligned>(x + off + i * k_per_iter);
                    Policy::template store<Policy::is_aligned>(out + off + i * k_per_iter,
                        Policy::mul(xv, sc));
                    return true;
                });
            }
            CONSTEXPR_IF (has_rem_tail) {
                if (left) {
                    const intm sv = Policy::to_interm(yy);
                    const uint16 off = (n_full * k + n_tail) * k_per_iter;
                    for (uint16 i = 0; i < left; ++i) {
                        intm a = Policy::to_interm(x[off + i]);
                        out[off + i] = Policy::from_interm(a * sv);
                    }
                }
            }
        }
    }

    static INLINE_PROP void normalize(const plaint *x, plaint *out, uint16 dim)
    {
        float norm_sq = 0.0f;
        for (uint16 i = 0; i < dim; ++i) {
            float v = (float)Policy::to_interm(x[i]);
            norm_sq += v * v;
        }
        if (norm_sq == 0.0f || norm_sq == 1.0f) {
            if (x != out) {
                memcpy(out, x, sizeof(plaint) * dim);
            }
            return;
        }
        const float inv_norm = 1.0f / sqrtf(norm_sq);
        mul(x, inv_norm, out, dim);
    }
};

#endif /* TRANSFORM_TEMPLATE_H */
