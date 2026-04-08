#include "vex_physical_index_scan.hpp"
#include "vex_graph_index.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

// #define VEX_PHYSICAL_SCAN_DEBUG
#ifdef VEX_PHYSICAL_SCAN_DEBUG
#include <iostream>
#endif

namespace duckdb {

//===--------------------------------------------------------------------===//
// LogicalVexIndexScan
//===--------------------------------------------------------------------===//

LogicalVexIndexScan::LogicalVexIndexScan(idx_t table_index_p, vector<LogicalType> output_types_p,
                                         DuckTableEntry &table_p, GraphIndex &graph_index_p,
                                         unique_ptr<Expression> query_vec_expr_p, idx_t k_p,
                                         vector<ColumnIndex> column_ids_p, vector<LogicalType> returned_types_p)
    : LogicalExtensionOperator(), table_index(table_index_p), output_types(std::move(output_types_p)),
      table(table_p), graph_index(graph_index_p), query_vec_expr(std::move(query_vec_expr_p)), k(k_p),
      column_ids(std::move(column_ids_p)), returned_types(std::move(returned_types_p)) {
}

PhysicalOperator &LogicalVexIndexScan::CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) {
	// Create physical plans for child operators (subquery pipelines)
	// The child pipeline provides the query vector at runtime.
	vector<reference<PhysicalOperator>> child_plans;
	for (auto &child : children) {
		child_plans.push_back(planner.CreatePlan(*child));
	}

	auto &scan = planner.Make<PhysicalVexIndexScan>(
	    types, estimated_cardinality,
	    table, graph_index,
	    query_vec_expr->Copy(), k,
	    column_ids, returned_types);

	// Attach child physical plans
	for (auto &child_plan : child_plans) {
		scan.children.push_back(child_plan.get());
	}

	return scan;
}

void LogicalVexIndexScan::ResolveColumnBindings(ColumnBindingResolver &res, vector<ColumnBinding> &bindings) {
	// First visit children (the subquery pipeline) to resolve their bindings
	for (auto &child : children) {
		res.VisitOperator(*child);
	}
	// Resolve the query vector expression (it may reference columns from the subquery child)
	res.VisitExpression(&query_vec_expr);
	// Update bindings to our output columns
	bindings = GetColumnBindings();
}

string LogicalVexIndexScan::GetExtensionName() const {
	return "vex_index_scan";
}

vector<ColumnBinding> LogicalVexIndexScan::GetColumnBindings() {
	// Expose bindings from the main table (same as the original GET)
	vector<ColumnBinding> result;
	for (idx_t i = 0; i < output_types.size(); i++) {
		result.emplace_back(table_index, i);
	}
	// Also expose bindings from the subquery child (if present)
	// so that the PROJECTION above can resolve references like col_ref(24, 0)
	for (auto &child : children) {
		auto child_bindings = child->GetColumnBindings();
		for (auto &binding : child_bindings) {
			result.push_back(binding);
		}
	}
	return result;
}

void LogicalVexIndexScan::Serialize(Serializer &serializer) const {
	throw NotImplementedException("LogicalVexIndexScan::Serialize not supported");
}

void LogicalVexIndexScan::ResolveTypes() {
	types = output_types;
	// Also include child types (subquery) since we replace CROSS_PRODUCT
	// which would have exposed both sides' columns
	for (auto &child : children) {
		for (auto &type : child->types) {
			types.push_back(type);
		}
	}
}

//===--------------------------------------------------------------------===//
// PhysicalVexIndexScan — Constructor
//===--------------------------------------------------------------------===//

