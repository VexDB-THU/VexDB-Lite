#include "vex_functions.hpp"
#include "vex_distance.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"

#include <cmath>

namespace duckdb {

// Bind function: resolve parameters to matching FLOAT[N] ARRAY types
static unique_ptr<FunctionData> BindDistanceFunction(ClientContext &context, ScalarFunction &bound_function,
                                                     vector<unique_ptr<Expression>> &arguments) {
	if (arguments[0]->return_type.id() == LogicalTypeId::UNKNOWN &&
	    arguments[1]->return_type.id() == LogicalTypeId::UNKNOWN) {
		throw ParameterNotResolvedException();
	}

	// Resolve from whichever side has a known type (prefer left, fallback to right)
	auto &primary = arguments[0]->return_type.id() != LogicalTypeId::UNKNOWN ? *arguments[0] : *arguments[1];
	auto resolved = ResolveToFloatArray(context, primary);
	bound_function.arguments[0] = resolved;
	bound_function.arguments[1] = resolved;

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

static void InnerProductFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	DistanceFunctionImpl(args, state, result, [](const float *a, const float *b, uint32_t d) {
		return static_cast<double>(vex::InnerProductDistance(a, b, d));
	});
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

// Helper: add all overloads (ANY+ANY, VARCHAR combos) for a distance function
static void AddDistanceOverloads(ScalarFunctionSet &set, scalar_function_t func) {
	// Primary: ARRAY/LIST inputs (resolved by BindDistanceFunction)
	set.AddFunction(ScalarFunction({LogicalType::ANY, LogicalType::ANY}, LogicalType::DOUBLE, func,
	                               BindDistanceFunction));
	// VARCHAR overloads: STRING_LITERAL only matches concrete types, not ANY
	set.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::DOUBLE, func,
	                               BindDistanceFunction));
	set.AddFunction(ScalarFunction({LogicalType::ANY, LogicalType::VARCHAR}, LogicalType::DOUBLE, func,
	                               BindDistanceFunction));
	set.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::ANY}, LogicalType::DOUBLE, func,
	                               BindDistanceFunction));
}

ScalarFunctionSet VexFunctions::GetL2DistanceFunction() {
	ScalarFunctionSet set("l2_distance");
	AddDistanceOverloads(set, L2DistanceFunction);
	return set;
}

ScalarFunctionSet VexFunctions::GetInnerProductFunction() {
	ScalarFunctionSet set("inner_product");
	AddDistanceOverloads(set, InnerProductFunction);
	return set;
}

ScalarFunctionSet VexFunctions::GetNegativeInnerProductFunction() {
	ScalarFunctionSet set("<~>");
	AddDistanceOverloads(set, NegativeInnerProductFunction);
	return set;
}

ScalarFunctionSet VexFunctions::GetCosineDistanceFunction() {
	ScalarFunctionSet set("cosine_distance");
	AddDistanceOverloads(set, CosineDistanceFunction);
	return set;
}

} // namespace duckdb
