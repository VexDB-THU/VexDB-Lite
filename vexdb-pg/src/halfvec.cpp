#include "pg_compat.h"
#include <math.h>
#include "halfvec.h"
#include "halfutils.h"
#include "vec_common.h"
#include "floatvector.h"
#include "knl/knl_variable.h"

#define HALF_L2_SQUARED_DIST g_instance.annvec_cxt.half_l2_squared_distance
#define HALF_NEGATIVE_INNER_PRODUCT_DIST g_instance.annvec_cxt.half_negative_inner_product
#define HALF_COSINE_DIST g_instance.annvec_cxt.half_cosine_distance

static half pq_getmsghalf(StringInfo msg)
{
    union {
        half h;
        uint16 i;
    } swap;
    swap.i = pq_getmsgint(msg, 2);
    return swap.h;
}

static void pq_sendhalf(StringInfo buf, half h)
{
    union {
        half h;
        uint16 i;
    } swap;
    swap.h = h;
    pq_sendint16(buf, swap.i);
}

static void CheckDims(HalfVector * a, HalfVector * b)
{
    if (a->dim != b->dim) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
                errmsg("different halfvector dimensions %d and %d", a->dim, b->dim)));
    }
}

static void CheckExpectedDim(int32 typmod, int dim)
{
    if (typmod != -1 && typmod != dim) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
                errmsg("expected %d dimensions, not %d", typmod, dim)));
    }
}

static void CheckDim(int dim)
{
    if (dim < 1) {
        ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
                errmsg("halfvector dimension must be at least 1")));
    }
    if (dim > HALFVEC_MAX_DIM) {
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                errmsg("halfvector gets dimension %d exceeding maximum %d", dim, HALFVEC_MAX_DIM)));
    }
}

static void CheckElement(half value)
{
    if (HalfIsNan(value)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("NaN not allowed in halfvector")));
    }

    if (HalfIsInf(value)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("infinite value not allowed in halfvector")));
    }
}

HalfVector *InitHalfVector(int dim)
{
    int size = HALFVEC_SIZE(dim);
    HalfVector *result = (HalfVector *)palloc0(size);
    SET_VARSIZE(result, size);
    result->dim = dim;
    return result;
}

static bool halfvector_isspace(char ch)
    { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f'; }

static float8 *CheckStateArray(ArrayType *statearray, const char *caller)
{
    if (ARR_NDIM(statearray) != 1 || ARR_DIMS(statearray)[0] < 1 ||
        ARR_HASNULL(statearray) || ARR_ELEMTYPE(statearray) != FLOAT8OID) {
        elog(ERROR, "%s: expected state array", caller);
    }
    return (float8 *) ARR_DATA_PTR(statearray);
}

static HalfVector* halfvector_input(char* lit, int32 atttypmod)
{
    half x[HALFVEC_MAX_DIM];
    int dim = 0;
    char *pt = lit;
    HalfVector *result;

    while (halfvector_isspace(*pt)) {
        pt++;
    }

    if (*pt != '[') {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("invalid input syntax for type halfvector: \"%s\"", lit),
                 errdetail("Halfvector contents must start with \"[\".")));
    }

    pt++;
    while (halfvector_isspace(*pt)) {
        pt++;
    }

    if (*pt == ']') {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("halfvector dimension must be at least 1")));
    }

    for (;;) {
        float val;
        char *stringEnd;

        if (dim == HALFVEC_MAX_DIM) {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("halfvector cannot have more than %d dimensions", HALFVEC_MAX_DIM)));
        }

        while (halfvector_isspace(*pt)) {
            pt++;
        }

        /* Check for empty string like float4in */
        if (*pt == '\0') {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid input syntax for type halfvector: \"%s\"", lit)));
        }

        errno = 0;
        /* Postgres sets LC_NUMERIC to C on startup */
        val = strtof(pt, &stringEnd);

        if (stringEnd == pt) {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid input syntax for type halfvector: \"%s\"", lit)));
        }
        if (errno == ERANGE && isinf(val)) {
            ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("\"%s\" is out of range for type halfvector",
                           pnstrdup(pt, stringEnd - pt))));
        }

        x[dim] = Float4ToHalf(val);
        dim++;
        pt = stringEnd;

        while (halfvector_isspace(*pt)) {
            pt++;
        }

        if (*pt == ',') {
            pt++;
        } else if (*pt == ']') {
            pt++;
            break;
        } else {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid input syntax for type halfvector: \"%s\"", lit)));
        }
    }

    /* Only whitespace is allowed after the closing brace */
    while (halfvector_isspace(*pt)) {
        pt++;
    }

    if (*pt != '\0') {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("invalid input syntax for type halfvector: \"%s\"", lit),
                 errdetail("Junk after closing right brace.")));
    }

    CheckDim(dim);
    CheckExpectedDim(atttypmod, dim);

    result = InitHalfVector(dim);
    memcpy(result->x, x, dim * sizeof(half));
    return result;
}