PhysicalVexIndexScan::PhysicalVexIndexScan(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                           idx_t estimated_cardinality, DuckTableEntry &table_p,
                                           GraphIndex &graph_index_p, unique_ptr<Expression> query_vec_expr_p,
                                           idx_t k_p, vector<ColumnIndex> column_ids_p,
                                           vector<LogicalType> returned_types_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
      table(table_p), graph_index(graph_index_p), query_vec_expr(std::move(query_vec_expr_p)), k(k_p),
      column_ids(std::move(column_ids_p)), returned_types(std::move(returned_types_p)) {
}

//===--------------------------------------------------------------------===//
// PhysicalVexIndexScan — Operator State
//===--------------------------------------------------------------------===//

class VexIndexScanOperatorState : public OperatorState {
public:
	VexIndexScanOperatorState() : searched(false), scan_offset(0) {
	}

	//! Whether the ANN search has been performed
	bool searched;
	//! Buffered result collection
	unique_ptr<ColumnDataCollection> collection;
	//! Scan state for reading from collection
	ColumnDataScanState scan_state;
	//! Current scan offset
	idx_t scan_offset;
};

unique_ptr<OperatorState> PhysicalVexIndexScan::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<VexIndexScanOperatorState>();
}

//===--------------------------------------------------------------------===//
// Helper: extract float vector from a Value
//===--------------------------------------------------------------------===//

static bool ExtractFloatVector(const Value &val, vector<float> &out_vec) {
	auto &type = val.type();

	// Handle ARRAY type (FLOAT[N])
	if (type.id() == LogicalTypeId::ARRAY) {
		auto &children = ArrayValue::GetChildren(val);
		out_vec.clear();
		out_vec.reserve(children.size());
		for (auto &child : children) {
			try {
				auto float_val = child.DefaultCastAs(LogicalType::FLOAT);
				out_vec.push_back(FloatValue::Get(float_val));
			} catch (...) {
				out_vec.clear();
				return false;
			}
		}
		return !out_vec.empty();
	}

	// Handle LIST type
	if (type.id() == LogicalTypeId::LIST) {
		auto &children = ListValue::GetChildren(val);
		out_vec.clear();
		out_vec.reserve(children.size());
		for (auto &child : children) {
			try {
				auto float_val = child.DefaultCastAs(LogicalType::FLOAT);
				out_vec.push_back(FloatValue::Get(float_val));
			} catch (...) {
				out_vec.clear();
				return false;
			}
		}
		return !out_vec.empty();
	}

	return false;
}

//===--------------------------------------------------------------------===//
// Helper: perform ANN search and fetch rows into a ColumnDataCollection
//===--------------------------------------------------------------------===//

