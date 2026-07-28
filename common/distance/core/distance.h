/**
 * Copyright (c) 2026 VexDB-THU
 * Distance calculation utilities.
 */

#ifndef DISKANN_UTILS_DISTANCE_H
#define DISKANN_UTILS_DISTANCE_H

#include <functional>
#include <random>
#include <thread>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <boost/preprocessor/seq.hpp>
#include <vtl/expr_helper>

/* Vector alignment constants must be available to early includes (e.g. storage headers). */
namespace ann_helper {
#ifdef __x86_64__
constexpr size_t vector_aligned_size = 64ul;
#define vector_step_size 16
#elif defined(__arm__) || defined(__arm) || defined(__aarch64__) || defined(__aarch64)
constexpr size_t vector_aligned_size = 32ul;
#define vector_step_size 8
#else
constexpr size_t vector_aligned_size = 64ul;
#define vector_step_size 16
#endif
}

#if defined(PG_VEXDB_TARGET_PG)
extern "C" {
#include "postgres.h"
#include "miscadmin.h"
#include "utils/rel.h"
}
#elif (defined(PG_VEXDB_TARGET_DUCK) || defined(PG_VEXDB_TARGET_SQLITE))
using Relation = void *;
#endif

#include "distance/core/architecture_macro.h"
#include "distance/core/distance_func.h"
#if !(defined(PG_VEXDB_TARGET_DUCK) || defined(PG_VEXDB_TARGET_SQLITE))
#include "quantizer.h"
#endif

static_assert(__cplusplus >= 201402L, "C++14 or later required");

enum class Arch : uint16 {
    BOOST_PP_SEQ_ENUM(DISTANCER_ISAS)
};

#if (defined(PG_VEXDB_TARGET_DUCK) || defined(PG_VEXDB_TARGET_SQLITE))
enum class Metric : uint32 {
    L2 = 0,
    COSINE = 1,
    INNER_PRODUCT = 2,
    FAST_COSINE = 4,
    SPHERICAL = 8,
    L2_SQRT = 9,
    L2_NORM = 10,
    CUSTOM = 11
};

enum class TransformOp : uint8 {
    ADD,
    SUB,
    MUL_SCALAR,
    NORMALIZE
};

enum class DistPrecisionType : uint8 {
    FLOAT,
    HALF,  /* deprecated: kept for ABI/enum-stability; no longer implemented */
    INT8,
    CUSTOM,
    /* sentinel: boost.PP single-element SEQ placeholder. Never appears at runtime. */
    _DISPATCH_PAD_ = 0xFE,
};

constexpr inline size_t get_dtype_size(DistPrecisionType dt)
{
    switch (dt) {
        case DistPrecisionType::FLOAT:
            return 4;
        case DistPrecisionType::INT8:
            return 1;
        case DistPrecisionType::_DISPATCH_PAD_:
            return 4;
        default:
            __builtin_unreachable();
    }
}

enum class RemainderSituation {
    Unknown,
    NoPartial,
    NoTail
};

#define DIST_PRECISION_TYPE_SEQ ((FLOAT, 4)) ((INT8, 1))
#define REMAINDER_SITUATION_SEQ (Unknown)(NoPartial)(NoTail)
#define TRANSFORM_OP_SEQ (ADD)(SUB)(MUL_SCALAR)(NORMALIZE)
#else
#define METRIC_SEQ \
    ((L2, 0)) \
    ((COSINE, 1)) \
    ((INNER_PRODUCT, 2)) \
    ((FAST_COSINE, 4)) \
    ((SPHERICAL, 8)) \
    ((L2_SQRT, 9)) \
    ((L2_NORM, 10))

#define ENUM_ITEM(r, data, elem) \
    BOOST_PP_TUPLE_ELEM(0, elem) = BOOST_PP_TUPLE_ELEM(1, elem),
enum class Metric : uint32 {
    BOOST_PP_SEQ_FOR_EACH(ENUM_ITEM, _, METRIC_SEQ)
    CUSTOM = 11
};
#undef ENUM_ITEM

#define TRANSFORM_OP_SEQ (ADD)(SUB)(MUL_SCALAR)(NORMALIZE)

enum class TransformOp : uint8 {
    BOOST_PP_SEQ_ENUM(TRANSFORM_OP_SEQ)
};

