#ifndef ANNVECTOR_DISTANCE_FUNC_H
#define ANNVECTOR_DISTANCE_FUNC_H

#include <cstddef>
#include <cstdint>
#include "half.h"

#if defined(PG_VEXDB_TARGET_PG)
#include "pg_compat.h"
#else
using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
#endif

namespace ann_helper {
typedef float (*distance_func)(const void *x, const void *y, uint16 dim);
typedef void (*distance_func_batch)(const void *x, void *const *y, uint16 dim, uint16 y_size, float *out);
typedef void (*vector_preprocess_func)(const void *x, uint16 dim, void *out);
typedef half (*float_to_half_func)(float num);
typedef float (*half_to_float_func)(half num);
/*pq related*/
typedef void (*fvec_ny_distance_func)(float *dis, const float *x, const float *y, uint32 d, uint32 ny);
typedef uint32 (*fvec_L2sqr_ny_nearest_func)(float *distances_tmp_buffer, const float *x, const float *y, uint32 d, uint32 ny);
typedef float (*distance_single_code_func)(uint32 M, uint32 nbits, const float *sim_table, const uint8 *code);
typedef void (*distance_four_codes_func)(uint32 M, uint32 nbits, const float *sim_table,
    const uint8 *__restrict code0, const uint8 *__restrict code1,
    const uint8 *__restrict code2, const uint8 *__restrict code3,
    float &result0, float &result1, float &result2, float &result3);
typedef void (*fht_func)(float *buf);
typedef void (*flip_sign_func)(const uint8 *flip, float *data, size_t dim);
typedef void (*kacs_walk_func)(float *data, size_t len);
typedef float (*warmup_ip_x0_q_func)(uint64 *data, const uint64 *query, float delta, float vl, size_t dim);
typedef float (*ip_fxi_func)(float *query, uint8 *data, size_t dim);
typedef float (*mask_ip_x0_q_func)(float *query, uint64 *data, size_t dim);
} /* namespace ann_helper */

#endif /* ANNVECTOR_DISTANCE_FUNC_H */
