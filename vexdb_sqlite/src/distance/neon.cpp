#include "distance/core/architecture_macro.h"

#if COMPILER_SUPPORT_NEONV8
#include "distance/core/distance_utils_core.h"
#include "distance/core/transform_template_core.h"

#define DISTANCE_FUNC_NAME(name) NEONV8_FUNC(name)
#define __NEON_SUPPORT__
#include "../../../common/distance/src/distances_simd_template.cpp"
#include "../../../common/distance/src/code_distance_template.cpp"
#include "../../../common/distance/src/rabitq_template.cpp"
#endif /* COMPILER_SUPPORT_NEONV8 */
