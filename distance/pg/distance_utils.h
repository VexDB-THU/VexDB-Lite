/**
 * Copyright ...
 * Definitions for distance names
 */

#ifndef DISTANCE_UTILS_H
#define DISTANCE_UTILS_H

#include <boost/preprocessor/seq.hpp>
#include <boost/preprocessor/repetition/repeat_from_to.hpp>
#include <boost/preprocessor/arithmetic/add.hpp>
#include <boost/preprocessor/control/if.hpp>

#include "pg_compat.h"
#include "distance/core/distance.h"
#include "floatvector.h"

#if __GNUC__ >= 10
#define INLINE_PROP FORCE_INLINE
#else
#define INLINE_PROP
#endif

#include "distance/core/distance_template.h"
#include "distance/pg/transform_template.h"

#define NEONV8_FUNC(name) neonv8_##name
#define SVEV8_FUNC(name) svev8_##name
#define SVE2V8_FUNC(name) sve2v8_##name
#define NEONV9_FUNC(name) neonv9_##name
#define SVEV9_FUNC(name) svev9_##name
#define SVE2V9_FUNC(name) sve2v9_##name
#define SMEV9_FUNC(name) smev9_##name
#define SME2V9_FUNC(name) sme2v9_##name
#define SSE_FUNC(name) sse_##name
#define AVX_FUNC(name) avx_##name
#define AVX512_FUNC(name) avx512_##name
#define GENERAL_FUNC(name) genernal_##name
#define NEONV8_STRUCT(name) NeonV8##name
#define SVEV8_STRUCT(name) SveV8##name
#define SVE2V8_STRUCT(name) Sve2V8##name
#define NEONV9_STRUCT(name) NeonV9##name
#define SVEV9_STRUCT(name) SveV9##name
#define SVE2V9_STRUCT(name) Sve2V9##name
#define SMEV9_STRUCT(name) SmeV9##name
#define SME2V9_STRUCT(name) Sme2V9##name
#define SSE_STRUCT(name) Sse##name
#define AVX_STRUCT(name) Avx##name
#define AVX512_STRUCT(name) Avx512##name
#define GENERAL_STRUCT(name) Genernal##name

#define max_vector_bottom_dim 14
namespace detail {
/* n cannot be 0 */
constexpr inline uint32 floor_log2(uint32 n)
{
    uint32 res = 0;
    constexpr uint32 a[] = {16, 8, 4, 2, 1};
    for (uint32 off : a) {
        if (n >= (1u << off)) {
            n >>= off;
            res += off;
        }
    }
    return res;
}
static_assert(floor_log2(FLOATVECTOR_MAX_DIM) <= max_vector_bottom_dim,
    "incorrect max_vector_bottom_dim");
}   /* detail */

#define DECL_DISTANCE(z, data, isa) \
    void BOOST_PP_CAT(isa, _FUNC(fvec_inner_products_ny))(float *dis, const float *x,   \
        const float *y, uint32 d, uint32 ny);   \
    void BOOST_PP_CAT(isa, _FUNC(fvec_L2sqr_ny))(float *dis, const float *x,    \
        const float *y, uint32 d, uint32 ny);   \
    uint32 BOOST_PP_CAT(isa, _FUNC(fvec_L2sqr_ny_nearest))(float *distances_tmp_buffer, \
        const float *x, const float *y, uint32 d, uint32 ny); \
    half BOOST_PP_CAT(isa, _FUNC(float_to_half))(float num); \
    float BOOST_PP_CAT(isa, _FUNC(half_to_float))(half num); 
#define DECL_DISTANCE2(z, data, isa) \
    float BOOST_PP_CAT(isa, _FUNC(distance_single_code_g))( \
        uint32 M, uint32 nbits, const float *sim_table, const uint8 *code); \
    float BOOST_PP_CAT(isa, _FUNC(distance_single_code_8))( \
        uint32 M, uint32 nbits, const float *sim_table, const uint8 *code); \
    float BOOST_PP_CAT(isa, _FUNC(distance_single_code_16))(\
        uint32 M, uint32 nbits, const float *sim_table, const uint8 *code); \
    void BOOST_PP_CAT(isa, _FUNC(distance_four_codes_g))(   \
        const uint32 M, const uint32 nbits, const float *sim_table, \
        const uint8 *__restrict code0, const uint8 *__restrict code1,   \
        const uint8 *__restrict code2, const uint8 *__restrict code3,   \
        float &result0, float &result1, float &result2, float &result3);\
    void BOOST_PP_CAT(isa, _FUNC(distance_four_codes_8))(   \
        const uint32 M, const uint32 nbits, const float *sim_table, \
        const uint8 *__restrict code0, const uint8 *__restrict code1,   \
        const uint8 *__restrict code2, const uint8 *__restrict code3,   \
        float &result0, float &result1, float &result2, float &result3);\
    void BOOST_PP_CAT(isa, _FUNC(distance_four_codes_16))(  \
        const uint32 M, const uint32 nbits, const float *sim_table, \
        const uint8 *__restrict code0, const uint8 *__restrict code1,   \
        const uint8 *__restrict code2, const uint8 *__restrict code3,   \
        float &result0, float &result1, float &result2, float &result3);

