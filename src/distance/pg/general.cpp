#include "distance/pg/distance_utils.h"
#include "distance/pg/cblas_interface.h"

#define DISTANCE_FUNC_NAME(name)  GENERAL_FUNC(name)
#define DISTANCE_STRUCT_NAME(name) GENERAL_STRUCT(name)
#include "./template_half.cpp"
#include "../core/distances_simd_template.cpp"
#include "../core/code_distance_template.cpp"
#include "./rabitq_template.cpp"
