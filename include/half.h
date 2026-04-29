#ifndef HALF_H
#define HALF_H

#if defined(PG_VEXDB_TARGET_DUCK)
#include <cstdint>
using uint16 = uint16_t;
#else
#include "c.h"
#endif

/*
 * Always use uint16 for half on x86_64 for consistent ABI.
 * SIMD files use F16C intrinsics directly for conversion.
 */
#if defined(__x86_64__) || defined(_M_X64)
using half = uint16;
constexpr float HALF_MAX = 65504;
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
using half = float16_t;
constexpr float HALF_MAX = 65504;
#else
using half = uint16;
constexpr float HALF_MAX = 65504;
#endif

#endif /* HALF_H */
