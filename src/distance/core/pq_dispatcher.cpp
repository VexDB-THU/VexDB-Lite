// Backend-neutral PQ distance dispatchers shared by PG and duck builds.
#include "distance/core/distance.h"
#include "distance/core/distance_utils_core.h"
#include "distance/core/arch_dispatch_macros.h"
#include "distance_funcs.h"

namespace ann_helper {

static const Arch best_arch = ann_helper::get_best_arch();

fvec_ny_distance_func get_fvec_ny_distance_func(Metric metric)
{
    switch (metric) {
        case Metric::L2:
#define DISTANCER_ARCH_ARG fvec_L2sqr_ny
#define DISTANCER_ARCH_CALL(fvec_ny) return fvec_ny
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
            break;
        case Metric::INNER_PRODUCT:
#define DISTANCER_ARCH_ARG fvec_inner_products_ny
#define DISTANCER_ARCH_CALL(fvec_ny) return fvec_ny
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
            break;
        default:
            __builtin_unreachable();
    }
}

fvec_L2sqr_ny_nearest_func get_fvec_L2sqr_ny_nearest_func()
{
#define DISTANCER_ARCH_ARG fvec_L2sqr_ny_nearest
#define DISTANCER_ARCH_CALL(nearest) return nearest
    ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
}

distance_single_code_func get_distance_single_code_func(uint32 nbits)
{
    switch (nbits) {
        case 8:
#define DISTANCER_ARCH_ARG distance_single_code_8
#define DISTANCER_ARCH_CALL(d2code) return d2code
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
            break;
        case 16:
#define DISTANCER_ARCH_ARG distance_single_code_16
#define DISTANCER_ARCH_CALL(d2code) return d2code
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
            break;
        default:
#define DISTANCER_ARCH_ARG distance_single_code_g
#define DISTANCER_ARCH_CALL(d2code) return d2code
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
            break;
    }
}

distance_four_codes_func get_distance_four_codes_func(uint32 nbits)
{
    switch (nbits) {
        case 8:
#define DISTANCER_ARCH_ARG distance_four_codes_8
#define DISTANCER_ARCH_CALL(d2code4) return d2code4
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
            break;
        case 16:
#define DISTANCER_ARCH_ARG distance_four_codes_16
#define DISTANCER_ARCH_CALL(d2code4) return d2code4
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
            break;
        default:
#define DISTANCER_ARCH_ARG distance_four_codes_g
#define DISTANCER_ARCH_CALL(d2code4) return d2code4
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
            break;
    }
}

} // namespace ann_helper
