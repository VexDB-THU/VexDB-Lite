#ifndef VEC_COMMON_H
#define VEC_COMMON_H

#include <algorithm> 

#ifndef FLOAT_SHORTEST_DECIMAL_LEN
#define FLOAT_SHORTEST_DECIMAL_LEN 16
#endif

#ifndef TYPALIGN_DOUBLE
#define TYPALIGN_DOUBLE 'd'
#endif

#ifndef TYPALIGN_INT
#define TYPALIGN_INT 'i'
#endif

#define STATE_DIMS(x) (ARR_DIMS(x)[0] - 1)
#define CreateStateDatums(dim) palloc(sizeof(Datum) * (dim + 1))

/*
 * A preparation for floatvectorout function
 */
inline int float_to_shortest_decimal_bufn(float4 f, char *buf)
{
    int len = snprintf(buf, FLOAT_SHORTEST_DECIMAL_LEN, "%g", f);
    return std::min(len, FLOAT_SHORTEST_DECIMAL_LEN - 1);
}

#define AppendChar(ptr, c) (*(ptr)++ = (c))
#define AppendFloat(ptr, f) ((ptr) += float_to_shortest_decimal_bufn((f), (ptr)))

inline void float_overflow_error(void)
{
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
        errmsg("value out of range: overflow")));
}

inline void float_underflow_error(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("value out of range: underflow")));
}

#endif /* VEC_COMMON_H */
