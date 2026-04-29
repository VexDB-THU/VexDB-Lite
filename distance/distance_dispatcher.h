/**
 * Copyright ...
 */

#ifndef DISTANCE_DISPATCHER_H
#define DISTANCE_DISPATCHER_H

#include "distance/distance.h"
#include "pq.h"
#include "rabitq/rabitq_distancer.h"
#include "graph_index/graph_index_struct.h"

enum class DispatcherMode {
    DEFAULT,        /* use direct distancer */
    BUILD_PAIR,     /* 1: distancer with plain; 2: distancer with potential quantizer */
    NO_QUANT,       /* use direct distancer and we know there won't be quantization */
    BUILD_PAIR_NO_QUANT,
};

/**
 * For conveniance, FAST_COSINE and IP are treated as the same, and we will ignore FAST_COSINE
 * and redirect that metric to IP.
 * 
 * Note that int8 use cosine distance directly since normalization does not make sense.
 * To avoid adding overhead anywhere it calls, we will add a hard judgement in the metric logic.
 */
template <bool aligned, typename MetricSeq, typename DistPrecisionTypeSeq,
          DispatcherMode mode = DispatcherMode::DEFAULT>
class DispatchRunner;
template <bool aligned, Metric... Ms, DistPrecisionType... Ds, DispatcherMode mode>
class DispatchRunner<aligned, MetricList<Ms...>, DistPrecisionTypeList<Ds...>, mode> {
public:
    template <typename F>
    static auto call(const GraphIndexMetaPage metap, F &&f) {
        return call(metap->metric, metap->precision_type, metap->dimension,
            metap->quantizer_metainfo.get_type(), std::forward<F>(f));
    }
    template <typename F>
    static auto call(Metric m, DistPrecisionType dp, uint16 dim, QuantizerType qt, F &&f) {
        CONSTEXPR_IF (mode != DispatcherMode::NO_QUANT && mode != DispatcherMode::BUILD_PAIR_NO_QUANT) {
            if (qt == QuantizerType::PQ) {
                CONSTEXPR_IF (mode == DispatcherMode::DEFAULT) {
                    return call<PQDistancer>(std::forward<F>(f));
                } else CONSTEXPR_IF (mode == DispatcherMode::BUILD_PAIR) {
                    return DispatchRunner<aligned, MetricList<Ms...>, DistPrecisionTypeList<Ds...>,
                                          DispatcherMode::DEFAULT>::call(
                        m, dp, dim, QuantizerType::NONE,
                        [&](auto &d1) {
                            return call<PQDistancer>([&](auto &d2) { return f(d1, d2); });
                        }
                    );
                }
            }
            if (qt == QuantizerType::RABITQ) {
                CONSTEXPR_IF (mode == DispatcherMode::DEFAULT) {
                    return call<rabitq::RabitqDistancer>(std::forward<F>(f));
                } else CONSTEXPR_IF (mode == DispatcherMode::BUILD_PAIR) {
                    return DispatchRunner<aligned, MetricList<Ms...>, DistPrecisionTypeList<Ds...>,
                                          DispatcherMode::DEFAULT>::call(
                        m, dp, dim, QuantizerType::NONE,
                        [&](auto &d1) {
                            return call<rabitq::RabitqDistancer>([&](auto &d2) { return f(d1, d2); });
                        }
                    );
                }
            }
        } else {
            Assert(qt == QuantizerType::NONE);
        }
        if (m == Metric::FAST_COSINE) {
            m = Metric::INNER_PRODUCT;
        }
        Arch arch = get_best_arch(m, dp, dim);
#define ENUM_ITEM(r, data, elem) \
    case Arch::elem: \
        return call<Arch::elem>(m, dp, dim, std::forward<F>(f));
        switch (arch) {
            BOOST_PP_SEQ_FOR_EACH(ENUM_ITEM, _, DISTANCER_ISAS)
            default:
                Assume(false);
        }
#undef ENUM_ITEM
    }

private:
    template <Arch arch, typename F>
    static auto call(Metric m, DistPrecisionType dp, uint16 dim, F &&f) {
        return check_metric<arch, Ms...>(m, dp, dim, std::forward<F>(f));
    }

    template <Arch arch, Metric M, Metric... Rest, typename F>
    static auto check_metric(Metric m, DistPrecisionType dp, uint16 dim, F &&f) {
        if constexpr (M != Metric::FAST_COSINE) {
            if (m == M) {
                return check_dptype<arch, M, Ds...>(dp, dim, std::forward<F>(f));
            }
        }
        if constexpr (sizeof...(Rest) > 0) {
            return check_metric<arch, Rest...>(m, dp, dim, std::forward<F>(f));
        }
        Assume(m == Metric::COSINE && dp == DistPrecisionType::INT8);
        RemainderSituation rs = RemainderPatcher<arch, DistPrecisionType::INT8>::get_remainder_situation(dim);
        using ResCats = typename RemainderPatcher<arch, DistPrecisionType::INT8>::res_cats;
        return check_remainder<arch, Metric::COSINE, DistPrecisionType::INT8>(ResCats{}, rs, std::forward<F>(f));
    }

    template <Arch arch, Metric M, DistPrecisionType D, DistPrecisionType... Rest, typename F>
    static auto check_dptype(DistPrecisionType dp, uint16 dim, F &&f) {
        if (dp == D) {
            RemainderSituation rs = RemainderPatcher<arch, D>::get_remainder_situation(dim);
            using ResCats = typename RemainderPatcher<arch, D>::res_cats;
            return check_remainder<arch, M, D>(ResCats{}, rs, std::forward<F>(f));
        }
        if constexpr (sizeof...(Rest) > 0) {
            return check_dptype<arch, M, Rest...>(dp, dim, std::forward<F>(f));
        }
        Assume(false);
    }

    template <Arch arch, Metric M, DistPrecisionType D, RemainderSituation... Rs, typename F>
    static auto check_remainder(RemainderSituationList<Rs...>, RemainderSituation rs, F &&f)
    {
        return check_remainder<arch, M, D, Rs...>(rs, std::forward<F>(f));
    }

    template <Arch arch, Metric M, DistPrecisionType D, RemainderSituation R,
              RemainderSituation... Rest, typename F>
    static auto check_remainder(RemainderSituation rs, F &&f)
    {
        if (rs == R) {
            using dist_type = Distancer<arch, M, D, R, aligned>;
            CONSTEXPR_IF (mode == DispatcherMode::BUILD_PAIR || mode == DispatcherMode::BUILD_PAIR_NO_QUANT) {
                return call2<dist_type>(std::forward<F>(f));
            } else {
                return call<dist_type>(std::forward<F>(f));
            }
        }
        if constexpr (sizeof...(Rest) > 0) {
            return check_remainder<arch, M, D, Rest...>(rs, std::forward<F>(f));
        }
        Assume(false);
    }

    template <typename D, typename F>
    static auto call(F &&f)
    {
        D distancer;
        if constexpr (std::is_void_v<RESULT_OF(F, D &)>) {
            f(distancer);
            distancer.destroy();
            return;
        } else {
            auto res = f(distancer);
            distancer.destroy();
            return res;
        }
    }

    template <typename D, typename F>
    static auto call2(F &&f)
    {
        D d1;
        D d2;
        if constexpr (std::is_void_v<RESULT_OF(F, D &, D &)>) {
            f(d1, d2);
            d1.destroy();
            d2.destroy();
            return;
        } else {
            auto res = f(d1, d2);
            d1.destroy();
            d2.destroy();
            return res;
        }
    }
};

#endif /* DISTANCE_DISPATCHER_H */
