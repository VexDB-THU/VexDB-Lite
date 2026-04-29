#include "vex_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar_function.hpp"

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
        auto &children = ListValue::GetChildren(val);
        if (children.empty()) {
            throw InvalidInputException("Vector functions require non-empty vector inputs");
        }
        return LogicalType::ARRAY(LogicalType::FLOAT, children.size());
    }
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

static unique_ptr<FunctionData> BindResolveInput(ClientContext &context, ScalarFunction &bound_function,
                                                 vector<unique_ptr<Expression>> &arguments) {
    if (arguments[0]->return_type.id() == LogicalTypeId::UNKNOWN) {
        throw ParameterNotResolvedException();
    }
    auto resolved = ResolveToFloatArray(context, *arguments[0]);
    bound_function.arguments[0] = resolved;
    return nullptr;
}

static void VectorDimsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    (void)state;
    auto &vec = args.data[0];
    auto count = args.size();
    auto dim = static_cast<int32_t>(ArrayType::GetSize(vec.GetType()));

    bool is_constant = vec.GetVectorType() == VectorType::CONSTANT_VECTOR;
    vec.Flatten(count);
    auto result_data = FlatVector::GetData<int32_t>(result);
    auto &validity = FlatVector::Validity(vec);

    for (idx_t i = 0; i < count; i++) {
        if (!validity.RowIsValid(i)) {
            FlatVector::SetNull(result, i, true);
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

} // namespace duckdb
