#pragma once

#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/storage/storage_index.hpp"
#include "duckdb/common/column_index.hpp"

namespace duckdb {

class GraphIndex;

//===--------------------------------------------------------------------===//
// LogicalVexIndexScan — inserted into the logical plan by the optimizer
// when the query vector cannot be evaluated at optimization time (deferred).
// CreatePlan() produces the PhysicalVexIndexScan.
//===--------------------------------------------------------------------===//
struct LogicalVexIndexScan : public LogicalExtensionOperator {
public:
	LogicalVexIndexScan(idx_t table_index, vector<LogicalType> output_types,
	                    DuckTableEntry &table, GraphIndex &graph_index,
	                    unique_ptr<Expression> query_vec_expr, idx_t k,
	                    vector<ColumnIndex> column_ids, vector<LogicalType> returned_types);

	// LogicalExtensionOperator interface
	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override;
	void ResolveColumnBindings(ColumnBindingResolver &res, vector<ColumnBinding> &bindings) override;
	string GetExtensionName() const override;

	// LogicalOperator interface
	vector<ColumnBinding> GetColumnBindings() override;
	void Serialize(Serializer &serializer) const override;

protected:
	void ResolveTypes() override;

public:
	//! The table index for column binding resolution
	idx_t table_index;
	//! The output types of this operator
	vector<LogicalType> output_types;
	//! The table to fetch from
	DuckTableEntry &table;
	//! The graph index to search
	GraphIndex &graph_index;
	//! Expression that produces the query vector (evaluated at runtime)
	unique_ptr<Expression> query_vec_expr;
	//! Number of nearest neighbors to return
	idx_t k;
	//! Column IDs to fetch from the table (logical column indices from LogicalGet)
	vector<ColumnIndex> column_ids;
	//! The returned types of the underlying table columns (indexed by physical column position)
	vector<LogicalType> returned_types;
};

//===--------------------------------------------------------------------===//
// PhysicalVexIndexScan — operator that receives a query vector from
// its child pipeline, runs GraphIndex::ANNSearch, and fetches result rows.
// This is a 1-to-many operator: it receives 1 row (the query vector)
// and produces k rows (the nearest neighbors).
//===--------------------------------------------------------------------===//
class PhysicalVexIndexScan : public PhysicalOperator {
public:
	PhysicalVexIndexScan(PhysicalPlan &physical_plan, vector<LogicalType> types, idx_t estimated_cardinality,
	                     DuckTableEntry &table, GraphIndex &graph_index,
	                     unique_ptr<Expression> query_vec_expr, idx_t k,
	                     vector<ColumnIndex> column_ids, vector<LogicalType> returned_types);

	// Operator interface
	unique_ptr<OperatorState> GetOperatorState(ExecutionContext &context) const override;
	OperatorResultType Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
	                           GlobalOperatorState &gstate, OperatorState &state) const override;

	bool RequiresFinalExecute() const override {
		return true;
	}
	OperatorFinalizeResultType FinalExecute(ExecutionContext &context, DataChunk &chunk,
	                                         GlobalOperatorState &gstate, OperatorState &state) const override;

public:
	//! The table to fetch from
	DuckTableEntry &table;
	//! The graph index to search
	GraphIndex &graph_index;
	//! Expression that produces the query vector (evaluated at runtime)
	unique_ptr<Expression> query_vec_expr;
	//! Number of nearest neighbors to return
	idx_t k;
	//! Column IDs from LogicalGet (logical column indices)
	vector<ColumnIndex> column_ids;
	//! The returned types of the underlying table columns
	vector<LogicalType> returned_types;
};

} // namespace duckdb
