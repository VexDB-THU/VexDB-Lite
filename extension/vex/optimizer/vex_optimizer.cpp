#include "vex_optimizer.hpp"
#include "vex_graph_index.hpp"
#include "vex_hybrid_index.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_column_data_get.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

namespace duckdb {

// Distance function names we recognize for index optimization
static bool IsDistanceFunction(const string &name) {
	return name == "list_distance" || name == "<->" ||
	       name == "l2_distance" ||
	       name == "list_cosine_distance" || name == "<=>" ||
	       name == "cosine_distance" ||
	       name == "list_negative_inner_product" ||
	       name == "<#>";
}

// Extract a constant float array from an expression
static bool TryExtractConstantVector(const Expression &expr, vector<float> &out_vec) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	auto &const_expr = expr.Cast<BoundConstantExpression>();
	auto &val = const_expr.value;

	// Must be an ARRAY type with FLOAT children
	auto &type = val.type();
	if (type.id() != LogicalTypeId::ARRAY) {
		return false;
	}
	auto child_type = ArrayType::GetChildType(type);
	if (child_type.id() != LogicalTypeId::FLOAT) {
		return false;
	}

	auto &children = ArrayValue::GetChildren(val);
	out_vec.clear();
	out_vec.reserve(children.size());
	for (auto &child : children) {
		out_vec.push_back(FloatValue::Get(child));
	}
	return !out_vec.empty();
}

// Check if an expression references a column from a specific table
static bool IsColumnRefFromTable(const Expression &expr, idx_t table_index, idx_t &col_index) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	auto &colref = expr.Cast<BoundColumnRefExpression>();
	if (colref.binding.table_index != table_index) {
		return false;
	}
	col_index = colref.binding.column_index;
	return true;
}

// Shared helper: fetch rows by row_ids and build a ColumnDataCollection
static unique_ptr<ColumnDataCollection> FetchRowsByIds(ClientContext &context, DuckTableEntry &duck_table,
                                                       LogicalGet *get, const vector<row_t> &result_row_ids,
                                                       idx_t limit, idx_t offset) {
	auto &storage = duck_table.GetStorage();
	auto &db = duck_table.ParentCatalog().GetAttached();
	auto &transaction = DuckTransaction::Get(context, db);

	// Filter out row_ids that are not visible in the current transaction (e.g. deleted rows)
	vector<row_t> visible_row_ids;
	visible_row_ids.reserve(result_row_ids.size());
	for (auto &rid : result_row_ids) {
		if (storage.CanFetch(transaction, rid)) {
			visible_row_ids.push_back(rid);
		}
	}
	auto &row_ids_ref = visible_row_ids;

	auto &output_types = get->types;
	auto &column_ids = get->GetColumnIds();

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

	idx_t total = row_ids_ref.size();
	idx_t start = offset;
	idx_t count = (start >= total) ? 0 : MinValue<idx_t>(limit, total - start);

	vector<LogicalType> fetch_types;
	for (idx_t pos : fetch_output_positions) {
		fetch_types.push_back(output_types[pos]);
	}

	auto collection = make_uniq<ColumnDataCollection>(context, output_types);

	if (count == 0) {
		return collection;
	}

	ColumnFetchState fetch_state;
	DataChunk fetch_chunk;
	fetch_chunk.Initialize(context, fetch_types);
	DataChunk output_chunk;
	output_chunk.Initialize(context, output_types);

	for (idx_t off = start; off < start + count;) {
		idx_t batch = MinValue<idx_t>(STANDARD_VECTOR_SIZE, start + count - off);
		fetch_chunk.Reset();
		output_chunk.Reset();

		Vector row_id_vec(LogicalType::ROW_TYPE, batch);
		auto row_id_data = FlatVector::GetData<row_t>(row_id_vec);
		for (idx_t i = 0; i < batch; i++) {
			row_id_data[i] = row_ids_ref[off + i];
		}

		storage.Fetch(transaction, fetch_chunk, fetch_col_ids, row_id_vec, batch, fetch_state);

		for (idx_t f = 0; f < fetch_output_positions.size(); f++) {
			output_chunk.data[fetch_output_positions[f]].Reference(fetch_chunk.data[f]);
		}
		for (idx_t rid_pos : rowid_positions) {
			auto rowid_data_ptr = FlatVector::GetData<row_t>(output_chunk.data[rid_pos]);
			for (idx_t i = 0; i < batch; i++) {
				rowid_data_ptr[i] = row_ids_ref[off + i];
			}
		}

		output_chunk.SetCardinality(batch);
		collection->Append(output_chunk);
		off += batch;
	}

	return collection;
}

