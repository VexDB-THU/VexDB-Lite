#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"

namespace duckdb {

class LogicalOperator;
class TableCatalogEntry;
class DuckTableEntry;

class PhysicalVexCreateIndex : public PhysicalOperator {
public:
    static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::CREATE_INDEX;

public:
    PhysicalVexCreateIndex(PhysicalPlan &physical_plan, LogicalOperator &op, TableCatalogEntry &table,
                           const vector<column_t> &column_ids, unique_ptr<CreateIndexInfo> info,
                           vector<unique_ptr<Expression>> unbound_expressions, idx_t estimated_cardinality,
                           unique_ptr<AlterTableInfo> alter_table_info = nullptr);

    DuckTableEntry &table;
    vector<column_t> storage_ids;
    unique_ptr<CreateIndexInfo> info;
    vector<unique_ptr<Expression>> unbound_expressions;
    unique_ptr<AlterTableInfo> alter_table_info;

public:
    SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                     OperatorSourceInput &input) const override;
    bool IsSource() const override {
        return true;
    }

    unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;
    unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
    SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
    SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
    SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                              OperatorSinkFinalizeInput &input) const override;

    bool IsSink() const override {
        return true;
    }
    bool ParallelSink() const override {
        return true;
    }
};

} // namespace duckdb
