#include "distance/core/architecture_macro.h"

#if COMPILER_SUPPORT_SSE
#include "distance/core/distance_utils_core.h"
#include "distance/core/transform_template_core.h"

#define DISTANCE_FUNC_NAME(name) SSE_FUNC(name)
#define __SSE_SUPPORT__
#include "../core/distances_simd_template.cpp"
#include "../core/code_distance_template.cpp"
#endif
