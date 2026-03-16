#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/execution/index/index_pointer.hpp"

#include <cstdint>
#include <cstring>

namespace duckdb {
namespace vex {

// Maximum number of upper levels supported (above level 0)
static constexpr int HNSW_MAX_UPPER_LEVELS = 8;

// ============================================================
// Allocator indices
// ============================================================
enum class HNSWAllocType : uint8_t {
	NODE = 0,    // Node header + level-0 neighbors
	VECTOR = 1,  // Vector float data
	UPPER = 2,   // Upper-level neighbors (level 1+)
	COUNT = 3
};

static constexpr idx_t HNSW_ALLOCATOR_COUNT = 3;

// ============================================================
// Allocator 0: Node header segment
// Layout: [HNSWNodeHeader][IndexPointer level0_neighbors[M*2]]
// ============================================================
struct HNSWNodeHeader {
	row_t row_id;              // 8B - primary row_id (extra row_ids stored in dedup_map)
	uint8_t level;             // 1B
	uint8_t deleted;           // 1B
	uint16_t level0_count;     // 2B - active level-0 neighbor count
	uint16_t extra_row_count;  // 2B - number of extra deduplicated row_ids (0 = no dedup)
	uint16_t reserved;         // 2B - alignment padding
	IndexPointer vector_ptr;   // 8B - points to VectorSegment
	IndexPointer upper_ptr;    // 8B - points to UpperLevelSegment (null if level==0)
	// Total fixed part: 32B
	// Followed by: IndexPointer level0_neighbors[M*2]

	IndexPointer *GetLevel0Neighbors() {
		return reinterpret_cast<IndexPointer *>(reinterpret_cast<char *>(this) + sizeof(HNSWNodeHeader));
	}
	const IndexPointer *GetLevel0Neighbors() const {
		return reinterpret_cast<const IndexPointer *>(reinterpret_cast<const char *>(this) + sizeof(HNSWNodeHeader));
	}

	static idx_t SegmentSize(int m) {
		return sizeof(HNSWNodeHeader) + static_cast<idx_t>(m) * 2 * sizeof(IndexPointer);
	}
};

static_assert(sizeof(HNSWNodeHeader) == 32, "HNSWNodeHeader must be 32 bytes");

// ============================================================
// Allocator 1: Vector data segment
// Layout: float data[dim]
// Segment size = dim * sizeof(float)
// ============================================================
// No struct needed - just raw float array

// ============================================================
// Allocator 2: Upper-level neighbor segment (for nodes with level > 0)
// Layout: [HNSWUpperLevel][IndexPointer neighbors[HNSW_MAX_UPPER_LEVELS * M]]
// ============================================================
struct HNSWUpperLevel {
	uint16_t counts[HNSW_MAX_UPPER_LEVELS]; // neighbor count per upper level (index 0 = level 1)
	// Total fixed part: HNSW_MAX_UPPER_LEVELS * 2B = 16B
	// Followed by: IndexPointer neighbors[HNSW_MAX_UPPER_LEVELS * M]
	// Layout: level1_neighbors[M], level2_neighbors[M], ...

	IndexPointer *GetNeighbors(int upper_level_idx, int m) {
		auto *base = reinterpret_cast<IndexPointer *>(reinterpret_cast<char *>(this) + sizeof(HNSWUpperLevel));
		return base + upper_level_idx * m;
	}
	const IndexPointer *GetNeighbors(int upper_level_idx, int m) const {
		auto *base = reinterpret_cast<const IndexPointer *>(
		    reinterpret_cast<const char *>(this) + sizeof(HNSWUpperLevel));
		return base + upper_level_idx * m;
	}

	static idx_t SegmentSize(int m) {
		return sizeof(HNSWUpperLevel) +
		       static_cast<idx_t>(HNSW_MAX_UPPER_LEVELS) * static_cast<idx_t>(m) * sizeof(IndexPointer);
	}
};

} // namespace vex
} // namespace duckdb
