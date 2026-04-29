#pragma once

#include "duckdb.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#define VEXDB_DUCK_ASSERT(cond)                                                                                         \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            throw duckdb::InternalException("VEXDB-DUCK assertion failed: %s", #cond);                                \
        }                                                                                                              \
    } while (0)
