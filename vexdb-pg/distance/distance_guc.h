/**
 * Copyright ...
 */

#ifndef DISTANCE_GUC_H
#define DISTANCE_GUC_H

#include "utils/guc.h"

#ifdef __cplusplus
extern "C" {
#endif

extern bool check_vec_arch_str(char **newval, void **extra, GucSource source);
extern void assign_vec_arch(const char *newval, void *extra);

#ifdef __cplusplus
}
#endif

#endif /* DISTANCE_GUC_H */
