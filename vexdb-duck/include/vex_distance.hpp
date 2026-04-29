#pragma once

#include "duckdb.hpp"

namespace duckdb {

enum class VexMetric : uint8_t {
    L2 = 0
};

VexMetric ParseMetric(const string &metric_name);

} // namespace duckdb
