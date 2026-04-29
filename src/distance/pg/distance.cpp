#include <cmath>
#include <boost/preprocessor/repetition/repeat.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/stringize.hpp>
#include <vtl/string_view>

#include "pg_compat.h"
#include "parser/scansup.h"
#include "utils/fmgroids.h"
#include "distance/pg/cblas_interface.h"
#include "distance/pq/pq_endecode.h"
#include "distance/core/distance_utils_core.h"
#include "distance/core/transform_template_core.h"
#include "floatvector.h"
#include "distance/core/distance_dispatcher.h"
#include "distance/pg/distance_guc.h"
#include "ann_utils.h"
#include "halfutils.h"
#include "distance_funcs.h"

GlobalInstance g_instance;

static const Arch best_arch = ann_helper::get_best_arch();

Metric get_func_metric(Oid func_id)
{
    /* Look up function name to determine metric type */
    char *func_name = get_func_name(func_id);
    if (func_name == NULL) {
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("Could not get function name for OID %u", func_id)));
        return Metric::L2;
    }
    
    Metric result = Metric::L2;
    
    if (strcmp(func_name, "l2_distance") == 0 ||
        strcmp(func_name, "floatvector_l2_squared_distance") == 0 ||
        strcmp(func_name, "halfvector_l2_distance") == 0 ||
        strcmp(func_name, "halfvector_l2_squared_distance") == 0 ||
        strcmp(func_name, "int8vector_l2_distance") == 0 ||
        strcmp(func_name, "int8vector_l2_squared_distance") == 0) {
        result = Metric::L2;
    } else if (strcmp(func_name, "cosine_distance") == 0 ||
               strcmp(func_name, "halfvector_cosine_distance") == 0 ||
               strcmp(func_name, "int8vector_cosine_distance") == 0) {
        result = Metric::FAST_COSINE;
    } else if (strcmp(func_name, "inner_product") == 0 ||
               strcmp(func_name, "floatvector_negative_inner_product") == 0 ||
               strcmp(func_name, "halfvector_inner_product") == 0 ||
               strcmp(func_name, "halfvector_negative_inner_product") == 0 ||
               strcmp(func_name, "int8vector_inner_product") == 0 ||
               strcmp(func_name, "int8vector_negative_inner_product") == 0) {
        result = Metric::INNER_PRODUCT;
    } else if (strcmp(func_name, "floatvector_spherical_distance") == 0 ||
               strcmp(func_name, "halfvector_spherical_distance") == 0 ||
               strcmp(func_name, "int8vector_spherical_distance") == 0 ||
               strcmp(func_name, "int8vector_cosine_distance") == 0) {
        result = Metric::COSINE;
    } else {
        pfree(func_name);
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("Unsupported distance function")));
    }
    
    pfree(func_name);
    return result;
}

#if COMPILER_SUPPORT_NEONV8
#define ISA_FUNC_CALL_NEONV8(arg, call) \
    case Arch::NEONV8:                  \
        call(NEONV8_FUNC(arg));
#else
#define ISA_FUNC_CALL_NEONV8(arg, call)
#endif

#if COMPILER_SUPPORT_SVEV8
#define ISA_FUNC_CALL_SVEV8(arg, call) \
    case Arch::SVEV8:                  \
        call(SVEV8_FUNC(arg));
#else
#define ISA_FUNC_CALL_SVEV8(arg, call)
#endif

#if COMPILER_SUPPORT_SVE2V8
#define ISA_FUNC_CALL_SVE2V8(arg, call) \
    case Arch::SVE2V8:                  \
        call(SVE2V8_FUNC(arg));
#else
#define ISA_FUNC_CALL_SVE2V8(arg, call)
#endif

#if COMPILER_SUPPORT_NEONV9
#define ISA_FUNC_CALL_NEONV9(arg, call) \
    case Arch::NEONV9:                  \
        call(NEONV9_FUNC(arg));
#else
#define ISA_FUNC_CALL_NEONV9(arg, call)
#endif

#if COMPILER_SUPPORT_SVEV9
#define ISA_FUNC_CALL_SVEV9(arg, call) \
    case Arch::SVEV9:                  \
        call(SVEV9_FUNC(arg));
#else
#define ISA_FUNC_CALL_SVEV9(arg, call)
#endif

