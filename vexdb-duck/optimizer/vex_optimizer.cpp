#include "vex_optimizer.hpp"

#include <cmath>

#include "vex_fetch_utils.hpp"
#include "vex_graph_index.hpp"
#include "vex_physical_index_scan.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

namespace duckdb {

struct DistanceFuncEntry {
    const char *name;
    VexMetric metric;
    // smaller = closer ⇒ user-friendly ASC matches index ranking; for raw inner
    // product (positive a·b ⇒ similar) the user must specify DESC.
    bool ascending;
};

static const DistanceFuncEntry kDistanceFuncs[] = {
    {"l2_distance", VexMetric::L2, true},
    {"<->", VexMetric::L2, true},
    {"array_distance", VexMetric::L2, true},
    {"list_distance", VexMetric::L2, true},

    {"inner_product", VexMetric::INNER_PRODUCT, false},
    {"<#>", VexMetric::INNER_PRODUCT, false},
    {"<~>", VexMetric::INNER_PRODUCT, true},
    {"array_inner_product", VexMetric::INNER_PRODUCT, false},
    {"list_inner_product", VexMetric::INNER_PRODUCT, false},
    {"array_negative_inner_product", VexMetric::INNER_PRODUCT, true},
    {"list_negative_inner_product", VexMetric::INNER_PRODUCT, true},

    {"cosine_distance", VexMetric::COSINE, true},
    {"<=>", VexMetric::COSINE, true},
    {"array_cosine_distance", VexMetric::COSINE, true},
    {"list_cosine_distance", VexMetric::COSINE, true},
    {"array_cosine_similarity", VexMetric::COSINE, false},
    {"list_cosine_similarity", VexMetric::COSINE, false},
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

static bool DistanceFunctionMatchesMetric(const string &name, VexMetric metric) {
    auto *entry = FindDistanceFunc(name);
    return entry && entry->metric == metric;
}

static bool IsAscDistanceFunction(const string &name) {
    auto *entry = FindDistanceFunc(name);
    return entry ? entry->ascending : true;
}

static bool IsColumnRefFromTable(const Expression &expr, TableIndex table_index, idx_t &col_index) {
    const Expression *cur = &expr;
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

static bool HasColumnRefFromTable(const Expression &expr, TableIndex table_index) {
    const Expression *cur = &expr;
    while (cur->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        cur = cur->Cast<BoundCastExpression>().child.get();
    }
    if (cur->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        auto &colref = cur->Cast<BoundColumnRefExpression>();
        return colref.binding.table_index == table_index;
    }
    if (cur->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
        auto &func = cur->Cast<BoundFunctionExpression>();
        for (auto &child : func.children) {
            if (HasColumnRefFromTable(*child, table_index)) {
                return true;
            }
        }
    }
    return false;
}

struct DistanceOrderInfo {
    BoundFunctionExpression *func_expr = nullptr;
    Expression *query_vec_expr = nullptr;
    idx_t col_index = 0;
};

static bool TryResolveDistanceOrder(Expression *order_expr, LogicalGet *get, LogicalProjection *proj,
                                    OrderType order_type, DistanceOrderInfo &info) {
    Expression *resolved = order_expr;
    while (resolved->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        resolved = resolved->Cast<BoundCastExpression>().child.get();
    }

    if (proj && resolved->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        auto &colref = resolved->Cast<BoundColumnRefExpression>();
        if (colref.binding.table_index == proj->table_index && colref.binding.column_index < proj->expressions.size()) {
            resolved = proj->expressions[colref.binding.column_index].get();
        }
    }

    while (resolved->GetExpressionClass() == ExpressionClass::BOUND_CAST) {
        resolved = resolved->Cast<BoundCastExpression>().child.get();
    }
    if (resolved->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
        return false;
    }

    info.func_expr = &resolved->Cast<BoundFunctionExpression>();
    if (!IsDistanceFunction(info.func_expr->function.GetName()) || info.func_expr->children.size() != 2) {
        return false;
    }
    // For inner_product / <#> the user-facing scalar returns +(a·b) (greater = more similar),
    // so the natural query is ORDER BY ... DESC. The index ranks by -(a·b) ascending, which
    // matches; for L2 / cosine the user does ASC and the index ranks by sqrt-L2 / -cos_sim
    // ascending, also matching.
    OrderType expected = IsAscDistanceFunction(info.func_expr->function.GetName())
                             ? OrderType::ASCENDING
                             : OrderType::DESCENDING;
    if (order_type != expected) {
        return false;
    }

    for (idx_t col_side = 0; col_side < 2; col_side++) {
        idx_t vec_side = 1 - col_side;
        if (IsColumnRefFromTable(*info.func_expr->children[col_side], get->table_index, info.col_index)) {
            if (HasColumnRefFromTable(*info.func_expr->children[vec_side], get->table_index)) {
                continue;
            }
            info.query_vec_expr = info.func_expr->children[vec_side].get();
            return true;
        }
    }
    return false;
}

struct GetChildInfo {
    LogicalGet *get = nullptr;
    LogicalProjection *proj = nullptr;
    LogicalFilter *filter = nullptr;
    LogicalOperator *cross_product = nullptr;
    idx_t subquery_child_idx = 0;
    bool filter_inside_cross_product = false;
};

static bool FindGetChild(LogicalOperator &child, GetChildInfo &info) {
    LogicalOperator *cur = &child;

    if (cur->type == LogicalOperatorType::LOGICAL_PROJECTION && cur->children.size() == 1) {
        info.proj = &cur->Cast<LogicalProjection>();
        cur = cur->children[0].get();
    }

    if (cur->type == LogicalOperatorType::LOGICAL_FILTER && cur->children.size() == 1) {
        info.filter = &cur->Cast<LogicalFilter>();
        cur = cur->children[0].get();
    }

    if (cur->type == LogicalOperatorType::LOGICAL_GET) {
        info.get = &cur->Cast<LogicalGet>();
        return true;
    }

    if (cur->type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT && cur->children.size() == 2) {
        for (idx_t i = 0; i < 2; i++) {
            auto *cp_child = cur->children[i].get();
            if (cp_child->type == LogicalOperatorType::LOGICAL_GET) {
                info.get = &cp_child->Cast<LogicalGet>();
                info.cross_product = cur;
                info.subquery_child_idx = 1 - i;
                return true;
            }
            // CROSS_PRODUCT -> [FILTER -> GET, subquery]: WHERE clause + scalar
            // subquery query vector. Rewrite the GET; FILTER stays where it is and
            // post-filters VEX_INDEX_SCAN's row_id-fetched output.
            if (cp_child->type == LogicalOperatorType::LOGICAL_FILTER &&
                cp_child->children.size() == 1 &&
                cp_child->children[0]->type == LogicalOperatorType::LOGICAL_GET) {
                info.filter = &cp_child->Cast<LogicalFilter>();
                info.get = &cp_child->children[0]->Cast<LogicalGet>();
                info.cross_product = cur;
                info.subquery_child_idx = 1 - i;
                info.filter_inside_cross_product = true;
                return true;
            }
        }
    }
    return false;
}

static void ReplaceGetWithVexScan(unique_ptr<LogicalOperator> &get_owner, const GetChildInfo &info,
                                  unique_ptr<LogicalOperator> vex_scan) {
    if (info.proj) {
        info.proj->children[0] = std::move(vex_scan);
    } else if (info.filter) {
        info.filter->children[0] = std::move(vex_scan);
    } else {
        get_owner = std::move(vex_scan);
    }
}

static vector<LogicalType> GetOutputTypes(LogicalGet *get) {
    // We always derive types from the full column_ids list so that the LogicalVexIndexScan
    // exposes one column binding per column read by the original LogicalGet. Upstream
    // operators (Filter, Order) bind to columns by index into column_ids; falling back to
    // get->types here would silently drop bindings when filter pushdown adds extra read
    // columns that aren't part of the projected output (e.g. WHERE tag = '...' adds 'tag').
    return BuildOutputTypes(get->GetColumnIds(), get->returned_types);
}

struct IndexMatch {
    GraphIndex *graph_idx = nullptr;
};

// Default over-sample factor when the planner can't estimate filter selectivity.
// Picked so a moderately selective filter (~25%) still produces k results without
// burning budget on filters that are barely selective at all.
static constexpr idx_t kDefaultOversampleFactor = 4;

// Decide how many rows VEX_INDEX_SCAN should produce so that, after the post-filter
// trims, the outer TOPN still has at least k rows. When cardinality estimates are
// unavailable or the filter isn't selective, fall back to k * factor; clamp to the
// table's row count so we never ask for more than exists.
static idx_t ComputeScanK(idx_t k, idx_t get_card, idx_t filter_card) {
    idx_t k_scan;
    if (get_card > 0 && filter_card > 0 && filter_card < get_card) {
        double selectivity = static_cast<double>(filter_card) / static_cast<double>(get_card);
        k_scan = static_cast<idx_t>(std::ceil(static_cast<double>(k) / selectivity));
    } else {
        k_scan = k * kDefaultOversampleFactor;
    }
    if (get_card > 0) {
        k_scan = MinValue<idx_t>(k_scan, get_card);
    }
    return MaxValue<idx_t>(k_scan, k);
}

static bool TryFindMatchingIndex(ClientContext &context, DataTable &storage, const vector<ColumnIndex> &column_ids,
                                 idx_t col_index, const string &distance_func, IndexMatch &match) {
    column_t physical_col_id = DConstants::INVALID_INDEX;
    if (col_index < column_ids.size()) {
        physical_col_id = column_ids[col_index].GetPrimaryIndex();
    }
    if (physical_col_id == DConstants::INVALID_INDEX) {
        return false;
    }

    auto &table_info = *storage.GetDataTableInfo();
    // After DB reopen, GRAPH_INDEX entries are lazy-deserialized — IsBound() returns
    // false until something triggers Bind(). Without this call the loop below skips
    // every unbound GRAPH_INDEX and the optimizer falls back to a sequential scan.
    // BindIndexes is the same API used by physical_insert / table_scan; it short-
    // circuits when there are no unbound entries, and must be called *before* we
    // enter the Indexes() iterator (which holds index_entries_lock).
    if (table_info.GetIndexes().HasUnbound()) {
        table_info.BindIndexes(context, GraphIndex::TYPE_NAME);
    }
    auto &index_list = table_info.GetIndexes();
    GraphIndex *fallback = nullptr;
    for (auto &index : index_list.Indexes()) {
        if (!index.IsBound()) {
            continue;
        }
        auto &bound = index.Cast<BoundIndex>();
        if (bound.GetIndexType() != GraphIndex::TYPE_NAME) {
            continue;
        }
        if (bound.GetColumnIds().empty() || bound.GetColumnIds()[0] != physical_col_id) {
            continue;
        }
        auto &candidate = bound.Cast<GraphIndex>();
        if (DistanceFunctionMatchesMetric(distance_func, candidate.GetMetric())) {
            match.graph_idx = &candidate;
            return true;
        }
        if (!fallback) {
            fallback = &candidate;
        }
    }
    // No metric-matching index; fall back to any column-matching index only if no
    // distance function was specified (shouldn't normally happen, but keeps backward
    // compat for the legacy single-index code path).
    (void)fallback;
    return false;
}

static bool TryOptimizeANN(ClientContext &context, unique_ptr<LogicalOperator> &node,
                           unique_ptr<LogicalOperator> &get_owner, const GetChildInfo &get_info,
                           const DistanceOrderInfo &dist_info, idx_t limit, idx_t offset) {
    auto *get = get_info.get;
    auto table_entry = get->GetTable();
    if (!table_entry || !table_entry->IsDuckTable()) {
        return false;
    }

    auto &duck_table = table_entry->Cast<DuckTableEntry>();
    auto &storage = duck_table.GetStorage();
    auto &db = duck_table.ParentCatalog().GetAttached();
    auto &transaction = DuckTransaction::Get(context, db);
    if (transaction.ChangesMade()) {
        return false;
    }

    // If WHERE predicates have been pushed into the LogicalGet (table_filters /
    // dynamic_filters), the rewrite would silently drop them: VEX_INDEX_SCAN reads
    // by row_id and does not honor LogicalGet's pushdown filters. Fall back to the
    // default plan (seq scan + sort), which produces correct results (just without
    // the index acceleration for the filter case).
    if (get->table_filters.HasFilters()) {
        return false;
    }
    if (get->dynamic_filters && get->dynamic_filters->HasFilters()) {
        return false;
    }

    auto &column_ids = get->GetColumnIds();
    IndexMatch match;
    if (!TryFindMatchingIndex(context, storage, column_ids, dist_info.col_index,
                              dist_info.func_expr->function.GetName(), match)) {
        return false;
    }

    // Honor vex_brute_force_threshold: when node_count <= threshold, skip the rewrite
    // and let DuckDB run the natural SEQ_SCAN+ORDER_BY+LIMIT plan, which returns the
    // exact top-k. Users raise this to bypass HNSW (e.g., recall degradation after
    // bulk delete + reinsert).
    if (static_cast<int64_t>(match.graph_idx->GetNodeCount()) <= GetBruteForceThreshold(context)) {
        return false;
    }

    idx_t k = limit + offset;
    vector<idx_t> fetch_output_positions;
    for (idx_t i = 0; i < column_ids.size(); i++) {
        fetch_output_positions.push_back(i);
    }

    idx_t k_scan = k;
    if (get_info.filter) {
        idx_t get_card = get->has_estimated_cardinality ? get->estimated_cardinality : 0;
        idx_t filter_card = get_info.filter->has_estimated_cardinality
                                ? get_info.filter->estimated_cardinality
                                : 0;
        k_scan = ComputeScanK(k, get_card, filter_card);
    }

    auto scan = make_uniq<LogicalVexIndexScan>(get->table_index, GetOutputTypes(get), duck_table, *match.graph_idx,
                                               dist_info.query_vec_expr->Copy(), k_scan, column_ids,
                                               std::move(fetch_output_positions), optional_idx(),
                                               get->returned_types);
    scan->SetEstimatedCardinality(k);

    if (get_info.cross_product) {
        scan->children.push_back(std::move(get_info.cross_product->children[get_info.subquery_child_idx]));
    }
    ReplaceGetWithVexScan(get_owner, get_info, std::move(scan));
    (void)node;
    return true;
}

VexOptimizerExtension::VexOptimizerExtension() {
    pre_optimize_function = OptimizeFunction;
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
        auto &topn = node->Cast<LogicalTopN>();
        if (topn.orders.size() != 1) {
            return;
        }
        GetChildInfo get_info;
        if (!FindGetChild(*topn.children[0], get_info)) {
            return;
        }
        DistanceOrderInfo dist_info;
        if (!TryResolveDistanceOrder(topn.orders[0].expression.get(), get_info.get, get_info.proj,
                                     topn.orders[0].type, dist_info)) {
            return;
        }
        (void)TryOptimizeANN(context, node, topn.children[0], get_info, dist_info, topn.limit, topn.offset);
    } else if (node->type == LogicalOperatorType::LOGICAL_LIMIT) {
        auto &limit = node->Cast<LogicalLimit>();
        if (limit.children.size() != 1 || limit.children[0]->type != LogicalOperatorType::LOGICAL_ORDER_BY) {
            return;
        }
        if (limit.offset_val.Type() != LimitNodeType::CONSTANT_VALUE || limit.offset_val.GetConstantValue() != 0) {
            return;
        }
        if (limit.limit_val.Type() != LimitNodeType::CONSTANT_VALUE) {
            return;
        }
        auto &order_node = limit.children[0]->Cast<LogicalOrder>();
        if (order_node.orders.size() != 1 || order_node.children.size() != 1) {
            return;
        }
        GetChildInfo get_info;
        if (!FindGetChild(*order_node.children[0], get_info)) {
            return;
        }
        DistanceOrderInfo dist_info;
        if (!TryResolveDistanceOrder(order_node.orders[0].expression.get(), get_info.get, get_info.proj,
                                     order_node.orders[0].type, dist_info)) {
            return;
        }
        (void)TryOptimizeANN(context, node, order_node.children[0], get_info, dist_info,
                             limit.limit_val.GetConstantValue(), limit.offset_val.GetConstantValue());
    }
}

} // namespace duckdb
