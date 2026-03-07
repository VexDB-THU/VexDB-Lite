#include "vex_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

#include <cmath>

namespace duckdb {

// Bind function: propagate input array type as return type (for single-arg functions)
static unique_ptr<FunctionData> BindUnaryArrayReturn(ClientContext &context, ScalarFunction &bound_function,
                                                     vector<unique_ptr<Expression>> &arguments) {
	if (arguments[0]->return_type.id() == LogicalTypeId::UNKNOWN) {
		throw ParameterNotResolvedException();
	}
	bound_function.return_type = arguments[0]->return_type;
	return nullptr;
}

// Bind function: propagate input array type as return type (for binary functions)
static unique_ptr<FunctionData> BindBinaryArrayReturn(ClientContext &context, ScalarFunction &bound_function,
                                                      vector<unique_ptr<Expression>> &arguments) {
	if (arguments[0]->return_type.id() == LogicalTypeId::UNKNOWN) {
		throw ParameterNotResolvedException();
	}
	bound_function.return_type = arguments[0]->return_type;
	bound_function.arguments[1] = arguments[1]->return_type;
	return nullptr;
}

// ============================================================
// vector_dims: return the dimension of a vector
// ============================================================
static void VectorDimsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &vec = args.data[0];
	auto count = args.size();
	auto dim = static_cast<int32_t>(ArrayType::GetSize(vec.GetType()));

	vec.Flatten(count);
	auto result_data = FlatVector::GetData<int32_t>(result);
	auto &result_validity = FlatVector::Validity(result);
	auto &validity = FlatVector::Validity(vec);

	for (idx_t i = 0; i < count; i++) {
		if (!validity.RowIsValid(i)) {
			result_validity.SetInvalid(i);
			continue;
		}
		result_data[i] = dim;
	}
}

ScalarFunction VexFunctions::GetVectorDimsFunction() {
	return ScalarFunction("vector_dims", {LogicalType::ANY}, LogicalType::INTEGER, VectorDimsFunction);
}

// ============================================================
// vector_norm: return the L2 norm of a vector
// ============================================================
static void VectorNormFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &vec = args.data[0];
	auto count = args.size();
	auto dim = ArrayType::GetSize(vec.GetType());

	auto result_data = FlatVector::GetData<double>(result);
	auto &result_validity = FlatVector::Validity(result);

	vec.Flatten(count);
	auto &child = ArrayVector::GetEntry(vec);
	auto data = FlatVector::GetData<float>(child);
	auto &validity = FlatVector::Validity(vec);

	for (idx_t i = 0; i < count; i++) {
		if (!validity.RowIsValid(i)) {
			result_validity.SetInvalid(i);
			continue;
		}
		const float *v = data + i * dim;
		double sum = 0.0;
		for (idx_t d = 0; d < dim; d++) {
			double val = static_cast<double>(v[d]);
			sum += val * val;
		}
		result_data[i] = std::sqrt(sum);
	}
}

ScalarFunction VexFunctions::GetVectorNormFunction() {
	return ScalarFunction("vector_norm", {LogicalType::ANY}, LogicalType::DOUBLE, VectorNormFunction);
}

// ============================================================
// l2_normalize: return unit vector (v / ||v||)
// ============================================================
static void L2NormalizeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &vec = args.data[0];
	auto count = args.size();
	auto dim = ArrayType::GetSize(vec.GetType());

	vec.Flatten(count);
	auto &child_in = ArrayVector::GetEntry(vec);
	auto data_in = FlatVector::GetData<float>(child_in);
	auto &validity = FlatVector::Validity(vec);

	auto &child_out = ArrayVector::GetEntry(result);
	auto data_out = FlatVector::GetData<float>(child_out);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		if (!validity.RowIsValid(i)) {
			result_validity.SetInvalid(i);
			continue;
		}
		const float *v = data_in + i * dim;
		float *out = data_out + i * dim;
		double norm = 0.0;
		for (idx_t d = 0; d < dim; d++) {
			double val = static_cast<double>(v[d]);
			norm += val * val;
		}
		norm = std::sqrt(norm);
		if (norm == 0.0) {
			for (idx_t d = 0; d < dim; d++) {
				out[d] = 0.0f;
			}
		} else {
			for (idx_t d = 0; d < dim; d++) {
				out[d] = static_cast<float>(static_cast<double>(v[d]) / norm);
			}
		}
	}
}