#if COMPILER_SUPPORT_SVE2V9
#define ISA_FUNC_CALL_SVE2V9(arg, call) \
    case Arch::SVE2V9:                  \
        call(SVE2V9_FUNC(arg));
#else
#define ISA_FUNC_CALL_SVE2V9(arg, call)
#endif

#if COMPILER_SUPPORT_SMEV9
#define ISA_FUNC_CALL_SMEV9(arg, call) \
    case Arch::SMEV9:                  \
        call(SMEV9_FUNC(arg));
#else
#define ISA_FUNC_CALL_SMEV9(arg, call)
#endif

#if COMPILER_SUPPORT_SME2V9
#define ISA_FUNC_CALL_SME2V9(arg, call) \
    case Arch::SME2V9:                  \
        call(SME2V9_FUNC(arg));
#else
#define ISA_FUNC_CALL_SME2V9(arg, call)
#endif

#if COMPILER_SUPPORT_SSE
#define ISA_FUNC_CALL_SSE(arg, call) \
    case Arch::SSE:                  \
        call(SSE_FUNC(arg));
#else
#define ISA_FUNC_CALL_SSE(arg, call)
#endif

#if COMPILER_SUPPORT_AVX
#define ISA_FUNC_CALL_AVX(arg, call) \
    case Arch::AVX:                  \
        call(AVX_FUNC(arg));
#else
#define ISA_FUNC_CALL_AVX(arg, call)
#endif

#if COMPILER_SUPPORT_AVX512_EXTEND
#define ISA_FUNC_CALL_AVX512(arg, call) \
    case Arch::AVX512:                 \
        call(AVX512_FUNC(arg));
#else
#define ISA_FUNC_CALL_AVX512(arg, call)
#endif

#define ARCH_FUNC_CALL(arch, arg, call) \
    switch (arch) {                     \
        ISA_FUNC_CALL_NEONV8(arg, call) \
        ISA_FUNC_CALL_SVEV8(arg, call)  \
        ISA_FUNC_CALL_SVE2V8(arg, call) \
        ISA_FUNC_CALL_NEONV9(arg, call) \
        ISA_FUNC_CALL_SVEV9(arg, call)  \
        ISA_FUNC_CALL_SVE2V9(arg, call) \
        ISA_FUNC_CALL_SMEV9(arg, call)  \
        ISA_FUNC_CALL_SME2V9(arg, call) \
        ISA_FUNC_CALL_SSE(arg, call)    \
        ISA_FUNC_CALL_AVX(arg, call)    \
        ISA_FUNC_CALL_AVX512(arg, call) \
        case Arch::GENERAL:             \
        default:                        \
            call(GENERAL_FUNC(arg));    \
    }

static void pairwise_distance_l2(const float *x, const float *y, const float *x_norm,
    const float *y_norm, uint32 dim, uint32 x_size, uint32 y_size, float *out)
{
    uint32 one_size = std::max(x_size, y_size);
    float *ones = (float *)palloc(one_size * sizeof(float));
    std::fill_n(ones, one_size, 1.0f);
    cblas_sgemm_rnt(x_size, y_size, 1, 1.0f, x_norm, 1, ones, 1, 0.0f, out, y_size);
    cblas_sgemm_rnt(x_size, y_size, 1, 1.0f, ones, 1, y_norm, 1, 1.0f, out, y_size);
    cblas_sgemm_rnt(x_size, y_size, dim, -2.0f, x, dim, y, dim, 1.0f, out, y_size);
    pfree(ones);
}

static void pairwise_distance_dot(const float *x, const float *y, uint32 dim,
    uint32 x_size, uint32 y_size, float *out)
{
    cblas_sgemm_rnt(x_size, y_size, dim, -1.0f, x, dim, y, dim, 0.0f, out, y_size);
}

