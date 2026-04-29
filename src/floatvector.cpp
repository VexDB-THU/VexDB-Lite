/* -------------------------------------------------------------------------
 *
 * floatvecor.c
 *      Functions for the built-in type "floatvector".
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *      src/gausskernel/storage/access/annvector/floatvector.cpp
 *
 * -------------------------------------------------------------------------
 */
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <float.h>
#include <algorithm> 

#include "pg_compat.h"
#include "floatvector.h"
#include "vec_common.h"
#include "knl/knl_variable.h"

#define L2_SQUARED_DIST g_instance.annvec_cxt.l2_squared_distance
#define NEGATIVE_INNER_PRODUCT_DIST g_instance.annvec_cxt.negative_inner_product
#define COSINE_DIST g_instance.annvec_cxt.cosine_distance

static bool floatvector_isspace(char ch)
    { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f'; }

static void CheckElement(float value)
{
    if (isnan(value)) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
            errmsg("NaN not allowed in vector")));
    }

    if (isinf(value)) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
            errmsg("infinite value not allowed in vector")));
    }
}

static void CheckExpectedDim(int32 typmod, int dim)
{
    if (typmod != -1 && typmod != dim) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
            errmsg("expected %d dimensions, not %d", typmod, dim)));
    }
}

static FloatVector* floatvector_input(const char* s, int32 atttypmod)
{
    char *lit = pstrdup(s);
    char *str = lit;
    while (floatvector_isspace(*str)) {
        ++str;
    }

    if (*str != '[') {
        ereport(ERROR, (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
            errmsg("malformed floatvector literal: \"%s\"", s),
            errdetail("FloatVector contents must start with \"[\".")));
    }

    ++str;
    float x[FLOATVECTOR_MAX_DIM];
    char *psave = NULL;
    char *pt = strtok_r(str, ",", &psave);
    char *stringEnd = pt;
    int dim = 0;
    while (pt != NULL && *stringEnd != ']') {
        if (dim == FLOATVECTOR_MAX_DIM) {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("floatvector cannot have more than %d dimensions", FLOATVECTOR_MAX_DIM)));
        }

        while (floatvector_isspace(*pt)) {
            ++pt;
        }

        /* Check for empty string like float4in */
        if (*pt == '\0') {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid input syntax for type floatvector: \"%s\"", s)));
        }

        /* Use strtof like float4in to avoid a double-rounding problem */
        x[dim] = strtof(pt, &stringEnd);
        CheckElement(x[dim]);
        ++dim;

        if (stringEnd == pt) {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid input syntax for type floatvector: \"%s\"", s)));
        }

        while (floatvector_isspace(*stringEnd)) {
            ++stringEnd;
        }

        if (*stringEnd != '\0' && *stringEnd != ']') {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid input syntax for type floatvector: \"%s\"", s)));
        }

        pt = strtok_r(NULL, ",", &psave);
    }

    if (stringEnd == NULL || *stringEnd != ']') {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("malformed floatvector literal: \"%s\"", s),
                 errdetail("Unexpected end of input.")));
    }

    ++stringEnd;

    /* Only whitespace is allowed after the closing brace */
    while (floatvector_isspace(*stringEnd)) {
        ++stringEnd;
    }

    if (*stringEnd != '\0') {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("malformed floatvector literal: \"%s\"", s),
                 errdetail("Junk after closing right brace.")));
    }

    /* Ensure no consecutive delimiters since strtok skips */
    for (pt = lit + 1; *pt != '\0'; ++pt) {
        if (pt[-1] == ',' && *pt == ',') {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("malformed floatvector literal: \"%s\"", s)));
        }
    }

    if (dim < 1) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("floatvector must have at least 1 dimension")));
    }

    pfree(lit);

    CheckExpectedDim(atttypmod, dim);
    FloatVector *result = InitFloatVector(dim);
    memcpy(result->x, x, dim * sizeof(float));
    return result;
}