ScalarFunction VexFunctions::GetL2NormalizeFunction() {
	auto func = ScalarFunction("l2_normalize", {LogicalType::ANY}, LogicalType::ANY, L2NormalizeFunction, BindUnaryArrayReturn);
	return func;
}

// ============================================================
// vector_add: element-wise addition
// ============================================================
static void VectorAddFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &vec_a = args.data[0];
	auto &vec_b = args.data[1];
	auto count = args.size();
	auto dim_a = ArrayType::GetSize(vec_a.GetType());
	auto dim_b = ArrayType::GetSize(vec_b.GetType());
	if (dim_a != dim_b) {
		throw InvalidInputException("Vector dimension mismatch: %d vs %d", dim_a, dim_b);
	}

	vec_a.Flatten(count);
	vec_b.Flatten(count);
	auto &child_a = ArrayVector::GetEntry(vec_a);
	auto &child_b = ArrayVector::GetEntry(vec_b);
	auto data_a = FlatVector::GetData<float>(child_a);
	auto data_b = FlatVector::GetData<float>(child_b);
	auto &validity_a = FlatVector::Validity(vec_a);
	auto &validity_b = FlatVector::Validity(vec_b);

	auto &child_out = ArrayVector::GetEntry(result);
	auto data_out = FlatVector::GetData<float>(child_out);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		if (!validity_a.RowIsValid(i) || !validity_b.RowIsValid(i)) {
			result_validity.SetInvalid(i);
			continue;
		}
		const float *a = data_a + i * dim_a;
		const float *b = data_b + i * dim_a;
		float *out = data_out + i * dim_a;
		for (idx_t d = 0; d < dim_a; d++) {
			out[d] = a[d] + b[d];
		}
	}
}

ScalarFunction VexFunctions::GetVectorAddFunction() {
	return ScalarFunction("vector_add", {LogicalType::ANY, LogicalType::ANY}, LogicalType::ANY, VectorAddFunction, BindBinaryArrayReturn);
}

// ============================================================
// vector_sub: element-wise subtraction
// ============================================================
static void VectorSubFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &vec_a = args.data[0];
	auto &vec_b = args.data[1];
	auto count = args.size();
	auto dim_a = ArrayType::GetSize(vec_a.GetType());
	auto dim_b = ArrayType::GetSize(vec_b.GetType());
	if (dim_a != dim_b) {
		throw InvalidInputException("Vector dimension mismatch: %d vs %d", dim_a, dim_b);
	}

	vec_a.Flatten(count);
	vec_b.Flatten(count);
	auto &child_a = ArrayVector::GetEntry(vec_a);
	auto &child_b = ArrayVector::GetEntry(vec_b);
	auto data_a = FlatVector::GetData<float>(child_a);
	auto data_b = FlatVector::GetData<float>(child_b);
	auto &validity_a = FlatVector::Validity(vec_a);
	auto &validity_b = FlatVector::Validity(vec_b);

	auto &child_out = ArrayVector::GetEntry(result);
	auto data_out = FlatVector::GetData<float>(child_out);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		if (!validity_a.RowIsValid(i) || !validity_b.RowIsValid(i)) {
			result_validity.SetInvalid(i);
			continue;
		}
		const float *a = data_a + i * dim_a;
		const float *b = data_b + i * dim_a;
		float *out = data_out + i * dim_a;
		for (idx_t d = 0; d < dim_a; d++) {
			out[d] = a[d] - b[d];
		}
	}
}

ScalarFunction VexFunctions::GetVectorSubFunction() {
	return ScalarFunction("vector_sub", {LogicalType::ANY, LogicalType::ANY}, LogicalType::ANY, VectorSubFunction, BindBinaryArrayReturn);
}

// ============================================================
// Register all functions
// ============================================================
void VexFunctions::Register(ExtensionLoader &loader) {
	// Distance functions
	loader.RegisterFunction(GetL2DistanceFunction());
	loader.RegisterFunction(GetInnerProductFunction());
	loader.RegisterFunction(GetNegativeInnerProductFunction());
	loader.RegisterFunction(GetCosineDistanceFunction());

	// Vector utility functions
	loader.RegisterFunction(GetVectorDimsFunction());
	loader.RegisterFunction(GetVectorNormFunction());
	loader.RegisterFunction(GetL2NormalizeFunction());
	loader.RegisterFunction(GetVectorAddFunction());
	loader.RegisterFunction(GetVectorSubFunction());

	// Table functions
	RegisterANNSearchFunction(loader);
}

} // namespace duckdb