static unique_ptr<ColumnDataCollection> PerformSearchAndFetch(
    ClientContext &client_context, DuckTableEntry &table, GraphIndex &graph_index,
    const vector<float> &query_vec, idx_t k,
    const vector<ColumnIndex> &column_ids, const vector<LogicalType> &returned_types,
    const vector<LogicalType> &output_types) {

	// Read ef_search and brute_force_threshold from settings
	int ef = static_cast<int>(k) * 2;
	Value ef_val;
	if (client_context.TryGetCurrentSetting("vex_ef_search", ef_val)) {
		ef = ef_val.GetValue<int>();
		if (ef < 1 || ef > 10000) {
			throw InvalidInputException("vex_ef_search must be between 1 and 10000, got %d", ef);
		}
	}
	if (static_cast<int>(k) > ef) {
		ef = static_cast<int>(k) * 2;
	}

	idx_t bf_threshold = GraphIndexCore::BRUTE_FORCE_THRESHOLD;
	Value bf_val;
	if (client_context.TryGetCurrentSetting("vex_brute_force_threshold", bf_val)) {
		bf_threshold = bf_val.GetValue<idx_t>();
	}

	// Execute ANN search
	vector<row_t> result_row_ids;
	vector<float> result_distances;
	graph_index.ANNSearch(query_vec.data(), k, ef, result_row_ids, result_distances, bf_threshold);

#ifdef VEX_PHYSICAL_SCAN_DEBUG
	std::cerr << "[VEX PhysicalScan] ANNSearch returned " << result_row_ids.size()
	          << " results for k=" << k << std::endl;
#endif

	// Build output types from column_ids
	vector<LogicalType> get_output_types;
	get_output_types.reserve(column_ids.size());
	for (idx_t i = 0; i < column_ids.size(); i++) {
		if (column_ids[i].IsVirtualColumn()) {
			get_output_types.push_back(LogicalType::ROW_TYPE);
		} else {
			idx_t physical_idx = column_ids[i].GetPrimaryIndex();
			if (physical_idx < returned_types.size()) {
				get_output_types.push_back(returned_types[physical_idx]);
			} else {
				throw InternalException("VEX PhysicalScan: column physical index %llu out of range", physical_idx);
			}
		}
	}

	auto collection = make_uniq<ColumnDataCollection>(client_context, output_types);

	if (result_row_ids.empty()) {
		return collection;
	}

	// Filter by transaction visibility
	auto &storage = table.GetStorage();
	auto &db = table.ParentCatalog().GetAttached();
	auto &transaction = DuckTransaction::Get(client_context, db);

	vector<row_t> visible_row_ids;
	visible_row_ids.reserve(result_row_ids.size());
	for (auto &rid : result_row_ids) {
		if (storage.CanFetch(transaction, rid)) {
			visible_row_ids.push_back(rid);
		}
	}

	if (visible_row_ids.empty()) {
		return collection;
	}

	// Separate physical columns from virtual (row_id) columns
	vector<StorageIndex> fetch_col_ids;
	vector<idx_t> fetch_output_positions;
	vector<idx_t> rowid_positions;

	for (idx_t i = 0; i < column_ids.size(); i++) {
		if (column_ids[i].IsVirtualColumn()) {
			rowid_positions.push_back(i);
		} else {
			fetch_col_ids.emplace_back(column_ids[i].GetPrimaryIndex());
			fetch_output_positions.push_back(i);
		}
	}

	vector<LogicalType> fetch_types;
	for (idx_t pos : fetch_output_positions) {
		fetch_types.push_back(get_output_types[pos]);
	}

	ColumnFetchState fetch_state;
	DataChunk fetch_chunk;
	fetch_chunk.Initialize(client_context, fetch_types);
	DataChunk output_chunk;
	output_chunk.Initialize(client_context, output_types);

	idx_t total = visible_row_ids.size();
	for (idx_t off = 0; off < total;) {
		idx_t batch = MinValue<idx_t>(STANDARD_VECTOR_SIZE, total - off);
		fetch_chunk.Reset();
		output_chunk.Reset();

		Vector row_id_vec(LogicalType::ROW_TYPE, batch);
		auto row_id_data = FlatVector::GetData<row_t>(row_id_vec);
		for (idx_t i = 0; i < batch; i++) {
			row_id_data[i] = visible_row_ids[off + i];
		}

		storage.Fetch(transaction, fetch_chunk, fetch_col_ids, row_id_vec, batch, fetch_state);

		// Map fetched columns to output positions
		for (idx_t f = 0; f < fetch_output_positions.size(); f++) {
			output_chunk.data[fetch_output_positions[f]].Reference(fetch_chunk.data[f]);
		}

		// Fill in row_id columns
		for (idx_t rid_pos : rowid_positions) {
			auto rowid_data_ptr = FlatVector::GetData<row_t>(output_chunk.data[rid_pos]);
			for (idx_t i = 0; i < batch; i++) {
				rowid_data_ptr[i] = visible_row_ids[off + i];
			}
		}

		output_chunk.SetCardinality(batch);
		collection->Append(output_chunk);
		off += batch;
	}

#ifdef VEX_PHYSICAL_SCAN_DEBUG
	std::cerr << "[VEX PhysicalScan] collection count=" << collection->Count() << std::endl;
#endif

	return collection;
}

//===--------------------------------------------------------------------===//
// PhysicalVexIndexScan::Execute
//===--------------------------------------------------------------------===//