extern "C" {

/*
 * Convert textual representation to internal representation
 */
PG_FUNCTION_INFO_V1(halfvector_in);
Datum halfvector_in(PG_FUNCTION_ARGS)
{
    char *lit = PG_GETARG_CSTRING(0);
    int32 typmod = PG_GETARG_INT32(2);
    HalfVector *result = halfvector_input(lit, typmod);
    PG_RETURN_HALFVEC_P(result);
}

/*
 * Convert internal representation to textual representation
 */
PG_FUNCTION_INFO_V1(halfvector_out);
Datum halfvector_out(PG_FUNCTION_ARGS)
{
    HalfVector *vector = PG_GETARG_HALFVEC_P(0);
    int dim = vector->dim;
    char *buf;
    char *ptr;

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
    buf = (char *) palloc(FLOAT_SHORTEST_DECIMAL_LEN * dim + 2);
    ptr = buf;

    AppendChar(ptr, '[');

    for (int i = 0; i < dim; i++) {
        if (i > 0) {
            AppendChar(ptr, ',');
        }

        /*
         * Use shortest decimal representation of single-precision float for
         * simplicity
         */
        AppendFloat(ptr, HalfToFloat4(vector->x[i]));
    }

    AppendChar(ptr, ']');
    *ptr = '\0';

    PG_FREE_IF_COPY(vector, 0);
    PG_RETURN_CSTRING(buf);
}

/*
 * Convert type modifier
 */
PG_FUNCTION_INFO_V1(halfvector_typmod_in);
Datum halfvector_typmod_in(PG_FUNCTION_ARGS)
{
    ArrayType *ta = PG_GETARG_ARRAYTYPE_P(0);
    int32 *tl;
    int n;

    tl = ArrayGetIntegerTypmods(ta, &n);

    if (n != 1) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid type modifier")));
    }

    if (*tl < 1) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("halfvector dimension must be at least 1")));
    }

    if (*tl > HALFVEC_MAX_DIM) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("halfvector cannot have more than %d dimensions", HALFVEC_MAX_DIM)));
    }

    PG_RETURN_INT32(*tl);
}


PG_FUNCTION_INFO_V1(halfvector_typmod_out);
Datum halfvector_typmod_out(PG_FUNCTION_ARGS)
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
PG_FUNCTION_INFO_V1(halfvector_recv);
Datum halfvector_recv(PG_FUNCTION_ARGS)
{
    StringInfo buf = (StringInfo) PG_GETARG_POINTER(0);
    int32 typmod = PG_GETARG_INT32(2);
    int16 dim = pq_getmsgint(buf, sizeof(int16));
    int16 unused = pq_getmsgint(buf, sizeof(int16));

    CheckDim(dim);
    CheckExpectedDim(typmod, dim);

    if (unused != 0) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("expected unused to be 0, not %d", unused)));
    }

    HalfVector *result = InitHalfVector(dim);
    for (int i = 0; i < dim; i++) {
        result->x[i] = pq_getmsghalf(buf);
        CheckElement(result->x[i]);
    }

    PG_RETURN_HALFVEC_P(result);
}

/*
 * Convert internal representation to the external binary representation
 */
