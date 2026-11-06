#ifndef VEXDB_SQLITE_BUILD_MEMORY_BUDGET_H
#define VEXDB_SQLITE_BUILD_MEMORY_BUDGET_H

#include <cstdint>
#include <limits>
#include <string_view>

namespace vexdb_sqlite {

inline bool ParseBuildMemoryLimit(std::string_view text, std::int64_t &out) {
    if (text.empty()) return false;
    std::int64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        const int digit = c - '0';
        if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

// Zero keeps the existing four-times-query-cache maintenance policy.
// An explicit budget affects construction only, never per-connection reads.
inline std::int64_t BuildMemoryBudget(std::int64_t query_limit,
                                     std::int64_t build_limit) {
    if (build_limit > 0) return build_limit;
    if (query_limit <= 0) return 0;
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    return query_limit > maximum / 4 ? maximum : query_limit * 4;
}

} // namespace vexdb_sqlite
#endif
