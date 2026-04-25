#ifndef VEX_CONFIG_HPP
#define VEX_CONFIG_HPP

#include "vex/vex_node.hpp"

#include <algorithm>
#include <cmath>

namespace vex {

struct GraphIndexConfig {
    static constexpr int DEFAULT_M = 16;
    static constexpr int MIN_M = 2;
    static constexpr int MAX_M = 128;

    static constexpr int DEFAULT_EF_CONSTRUCTION = 64;
    static constexpr int MIN_EF_CONSTRUCTION = 1;
    static constexpr int MAX_EF_CONSTRUCTION = 10000;

    static constexpr int DEFAULT_EF_SEARCH = 40;
    static constexpr int MIN_EF_SEARCH = 1;
    static constexpr int MAX_EF_SEARCH = 10000;

    int m = DEFAULT_M;
    int ef_construction = DEFAULT_EF_CONSTRUCTION;

    static inline double GetMl(int m_value) {
        return 1.0 / std::log(static_cast<double>(std::max(m_value, MIN_M)));
    }

    static constexpr int GetMaxLevel(int) {
        return HNSW_MAX_UPPER_LEVELS;
    }

    static constexpr int GetLayerM(int m_value, int level) {
        return level == 0 ? m_value * 2 : m_value;
    }
};

} // namespace vex

#endif // VEX_CONFIG_HPP
