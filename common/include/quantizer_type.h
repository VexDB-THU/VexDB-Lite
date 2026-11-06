// 宿主无关的量化器类型枚举。
//
// PostgreSQL、DuckDB 与 SQLite 共用这一份定义，避免宿主适配层各自复制后漂移。
#ifndef VEX_COMMON_QUANTIZER_TYPE_H
#define VEX_COMMON_QUANTIZER_TYPE_H

#include <cstdint>

enum class QuantizerType : uint8_t {
    NONE = 0,
    PQ = 1,
    RABITQ = 2
};

#endif  // VEX_COMMON_QUANTIZER_TYPE_H
