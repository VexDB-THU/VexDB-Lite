#pragma once

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/column_index.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"

namespace duckdb {

class GraphIndex;

struct LogicalVexIndexScan : public LogicalExtensionOperator {
public:
    LogicalVexIndexScan(idx_t table_index, vector<LogicalType> output_types,
                        DuckTableEntry &table, GraphIndex &graph_index,
                        unique_ptr<Expression> query_vec_expr, idx_t k,
                        vector<ColumnIndex> column_ids, vector<idx_t> fetch_output_positions,
                        optional_idx distance_output_index, vector<LogicalType> returned_types);

    PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override;
    void ResolveColumnBindings(ColumnBindingResolver &res, vector<ColumnBinding> &bindings) override;
    string GetExtensionName() const override;
    vector<ColumnBinding> GetColumnBindings() override;
    void Serialize(Serializer &serializer) const override;

protected:
    void ResolveTypes() override;

public:
    idx_t table_index;
    vector<LogicalType> output_types;
    DuckTableEntry &table;
    GraphIndex &graph_index;
    unique_ptr<Expression> query_vec_expr;
    idx_t k;
    vector<ColumnIndex> column_ids;
    vector<idx_t> fetch_output_positions;
    optional_idx distance_output_index;
    vector<LogicalType> returned_types;
};

class PhysicalVexIndexScan : public PhysicalOperator {
public:
    static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

	PhysicalVexIndexScan(PhysicalPlan &physical_plan, vector<LogicalType> types, idx_t estimated_cardinality,
	                     DuckTableEntry &table, GraphIndex &graph_index,
	                     unique_ptr<Expression> query_vec_expr, idx_t k,
	                     vector<ColumnIndex> column_ids, vector<idx_t> fetch_output_positions,
	                     optional_idx distance_output_index, vector<LogicalType> returned_types,
	                     idx_t base_output_count);

    string GetName() const override;
    InsertionOrderPreservingMap<string> ParamsToString() const override;
    unique_ptr<OperatorState> GetOperatorState(ExecutionContext &context) const override;
    OperatorResultType Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                               GlobalOperatorState &gstate, OperatorState &state) const override;

    bool RequiresFinalExecute() const override {
        return true;
    }
    OperatorFinalizeResultType FinalExecute(ExecutionContext &context, DataChunk &chunk,
                                            GlobalOperatorState &gstate, OperatorState &state) const override;

public:
	DuckTableEntry &table;
	GraphIndex &graph_index;
	unique_ptr<Expression> query_vec_expr;
	idx_t base_output_count;
	idx_t k;
    vector<ColumnIndex> column_ids;
    vector<idx_t> fetch_output_positions;
    optional_idx distance_output_index;
    vector<LogicalType> returned_types;
    string algorithm_used;
};

} // namespace duckdb