PG_FUNCTION_INFO_V1(halfvector_send);
Datum halfvector_send(PG_FUNCTION_ARGS)
{
    HalfVector *vec = PG_GETARG_HALFVEC_P(0);
    StringInfoData buf;

    pq_begintypsend(&buf);
    pq_sendint(&buf, vec->dim, sizeof(int16));
    pq_sendint(&buf, vec->unused, sizeof(int16));
    for (int i = 0; i < vec->dim; i++) {
        pq_sendhalf(&buf, vec->x[i]);
    }

    PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * Convert half vector to half vector
 * This is needed to check the type modifier
 */
PG_FUNCTION_INFO_V1(halfvector_to_halfvector);
Datum halfvector_to_halfvector(PG_FUNCTION_ARGS)
{
    HalfVector *vec = PG_GETARG_HALFVEC_P(0);
    int32 typmod = PG_GETARG_INT32(1);
    CheckExpectedDim(typmod, vec->dim);
    PG_RETURN_HALFVEC_P(vec);
}

/*
 * Convert array to half vector
 */
PG_FUNCTION_INFO_V1(array_to_halfvector);
Datum array_to_halfvector(PG_FUNCTION_ARGS)
{
    ArrayType  *array = PG_GETARG_ARRAYTYPE_P(0);
    int32 typmod = PG_GETARG_INT32(1);
    HalfVector *result;
    int16 typlen;
    bool typbyval;
    char typalign;
    Datum *elemsp;
    int nelemsp;

    if (ARR_NDIM(array) > 1) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("array must be 1-D")));
    }

    if (ARR_HASNULL(array) && array_contains_nulls(array)) {
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("array must not contain nulls")));
    }

    get_typlenbyvalalign(ARR_ELEMTYPE(array), &typlen, &typbyval, &typalign);
    deconstruct_array(array, ARR_ELEMTYPE(array), typlen, typbyval, typalign, &elemsp, NULL, &nelemsp);

    CheckDim(nelemsp);
    CheckExpectedDim(typmod, nelemsp);

    result = InitHalfVector(nelemsp);

    if (ARR_ELEMTYPE(array) == INT4OID) {
        for (int i = 0; i < nelemsp; i++) {
            result->x[i] = Float4ToHalf(DatumGetInt32(elemsp[i]));
        }
    } else if (ARR_ELEMTYPE(array) == FLOAT8OID) {
        for (int i = 0; i < nelemsp; i++) {
            result->x[i] = Float4ToHalf(DatumGetFloat8(elemsp[i]));
        }
    } else if (ARR_ELEMTYPE(array) == FLOAT4OID) {
        for (int i = 0; i < nelemsp; i++) {
            result->x[i] = Float4ToHalf(DatumGetFloat4(elemsp[i]));
        }
    } else if (ARR_ELEMTYPE(array) == NUMERICOID) {
        for (int i = 0; i < nelemsp; i++) {
            result->x[i] = Float4ToHalf(DatumGetFloat4(DirectFunctionCall1(numeric_float4, elemsp[i])));
        }
    } else {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("unsupported array type")));
    }

    /*
     * Free allocation from deconstruct_array. Do not free individual elements
     * when pass-by-reference since they point to original array.
     */
    pfree(elemsp);

    /* Check elements */
    for (int i = 0; i < result->dim; i++) {
        CheckElement(result->x[i]);
    }

    PG_RETURN_HALFVEC_P(result);
}

/*
 * Convert half vector to float4[]
 */
PG_FUNCTION_INFO_V1(halfvector_to_float4);
Datum halfvector_to_float4(PG_FUNCTION_ARGS)
{
    HalfVector *vec = PG_GETARG_HALFVEC_P(0);
    Datum *datums = (Datum *) palloc(sizeof(Datum) * vec->dim);
    for (int i = 0; i < vec->dim; i++) {
        datums[i] = Float4GetDatum(HalfToFloat4(vec->x[i]));
    }

    /* Use TYPALIGN_INT for float4 */
    ArrayType *result = construct_array(datums, vec->dim, FLOAT4OID, sizeof(float4), true, TYPALIGN_INT);

    pfree(datums);

    PG_RETURN_POINTER(result);
}

/*
 * Convert vector to half vec
 */
PG_FUNCTION_INFO_V1(floatvector_to_halfvector);
Datum floatvector_to_halfvector(PG_FUNCTION_ARGS)
{
	FloatVector	*vec = PG_GETARG_FLOATVECTOR_P(0);
	int32		typmod = PG_GETARG_INT32(1);
	HalfVector *result;

	CheckDim(vec->dim);
	CheckExpectedDim(typmod, vec->dim);

	result = InitHalfVector(vec->dim);

	for (int i = 0; i < vec->dim; i++)
		result->x[i] = Float4ToHalf(vec->x[i]);

	PG_RETURN_POINTER(result);
}


