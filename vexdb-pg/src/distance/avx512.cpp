#include "distance/architecture_macro.h"

#if COMPILER_SUPPORT_AVX512_EXTEND
#include "distance/distance_utils.h"
#include "distance/cblas_interface.h"

#define DISTANCE_FUNC_NAME(name)  AVX512_FUNC(name)
#define DISTANCE_STRUCT_NAME(name) AVX512_STRUCT(name)
#define __SSE_SUPPORT__
#define __AVX_SUPPORT__
#define __AVX512_SUPPORT__
#include "./distances_simd_template.cpp"
#include "./code_distance_template.cpp"
#include "./rabitq_template.cpp"
#include "./template_half.cpp"
#endif /* COMPILER_SUPPORT_AVX512_EXTEND */
