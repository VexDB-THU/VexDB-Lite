#ifndef HALFUTILS_H
#define HALFUTILS_H

#include <cmath>
#include <vtl/expr_helper>
#ifdef F16C_SUPPORT
#include <immintrin.h>
#endif

#include "halfvec.h"
#include "vec_common.h"
#include "knl/knl_instance.h"
#include "distance/core/halfutils_core.h"

inline bool HalfIsNan(half num)
{
#ifdef FLT16_SUPPORT
    return std::isnan((float)num);
#else
    return (num & 0x7C00) == 0x7C00 && (num & 0x7FFF) != 0x7C00;
#endif
}

inline bool HalfIsInf(half num)
{
#ifdef FLT16_SUPPORT
    return std::isinf((float)num);
#else
    return (num & 0x7FFF) == 0x7C00;
#endif
}

inline bool HalfIsZero(half num)
{
#ifdef FLT16_SUPPORT
    return num == 0;
#else
    return (num & 0x7FFF) == 0x0000;
#endif
}

inline float HalfToFloat4(half num)
{
    return g_instance.annvec_cxt.half_to_float(num);
}

inline half Float4ToHalfUnchecked(float num)
{
    return g_instance.annvec_cxt.float_to_half(num);
}

inline half Float4ToHalf(float num)
{
    if (isnan(num)) {
        ereport(ERROR,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("NaN not allowed in halfvector")));
    }
    if (isinf(num)) {
        ereport(ERROR,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("infinite value not allowed in halfvector")));
    }
    if (num > HALF_MAX) {
        ereport(ERROR,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("halfvector value %f is not allowed to be greater than %f", num, HALF_MAX)));
    }
    if (num < -HALF_MAX) {
        ereport(ERROR,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("halfvector value %f is not allowed to be less than %f", num, -HALF_MAX)));
    }
    return Float4ToHalfUnchecked(num);
}

#endif /* HALFUTILS_H */
