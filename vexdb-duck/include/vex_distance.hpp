#pragma once

#include "duckdb.hpp"

namespace duckdb {

enum class VexMetric : uint8_t {
    L2 = 0,
    INNER_PRODUCT = 1,
    COSINE = 2
};

VexMetric ParseMetric(const string &metric_name);

} // namespace duckdb
