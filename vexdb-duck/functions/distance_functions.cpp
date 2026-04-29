#include "vex_functions.hpp"

#include "distance/distance.h"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

static unique_ptr<FunctionData> BindDistanceFunction(ClientContext &context, ScalarFunction &bound_function,
                                                     vector<unique_ptr<Expression>> &arguments) {
    if (arguments[0]->return_type.id() == LogicalTypeId::UNKNOWN &&
        arguments[1]->return_type.id() == LogicalTypeId::UNKNOWN) {
        throw ParameterNotResolvedException();
    }

    auto &primary = arguments[0]->return_type.id() != LogicalTypeId::UNKNOWN ? *arguments[0] : *arguments[1];
    auto resolved = ResolveToFloatArray(context, primary);
    bound_function.arguments[0] = resolved;
    bound_function.arguments[1] = resolved;
    return nullptr;
}

template <typename ComputeFn>
static void DistanceFunctionImpl(DataChunk &args, ExpressionState &state, Vector &result, ComputeFn compute) {
    (void)state;
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

    auto result_data = FlatVector::GetData<float>(result);

    vec_a.Flatten(count);
    vec_b.Flatten(count);
    auto &child_a = ArrayVector::GetEntry(vec_a);
    auto &child_b = ArrayVector::GetEntry(vec_b);
    child_a.Flatten(count * dim_a);
    child_b.Flatten(count * dim_b);
    auto data_a = FlatVector::GetData<float>(child_a);
    auto data_b = FlatVector::GetData<float>(child_b);
    auto &validity_a = FlatVector::Validity(vec_a);
    auto &validity_b = FlatVector::Validity(vec_b);

    auto dim = static_cast<uint16>(dim_a);
    for (idx_t i = 0; i < count; i++) {
        if (!validity_a.RowIsValid(i) || !validity_b.RowIsValid(i)) {
            FlatVector::SetNull(result, i, true);
            continue;
        }
        result_data[i] = compute(data_a + i * dim_a, data_b + i * dim_a, dim);
    }

    if (all_constant) {
        result.SetVectorType(VectorType::CONSTANT_VECTOR);
    }
}

static void L2DistanceFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    using L2Distancer =
        Distancer<Arch::GENERAL, Metric::L2, DistPrecisionType::FLOAT, RemainderSituation::Unknown, false>;
    DistanceFunctionImpl(args, state, result, [](const float *a, const float *b, uint16 d) {
        return L2Distancer::get_distance_single(a, b, d);
    });
}

static void VexTestVec3Function(DataChunk &args, ExpressionState &state, Vector &result) {
    (void)args;
    (void)state;
    std::vector<Value> values;
    values.emplace_back(Value::FLOAT(1.0f));
    values.emplace_back(Value::FLOAT(2.0f));
    values.emplace_back(Value::FLOAT(3.0f));
    result.SetValue(0, Value::ARRAY(LogicalType::FLOAT, std::move(values)));
    result.SetVectorType(VectorType::CONSTANT_VECTOR);
}

static void AddDistanceOverloads(ScalarFunctionSet &set, scalar_function_t func) {
    set.AddFunction(ScalarFunction({LogicalType::ANY, LogicalType::ANY}, LogicalType::FLOAT, func,
                                   BindDistanceFunction));
    set.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::FLOAT, func,
                                   BindDistanceFunction));
    set.AddFunction(ScalarFunction({LogicalType::ANY, LogicalType::VARCHAR}, LogicalType::FLOAT, func,
                                   BindDistanceFunction));
    set.AddFunction(ScalarFunction({LogicalType::VARCHAR, LogicalType::ANY}, LogicalType::FLOAT, func,
                                   BindDistanceFunction));
}

ScalarFunctionSet VexFunctions::GetL2DistanceFunction() {
    ScalarFunctionSet set("l2_distance");
    AddDistanceOverloads(set, L2DistanceFunction);
    return set;
}

ScalarFunctionSet VexFunctions::GetL2DistanceOperator() {
    ScalarFunctionSet set("<->");
    AddDistanceOverloads(set, L2DistanceFunction);
    return set;
}

ScalarFunctionSet VexFunctions::GetL2DistanceArrayAlias() {
    ScalarFunctionSet set("array_distance");
    AddDistanceOverloads(set, L2DistanceFunction);
    return set;
}

ScalarFunctionSet VexFunctions::GetL2DistanceListAlias() {
    ScalarFunctionSet set("list_distance");
    AddDistanceOverloads(set, L2DistanceFunction);
    return set;
}

void VexFunctions::Register(ExtensionLoader &loader) {
    loader.RegisterFunction(GetL2DistanceFunction());
    loader.RegisterFunction(GetL2DistanceOperator());
    loader.RegisterFunction(GetL2DistanceArrayAlias());
    loader.RegisterFunction(GetL2DistanceListAlias());
    loader.RegisterFunction(GetVectorDimsFunction());
    loader.RegisterFunction(ScalarFunction("vex_testvec3", {}, LogicalType::ARRAY(LogicalType::FLOAT, 3),
                                           VexTestVec3Function));
}

} // namespace duckdb
