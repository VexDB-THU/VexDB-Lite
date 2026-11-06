#include "vex_optimizer.hpp"

#include <algorithm>
#include <cmath>

#include "vex_fetch_utils.hpp"
#include "vex_graph_index.hpp"
#include "vex_physical_index_scan.hpp"
#include "vex_native_topn_fetch.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/main/config.hpp"

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

static uint64_t MixCoverage64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

static uint64_t HashCoverageBytes(const_data_ptr_t data, idx_t size) {
    uint64_t h = 1469598103934665603ULL;
    for (idx_t i = 0; i < size; i++) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t HashCoverageRowId(row_t row_id) {
    return MixCoverage64(static_cast<uint64_t>(row_id));
}

static uint64_t HashCoverageRowVector(row_t row_id, const float *vec, idx_t dim) {
    auto bytes = const_data_ptr_cast(reinterpret_cast<const char *>(vec));
    uint64_t h = HashCoverageRowId(row_id);
    h ^= HashCoverageBytes(bytes, dim * sizeof(float)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return MixCoverage64(h);
}

static void NormalizeCoverageVectorInPlace(float *vec, idx_t dim) {
    NormalizeDuckVectorInPlace(vec, dim);
}

static bool DistanceFunctionMatchesMetric(const string &name, VexMetric metric) {
    auto *entry = FindDistanceFunc(name);
    return entry && entry->metric == metric;
}

static bool IsAscDistanceFunction(const string &name) {
    auto *entry = FindDistanceFunc(name);
    return entry ? entry->ascending : true;
}

static string FilteredSearchMode(ClientContext &context) {
    Value setting;
    string mode = "exact";
    if (context.TryGetCurrentSetting("vexdb_filtered_search_mode", setting)) mode = StringUtil::Lower(setting.ToString());
    if (mode != "auto" && mode != "exact")
        throw InvalidInputException("vexdb_filtered_search_mode must be 'auto' or 'exact'");
    return mode;
}

static bool IsColumnRefFromTable(const Expression &expr, idx_t table_index, idx_t &col_index) {
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

static bool HasColumnRefFromTable(const Expression &expr, idx_t table_index) {
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
    unique_ptr<Expression> resolved_order;
    BoundFunctionExpression *func_expr = nullptr;
    Expression *query_vec_expr = nullptr;
    idx_t col_index = 0;
    OrderByNullType null_order = OrderByNullType::ORDER_DEFAULT;
};

static bool ExpandProjectionBindings(unique_ptr<Expression> &expr,
                                     const vector<LogicalProjection *> &projections, idx_t depth = 0) {
    if (depth > projections.size() + 1) return false;
    if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
        const auto binding = expr->Cast<BoundColumnRefExpression>().binding;
        for (auto *proj : projections) if (binding.table_index == proj->table_index) {
            if (binding.column_index >= proj->expressions.size()) return false;
            expr = proj->expressions[binding.column_index]->Copy();
            return ExpandProjectionBindings(expr, projections, depth + 1);
        }
    }
    bool valid = true;
    ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
        valid &= ExpandProjectionBindings(child, projections, depth);
    });
    return valid;
}