static void CheckDims(FloatVector * a, FloatVector * b)
{
    if (a->dim != b->dim) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
            errmsg("different floatvector dimensions %d and %d", a->dim, b->dim)));
    }
}

static void CheckDim(int dim)
{
    if (dim < 1) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
            errmsg("vector must have at least 1 dimension")));
    }
    if (dim > FLOATVECTOR_MAX_DIM) {
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
            errmsg("vector cannot have more than %d dimensions", FLOATVECTOR_MAX_DIM)));
    }
}

FloatVector *InitFloatVector(int dim)
{
    int size = FLOATVECTOR_SIZE(dim);
    FloatVector *result = (FloatVector *) palloc0(size);
    SET_VARSIZE(result, size);
    result->dim = dim;
    return result;
}

static float8 *CheckStateArray(ArrayType *statearray, const char *caller)
{
    if (ARR_NDIM(statearray) != 1 || ARR_DIMS(statearray)[0] < 1 ||
        ARR_HASNULL(statearray) || ARR_ELEMTYPE(statearray) != FLOAT8OID) {
        elog(ERROR, "%s: expected state array", caller);
    }
    return (float8 *)ARR_DATA_PTR(statearray);
}

extern "C" {

/*
 * Convert textual representation to internal representation
 */
PG_FUNCTION_INFO_V1(floatvector_in);
Datum floatvector_in(PG_FUNCTION_ARGS)
{
    char *s = PG_GETARG_CSTRING(0);
    int32 atttypmod = PG_GETARG_INT32(2);
    FloatVector *result = floatvector_input(s, atttypmod);
    PG_RETURN_FLOATVECTOR_P(result);
}

Datum input_floatvector_in(char* str, Oid typioparam, int32 atttypmod)
{
    if (str == NULL) {
        return (Datum)0;
    }
    FloatVector *result = floatvector_input(str, atttypmod);
    PG_RETURN_FLOATVECTOR_P(result);
}

/*
 * Convert internal representation to textual representation
 */
PG_FUNCTION_INFO_V1(floatvector_out);
Datum floatvector_out(PG_FUNCTION_ARGS)
{
    FloatVector *floatvector = PG_GETARG_FLOATVECTOR_P(0);
    int dim = floatvector->dim;

    /*
     * Need:
     *
     * dim * (FLOAT_SHORTEST_DECIMAL_LEN - 1) bytes for
     * float_to_shortest_decimal_bufn
     *
     * dim - 1 bytes for separator
     *
     * 3 bytes for [, ], and \0
     */
    char *buf = (char *)palloc(FLOAT_SHORTEST_DECIMAL_LEN * dim + 2);
    char *ptr = buf;

    AppendChar(ptr, '[');
    for (int i = 0; i < dim; ++i) {
        if (i > 0) {
            AppendChar(ptr, ',');
        }
        AppendFloat(ptr, floatvector->x[i]);
    }
    AppendChar(ptr, ']');
    *ptr = '\0';

    PG_FREE_IF_COPY(floatvector, 0);
    PG_RETURN_CSTRING(buf);
}

/*
 * Convert type modifier
 */
PG_FUNCTION_INFO_V1(floatvector_typmod_in);
Datum floatvector_typmod_in(PG_FUNCTION_ARGS)
{
    ArrayType *ta = PG_GETARG_ARRAYTYPE_P(0);
    int n;
    int32 *tl = ArrayGetIntegerTypmods(ta, &n);

    if (n != 1) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("invalid type modifier")));
    }
    if (*tl < 1) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("dimensions for type floatvector must be at least 1")));
    }
    if (*tl > FLOATVECTOR_MAX_DIM) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("dimensions for type floatvector cannot exceed %d", FLOATVECTOR_MAX_DIM)));
    }
    PG_RETURN_INT32(*tl);
}

PG_FUNCTION_INFO_V1(floatvector_typmod_out);
Datum floatvector_typmod_out(PG_FUNCTION_ARGS)
{
    int32 typmod = PG_GETARG_INT32(0);
    if (typmod < 0) {
        PG_RETURN_CSTRING(pstrdup(""));
    }

    /* Convert typmod to string */
    char *result = psprintf("(%d)", typmod);
    PG_RETURN_CSTRING(result);
}