#define DIST_PRECISION_TYPE_SEQ \
    ((FLOAT, 4)) \
    ((INT8, 1))
/* HALF is intentionally absent from DIST_PRECISION_TYPE_SEQ so the dispatcher
 * macro-machinery skips instantiating HALF specializations. It is preserved
 * in the enum below for ABI/enum-stability of any persisted on-disk values. */
#define ENUM_ITEM(r, data, elem) BOOST_PP_TUPLE_ELEM(0, elem),
enum class DistPrecisionType : uint8 {
    BOOST_PP_SEQ_FOR_EACH(ENUM_ITEM, _, ((FLOAT, 4)) ((HALF, 2)) ((INT8, 1)))
    CUSTOM,
    /* sentinel: boost.PP single-element SEQ placeholder. Never appears at runtime. */
    _DISPATCH_PAD_ = 0xFE,
};
#undef ENUM_ITEM

#define ENUM_ITEM(r, data, elem) \
    case DistPrecisionType::BOOST_PP_TUPLE_ELEM(0, elem): \
        return BOOST_PP_TUPLE_ELEM(1, elem);
constexpr inline size_t get_dtype_size(DistPrecisionType dt)
{
    switch (dt) {
        BOOST_PP_SEQ_FOR_EACH(ENUM_ITEM, _, DIST_PRECISION_TYPE_SEQ)
        case DistPrecisionType::_DISPATCH_PAD_:
            return 4;
        default:
            __builtin_unreachable();
    }
}
#undef ENUM_ITEM

/**
 * NoPartial: dim % (k * k_per_iter) == 0
 * NoTail:    dim % k_per_iter == 0
 */
#define REMAINDER_SITUATION_SEQ (Unknown)(NoPartial)(NoTail)
enum class RemainderSituation {
    BOOST_PP_SEQ_ENUM(REMAINDER_SITUATION_SEQ)
};
#endif

template <RemainderSituation... Rs>
struct RemainderSituationList {};

template <Metric... Ms>
struct MetricList {};

template <TransformOp... Ops>
struct TransformOpList {};

template <DistPrecisionType... Ds>
struct DistPrecisionTypeList {};

template <Arch arch, DistPrecisionType dt>
struct RemainderPatcher {
    using res_cats = std::conditional_t<
#if COMPILER_TARGET_X86_64
        arch == Arch::GENERAL || (arch == Arch::SSE && dt == DistPrecisionType::HALF),
#else
        arch == Arch::GENERAL,
#endif
        RemainderSituationList<RemainderSituation::Unknown>,
        RemainderSituationList<RemainderSituation::NoPartial, RemainderSituation::NoTail, RemainderSituation::Unknown>>;
    static RemainderSituation get_remainder_situation(uint16 dim);
};

extern Arch get_best_arch(Metric m, DistPrecisionType dt, uint16 dim);

template <Arch ar, TransformOp op, DistPrecisionType d, RemainderSituation r, bool a>
struct Transformer {
    static void transform_single(const void *x, const void *y, void *out, uint16 dim);
    static constexpr void destroy() {}

    static constexpr Arch arch = ar;
    static constexpr TransformOp operation = op;
    static constexpr DistPrecisionType dpt = d;
    static constexpr RemainderSituation rs = r;
    static constexpr bool aligned = a;
};

template <Arch ar, Metric m, DistPrecisionType d, RemainderSituation r, bool a>
struct Distancer {
    static constexpr bool has_estimation_func = false;
    static constexpr bool need_refine = false;
    static constexpr void prepare(Relation index, void *meta) {}
    static constexpr void process(const char *query) {}
    static constexpr void compute_code(float *query, char *code) {}
    static constexpr void destroy() {}

    static constexpr Arch arch = ar;
    static constexpr Metric metric = m;
    static constexpr DistPrecisionType dpt = d;
    static constexpr RemainderSituation rs = r;
    static constexpr bool aligned = a;

    template <TransformOp op>
    using transform_type = Transformer<arch, op, dpt, rs, aligned>;

