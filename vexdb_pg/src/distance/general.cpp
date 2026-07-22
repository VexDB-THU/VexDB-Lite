#include "distance/core/distance_utils_core.h"
#include "distance/core/transform_template_core.h"
#include "distance/cblas_interface.h"

#define DISTANCE_FUNC_NAME(name)  GENERAL_FUNC(name)
#define DISTANCE_STRUCT_NAME(name) GENERAL_STRUCT(name)
#include "../../../common/distance/src/distances_simd_template.cpp"
#include "../../../common/distance/src/code_distance_template.cpp"
#include "../../../common/distance/src/rabitq_template.cpp"