/*
 * Convert external binary representation to internal representation
 */
PG_FUNCTION_INFO_V1(floatvector_recv);
Datum floatvector_recv(PG_FUNCTION_ARGS)
{
    StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
    int32 typmod = PG_GETARG_INT32(2);

    int16 dim = pq_getmsgint(buf, sizeof(int16));
    int16 unused = pq_getmsgint(buf, sizeof(int16));

    CheckDim(dim);
    CheckExpectedDim(typmod, dim);

    if (unused != 0) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
            errmsg("expected unused to be 0, not %d", unused)));
    }

    FloatVector *result = InitFloatVector(dim);
    for (int16 i = 0; i < dim; ++i) {
        result->x[i] = pq_getmsgfloat4(buf);
    }
    for (int16 i = 0; i < dim; ++i) {
        CheckElement(result->x[i]);
    }
    PG_RETURN_POINTER(result);
}

/*
 * Convert internal representation to the external binary representation
 */
PG_FUNCTION_INFO_V1(floatvector_send);
Datum floatvector_send(PG_FUNCTION_ARGS)
{
    FloatVector *vec = PG_GETARG_FLOATVECTOR_P(0);
    StringInfoData buf;
    pq_begintypsend(&buf);
    pq_sendint(&buf, vec->dim, sizeof(int16));
    pq_sendint(&buf, vec->unused, sizeof(int16));
    for (int16 i = 0; i < vec->dim; ++i) {
        pq_sendfloat4(&buf, vec->x[i]);
    }
    PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * Convert vector to vector
 * This is needed to check the type modifier
 */
PG_FUNCTION_INFO_V1(floatvector_to_floatvector);
Datum floatvector_to_floatvector(PG_FUNCTION_ARGS)
{
    FloatVector *vec = PG_GETARG_FLOATVECTOR_P(0);
    int32 typmod = PG_GETARG_INT32(1);
    CheckExpectedDim(typmod, vec->dim);
    PG_RETURN_POINTER(vec);
}


/*
 * Convert array to floatvector
 */
PG_FUNCTION_INFO_V1(array_to_floatvector);
Datum array_to_floatvector(PG_FUNCTION_ARGS)
{
    ArrayType *array = PG_GETARG_ARRAYTYPE_P(0);
    int32 typmod = PG_GETARG_INT32(1);

    if (ARR_NDIM(array) > 1) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
            errmsg("array must be 1-D")));
    }

    if (ARR_HASNULL(array) && array_contains_nulls(array)) {
        ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
            errmsg("array must not contain nulls")));
    }

    bool typbyval;
    char typalign;
    int16 typlen;
    int nelemsp;
    Datum *elemsp;
    get_typlenbyvalalign(ARR_ELEMTYPE(array), &typlen, &typbyval, &typalign);
    deconstruct_array(array, ARR_ELEMTYPE(array), typlen, typbyval, typalign, &elemsp, NULL, &nelemsp);

    CheckDim(nelemsp);
    CheckExpectedDim(typmod, nelemsp);

    FloatVector *result = InitFloatVector(nelemsp);
    if (ARR_ELEMTYPE(array) == INT4OID) {
        for (int i = 0; i < nelemsp; ++i) {
            result->x[i] = DatumGetInt32(elemsp[i]);
        }
    } else if (ARR_ELEMTYPE(array) == FLOAT8OID) {
        for (int i = 0; i < nelemsp; ++i) {
            result->x[i] = DatumGetFloat8(elemsp[i]);
        }
    } else if (ARR_ELEMTYPE(array) == FLOAT4OID) {
        for (int i = 0; i < nelemsp; ++i)
            result->x[i] = DatumGetFloat4(elemsp[i]);
    } else if (ARR_ELEMTYPE(array) == NUMERICOID) {
        for (int i = 0; i < nelemsp; ++i) {
            result->x[i] = DatumGetFloat4(OidFunctionCall1(FLOAT4OID, elemsp[i]));
        }
    } else {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
            errmsg("unsupported array type")));
    }

    /*
     * Free allocation from deconstruct_array. Do not free individual elements
     * when pass-by-reference since they point to original array.
     */
    pfree(elemsp);

    /* Check elements */
    for (int i = 0; i < result->dim; ++i) {
        CheckElement(result->x[i]);
    }
    PG_FREE_IF_COPY(array, 0);
    PG_RETURN_POINTER(result);
}