    /* Distance implementation - inline */
    static inline float get_distance_single(const void *x, const void *y, uint16 dim) {
        /* General (scalar) implementation for all types */
        if constexpr (d == DistPrecisionType::FLOAT) {
            const float *fx = (const float *)x;
            const float *fy = (const float *)y;
            switch (m) {
                case Metric::L2:
                case Metric::L2_SQRT: {
                    float sum = 0.0f;
                    for (uint16 i = 0; i < dim; i++) {
                        float diff = fx[i] - fy[i];
                        sum += diff * diff;
                    }
                    return sum;
                }
                case Metric::INNER_PRODUCT:
                case Metric::FAST_COSINE: {
                    float sum = 0.0f;
                    for (uint16 i = 0; i < dim; i++) {
                        sum += fx[i] * fy[i];
                    }
                    return -sum;
                }
                case Metric::COSINE: {
                    float dot = 0.0f, norm_x = 0.0f, norm_y = 0.0f;
                    for (uint16 i = 0; i < dim; i++) {
                        dot += fx[i] * fy[i];
                        norm_x += fx[i] * fx[i];
                        norm_y += fy[i] * fy[i];
                    }
                    if (norm_x == 0.0f || norm_y == 0.0f) return 2.0f;
                    return 1.0f - dot / sqrtf(norm_x * norm_y);
                }
                case Metric::SPHERICAL: {
                    float sum = 0.0f;
                    for (uint16 i = 0; i < dim; i++) {
                        sum += fx[i] * fy[i];
                    }
                    float dist = -sum;
                    if (dist > 1) dist = 1;
                    else if (dist < -1) dist = -1;
                    return acosf(dist) / (float)M_PI;
                }
                case Metric::L2_NORM: {
                    float sum = 0.0f;
                    for (uint16 i = 0; i < dim; i++) {
                        sum += fx[i] * fx[i];
                    }
                    return sqrtf(sum);
                }
                default:
                    return 0.0f;
            }
        } else if constexpr (d == DistPrecisionType::HALF) {
            /* Half precision - treat as float for now */
            const float *fx = (const float *)x;
            const float *fy = (const float *)y;
            switch (m) {
                case Metric::L2: {
                    float sum = 0.0f;
                    for (uint16 i = 0; i < dim; i++) {
                        float diff = fx[i] - fy[i];
                        sum += diff * diff;
                    }
                    return sum;
                }
                case Metric::INNER_PRODUCT:
                case Metric::FAST_COSINE: {
                    float sum = 0.0f;
                    for (uint16 i = 0; i < dim; i++) {
                        sum += fx[i] * fy[i];
                    }
                    return -sum;
                }
                case Metric::COSINE: {
                    float dot = 0.0f, norm_x = 0.0f, norm_y = 0.0f;
                    for (uint16 i = 0; i < dim; i++) {
                        dot += fx[i] * fy[i];
                        norm_x += fx[i] * fx[i];
                        norm_y += fy[i] * fy[i];
                    }
                    if (norm_x == 0.0f || norm_y == 0.0f) return 2.0f;
                    return 1.0f - dot / sqrtf(norm_x * norm_y);
                }
                default:
                    return 0.0f;
            }
        } else if constexpr (d == DistPrecisionType::INT8) {
            const int8 *ix = (const int8 *)x;
            const int8 *iy = (const int8 *)y;
            switch (m) {
                case Metric::L2: {
                    int32 sum = 0;
                    for (uint16 i = 0; i < dim; i++) {
                        int32 diff = (int32)ix[i] - (int32)iy[i];
                        sum += diff * diff;
                    }
                    return (float)sum;
                }
                case Metric::INNER_PRODUCT:
                case Metric::FAST_COSINE: {
                    int32 sum = 0;
                    for (uint16 i = 0; i < dim; i++) {
                        sum += (int32)ix[i] * (int32)iy[i];
                    }
                    return -(float)sum;
                }
                case Metric::COSINE: {
                    int32 dot = 0, norm_x = 0, norm_y = 0;
                    for (uint16 i = 0; i < dim; i++) {
                        int32 xv = (int32)ix[i];
                        int32 yv = (int32)iy[i];
                        dot += xv * yv;
                        norm_x += xv * xv;
                        norm_y += yv * yv;
                    }
                    if (norm_x == 0 || norm_y == 0) return 2.0f;
                    return 1.0f - (float)dot / sqrtf((float)norm_x * (float)norm_y);
                }
                default:
                    return 0.0f;
            }
        }
        return 0.0f;
    }

    static inline void get_distance_batch2(const void *x, void *const *y, uint16 dim, uint16 y_size, float *out) {
        for (uint16 i = 0; i < y_size; i++) {
            out[i] = get_distance_single(x, y[i], dim);
        }
    }
};

