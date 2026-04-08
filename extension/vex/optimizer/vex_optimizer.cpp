#include "vex_optimizer.hpp"
#include "vex_graph_index.hpp"
#ifdef VEX_ENABLE_HYBRID_INDEX
#include "vex_hybrid_index.hpp"
#endif
#include "vex_distance.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_column_data_get.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "vex_physical_index_scan.hpp"

// #define VEX_OPTIMIZER_DEBUG
#ifdef VEX_OPTIMIZER_DEBUG
#include <iostream>
#endif

namespace duckdb {

//===--------------------------------------------------------------------===//
// Helper utilities
//===--------------------------------------------------------------------===//

static int GetEfSearch(ClientContext &context, idx_t k) {
	Value val;
	int ef = GraphIndexConfig::DEFAULT_EF_SEARCH;
	if (context.TryGetCurrentSetting("vex_ef_search", val)) {
		ef = val.GetValue<int>();
		if (ef < 1 || ef > 10000) {
			throw InvalidInputException("vex_ef_search must be between 1 and 10000, got %d", ef);
		}
	}
	if (static_cast<int>(k) > ef) {
		ef = static_cast<int>(k) * 2;
	}
	return ef;
}

static idx_t GetBruteForceThreshold(ClientContext &context) {
	Value val;
	if (context.TryGetCurrentSetting("vex_brute_force_threshold", val)) {
		auto v = val.GetValue<idx_t>();
		if (v > 1000000) {
			throw InvalidInputException("vex_brute_force_threshold must be <= 1000000, got %llu",
			                            static_cast<unsigned long long>(v));
		}
		return v;
	}
	return GraphIndexCore::BRUTE_FORCE_THRESHOLD;
}

// Table-driven distance function registry — single source of truth for
// function recognition, metric mapping, and sort direction requirement
struct DistanceFuncEntry {
	const char *name;
	vex::VexMetric metric;
	bool requires_desc; // true = ORDER BY ... DESC to use index
};

static const DistanceFuncEntry kDistanceFuncs[] = {
    // L2 family
    {"l2_distance",                    vex::VexMetric::L2,            false},
    {"list_distance",                  vex::VexMetric::L2,            false},
    {"array_distance",                 vex::VexMetric::L2,            false},
    {"<->",                            vex::VexMetric::L2,            false},
    // Cosine family
    {"cosine_distance",                vex::VexMetric::COSINE,        false},
    {"list_cosine_distance",           vex::VexMetric::COSINE,        false},
    {"array_cosine_distance",          vex::VexMetric::COSINE,        false},
    {"<=>",                            vex::VexMetric::COSINE,        false},
    // Inner product (positive — needs DESC)
    {"inner_product",                  vex::VexMetric::INNER_PRODUCT, true},
    {"array_inner_product",            vex::VexMetric::INNER_PRODUCT, true},
    {"list_inner_product",             vex::VexMetric::INNER_PRODUCT, true},
    // Negative inner product (needs ASC like L2/Cosine)
    {"<~>",                            vex::VexMetric::INNER_PRODUCT, false},
    {"list_negative_inner_product",    vex::VexMetric::INNER_PRODUCT, false},
    {"array_negative_inner_product",   vex::VexMetric::INNER_PRODUCT, false},
};

static const DistanceFuncEntry *FindDistanceFunc(const string &name) {
	for (auto &entry : kDistanceFuncs) {
		if (name == entry.name) {
			return &entry;
		}
	}
	return nullptr;
}

static bool IsDistanceFunction(const string &name) {
	return FindDistanceFunc(name) != nullptr;
}

static vex::VexMetric GetFunctionMetric(const string &name) {
	auto *entry = FindDistanceFunc(name);
	return entry ? entry->metric : vex::VexMetric::L2;
}

static bool RequiresDescending(const string &name) {
	auto *entry = FindDistanceFunc(name);
	return entry ? entry->requires_desc : false;
}

static bool IsColumnRefFromTable(const Expression &expr, idx_t table_index, idx_t &col_index) {
	const Expression *cur = &expr;
	// Unwrap any BOUND_CAST layers (e.g., CAST(vec AS FLOAT[3]))
	while (cur->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		cur = cur->Cast<BoundCastExpression>().child.get();
	}
	if (cur->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return false;
	}
	auto &colref = cur->Cast<BoundColumnRefExpression>();
	if (colref.binding.table_index != table_index) {
		return false;
	}
	col_index = colref.binding.column_index;
	return true;
}

//===--------------------------------------------------------------------===//
// Extract constant float vector from expression
//===--------------------------------------------------------------------===//

// Try to extract a constant float array from a BOUND_CONSTANT expression
static bool TryExtractFromConstant(const Expression &expr, vector<float> &out_vec) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	auto &const_expr = expr.Cast<BoundConstantExpression>();
	auto &val = const_expr.value;
	auto &type = val.type();
	if (type.id() != LogicalTypeId::ARRAY) {
		return false;
	}
	auto child_type_id = ArrayType::GetChildType(type).id();
	auto &children = ArrayValue::GetChildren(val);
	out_vec.clear();
	out_vec.reserve(children.size());
	if (child_type_id == LogicalTypeId::FLOAT) {
		for (auto &child : children) {
			out_vec.push_back(FloatValue::Get(child));
		}
	} else {
		// Handle DOUBLE, DECIMAL, INTEGER, etc. by casting each element to FLOAT
		for (auto &child : children) {
			try {
				auto float_val = child.DefaultCastAs(LogicalType::FLOAT);
				out_vec.push_back(FloatValue::Get(float_val));
			} catch (...) {
				out_vec.clear();
				return false;
			}
		}
	}
	return !out_vec.empty();
}

