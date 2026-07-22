// SQLite-side GENERAL fallback instantiation of distance SIMD kernels.
// (sse.cpp / avx.cpp / avx512.cpp / neon.cpp follow the same pattern with
// their respective DISTANCE_FUNC_NAME prefix and arch guard.)
#include "distance/core/architecture_macro.h"
#include "distance/core/distance_utils_core.h"
#include "distance/core/transform_template_core.h"

#define DISTANCE_FUNC_NAME(name) GENERAL_FUNC(name)
#include "../../../common/distance/src/distances_simd_template.cpp"
#include "../../../common/distance/src/code_distance_template.cpp"
#include "../../../common/distance/src/rabitq_template.cpp"