/*
 * Convert floatvector to float4[]
 */
PG_FUNCTION_INFO_V1(floatvector_to_float4);
Datum floatvector_to_float4(PG_FUNCTION_ARGS)
{
    FloatVector *vec = PG_GETARG_FLOATVECTOR_P(0);
    Datum *datums = (Datum *)palloc(sizeof(Datum) * vec->dim);

    for (int16 i = 0; i < vec->dim; ++i) {
        datums[i] = Float4GetDatum(vec->x[i]);
    }

    /* Use TYPALIGN_INT for float4 */
    ArrayType *result = construct_array(datums, vec->dim, FLOAT4OID, sizeof(float4), true, TYPALIGN_INT);

    pfree(datums);

    PG_RETURN_POINTER(result);
}

/*
 * Get the L2 distance between floatvectors
 */
PG_FUNCTION_INFO_V1(l2_distance);
Datum l2_distance(PG_FUNCTION_ARGS)
{
    FloatVector *a = PG_GETARG_FLOATVECTOR_P(0);
    FloatVector *b = PG_GETARG_FLOATVECTOR_P(1);
    CheckDims(a, b);
    double distance = L2_SQUARED_DIST(a->x, b->x, a->dim);
    PG_RETURN_FLOAT8(sqrt(distance));
}

/*
 * Get the L2 squared distance between floatvectors
 * This saves a sqrt calculation
 */
PG_FUNCTION_INFO_V1(floatvector_l2_squared_distance);
Datum floatvector_l2_squared_distance(PG_FUNCTION_ARGS)
{
    FloatVector *a = PG_GETARG_FLOATVECTOR_P(0);
    FloatVector *b = PG_GETARG_FLOATVECTOR_P(1);
    CheckDims(a, b);
    double distance = L2_SQUARED_DIST(a->x, b->x, a->dim);
    PG_RETURN_FLOAT8(distance);
}

/*
 * Get the inner product of two floatvectors
 */
PG_FUNCTION_INFO_V1(inner_product);
Datum inner_product(PG_FUNCTION_ARGS)
{
    FloatVector *a = PG_GETARG_FLOATVECTOR_P(0);
    FloatVector *b = PG_GETARG_FLOATVECTOR_P(1);
    CheckDims(a, b);
    double distance = NEGATIVE_INNER_PRODUCT_DIST(a->x, b->x, a->dim);
    PG_RETURN_FLOAT8(-distance);
}

/*
 * Get the negative inner product of two floatvectors
 */
PG_FUNCTION_INFO_V1(floatvector_negative_inner_product);
Datum floatvector_negative_inner_product(PG_FUNCTION_ARGS)
{
    FloatVector *a = PG_GETARG_FLOATVECTOR_P(0);
    FloatVector *b = PG_GETARG_FLOATVECTOR_P(1);
    CheckDims(a, b);
    double distance = NEGATIVE_INNER_PRODUCT_DIST(a->x, b->x, a->dim);
    PG_RETURN_FLOAT8(abs(distance) == 0.0 ? 0 : distance);
}

/*
 * Get the cosine distance between two floatvectors
 */
PG_FUNCTION_INFO_V1(cosine_distance);
Datum cosine_distance(PG_FUNCTION_ARGS)
{
    FloatVector *a = PG_GETARG_FLOATVECTOR_P(0);
    FloatVector *b = PG_GETARG_FLOATVECTOR_P(1);
    CheckDims(a, b);
    double distance = COSINE_DIST(a->x, b->x, a->dim);
    /* Use sqrt(a * b) over sqrt(a) * sqrt(b) */
    PG_RETURN_FLOAT8(1 + distance);
}

