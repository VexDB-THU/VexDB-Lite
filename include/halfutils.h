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

namespace half_util {
namespace internal {
union FloatBits {
    float v;
    uint32 bits;
    FloatBits() = default;
    constexpr FloatBits(float _v) : v(_v) {}
    explicit constexpr FloatBits(uint32 _bits) : bits(_bits) {}
};
} /* namespace half_util::internal */

template <bool res_signed>
inline _GLIBCXX17_CONSTEXPR float half_to_float_internal(uint16 s)
{
    uint32 exponent = (s & 0x7C00) >> 10;
    uint32 mantissa = s & 0x03FF;
    uint32 result = res_signed ? (s & 0x8000) << 16 : 0;

    if (unlikely(exponent == 0)) {
        if (mantissa != 0) {
            exponent = -14;
            for (int i = 0; i < 10; ++i) {
                mantissa <<= 1;
                exponent -= 1;
                if ((mantissa >> 10) % 2 == 1) {
                    mantissa &= 0x03ff;
                    break;
                }
            }
            result |= (exponent + 127) << 23;
        }
    } else {
        result |= (exponent - 15 + 127) << 23;
    }

    result |= mantissa << 13;
    return internal::FloatBits(result).v;
}
} /* namespace half_util */

inline _GLIBCXX17_CONSTEXPR float half_to_float(uint16 s)
    { return half_util::half_to_float_internal<true>(s); }
inline _GLIBCXX17_CONSTEXPR float half_to_float_unsigned(uint16 s)
    { return half_util::half_to_float_internal<false>(s); }
inline _GLIBCXX17_CONSTEXPR uint16 float_to_half(half_util::internal::FloatBits s)
{
    int exponent = (s.bits & 0x7F800000) >> 23;
    int mantissa = s.bits & 0x007FFFFF;
    uint16 result = (s.bits & 0x80000000) >> 16;

    if (exponent > 98) {
        exponent -= 127;
        int s = mantissa & 0x00000FFF;
        if (exponent < -14) {
            int diff = -exponent - 14;
            mantissa >>= diff;
            mantissa += 1 << (23 - diff);
            s |= mantissa & 0x00000FFF;
        }
        int m = mantissa >> 13;

        int gr = (mantissa >> 12) % 4;
        if (gr == 3 || (gr == 1 && s != 0)) {
            m += 1;
        }
        if (m == 1024) {
            m = 0;
            exponent += 1;
        }
        if (exponent >= -14) {
            result |= (exponent + 15) << 10;
        }
        result |= m;
    }

    return result;
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