OperatorResultType PhysicalVexIndexScan::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                  GlobalOperatorState &gstate, OperatorState &state_p) const {
	auto &state = state_p.Cast<VexIndexScanOperatorState>();

	if (!state.searched) {
		auto &client_context = context.client;

		// Extract query vector from the input chunk.
		// The input comes from the subquery/CROSS_PRODUCT pipeline.
		// After column binding resolution, query_vec_expr is a BoundReferenceExpression
		// that indexes into the input chunk.
		D_ASSERT(input.size() > 0);

		// Use ExpressionExecutor to evaluate the expression against the input chunk
		ExpressionExecutor executor(client_context, *query_vec_expr);
		Vector result_vec(query_vec_expr->return_type);
		executor.ExecuteExpression(input, result_vec);

		auto query_value = result_vec.GetValue(0);

#ifdef VEX_PHYSICAL_SCAN_DEBUG
		std::cerr << "[VEX PhysicalScan] query_value type=" << query_value.type().ToString()
		          << " value=" << query_value.ToString() << std::endl;
#endif

		vector<float> query_vec;
		if (!ExtractFloatVector(query_value, query_vec)) {
			throw InvalidInputException("VEX index scan: could not extract float vector from query expression "
			                            "(got type %s)", query_value.type().ToString());
		}

		// The output types include both the main table's columns and the subquery's
		// columns (since we replace CROSS_PRODUCT). Build the "get types" for fetching
		// from the table (only the main table portion).
		vector<LogicalType> get_types;
		for (idx_t i = 0; i < column_ids.size(); i++) {
			if (column_ids[i].IsVirtualColumn()) {
				get_types.push_back(LogicalType::ROW_TYPE);
			} else {
				idx_t physical_idx = column_ids[i].GetPrimaryIndex();
				get_types.push_back(returned_types[physical_idx]);
			}
		}

		auto fetch_collection = PerformSearchAndFetch(
		    client_context, table, graph_index, query_vec, k,
		    column_ids, returned_types, get_types);

		// Now build the final collection that includes the subquery columns too.
		// The subquery columns are constant (same query vector for every row).
		state.collection = make_uniq<ColumnDataCollection>(client_context, types);

		if (fetch_collection->Count() > 0) {
			ColumnDataScanState fetch_scan;
			fetch_collection->InitializeScan(fetch_scan);

			DataChunk fetch_chunk;
			fetch_chunk.Initialize(client_context, get_types);
			DataChunk output_chunk;
			output_chunk.Initialize(client_context, types);

			while (true) {
				fetch_chunk.Reset();
				output_chunk.Reset();
				fetch_collection->Scan(fetch_scan, fetch_chunk);
				if (fetch_chunk.size() == 0) {
					break;
				}

				// Copy main table columns
				for (idx_t i = 0; i < get_types.size(); i++) {
					output_chunk.data[i].Reference(fetch_chunk.data[i]);
				}

				// Fill in subquery columns (constant: same query vector for every row)
				// The subquery columns come from the input chunk (row 0)
				idx_t subquery_col_start = get_types.size();
				for (idx_t i = subquery_col_start; i < types.size(); i++) {
					idx_t input_col = i - subquery_col_start;
					if (input_col < input.ColumnCount()) {
						auto val = input.data[input_col].GetValue(0);
						for (idx_t r = 0; r < fetch_chunk.size(); r++) {
							output_chunk.data[i].SetValue(r, val);
						}
					}
				}

				output_chunk.SetCardinality(fetch_chunk.size());
				state.collection->Append(output_chunk);
			}
		}

		state.collection->InitializeScan(state.scan_state);
		state.searched = true;
	}

	// Emit rows from the collection
	state.collection->Scan(state.scan_state, chunk);
	if (chunk.size() == 0) {
		return OperatorResultType::NEED_MORE_INPUT;
	}

	return OperatorResultType::HAVE_MORE_OUTPUT;
}

//===--------------------------------------------------------------------===//
// PhysicalVexIndexScan::FinalExecute
// Called after Execute returns NEED_MORE_INPUT and there's no more input.
// We use this to emit any remaining buffered results.
//===--------------------------------------------------------------------===//

OperatorFinalizeResultType PhysicalVexIndexScan::FinalExecute(ExecutionContext &context, DataChunk &chunk,
                                                               GlobalOperatorState &gstate, OperatorState &state_p) const {
	auto &state = state_p.Cast<VexIndexScanOperatorState>();

	if (!state.searched || !state.collection) {
		return OperatorFinalizeResultType::FINISHED;
	}

	state.collection->Scan(state.scan_state, chunk);
	if (chunk.size() == 0) {
		return OperatorFinalizeResultType::FINISHED;
	}

	return OperatorFinalizeResultType::HAVE_MORE_OUTPUT;
}

} // namespace duckdb
