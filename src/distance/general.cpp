#include "distance/distance_utils.h"
#include "distance/cblas_interface.h"

#define DISTANCE_FUNC_NAME(name)  GENERAL_FUNC(name)
#define DISTANCE_STRUCT_NAME(name) GENERAL_STRUCT(name)
#include "./template_half.cpp"
#include "./distances_simd_template.cpp"
#include "./code_distance_template.cpp"
#include "./rabitq_template.cpp"