// Try to extract floats from a list_value(...) function call where all args are constants
static bool TryExtractFromListValue(const Expression &expr, vector<float> &out_vec) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &func_expr = expr.Cast<BoundFunctionExpression>();
#ifdef VEX_OPTIMIZER_DEBUG
	std::cerr << "[VEX] TryExtractFromListValue: func_name='" << func_expr.function.name
	          << "' children=" << func_expr.children.size() << std::endl;
	if (!func_expr.children.empty()) {
		std::cerr << "[VEX]   first child class=" << (int)func_expr.children[0]->GetExpressionClass();
		if (func_expr.children[0]->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			auto &cv = func_expr.children[0]->Cast<BoundConstantExpression>();
			std::cerr << " type=" << cv.value.type().ToString();
		}
		std::cerr << std::endl;
	}
#endif
	if (func_expr.function.name != "list_value") {
		return false;
	}
	out_vec.clear();
	out_vec.reserve(func_expr.children.size());
	for (auto &child : func_expr.children) {
		// Unwrap CAST layers (DECIMAL values are wrapped in BOUND_CAST → FLOAT)
		const Expression *elem = child.get();
		while (elem->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			elem = elem->Cast<BoundCastExpression>().child.get();
		}
		if (elem->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			return false;
		}
		auto &const_child = elem->Cast<BoundConstantExpression>();
		auto &val = const_child.value;
		// Convert any numeric type to float
		try {
			auto float_val = val.DefaultCastAs(LogicalType::FLOAT);
			out_vec.push_back(FloatValue::Get(float_val));
		} catch (...) {
			return false;
		}
	}
	return !out_vec.empty();
}

// Extract a constant float vector from an expression, handling:
// 1. Direct BOUND_CONSTANT (ARRAY type) - post-optimize / constant-folded
// 2. BOUND_CAST(...(list_value(...))) - pre-optimize, possibly nested CASTs
// 3. BOUND_CAST(BOUND_CONSTANT) - cast of already-folded constant
static bool TryExtractConstantVector(const Expression &expr, vector<float> &out_vec) {
	// Unwrap all CAST layers to find the innermost expression
	const Expression *cur = &expr;
	while (cur->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		cur = cur->Cast<BoundCastExpression>().child.get();
	}

	// Try as constant array
	if (TryExtractFromConstant(*cur, out_vec)) {
		return true;
	}

	// Try as list_value(...)
	if (TryExtractFromListValue(*cur, out_vec)) {
#ifdef VEX_OPTIMIZER_DEBUG
		std::cerr << "[VEX] Extracted " << out_vec.size() << " floats from list_value:";
		for (size_t i = 0; i < std::min(out_vec.size(), (size_t)5); i++) {
			std::cerr << " " << out_vec[i];
		}
		if (out_vec.size() > 5) std::cerr << " ...";
		std::cerr << std::endl;
#endif
		return true;
	}

	return false;
}

//===--------------------------------------------------------------------===//
// Shared: parsed distance order info
//===--------------------------------------------------------------------===//

struct DistanceOrderInfo {
	BoundFunctionExpression *func_expr = nullptr;
	vector<float> query_vec;           // constant path: extracted at optimize time
	Expression *query_vec_expr = nullptr; // deferred path: expression to evaluate at runtime
	idx_t col_index = 0;
	bool needs_desc = false;
	bool deferred = false;             // true = query_vec_expr needs runtime evaluation
};

