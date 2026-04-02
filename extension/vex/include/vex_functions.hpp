#pragma once

#include "duckdb.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

// Resolve a single expression's type to FLOAT[N]:
// - ARRAY → ensure child type is FLOAT
// - LIST literal → evaluate to get size, then convert to FLOAT[N]
// - Other → throw
LogicalType ResolveToFloatArray(ClientContext &context, Expression &expr);

struct VexFunctions {
	static void Register(ExtensionLoader &loader);

	static ScalarFunctionSet GetL2DistanceFunction();
	static ScalarFunctionSet GetInnerProductFunction();
	static ScalarFunctionSet GetNegativeInnerProductFunction();
	static ScalarFunctionSet GetCosineDistanceFunction();

	static ScalarFunction GetVectorDimsFunction();
	static ScalarFunction GetVectorNormFunction();
	static ScalarFunction GetL2NormalizeFunction();
	static ScalarFunction GetVectorAddFunction();
	static ScalarFunction GetVectorSubFunction();

	static void RegisterANNSearchFunction(ExtensionLoader &loader);
	static void RegisterIndexInfoFunction(ExtensionLoader &loader);
};

} // namespace duckdb
