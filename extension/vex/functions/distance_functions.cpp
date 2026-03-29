#include "vex_functions.hpp"
#include "vex_distance.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"

#include <cmath>

namespace duckdb {

// Bind function for distance functions: resolve ANY parameters to matching ARRAY types
static unique_ptr<FunctionData> BindDistanceFunction(ClientContext &context, ScalarFunction &bound_function,
                                                     vector<unique_ptr<Expression>> &arguments) {
	auto &left_type = arguments[0]->return_type;
	auto &right_type = arguments[1]->return_type;

	// If left is unknown (e.g. prepared statement parameter), try to resolve from right
	if (left_type.id() == LogicalTypeId::UNKNOWN && right_type.id() == LogicalTypeId::UNKNOWN) {
		throw ParameterNotResolvedException();
	}

	// Resolve types: use the known ARRAY type to cast the other argument
	LogicalType resolved_type;
	if (left_type.id() == LogicalTypeId::ARRAY) {
		resolved_type = left_type;
	} else if (right_type.id() == LogicalTypeId::ARRAY) {
		resolved_type = right_type;
	} else {
		// Neither side is ARRAY (e.g. two uncast LIST literals) — let DuckDB handle default casting
		return nullptr;
	}
	bound_function.arguments[0] = resolved_type;
	bound_function.arguments[1] = resolved_type;

	return nullptr;
}

static void CheckDimensions(idx_t dim_a, idx_t dim_b) {
	if (dim_a != dim_b) {
		throw InvalidInputException("Vector dimension mismatch: %d vs %d", dim_a, dim_b);
	}
}

// ============================================================
// Common distance function template — all distance functions share
// the same flatten/validate/iterate logic, only the final compute differs
// ============================================================
template <typename ComputeFn>
static void DistanceFunctionImpl(DataChunk &args, ExpressionState &state, Vector &result, ComputeFn compute) {
	auto &vec_a = args.data[0];
	auto &vec_b = args.data[1];
	auto count = args.size();

	auto dim_a = ArrayType::GetSize(vec_a.GetType());
	auto dim_b = ArrayType::GetSize(vec_b.GetType());
	CheckDimensions(dim_a, dim_b);

	auto result_data = FlatVector::GetData<double>(result);
	auto &result_validity = FlatVector::Validity(result);

	vec_a.Flatten(count);
	vec_b.Flatten(count);
	auto data_a = FlatVector::GetData<float>(ArrayVector::GetEntry(vec_a));
	auto data_b = FlatVector::GetData<float>(ArrayVector::GetEntry(vec_b));
	auto &validity_a = FlatVector::Validity(vec_a);
	auto &validity_b = FlatVector::Validity(vec_b);

	auto dim = static_cast<uint32_t>(dim_a);
	for (idx_t i = 0; i < count; i++) {
		if (!validity_a.RowIsValid(i) || !validity_b.RowIsValid(i)) {
			result_validity.SetInvalid(i);
			continue;
		}
		result_data[i] = compute(data_a + i * dim_a, data_b + i * dim_a, dim);
	}
}

static void L2DistanceFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	DistanceFunctionImpl(args, state, result, [](const float *a, const float *b, uint32_t d) {
		return std::sqrt(static_cast<double>(vex::L2SqrDistance(a, b, d)));
	});
}

ScalarFunctionSet VexFunctions::GetL2DistanceFunction() {
	ScalarFunctionSet set("l2_distance");
	set.AddFunction(ScalarFunction({LogicalType::ANY, LogicalType::ANY}, LogicalType::DOUBLE, L2DistanceFunction,
	                               BindDistanceFunction));
	return set;
}

static void InnerProductFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	DistanceFunctionImpl(args, state, result, [](const float *a, const float *b, uint32_t d) {
		return static_cast<double>(vex::InnerProductDistance(a, b, d));
	});
}

ScalarFunctionSet VexFunctions::GetInnerProductFunction() {
	ScalarFunctionSet set("inner_product");
	set.AddFunction(ScalarFunction({LogicalType::ANY, LogicalType::ANY}, LogicalType::DOUBLE, InnerProductFunction,
	                               BindDistanceFunction));
	return set;
}

static void CosineDistanceFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	DistanceFunctionImpl(args, state, result, [](const float *a, const float *b, uint32_t d) {
		return static_cast<double>(vex::CosineDistance(a, b, d));
	});
}

static void NegativeInnerProductFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	DistanceFunctionImpl(args, state, result, [](const float *a, const float *b, uint32_t d) {
		return -static_cast<double>(vex::InnerProductDistance(a, b, d));
	});
}

ScalarFunctionSet VexFunctions::GetNegativeInnerProductFunction() {
	ScalarFunctionSet set("<~>");
	set.AddFunction(ScalarFunction({LogicalType::ANY, LogicalType::ANY}, LogicalType::DOUBLE, NegativeInnerProductFunction,
	                               BindDistanceFunction));
	return set;
}

ScalarFunctionSet VexFunctions::GetCosineDistanceFunction() {
	ScalarFunctionSet set("cosine_distance");
	set.AddFunction(ScalarFunction({LogicalType::ANY, LogicalType::ANY}, LogicalType::DOUBLE, CosineDistanceFunction,
	                               BindDistanceFunction));
	return set;
}

} // namespace duckdb
