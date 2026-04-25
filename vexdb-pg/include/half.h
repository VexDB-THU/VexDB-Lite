#ifndef HALF_H
#define HALF_H

#include "c.h"

/* F16C has better performance than _Float16 (on x86-64) */
#if defined(__F16C__)
#define F16C_SUPPORT
using half = uint16;
constexpr float HALF_MAX = 65504;
#elif defined(__FLT16_MAX__)
#define FLT16_SUPPORT
#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
using half = float16_t;
#else
#include <float.h>
using half = _Float16;
#endif
constexpr float HALF_MAX = 65504;
#else
using half = uint16;
constexpr float HALF_MAX = 65504;
#endif

#endif /* HALF_H */
