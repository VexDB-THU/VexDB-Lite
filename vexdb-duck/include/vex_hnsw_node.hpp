#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/execution/index/index_pointer.hpp"

#include <cstdint>
#include <cstring>

namespace duckdb {
namespace vex {

static constexpr int HNSW_MAX_UPPER_LEVELS = 8;

enum class HNSWAllocType : uint8_t {
	NODE = 0,
	VECTOR = 1,
	UPPER = 2,
	COUNT = 3
};

static constexpr idx_t HNSW_ALLOCATOR_COUNT = 3;

template <typename T>
struct HNSWNodeHeader {
	row_t row_id;
	uint8_t level;
	uint8_t deleted;
	uint16_t level0_count;
	uint16_t extra_row_count;
	uint16_t reserved;
	IndexPointer vector_ptr;
	IndexPointer upper_ptr;

	T *GetLevel0Neighbors() {
		return reinterpret_cast<T *>(reinterpret_cast<char *>(this) + sizeof(HNSWNodeHeader<T>));
	}
	const T *GetLevel0Neighbors() const {
		return reinterpret_cast<const T *>(reinterpret_cast<const char *>(this) + sizeof(HNSWNodeHeader<T>));
	}

	static idx_t SegmentSize(int m) {
		return sizeof(HNSWNodeHeader<T>) + static_cast<idx_t>(m) * 2 * sizeof(T);
	}
};

template <typename T>
struct HNSWUpperLevel {
	uint16_t counts[HNSW_MAX_UPPER_LEVELS];
	T lower_layer_idx;
	T id;

	T *GetNeighbors(int upper_level_idx, int m) {
		auto *base = reinterpret_cast<T *>(reinterpret_cast<char *>(this) + sizeof(HNSWUpperLevel<T>));
		return base + upper_level_idx * m;
	}
	const T *GetNeighbors(int upper_level_idx, int m) const {
		auto *base = reinterpret_cast<const T *>(reinterpret_cast<const char *>(this) + sizeof(HNSWUpperLevel<T>));
		return base + upper_level_idx * m;
	}

	static idx_t SegmentSize(int m) {
		return sizeof(HNSWUpperLevel<T>) +
		       static_cast<idx_t>(HNSW_MAX_UPPER_LEVELS) * static_cast<idx_t>(m) * sizeof(T);
	}
};

} // namespace vex
} // namespace duckdb