static bool TryResolveDistanceOrder(Expression *order_expr, LogicalGet *get, LogicalProjection *proj,
                                    OrderType order_type, DistanceOrderInfo &info) {
	Expression *resolved = order_expr;

	// Unwrap CAST layers on the ORDER BY expression itself
	while (resolved->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		resolved = resolved->Cast<BoundCastExpression>().child.get();
	}

#ifdef VEX_OPTIMIZER_DEBUG
	std::cerr << "[VEX] TryResolveDistanceOrder: expr_class=" << (int)resolved->GetExpressionClass()
	          << " order_type=" << (int)order_type << std::endl;
#endif

	if (proj && resolved->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = resolved->Cast<BoundColumnRefExpression>();
#ifdef VEX_OPTIMIZER_DEBUG
		std::cerr << "[VEX]   colref: table_index=" << colref.binding.table_index
		          << " col_index=" << colref.binding.column_index
		          << " proj->table_index=" << proj->table_index
		          << " proj->expressions.size()=" << proj->expressions.size() << std::endl;
#endif
		if (colref.binding.table_index == proj->table_index &&
		    colref.binding.column_index < proj->expressions.size()) {
			resolved = proj->expressions[colref.binding.column_index].get();
#ifdef VEX_OPTIMIZER_DEBUG
			std::cerr << "[VEX]   resolved through projection to class=" << (int)resolved->GetExpressionClass() << std::endl;
#endif
		}
	}

	// Unwrap CAST layers after projection resolution (e.g., CAST(distance_func(...) AS REAL))
	while (resolved->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
		resolved = resolved->Cast<BoundCastExpression>().child.get();
	}

	if (resolved->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
#ifdef VEX_OPTIMIZER_DEBUG
		std::cerr << "[VEX]   NOT a BOUND_FUNCTION, class=" << (int)resolved->GetExpressionClass() << std::endl;
#endif
		return false;
	}

	info.func_expr = &resolved->Cast<BoundFunctionExpression>();
	if (!IsDistanceFunction(info.func_expr->function.name) || info.func_expr->children.size() != 2) {
#ifdef VEX_OPTIMIZER_DEBUG
		std::cerr << "[VEX]   not a distance function or wrong arg count: name="
		          << info.func_expr->function.name << " children=" << info.func_expr->children.size() << std::endl;
#endif
		return false;
	}

	info.needs_desc = RequiresDescending(info.func_expr->function.name);
	if (info.needs_desc && order_type != OrderType::DESCENDING) {
		return false;
	}
	if (!info.needs_desc && order_type != OrderType::ASCENDING) {
		return false;
	}

#ifdef VEX_OPTIMIZER_DEBUG
	std::cerr << "[VEX]   func=" << info.func_expr->function.name
	          << " child[0].class=" << (int)info.func_expr->children[0]->GetExpressionClass()
	          << " child[1].class=" << (int)info.func_expr->children[1]->GetExpressionClass()
	          << " get->table_index=" << get->table_index << std::endl;
	// If child is a column ref, show its binding
	for (int ci = 0; ci < 2; ci++) {
		auto *c = info.func_expr->children[ci].get();
		if (c->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			auto &cr = c->Cast<BoundColumnRefExpression>();
			std::cerr << "[VEX]   child[" << ci << "] colref: table=" << cr.binding.table_index << " col=" << cr.binding.column_index << std::endl;
		} else if (c->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			auto &cast = c->Cast<BoundCastExpression>();
			std::cerr << "[VEX]   child[" << ci << "] is BOUND_CAST, inner class=" << (int)cast.child->GetExpressionClass() << std::endl;
			if (cast.child->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
				auto &cr = cast.child->Cast<BoundColumnRefExpression>();
				std::cerr << "[VEX]     inner colref: table=" << cr.binding.table_index << " col=" << cr.binding.column_index << std::endl;
			}
		}
	}
#endif

	// Try direct constant extraction first (fastest path)
	if (IsColumnRefFromTable(*info.func_expr->children[0], get->table_index, info.col_index) &&
	    TryExtractConstantVector(*info.func_expr->children[1], info.query_vec)) {
		return true;
	}
	if (IsColumnRefFromTable(*info.func_expr->children[1], get->table_index, info.col_index) &&
	    TryExtractConstantVector(*info.func_expr->children[0], info.query_vec)) {
		return true;
	}

	// Fallback: accept non-constant vector expression for deferred (runtime) evaluation.
	// This handles cases like subqueries: ORDER BY dist(col, (SELECT vec FROM tmp)) LIMIT k
	// where the query vector is not known at optimize time.
	for (idx_t col_side = 0; col_side < 2; col_side++) {
		idx_t vec_side = 1 - col_side;
		if (IsColumnRefFromTable(*info.func_expr->children[col_side], get->table_index, info.col_index)) {
			// The other side is a non-constant expression (subquery result, parameter, etc.)
			// Verify it is NOT also a column ref from the same table (that would be col-vs-col, not ANN)
			idx_t dummy_col;
			if (IsColumnRefFromTable(*info.func_expr->children[vec_side], get->table_index, dummy_col)) {
				continue; // both sides are columns from the same table — not an ANN pattern
			}
			info.query_vec_expr = info.func_expr->children[vec_side].get();
			info.deferred = true;
#ifdef VEX_OPTIMIZER_DEBUG
			std::cerr << "[VEX] TryResolveDistanceOrder: deferred mode — query vector expression at runtime" << std::endl;
#endif
			return true;
		}
	}

	return false;
}

//===--------------------------------------------------------------------===//
// Shared: index lookup
//===--------------------------------------------------------------------===//

struct IndexMatch {
	GraphIndex *graph_idx = nullptr;
#ifdef VEX_ENABLE_HYBRID_INDEX
	HybridIndex *hybrid_idx = nullptr;
#endif
};

static bool TryFindMatchingIndex(ClientContext &context, DataTable &storage,
                                 const vector<ColumnIndex> &column_ids,
                                 idx_t col_index, const string &func_name, IndexMatch &match) {
	column_t physical_col_id = DConstants::INVALID_INDEX;
	if (col_index < column_ids.size()) {
		physical_col_id = column_ids[col_index].GetPrimaryIndex();
	}
	if (physical_col_id == DConstants::INVALID_INDEX) {
		return false;
	}

	auto query_metric = GetFunctionMetric(func_name);

	// Check if there are unbound VEX indexes on the target column.
	// After DB reopen, indexes are lazy-deserialized and IsBound() returns false.
	bool needs_bind = false;
	for (auto &index : storage.GetDataTableInfo()->GetIndexes().Indexes()) {
		if (!index.IsBound()) {
			auto &idx_type = index.GetIndexType();
			if (idx_type == GraphIndex::TYPE_NAME
#ifdef VEX_ENABLE_HYBRID_INDEX
			    || idx_type == HybridIndex::TYPE_NAME
#endif
			    ) {
				for (auto &idx_col : index.GetColumnIds()) {
					if (idx_col == physical_col_id) {
						needs_bind = true;
						break;
					}
				}
				if (needs_bind) {
					break;
				}
			}
		}
	}

	if (needs_bind) {
		storage.GetDataTableInfo()->GetIndexes().Bind(context, *storage.GetDataTableInfo());
	}

	// Now scan bound indexes for a match.
	bool found_match = false;
	for (auto &index : storage.GetDataTableInfo()->GetIndexes().Indexes()) {
		if (!index.IsBound()) {
			continue;
		}
		if (index.GetIndexType() == GraphIndex::TYPE_NAME) {
			if (index.Cast<GraphIndex>().GetMetric() != query_metric) {
				continue; // metric mismatch, continue scanning
			}
			for (auto &idx_col : index.GetColumnIds()) {
				if (idx_col == physical_col_id) {
					match.graph_idx = &index.Cast<GraphIndex>();
					found_match = true;
					break;
				}
			}
		}
#ifdef VEX_ENABLE_HYBRID_INDEX
		else if (index.GetIndexType() == HybridIndex::TYPE_NAME) {
			if (index.Cast<HybridIndex>().GetMetric() != query_metric) {
				continue; // metric mismatch, continue scanning
			}
			auto &idx_col_ids = index.GetColumnIds();
			if (!idx_col_ids.empty() && idx_col_ids[0] == physical_col_id) {
				match.hybrid_idx = &index.Cast<HybridIndex>();
				found_match = true;
			}
		}
#endif
		if (found_match) {
			break;
		}
	}

	return match.graph_idx
#ifdef VEX_ENABLE_HYBRID_INDEX
	       || match.hybrid_idx
#endif
	       ;
}

