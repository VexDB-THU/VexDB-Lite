#pragma once

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/local_storage.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_set>
#include "vex_search_profile.hpp"

namespace duckdb {

// Predicate references are local positions in the metadata scan, not bindings
// in the outer SQL plan. Original SQL filters are preserved above the scan.
struct VexFilterPlan {
    vector<column_t> columns;
    vector<LogicalType> types;
    unique_ptr<Expression> predicate;
    unique_ptr<Expression> distance;
    column_t vector_column;
    LogicalType vector_type;
    idx_t query_argument;
    bool ascending;

    unique_ptr<VexFilterPlan> Copy() const {
        auto p = make_uniq<VexFilterPlan>();
        p->columns = columns;
        p->types = types;
        p->predicate = predicate->Copy();
        p->distance = distance->Copy();
        p->vector_column = vector_column;
        p->vector_type = vector_type;
        p->query_argument = query_argument;
        p->ascending = ascending;
        return p;
    }
};

inline vector<row_t> VexScanAllowedRows(ClientContext &context, DataTable &storage,
                                       DuckTransaction &transaction, const VexFilterPlan &plan,
                                       VexSearchProfileData *profile = nullptr) {
    vector<StorageIndex> columns;
    for (auto id : plan.columns) columns.emplace_back(id);
    columns.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
    auto types = plan.types;
    types.push_back(LogicalType::ROW_TYPE);
    TableScanState state;
    storage.InitializeScan(context, transaction, state, columns);
    DataChunk chunk;
    chunk.Initialize(context, types);
    ExpressionExecutor predicate(context, *plan.predicate);
    SelectionVector selection(STANDARD_VECTOR_SIZE);
    vector<row_t> rows;
    while (true) {
        if (context.IsInterrupted()) throw InterruptException();
        chunk.Reset();
        storage.Scan(transaction, chunk, state);
        if (!chunk.size()) break;
        if (profile) profile->metadata_rows += chunk.size();
        const auto count = predicate.SelectExpression(chunk, selection);
        chunk.data.back().Flatten(chunk.size());
        const auto ids = FlatVector::GetData<row_t>(chunk.data.back());
        for (idx_t i = 0; i < count; ++i) rows.push_back(ids[selection.get_index(i)]);
    }
    return rows;
}

// Stream only the qualifying vectors from the current transaction. This is
// also the safe fallback for missing graph rows, compact/IP modes or an
// exhausted graph. No raw-vector collection proportional to N * dimension.
inline void VexFilteredExact(ClientContext &context, DataTable &storage, DuckTransaction &transaction,
                              const VexFilterPlan &plan, const Value &query, const vector<row_t> &allowed,
                              idx_t k, std::vector<row_t> &ids, std::vector<float> &distances,
                              VexSearchProfileData *profile = nullptr) {
    ids.clear();
    distances.clear();
    k = MinValue<idx_t>(k, allowed.size());
    if (!k) return;
    auto distance = plan.distance->Copy();
    distance->Cast<BoundFunctionExpression>().children[plan.query_argument] = make_uniq<BoundConstantExpression>(query);
    ExpressionExecutor executor(context, *distance);
    DataChunk chunk;
    chunk.Initialize(context, {plan.vector_type});
    ColumnFetchState fetch_state;
    vector<StorageIndex> columns {StorageIndex(plan.vector_column)};
    struct Entry { double score; row_t row; bool null; };
    auto closer = [&](const Entry &a, const Entry &b) {
        if (a.null != b.null) return !a.null; // only NULLS LAST plans enter this path
        if (!a.null) {
            if (std::isnan(a.score) != std::isnan(b.score))
                return plan.ascending ? !std::isnan(a.score) : std::isnan(a.score);
            if (a.score != b.score && !std::isnan(a.score))
                return plan.ascending ? a.score < b.score : a.score > b.score;
        }
        return a.row < b.row;
    };
    std::vector<Entry> heap;
    heap.reserve(k);
    for (idx_t off = 0; off < allowed.size();) {
        if (context.IsInterrupted()) throw InterruptException();
        auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, allowed.size() - off);
        const bool local = allowed[off] >= MAX_ROW_ID;
        for (idx_t i = 1; i < count; ++i) {
            if ((allowed[off+i] >= MAX_ROW_ID) != local) { count = i; break; }
        }
        Vector row_ids(LogicalType::ROW_TYPE, count);
        auto row_data = FlatVector::GetData<row_t>(row_ids);
        for (idx_t i = 0; i < count; ++i) row_data[i] = allowed[off + i];
        chunk.Reset();
        if (local) {
            LocalStorage::Get(transaction).FetchChunk(storage, row_ids, count, columns, chunk, fetch_state);
        } else {
            storage.Fetch(transaction, chunk, columns, row_ids, count, fetch_state);
        }
        chunk.SetCardinality(count);
        Vector scores(distance->return_type);
        executor.ExecuteExpression(chunk, scores);
        if (profile) profile->exact_rows_scored += count;
        scores.Flatten(count);
        const auto &validity = FlatVector::Validity(scores);
        for (idx_t i = 0; i < count; ++i) {
            const bool null = !validity.RowIsValid(i);
            double score = 0;
            if (!null) score = scores.GetType().id() == LogicalTypeId::FLOAT
                ? FlatVector::GetData<float>(scores)[i] : FlatVector::GetData<double>(scores)[i];
            Entry entry {score, allowed[off + i], null};
            if (heap.size() < k) {
                heap.push_back(entry);
                std::push_heap(heap.begin(), heap.end(), closer);
            } else if (closer(entry, heap.front())) {
                std::pop_heap(heap.begin(), heap.end(), closer);
                heap.back() = entry;
                std::push_heap(heap.begin(), heap.end(), closer);
            }
        }
        off += count;
    }
    std::sort_heap(heap.begin(), heap.end(), closer);
    for (const auto &entry : heap) { ids.push_back(entry.row); distances.push_back(float(entry.score)); }
}

} // namespace duckdb
