#ifndef VEX_TYPES_H
#define VEX_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t vex_row_id_t;
typedef uint32_t vex_node_id_t;
typedef uint64_t vex_idx_t;

typedef enum vex_type_id_t {
    VEX_TYPE_INVALID = 0,
    VEX_TYPE_BOOLEAN = 1,
    VEX_TYPE_INT8 = 2,
    VEX_TYPE_UINT8 = 3,
    VEX_TYPE_INT16 = 4,
    VEX_TYPE_UINT16 = 5,
    VEX_TYPE_INT32 = 6,
    VEX_TYPE_UINT32 = 7,
    VEX_TYPE_INT64 = 8,
    VEX_TYPE_UINT64 = 9,
    VEX_TYPE_FLOAT32 = 10,
    VEX_TYPE_FLOAT64 = 11
} vex_type_id_t;

#ifdef __cplusplus
}

#include <cstdint>

namespace vex {

using row_id_t = vex_row_id_t;
using node_id_t = vex_node_id_t;
using idx_t = vex_idx_t;

enum class TypeId : uint8_t {
    INVALID = static_cast<uint8_t>(VEX_TYPE_INVALID),
    BOOLEAN = static_cast<uint8_t>(VEX_TYPE_BOOLEAN),
    INT8 = static_cast<uint8_t>(VEX_TYPE_INT8),
    UINT8 = static_cast<uint8_t>(VEX_TYPE_UINT8),
    INT16 = static_cast<uint8_t>(VEX_TYPE_INT16),
    UINT16 = static_cast<uint8_t>(VEX_TYPE_UINT16),
    INT32 = static_cast<uint8_t>(VEX_TYPE_INT32),
    UINT32 = static_cast<uint8_t>(VEX_TYPE_UINT32),
    INT64 = static_cast<uint8_t>(VEX_TYPE_INT64),
    UINT64 = static_cast<uint8_t>(VEX_TYPE_UINT64),
    FLOAT32 = static_cast<uint8_t>(VEX_TYPE_FLOAT32),
    FLOAT64 = static_cast<uint8_t>(VEX_TYPE_FLOAT64)
};

constexpr inline TypeId ToTypeId(vex_type_id_t type) {
    return static_cast<TypeId>(type);
}

constexpr inline vex_type_id_t ToCTypeId(TypeId type) {
    return static_cast<vex_type_id_t>(type);
}

} // namespace vex
#endif

#endif // VEX_TYPES_H
