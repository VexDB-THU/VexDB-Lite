#ifndef DISTANCE_FUNC_NAME
static_assert(false, "don't use the file without definition DISTANCE_FUNC_NAME");
#endif

#include "halfvec.h"
#include "halfutils.h"

namespace ann_helper {
half DISTANCE_FUNC_NAME(float_to_half)(float num)
{
#ifdef USE_AVX
    return _cvtss_sh(num, _MM_FROUND_TO_NEAREST_INT|_MM_FROUND_NO_EXC);
#elif defined(__NEON_SUPPORT__)
    float32x4_t v = vdupq_n_f32(num);    
    float16x4_t r = vcvt_f16_f32(v);
    return vget_lane_f16(r, 0);
#else
    return float_to_half(num);
#endif
}

float DISTANCE_FUNC_NAME(half_to_float)(half num)
{
#ifdef USE_AVX
    return _cvtsh_ss(num);
#elif defined(__NEON_SUPPORT__)
    float16x4_t v = vdup_n_f16(num);
    float32x4_t r = vcvt_f32_f16(v);
    return vgetq_lane_f32(r, 0);
#else
    return half_to_float(num);
#endif
}
} /* namespace ann_helper */
