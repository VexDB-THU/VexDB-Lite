/**
 * Copyright ...
 * Core (PG-free) half conversion helpers
 */

#ifndef HALFUTILS_CORE_H
#define HALFUTILS_CORE_H

#include <cstdint>

#include "half.h"

namespace half_core {
namespace internal {
union FloatBits {
    float v;
    uint32 bits;
    FloatBits() = default;
    constexpr FloatBits(float _v) : v(_v) {}
    explicit constexpr FloatBits(uint32 _bits) : bits(_bits) {}
};
} /* namespace half_core::internal */

template <bool res_signed>
inline _GLIBCXX17_CONSTEXPR float half_to_float_internal(uint16 s)
{
    uint32 exponent = (s & 0x7C00) >> 10;
    uint32 mantissa = s & 0x03FF;
    uint32 result = res_signed ? (s & 0x8000) << 16 : 0;

    if (exponent == 0) {
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
} /* namespace half_core */

inline _GLIBCXX17_CONSTEXPR float half_to_float(uint16 s)
{
    return half_core::half_to_float_internal<true>(s);
}

inline _GLIBCXX17_CONSTEXPR float half_to_float_unsigned(uint16 s)
{
    return half_core::half_to_float_internal<false>(s);
}

inline _GLIBCXX17_CONSTEXPR uint16 float_to_half(half_core::internal::FloatBits s)
{
    int exponent = (s.bits & 0x7F800000) >> 23;
    int mantissa = s.bits & 0x007FFFFF;
    uint16 result = (s.bits & 0x80000000) >> 16;

    if (exponent > 98) {
        exponent -= 127;
        int rem = mantissa & 0x00000FFF;
        if (exponent < -14) {
            int diff = -exponent - 14;
            mantissa >>= diff;
            mantissa += 1 << (23 - diff);
            rem |= mantissa & 0x00000FFF;
        }
        int m = mantissa >> 13;

        int gr = (mantissa >> 12) % 4;
        if (gr == 3 || (gr == 1 && rem != 0)) {
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

inline _GLIBCXX17_CONSTEXPR uint16 float_to_half(float v)
{
    return float_to_half(half_core::internal::FloatBits(v));
}

#endif /* HALFUTILS_CORE_H */
