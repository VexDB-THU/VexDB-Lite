#ifndef FLOATVECTOR_H
#define FLOATVECTOR_H

#include <cstdio>
#include "fmgr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FLOATVECTOR_MAX_DIM 16384

struct FloatVector {
    int32 vl_len_;  /* varlena header (do not touch directly!) */
    int16 dim;      /* number of dimensions */
    int16 unused;   /* reserved for future use, always zero */
    float4 x[FLEXIBLE_ARRAY_MEMBER];
};

#define FLOATVECTOR_SIZE(_dim)      (offsetof(FloatVector, x) + sizeof(float)*(_dim))
#define DatumGetFloatVector(x)      ((FloatVector *) PG_DETOAST_DATUM(x))
#define PG_GETARG_FLOATVECTOR_P(x)  DatumGetFloatVector(PG_GETARG_DATUM(x))
#define PG_RETURN_FLOATVECTOR_P(x)  PG_RETURN_POINTER(x)

struct FloatVectorArrayData {
    int length;
    int maxlen;
    int dim;
    float *items;
};

typedef FloatVectorArrayData *FloatVectorArray;

#define FLOATVECTOR_COMPACT_SIZE(_dim) sizeof(float) * (_dim)
#define FLOATVECTOR_ARRAY_SIZE(_length, _dim) (sizeof(FloatVectorArrayData) + (_length) * (_dim * sizeof(float)))
#define FLOATVECTOR_ARRAY_OFFSET(_arr, _offset) ((char *) (_arr)->items + (_offset) * (_arr)->dim * sizeof(float))
#define FloatVectorArrayGet(_arr, _offset) ((float *) FLOATVECTOR_ARRAY_OFFSET(_arr, _offset))
#define FloatVectorArraySet(_arr, _offset, _val) do { \
    memcpy(FLOATVECTOR_ARRAY_OFFSET(_arr, _offset), _val, ((_arr)->dim * sizeof(float))); \
} while (0)

#define FloatVectorSet(_destVector, _srcVector) do { \
    memcpy((char*)_destVector, (char*)_srcVector, FLOATVECTOR_SIZE((_srcVector)->dim)); \
} while (0)

extern FloatVectorArray FloatVectorArrayInit(int maxlen, int dimensions);
extern void FloatVectorArrayFree(FloatVectorArray arr);
extern FloatVector *InitFloatVector(int dim);

/*   floatvector related functions   */
extern Datum floatvector_to_floatvector(PG_FUNCTION_ARGS);
extern Datum floatvector_in(PG_FUNCTION_ARGS);
extern Datum input_floatvector_in(char* str, Oid typioparam, int32 atttypmod);
extern Datum floatvector_out(PG_FUNCTION_ARGS);
extern Datum floatvector_typmod_in(PG_FUNCTION_ARGS);
extern Datum floatvector_typmod_out(PG_FUNCTION_ARGS);
extern Datum floatvector_recv(PG_FUNCTION_ARGS);
extern Datum floatvector_send(PG_FUNCTION_ARGS);
extern Datum array_to_floatvector(PG_FUNCTION_ARGS);
extern Datum floatvector_to_float4(PG_FUNCTION_ARGS);
extern Datum l2_distance(PG_FUNCTION_ARGS);
extern Datum floatvector_l2_squared_distance(PG_FUNCTION_ARGS);
extern Datum inner_product(PG_FUNCTION_ARGS);
extern Datum floatvector_negative_inner_product(PG_FUNCTION_ARGS);
extern Datum cosine_distance(PG_FUNCTION_ARGS);
extern Datum floatvector_spherical_distance(PG_FUNCTION_ARGS);
extern Datum floatvector_dims(PG_FUNCTION_ARGS);
extern Datum floatvector_norm(PG_FUNCTION_ARGS);
extern Datum l2_normalize(PG_FUNCTION_ARGS);
extern void floatvector_normalize(float *ax, int16 dim);
extern Datum floatvector_add(PG_FUNCTION_ARGS);
extern Datum floatvector_sub(PG_FUNCTION_ARGS);
extern void floatvector_sub_inplace(float *a, float *b, int dim);
extern Datum floatvector_lt(PG_FUNCTION_ARGS);
extern Datum floatvector_le(PG_FUNCTION_ARGS);
extern Datum floatvector_eq(PG_FUNCTION_ARGS);
extern Datum floatvector_ne(PG_FUNCTION_ARGS);
extern Datum floatvector_ge(PG_FUNCTION_ARGS);
extern Datum floatvector_gt(PG_FUNCTION_ARGS);
extern Datum floatvector_cmp(PG_FUNCTION_ARGS);
extern Datum floatvector_sortsupport(PG_FUNCTION_ARGS);
extern Datum hashfloatvector(PG_FUNCTION_ARGS);
extern Datum floatvector_accum(PG_FUNCTION_ARGS);
extern Datum floatvector_combine(PG_FUNCTION_ARGS);
extern Datum floatvector_avg(PG_FUNCTION_ARGS);
extern Datum subfloatvector(PG_FUNCTION_ARGS);

#ifdef __cplusplus
}
#endif

#endif /* FLOATVECTOR_H */
