// Duck-side GENERAL fallback instantiation of PQ SIMD kernels.
// (sse.cpp / avx.cpp / avx512.cpp follow the same pattern with their
// respective DISTANCE_FUNC_NAME prefix and arch guard.)
#include "distance/core/architecture_macro.h"
#include "distance/core/distance_utils_core.h"
#include "distance/core/transform_template_core.h"

#define DISTANCE_FUNC_NAME(name) GENERAL_FUNC(name)
#include "../core/distances_simd_template.cpp"
#include "../core/code_distance_template.cpp"
