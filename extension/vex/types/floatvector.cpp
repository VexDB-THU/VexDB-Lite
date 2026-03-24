#include "vex_types.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"

namespace duckdb {

static LogicalType BindFloatVectorType(BindLogicalTypeInput &input) {
	auto &modifiers = input.modifiers;
	if (modifiers.size() != 1) {
		throw BinderException("FLOATVECTOR requires exactly one dimension argument, e.g. FLOATVECTOR(128)");
	}

	auto dim_val = modifiers[0].GetValue();
	if (!dim_val.DefaultTryCastAs(LogicalTypeId::BIGINT)) {
		throw BinderException("FLOATVECTOR dimension must be an integer");
	}
	auto dim_value = dim_val.GetValueUnsafe<int64_t>();
	if (dim_value < 1 || static_cast<idx_t>(dim_value) > VexTypes::FLOATVECTOR_MAX_DIM) {
		throw BinderException("FLOATVECTOR dimension must be between 1 and %d, got %lld",
		                      VexTypes::FLOATVECTOR_MAX_DIM, dim_value);
	}

	auto dim = static_cast<idx_t>(dim_value);
	return LogicalType::ARRAY(LogicalType::FLOAT, dim);
}

void VexTypes::Register(ExtensionLoader &loader) {
	// Use dimension 1 as placeholder; actual dimension is resolved by BindFloatVectorType
	auto base_type = LogicalType::ARRAY(LogicalType::FLOAT, 1);
	base_type.SetAlias(FLOATVECTOR_TYPE_NAME);
	loader.RegisterType(FLOATVECTOR_TYPE_NAME, std::move(base_type), BindFloatVectorType);
}

// C++11/14 out-of-line definitions for static constexpr members
constexpr const char *VexTypes::FLOATVECTOR_TYPE_NAME;
constexpr idx_t VexTypes::FLOATVECTOR_MAX_DIM;

} // namespace duckdb
