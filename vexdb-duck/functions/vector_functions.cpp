#include "vex_functions.hpp"
#include "vex_duckdb_compat.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

#include <cmath>

namespace duckdb {

LogicalType ResolveToFloatArray(ClientContext &context, Expression &expr) {
	auto &type = expr.return_type;
	if (type.id() == LogicalTypeId::ARRAY) {
		if (ArrayType::GetChildType(type).id() != LogicalTypeId::FLOAT) {
			return LogicalType::ARRAY(LogicalType::FLOAT, ArrayType::GetSize(type));
		}
		return type;
	}
	if (type.id() == LogicalTypeId::LIST) {
		if (!expr.IsFoldable()) {
			throw InvalidInputException("Vector functions require FLOAT[N] array inputs, got non-constant LIST");
		}
		auto val = ExpressionExecutor::EvaluateScalar(context, expr, false);
		if (val.IsNull()) {
			throw InvalidInputException("Vector functions do not accept NULL vector inputs");
		}
		auto &list_children = ListValue::GetChildren(val);
		if (list_children.empty()) {
			throw InvalidInputException("Vector functions require non-empty vector inputs");
		}
		return LogicalType::ARRAY(LogicalType::FLOAT, list_children.size());
	}
	// VARCHAR/STRING_LITERAL → FLOAT[N]: parse '[1.0, 2.0, 3.0]' string to determine dimension
	if (type.id() == LogicalTypeId::VARCHAR || type.id() == LogicalTypeId::STRING_LITERAL) {
		if (!expr.IsFoldable()) {
			throw InvalidInputException("Vector functions require FLOAT[N] array inputs, got non-constant VARCHAR");
		}
		auto val = ExpressionExecutor::EvaluateScalar(context, expr, false);
		if (val.IsNull()) {
			throw InvalidInputException("Vector functions do not accept NULL vector inputs");
		}
		auto str = StringValue::Get(val);
		if (str.size() < 3 || str.front() != '[' || str.back() != ']') {
			throw InvalidInputException("Vector string must be in format '[1.0, 2.0, ...]', got '%s'", str);
		}
		// Count commas between brackets to determine dimension
		idx_t dim = 1;
		for (idx_t i = 1; i + 1 < str.size(); i++) {
			if (str[i] == ',') {
				dim++;
			}
		}
		return LogicalType::ARRAY(LogicalType::FLOAT, dim);
	}
	throw InvalidInputException("Vector functions require FLOAT[N] array inputs, got %s", type.ToString());
}

// Bind helper: resolve first argument to FLOAT[N]
static unique_ptr<FunctionData> BindResolveInput(ClientContext &context, ScalarFunction &bound_function,
                                                  vector<unique_ptr<Expression>> &arguments) {
	if (arguments[0]->return_type.id() == LogicalTypeId::UNKNOWN) {
		throw ParameterNotResolvedException();
	}
	auto resolved = ResolveToFloatArray(context, *arguments[0]);
	bound_function.arguments[0] = resolved;
	return nullptr;
}

// Bind for unary functions that return array (l2_normalize)
static unique_ptr<FunctionData> BindUnaryArrayReturn(ClientContext &context, ScalarFunction &bound_function,
                                                     vector<unique_ptr<Expression>> &arguments) {
	BindResolveInput(context, bound_function, arguments);
	bound_function.return_type = bound_function.arguments[0];
	return nullptr;
}

// Bind for binary functions that return array (vector_add, vector_sub)
static unique_ptr<FunctionData> BindBinaryArrayReturn(ClientContext &context, ScalarFunction &bound_function,
                                                      vector<unique_ptr<Expression>> &arguments) {
	BindResolveInput(context, bound_function, arguments);
	bound_function.arguments[1] = bound_function.arguments[0];
	bound_function.return_type = bound_function.arguments[0];
	return nullptr;
}

// ============================================================
// vector_dims: return the dimension of a vector
// ============================================================
static void VectorDimsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &vec = args.data[0];
	auto count = args.size();
	auto dim = static_cast<int32_t>(ArrayType::GetSize(vec.GetType()));

	bool is_constant = vec.GetVectorType() == VectorType::CONSTANT_VECTOR;

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

	if (is_constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

ScalarFunctionSet VexFunctions::GetVectorDimsFunction() {
	ScalarFunctionSet set("vector_dims");
	set.AddFunction(ScalarFunction({LogicalType::ANY}, LogicalType::INTEGER, VectorDimsFunction, BindResolveInput));
	set.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::INTEGER, VectorDimsFunction, BindResolveInput));
	return set;
}

