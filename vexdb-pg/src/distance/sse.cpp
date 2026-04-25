#include "distance/architecture_macro.h"

#if COMPILER_SUPPORT_SSE
#include "distance/distance_utils.h"
#include "distance/cblas_interface.h"

#define DISTANCE_FUNC_NAME(name)  SSE_FUNC(name)
#define DISTANCE_STRUCT_NAME(name) SSE_STRUCT(name)
#define __SSE_SUPPORT__
#include "./distances_simd_template.cpp"
#include "./code_distance_template.cpp"
#include "./rabitq_template.cpp"
#include "./template_half.cpp"
#endif /* COMPILER_SUPPORT_SSE */
