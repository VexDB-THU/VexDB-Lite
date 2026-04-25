#ifndef VEX_NODE_HPP
#define VEX_NODE_HPP

#include "vex/vex_types.h"

#include <cstdint>

namespace vex {

static constexpr int HNSW_MAX_UPPER_LEVELS = 8;
static constexpr node_id_t INVALID_NODE_ID = static_cast<node_id_t>(UINT32_MAX);

struct NodeHeader {
    row_id_t row_id;
    uint8_t level;
    uint8_t deleted;
    uint16_t level0_count;
    uint16_t extra_row_count;
    uint16_t reserved;
    uint32_t upper_offset;
    uint32_t metadata_offset;
};

static_assert(sizeof(NodeHeader) == 24, "NodeHeader must be 24 bytes");

} // namespace vex

#endif // VEX_NODE_HPP