// ============================================================
// vector_norm: return the L2 norm of a vector
// ============================================================
static void VectorNormFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &vec = args.data[0];
	auto count = args.size();
	auto dim = ArrayType::GetSize(vec.GetType());

	bool is_constant = vec.GetVectorType() == VectorType::CONSTANT_VECTOR;

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

	if (is_constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

ScalarFunctionSet VexFunctions::GetVectorNormFunction() {
	ScalarFunctionSet set("vector_norm");
	set.AddFunction(ScalarFunction({LogicalType::ANY}, LogicalType::DOUBLE, VectorNormFunction, BindResolveInput));
	set.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::DOUBLE, VectorNormFunction, BindResolveInput));
	return set;
}

// ============================================================
// l2_normalize: return unit vector (v / ||v||)
// ============================================================
static void L2NormalizeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &vec = args.data[0];
	auto count = args.size();
	auto dim = ArrayType::GetSize(vec.GetType());

	bool is_constant = vec.GetVectorType() == VectorType::CONSTANT_VECTOR;

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

	if (is_constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

ScalarFunctionSet VexFunctions::GetL2NormalizeFunction() {
	ScalarFunctionSet set("l2_normalize");
	set.AddFunction(ScalarFunction({LogicalType::ANY}, LogicalType::ANY, L2NormalizeFunction, BindUnaryArrayReturn));
	set.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::ANY, L2NormalizeFunction, BindUnaryArrayReturn));
	return set;
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

	bool all_constant = vec_a.GetVectorType() == VectorType::CONSTANT_VECTOR &&
	                    vec_b.GetVectorType() == VectorType::CONSTANT_VECTOR;

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

	if (all_constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

ScalarFunctionSet VexFunctions::GetVectorAddFunction() {
	ScalarFunctionSet set("vector_add");
	set.AddFunction(ScalarFunction({LogicalType::ANY, LogicalType::ANY}, LogicalType::ANY, VectorAddFunction, BindBinaryArrayReturn));
	set.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::ANY}, LogicalType::ANY, VectorAddFunction, BindBinaryArrayReturn));
	set.AddFunction(ScalarFunction({LogicalType::ANY, LogicalType::VARCHAR}, LogicalType::ANY, VectorAddFunction, BindBinaryArrayReturn));
	set.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::ANY, VectorAddFunction, BindBinaryArrayReturn));
	return set;
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

	bool all_constant = vec_a.GetVectorType() == VectorType::CONSTANT_VECTOR &&
	                    vec_b.GetVectorType() == VectorType::CONSTANT_VECTOR;

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

	if (all_constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

ScalarFunctionSet VexFunctions::GetVectorSubFunction() {
	ScalarFunctionSet set("vector_sub");
	set.AddFunction(ScalarFunction({LogicalType::ANY, LogicalType::ANY}, LogicalType::ANY, VectorSubFunction, BindBinaryArrayReturn));
	set.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::ANY}, LogicalType::ANY, VectorSubFunction, BindBinaryArrayReturn));
	set.AddFunction(ScalarFunction({LogicalType::ANY, LogicalType::VARCHAR}, LogicalType::ANY, VectorSubFunction, BindBinaryArrayReturn));
	set.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::ANY, VectorSubFunction, BindBinaryArrayReturn));
	return set;
}

// ============================================================
// Register all functions
// ============================================================
void VexFunctions::Register(ExtensionLoader &loader) {
	// Distance functions
	loader.RegisterFunction(GetL2DistanceFunction());
	loader.RegisterFunction(GetL2DistanceOperator());         // <->
	loader.RegisterFunction(GetInnerProductFunction());
	loader.RegisterFunction(GetNegativeInnerProductFunction()); // <~>
	loader.RegisterFunction(GetCosineDistanceFunction());
	loader.RegisterFunction(GetCosineDistanceOperator());     // <=>

	// Vector utility functions
	loader.RegisterFunction(GetVectorDimsFunction());
	loader.RegisterFunction(GetVectorNormFunction());
	loader.RegisterFunction(GetL2NormalizeFunction());
	loader.RegisterFunction(GetVectorAddFunction());
	loader.RegisterFunction(GetVectorSubFunction());

	// Table functions
	RegisterANNSearchFunction(loader);
	RegisterIndexInfoFunction(loader);
}

} // namespace duckdb
