#ifndef BUFFER_BASE_H
#define BUFFER_BASE_H

#include "postgres.h"

/* VecStorageType is defined in pg_compat.h - do not redefine */
/* This file just provides additional buffer-related definitions */

struct VecStorageTypeInfo {
    uint8 ntype;
    uint8 type;
};

inline
#if __cplusplus >= 201703L
constexpr
#endif /* c++17 or greater */
VecStorageTypeInfo get_vec_storage_type_info(VecStorageType vec_storage_type) {
    if (vec_storage_type == VecStorageType::PureVec || vec_storage_type == VecStorageType::PureCode) {
        return {1, 0};
    } else if (vec_storage_type == VecStorageType::VecWithCode) {
        return {2, 0};
    } else if (vec_storage_type == VecStorageType::CodeWithVec) {
        return {2, 1};
    } else {
        return {0, 0};
    }
}

#endif /* BUFFER_BASE_H */