namespace ann_helper {
distance_func get_general_distance_func(Metric metric, uint32 dim)
{
    return DispatchRunner<false,
        MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE, Metric::COSINE,
                   Metric::SPHERICAL, Metric::L2_SQRT, Metric::L2_NORM>,
        DistPrecisionTypeList<DistPrecisionType::FLOAT>,
        DispatcherMode::NO_QUANT>::call(metric, DistPrecisionType::FLOAT, dim, QuantizerType::NONE,
            [](auto &d) -> distance_func {
                return std::decay_t<decltype(d)>::get_distance_single;
            });
}

distance_func get_aligned_distance_func(Metric metric, uint32 dim)
{
    return DispatchRunner<true,
        MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE, Metric::COSINE>,
        DistPrecisionTypeList<DistPrecisionType::FLOAT>,
        DispatcherMode::NO_QUANT>::call(metric, DistPrecisionType::FLOAT, dim, QuantizerType::NONE,
            [](auto &d) -> distance_func {
                return std::decay_t<decltype(d)>::get_distance_single;
            });
}

distance_func_batch get_general_distance_batch_func2(Metric metric, uint32 dim)
{
    return DispatchRunner<false,
        MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE, Metric::COSINE,
                   Metric::SPHERICAL, Metric::L2_SQRT, Metric::L2_NORM>,
        DistPrecisionTypeList<DistPrecisionType::FLOAT>,
        DispatcherMode::NO_QUANT>::call(metric, DistPrecisionType::FLOAT, dim, QuantizerType::NONE,
            [](auto &d) -> distance_func_batch {
                return std::decay_t<decltype(d)>::get_distance_batch2;
            });
}

distance_func_batch get_aligned_distance_batch_func2(Metric metric, uint32 dim)
{
    return DispatchRunner<true,
        MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE, Metric::COSINE>,
        DistPrecisionTypeList<DistPrecisionType::FLOAT>,
        DispatcherMode::NO_QUANT>::call(metric, DistPrecisionType::FLOAT, dim, QuantizerType::NONE,
            [](auto &d) -> distance_func_batch {
                return std::decay_t<decltype(d)>::get_distance_batch2;
            });
}

distance_func get_general_half_distance_func(Metric metric, uint32 dim)
{
    return DispatchRunner<false,
        MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::COSINE>,
        DistPrecisionTypeList<DistPrecisionType::HALF>,
        DispatcherMode::NO_QUANT>::call(metric, DistPrecisionType::HALF, dim, QuantizerType::NONE,
            [](auto &d) -> distance_func {
                return std::decay_t<decltype(d)>::get_distance_single;
            });
}
distance_func get_aligned_half_distance_func(Metric metric, uint32 dim)
{
    return DispatchRunner<true,
        MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE, Metric::COSINE>,
        DistPrecisionTypeList<DistPrecisionType::HALF>,
        DispatcherMode::NO_QUANT>::call(metric, DistPrecisionType::HALF, dim, QuantizerType::NONE,
            [](auto &d) -> distance_func {
                return std::decay_t<decltype(d)>::get_distance_single;
            });
}
distance_func_batch get_general_half_distance_batch_func2(Metric metric, uint32 dim)
{
    /* not used */
    Assert(false);
    return NULL;
}
distance_func_batch get_aligned_half_distance_batch_func2(Metric metric, uint32 dim)
{
    /* not used */
    Assert(false);
    return NULL;
}

distance_func get_general_int8_distance_func(Metric metric, uint32 dim)
{
    return DispatchRunner<false,
        MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::COSINE>,
        DistPrecisionTypeList<DistPrecisionType::INT8>,
        DispatcherMode::NO_QUANT>::call(metric, DistPrecisionType::INT8, dim, QuantizerType::NONE,
            [](auto &d) -> distance_func {
                return std::decay_t<decltype(d)>::get_distance_single;
            });
}

distance_func get_aligned_int8_distance_func(Metric metric, uint32 dim)
{
    return DispatchRunner<true,
        MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE, Metric::COSINE>,
        DistPrecisionTypeList<DistPrecisionType::INT8>,
        DispatcherMode::NO_QUANT>::call(metric, DistPrecisionType::INT8, dim, QuantizerType::NONE,
            [](auto &d) -> distance_func {
                return std::decay_t<decltype(d)>::get_distance_single;
            });
}

distance_func_batch get_general_int8_distance_batch_func2(Metric metric, uint32 dim)
{
    /* not used */
    Assert(false);
    return NULL;
}

distance_func_batch get_aligned_int8_distance_batch_func2(Metric metric, uint32 dim)
{
    /* not used */
    Assert(false);
    return NULL;
}

distance_func get_general_distance_func(Metric metric)
    { return get_general_distance_func(metric, 1); }
distance_func get_general_half_distance_func(Metric metric)
    { return get_general_half_distance_func(metric, 1); }
distance_func get_general_int8_distance_func(Metric metric)
    { return get_general_int8_distance_func(metric, 1); }

vector_preprocess_func get_vector_preprocess_func(Metric metric, DistPrecisionType type, uint16 dim)
{
    if (metric != Metric::FAST_COSINE) {
        return NULL;
    }
    return DispatchRunner<false,
        MetricList<Metric::INNER_PRODUCT, Metric::FAST_COSINE>,
        DistPrecisionTypeList<DistPrecisionType::FLOAT, DistPrecisionType::HALF>,
        DispatcherMode::NO_QUANT>::call(Metric::FAST_COSINE, type, dim, QuantizerType::NONE,
            [](auto &d) -> vector_preprocess_func {
                using tr = typename std::decay_t<decltype(d)>::transform_type<TransformOp::NORMALIZE>;
                return [](const void *x, uint16 dim, void *out) {
                    tr::transform_single(x, x, out, dim);
                };
            });
}

float_to_half_func get_float_to_half_func()
{
#define DISTANCER_ARCH_ARG float_to_half
#define DISTANCER_ARCH_CALL(n) return n
    ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
}

half_to_float_func get_half_to_float_func()
{
#define DISTANCER_ARCH_ARG half_to_float
#define DISTANCER_ARCH_CALL(n) return n
    ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
}

fvec_ny_distance_func get_fvec_ny_distance_func(Metric Metric)
{
    switch (Metric) {
        case Metric::L2:
#define DISTANCER_ARCH_ARG fvec_L2sqr_ny
#define DISTANCER_ARCH_CALL(fvec_ny) return fvec_ny
            ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
            break;
        case Metric::INNER_PRODUCT:
#define DISTANCER_ARCH_ARG  fvec_inner_products_ny
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

fht_func get_fht_func(uint32 bottom_log_dim)
{
#define FHT_HELPER(z, i, func) case i: return func##i;
#define DISTANCER_ARCH_ARG fht_helper_
#define DISTANCER_ARCH_CALL(fht)    \
    switch (bottom_log_dim) { \
        BOOST_PP_REPEAT_FROM_TO(1, BOOST_PP_ADD(max_vector_bottom_dim, 1), FHT_HELPER, fht)   \
    }   \
    return NULL

    ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
#undef FHT_HELPER
}

void init_rabitq_func()
{
#define DISTANCER_ARCH_ARG flip_sign
#define DISTANCER_ARCH_CALL(fs) g_instance.annvec_cxt.f_flip_sign = fs; break
        ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

#define DISTANCER_ARCH_ARG kacs_walk
#define DISTANCER_ARCH_CALL(kw) g_instance.annvec_cxt.f_kacs_walk = kw; break
        ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

#define DISTANCER_ARCH_ARG warmup_ip_x0_q
#define DISTANCER_ARCH_CALL(wiq) g_instance.annvec_cxt.f_warmup_ip_x0_q = wiq; break
        ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

#define DISTANCER_ARCH_ARG ip_fxi
#define DISTANCER_ARCH_CALL(ipf) g_instance.annvec_cxt.f_ip_fxi = ipf; break
        ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL

#define DISTANCER_ARCH_ARG mask_ip_x0_q
#define DISTANCER_ARCH_CALL(miq) g_instance.annvec_cxt.f_mask_ip_x0_q = miq; break
        ARCH_FUNC_CALL(best_arch, DISTANCER_ARCH_ARG, DISTANCER_ARCH_CALL);
#undef DISTANCER_ARCH_ARG
#undef DISTANCER_ARCH_CALL
}

void pairwise_distance(const Metric metric, const float *x, const float *y, const float *x_norm,
    const float *y_norm, uint32 dim, uint32 x_size, uint32 y_size, float *out)
{
    if (metric == Metric::L2) {
        pairwise_distance_l2(x, y, x_norm, y_norm, dim, x_size, y_size, out);
    } else {
        pairwise_distance_dot(x, y, dim, x_size, y_size, out);
        if (metric == Metric::COSINE || metric == Metric::FAST_COSINE) {
            for (uint32 i = 0; i != x_size * y_size; ++i) {
                out[i] /= x_norm[i / y_size] * y_norm[i % y_size];
            }
        }
    }
}
} /* namespace ann_helper */

uint32 get_aligned_dim(uint32 dim)
    { return (dim + vector_step_size - 1) / vector_step_size * vector_step_size; }

size_t get_aligned_vec_size(size_t vec_size)
{
    return (vec_size + ann_helper::vector_aligned_size - 1) /
        ann_helper::vector_aligned_size * ann_helper::vector_aligned_size;
}

float *alloc_floatvector(uint32 dim, size_t n)
{
    void *res = mem_align_alloc(ann_helper::vector_aligned_size, sizeof(float) * dim * n);
    return (float *)__builtin_assume_aligned(res, ann_helper::vector_aligned_size);
}

char *alloc_vector(size_t vec_size, size_t n)
{
    void *res = mem_align_alloc(ann_helper::vector_aligned_size, vec_size * n);
    return (char *)__builtin_assume_aligned(res, ann_helper::vector_aligned_size);
}

bool is_aligned(const void *ptr)
{
    return reinterpret_cast<uintptr_t>(ptr) % ann_helper::vector_aligned_size == 0;
}


namespace {
constexpr uint16 vec_arch_unset = 0;

enum class UsageType : uint8 {
    ALL,
    DTYPE,
    METRIC,
    COMBO,
};

struct UsageTarget {
    UsageType type;
    uint8 dtype;
    uint8 metric;
};

struct VecArchConfig {
    uint16 all;
    uint16 dtype[3];
    uint16 metric[3];
    uint16 combo[3][3];
};

struct VecArchConfigResolved {
    uint16 float_l2_arch;
    uint16 float_ip_arch;
    uint16 float_cos_arch;
    uint16 half_l2_arch;
    uint16 half_ip_arch;
    uint16 half_cos_arch;
    uint16 int8_l2_arch;
    uint16 int8_ip_arch;
    uint16 int8_cos_arch;
};

static inline bool sv_ieq(StringView sv, const char *lit)
{
    size_t n = strlen(lit);
    return sv.size() == n && pg_strncasecmp(sv.data(), lit, n) == 0;
}

static inline StringView sv_trim(StringView sv)
{
    while (!sv.empty() && isspace((unsigned char)sv.front())) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && isspace((unsigned char)sv.back())) {
        sv.remove_suffix(1);
    }
    return sv;
}

static inline bool parse_dtype_sv(StringView sv, uint8 &dtype)
{
    if (sv_ieq(sv, "float")) {
        dtype = 0;
        return true;
    }
    if (sv_ieq(sv, "half")) {
        dtype = 1;
        return true;
    }
    if (sv_ieq(sv, "int8")) {
        dtype = 2;
        return true;
    }
    return false;
}

static inline bool parse_metric_sv(StringView sv, uint8 &metric)
{
    if (sv_ieq(sv, "l2")) {
        metric = 0;
        return true;
    }
    if (sv_ieq(sv, "ip")) {
        metric = 1;
        return true;
    }
    if (sv_ieq(sv, "cos")) {
        metric = 2;
        return true;
    }
    return false;
}

static inline Metric metric_from_idx(uint8 metric)
{
    switch (metric) {
        case 0:
            return Metric::L2;
        case 1:
            return Metric::INNER_PRODUCT;
        case 2:
            return Metric::COSINE;
        default:
            __builtin_unreachable();
    }
}

static inline DistPrecisionType dtype_from_idx(uint8 dtype)
{
    switch (dtype) {
        case 0:
            return DistPrecisionType::FLOAT;
        case 1:
            return DistPrecisionType::HALF;
        case 2:
            return DistPrecisionType::INT8;
        default:
            __builtin_unreachable();
    }
}

static inline bool parse_usage_sv(StringView sv, UsageTarget &target)
{
    sv = sv_trim(sv);
    if (sv.empty()) {
        return false;
    }
    if (sv_ieq(sv, "all")) {
        target = {UsageType::ALL, 0, 0};
        return true;
    }

    uint8 dtype = 0;
    if (parse_dtype_sv(sv, dtype)) {
        target = {UsageType::DTYPE, dtype, 0};
        return true;
    }

    uint8 metric = 0;
    if (parse_metric_sv(sv, metric)) {
        target = {UsageType::METRIC, 0, metric};
        return true;
    }

    size_t us_pos = sv.find('_');
    if (us_pos == StringView::npos) {
        return false;
    }
    StringView left = sv_trim(sv.substr(0, us_pos));
    StringView right = sv_trim(sv.substr(us_pos + 1));
    if (left.empty() || right.empty()) {
        return false;
    }
    if (!parse_dtype_sv(left, dtype) || !parse_metric_sv(right, metric)) {
        return false;
    }
    target = {UsageType::COMBO, dtype, metric};
    return true;
}

static inline const char *supported_usage_tokens()
{
    return "all,float,half,int8,l2,ip,cos,float_l2,float_ip,float_cos,"
           "half_l2,half_ip,half_cos,int8_l2,int8_ip,int8_cos";
}

static inline const char *supported_arch_tokens()
{
#define ARCH_TOKEN_STR_ITEM(r, data, isa) BOOST_PP_STRINGIZE(isa) ","
    return BOOST_PP_SEQ_FOR_EACH(ARCH_TOKEN_STR_ITEM, _, DISTANCER_ISAS);
#undef ARCH_TOKEN_STR_ITEM
}

static inline bool parse_arch_sv(StringView sv, Arch &arch)
{
    sv = sv_trim(sv);
    if (sv.empty()) {
        return false;
    }
#define PARSE_ARCH_ITEM(r, data, isa)      \
    if (sv_ieq(sv, BOOST_PP_STRINGIZE(isa))) { \
        arch = Arch::isa;                   \
        return true;                        \
    }
    BOOST_PP_SEQ_FOR_EACH(PARSE_ARCH_ITEM, _, DISTANCER_ISAS)
#undef PARSE_ARCH_ITEM

    return false;
}

static inline bool arch_supported_for_case(Arch arch, DistPrecisionType dt, Metric metric)
{
    return ann_helper::is_arch_available(arch, metric, dt);
}

static inline void vec_cfg_init(VecArchConfig &cfg)
{
    cfg.all = vec_arch_unset;
    for (uint8 i = 0; i < 3; ++i) {
        cfg.dtype[i] = vec_arch_unset;
        cfg.metric[i] = vec_arch_unset;
        for (uint8 j = 0; j < 3; ++j) {
            cfg.combo[i][j] = vec_arch_unset;
        }
    }
}

static bool validate_usage_arch(UsageTarget usage, Arch arch)
{
    uint8 d_start = 0;
    uint8 d_end = 2;
    uint8 m_start = 0;
    uint8 m_end = 2;
    if (usage.type == UsageType::DTYPE || usage.type == UsageType::COMBO) {
        d_start = usage.dtype;
        d_end = usage.dtype;
    }
    if (usage.type == UsageType::METRIC || usage.type == UsageType::COMBO) {
        m_start = usage.metric;
        m_end = usage.metric;
    }
    for (uint8 d = d_start; d <= d_end; ++d) {
        for (uint8 m = m_start; m <= m_end; ++m) {
            DistPrecisionType dt = dtype_from_idx(d);
            Metric mt = metric_from_idx(m);
            if (!arch_supported_for_case(arch, dt, mt)) {
                GUC_check_errdetail("arch is unavailable for requested usage on this system");
                return false;
            }
        }
    }
    return true;
}

static bool parse_vec_arch_rule(StringView rule, UsageTarget &usage, Arch &arch)
{
    rule = sv_trim(rule);
    size_t pos = rule.find(':');
    if (pos == StringView::npos) {
        GUC_check_errdetail("rule must be in '<usage>:<arch>' format");
        return false;
    }
    StringView usage_sv = sv_trim(rule.substr(0, pos));
    StringView arch_sv = sv_trim(rule.substr(pos + 1));
    if (usage_sv.empty() || arch_sv.empty()) {
        GUC_check_errdetail("usage and arch must be non-empty");
        return false;
    }
    if (!parse_usage_sv(usage_sv, usage)) {
        GUC_check_errdetail("invalid usage token in vec_architecture, supported usage tokens: %s",
            supported_usage_tokens());
        return false;
    }
    if (!parse_arch_sv(arch_sv, arch)) {
        GUC_check_errdetail("invalid arch token in vec_architecture, supported arch tokens: %s",
            supported_arch_tokens());
        return false;
    }
    if (!validate_usage_arch(usage, arch)) {
        return false;
    }
    return true;
}

static bool parse_vec_arch_str_impl(const char *newval, VecArchConfig &cfg)
{
    vec_cfg_init(cfg);
    if (newval == NULL || newval[0] == '\0') {
        return true;
    }

    for (StringView elem : split(StringView(newval), ',', true)) {
        UsageTarget usage;
        Arch arch;
        if (!parse_vec_arch_rule(StringView(elem), usage, arch)) {
            return false;
        }
        uint16 archv = (uint16)arch + 1;
        switch (usage.type) {
            case UsageType::ALL:
                cfg.all = archv;
                break;
            case UsageType::DTYPE:
                cfg.dtype[usage.dtype] = archv;
                break;
            case UsageType::METRIC:
                cfg.metric[usage.metric] = archv;
                break;
            case UsageType::COMBO:
                cfg.combo[usage.dtype][usage.metric] = archv;
                break;
            default:
                Assume(false);
        }
    }

    return true;
}

static inline uint16 resolve_arch_for_case(const VecArchConfig &cfg, uint8 dtype, uint8 metric)
{
    if (cfg.combo[dtype][metric] != vec_arch_unset) {
        return cfg.combo[dtype][metric];
    }
    if (cfg.metric[metric] != vec_arch_unset) {
        return cfg.metric[metric];
    }
    if (cfg.dtype[dtype] != vec_arch_unset) {
        return cfg.dtype[dtype];
    }
    return cfg.all;
}

static void resolve_vec_arch_cfg(const VecArchConfig &cfg, VecArchConfigResolved &resolved)
{
    resolved.float_l2_arch = resolve_arch_for_case(cfg, 0, 0);
    resolved.float_ip_arch = resolve_arch_for_case(cfg, 0, 1);
    resolved.float_cos_arch = resolve_arch_for_case(cfg, 0, 2);

    resolved.half_l2_arch = resolve_arch_for_case(cfg, 1, 0);
    resolved.half_ip_arch = resolve_arch_for_case(cfg, 1, 1);
    resolved.half_cos_arch = resolve_arch_for_case(cfg, 1, 2);

    resolved.int8_l2_arch = resolve_arch_for_case(cfg, 2, 0);
    resolved.int8_ip_arch = resolve_arch_for_case(cfg, 2, 1);
    resolved.int8_cos_arch = resolve_arch_for_case(cfg, 2, 2);
}

static void apply_vec_arch_cfg(const VecArchConfigResolved &resolved)
{
    pg_vexdb_session.attr_storage.float_l2_arch = resolved.float_l2_arch;
    pg_vexdb_session.attr_storage.float_ip_arch = resolved.float_ip_arch;
    pg_vexdb_session.attr_storage.float_cos_arch = resolved.float_cos_arch;

    pg_vexdb_session.attr_storage.half_l2_arch = resolved.half_l2_arch;
    pg_vexdb_session.attr_storage.half_ip_arch = resolved.half_ip_arch;
    pg_vexdb_session.attr_storage.half_cos_arch = resolved.half_cos_arch;

    pg_vexdb_session.attr_storage.int8_l2_arch = resolved.int8_l2_arch;
    pg_vexdb_session.attr_storage.int8_ip_arch = resolved.int8_ip_arch;
    pg_vexdb_session.attr_storage.int8_cos_arch = resolved.int8_cos_arch;
}
} /* namespace */

extern "C" {

bool check_vec_arch_str(char **newval, void **extra, GucSource source)
{
    (void)source;
    VecArchConfig cfg;
    if (!parse_vec_arch_str_impl(*newval, cfg)) {
        return false;
    }
    VecArchConfigResolved resolved;
    resolve_vec_arch_cfg(cfg, resolved);
    VecArchConfigResolved *myextra =
        (VecArchConfigResolved *)MemoryContextAlloc(TopMemoryContext,
            sizeof(VecArchConfigResolved));
    if (myextra == NULL) {
        return false;
    }
    *myextra = resolved;
    *extra = (void *)myextra;
    return true;
}

void assign_vec_arch(const char *newval, void *extra)
{
    (void)newval;
    if (extra == NULL) {
        return;
    }
    apply_vec_arch_cfg(*(VecArchConfigResolved *)extra);
}

} /* extern "C" */
