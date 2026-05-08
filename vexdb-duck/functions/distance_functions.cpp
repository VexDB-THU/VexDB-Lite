#include "vex_functions.hpp"

#include "distance/core/distance.h"
#include "distance/core/distance_dispatcher.h"

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

// The shared distance core has `ann_helper::get_general_distance_func(Metric)` but
// only ships with the PG build target. For vexdb-duck we instantiate the dispatcher
// once per metric here. Same template parameters across all three calls — only the
// runtime Metric value differs.
static ann_helper::distance_func GetRawDistanceFunc(Metric metric) {
    return DispatchRunner<false,
        MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::COSINE>,
        DistPrecisionTypeList<DistPrecisionType::FLOAT>,
        DispatcherMode::NO_QUANT>::call(
            metric, DistPrecisionType::FLOAT, 1, QuantizerType::NONE,
            [](auto &d) -> ann_helper::distance_func {
                return std::decay_t<decltype(d)>::get_distance_single;
            });
}

static void L2DistanceFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    static const auto squared_func = GetRawDistanceFunc(Metric::L2);
    auto sqrt_func = [](const void *xx, const void *yy, uint16 dim) -> float {
        return std::sqrt(squared_func(xx, yy, dim));
    };
    DistanceFunctionImpl(args, state, result, sqrt_func);
}

static void InnerProductFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    static const auto neg_ip_func = GetRawDistanceFunc(Metric::INNER_PRODUCT);
    auto ip_func = [](const void *xx, const void *yy, uint16 dim) -> float {
        return -neg_ip_func(xx, yy, dim);
    };
    DistanceFunctionImpl(args, state, result, ip_func);
}

// `<~>` returns -dot, matching pgvector-style "lower = more similar" so that
// ORDER BY a <~> b ASC sorts most-similar first. Alias of the negative-inner-
// product metric, mirrors the operator main exposes for vexdb users.
static void NegativeInnerProductFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    static const auto neg_ip_func = GetRawDistanceFunc(Metric::INNER_PRODUCT);
    DistanceFunctionImpl(args, state, result, neg_ip_func);
}

static void CosineDistanceFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    static const auto neg_cos_func = GetRawDistanceFunc(Metric::COSINE);
    auto cos_dist_func = [](const void *xx, const void *yy, uint16 dim) -> float {
        return 1.0f + neg_cos_func(xx, yy, dim);
    };
    DistanceFunctionImpl(args, state, result, cos_dist_func);
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

ScalarFunctionSet VexFunctions::GetInnerProductFunction() {
    ScalarFunctionSet set("inner_product");
    AddDistanceOverloads(set, InnerProductFunction);
    return set;
}

ScalarFunctionSet VexFunctions::GetCosineDistanceFunction() {
    ScalarFunctionSet set("cosine_distance");
    AddDistanceOverloads(set, CosineDistanceFunction);
    return set;
}

ScalarFunctionSet VexFunctions::GetCosineDistanceOperator() {
    ScalarFunctionSet set("<=>");
    AddDistanceOverloads(set, CosineDistanceFunction);
    return set;
}

ScalarFunctionSet VexFunctions::GetInnerProductOperator() {
    ScalarFunctionSet set("<#>");
    AddDistanceOverloads(set, InnerProductFunction);
    return set;
}

ScalarFunctionSet VexFunctions::GetNegativeInnerProductOperator() {
    ScalarFunctionSet set("<~>");
    AddDistanceOverloads(set, NegativeInnerProductFunction);
    return set;
}

void VexFunctions::Register(ExtensionLoader &loader) {
    loader.RegisterFunction(GetL2DistanceFunction());
    loader.RegisterFunction(GetL2DistanceOperator());
    loader.RegisterFunction(GetL2DistanceArrayAlias());
    loader.RegisterFunction(GetL2DistanceListAlias());
    loader.RegisterFunction(GetInnerProductFunction());
    loader.RegisterFunction(GetInnerProductOperator());
    loader.RegisterFunction(GetNegativeInnerProductOperator());
    loader.RegisterFunction(GetCosineDistanceFunction());
    loader.RegisterFunction(GetCosineDistanceOperator());
    loader.RegisterFunction(GetVectorDimsFunction());
    loader.RegisterFunction(GetVectorNormFunction());
    loader.RegisterFunction(GetVectorAddFunction());
    loader.RegisterFunction(GetVectorSubFunction());
    loader.RegisterFunction(GetL2NormalizeFunction());
    loader.RegisterFunction(ScalarFunction("vex_testvec3", {}, LogicalType::ARRAY(LogicalType::FLOAT, 3),
                                           VexTestVec3Function));
    RegisterIndexInfoFunction(loader);
}

} // namespace duckdb
