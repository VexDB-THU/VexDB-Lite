#include "distance/core/architecture_macro.h"

#if COMPILER_SUPPORT_AVX
#include "distance/core/distance_utils_core.h"
#include "distance/core/transform_template_core.h"
#include "distance/cblas_interface.h"

#define DISTANCE_FUNC_NAME(name)  AVX_FUNC(name)
#define DISTANCE_STRUCT_NAME(name) AVX_STRUCT(name)
#define __SSE_SUPPORT__
#define __AVX_SUPPORT__
#include "../../../common/distance/src/distances_simd_template.cpp"
#include "../../../common/distance/src/code_distance_template.cpp"
#include "../../../common/distance/src/rabitq_template.cpp"
#endif /* COMPILER_SUPPORT_AVX */