//===--------------------------------------------------------------------===//
// Shared: execute ANN search
//===--------------------------------------------------------------------===//

static bool ExecuteANNSearch(ClientContext &context, const IndexMatch &match, LogicalGet *get,
                             const float *query_vec, idx_t k,
                             vector<row_t> &result_row_ids, vector<float> &result_distances) {
	int ef = GetEfSearch(context, k);
	idx_t bf_threshold = GetBruteForceThreshold(context);

	if (match.graph_idx) {
		match.graph_idx->ANNSearch(query_vec, k, ef, result_row_ids, result_distances, bf_threshold);
	}
#ifdef VEX_ENABLE_HYBRID_INDEX
	else if (match.hybrid_idx) {
		bool filtered = false;
		auto &idx_col_ids = match.hybrid_idx->GetColumnIds();
		if (idx_col_ids.size() >= 2) {
			column_t filter_physical_col = idx_col_ids[1];
			auto &column_ids = get->GetColumnIds();
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
				if (filter_it != table_filters.filters.end()) {
					auto &tf = *filter_it->second;
					if (tf.filter_type == TableFilterType::CONSTANT_COMPARISON) {
						auto &const_filter = tf.Cast<ConstantFilter>();
						if (const_filter.comparison_type == ExpressionType::COMPARE_EQUAL) {
							string partition_key = HybridIndex::ValueToPartitionKey(const_filter.constant);
							match.hybrid_idx->FilteredSearch(partition_key, query_vec, k, ef,
							                                 result_row_ids, result_distances, bf_threshold);
							filtered = true;
						}
					} else if (tf.filter_type == TableFilterType::IN_FILTER) {
						auto &in_filter = tf.Cast<InFilter>();
						// Search each partition in the IN list, then merge top-k
						std::vector<std::pair<row_t, float>> all_results;
						for (auto &val : in_filter.values) {
							string partition_key = HybridIndex::ValueToPartitionKey(val);
							std::vector<row_t> part_ids;
							std::vector<float> part_dists;
							match.hybrid_idx->FilteredSearch(partition_key, query_vec, k, ef,
							                                 part_ids, part_dists, bf_threshold);
							for (idx_t i = 0; i < part_ids.size(); i++) {
								all_results.push_back(std::make_pair(part_ids[i], part_dists[i]));
							}
						}
						std::sort(all_results.begin(), all_results.end(),
						          [](const std::pair<row_t, float> &a, const std::pair<row_t, float> &b) {
							          return a.second < b.second;
						          });
						idx_t count = std::min(k, static_cast<idx_t>(all_results.size()));
						for (idx_t i = 0; i < count; i++) {
							result_row_ids.push_back(all_results[i].first);
							result_distances.push_back(all_results[i].second);
						}
						filtered = true;
					}
				}
			}
		}

		if (!filtered) {
			match.hybrid_idx->GlobalSearch(query_vec, k, ef, result_row_ids, result_distances, bf_threshold);
		}
	}
#endif
	else {
		return false;
	}

	return !result_row_ids.empty();
}

//===--------------------------------------------------------------------===//
// Shared: fetch rows by row_ids
//===--------------------------------------------------------------------===//

// Get the output types for a LogicalGet node.
// In pre-optimize stage, get->types may be empty. returned_types is indexed by
// physical column position, but column_ids may be in arbitrary order.
// We must build the output types by mapping column_ids through returned_types.
static vector<LogicalType> GetOutputTypes(LogicalGet *get) {
	if (!get->types.empty()) {
		return get->types;
	}
	// Build output types from column_ids + returned_types
	auto &column_ids = get->GetColumnIds();
	vector<LogicalType> output_types;
	output_types.reserve(column_ids.size());
	for (idx_t i = 0; i < column_ids.size(); i++) {
		if (column_ids[i].IsVirtualColumn()) {
			output_types.push_back(LogicalType::ROW_TYPE);
		} else {
			idx_t physical_idx = column_ids[i].GetPrimaryIndex();
			if (physical_idx < get->returned_types.size()) {
				output_types.push_back(get->returned_types[physical_idx]);
			} else {
				throw InternalException("VEX optimizer: column physical index %llu out of range (returned_types size %llu)",
				                        physical_idx, get->returned_types.size());
			}
		}
	}
	return output_types;
}