inline void *transform_scalar_to_ptr(float v)
{
    union {
        float f;
        uint32 u;
    } cvt;
    cvt.f = v;
    return reinterpret_cast<void *>((uintptr_t)cvt.u);
}

inline void transform_int8_to_int16(const int8 *src, int16 *dst, uint16 dim)
{
    for (uint16 i = dim; i > 0; --i) {
        uint16 j = i - 1;
        dst[j] = (int16)src[j];
    }
}

inline void transform_int16_to_int8(const int16 *src, int8 *dst, uint16 dim)
{
    for (uint16 i = 0; i < dim; ++i) {
        dst[i] = (int8)src[i];
    }
}

#if (defined(PG_VEXDB_TARGET_DUCK) || defined(PG_VEXDB_TARGET_SQLITE))
Metric get_func_metric(uint32 func_id);
#else
Metric get_func_metric(Oid func_id);
#endif

namespace ann_helper {

/* use dim as input can help improve dist calculation efficiency, I guess... */
distance_func get_general_distance_func(Metric metric, uint32 dim);
distance_func get_aligned_distance_func(Metric metric, uint32 dim);
distance_func get_general_distance_func(Metric metric);
distance_func_batch get_general_distance_batch_func2(Metric metric, uint32 dim);
distance_func_batch get_aligned_distance_batch_func2(Metric metric, uint32 dim);

/* int8 vector related */
distance_func get_general_int8_distance_func(Metric metric, uint32 dim);
distance_func get_aligned_int8_distance_func(Metric metric, uint32 dim);
distance_func get_general_int8_distance_func(Metric metric);
distance_func_batch get_general_int8_distance_batch_func2(Metric metric, uint32 dim);
distance_func_batch get_aligned_int8_distance_batch_func2(Metric metric, uint32 dim);

vector_preprocess_func get_vector_preprocess_func(Metric metric, DistPrecisionType type, uint16 dim);

fvec_ny_distance_func get_fvec_ny_distance_func(Metric metric);
fvec_L2sqr_ny_nearest_func get_fvec_L2sqr_ny_nearest_func();
distance_single_code_func get_distance_single_code_func(uint32 nbits);
distance_four_codes_func get_distance_four_codes_func(uint32 nbits);
fht_func get_fht_func(uint32 bottom_log_dim);
RabitqKernels get_rabitq_kernels();
void init_rabitq_func();

/* we don't use func pointer here since it is not trivial */
void pairwise_distance(const Metric metric, const float *x, const float *y, const float *x_norm,
    const float *y_norm, uint32 dim, uint32 x_size, uint32 y_size, float *out);
} /* namespace ann_helper */

uint32 get_aligned_dim(uint32 dim);
size_t get_aligned_vec_size(size_t vec_size);
float *alloc_floatvector(uint32 dim, size_t n = 1);
char *alloc_vector(size_t vec_size, size_t n = 1);
#if (defined(PG_VEXDB_TARGET_DUCK) || defined(PG_VEXDB_TARGET_SQLITE))
inline void free_vector(void *vec) { std::free(vec); }
#else
inline void free_vector(void *vec) {
    if (vec != nullptr) {
        pfree(vec);
    }
}
#endif
bool is_aligned(const void *ptr);

/* Random number generator for level assignment.
 *
 * DuckDB/SQLite build with threads, so the thread id gives each worker an
 * independent stream and avoids sharing mt19937 state. PostgreSQL build with
 * processes: every worker's only C++ thread can have the same thread-id hash.
 * Seeding those processes identically makes their level choices move in
 * lockstep, repeatedly forcing the entry lock into exclusive mode and
 * serializing an otherwise parallel disk build. Mix in MyProcPid for PG so
 * sibling workers retain the same geometric distribution but not the same
 * sequence.
 */
inline double RandomDouble() {
#if defined(PG_VEXDB_TARGET_PG)
    thread_local std::mt19937 rng(42u ^
        (static_cast<unsigned>(MyProcPid) * 0x9e3779b9u));
#else
    thread_local std::mt19937 rng(42u + static_cast<unsigned>(
        std::hash<std::thread::id>{}(std::this_thread::get_id())));
#endif
    thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng);
}

#endif /* DISKANN_UTILS_DISTANCE_H */
