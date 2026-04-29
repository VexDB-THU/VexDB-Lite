#include "vex_optimizer.hpp"

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

static bool IsDistanceFunction(const string &name) {
    return name == "l2_distance";
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
    col_index = colref.binding.column_index.GetIndex();
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
    if (!IsDistanceFunction(info.func_expr->function.name) || info.func_expr->children.size() != 2) {
        return false;
    }
    if (order_type != OrderType::ASCENDING) {
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
    if (!get->types.empty()) {
        return get->types;
    }
    return BuildOutputTypes(get->GetColumnIds(), get->returned_types);
}

struct IndexMatch {
    GraphIndex *graph_idx = nullptr;
};

static bool TryFindMatchingIndex(ClientContext &context, DataTable &storage, const vector<ColumnIndex> &column_ids,
                                 idx_t col_index, IndexMatch &match) {
    column_t physical_col_id = DConstants::INVALID_INDEX;
    if (col_index < column_ids.size()) {
        physical_col_id = column_ids[col_index].GetPrimaryIndex();
    }
    if (physical_col_id == DConstants::INVALID_INDEX) {
        return false;
    }

    auto &index_list = storage.GetDataTableInfo()->GetIndexes();
    for (auto &index : index_list.Indexes()) {
        if (!index.IsBound()) {
            continue;
        }
        auto &bound = index.Cast<BoundIndex>();
        if (bound.GetIndexType() != GraphIndex::TYPE_NAME) {
            continue;
        }
        if (!bound.GetColumnIds().empty() && bound.GetColumnIds()[0] == physical_col_id) {
            match.graph_idx = &bound.Cast<GraphIndex>();
            return true;
        }
    }
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

    auto &column_ids = get->GetColumnIds();
    IndexMatch match;
    if (!TryFindMatchingIndex(context, storage, column_ids, dist_info.col_index, match)) {
        return false;
    }

    idx_t k = limit + offset;
    vector<idx_t> fetch_output_positions;
    for (idx_t i = 0; i < column_ids.size(); i++) {
        fetch_output_positions.push_back(i);
    }
    auto scan = make_uniq<LogicalVexIndexScan>(get->table_index, GetOutputTypes(get), duck_table, *match.graph_idx,
                                               dist_info.query_vec_expr->Copy(), k, column_ids,
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