static unique_ptr<ColumnDataCollection> FetchRowsByIds(ClientContext &context, DuckTableEntry &duck_table,
                                                       LogicalGet *get, const vector<row_t> &result_row_ids,
                                                       idx_t limit, idx_t offset) {
	auto &storage = duck_table.GetStorage();
	auto &db = duck_table.ParentCatalog().GetAttached();
	auto &transaction = DuckTransaction::Get(context, db);

	vector<row_t> visible_row_ids;
	visible_row_ids.reserve(result_row_ids.size());
	for (auto &rid : result_row_ids) {
		if (storage.CanFetch(transaction, rid)) {
			visible_row_ids.push_back(rid);
		}
	}

	auto output_types = GetOutputTypes(get);
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

	idx_t total = visible_row_ids.size();
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
			row_id_data[i] = visible_row_ids[off + i];
		}

		storage.Fetch(transaction, fetch_chunk, fetch_col_ids, row_id_vec, batch, fetch_state);

#ifdef VEX_OPTIMIZER_DEBUG
		std::cerr << "[VEX] FetchRowsByIds: batch=" << batch
		          << " fetch_chunk.size()=" << fetch_chunk.size()
		          << " fetch_col_ids.size()=" << fetch_col_ids.size() << std::endl;
		for (idx_t i = 0; i < output_types.size(); i++) {
			std::cerr << "[VEX]   output_type[" << i << "]=" << output_types[i].ToString() << std::endl;
		}
		for (idx_t i = 0; i < fetch_types.size(); i++) {
			std::cerr << "[VEX]   fetch_type[" << i << "]=" << fetch_types[i].ToString() << std::endl;
		}
#endif

		for (idx_t f = 0; f < fetch_output_positions.size(); f++) {
			output_chunk.data[fetch_output_positions[f]].Reference(fetch_chunk.data[f]);
		}
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

#ifdef VEX_OPTIMIZER_DEBUG
	std::cerr << "[VEX] FetchRowsByIds: collection count=" << collection->Count() << std::endl;
#endif

	return collection;
}

//===--------------------------------------------------------------------===//
// Shared: find LogicalGet child
//===--------------------------------------------------------------------===//

struct GetChildInfo {
	LogicalGet *get = nullptr;
	LogicalProjection *proj = nullptr;
	LogicalFilter *filter = nullptr;
	LogicalOperator *cross_product = nullptr;  // non-null when GET was found inside CROSS_PRODUCT
	idx_t subquery_child_idx = 0;              // index of the subquery child in cross_product
};

// Find the GET node through optional PROJECTION and FILTER layers.
// Supported patterns:
//   GET
//   PROJECTION -> GET
//   FILTER -> GET
//   PROJECTION -> FILTER -> GET
static bool FindGetChild(LogicalOperator &child, GetChildInfo &info) {
	LogicalOperator *cur = &child;

	// Optional PROJECTION layer
	if (cur->type == LogicalOperatorType::LOGICAL_PROJECTION && cur->children.size() == 1) {
		info.proj = &cur->Cast<LogicalProjection>();
		cur = cur->children[0].get();
	}

	// Optional FILTER layer
	if (cur->type == LogicalOperatorType::LOGICAL_FILTER && cur->children.size() == 1) {
		info.filter = &cur->Cast<LogicalFilter>();
		cur = cur->children[0].get();
	}

	// Direct GET
	if (cur->type == LogicalOperatorType::LOGICAL_GET) {
		info.get = &cur->Cast<LogicalGet>();
		return true;
	}

	// CROSS_PRODUCT from scalar subquery flattening:
	// DuckDB converts `ORDER BY dist(col, (SELECT ...))` into
	// CROSS_PRODUCT -> [GET, subquery_pipeline].
	// The GET side is the main table; the other side is the single-row subquery result.
	if (cur->type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT && cur->children.size() == 2) {
		for (idx_t i = 0; i < 2; i++) {
			if (cur->children[i]->type == LogicalOperatorType::LOGICAL_GET) {
				info.get = &cur->children[i]->Cast<LogicalGet>();
				info.cross_product = cur;
				info.subquery_child_idx = 1 - i;
				return true;
			}
		}
	}

	return false;
}

static void ReplacePlanWithResults(unique_ptr<LogicalOperator> &node, unique_ptr<LogicalOperator> &child_owner,
                                   const GetChildInfo &info, unique_ptr<LogicalColumnDataGet> column_data_get) {
	auto cardinality = column_data_get->estimated_cardinality;
	if (info.proj) {
		child_owner->children[0] = std::move(column_data_get);
		child_owner->SetEstimatedCardinality(cardinality);
		node = std::move(child_owner);
	} else {
		node = std::move(column_data_get);
	}
}

// Forward declaration
static bool TryExtractEqualityFilter(const Expression &expr, idx_t table_index,
                                     idx_t &out_col_index, Value &out_value);

//===--------------------------------------------------------------------===//
// Shared: core ANN optimization logic
//===--------------------------------------------------------------------===//