PG_FUNCTION_INFO_V1(halfvector_to_floatvector);
Datum halfvector_to_floatvector(PG_FUNCTION_ARGS)
{
    HalfVector	*halfvec = PG_GETARG_HALFVEC_P(0);
	int32		typmod = PG_GETARG_INT32(1);
	FloatVector *result;

	CheckDim(halfvec->dim);
	CheckExpectedDim(typmod, halfvec->dim);

	result = InitFloatVector(halfvec->dim);

	for (int i = 0; i < halfvec->dim; i++)
		result->x[i] = HalfToFloat4(halfvec->x[i]);

	PG_RETURN_POINTER(result);
}

/*
 * Get the L2 distance between half vectors
 */
PG_FUNCTION_INFO_V1(halfvector_l2_distance);
Datum halfvector_l2_distance(PG_FUNCTION_ARGS)
{
    HalfVector *a = PG_GETARG_HALFVEC_P(0);
    HalfVector *b = PG_GETARG_HALFVEC_P(1);

    CheckDims(a, b);

    PG_RETURN_FLOAT8(sqrt((double) HALF_L2_SQUARED_DIST(a->x, b->x, a->dim)));
}

/*
 * Get the L2 squared distance between half vectors
 */
PG_FUNCTION_INFO_V1(halfvector_l2_squared_distance);
Datum halfvector_l2_squared_distance(PG_FUNCTION_ARGS)
{
    HalfVector *a = PG_GETARG_HALFVEC_P(0);
    HalfVector *b = PG_GETARG_HALFVEC_P(1);

    CheckDims(a, b);

    PG_RETURN_FLOAT8((double) HALF_L2_SQUARED_DIST(a->x, b->x, a->dim));
}

/*
 * Get the inner product of two half vectors
 */
PG_FUNCTION_INFO_V1(halfvector_inner_product);
Datum halfvector_inner_product(PG_FUNCTION_ARGS)
{
    HalfVector *a = PG_GETARG_HALFVEC_P(0);
    HalfVector *b = PG_GETARG_HALFVEC_P(1);

    CheckDims(a, b);

    PG_RETURN_FLOAT8((double) -HALF_NEGATIVE_INNER_PRODUCT_DIST(a->x, b->x, a->dim));
}

/*
 * Get the negative inner product of two half vectors
 */
PG_FUNCTION_INFO_V1(halfvector_negative_inner_product);
Datum halfvector_negative_inner_product(PG_FUNCTION_ARGS)
{
    HalfVector *a = PG_GETARG_HALFVEC_P(0);
    HalfVector *b = PG_GETARG_HALFVEC_P(1);

    CheckDims(a, b);
    double distance = HALF_NEGATIVE_INNER_PRODUCT_DIST(a->x, b->x, a->dim);
    PG_RETURN_FLOAT8(abs(distance) == 0.0 ? 0 : distance);
}

/*
 * Get the cosine distance between two half vectors
 */
PG_FUNCTION_INFO_V1(halfvector_cosine_distance);
Datum halfvector_cosine_distance(PG_FUNCTION_ARGS)
{
    HalfVector *a = PG_GETARG_HALFVEC_P(0);
    HalfVector *b = PG_GETARG_HALFVEC_P(1);
    CheckDims(a, b);
    double similarity = HALF_COSINE_DIST(a->x, b->x, a->dim);
    PG_RETURN_FLOAT8(1 + similarity);
}

/*
 * Get the distance for spherical k-means
 * Currently uses angular distance since needs to satisfy triangle inequality
 * Assumes inputs are unit vectors (skips norm)
 */
PG_FUNCTION_INFO_V1(halfvector_spherical_distance);
Datum halfvector_spherical_distance(PG_FUNCTION_ARGS)
{
    HalfVector *a = PG_GETARG_HALFVEC_P(0);
    HalfVector *b = PG_GETARG_HALFVEC_P(1);

    CheckDims(a, b);

    double distance = -HALF_NEGATIVE_INNER_PRODUCT_DIST(a->x, b->x, a->dim);

    /* Prevent NaN with acos with loss of precision */
    if (distance > 1) {
        distance = 1;
    } else if (distance < -1) {
        distance = -1;
    }

    PG_RETURN_FLOAT8(acos(distance) / M_PI);
}