#define DECL_DISTANCE3_HELPER(z, i, isa)  \
    void BOOST_PP_CAT(isa, _FUNC(fht_helper_##i))(float *buf);
#define DECL_DISTANCE3(z, data, isa) \
    BOOST_PP_REPEAT_FROM_TO(1, BOOST_PP_ADD(max_vector_bottom_dim, 1), DECL_DISTANCE3_HELPER, isa)  \
    void BOOST_PP_CAT(isa, _FUNC(flip_sign))(const uint8 *flip, float *data, size_t dim);   \
    void BOOST_PP_CAT(isa, _FUNC(kacs_walk))(float *data, size_t len);  \
    float BOOST_PP_CAT(isa, _FUNC(warmup_ip_x0_q))(uint64 *data, const uint64 *query, float delta, float vl, size_t dim);  \
    float BOOST_PP_CAT(isa, _FUNC(ip_fxi))(float *query, uint8 *data, size_t dim);  \
    float BOOST_PP_CAT(isa, _FUNC(mask_ip_x0_q))(float *query, uint64 *data, size_t dim);

#define GENERATE_ISA_DECLARATIONS(r, data, isa) \
    DECL_DISTANCE(_, _, isa)    \
    DECL_DISTANCE2(_, _, isa)   \
    DECL_DISTANCE3(_, _, isa)

namespace ann_helper {
Arch get_best_arch();
bool is_arch_available(Arch arch, Metric m, DistPrecisionType dt);
BOOST_PP_SEQ_FOR_EACH(GENERATE_ISA_DECLARATIONS, _, DISTANCER_ISAS)
} /* namespace ann_helper */

namespace internal {
template <typename>
constexpr std::false_type has_turn_off_h(long);
template <typename T>
constexpr auto has_turn_off_h(int) -> decltype(T::turn_off(), std::true_type{});
template <typename T>
using has_turn_off = decltype(has_turn_off_h<T>(0));
template <typename T>
void optional_turn_off(std::true_type const &) { T::turn_off(); }
template <typename T>
void optional_turn_off(std::false_type const &) {}
template <typename T>
void turn_off() { optional_turn_off<T>(has_turn_off<T>{}); }
} /* namespace internal */

template <template <Metric, DistPrecisionType, RemainderSituation, bool> class PolicyClass,
          Metric m, DistPrecisionType dt, RemainderSituation rs, bool aligned>
struct DistanceDispatcher {
    static float get_distance_single(const void *x, const void *y, uint16 dim)
    {
        using Policy = PolicyClass<m, dt, rs, aligned>;
        using PlainT = typename Policy::PlainT;
        constexpr auto func = helper::template get_function<m>();
        float res = func(static_cast<const PlainT *>(x), static_cast<const PlainT *>(y), dim);
        internal::turn_off<Policy>();
        return res;
    }

    static void get_distance_batch(const void *x, const void *y, uint16 dim, uint16 y_size, float *out)
    {
        using Policy = PolicyClass<m, dt, rs, rs == RemainderSituation::Unknown ? false : aligned>;
        using PlainT = typename Policy::PlainT;
        constexpr auto func = helper::template get_function<m>();
        const PlainT *xp = static_cast<const PlainT *>(x);
        const PlainT *yp = static_cast<const PlainT *>(y);
        for (uint16 i = 0; i < y_size; ++i) {
            out[i] = func(xp, yp, dim);
            yp += dim;
        }
        internal::turn_off<Policy>();
    }

    static void get_distance_batch2(const void *x, void *const *y, uint16 dim, uint16 y_size, float *out)
    {
        using Policy = PolicyClass<m, dt, rs, aligned>;
        using PlainT = typename Policy::PlainT;
        constexpr auto func = helper::template get_function<m>();
        const PlainT *xp = static_cast<const PlainT *>(x);
        PlainT *const *yp = reinterpret_cast<PlainT *const *>(y);
        for (uint16 i = 0; i < y_size; ++i) {
            ann_helper::unroll<helper::k_bytes * helper::skip_step / helper::cache_size>(
                [&](auto j) -> void {
                    favored_prefetch((char *)yp[i] + j * helper::cache_size);
                });
            out[i] = func(xp, yp[i], dim);
        }
        internal::turn_off<Policy>();
    }

    static FORCE_INLINE void unpolluted_prefetch(void *ptr) { helper::unpolluted_prefetch(ptr); }
    static FORCE_INLINE void favored_prefetch(void *ptr) { helper::favored_prefetch(ptr); }

private:
    using helper = DistanceHelper<PolicyClass<m, dt, rs, aligned>>;
};

template <template <TransformOp, DistPrecisionType, RemainderSituation, bool> class PolicyClass,
          TransformOp op, DistPrecisionType dt, RemainderSituation rs, bool aligned>
struct TransformDispatcher {
    static void transform_single(const void *x, const void *y, void *out, uint16 dim)
    {
        using Policy = PolicyClass<op, dt, rs, aligned>;
        using helper = TransformHelper<Policy>;
        helper::template transform_single<op>(x, y, out, dim);
        internal::turn_off<Policy>();
    }
};

#undef DECL_DISTANCE3_HELPER
#undef DECL_DISTANCE3_HELPER2
#undef DECL_DISTANCE3_HELPER3
#undef DECL_DISTANCE
#undef DECL_DISTANCE2
#undef DECL_DISTANCE3
#undef GENERATE_ISA_DECLARATIONS

#endif /* DISTANCE_UTILS_H */