static bool TryOptimizeANN(ClientContext &context, unique_ptr<LogicalOperator> &node,
                           unique_ptr<LogicalOperator> &get_owner, const GetChildInfo &get_info,
                           const DistanceOrderInfo &dist_info, idx_t limit, idx_t offset) {
	auto *get = get_info.get;

	if (!get->projection_ids.empty()) {
		return false;
	}

	auto table_entry = get->GetTable();
	if (!table_entry) {
		return false;
	}

	auto &duck_table = table_entry->Cast<DuckTableEntry>();
	auto &storage = duck_table.GetStorage();

	// Skip index optimization if current transaction has uncommitted changes.
	// - INSERT: new rows live in LocalStorage, not in the index → would be missed
	// - DELETE: CanFetch filters deleted rows, may return fewer than k results
	// - UPDATE: combination of delete + insert issues
	// In autocommit mode each statement is its own transaction (no pending changes),
	// so this check only triggers inside explicit BEGIN/COMMIT blocks.
	auto &db = duck_table.ParentCatalog().GetAttached();
	auto &transaction = DuckTransaction::Get(context, db);
	if (transaction.ChangesMade()) {
#ifdef VEX_OPTIMIZER_DEBUG
		std::cerr << "[VEX] TryOptimizeANN: skipping — transaction has uncommitted changes" << std::endl;
#endif
		return false;
	}

	auto &column_ids = get->GetColumnIds();

	IndexMatch match;
	if (!TryFindMatchingIndex(context, storage, column_ids, dist_info.col_index, dist_info.func_expr->function.name, match)) {
		return false;
	}

	// If there's a FILTER node but the index is a plain GRAPH_INDEX (not hybrid),
	// we cannot apply the filter through ANN search — skip optimization to let
	// the regular plan handle it (full scan + filter).
	if (get_info.filter
#ifdef VEX_ENABLE_HYBRID_INDEX
	    && !match.hybrid_idx
#endif
	    ) {
#ifdef VEX_OPTIMIZER_DEBUG
		std::cerr << "[VEX] TryOptimizeANN: skipping — FILTER present but index is not hybrid" << std::endl;
#endif
		return false;
	}

	//--- Deferred path: query vector not known at optimize time ---
	// Generate a LogicalVexIndexScan that will be converted to PhysicalVexIndexScan
	// during physical planning. The physical operator evaluates the query vector
	// expression and executes the ANN search at runtime.
	//
	// For subqueries, DuckDB flattens `ORDER BY dist(col, (SELECT ...))` into:
	//   LIMIT -> ORDER_BY -> PROJECTION -> CROSS_PRODUCT -> [GET, subquery]
	// We replace CROSS_PRODUCT with LogicalVexIndexScan, keeping the subquery
	// as a child. The PROJECTION, ORDER_BY and LIMIT stay in place (ORDER_BY and
	// LIMIT become no-ops since the index returns pre-sorted top-k results).
	if (dist_info.deferred) {
		if (!match.graph_idx) {
#ifdef VEX_OPTIMIZER_DEBUG
			std::cerr << "[VEX] TryOptimizeANN: deferred mode only supports GraphIndex" << std::endl;
#endif
			return false;
		}
		if (!get_info.cross_product) {
#ifdef VEX_OPTIMIZER_DEBUG
			std::cerr << "[VEX] TryOptimizeANN: deferred mode requires CROSS_PRODUCT (subquery)" << std::endl;
#endif
			return false;
		}
#ifdef VEX_OPTIMIZER_DEBUG
		std::cerr << "[VEX] TryOptimizeANN: deferred mode — emitting LogicalVexIndexScan" << std::endl;
#endif
		idx_t k = limit + offset;

		auto output_types = GetOutputTypes(get);
		auto &column_ids = get->GetColumnIds();

		auto scan = make_uniq<LogicalVexIndexScan>(
		    get->table_index, output_types,
		    duck_table, *match.graph_idx,
		    dist_info.query_vec_expr->Copy(),
		    k,
		    column_ids, get->returned_types);
		scan->SetEstimatedCardinality(k);

		// Keep the subquery pipeline as a child so its column bindings are
		// available for the inner PROJECTION's distance expression.
		scan->children.push_back(
		    std::move(get_info.cross_product->children[get_info.subquery_child_idx]));

		// Replace CROSS_PRODUCT with LogicalVexIndexScan in the plan.
		// The PROJECTION, ORDER_BY and LIMIT stay intact above.
		if (get_info.proj) {
			// PROJECTION -> CROSS_PRODUCT  =>  PROJECTION -> LogicalVexIndexScan
			get_info.proj->children[0] = std::move(scan);
		} else if (get_info.filter) {
			// FILTER -> CROSS_PRODUCT  =>  FILTER -> LogicalVexIndexScan
			get_info.filter->children[0] = std::move(scan);
		}
		// Don't replace `node` — the LIMIT/ORDER_BY stay in the plan.
		return true;
	}

	idx_t k = limit + offset;
	vector<row_t> result_row_ids;
	vector<float> result_distances;
	int ef = GetEfSearch(context, k);
	idx_t bf_threshold = GetBruteForceThreshold(context);

	// For hybrid index with a FILTER node (pre-optimize: filter not yet pushed to GET)
	bool did_filtered_search = false;
#ifdef VEX_ENABLE_HYBRID_INDEX
#ifdef VEX_OPTIMIZER_DEBUG
	std::cerr << "[VEX] TryOptimizeANN: hybrid_idx=" << (match.hybrid_idx != nullptr)
	          << " filter=" << (get_info.filter != nullptr)
	          << " filter_exprs=" << (get_info.filter ? get_info.filter->expressions.size() : 0) << std::endl;
#endif
	if (match.hybrid_idx && get_info.filter && get_info.filter->expressions.size() == 1) {
		auto &idx_col_ids = match.hybrid_idx->GetColumnIds();
		if (idx_col_ids.size() >= 2) {
			column_t filter_physical_col = idx_col_ids[1];

			idx_t filter_col_index = 0;
			Value filter_value;
			if (TryExtractEqualityFilter(*get_info.filter->expressions[0], get->table_index,
			                             filter_col_index, filter_value)) {
				// Verify the filter column matches the hybrid index partition column
				if (filter_col_index < column_ids.size() &&
				    column_ids[filter_col_index].GetPrimaryIndex() == filter_physical_col) {
					string partition_key = HybridIndex::ValueToPartitionKey(filter_value);
					match.hybrid_idx->FilteredSearch(partition_key, dist_info.query_vec.data(), k, ef,
					                                 result_row_ids, result_distances, bf_threshold);
					did_filtered_search = true;
				}
			}
		}
	}
#endif

	// If there's a FILTER node but we couldn't apply it through the index,
	// we MUST bail out. Otherwise ReplacePlanWithResults will drop the FILTER
	// node and we'd return unfiltered results — a correctness bug.
	if (get_info.filter && !did_filtered_search) {
#ifdef VEX_OPTIMIZER_DEBUG
		std::cerr << "[VEX] TryOptimizeANN: skipping — FILTER present but filter extraction failed" << std::endl;
#endif
		return false;
	}

	// If filtered search returned empty, fall back to regular plan.
	// Empty could mean the partition has no items OR HNSW search missed remaining items after delete.
	// Either way, the regular plan (full scan + filter) will produce correct results.
	if (did_filtered_search && result_row_ids.empty()) {
#ifdef VEX_OPTIMIZER_DEBUG
		std::cerr << "[VEX] TryOptimizeANN: filtered search returned empty, falling back to regular plan" << std::endl;
#endif
		return false;
	}

	// Fallback: use ExecuteANNSearch (handles table_filters for post-optimize, or global search)
	if (!did_filtered_search && result_row_ids.empty()) {
		if (!ExecuteANNSearch(context, match, get, dist_info.query_vec.data(), k, result_row_ids, result_distances)) {
			return false;
		}
	}

#ifdef VEX_OPTIMIZER_DEBUG
	std::cerr << "[VEX] ANN search returned " << result_row_ids.size() << " results, k=" << k << std::endl;
	for (idx_t i = 0; i < result_row_ids.size() && i < 5; i++) {
		std::cerr << "[VEX]   row_id=" << result_row_ids[i] << " dist=" << result_distances[i] << std::endl;
	}
	std::cerr << "[VEX] get->types.size()=" << get->types.size()
	          << " column_ids.size()=" << column_ids.size()
	          << " projection_ids.size()=" << get->projection_ids.size() << std::endl;
	for (idx_t i = 0; i < get->types.size(); i++) {
		std::cerr << "[VEX]   type[" << i << "]=" << get->types[i].ToString() << std::endl;
	}
	for (idx_t i = 0; i < column_ids.size(); i++) {
		std::cerr << "[VEX]   col_id[" << i << "]=" << column_ids[i].GetPrimaryIndex() << std::endl;
	}
	if (get_info.proj) {
		std::cerr << "[VEX] proj->types.size()=" << get_info.proj->types.size() << std::endl;
		for (idx_t i = 0; i < get_info.proj->types.size(); i++) {
			std::cerr << "[VEX]   proj_type[" << i << "]=" << get_info.proj->types[i].ToString() << std::endl;
		}
	}
	// Also check returned_types
	std::cerr << "[VEX] get->returned_types.size()=" << get->returned_types.size() << std::endl;
#endif

	auto collection = FetchRowsByIds(context, duck_table, get, result_row_ids, limit, offset);
	auto actual_count = collection->Count();
	auto column_data_get = make_uniq<LogicalColumnDataGet>(get->table_index, GetOutputTypes(get), std::move(collection));
	column_data_get->SetEstimatedCardinality(actual_count);

	ReplacePlanWithResults(node, get_owner, get_info, std::move(column_data_get));
	return true;
}

