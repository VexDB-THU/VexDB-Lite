// Backend-neutral RaBitQ kernel dispatch. PG, DuckDB and SQLite compile the
// same per-ISA template and select the best implementation at runtime here.
#include <boost/preprocessor/arithmetic/add.hpp>
#include <boost/preprocessor/repetition/repeat_from_to.hpp>

#include "distance/core/arch_dispatch_macros.h"
#include "distance/core/distance.h"
#include "distance/core/distance_dispatcher.h"
#include "distance/core/distance_utils_core.h"

namespace ann_helper {

static const Arch rabitq_best_arch = ann_helper::get_best_arch();

distance_func get_general_distance_func(Metric metric, uint32 dim)
{
    return DispatchRunner<false,
        MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE,
                   Metric::COSINE, Metric::SPHERICAL, Metric::L2_SQRT,
                   Metric::L2_NORM>,
        DistPrecisionTypeList<DistPrecisionType::FLOAT>,
        DispatcherMode::NO_QUANT>::call(
            metric, DistPrecisionType::FLOAT, static_cast<uint16>(dim),
            QuantizerType::NONE,
            [](auto &distancer) -> distance_func {
                return std::decay_t<decltype(distancer)>::get_distance_single;
            });
}

fht_func get_fht_func(uint32 bottom_log_dim)
{
    if (bottom_log_dim == 0) {
        return +[](float *) {};
    }
#define FHT_HELPER(z, i, func) case i: return func##i;
#define DISTANCER_ARCH_ARG fht_helper_
#define DISTANCER_ARCH_CALL(fht) \
    switch (bottom_log_dim) { \
        BOOST_PP_REPEAT_FROM_TO(1, BOOST_PP_ADD(max_vector_bottom_dim, 1), FHT_HELPER, fht) \
    } \
    return nullptr

    ARCH_FUNC_CALL(rabitq_best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
#undef FHT_HELPER
}

RabitqKernels get_rabitq_kernels()
{
    RabitqKernels kernels;

#define DISTANCER_ARCH_ARG flip_sign
#define DISTANCER_ARCH_CALL(fn) kernels.flip_sign = fn
    ARCH_FUNC_CALL(rabitq_best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

#define DISTANCER_ARCH_ARG kacs_walk
#define DISTANCER_ARCH_CALL(fn) kernels.kacs_walk = fn
    ARCH_FUNC_CALL(rabitq_best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

#define DISTANCER_ARCH_ARG warmup_ip_x0_q
#define DISTANCER_ARCH_CALL(fn) kernels.warmup_ip_x0_q = fn
    ARCH_FUNC_CALL(rabitq_best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

#define DISTANCER_ARCH_ARG ip_fxi
#define DISTANCER_ARCH_CALL(fn) kernels.ip_fxi = fn
    ARCH_FUNC_CALL(rabitq_best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

#define DISTANCER_ARCH_ARG mask_ip_x0_q
#define DISTANCER_ARCH_CALL(fn) kernels.mask_ip_x0_q = fn
    ARCH_FUNC_CALL(rabitq_best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

    return kernels;
}

// Kept for the existing PG initialization call. Kernel selection is now
// stateless, so initialization no longer writes PG global function pointers.
void init_rabitq_func() {}

} // namespace ann_helper
