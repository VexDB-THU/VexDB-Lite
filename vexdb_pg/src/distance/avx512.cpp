#include "distance/core/architecture_macro.h"

#if COMPILER_SUPPORT_AVX512_EXTEND
#include "distance/core/distance_utils_core.h"
#include "distance/core/transform_template_core.h"
#include "distance/cblas_interface.h"

#define DISTANCE_FUNC_NAME(name)  AVX512_FUNC(name)
#define DISTANCE_STRUCT_NAME(name) AVX512_STRUCT(name)
#define __SSE_SUPPORT__
#define __AVX_SUPPORT__
#define __AVX512_SUPPORT__
#include "../../../common/distance/src/distances_simd_template.cpp"
#include "../../../common/distance/src/code_distance_template.cpp"
#include "../../../common/distance/src/rabitq_template.cpp"
#endif /* COMPILER_SUPPORT_AVX512_EXTEND */
