#pragma once

#include "duckdb.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

LogicalType ResolveToFloatArray(ClientContext &context, Expression &expr);

struct VexFunctions {
    static void Register(ExtensionLoader &loader);

    static ScalarFunctionSet GetL2DistanceFunction();
    static ScalarFunctionSet GetL2DistanceOperator();
    static ScalarFunctionSet GetL2DistanceArrayAlias();
    static ScalarFunctionSet GetL2DistanceListAlias();
    static ScalarFunctionSet GetInnerProductFunction();
    static ScalarFunctionSet GetInnerProductOperator();
    static ScalarFunctionSet GetNegativeInnerProductOperator();
    static ScalarFunctionSet GetCosineDistanceFunction();
    static ScalarFunctionSet GetCosineDistanceOperator();

    static ScalarFunctionSet GetVectorDimsFunction();
    static ScalarFunctionSet GetVectorNormFunction();
    static ScalarFunctionSet GetVectorAddFunction();
    static ScalarFunctionSet GetVectorSubFunction();
    static ScalarFunctionSet GetL2NormalizeFunction();

    static void RegisterIndexInfoFunction(ExtensionLoader &loader);
};

} // namespace duckdb
