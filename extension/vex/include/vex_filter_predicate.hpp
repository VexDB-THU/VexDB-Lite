#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

namespace duckdb {
namespace vex {

// ============================================================
// Metadata Column Descriptor
// ============================================================
struct MetaColumnDesc {
	LogicalTypeId type_id;
	uint32_t offset; // byte offset within the metadata segment
	uint32_t size;   // byte size of this column's data

	static uint32_t GetTypeSize(LogicalTypeId type_id) {
		switch (type_id) {
		case LogicalTypeId::BOOLEAN:
		case LogicalTypeId::TINYINT:
		case LogicalTypeId::UTINYINT:
			return 1;
		case LogicalTypeId::SMALLINT:
		case LogicalTypeId::USMALLINT:
			return 2;
		case LogicalTypeId::INTEGER:
		case LogicalTypeId::UINTEGER:
		case LogicalTypeId::FLOAT:
			return 4;
		case LogicalTypeId::BIGINT:
		case LogicalTypeId::UBIGINT:
		case LogicalTypeId::DOUBLE:
			return 8;
		default:
			return 8; // default to 8 bytes for unknown types (store as raw bytes)
		}
	}
};

// ============================================================
// Filter Predicate — base class for filtered HNSW search
// ============================================================
class FilterPredicate {
public:
	virtual ~FilterPredicate() = default;

	//! Check if the metadata at the given pointer matches this predicate
	virtual bool Matches(const uint8_t *meta_data) const = 0;

	//! Estimated selectivity (fraction of rows that match), 1.0 = match all
	virtual double Selectivity() const { return 1.0; }
};

// ============================================================
// EqualityFilter — matches a single value
// ============================================================
class EqualityFilter : public FilterPredicate {
public:
	uint32_t offset;
	uint32_t size;
	std::vector<uint8_t> value;
	double selectivity_;

	EqualityFilter(uint32_t offset, uint32_t size, const void *val, double sel = 0.1)
	    : offset(offset), size(size), selectivity_(sel) {
		value.resize(size);
		std::memcpy(value.data(), val, size);
	}

	bool Matches(const uint8_t *meta_data) const override {
		return std::memcmp(meta_data + offset, value.data(), size) == 0;
	}

	double Selectivity() const override { return selectivity_; }
};

// ============================================================
// RangeFilter — matches values in [min, max] range
// ============================================================
class RangeFilter : public FilterPredicate {
public:
	uint32_t offset;
	uint32_t size;
	LogicalTypeId type_id;
	std::vector<uint8_t> min_val;
	std::vector<uint8_t> max_val;
	bool has_min;
	bool has_max;
	double selectivity_;

	RangeFilter(uint32_t offset, uint32_t size, LogicalTypeId type_id, double sel = 0.3)
	    : offset(offset), size(size), type_id(type_id), has_min(false), has_max(false), selectivity_(sel) {}

	void SetMin(const void *val) {
		min_val.resize(size);
		std::memcpy(min_val.data(), val, size);
		has_min = true;
	}

	void SetMax(const void *val) {
		max_val.resize(size);
		std::memcpy(max_val.data(), val, size);
		has_max = true;
	}

	bool Matches(const uint8_t *meta_data) const override {
		const uint8_t *data = meta_data + offset;
		if (has_min && CompareValues(data, min_val.data()) < 0) return false;
		if (has_max && CompareValues(data, max_val.data()) > 0) return false;
		return true;
	}

	double Selectivity() const override { return selectivity_; }

private:
	int CompareValues(const uint8_t *a, const uint8_t *b) const {
		switch (type_id) {
		case LogicalTypeId::INTEGER: {
			int32_t va, vb;
			std::memcpy(&va, a, 4);
			std::memcpy(&vb, b, 4);
			return (va < vb) ? -1 : (va > vb ? 1 : 0);
		}
		case LogicalTypeId::BIGINT: {
			int64_t va, vb;
			std::memcpy(&va, a, 8);
			std::memcpy(&vb, b, 8);
			return (va < vb) ? -1 : (va > vb ? 1 : 0);
		}
		case LogicalTypeId::FLOAT: {
			float va, vb;
			std::memcpy(&va, a, 4);
			std::memcpy(&vb, b, 4);
			return (va < vb) ? -1 : (va > vb ? 1 : 0);
		}
		case LogicalTypeId::DOUBLE: {
			double va, vb;
			std::memcpy(&va, a, 8);
			std::memcpy(&vb, b, 8);
			return (va < vb) ? -1 : (va > vb ? 1 : 0);
		}
		default:
			return std::memcmp(a, b, size);
		}
	}
};

// ============================================================
// InListFilter — matches any value in a set
// ============================================================
class InListFilter : public FilterPredicate {
public:
	uint32_t offset;
	uint32_t size;
	std::vector<std::vector<uint8_t>> values;
	double selectivity_;

	InListFilter(uint32_t offset, uint32_t size, double sel = 0.1)
	    : offset(offset), size(size), selectivity_(sel) {}

	void AddValue(const void *val) {
		std::vector<uint8_t> v(size);
		std::memcpy(v.data(), val, size);
		values.push_back(std::move(v));
	}

	bool Matches(const uint8_t *meta_data) const override {
		const uint8_t *data = meta_data + offset;
		for (auto &v : values) {
			if (std::memcmp(data, v.data(), size) == 0) return true;
		}
		return false;
	}

	double Selectivity() const override { return selectivity_; }
};

// ============================================================
// ConjunctionFilter — AND of multiple filters
// ============================================================
class ConjunctionFilter : public FilterPredicate {
public:
	std::vector<unique_ptr<FilterPredicate>> children;

	void AddChild(unique_ptr<FilterPredicate> child) {
		children.push_back(std::move(child));
	}

	bool Matches(const uint8_t *meta_data) const override {
		for (auto &child : children) {
			if (!child->Matches(meta_data)) return false;
		}
		return true;
	}

	double Selectivity() const override {
		double sel = 1.0;
		for (auto &child : children) {
			sel *= child->Selectivity();
		}
		return sel;
	}
};

} // namespace vex
} // namespace duckdb