VexOptimizerExtension::VexOptimizerExtension() {
	optimize_function = OptimizeFunction;
}

void VexOptimizerExtension::OptimizeFunction(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	OptimizeNode(input.context, plan);
}

void VexOptimizerExtension::OptimizeNode(ClientContext &context, unique_ptr<LogicalOperator> &node) {
	for (auto &child : node->children) {
		OptimizeNode(context, child);
	}

	if (node->type == LogicalOperatorType::LOGICAL_TOP_N) {
		if (!TryOptimizeHybridTopN(context, node)) {
			TryOptimizeTopN(context, node);
		}
	}
}

bool VexOptimizerExtension::TryOptimizeTopN(ClientContext &context, unique_ptr<LogicalOperator> &node) {
	auto &topn = node->Cast<LogicalTopN>();

	// Only handle single ORDER BY
	if (topn.orders.size() != 1) {
		return false;
	}

	// Must be ascending order (distance should be small first)
	if (topn.orders[0].type != OrderType::ASCENDING) {
		return false;
	}

	// Find the LogicalGet child (may be through Projections)
	LogicalGet *get = nullptr;
	LogicalProjection *proj = nullptr;
	if (topn.children[0]->type == LogicalOperatorType::LOGICAL_GET) {
		get = &topn.children[0]->Cast<LogicalGet>();
	} else if (topn.children[0]->type == LogicalOperatorType::LOGICAL_PROJECTION &&
	           topn.children[0]->children.size() == 1 &&
	           topn.children[0]->children[0]->type == LogicalOperatorType::LOGICAL_GET) {
		proj = &topn.children[0]->Cast<LogicalProjection>();
		get = &topn.children[0]->children[0]->Cast<LogicalGet>();
	}
	if (!get) {
		return false;
	}

	// Resolve the ORDER BY expression to a distance function call
	// If there's a Projection, the TopN ORDER BY references the Projection's output column
	auto &order_expr = topn.orders[0].expression;
	Expression *resolved_expr = order_expr.get();

	// If the ORDER BY is a column ref pointing to the Projection, resolve it
	if (proj && resolved_expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = resolved_expr->Cast<BoundColumnRefExpression>();
		if (colref.binding.table_index == proj->table_index &&
		    colref.binding.column_index < proj->expressions.size()) {
			resolved_expr = proj->expressions[colref.binding.column_index].get();
		}
	}

	if (resolved_expr->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}

	auto &func_expr = resolved_expr->Cast<BoundFunctionExpression>();
	if (!IsDistanceFunction(func_expr.function.name)) {
		return false;
	}

	// Must have exactly 2 arguments
	if (func_expr.children.size() != 2) {
		return false;
	}

	// Identify which argument is the column ref and which is the constant
	vector<float> query_vec;
	idx_t col_index = 0;
	bool found = false;

	if (IsColumnRefFromTable(*func_expr.children[0], get->table_index, col_index) &&
	    TryExtractConstantVector(*func_expr.children[1], query_vec)) {
		found = true;
	} else if (IsColumnRefFromTable(*func_expr.children[1], get->table_index, col_index) &&
	           TryExtractConstantVector(*func_expr.children[0], query_vec)) {
		found = true;
	}

	if (!found) {
		return false;
	}

	// Get the table catalog entry
	auto table_entry = get->GetTable();
	if (!table_entry) {
		return false;
	}

	auto &duck_table = table_entry->Cast<DuckTableEntry>();
	auto &storage = duck_table.GetStorage();

	auto &column_ids = get->GetColumnIds();

	// Map from logical column index in the Get to physical column id
	column_t physical_col_id = DConstants::INVALID_INDEX;
	if (col_index < column_ids.size()) {
		physical_col_id = column_ids[col_index].GetPrimaryIndex();
	}
	if (physical_col_id == DConstants::INVALID_INDEX) {
		return false;
	}

	// Try to find a matching index: GraphIndex or HybridIndex
	GraphIndex *graph_idx = nullptr;
	HybridIndex *hybrid_idx = nullptr;

	storage.GetDataTableInfo()->GetIndexes().Scan([&](Index &index) {
		if (!index.IsBound()) {
			return false;
		}
		if (index.GetIndexType() == GraphIndex::TYPE_NAME) {
			auto &idx_col_ids = index.GetColumnIds();
			for (auto &idx_col : idx_col_ids) {
				if (idx_col == physical_col_id) {
					graph_idx = &index.Cast<GraphIndex>();
					return true;
				}
			}
		} else if (index.GetIndexType() == HybridIndex::TYPE_NAME) {
			auto &idx_col_ids = index.GetColumnIds();
			// HybridIndex: first col is vector
			if (!idx_col_ids.empty() && idx_col_ids[0] == physical_col_id) {
				hybrid_idx = &index.Cast<HybridIndex>();
				return true;
			}
		}
		return false;
	});

	// Execute the ANN search
	idx_t k = topn.limit + topn.offset;
	int ef = GraphIndexConfig::DEFAULT_EF_SEARCH;
	if (static_cast<int>(k) > ef) {
		ef = static_cast<int>(k) * 2;
	}

	vector<row_t> result_row_ids;
	vector<float> result_distances;

	if (hybrid_idx) {
		// Check for pushed-down equality filter on the scalar partition column
		auto &idx_col_ids = hybrid_idx->GetColumnIds();
		if (idx_col_ids.size() >= 2) {
			column_t filter_physical_col = idx_col_ids[1];
			// Find which logical column index maps to this physical column
			idx_t filter_logical_col = DConstants::INVALID_INDEX;
			for (idx_t i = 0; i < column_ids.size(); i++) {
				if (column_ids[i].GetPrimaryIndex() == filter_physical_col) {
					filter_logical_col = i;
					break;
				}
			}

			if (filter_logical_col != DConstants::INVALID_INDEX) {
				auto &table_filters = get->table_filters;
				auto filter_it = table_filters.filters.find(filter_logical_col);
				if (filter_it != table_filters.filters.end() &&
				    filter_it->second->filter_type == TableFilterType::CONSTANT_COMPARISON) {
					auto &const_filter = filter_it->second->Cast<ConstantFilter>();
					if (const_filter.comparison_type == ExpressionType::COMPARE_EQUAL) {
						string partition_key = HybridIndex::ValueToPartitionKey(const_filter.constant);
						hybrid_idx->FilteredSearch(partition_key, query_vec.data(), k, ef,
						                           result_row_ids, result_distances);
					}
				}
			}
		}

		// If no filter matched, do global search
		if (result_row_ids.empty()) {
			hybrid_idx->GlobalSearch(query_vec.data(), k, ef, result_row_ids, result_distances);
		}
	} else if (graph_idx) {
		graph_idx->ANNSearch(query_vec.data(), k, ef, result_row_ids, result_distances);
	} else {
		return false;
	}

	if (result_row_ids.empty()) {
		return false;
	}

	if (!get->projection_ids.empty()) {
		return false;
	}

	auto collection = FetchRowsByIds(context, duck_table, get, result_row_ids, topn.limit, topn.offset);
	auto column_data_get = make_uniq<LogicalColumnDataGet>(get->table_index, get->types, std::move(collection));

	if (topn.children[0]->type == LogicalOperatorType::LOGICAL_GET) {
		node = std::move(column_data_get);
	} else if (topn.children[0]->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		topn.children[0]->children[0] = std::move(column_data_get);
		node = std::move(topn.children[0]);
	}
	return true;
}