/*
 * Get the dimensions of a half vector
 */
PG_FUNCTION_INFO_V1(halfvector_dims);
Datum halfvector_dims(PG_FUNCTION_ARGS)
{
    HalfVector *a = PG_GETARG_HALFVEC_P(0);
    PG_RETURN_INT32(a->dim);
}

/*
 * Get the L2 norm of a half vector
 */
PG_FUNCTION_INFO_V1(halfvector_l2_norm);
Datum halfvector_l2_norm(PG_FUNCTION_ARGS)
{
    HalfVector *a = PG_GETARG_HALFVEC_P(0);
    double norm = -HALF_NEGATIVE_INNER_PRODUCT_DIST(a->x, a->x, a->dim);
    PG_RETURN_FLOAT8(sqrt(norm));
}

/*
 * Normalize a half vector with the L2 norm
 */
PG_FUNCTION_INFO_V1(halfvector_l2_normalize);
Datum halfvector_l2_normalize(PG_FUNCTION_ARGS)
{
    HalfVector *a = PG_GETARG_HALFVEC_P(0);
    const half *ax = a->x;
    double norm = -HALF_NEGATIVE_INNER_PRODUCT_DIST(ax, ax, a->dim);
    norm = sqrt(norm);
    
    HalfVector *result = InitHalfVector(a->dim);
    if (norm <= 0) { /* Return zero vector for zero norm */
        PG_RETURN_POINTER(result);
    }
    
    half *rx = result->x;
    if (norm == 1) {
        memcpy(rx, a->x, a->dim * sizeof(half));
        PG_RETURN_POINTER(result);
    }

    for (int i = 0; i < a->dim; i++) {
        rx[i] = Float4ToHalfUnchecked(HalfToFloat4(ax[i]) / norm);
    }
        
    /* Check for overflow */
    for (int i = 0; i < a->dim; i++)
    {
        if (HalfIsInf(rx[i]))
            float_overflow_error();
    }

    PG_RETURN_POINTER(result);
}

/*
 * Add half vectors
 */
PG_FUNCTION_INFO_V1(halfvector_add);
Datum halfvector_add(PG_FUNCTION_ARGS)
{
    HalfVector *a = PG_GETARG_HALFVEC_P(0);
    HalfVector *b = PG_GETARG_HALFVEC_P(1);
    half *ax = a->x;
    half *bx = b->x;

    CheckDims(a, b);

    HalfVector *result = InitHalfVector(a->dim);
    half *rx = result->x;

    /* Auto-vectorized */
    for (int i = 0, imax = a->dim; i < imax; i++) {
#ifdef FLT16_SUPPORT
        rx[i] = ax[i] + bx[i];
#else
        rx[i] = Float4ToHalfUnchecked(HalfToFloat4(ax[i]) + HalfToFloat4(bx[i]));
#endif
    }

    /* Check for overflow */
    for (int i = 0, imax = a->dim; i < imax; i++) {
        if (HalfIsInf(rx[i])) {
            float_overflow_error();
        }
    }

    PG_RETURN_HALFVEC_P(result);
}

/*
 * Subtract half vectors
 */
PG_FUNCTION_INFO_V1(halfvector_sub);
Datum halfvector_sub(PG_FUNCTION_ARGS)
{
    HalfVector *a = PG_GETARG_HALFVEC_P(0);
    HalfVector *b = PG_GETARG_HALFVEC_P(1);
    half *ax = a->x;
    half *bx = b->x;

    CheckDims(a, b);

    HalfVector *result = InitHalfVector(a->dim);
    half *rx = result->x;

    /* Auto-vectorized */
    for (int i = 0, imax = a->dim; i < imax; i++) {
#ifdef FLT16_SUPPORT
        rx[i] = ax[i] - bx[i];
#else
        rx[i] = Float4ToHalfUnchecked(HalfToFloat4(ax[i]) - HalfToFloat4(bx[i]));
#endif
    }

    /* Check for overflow */
    for (int i = 0, imax = a->dim; i < imax; i++) {
        if (HalfIsInf(rx[i])) {
            float_overflow_error();
        }
    }

    PG_RETURN_HALFVEC_P(result);
}