//===--------------------------------------------------------------------===//
// Optimizer entry point
//===--------------------------------------------------------------------===//

VexOptimizerExtension::VexOptimizerExtension() {
	pre_optimize_function = OptimizeFunction;
}

void VexOptimizerExtension::OptimizeFunction(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	OptimizeNode(input.context, plan);
}

void VexOptimizerExtension::OptimizeNode(ClientContext &context, unique_ptr<LogicalOperator> &node) {
#ifdef VEX_OPTIMIZER_DEBUG
	std::cerr << "[VEX] OptimizeNode: type=" << (int)node->type
	          << " (" << LogicalOperatorToString(node->type) << ")" << std::endl;
#endif
	for (auto &child : node->children) {
		OptimizeNode(context, child);
	}

	if (node->type == LogicalOperatorType::LOGICAL_TOP_N) {
		TryOptimizeTopN(context, node);
	} else if (node->type == LogicalOperatorType::LOGICAL_LIMIT) {
		TryOptimizeLimitOrderBy(context, node);
	}
}

//===--------------------------------------------------------------------===//
// Pattern: TOP_N -> [PROJECTION ->] GET
//===--------------------------------------------------------------------===//

bool VexOptimizerExtension::TryOptimizeTopN(ClientContext &context, unique_ptr<LogicalOperator> &node) {
	auto &topn = node->Cast<LogicalTopN>();

	if (topn.orders.size() != 1) {
		return false;
	}

	GetChildInfo get_info;
	if (!FindGetChild(*topn.children[0], get_info)) {
		return false;
	}

	DistanceOrderInfo dist_info;
	if (!TryResolveDistanceOrder(topn.orders[0].expression.get(), get_info.get, get_info.proj,
	                             topn.orders[0].type, dist_info)) {
		return false;
	}

	return TryOptimizeANN(context, node, topn.children[0], get_info, dist_info, topn.limit, topn.offset);
}

