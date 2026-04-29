/**
 * Copyright (c) 2021-2024 Huawei Technologies Co., Ltd.
 * Bytes to size string helper.
 * Adapted for PostgreSQL from openGauss src/include/access/annvector/module/size_format.h
 */

#ifndef PG_VEXDB_SIZE_FORMAT_H
#define PG_VEXDB_SIZE_FORMAT_H

#include "c.h"

namespace ann_helper {
namespace internal {
    constexpr size_t byte_to_kb = 1024ul;
    constexpr size_t kb_to_mb = 1024ul;
    constexpr size_t mb_to_gb = 1024ul;
    constexpr size_t gb_to_tb = 1024ul;
    constexpr size_t byte_to_mb = byte_to_kb * kb_to_mb;
    constexpr size_t byte_to_gb = byte_to_mb * mb_to_gb;
    constexpr size_t byte_to_tb = byte_to_gb * gb_to_tb;
    constexpr size_t kb_to_gb = kb_to_mb * mb_to_gb;
    constexpr size_t kb_to_tb = kb_to_gb * gb_to_tb;
    constexpr size_t mb_to_tb = mb_to_gb * gb_to_tb;
}

enum class SizeUnit {
    B, KB, MB, GB, TB
};

inline const char *unit_to_str(SizeUnit unit)
{
    switch (unit) {
        case SizeUnit::B:
            return "bytes";
        case SizeUnit::KB:
            return "KB";
        case SizeUnit::MB:
            return "MB";
        case SizeUnit::GB:
            return "GB";
        case SizeUnit::TB:
            return "TB";
    }
    return "UNKNOWN";
}

struct SizeFormatContent {
    double n;
    SizeUnit unit;
    const char *unit_str() const { return unit_to_str(unit); }
};

inline SizeFormatContent format_size(size_t bytes)
{
    constexpr double unit_base_min = 10.0;
    double n = bytes;
    if (n <= unit_base_min * internal::byte_to_kb) {
        return {n, SizeUnit::B};
    }
    n /= internal::byte_to_kb;
    if (n <= unit_base_min * internal::kb_to_mb) {
        return {n, SizeUnit::KB};
    }
    n /= internal::kb_to_mb;
    if (n <= unit_base_min * internal::mb_to_gb) {
        return {n, SizeUnit::MB};
    }
    n /= internal::mb_to_gb;
    if (n <= unit_base_min * internal::gb_to_tb) {
        return {n, SizeUnit::GB};
    }
    return {n / internal::gb_to_tb, SizeUnit::TB};
}
}

#endif /* PG_VEXDB_SIZE_FORMAT_H */