/*
 * Get a subvector
 */
PG_FUNCTION_INFO_V1(halfvector_subvector);
Datum halfvector_subvector(PG_FUNCTION_ARGS)
{
    HalfVector *a = PG_GETARG_HALFVEC_P(0);
    int32 start = PG_GETARG_INT32(1);
    int32 count = PG_GETARG_INT32(2);
    int32 end;
    HalfVector *result;
    int32 dim;

    if (count < 1) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("halfvector dimension must be at least 1")));
    }

    /*
     * Check if (start + count > a->dim), avoiding integer overflow. a->dim
     * and count are both positive, so a->dim - count won't overflow.
     */
    if (start > a->dim - count) {
        end = a->dim + 1;
    } else {
        end = start + count;
    }

    /* Indexing starts at 1, like substring */
    if (start < 1) {
        start = 1;
    } else if (start > a->dim) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("halfvector dimension must be at least 1")));
    }

    dim = end - start;
    CheckDim(dim);
    result = InitHalfVector(dim);
    memcpy(result->x, a->x + start - 1, dim * sizeof(half));
    PG_RETURN_POINTER(result);
}

/*
 * Internal helper to compare half vectors
 */
static int halfvector_cmp_internal(HalfVector * a, HalfVector * b)
{
    int dim = Min(a->dim, b->dim);

    /* Check values before dimensions to be consistent with Postgres arrays */
    for (int i = 0; i < dim; i++) {
        if (HalfToFloat4(a->x[i]) < HalfToFloat4(b->x[i])) {
            return -1;
        }

        if (HalfToFloat4(a->x[i]) > HalfToFloat4(b->x[i])) {
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

/*
 * Less than
 */
PG_FUNCTION_INFO_V1(halfvector_lt);
Datum halfvector_lt(PG_FUNCTION_ARGS)
{
    HalfVector *a = PG_GETARG_HALFVEC_P(0);
    HalfVector *b = PG_GETARG_HALFVEC_P(1);
    PG_RETURN_BOOL(halfvector_cmp_internal(a, b) < 0);
}

/*
 * Less than or equal
 */
PG_FUNCTION_INFO_V1(halfvector_le);
Datum halfvector_le(PG_FUNCTION_ARGS)
{
    HalfVector *a = PG_GETARG_HALFVEC_P(0);
    HalfVector *b = PG_GETARG_HALFVEC_P(1);
    PG_RETURN_BOOL(halfvector_cmp_internal(a, b) <= 0);
}

/*
 * Equal
 */
PG_FUNCTION_INFO_V1(halfvector_eq);
Datum halfvector_eq(PG_FUNCTION_ARGS)
{
    HalfVector *a = PG_GETARG_HALFVEC_P(0);
    HalfVector *b = PG_GETARG_HALFVEC_P(1);
    PG_RETURN_BOOL(halfvector_cmp_internal(a, b) == 0);
}

/*
 * Not equal
 */
PG_FUNCTION_INFO_V1(halfvector_ne);
Datum halfvector_ne(PG_FUNCTION_ARGS)
{
    HalfVector *a = PG_GETARG_HALFVEC_P(0);
    HalfVector *b = PG_GETARG_HALFVEC_P(1);
    PG_RETURN_BOOL(halfvector_cmp_internal(a, b) != 0);
}

/*
 * Greater than or equal
 */
PG_FUNCTION_INFO_V1(halfvector_ge);
Datum halfvector_ge(PG_FUNCTION_ARGS)
{
    HalfVector *a = PG_GETARG_HALFVEC_P(0);
    HalfVector *b = PG_GETARG_HALFVEC_P(1);
    PG_RETURN_BOOL(halfvector_cmp_internal(a, b) >= 0);
}

/*
 * Greater than
 */
PG_FUNCTION_INFO_V1(halfvector_gt);
Datum halfvector_gt(PG_FUNCTION_ARGS)
{
    HalfVector *a = PG_GETARG_HALFVEC_P(0);
    HalfVector *b = PG_GETARG_HALFVEC_P(1);
    PG_RETURN_BOOL(halfvector_cmp_internal(a, b) > 0);
}

/*
 * Compare half vectors
 */
PG_FUNCTION_INFO_V1(halfvector_cmp);
Datum halfvector_cmp(PG_FUNCTION_ARGS)
{
    HalfVector *a = PG_GETARG_HALFVEC_P(0);
    HalfVector *b = PG_GETARG_HALFVEC_P(1);
    PG_RETURN_INT32(halfvector_cmp_internal(a, b));
}

/*
 * Accumulate half vectors
 */
PG_FUNCTION_INFO_V1(halfvector_accum);
Datum halfvector_accum(PG_FUNCTION_ARGS)
{
    ArrayType  *statearray = PG_GETARG_ARRAYTYPE_P(0);
    HalfVector *newval = PG_GETARG_HALFVEC_P(1);
    float8 *statevalues;
    int16 dim;
    bool newarr;
    float8 n;
    Datum *statedatums;
    half *x = newval->x;
    ArrayType *result;

    /* Check array before using */
    statevalues = CheckStateArray(statearray, "halfvector_accum");
    dim = STATE_DIMS(statearray);
    newarr = dim == 0;

    if (newarr) {
        dim = newval->dim;
    } else {
        CheckExpectedDim(dim, newval->dim);
    }

    n = statevalues[0] + 1.0;

    statedatums = (Datum *)CreateStateDatums(dim);
    statedatums[0] = Float8GetDatum(n);

    if (newarr) {
        for (int i = 0; i < dim; i++) {
            statedatums[i + 1] = Float8GetDatum((double) HalfToFloat4(x[i]));
        }
    } else {
        for (int i = 0; i < dim; i++) {
            double v = statevalues[i + 1] + (double) HalfToFloat4(x[i]);

            /* Check for overflow */
            if (isinf(v)) {
                float_overflow_error();
            }

            statedatums[i + 1] = Float8GetDatum(v);
        }
    }

    /* Use float8 array like float4_accum */
    result = construct_array(statedatums, dim + 1,
                             FLOAT8OID,
                             sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);

    pfree(statedatums);

    PG_RETURN_ARRAYTYPE_P(result);
}

/*
 * Average half vectors
 */
PG_FUNCTION_INFO_V1(halfvector_avg);
Datum halfvector_avg(PG_FUNCTION_ARGS)
{
    ArrayType  *statearray = PG_GETARG_ARRAYTYPE_P(0);
    float8 *statevalues;
    float8 n;
    uint16 dim;
    HalfVector *result;

    /* Check array before using */
    statevalues = CheckStateArray(statearray, "halfvector_avg");
    n = statevalues[0];

    /* SQL defines AVG of no values to be NULL */
    if (n == 0.0) {
        PG_RETURN_NULL();
    }

    /* Create half vector */
    dim = STATE_DIMS(statearray);
    CheckDim(dim);
    result = InitHalfVector(dim);
    for (int i = 0; i < dim; i++) {
        result->x[i] = Float4ToHalf(statevalues[i + 1] / n);
        CheckElement(result->x[i]);
    }

    PG_RETURN_HALFVEC_P(result);
}


static int halfvector_fastcmp(Datum x, Datum y, SortSupport)
{
    HalfVector *a = (HalfVector *)DatumGetHalfVector(x);
    HalfVector *b = (HalfVector *)DatumGetHalfVector(y);
    return halfvector_cmp_internal(a, b);
}

PG_FUNCTION_INFO_V1(halfvector_sortsupport);
Datum halfvector_sortsupport(PG_FUNCTION_ARGS)
{
    SortSupport ssup = (SortSupport)PG_GETARG_POINTER(0);
    ssup->comparator = halfvector_fastcmp;
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(hashhalfvector);
Datum hashhalfvector(PG_FUNCTION_ARGS)
{
    HalfVector *a = (HalfVector *)PG_GETARG_HALFVEC_P(0);
    return hash_any((unsigned char *)a->x, sizeof(half) * a->dim);
}

void halfs_to_floats(half *h, float *f, uint32 dim)
{
    for (uint32 i = 0; i < dim; ++i) {
        f[i] = HalfToFloat4(h[i]);
    }
}

} /* extern "C" */