static bool TryResolveDistanceOrder(Expression *order_expr, LogicalGet *get, const vector<LogicalProjection *> &projections,
                                    OrderType order_type, OrderByNullType null_order, bool expand_views, DistanceOrderInfo &info) {
    info.null_order = null_order;
    Expression *resolved = order_expr;
    if (expand_views) {
        info.resolved_order = order_expr->Copy();
        if (!ExpandProjectionBindings(info.resolved_order, projections)) return false;
        resolved = info.resolved_order.get();
    } else {
        // Preserve the old direct-table shape by default. New view/projection
        // rewrites opt in with scalar/PQ integration, just like filtered ANN.
        if (projections.size() > 1) return false;
        while (resolved->GetExpressionClass() == ExpressionClass::BOUND_CAST)
            resolved = resolved->Cast<BoundCastExpression>().child.get();
        if (!projections.empty() && resolved->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
            const auto binding = resolved->Cast<BoundColumnRefExpression>().binding;
            if (binding.table_index == projections[0]->table_index && binding.column_index < projections[0]->expressions.size())
                resolved = projections[0]->expressions[binding.column_index].get();
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
    // For inner_product / <#> the user-facing scalar returns +(a·b) (greater = more similar),
    // so the natural query is ORDER BY ... DESC. The index ranks by -(a·b) ascending, which
    // matches; for L2 / cosine the user does ASC and the index ranks by sqrt-L2 / -cos_sim
    // ascending, also matching.
    OrderType expected = IsAscDistanceFunction(info.func_expr->function.name)
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
    unique_ptr<LogicalOperator> *get_owner = nullptr;
    vector<LogicalProjection *> projections;
    vector<LogicalFilter *> filters;
    unique_ptr<LogicalOperator> *cross_owner = nullptr;
};

static bool FindGetChild(unique_ptr<LogicalOperator> &child, GetChildInfo &info) {
    auto *owner = &child;
    LogicalOperator *cur = owner->get();

    while (cur->children.size() == 1 &&
           (cur->type == LogicalOperatorType::LOGICAL_PROJECTION || cur->type == LogicalOperatorType::LOGICAL_FILTER)) {
        if (cur->type == LogicalOperatorType::LOGICAL_PROJECTION) {
            if (!info.proj) info.proj = &cur->Cast<LogicalProjection>();
            info.projections.push_back(&cur->Cast<LogicalProjection>());
        } else {
            if (!info.filter) info.filter = &cur->Cast<LogicalFilter>();
            info.filters.push_back(&cur->Cast<LogicalFilter>());
        }
        owner = &cur->children[0];
        cur = owner->get();
    }

    if (cur->type == LogicalOperatorType::LOGICAL_GET) {
        info.get = &cur->Cast<LogicalGet>();
        info.get_owner = owner;
        return true;
    }

    if (cur->type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT && cur->children.size() == 2) {
        info.cross_owner = owner;
        for (idx_t i = 0; i < 2; i++) {
            auto *cp_child = cur->children[i].get();
            if (cp_child->type == LogicalOperatorType::LOGICAL_GET) {
                info.get = &cp_child->Cast<LogicalGet>();
                info.get_owner = &cur->children[i];
                info.cross_product = cur;
                info.subquery_child_idx = 1 - i;
                return true;
            }
            // CROSS_PRODUCT -> [FILTER -> GET, subquery]: WHERE clause + scalar
            // subquery query vector. Record the filter so TryOptimizeANN keeps
            // this query's natural filter-first exact plan.
            if (cp_child->type == LogicalOperatorType::LOGICAL_FILTER &&
                cp_child->children.size() == 1 &&
                cp_child->children[0]->type == LogicalOperatorType::LOGICAL_GET) {
                info.filter = &cp_child->Cast<LogicalFilter>();
                info.filters.push_back(info.filter);
                info.get = &cp_child->children[0]->Cast<LogicalGet>();
                info.get_owner = &cp_child->children[0];
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
    if (!info.get_owner) throw InternalException("VEX index scan lost its table owner");
    *(info.cross_owner ? info.cross_owner : info.get_owner) = std::move(vex_scan);
    (void)get_owner;
}

static unique_ptr<VexFilterPlan> MakeFilterPlan(const GetChildInfo &info, const DistanceOrderInfo &distance,
                                                bool allow_unfiltered = false) {
    auto &get = *info.get;
    if (get.dynamic_filters && get.dynamic_filters->HasFilters()) return nullptr;
    auto plan = make_uniq<VexFilterPlan>();
    auto conjunction = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
    for (auto *filter : info.filters) for (auto &expr : filter->expressions)
        conjunction->children.push_back(expr->Copy());
    // LogicalGet filters use absolute physical column IDs (DuckDB v1.5.2
    // CreateTableFilterSet), not positions in GetColumnIds().
    for (const auto &entry : get.table_filters.filters) {
        idx_t position = DConstants::INVALID_INDEX;
        for (idx_t i = 0; i < get.GetColumnIds().size(); ++i)
            if (get.GetColumnIds()[i].GetPrimaryIndex() == entry.first) { position = i; break; }
        if (position == DConstants::INVALID_INDEX) return nullptr;
        BoundColumnRefExpression column(get.GetColumnType(get.GetColumnIds()[position]), ColumnBinding(get.table_index, position));
        conjunction->children.push_back(entry.second->ToExpression(column));
    }
    if (conjunction->children.empty()) {
        if (!allow_unfiltered) return nullptr;
        plan->predicate = make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
    } else {
        plan->predicate = std::move(conjunction);
    }
    // Deterministic casts/arithmetic may be marked CanThrow even for ordinary
    // metadata expressions (e.g. id % 2). Execute them with DuckDB's evaluator
    // and preserve errors; do not silently disable filtering for such views.
    if (!ExpandProjectionBindings(plan->predicate, info.projections) || plan->predicate->IsVolatile()) return nullptr;
    bool valid = true;
    const auto vector_column = get.GetColumnIds()[distance.col_index].GetPrimaryIndex();
    std::function<void(unique_ptr<Expression> &)> bind = [&](unique_ptr<Expression> &expr) {
        if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
            const auto &ref = expr->Cast<BoundColumnRefExpression>();
            if (ref.binding.table_index != get.table_index || ref.depth || ref.binding.column_index >= get.GetColumnIds().size()) {
                valid = false; return;
            }
            const auto id = get.GetColumnIds()[ref.binding.column_index].GetPrimaryIndex();
            // If testing the predicate already reads the full vector, retain
            // natural SQL instead of adding another vector-search pass.
            if (id == vector_column) { valid = false; return; }
            if (id != COLUMN_IDENTIFIER_ROW_ID &&
                (id >= get.returned_types.size() || get.returned_types[id] != ref.return_type)) { valid = false; return; }
            idx_t position = 0;
            for (; position < plan->columns.size(); ++position) if (plan->columns[position] == id) break;
            if (position == plan->columns.size()) { plan->columns.push_back(id); plan->types.push_back(ref.return_type); }
            expr = make_uniq<BoundReferenceExpression>(ref.return_type, position);
            return;
        }
        if (expr->GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY || expr->GetExpressionClass() == ExpressionClass::BOUND_REF) {
            valid = false; return;
        }
        ExpressionIterator::EnumerateChildren(*expr, bind);
    };
    bind(plan->predicate);
    if (!valid) return nullptr;
    plan->vector_column = get.GetColumnIds()[distance.col_index].GetPrimaryIndex();
    plan->vector_type = get.returned_types[plan->vector_column];
    plan->distance = distance.func_expr->Copy();
    auto &function = plan->distance->Cast<BoundFunctionExpression>();
    idx_t vector_argument = 0;
    idx_t unused;
    if (!IsColumnRefFromTable(*function.children[0], get.table_index, unused)) vector_argument = 1;
    plan->query_argument = 1 - vector_argument;
    function.children[vector_argument] = make_uniq<BoundReferenceExpression>(plan->vector_type, 0);
    plan->ascending = IsAscDistanceFunction(function.function.name);
    return plan;
}

// The selector path scans metadata and then fetches vectors by row ID. For a
// selective stored-column equality, DuckDB's own filter/ART and vectorized scan
// can be much cheaper. Use column statistics and the user's existing search
// budget to retain that native plan. This estimate only chooses an execution
// path: native SQL remains exact even with skewed or stale statistics.
static bool PreferNativeEquality(ClientContext &context, DuckTableEntry &table,
                                 const VexFilterPlan &plan, Expression &predicate, double budget) {
    if (predicate.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
        for (auto &child : predicate.Cast<BoundConjunctionExpression>().children)
            if (PreferNativeEquality(context, table, plan, *child, budget)) return true;
        return false;
    }
    if (predicate.GetExpressionType() != ExpressionType::COMPARE_EQUAL) return false;
    auto &comparison = predicate.Cast<BoundComparisonExpression>();
    Expression *column = comparison.left.get(), *value = comparison.right.get();
    if (column->GetExpressionClass() != ExpressionClass::BOUND_REF) std::swap(column, value);
    if (column->GetExpressionClass() != ExpressionClass::BOUND_REF ||
        (!value->IsFoldable() && value->GetExpressionClass() != ExpressionClass::BOUND_PARAMETER)) return false;
    const auto position = column->Cast<BoundReferenceExpression>().index;
    if (position >= plan.columns.size() || plan.columns[position] == COLUMN_IDENTIFIER_ROW_ID) return false;
    auto stats = table.GetStatistics(context, StorageIndex(plan.columns[position]));
    idx_t distinct = stats ? stats->GetDistinctCount() : 0;
    // A boolean equality can retain half the table even without NDV statistics.
    if (!distinct && column->return_type == LogicalType::BOOLEAN) distinct = 2;
    if (!distinct) return true; // Unknown selectivity: preserve native SQL.
    return double(table.GetStorage().GetTotalRows()) / double(distinct) <= budget;
}

static double FilterSearchBudget(ClientContext &context, idx_t k) {
    Value value;
    int ef = 40;
    if (context.TryGetCurrentSetting("vexdb_ef_search", value)) ef = value.GetValue<int>();
    if (ef < 1 || ef > 65535) throw InvalidInputException("vexdb_ef_search must be in [1, 65535], got %d", ef);
    double factor = 1.0;
    if (context.TryGetCurrentSetting("vexdb_pq_refine_k_factor", value)) factor = value.GetValue<double>();
    if (!std::isfinite(factor) || factor < 1.0 || factor > 1000.0)
        throw InvalidInputException("vexdb_pq_refine_k_factor must be in [1.0, 1000.0], got %.3f", factor);
    return std::max({double(GetBruteForceThreshold(context)), double(ef), double(k) * factor});
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

struct TableRowIdCoverage {
    idx_t live_count = 0;
    idx_t rowid_upper_bound = 0;
    uint64_t rowid_checksum = 0;
    uint64_t vector_checksum = 0;
    bool has_vector_checksum = false;
};

static TableRowIdCoverage ScanTableRowIdCoverage(ClientContext &context, DataTable &storage,
                                                  DuckTransaction &transaction, column_t vector_column_id,
                                                  GraphIndex &graph_idx) {
    TableRowIdCoverage coverage;
    auto dimension = graph_idx.GetDimension();
    auto metric = graph_idx.GetMetric();
    auto use_pq_checksum = graph_idx.UsesPQCoverageChecksum();
    auto use_rabitq_checksum = graph_idx.UsesRaBitQCoverageChecksum();

    vector<StorageIndex> scan_column_ids;
    scan_column_ids.emplace_back(vector_column_id);
    scan_column_ids.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
    TableScanState scan_state;
    storage.InitializeScan(context, transaction, scan_state, scan_column_ids);

    auto table_types = storage.GetTypes();
    if (vector_column_id >= table_types.size()) {
        return coverage;
    }
    DataChunk chunk;
    chunk.Initialize(Allocator::Get(context), {table_types[vector_column_id], LogicalType::ROW_TYPE});
    vector<float> normalized;
    if (metric == VexMetric::COSINE) {
        normalized.resize(dimension);
    }
    while (true) {
        chunk.Reset();
        storage.Scan(transaction, chunk, scan_state);
        if (chunk.size() == 0) {
            break;
        }
        auto &vec_vector = chunk.data[0];
        auto &rowid_vec = chunk.data[1];
        vec_vector.Flatten(chunk.size());
        rowid_vec.Flatten(chunk.size());
        auto &vec_validity = FlatVector::Validity(vec_vector);
        auto &child_vec = ArrayVector::GetEntry(vec_vector);
        child_vec.Flatten(chunk.size() * dimension);
        auto vec_data = FlatVector::GetData<float>(child_vec);
        auto row_ids = FlatVector::GetData<row_t>(rowid_vec);
        auto &validity = FlatVector::Validity(rowid_vec);
        for (idx_t i = 0; i < chunk.size(); i++) {
            if (!validity.RowIsValid(i) || !vec_validity.RowIsValid(i)) {
                continue;
            }
            auto rid = row_ids[i];
            if (rid < 0 || rid >= MAX_ROW_ID) {
                continue;
            }
            const float *vec = vec_data + i * dimension;
            coverage.live_count++;
            coverage.rowid_upper_bound =
                std::max<idx_t>(coverage.rowid_upper_bound, static_cast<idx_t>(rid) + 1);
            coverage.rowid_checksum += HashCoverageRowId(rid);
            if (metric == VexMetric::COSINE) {
                std::copy(vec, vec + dimension, normalized.data());
                NormalizeCoverageVectorInPlace(normalized.data(), dimension);
                vec = normalized.data();
            }
            if (use_pq_checksum) {
                coverage.vector_checksum += graph_idx.HashPQVectorForCoverage(rid, vec);
            } else if (use_rabitq_checksum) {
                coverage.vector_checksum += graph_idx.HashRaBitQVectorForCoverage(rid, vec);
            } else {
                coverage.vector_checksum += HashCoverageRowVector(rid, vec, dimension);
            }
            coverage.has_vector_checksum = true;
        }
    }
    return coverage;
}

static bool GraphIndexMayBeStaleAfterRecovery(ClientContext &context, DataTable &storage,
                                              DuckTransaction &transaction, GraphIndex &graph_idx) {
    if (graph_idx.NeedsQuantizerRebuild() && graph_idx.IsCompactMode()) {
        return true;
    }
    if (graph_idx.HasStoragePointerCorruption()) {
        graph_idx.MarkRowIdCoverageChecked(true);
        return true;
    }
    if (graph_idx.HasRowIdCoverageCheck()) {
        return graph_idx.IsRowIdCoverageStale();
    }

    // DuckDB v1.5.x WAL recovery replays ordinary INSERT/DELETE records into
    // table storage but does not replay those changes into secondary indexes.
    // Validate live row-id coverage once after binding a persisted GRAPH_INDEX.
    // If the table has live row ids or live cardinality the graph does not
    // cover, keep the original exact plan instead of returning stale ANN rows.
    auto column_ids = graph_idx.GetColumnIds();
    if (column_ids.empty()) {
        graph_idx.MarkRowIdCoverageChecked(true);
        return true;
    }
    auto table_coverage = ScanTableRowIdCoverage(context, storage, transaction, column_ids[0], graph_idx);
    auto index_coverage = graph_idx.GetRowIdCoverage();
    bool stale = table_coverage.live_count != index_coverage.live_count ||
                 table_coverage.rowid_upper_bound > index_coverage.rowid_upper_bound ||
                 table_coverage.rowid_checksum != index_coverage.rowid_checksum;
    if (!stale) {
        if (table_coverage.has_vector_checksum && index_coverage.has_vector_checksum) {
            stale = table_coverage.vector_checksum != index_coverage.vector_checksum;
        } else if (table_coverage.has_vector_checksum != index_coverage.has_vector_checksum) {
            stale = true;
        }
    }
    graph_idx.MarkRowIdCoverageChecked(stale);
    return stale;
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
    if (DBConfig::GetConfig(context).ResolveNullOrder(context,
            IsAscDistanceFunction(dist_info.func_expr->function.name) ? OrderType::ASCENDING : OrderType::DESCENDING,
            dist_info.null_order) == OrderByNullType::NULLS_FIRST || dist_info.query_vec_expr->IsVolatile()) return false;

    if (get->dynamic_filters && get->dynamic_filters->HasFilters()) return false;
    const bool filtered = !get_info.filters.empty() || !get->table_filters.filters.empty();
    unique_ptr<VexFilterPlan> filter_plan;
    if (filtered) {
        const auto mode = FilteredSearchMode(context);
        if (mode == "exact" || dist_info.null_order == OrderByNullType::NULLS_FIRST) return false;
        filter_plan = MakeFilterPlan(get_info, dist_info);
        if (!filter_plan) return false;
    }

    auto &column_ids = get->GetColumnIds();
    IndexMatch match;
    if (!TryFindMatchingIndex(context, storage, column_ids, dist_info.col_index,
                              dist_info.func_expr->function.name, match)) {
        return false;
    }
    if (GraphIndexMayBeStaleAfterRecovery(context, storage, transaction, *match.graph_idx)) {
        return false;
    }

    // Honor vexdb_brute_force_threshold: when node_count <= threshold, skip the rewrite
    // and let DuckDB run the natural SEQ_SCAN+ORDER_BY+LIMIT plan, which returns the
    // exact top-k. Users raise this to bypass HNSW (e.g., recall degradation after
    // bulk delete + reinsert).
    if (static_cast<int64_t>(match.graph_idx->GetNodeCount()) <= GetBruteForceThreshold(context)) {
        return false;
    }

    if (limit > std::numeric_limits<idx_t>::max() - offset) return false;
    idx_t k = limit + offset;
    if (filter_plan && PreferNativeEquality(context, duck_table, *filter_plan, *filter_plan->predicate,
                                            FilterSearchBudget(context, k))) return false;
    vector<idx_t> fetch_output_positions;
    for (idx_t i = 0; i < column_ids.size(); i++) {
        fetch_output_positions.push_back(i);
    }

    auto scan = make_uniq<LogicalVexIndexScan>(get->table_index, GetOutputTypes(get), duck_table, *match.graph_idx,
                                               dist_info.query_vec_expr->Copy(), k, column_ids,
                                               std::move(fetch_output_positions), optional_idx(),
                                               get->returned_types);
    scan->quality_fallback_plan = MakeFilterPlan(get_info, dist_info, true);
    if (!scan->quality_fallback_plan) return false;
    scan->filter_plan = std::move(filter_plan);
    scan->SetEstimatedCardinality(k);

    if (get_info.cross_product) {
        scan->children.push_back(std::move(get_info.cross_product->children[get_info.subquery_child_idx]));
    }

    unique_ptr<LogicalOperator> replacement = std::move(scan);
    ReplaceGetWithVexScan(get_owner, get_info, std::move(replacement));
    (void)node;
    return true;
}

VexOptimizerExtension::VexOptimizerExtension() {
    pre_optimize_function = OptimizeFunction;
    optimize_function = OptimizeFunction;
}

// Follow an unmodified physical row ID through the native unary TopN plan.
// Reject joins, casts, expressions and other sources that could duplicate or
// manufacture row IDs. A SEMI join can otherwise return fewer rows than RHS.
static bool IsNativeRowId(LogicalOperator &node, ColumnBinding binding, DuckTableEntry &table) {
    if (node.type == LogicalOperatorType::LOGICAL_GET) {
        auto &get = node.Cast<LogicalGet>();
        return get.GetTable().get() == &table && binding.table_index == get.table_index &&
               binding.column_index < get.GetColumnIds().size() &&
               get.GetColumnIds()[binding.column_index].GetPrimaryIndex() == COLUMN_IDENTIFIER_ROW_ID;
    }
    if (node.children.size() != 1) return false;
    if (node.type == LogicalOperatorType::LOGICAL_PROJECTION) {
        auto &projection = node.Cast<LogicalProjection>();
        if (binding.table_index != projection.table_index || binding.column_index >= projection.expressions.size()) return false;
        auto &expression = *projection.expressions[binding.column_index];
        if (expression.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) return false;
        return IsNativeRowId(*node.children[0], expression.Cast<BoundColumnRefExpression>().binding, table);
    }
    if (node.type != LogicalOperatorType::LOGICAL_TOP_N && node.type != LogicalOperatorType::LOGICAL_FILTER) return false;
    return IsNativeRowId(*node.children[0], binding, table);
}

static void TryNativeTopNFetch(ClientContext &context, unique_ptr<LogicalOperator> &node) {
    Value setting;
    if (!context.TryGetCurrentSetting("vexdb_native_topn_fetch", setting) || !setting.GetValue<bool>()) return;
    if (node->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN || node->children.size() != 2) return;
    auto &join = node->Cast<LogicalComparisonJoin>();
    if (join.join_type != JoinType::SEMI || join.conditions.size() != 1 || join.predicate ||
        !join.duplicate_eliminated_columns.empty() || node->children[0]->type != LogicalOperatorType::LOGICAL_GET ||
        node->children[1]->type != LogicalOperatorType::LOGICAL_TOP_N) return;
    auto &condition = join.conditions[0];
    if (condition.comparison != ExpressionType::COMPARE_EQUAL ||
        condition.left->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
        condition.right->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) return;
    auto &left = node->children[0]->Cast<LogicalGet>();
    auto entry = left.GetTable();
    if (!entry || !entry->IsDuckTable() || !left.table_filters.filters.empty()) return;
    auto &table = entry->Cast<DuckTableEntry>();
    if (!IsNativeRowId(left, condition.left->Cast<BoundColumnRefExpression>().binding, table) ||
        !IsNativeRowId(*node->children[1], condition.right->Cast<BoundColumnRefExpression>().binding, table)) return;
    auto &topn = node->children[1]->Cast<LogicalTopN>();
    if (topn.orders.size() != 1 || !topn.limit) return;
    GetChildInfo info;
    DistanceOrderInfo distance;
    if (!FindGetChild(topn.children[0], info) || info.cross_product ||
        !TryResolveDistanceOrder(topn.orders[0].expression.get(), info.get, info.projections,
                                 topn.orders[0].type, topn.orders[0].null_order, true, distance)) return;
    auto output_bindings = join.GetColumnBindings();
    const auto left_bindings = left.GetColumnBindings();
    vector<ColumnIndex> columns;
    vector<LogicalType> types;
    for (const auto &binding : output_bindings) {
        const auto it = std::find(left_bindings.begin(), left_bindings.end(), binding);
        if (it == left_bindings.end() || binding.column_index >= left.GetColumnIds().size() ||
            idx_t(it - left_bindings.begin()) >= left.types.size()) return;
        const auto &column = left.GetColumnIds()[binding.column_index];
        if (!column.HasPrimaryIndex() || column.HasChildren() || column.IsPushdownExtract() ||
            (column.IsVirtualColumn() && column.GetPrimaryIndex() != COLUMN_IDENTIFIER_ROW_ID)) return;
        columns.push_back(column);
        types.push_back(left.types[idx_t(it - left_bindings.begin())]);
    }
    auto fetch = make_uniq<LogicalVexNativeTopNFetch>(table, std::move(columns), std::move(types),
                                                     std::move(output_bindings), condition.right->Copy());
    fetch->SetEstimatedCardinality(topn.limit);
    fetch->children.push_back(std::move(node->children[1]));
    node = std::move(fetch);
}

void VexOptimizerExtension::OptimizeFunction(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
    OptimizeNode(input.context, plan);
}

// The SQL expressions and candidate set stay unchanged. Only move the unary
// output work into the bounded scan, so an ARRAY used solely for computing a
// scalar distance need not cross a native pipeline boundary. Correlated scans
// and volatile/subquery expressions retain their original operators.
static void TryFuseScanOutput(unique_ptr<LogicalOperator> &node) {
    if ((node->type != LogicalOperatorType::LOGICAL_PROJECTION &&
         node->type != LogicalOperatorType::LOGICAL_FILTER) || node->children.size() != 1) return;
    auto *scan = dynamic_cast<LogicalVexIndexScan *>(node->children[0].get());
    if (!scan || !scan->children.empty()) return;
    auto bindings = scan->GetColumnBindings();
    auto input_types = scan->output_steps.empty() ? scan->output_types : scan->output_steps.back().types;
    if (bindings.size() != input_types.size()) return;
    bool valid = true;
    std::function<void(unique_ptr<Expression> &)> bind = [&](unique_ptr<Expression> &expr) {
        if (expr->IsVolatile() || expr->GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY ||
            expr->GetExpressionClass() == ExpressionClass::BOUND_REF) { valid = false; return; }
        if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
            const auto &ref = expr->Cast<BoundColumnRefExpression>();
            const auto it = std::find(bindings.begin(), bindings.end(), ref.binding);
            if (ref.depth || it == bindings.end() || input_types[idx_t(it - bindings.begin())] != ref.return_type) {
                valid = false; return;
            }
            expr = make_uniq<BoundReferenceExpression>(ref.return_type, idx_t(it - bindings.begin()));
            return;
        }
        ExpressionIterator::EnumerateChildren(*expr, bind);
    };
    VexScanOutputStep step;
    step.bindings = node->GetColumnBindings();
    if (node->type == LogicalOperatorType::LOGICAL_PROJECTION) {
        for (auto &expr : node->expressions) {
            auto copy = expr->Copy();
            bind(copy);
            step.types.push_back(copy->return_type);
            step.expressions.push_back(std::move(copy));
        }
    } else {
        auto conjunction = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
        for (auto &expr : node->expressions) {
            auto copy = expr->Copy();
            bind(copy);
            conjunction->children.push_back(std::move(copy));
        }
        if (!conjunction->children.empty()) step.predicate = std::move(conjunction);
        const auto &map = node->Cast<LogicalFilter>().projection_map;
        for (idx_t i = 0; i < (map.empty() ? input_types.size() : map.size()); ++i) {
            const auto pos = map.empty() ? i : map[i];
            if (pos >= input_types.size()) return;
            step.types.push_back(input_types[pos]);
            step.expressions.push_back(make_uniq<BoundReferenceExpression>(input_types[pos], pos));
        }
    }
    if (!valid || step.types.empty() || step.types.size() != step.bindings.size()) return;
    scan->output_steps.push_back(std::move(step));
    auto replacement = std::move(node->children[0]);
    node = std::move(replacement);
    node->ResolveOperatorTypes();
}

void VexOptimizerExtension::OptimizeNode(ClientContext &context, unique_ptr<LogicalOperator> &node) {
    for (auto &child : node->children) {
        OptimizeNode(context, child);
    }

    TryFuseScanOutput(node);
    TryNativeTopNFetch(context, node);

    if (node->type == LogicalOperatorType::LOGICAL_TOP_N) {
        auto &topn = node->Cast<LogicalTopN>();
        if (topn.orders.size() != 1) {
            return;
        }
        GetChildInfo get_info;
        if (!FindGetChild(topn.children[0], get_info)) {
            return;
        }
        if (FilteredSearchMode(context) == "exact" && (!get_info.filters.empty() ||
            !get_info.get->table_filters.filters.empty())) return;
        DistanceOrderInfo dist_info;
        if (!TryResolveDistanceOrder(topn.orders[0].expression.get(), get_info.get, get_info.projections,
                                     topn.orders[0].type, topn.orders[0].null_order, FilteredSearchMode(context) == "auto", dist_info)) {
            return;
        }
        (void)TryOptimizeANN(context, node, topn.children[0], get_info, dist_info, topn.limit, topn.offset);
    } else if (node->type == LogicalOperatorType::LOGICAL_LIMIT) {
        auto &limit = node->Cast<LogicalLimit>();
        if (limit.children.size() != 1 || limit.children[0]->type != LogicalOperatorType::LOGICAL_ORDER_BY) {
            return;
        }
        // LIMIT 10 without OFFSET binds as offset_val.Type() == UNSET, not CONSTANT_VALUE=0.
        // If we required CONSTANT_VALUE here, the pre-optimize pass would skip every plain
        // `ORDER BY dist(...) LIMIT k` query — then LATE_MATERIALIZATION (which runs before
        // post-optimize) wraps the Get in a SEMI hash-join + SEQ_SCAN to fetch non-order
        // columns by row_id, and the post-optimize rewrite leaves that wrapper in place.
        // Accept UNSET as offset=0 so the rewrite happens at pre-optimize time, before
        // late_materialization can fire.
        idx_t offset_val = 0;
        if (limit.offset_val.Type() == LimitNodeType::CONSTANT_VALUE) {
            offset_val = limit.offset_val.GetConstantValue();
        } else if (limit.offset_val.Type() != LimitNodeType::UNSET) {
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
        if (!FindGetChild(order_node.children[0], get_info)) {
            return;
        }
        if (FilteredSearchMode(context) == "exact" && (!get_info.filters.empty() ||
            !get_info.get->table_filters.filters.empty())) return;
        DistanceOrderInfo dist_info;
        if (!TryResolveDistanceOrder(order_node.orders[0].expression.get(), get_info.get, get_info.projections,
                                     order_node.orders[0].type, order_node.orders[0].null_order, FilteredSearchMode(context) == "auto", dist_info)) {
            return;
        }
        (void)TryOptimizeANN(context, node, order_node.children[0], get_info, dist_info,
                             limit.limit_val.GetConstantValue(), offset_val);
    }
}

} // namespace duckdb