/*
 * Get the distance for spherical k-means
 * Currently uses angular distance since needs to satisfy triangle inequality
 * Assumes inputs are unit vectors (skips norm)
 */
PG_FUNCTION_INFO_V1(floatvector_spherical_distance);
Datum floatvector_spherical_distance(PG_FUNCTION_ARGS)
{
    FloatVector *a = PG_GETARG_FLOATVECTOR_P(0);
    FloatVector *b = PG_GETARG_FLOATVECTOR_P(1);
    CheckDims(a, b);
    double distance = -NEGATIVE_INNER_PRODUCT_DIST(a->x, b->x, a->dim);

    /* Prevent NaN with acos with loss of precision */
    if (distance > 1) {
        distance = 1;
    } else if (distance < -1) {
        distance = -1;
    }
    PG_RETURN_FLOAT8(acos(distance) / M_PI);
}

/*
 * Get the dimensions of a floatvector
 */
PG_FUNCTION_INFO_V1(floatvector_dims);
Datum floatvector_dims(PG_FUNCTION_ARGS)
{
    FloatVector *a = PG_GETARG_FLOATVECTOR_P(0);
    PG_RETURN_INT32(a->dim);
}

/*
 * Get the L2 norm of a floatvector
 */
PG_FUNCTION_INFO_V1(floatvector_norm);
Datum floatvector_norm(PG_FUNCTION_ARGS)
{
    FloatVector *a = PG_GETARG_FLOATVECTOR_P(0);
    const float *ax = a->x;
    double norm = -NEGATIVE_INNER_PRODUCT_DIST(ax, ax, a->dim);
    PG_RETURN_FLOAT8(sqrt(norm));
}

PG_FUNCTION_INFO_V1(l2_normalize);
Datum l2_normalize(PG_FUNCTION_ARGS)
{
    FloatVector *a = PG_GETARG_FLOATVECTOR_P(0);
    const float *ax = a->x;
    double norm = -NEGATIVE_INNER_PRODUCT_DIST(ax, ax, a->dim);
    norm = sqrt(norm);

    FloatVector *result = InitFloatVector(a->dim);
    if (norm <= 0) { /* Return zero vector for zero norm */
        PG_RETURN_POINTER(result);
    }

    float *rx = result->x;
    if (norm == 1) {
        memcpy(rx, ax, a->dim * sizeof(float));
        PG_RETURN_POINTER(result);
    }

    for (int16 i = 0; i < a->dim; ++i) {
        rx[i] = ax[i] / norm;
    }
    for (int16 i = 0; i < a->dim; ++i) {
        if (isinf(rx[i])) {
            float_overflow_error();
        }
    }
    PG_RETURN_POINTER(result);
}

void floatvector_normalize(float *ax, int16 dim)
{
    double norm = 0;
    for (int16 i = 0; i < dim; ++i) {
        norm += ax[i] * ax[i];
    }
    norm = sqrt(norm);
    if (norm > 0 && norm != 1) {
        for (int16 i = 0; i < dim; ++i) {
            ax[i] /= norm;
        }
        for (int16 i = 0; i < dim; ++i) {
            if (isinf(ax[i])) {
                float_overflow_error();
            }
        }
    }
}