//===--------------------------------------------------------------------===//
// Pattern: LIMIT -> ORDER_BY -> [PROJECTION ->] GET
//===--------------------------------------------------------------------===//

bool VexOptimizerExtension::TryOptimizeLimitOrderBy(ClientContext &context, unique_ptr<LogicalOperator> &node) {
	auto &limit_node = node->Cast<LogicalLimit>();

	if (limit_node.limit_val.Type() != LimitNodeType::CONSTANT_VALUE) {
#ifdef VEX_OPTIMIZER_DEBUG
		std::cerr << "[VEX] LimitOrderBy: limit is not constant" << std::endl;
#endif
		return false;
	}
	idx_t limit = limit_node.limit_val.GetConstantValue();

	idx_t offset = 0;
	if (limit_node.offset_val.Type() == LimitNodeType::CONSTANT_VALUE) {
		offset = limit_node.offset_val.GetConstantValue();
	}

	if (node->children.size() != 1 || node->children[0]->type != LogicalOperatorType::LOGICAL_ORDER_BY) {
#ifdef VEX_OPTIMIZER_DEBUG
		std::cerr << "[VEX] LimitOrderBy: child is not ORDER_BY, child_type="
		          << (node->children.empty() ? -1 : (int)node->children[0]->type) << std::endl;
#endif
		return false;
	}
	auto &order_node = node->children[0]->Cast<LogicalOrder>();

	if (order_node.orders.size() != 1) {
#ifdef VEX_OPTIMIZER_DEBUG
		std::cerr << "[VEX] LimitOrderBy: orders.size()=" << order_node.orders.size() << std::endl;
#endif
		return false;
	}

#ifdef VEX_OPTIMIZER_DEBUG
	std::cerr << "[VEX] LimitOrderBy: order child type=" << (int)order_node.children[0]->type
	          << " (" << LogicalOperatorToString(order_node.children[0]->type) << ")" << std::endl;
#endif

	GetChildInfo get_info;
	if (!FindGetChild(*order_node.children[0], get_info)) {
#ifdef VEX_OPTIMIZER_DEBUG
		std::cerr << "[VEX] LimitOrderBy: FindGetChild failed" << std::endl;
#endif
		return false;
	}

#ifdef VEX_OPTIMIZER_DEBUG
	std::cerr << "[VEX] LimitOrderBy: found GET, table_index=" << get_info.get->table_index
	          << " has_proj=" << (get_info.proj != nullptr) << std::endl;
	auto &order_expr = order_node.orders[0].expression;
	std::cerr << "[VEX] LimitOrderBy: order_expr class=" << (int)order_expr->GetExpressionClass()
	          << " order_type=" << (int)order_node.orders[0].type << std::endl;
#endif

	DistanceOrderInfo dist_info;
	if (!TryResolveDistanceOrder(order_node.orders[0].expression.get(), get_info.get, get_info.proj,
	                             order_node.orders[0].type, dist_info)) {
#ifdef VEX_OPTIMIZER_DEBUG
		std::cerr << "[VEX] LimitOrderBy: TryResolveDistanceOrder failed" << std::endl;
#endif
		return false;
	}

#ifdef VEX_OPTIMIZER_DEBUG
	std::cerr << "[VEX] LimitOrderBy: resolved distance func=" << dist_info.func_expr->function.name
	          << " col_index=" << dist_info.col_index << " vec_size=" << dist_info.query_vec.size() << std::endl;
#endif

	return TryOptimizeANN(context, node, order_node.children[0], get_info, dist_info, limit, offset);
}

//===--------------------------------------------------------------------===//
// Pattern: TOP_N -> FILTER -> GET  (hybrid filtered search)
//===--------------------------------------------------------------------===//

static bool TryExtractEqualityFilter(const Expression &expr, idx_t table_index,
                                     idx_t &out_col_index, Value &out_value) {
#ifdef VEX_OPTIMIZER_DEBUG
	std::cerr << "[VEX] TryExtractEqualityFilter: expr_type=" << (int)expr.GetExpressionType()
	          << " expected=" << (int)ExpressionType::COMPARE_EQUAL
	          << " table_index=" << table_index << std::endl;
#endif
	if (expr.GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
		return false;
	}
	auto &cmp = expr.Cast<BoundComparisonExpression>();

	// Unwrap CAST layers to find BOUND_CONSTANT
	auto unwrap_to_constant = [](const Expression &e) -> const BoundConstantExpression * {
		const Expression *cur = &e;
		while (cur->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
			cur = cur->Cast<BoundCastExpression>().child.get();
		}
		if (cur->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			return &cur->Cast<BoundConstantExpression>();
		}
		return nullptr;
	};

	idx_t col_idx;
	if (IsColumnRefFromTable(*cmp.left, table_index, col_idx)) {
		auto *const_expr = unwrap_to_constant(*cmp.right);
		if (const_expr) {
			out_col_index = col_idx;
			out_value = const_expr->value;
			return true;
		}
	}
	if (IsColumnRefFromTable(*cmp.right, table_index, col_idx)) {
		auto *const_expr = unwrap_to_constant(*cmp.left);
		if (const_expr) {
			out_col_index = col_idx;
			out_value = const_expr->value;
			return true;
		}
	}
	return false;
}


} // namespace duckdb
