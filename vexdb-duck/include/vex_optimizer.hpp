#pragma once

#include "duckdb.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"

namespace duckdb {

class VexOptimizerExtension : public OptimizerExtension {
public:
    VexOptimizerExtension();

    static void OptimizeFunction(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);
    static void OptimizeNode(ClientContext &context, unique_ptr<LogicalOperator> &node);
};

} // namespace duckdb
