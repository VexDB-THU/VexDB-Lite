#include "vex_functions.hpp"
#include "vex_distance.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"

#include <cmath>

namespace duckdb {

static void CheckDimensions(idx_t dim_a, idx_t dim_b) {
	if (dim_a != dim_b) {
		throw InvalidInputException("Vector dimension mismatch: %d vs %d", dim_a, dim_b);
	}
}

// ============================================================
// L2 Distance: sqrt(sum((a_i - b_i)^2)) — SIMD optimized
// ============================================================
static void L2DistanceFunction(DataChunk &args, ExpressionState &state, Vector &result) {
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
	auto &child_a = ArrayVector::GetEntry(vec_a);
	auto &child_b = ArrayVector::GetEntry(vec_b);
	auto data_a = FlatVector::GetData<float>(child_a);
	auto data_b = FlatVector::GetData<float>(child_b);
	auto &validity_a = FlatVector::Validity(vec_a);
	auto &validity_b = FlatVector::Validity(vec_b);

	auto dim = static_cast<uint32_t>(dim_a);
	for (idx_t i = 0; i < count; i++) {
		if (!validity_a.RowIsValid(i) || !validity_b.RowIsValid(i)) {
			result_validity.SetInvalid(i);
			continue;
		}
		const float *a = data_a + i * dim_a;
		const float *b = data_b + i * dim_a;
		float l2sqr = vex::L2SqrDistance(a, b, dim);
		result_data[i] = std::sqrt(static_cast<double>(l2sqr));
	}
}

ScalarFunctionSet VexFunctions::GetL2DistanceFunction() {
	ScalarFunctionSet set("l2_distance");
	set.AddFunction(ScalarFunction({LogicalType::ANY, LogicalType::ANY}, LogicalType::DOUBLE, L2DistanceFunction,
	                               nullptr, nullptr, nullptr, nullptr, LogicalType::ANY));
	return set;
}

// ============================================================
// Inner Product: sum(a_i * b_i) — SIMD optimized
// ============================================================
static void InnerProductFunction(DataChunk &args, ExpressionState &state, Vector &result) {
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
	auto &child_a = ArrayVector::GetEntry(vec_a);
	auto &child_b = ArrayVector::GetEntry(vec_b);
	auto data_a = FlatVector::GetData<float>(child_a);
	auto data_b = FlatVector::GetData<float>(child_b);
	auto &validity_a = FlatVector::Validity(vec_a);
	auto &validity_b = FlatVector::Validity(vec_b);

	auto dim = static_cast<uint32_t>(dim_a);
	for (idx_t i = 0; i < count; i++) {
		if (!validity_a.RowIsValid(i) || !validity_b.RowIsValid(i)) {
			result_validity.SetInvalid(i);
			continue;
		}
		const float *a = data_a + i * dim_a;
		const float *b = data_b + i * dim_a;
		result_data[i] = static_cast<double>(vex::InnerProductDistance(a, b, dim));
	}
}

ScalarFunctionSet VexFunctions::GetInnerProductFunction() {
	ScalarFunctionSet set("inner_product");
	set.AddFunction(ScalarFunction({LogicalType::ANY, LogicalType::ANY}, LogicalType::DOUBLE, InnerProductFunction,
	                               nullptr, nullptr, nullptr, nullptr, LogicalType::ANY));
	return set;
}

// ============================================================
// Cosine Distance: 1 - (dot(a,b) / (norm(a) * norm(b))) — SIMD optimized
// ============================================================
static void CosineDistanceFunction(DataChunk &args, ExpressionState &state, Vector &result) {
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
	auto &child_a = ArrayVector::GetEntry(vec_a);
	auto &child_b = ArrayVector::GetEntry(vec_b);
	auto data_a = FlatVector::GetData<float>(child_a);
	auto data_b = FlatVector::GetData<float>(child_b);
	auto &validity_a = FlatVector::Validity(vec_a);
	auto &validity_b = FlatVector::Validity(vec_b);

	auto dim = static_cast<uint32_t>(dim_a);
	for (idx_t i = 0; i < count; i++) {
		if (!validity_a.RowIsValid(i) || !validity_b.RowIsValid(i)) {
			result_validity.SetInvalid(i);
			continue;
		}
		const float *a = data_a + i * dim_a;
		const float *b = data_b + i * dim_a;
		result_data[i] = static_cast<double>(vex::CosineDistance(a, b, dim));
	}
}

// ============================================================
// Negative Inner Product: -sum(a_i * b_i) — for ORDER BY <#> usage
// ============================================================
static void NegativeInnerProductFunction(DataChunk &args, ExpressionState &state, Vector &result) {
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
	auto &child_a = ArrayVector::GetEntry(vec_a);
	auto &child_b = ArrayVector::GetEntry(vec_b);
	auto data_a = FlatVector::GetData<float>(child_a);
	auto data_b = FlatVector::GetData<float>(child_b);
	auto &validity_a = FlatVector::Validity(vec_a);
	auto &validity_b = FlatVector::Validity(vec_b);

	auto dim = static_cast<uint32_t>(dim_a);
	for (idx_t i = 0; i < count; i++) {
		if (!validity_a.RowIsValid(i) || !validity_b.RowIsValid(i)) {
			result_validity.SetInvalid(i);
			continue;
		}
		const float *a = data_a + i * dim_a;
		const float *b = data_b + i * dim_a;
		result_data[i] = -static_cast<double>(vex::InnerProductDistance(a, b, dim));
	}
}

ScalarFunctionSet VexFunctions::GetNegativeInnerProductFunction() {
	ScalarFunctionSet set("<#>");
	set.AddFunction(ScalarFunction({LogicalType::ANY, LogicalType::ANY}, LogicalType::DOUBLE, NegativeInnerProductFunction,
	                               nullptr, nullptr, nullptr, nullptr, LogicalType::ANY));
	return set;
}

ScalarFunctionSet VexFunctions::GetCosineDistanceFunction() {
	ScalarFunctionSet set("cosine_distance");
	set.AddFunction(ScalarFunction({LogicalType::ANY, LogicalType::ANY}, LogicalType::DOUBLE, CosineDistanceFunction,
	                               nullptr, nullptr, nullptr, nullptr, LogicalType::ANY));
	return set;
}

} // namespace duckdb
