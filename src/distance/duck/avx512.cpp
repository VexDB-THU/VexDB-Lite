#include "distance/core/architecture_macro.h"

#if COMPILER_SUPPORT_AVX512_EXTEND
#include "distance/core/distance_utils_core.h"
#include "distance/core/transform_template_core.h"

#define DISTANCE_FUNC_NAME(name) AVX512_FUNC(name)
#define __SSE_SUPPORT__
#define __AVX_SUPPORT__
#define __AVX512_SUPPORT__
#include "../core/distances_simd_template.cpp"
#include "../core/code_distance_template.cpp"
#endif
