#pragma once

#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class VexOptimizerExtension : public OptimizerExtension {
public:
	VexOptimizerExtension();

	static void OptimizeFunction(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);

private:
	static void OptimizeNode(ClientContext &context, unique_ptr<LogicalOperator> &node);
	static bool TryOptimizeTopN(ClientContext &context, unique_ptr<LogicalOperator> &node);
	static bool TryOptimizeHybridTopN(ClientContext &context, unique_ptr<LogicalOperator> &node);
};

} // namespace duckdb