// Try to extract an equality filter: col_ref = constant
static bool TryExtractEqualityFilter(const Expression &expr, idx_t table_index,
                                     idx_t &out_col_index, Value &out_value) {
	if (expr.GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
		return false;
	}
	auto &cmp = expr.Cast<BoundComparisonExpression>();

	// Try left = column, right = constant
	idx_t col_idx;
	if (IsColumnRefFromTable(*cmp.left, table_index, col_idx) &&
	    cmp.right->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		out_col_index = col_idx;
		out_value = cmp.right->Cast<BoundConstantExpression>().value;
		return true;
	}
	// Try left = constant, right = column
	if (IsColumnRefFromTable(*cmp.right, table_index, col_idx) &&
	    cmp.left->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		out_col_index = col_idx;
		out_value = cmp.left->Cast<BoundConstantExpression>().value;
		return true;
	}
	return false;
}

bool VexOptimizerExtension::TryOptimizeHybridTopN(ClientContext &context, unique_ptr<LogicalOperator> &node) {
	auto &topn = node->Cast<LogicalTopN>();

	// Only handle single ORDER BY ascending
	if (topn.orders.size() != 1 || topn.orders[0].type != OrderType::ASCENDING) {
		return false;
	}

	// ORDER BY expression must be a distance function call
	auto &order_expr = topn.orders[0].expression;
	if (order_expr->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &func_expr = order_expr->Cast<BoundFunctionExpression>();
	if (!IsDistanceFunction(func_expr.function.name) || func_expr.children.size() != 2) {
		return false;
	}

	// Pattern: TopN -> Filter -> Get
	if (topn.children[0]->type != LogicalOperatorType::LOGICAL_FILTER) {
		return false;
	}
	auto &filter = topn.children[0]->Cast<LogicalFilter>();

	if (filter.children[0]->type != LogicalOperatorType::LOGICAL_GET) {
		return false;
	}
	auto *get = &filter.children[0]->Cast<LogicalGet>();

	// Extract the query vector and column ref from distance function
	vector<float> query_vec;
	idx_t vec_col_index = 0;
	bool found = false;

	if (IsColumnRefFromTable(*func_expr.children[0], get->table_index, vec_col_index) &&
	    TryExtractConstantVector(*func_expr.children[1], query_vec)) {
		found = true;
	} else if (IsColumnRefFromTable(*func_expr.children[1], get->table_index, vec_col_index) &&
	           TryExtractConstantVector(*func_expr.children[0], query_vec)) {
		found = true;
	}
	if (!found) {
		return false;
	}

	// Extract equality filter from the Filter node
	// We support a single equality filter for now
	if (filter.expressions.size() != 1) {
		return false;
	}

	idx_t filter_col_index = 0;
	Value filter_value;
	if (!TryExtractEqualityFilter(*filter.expressions[0], get->table_index, filter_col_index, filter_value)) {
		return false;
	}

	// Get table
	auto table_entry = get->GetTable();
	if (!table_entry) {
		return false;
	}

	auto &duck_table = table_entry->Cast<DuckTableEntry>();
	auto &storage = duck_table.GetStorage();

	// Map logical column indices to physical column IDs
	auto &column_ids = get->GetColumnIds();
	column_t vec_physical_col = DConstants::INVALID_INDEX;
	column_t filter_physical_col = DConstants::INVALID_INDEX;

	if (vec_col_index < column_ids.size()) {
		vec_physical_col = column_ids[vec_col_index].GetPrimaryIndex();
	}
	if (filter_col_index < column_ids.size()) {
		filter_physical_col = column_ids[filter_col_index].GetPrimaryIndex();
	}

	if (vec_physical_col == DConstants::INVALID_INDEX || filter_physical_col == DConstants::INVALID_INDEX) {
		return false;
	}

	// Find a HybridIndex that covers both columns
	HybridIndex *hybrid_idx = nullptr;
	storage.GetDataTableInfo()->GetIndexes().Scan([&](Index &index) {
		if (index.GetIndexType() != HybridIndex::TYPE_NAME) {
			return false;
		}
		if (!index.IsBound()) {
			return false;
		}
		auto &idx_col_ids = index.GetColumnIds();
		// HybridIndex: first col is vector, second is scalar filter
		if (idx_col_ids.size() >= 2 &&
		    idx_col_ids[0] == vec_physical_col &&
		    idx_col_ids[1] == filter_physical_col) {
			hybrid_idx = &index.Cast<HybridIndex>();
			return true;
		}
		return false;
	});

	if (!hybrid_idx) {
		return false;
	}

	// Execute filtered ANN search
	idx_t k = topn.limit + topn.offset;
	int ef = GraphIndexConfig::DEFAULT_EF_SEARCH;
	if (static_cast<int>(k) > ef) {
		ef = static_cast<int>(k) * 2;
	}

	string partition_key = HybridIndex::ValueToPartitionKey(filter_value);
	vector<row_t> result_row_ids;
	vector<float> result_distances;
	hybrid_idx->FilteredSearch(partition_key, query_vec.data(), k, ef, result_row_ids, result_distances);

	if (result_row_ids.empty()) {
		return false;
	}

	auto collection = FetchRowsByIds(context, duck_table, get, result_row_ids, topn.limit, topn.offset);
	node = make_uniq<LogicalColumnDataGet>(get->table_index, get->types, std::move(collection));
	return true;
}

} // namespace duckdb
