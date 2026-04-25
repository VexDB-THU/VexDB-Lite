#ifndef HALFVEC_H
#define HALFVEC_H

#define __STDC_WANT_IEC_60559_TYPES_EXT__

#include "fmgr.h"
#include "half.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HALFVEC_MAX_DIM 16384

#define HALFVEC_SIZE(_dim)     (offsetof(HalfVector, x) + sizeof(half) * (_dim))
#define DatumGetHalfVector(x)  ((HalfVector *) PG_DETOAST_DATUM(x))
#define PG_GETARG_HALFVEC_P(x) DatumGetHalfVector(PG_GETARG_DATUM(x))
#define PG_RETURN_HALFVEC_P(x) PG_RETURN_POINTER(x)

struct HalfVector {
    int32 vl_len_;  /* varlena header (do not touch directly!) */
    int16 dim;      /* number of dimensions */
    int16 unused;   /* reserved for future use, always zero */
    half x[FLEXIBLE_ARRAY_MEMBER];
};

extern HalfVector *InitHalfVector(int dim);

extern Datum halfvector_to_halfvector(PG_FUNCTION_ARGS);
extern Datum halfvector_in(PG_FUNCTION_ARGS);
extern Datum halfvector_out(PG_FUNCTION_ARGS);
extern Datum halfvector_typmod_in(PG_FUNCTION_ARGS);
extern Datum halfvector_typmod_out(PG_FUNCTION_ARGS);
extern Datum halfvector_recv(PG_FUNCTION_ARGS);
extern Datum halfvector_send(PG_FUNCTION_ARGS);
extern Datum array_to_halfvector(PG_FUNCTION_ARGS);
extern Datum halfvector_to_float4(PG_FUNCTION_ARGS);
extern Datum halfvector_l2_distance(PG_FUNCTION_ARGS);
extern Datum halfvector_l2_squared_distance(PG_FUNCTION_ARGS);
extern Datum halfvector_inner_product(PG_FUNCTION_ARGS);
extern Datum halfvector_negative_inner_product(PG_FUNCTION_ARGS);
extern Datum halfvector_cosine_distance(PG_FUNCTION_ARGS);
extern Datum halfvector_spherical_distance(PG_FUNCTION_ARGS);
extern Datum halfvector_dims(PG_FUNCTION_ARGS);
extern Datum halfvector_l2_norm(PG_FUNCTION_ARGS);
extern Datum halfvector_l2_normalize(PG_FUNCTION_ARGS);
extern Datum halfvector_add(PG_FUNCTION_ARGS);
extern Datum halfvector_sub(PG_FUNCTION_ARGS);
extern Datum halfvector_lt(PG_FUNCTION_ARGS);
extern Datum halfvector_le(PG_FUNCTION_ARGS);
extern Datum halfvector_eq(PG_FUNCTION_ARGS);
extern Datum halfvector_ne(PG_FUNCTION_ARGS);
extern Datum halfvector_ge(PG_FUNCTION_ARGS);
extern Datum halfvector_gt(PG_FUNCTION_ARGS);
extern Datum halfvector_cmp(PG_FUNCTION_ARGS);
extern Datum halfvector_sortsupport(PG_FUNCTION_ARGS);
extern Datum hashhalfvector(PG_FUNCTION_ARGS);
extern Datum halfvector_accum(PG_FUNCTION_ARGS);
extern Datum halfvector_avg(PG_FUNCTION_ARGS);
extern Datum halfvector_subvector(PG_FUNCTION_ARGS);
extern Datum floatvector_to_halfvector(PG_FUNCTION_ARGS);
extern Datum halfvector_to_floatvector(PG_FUNCTION_ARGS);

extern void halfs_to_floats(half *h, float *f, uint32 dim);

#ifdef __cplusplus
}
#endif

#endif /* HALFVEC_H */