PG_FUNCTION_INFO_V1(floatvector_add);
Datum floatvector_add(PG_FUNCTION_ARGS)
{
    FloatVector *a = PG_GETARG_FLOATVECTOR_P(0);
    FloatVector *b = PG_GETARG_FLOATVECTOR_P(1);
    CheckDims(a, b);
    const float *ax = a->x;
    const float *bx = b->x;
    FloatVector *result = InitFloatVector(a->dim);
    float *rx = result->x;
    for (int16 i = 0, imax = a->dim; i < imax; ++i) {
        rx[i] = ax[i] + bx[i];
    }
    for (int16 i = 0, imax = a->dim; i < imax; ++i) {
        if (isinf(rx[i])) {
            float_overflow_error();
        }
    }

    PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(floatvector_sub);
Datum floatvector_sub(PG_FUNCTION_ARGS)
{
    FloatVector *a = PG_GETARG_FLOATVECTOR_P(0);
    FloatVector *b = PG_GETARG_FLOATVECTOR_P(1);
    CheckDims(a, b);
    const float *ax = a->x;
    const float *bx = b->x;
    FloatVector *result = InitFloatVector(a->dim);
    float *rx = result->x;
    for (int16 i = 0, imax = a->dim; i < imax; ++i) {
        rx[i] = ax[i] - bx[i];
    }
    for (int16 i = 0, imax = a->dim; i < imax; ++i) {
        if (isinf(rx[i])) {
            float_overflow_error();
        }
    }

    PG_RETURN_POINTER(result);
}

/*
 * Subtract floatvectors without allocate new floatvector
 * the result is stored in first parameter `a`
 */
void floatvector_sub_inplace(float *ax, float *bx, int dim)
{   
    for (int i = 0, imax = dim; i < imax; ++i) {
        ax[i] = ax[i] - bx[i];
    }
}

/*
 * Internal helper to compare floatvectors
 */
static int floatvector_cmp_internal(FloatVector *a, FloatVector *b)
{
    int	dim = Min(a->dim, b->dim);
    for (int16 i = 0, imax = dim; i < imax; ++i) {
        if (a->x[i] < b->x[i]) {
            return -1;
        }
        if (a->x[i] > b->x[i]) {
            return 1;
        }
    }
    
    if (a->dim < b->dim) {
        return -1;
    }

    if (a->dim > b->dim) {
        return 1;
    }

    return 0;
}

PG_FUNCTION_INFO_V1(floatvector_lt);
Datum floatvector_lt(PG_FUNCTION_ARGS)
{
    FloatVector *a = (FloatVector *)PG_GETARG_FLOATVECTOR_P(0);
    FloatVector *b = (FloatVector *)PG_GETARG_FLOATVECTOR_P(1);
    PG_RETURN_BOOL(floatvector_cmp_internal(a, b) < 0);
}

PG_FUNCTION_INFO_V1(floatvector_le);
Datum floatvector_le(PG_FUNCTION_ARGS)
{
    FloatVector *a = (FloatVector *)PG_GETARG_FLOATVECTOR_P(0);
    FloatVector *b = (FloatVector *)PG_GETARG_FLOATVECTOR_P(1);
    PG_RETURN_BOOL(floatvector_cmp_internal(a, b) <= 0);
}

PG_FUNCTION_INFO_V1(floatvector_eq);
Datum floatvector_eq(PG_FUNCTION_ARGS)
{
    FloatVector *a = (FloatVector *)PG_GETARG_FLOATVECTOR_P(0);
    FloatVector *b = (FloatVector *)PG_GETARG_FLOATVECTOR_P(1);
    bool res = floatvector_cmp_internal(a, b) == 0;
    PG_FREE_IF_COPY(a, 0);
    PG_FREE_IF_COPY(b, 1);
    PG_RETURN_BOOL(res);
}

PG_FUNCTION_INFO_V1(floatvector_ne);
Datum floatvector_ne(PG_FUNCTION_ARGS)
{
    FloatVector *a = (FloatVector *)PG_GETARG_FLOATVECTOR_P(0);
    FloatVector *b = (FloatVector *)PG_GETARG_FLOATVECTOR_P(1);
    bool res = floatvector_cmp_internal(a, b) != 0;
    PG_FREE_IF_COPY(a, 0);
    PG_FREE_IF_COPY(b, 1);
    PG_RETURN_BOOL(res);
}

PG_FUNCTION_INFO_V1(floatvector_ge);
Datum floatvector_ge(PG_FUNCTION_ARGS)
{
    FloatVector *a = (FloatVector *)PG_GETARG_FLOATVECTOR_P(0);
    FloatVector *b = (FloatVector *)PG_GETARG_FLOATVECTOR_P(1);
    PG_RETURN_BOOL(floatvector_cmp_internal(a, b) >= 0);
}

PG_FUNCTION_INFO_V1(floatvector_gt);
Datum floatvector_gt(PG_FUNCTION_ARGS)
{
    FloatVector *a = (FloatVector *)PG_GETARG_FLOATVECTOR_P(0);
    FloatVector *b = (FloatVector *)PG_GETARG_FLOATVECTOR_P(1);
    PG_RETURN_BOOL(floatvector_cmp_internal(a, b) > 0);
}

PG_FUNCTION_INFO_V1(floatvector_cmp);
Datum floatvector_cmp(PG_FUNCTION_ARGS)
{
    FloatVector *a = (FloatVector *)PG_GETARG_FLOATVECTOR_P(0);
    FloatVector *b = (FloatVector *)PG_GETARG_FLOATVECTOR_P(1);
    PG_RETURN_INT32(floatvector_cmp_internal(a, b));
}

static int floatvector_fastcmp(Datum x, Datum y, SortSupport)
{
    FloatVector *a = (FloatVector *)DatumGetFloatVector(x);
    FloatVector *b = (FloatVector *)DatumGetFloatVector(y);
    return floatvector_cmp_internal(a, b);
}

PG_FUNCTION_INFO_V1(floatvector_sortsupport);
Datum floatvector_sortsupport(PG_FUNCTION_ARGS)
{
    SortSupport ssup = (SortSupport)PG_GETARG_POINTER(0);
    ssup->comparator = floatvector_fastcmp;
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(hashfloatvector);
Datum hashfloatvector(PG_FUNCTION_ARGS)
{
    FloatVector *a = (FloatVector *)PG_GETARG_FLOATVECTOR_P(0);
    return hash_any((unsigned char *)a->x, sizeof(float) * a->dim);
}

PG_FUNCTION_INFO_V1(floatvector_accum);
Datum floatvector_accum(PG_FUNCTION_ARGS)
{
    ArrayType *statearray = PG_GETARG_ARRAYTYPE_P(0);
    FloatVector *newval = PG_GETARG_FLOATVECTOR_P(1);

    /* Check array before using */
    float8 *statevalues = CheckStateArray(statearray, "floatvector_accum");
    int16 dim = STATE_DIMS(statearray);
    bool newarr = dim == 0;

    if (newarr) {
        dim = newval->dim;
    } else {
        CheckExpectedDim(dim, newval->dim);
    }

    float8 n = statevalues[0] + 1.0;
    Datum *statedatums = (Datum *)CreateStateDatums(dim);
    statedatums[0] = Float8GetDatum(n);

    const float *x = newval->x;
    if (newarr) {
        for (int16 i = 0; i < dim; ++i) {
            statedatums[i + 1] = Float8GetDatum((double)x[i]);
        }
    } else {
        for (int16 i = 0; i < dim; ++i) {
            double v = statevalues[i + 1] + x[i];
            if (isinf(v)) {
                float_overflow_error();
            }
            statedatums[i + 1] = Float8GetDatum(v);
        }
    }

    /* Use float8 array like float4_accum */
    ArrayType *result = construct_array(statedatums, dim + 1, FLOAT8OID, sizeof(float8),
        FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);

    pfree(statedatums);

    PG_RETURN_ARRAYTYPE_P(result);
}

PG_FUNCTION_INFO_V1(floatvector_combine);
Datum floatvector_combine(PG_FUNCTION_ARGS)
{
    ArrayType *statearray1 = PG_GETARG_ARRAYTYPE_P(0);
    ArrayType *statearray2 = PG_GETARG_ARRAYTYPE_P(1);

    /* Check arrays before using */
    float8 *statevalues1 = CheckStateArray(statearray1, "floatvector_combine");
    float8 *statevalues2 = CheckStateArray(statearray2, "floatvector_combine");
    float8 n1 = statevalues1[0];
    float8 n2 = statevalues2[0];

    float8 n;
    int16 dim;
    Datum *statedatums;
    if (n1 == 0.0) {
        n = n2;
        dim = STATE_DIMS(statearray2);
        statedatums = (Datum *)CreateStateDatums(dim);
        for (int16 i = 1; i <= dim; ++i) {
            statedatums[i] = Float8GetDatum(statevalues2[i]);
        }
    } else if (n2 == 0.0) {
        n = n1;
        dim = STATE_DIMS(statearray1);
        statedatums = (Datum *)CreateStateDatums(dim);
        for (int16 i = 1; i <= dim; ++i) {
            statedatums[i] = Float8GetDatum(statevalues1[i]);
        }
    } else {
        n = n1 + n2;
        dim = STATE_DIMS(statearray1);
        CheckExpectedDim(dim, STATE_DIMS(statearray2));
        statedatums = (Datum *)CreateStateDatums(dim);
        for (int16 i = 1; i <= dim; ++i) {
            double v = statevalues1[i] + statevalues2[i];
            if (isinf(v)) {
                float_overflow_error();
            }
            statedatums[i] = Float8GetDatum(v);
        }
    }

    statedatums[0] = Float8GetDatum(n);
    ArrayType *result = construct_array(statedatums, dim + 1, FLOAT8OID, sizeof(float8),
        FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);

    pfree(statedatums);

    PG_RETURN_ARRAYTYPE_P(result);
}

PG_FUNCTION_INFO_V1(floatvector_avg);
Datum floatvector_avg(PG_FUNCTION_ARGS)
{
    ArrayType *statearray = PG_GETARG_ARRAYTYPE_P(0);

    /* Check array before using */
    float8 *statevalues = CheckStateArray(statearray, "floatvector_avg");
    float8 n = statevalues[0];

    /* SQL defines AVG of no values to be NULL */
    if (n == 0.0) {
        PG_RETURN_NULL();
    }

    /* Create vector */
    uint16 dim = STATE_DIMS(statearray);
    CheckDim(dim);
    FloatVector *result = InitFloatVector(dim);
    for (uint16 i = 0; i < dim; ++i) {
        result->x[i] = statevalues[i + 1] / n;
    }
    for (uint16 i = 0; i < dim; ++i) {
        CheckElement(result->x[i]);
    }

    PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(subfloatvector);
Datum subfloatvector(PG_FUNCTION_ARGS)
{
    FloatVector *a = PG_GETARG_FLOATVECTOR_P(0);
    int32 start = PG_GETARG_INT32(1);
    int32 count = PG_GETARG_INT32(2);

    if (count < 1) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
            errmsg("vector must have at least 1 dimension")));
    }

    /*
     * Check if (start + count > a->dim), avoiding integer overflow. a->dim
     * and count are both positive, so a->dim - count won't overflow.
     */
    int32 end = start > a->dim - count ?
        a->dim + 1 :
        start + count;

    /* Indexing starts at 1, like substring */
    if (start < 1) {
        start = 1;
    } else if (start > a->dim) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
            errmsg("vector must have at least 1 dimension")));
    }

    int dim = end - start;
    CheckDim(dim);
    FloatVector *result = InitFloatVector(dim);
    memcpy(result->x, a->x + start - 1, dim * sizeof(float));
    PG_FREE_IF_COPY(a, 0);
    PG_RETURN_POINTER(result);
}

FloatVectorArray FloatVectorArrayInit(int maxlen, int dimensions)
{
    FloatVectorArray res = (FloatVectorArray)palloc(sizeof(FloatVectorArrayData));
    res->length = 0;
    res->maxlen = maxlen;
    res->dim = dimensions;
    res->items = (float *)palloc0(maxlen * (dimensions * sizeof(float)));
    return res;
}

void FloatVectorArrayFree(FloatVectorArray arr)
{
    pfree(arr->items);
    pfree(arr);
}

} /* extern "C" */
