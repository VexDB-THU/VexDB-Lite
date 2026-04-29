#pragma once

#include "duckdb/common/typedefs.hpp"

#include <cstdint>

namespace duckdb {
namespace vex_disk {

static constexpr uint32_t GRAPH_INDEX_DISK_MAGIC = 0x58444947; // "GIDX"
static constexpr uint32_t GRAPH_INDEX_DISK_VERSION = 1;

enum class SegmentKind : uint32_t {
    META = 1,
    BASE = 2,
    UPPER = 3,
    VEC = 4,
    ROWID = 5
};

struct DiskImageHeader {
    uint32_t magic = GRAPH_INDEX_DISK_MAGIC;
    uint32_t version = GRAPH_INDEX_DISK_VERSION;
    uint64_t total_size = 0;
    uint64_t meta_offset = 0;
    uint64_t meta_size = 0;
    uint64_t base_offset = 0;
    uint64_t base_size = 0;
    uint64_t upper_offset = 0;
    uint64_t upper_size = 0;
    uint64_t vec_offset = 0;
    uint64_t vec_size = 0;
    uint64_t rowid_offset = 0;
    uint64_t rowid_size = 0;
};

struct DiskMeta {
    uint32_t dimension = 0;
    uint32_t m = 0;
    uint32_t ef_construction = 0;
    uint32_t metric = 0;
    uint64_t vector_count = 0;
    uint64_t upper_count = 0;
    uint64_t entry_id = 0;
    uint64_t entry_cur_layer_idx = 0;
    int32_t entry_level = -1;
};

struct DiskBaseRecordHeader {
    uint32_t neighbor_count = 0;
};

struct DiskUpperRecordHeader {
    uint32_t neighbor_count = 0;
    uint32_t lower_layer_idx = 0;
    uint32_t id = 0;
};

struct DiskRowIdRecordHeader {
    uint32_t row_count = 0;
};

} // namespace vex_disk
} // namespace duckdb
