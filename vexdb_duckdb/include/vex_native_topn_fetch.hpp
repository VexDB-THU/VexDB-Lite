#pragma once

#include "vex_fetch_utils.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"

namespace duckdb {

// Retain DuckDB's exact filtered TopN, then fetch only its selected row IDs.
// This replaces the second full scan of a native late-materialization SEMI
// join. It neither reads graph vectors nor changes distance/order expressions.
class PhysicalVexNativeTopNFetch : public PhysicalOperator {
    struct FetchState : OperatorState {
        unique_ptr<ColumnDataCollection> rows;
        // Zero-copy vectors depend on scan-state pins until downstream has
        // consumed this output. Destroy the scan before its collection.
        ColumnDataScanState scan;
    };
public:
    static constexpr auto TYPE = PhysicalOperatorType::EXTENSION;
    PhysicalVexNativeTopNFetch(PhysicalPlan &plan, vector<LogicalType> types, idx_t cardinality,
                              DuckTableEntry &table, vector<ColumnIndex> columns,
                              unique_ptr<Expression> row_id)
        : PhysicalOperator(plan, TYPE, std::move(types), cardinality), table_(table),
          columns_(std::move(columns)), row_id_(std::move(row_id)) {}
    string GetName() const override { return "VEXDB_NATIVE_TOPN_FETCH"; }
    unique_ptr<OperatorState> GetOperatorState(ExecutionContext &) const override { return make_uniq<FetchState>(); }
    OperatorResultType Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                               GlobalOperatorState &, OperatorState &state) const override {
        if (context.client.IsInterrupted()) throw InterruptException();
        ExpressionExecutor executor(context.client, *row_id_);
        executor.SetChunk(input);
        Vector ids(LogicalType::ROW_TYPE);
        executor.ExecuteExpression(ids);
        ids.Flatten(input.size());
        const auto data = FlatVector::GetData<row_t>(ids);
        vector<row_t> selected;
        selected.reserve(input.size());
        for (idx_t i = 0; i < input.size(); ++i) {
            if (!FlatVector::Validity(ids).RowIsValid(i)) throw InternalException("Native TopN returned NULL row ID");
            selected.push_back(data[i]);
        }
        auto &fetch = state.Cast<FetchState>();
        // The preceding input has been consumed before the next invocation.
        fetch.scan = ColumnDataScanState();
        fetch.rows = FetchRowsByRowIds(context.client, table_, columns_, types, selected);
        fetch.rows->InitializeScan(fetch.scan);
        fetch.rows->Scan(fetch.scan, chunk); // input and fetch are both at most STANDARD_VECTOR_SIZE
        return OperatorResultType::NEED_MORE_INPUT;
    }
private:
    DuckTableEntry &table_;
    vector<ColumnIndex> columns_;
    unique_ptr<Expression> row_id_;
};

class LogicalVexNativeTopNFetch : public LogicalExtensionOperator {
public:
    LogicalVexNativeTopNFetch(DuckTableEntry &table, vector<ColumnIndex> columns, vector<LogicalType> output_types,
                             vector<ColumnBinding> bindings, unique_ptr<Expression> row_id)
        : table_(table), columns_(std::move(columns)), output_types_(std::move(output_types)),
          bindings_(std::move(bindings)), row_id_(std::move(row_id)) {}
    string GetExtensionName() const override { return "vex_native_topn_fetch"; }
    vector<ColumnBinding> GetColumnBindings() override { return bindings_; }
    void ResolveColumnBindings(ColumnBindingResolver &resolver, vector<ColumnBinding> &bindings) override {
        resolver.VisitOperator(*children[0]);
        resolver.VisitExpression(&row_id_);
        bindings = bindings_;
    }
    PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override {
        auto &fetch = planner.Make<PhysicalVexNativeTopNFetch>(types, estimated_cardinality, table_, columns_, row_id_->Copy());
        fetch.children.push_back(planner.CreatePlan(*children[0]));
        return fetch;
    }
    void Serialize(Serializer &) const override { throw NotImplementedException("Vex native TopN fetch serialization"); }
protected:
    void ResolveTypes() override { types = output_types_; }
private:
    DuckTableEntry &table_;
    vector<ColumnIndex> columns_;
    vector<LogicalType> output_types_;
    vector<ColumnBinding> bindings_;
    unique_ptr<Expression> row_id_;
};

} // namespace duckdb
