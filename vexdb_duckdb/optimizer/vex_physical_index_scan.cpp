#include "vex_physical_index_scan.hpp"

#include "vex_fetch_utils.hpp"
#include "vex_graph_index.hpp"
#include "vex_search_admission.hpp"
#include "vex_search_profile.hpp"
#include <cmath>

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/execution/operator/scan/physical_dummy_scan.hpp"

namespace duckdb {

static constexpr const char *kAlgorithmBruteForce = "brute-force";
static constexpr const char *kAlgorithmHnsw = "hnsw";

static string GetPQSearchMode(ClientContext &context, const GraphIndex &index, bool filtered = false) {
    string mode = "off";
    Value value;
    if (context.TryGetCurrentSetting("vexdb_pq_search_mode", value)) {
        mode = StringUtil::Lower(value.ToString());
    }
    if (mode.empty()) mode = "off";
    if (mode != "off" && mode != "pq_only" && mode != "pq_graph") {
        throw InvalidInputException("vexdb_pq_search_mode must be 'off', 'pq_only' or 'pq_graph', got '%s'", mode);
    }
    // The existing PQ tables/refinement implement L2 (normalized for cosine),
    // not negative inner product. Filtered IP must use its raw metric or SQL
    // fallback, including when the session requested pq_only.
    if (filtered && index.GetMetric() == VexMetric::INNER_PRODUCT && mode != "off") return "off";
    if (mode == "pq_graph" && (index.IsCompactMode() || index.GetMetric() == VexMetric::INNER_PRODUCT)) {
        if (filtered) return "off";
        throw InvalidInputException("pq_graph requires a non-compact L2 or cosine PQ index");
    }
    if (index.UsesRaBitQ()) return "off";
    if (index.IsCompactMode() && !filtered) mode = "pq_only";
    if (mode != "off" && !index.UsesPQ()) {
        if (filtered) return "off";
        throw InvalidInputException("PQ search requires the index to be built with WITH (quantizer='pq', pq_m=N)");
    }
    return mode;
}

class VexIndexScanOperatorState : public OperatorState {
public:
    bool searched = false;
    unique_ptr<ColumnDataCollection> collection;
    ColumnDataScanState scan_state;
};

LogicalVexIndexScan::LogicalVexIndexScan(idx_t table_index_p, vector<LogicalType> output_types_p,
                                         DuckTableEntry &table_p, GraphIndex &graph_index_p,
                                         unique_ptr<Expression> query_vec_expr_p, idx_t k_p,
                                         vector<ColumnIndex> column_ids_p, vector<idx_t> fetch_output_positions_p,
                                         optional_idx distance_output_index_p, vector<LogicalType> returned_types_p)
    : table_index(table_index_p), output_types(std::move(output_types_p)), table(table_p),
      graph_index(graph_index_p), query_vec_expr(std::move(query_vec_expr_p)), k(k_p),
      column_ids(std::move(column_ids_p)), fetch_output_positions(std::move(fetch_output_positions_p)),
      distance_output_index(distance_output_index_p), returned_types(std::move(returned_types_p)) {
}

PhysicalOperator &LogicalVexIndexScan::CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) {
	auto &scan = planner.Make<PhysicalVexIndexScan>(types, estimated_cardinality, table, graph_index,
	                                                query_vec_expr->Copy(), k, column_ids, fetch_output_positions,
	                                                distance_output_index, returned_types, output_types.size());
    {
        auto bft = GetBruteForceThreshold(context);
        auto &phys_scan = scan.Cast<PhysicalVexIndexScan>();
        if (filter_plan) phys_scan.filter_plan = filter_plan->Copy();
        if (quality_fallback_plan) phys_scan.quality_fallback_plan = quality_fallback_plan->Copy();
        if (!output_steps.empty()) {
            phys_scan.output_buffer_types = output_types;
            for (auto &step : output_steps) phys_scan.output_steps.push_back(step.Copy());
        }
        phys_scan.search_admission = VexSearchAdmission::Get(*context.db);
        const auto mode = GetPQSearchMode(context, graph_index, bool(filter_plan));
        phys_scan.algorithm_used = mode == "pq_graph" ? "pq-graph" : mode == "pq_only" ? "pq-full-scan" :
                                  (static_cast<int64_t>(graph_index.GetNodeCount()) <= bft)
                                       ? kAlgorithmBruteForce
                                       : kAlgorithmHnsw;
        if (filter_plan) phys_scan.algorithm_used = "filtered-auto (" + phys_scan.algorithm_used + " / exact)";
    }
    if (children.empty()) {
        auto &dummy_scan = planner.Make<PhysicalDummyScan>(vector<LogicalType> {LogicalType::INTEGER}, 1);
        scan.children.push_back(dummy_scan);
    } else {
        for (auto &child : children) {
            scan.children.push_back(planner.CreatePlan(*child));
        }
    }
    return scan;
}

void LogicalVexIndexScan::ResolveColumnBindings(ColumnBindingResolver &res, vector<ColumnBinding> &bindings) {
    for (auto &child : children) {
        res.VisitOperator(*child);
    }
    res.VisitExpression(&query_vec_expr);
    bindings = GetColumnBindings();
}

string LogicalVexIndexScan::GetExtensionName() const {
    return "vex_index_scan";
}

vector<ColumnBinding> LogicalVexIndexScan::GetColumnBindings() {
    if (!output_steps.empty()) return output_steps.back().bindings;
    vector<ColumnBinding> result;
    for (idx_t i = 0; i < output_types.size(); i++) {
        result.emplace_back(table_index, idx_t(i));
    }
    for (auto &child : children) {
        auto child_bindings = child->GetColumnBindings();
        for (auto &binding : child_bindings) {
            result.push_back(binding);
        }
    }
    return result;
}

void LogicalVexIndexScan::Serialize(Serializer &serializer) const {
    (void)serializer;
    throw NotImplementedException("LogicalVexIndexScan::Serialize is not implemented");
}

void LogicalVexIndexScan::ResolveTypes() {
    if (!output_steps.empty()) {
        types = output_steps.back().types;
        return;
    }
    types = output_types;
    for (auto &child : children) {
        for (auto &type : child->types) {
            types.push_back(type);
        }
    }
}

PhysicalVexIndexScan::PhysicalVexIndexScan(PhysicalPlan &physical_plan, vector<LogicalType> types_p,
	                                           idx_t estimated_cardinality, DuckTableEntry &table_p,
	                                           GraphIndex &graph_index_p, unique_ptr<Expression> query_vec_expr_p, idx_t k_p,
	                                           vector<ColumnIndex> column_ids_p, vector<idx_t> fetch_output_positions_p,
	                                           optional_idx distance_output_index_p, vector<LogicalType> returned_types_p,
	                                           idx_t base_output_count_p)
	: PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types_p), estimated_cardinality),
	  table(table_p), graph_index(graph_index_p), query_vec_expr(std::move(query_vec_expr_p)), k(k_p),
	  column_ids(std::move(column_ids_p)), fetch_output_positions(std::move(fetch_output_positions_p)),
	  distance_output_index(distance_output_index_p), returned_types(std::move(returned_types_p)),
	  base_output_count(base_output_count_p) {
    output_buffer_types = types;
}

string PhysicalVexIndexScan::GetName() const {
    return "VEXDB_INDEX_SCAN";
}

InsertionOrderPreservingMap<string> PhysicalVexIndexScan::ParamsToString() const {
    InsertionOrderPreservingMap<string> result;
    result["Index"] = graph_index.GetIndexName();
    result["Top"] = to_string(k);
    if (!algorithm_used.empty()) {
        result["Algorithm"] = algorithm_used;
    }
    if (filter_plan) result["Filter"] = "snapshot-visible scalar prefilter";
    if (!output_steps.empty()) result["Output"] = "projected inside scan";
    return result;
}

unique_ptr<OperatorState> PhysicalVexIndexScan::GetOperatorState(ExecutionContext &context) const {
    (void)context;
    return make_uniq<VexIndexScanOperatorState>();
}

static bool ExtractFloatVectorFromChildren(const vector<Value> &children, LogicalTypeId child_type_id,
                                           vector<float> &out_vec) {
    out_vec.clear();
    out_vec.reserve(children.size());
    bool is_float = (child_type_id == LogicalTypeId::FLOAT);
    for (auto &child : children) {
        try {
            out_vec.push_back(is_float ? FloatValue::Get(child)
                                       : FloatValue::Get(child.DefaultCastAs(LogicalType::FLOAT)));
        } catch (const std::exception &) {
            out_vec.clear();
            return false;
        }
    }
    return !out_vec.empty();
}

static bool ExtractFloatVector(const Value &val, vector<float> &out_vec) {
    auto &type = val.type();
    if (type.id() == LogicalTypeId::ARRAY) {
        return ExtractFloatVectorFromChildren(ArrayValue::GetChildren(val), ArrayType::GetChildType(type).id(),
                                              out_vec);
    }
    if (type.id() == LogicalTypeId::LIST) {
        return ExtractFloatVectorFromChildren(ListValue::GetChildren(val), ListType::GetChildType(type).id(), out_vec);
    }
    return false;
}

OperatorResultType PhysicalVexIndexScan::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                 GlobalOperatorState &gstate, OperatorState &state) const {
    auto &scan_state = state.Cast<VexIndexScanOperatorState>();
    (void)gstate;

	if (!scan_state.searched) {
        VexSearchProfileRun profile_run(context.client);
        auto *profile = profile_run.Get();
        VexProfileTimer prepare_timer(profile ? &profile->prepare_ns : nullptr);
		DataChunk query_input;
		if (!input.data.empty() && input.size() > 0) {
			vector<LogicalType> base_types;
			base_types.reserve(input.ColumnCount());
			for (idx_t i = 0; i < input.ColumnCount(); i++) {
				base_types.push_back(input.data[i].GetType());
			}
			query_input.Initialize(context.client, base_types, 1);
			query_input.SetCardinality(1);
			for (idx_t i = 0; i < input.ColumnCount(); i++) {
				query_input.data[i].Reference(input.data[i]);
			}
		}
		ExpressionExecutor query_executor(context.client, *query_vec_expr);
		if (query_input.ColumnCount() > 0) {
			query_executor.SetChunk(query_input);
		}
		Vector query_result(query_vec_expr->return_type, idx_t(1));
		query_executor.ExecuteExpression(query_result);
		query_result.Flatten(1);
		auto query_val = query_result.GetValue(0);

        vector<float> query_vec;
        if (!ExtractFloatVector(query_val, query_vec)) {
            throw InvalidInputException("VEXDB_INDEX_SCAN query vector expression did not evaluate to FLOAT[N]");
        }

        int ef = 40;
        Value ef_val;
        if (context.client.TryGetCurrentSetting("vexdb_ef_search", ef_val)) {
            ef = ef_val.GetValue<int>();
            if (ef < 1 || ef > 65535) {
                throw InvalidInputException(
                    "vexdb_ef_search must be in [1, 65535], got %d", ef);
            }
        }
        // bft is validated up front in TryOptimizeANN; by the time we run, the
        // optimizer has already routed sub-threshold queries to SEQ_SCAN.
        if (static_cast<int>(k) > ef) {
            ef = static_cast<int>(k) * 2;
        }

        auto pq_mode = GetPQSearchMode(context.client, graph_index, bool(filter_plan));
        const bool stale_quantizer = graph_index.NeedsQuantizerRebuild();
        if (stale_quantizer) pq_mode = "off";
        Value bidirectional_value;
        const bool bidirectional = context.client.TryGetCurrentSetting("vexdb_graph_bidirectional_search", bidirectional_value) &&
                                   bidirectional_value.GetValue<bool>();
        double refine_factor = 1.0;
        if (pq_mode != "off") {
            Value refine_val;
            if (context.client.TryGetCurrentSetting("vexdb_pq_refine_k_factor", refine_val)) {
                refine_factor = refine_val.GetValue<double>();
                if (!std::isfinite(refine_factor) || refine_factor < 1.0 || refine_factor > 1000.0) {
                    throw InvalidInputException(
                        "vexdb_pq_refine_k_factor must be in [1.0, 1000.0], got %.3f", refine_factor);
                }
            }
        }

        vector<row_t> result_row_ids;
        vector<float> result_distances;
        const auto &admission = search_admission;
        if (!admission) throw InternalException("VexDB index scan has no search admission state");
        auto &storage = table.GetStorage();
        auto &transaction = DuckTransaction::Get(context.client, table.ParentCatalog().GetAttached());
        idx_t requested = k, row_limit = 0;
        prepare_timer.Stop();
        auto acquire = [&] {
            VexProfileTimer timer(profile ? &profile->queue_wait_ns : nullptr);
            return admission->Acquire(context.client);
        };
        auto search_once = [&](idx_t count, const vex_duck::RowIdFilter<row_t> *mask) {
            if (profile) {
                ++profile->search_calls;
                if (profile->search_calls > 1) ++profile->retry_calls;
                profile->routes |= pq_mode == "pq_graph" ? 2 : pq_mode != "off" ? 4 : 1;
            }
            VexProfileTimer timer(profile ? &profile->search_ns : nullptr);
            VexProfileTimer retry_timer(profile && profile->search_calls > 1 ? &profile->retry_search_ns : nullptr);
            if (graph_index.UsesRaBitQ() && !stale_quantizer) {
                graph_index.SearchRaBitQ(query_vec.data(), count, ef, result_row_ids, result_distances,
                                        true, mask, profile);
            } else if (pq_mode != "off") {
                graph_index.SearchPQ(query_vec.data(), count, result_row_ids, result_distances, refine_factor,
                                     pq_mode == "pq_graph" ? ef : 0, nullptr, true, mask, bidirectional, profile);
            } else {
                graph_index.SearchANN(query_vec.data(), count, ef, result_row_ids, result_distances,
                                      true, mask, bidirectional, profile);
            }
        };
        // A prepared plan can outlive both model freshness and the transaction
        // in which it was bound. Recheck here, before acquiring graph locks.
        const bool exact_snapshot = transaction.ChangesMade() || stale_quantizer;
        if (exact_snapshot) {
            const auto *plan = filter_plan ? filter_plan.get() : quality_fallback_plan.get();
            if (!plan) throw InternalException("VexDB scan is missing its snapshot fallback plan");
            VexProfileTimer prefilter_timer(profile ? &profile->prefilter_ns : nullptr);
            auto allowed = VexScanAllowedRows(context.client, storage, transaction, *plan, profile);
            prefilter_timer.Stop();
            if (profile) {
                profile->allowed_rows += allowed.size();
                profile->routes |= 8;
                ++profile->exact_fallback_calls;
            }
            // An out-of-date model also leaves its graph topology unqualified
            // for raw navigation. Protect all storage modes until rebuild.
            if (!allowed.empty() && k) {
                auto permit = acquire();
                VexProfileTimer timer(profile ? &profile->exact_ns : nullptr);
                VexFilteredExact(context.client, storage, transaction, *plan, query_val,
                                 allowed, k, result_row_ids, result_distances, profile);
            } else if (profile) {
                profile->routes |= 16;
            }
        } else if (filter_plan) {
            Value mode_value;
            string filtered_mode = "exact";
            if (context.client.TryGetCurrentSetting("vexdb_filtered_search_mode", mode_value))
                filtered_mode = StringUtil::Lower(mode_value.ToString());
            if (filtered_mode != "auto" && filtered_mode != "exact")
                throw InvalidInputException("vexdb_filtered_search_mode must be 'auto' or 'exact'");
            // Metadata is evaluated by DuckDB expressions against this SQL
            // transaction. No storage/expression callback runs under graph locks.
            VexProfileTimer prefilter_timer(profile ? &profile->prefilter_ns : nullptr);
            auto allowed = VexScanAllowedRows(context.client, storage, transaction, *filter_plan, profile);
            prefilter_timer.Stop();
            if (profile) {
                profile->allowed_rows += allowed.size();
                if (allowed.empty()) profile->routes |= 16;
            }
            if (!allowed.empty() && k) {
                auto permit = acquire();
                const auto target = MinValue<idx_t>(k, allowed.size());
                const auto refine_budget = idx_t(std::min<double>(double(allowed.size()), double(target) * refine_factor));
                const auto budget = std::max<idx_t>(idx_t(std::max(1, ef)), refine_budget);
                const bool exact = filtered_mode == "exact" ||
                                   allowed.size() <= std::max<idx_t>(idx_t(GetBruteForceThreshold(context.client)), budget) ||
                                   (pq_mode == "off" && graph_index.IsCompactMode());
                if (!exact) {
                    VexProfileTimer mask_timer(profile ? &profile->mask_ns : nullptr);
                    const vex_duck::RowIdFilter<row_t> mask(allowed);
                    mask_timer.Stop();
                    search_once(target, &mask);
                    for (auto id : result_row_ids) if (!mask.Contains(id))
                        throw InternalException("Filtered vector search returned a row outside its snapshot predicate");
                }
                if (exact || result_row_ids.size() < target) {
                    VexProfileTimer timer(profile ? &profile->exact_ns : nullptr);
                    if (profile) {
                        profile->routes |= 8;
                        if (exact) ++profile->exact_selected_calls;
                        else ++profile->exact_fallback_calls;
                    }
                    VexFilteredExact(context.client, storage, transaction, *filter_plan, query_val,
                                     allowed, target, result_row_ids, result_distances, profile);
                }
                if (context.client.IsInterrupted()) throw InterruptException();
            }
        } else {
            // One FIFO admission/deadline per query, including visibility retries.
            // The permit is released before row fetching starts below.
            auto permit = acquire();
            while (true) {
                // Wait before SearchPQ/SearchANN acquire any graph or allocator
                // lock. A permit covers only search, never row fetch/transactions.
                {
                    search_once(requested, nullptr);
                }
                if (context.client.IsInterrupted()) throw InterruptException();
                const auto returned = result_row_ids.size();
                VexProfileTimer visibility_timer(profile ? &profile->visibility_ns : nullptr);
                idx_t visible = 0, invisible = 0;
                for (idx_t i = 0; i < returned && visible < k; ++i) {
                    if (storage.CanFetch(transaction, result_row_ids[i])) {
                        result_row_ids[visible] = result_row_ids[i];
                        result_distances[visible++] = result_distances[i];
                    } else {
                        ++invisible;
                    }
                }
                result_row_ids.resize(visible);
                result_distances.resize(visible);
                if (profile) profile->snapshot_rejected += invisible;
                visibility_timer.Stop();
                if (visible >= k || !invisible || returned < requested) break;
                // Concurrent commits can add closer rows outside this transaction's
                // snapshot. Grow only after observing visibility loss; no fixed
                // oversampling multiplier or work on the ordinary read-only path.
                if (!row_limit) row_limit = graph_index.GetRowIdCount(true);
                if (requested >= row_limit) break;
                requested += std::min(requested, row_limit - requested);
            }
        }

        VexProfileTimer fetch_timer(profile ? &profile->fetch_ns : nullptr);
        auto fetch_types = BuildOutputTypes(column_ids, returned_types);
        auto fetched = FetchRowsByRowIds(context.client, table, column_ids, fetch_types, result_row_ids, k, 0, true);
        fetch_timer.Stop();
        if (profile) profile->output_rows += result_row_ids.size();
        VexProfileTimer output_timer(profile ? &profile->output_ns : nullptr);

        scan_state.collection = make_uniq<ColumnDataCollection>(context.client, types);
        ColumnDataScanState fetch_scan;
        fetched->InitializeScan(fetch_scan);
        const auto output_capacity = MaxValue<idx_t>(1, MinValue<idx_t>(STANDARD_VECTOR_SIZE, fetched->Count()));
        DataChunk fetch_chunk;
        fetch_chunk.Initialize(context.client, fetch_types, output_capacity);
        DataChunk output_chunk;
        output_chunk.Initialize(context.client, output_buffer_types, output_capacity);
        idx_t dist_offset = 0;

        while (true) {
            fetch_chunk.Reset();
            output_chunk.Reset();
            fetched->Scan(fetch_scan, fetch_chunk);
            if (fetch_chunk.size() == 0) {
                break;
            }
            for (idx_t i = 0; i < fetch_output_positions.size(); i++) {
                output_chunk.data[fetch_output_positions[i]].Reference(fetch_chunk.data[i]);
            }
            if (!input.data.empty() && base_output_count < output_chunk.ColumnCount()) {
                for (idx_t child_idx = 0; child_idx < input.ColumnCount(); child_idx++) {
                    auto out_idx = base_output_count + child_idx;
                    if (out_idx >= output_chunk.ColumnCount()) {
                        break;
                    }
                    auto value = input.data[child_idx].GetValue(0);
                    output_chunk.data[out_idx].SetValue(0, value);
                    output_chunk.data[out_idx].SetVectorType(VectorType::CONSTANT_VECTOR);
                }
            }
            if (distance_output_index.IsValid()) {
                auto dist_data = FlatVector::GetData<float>(output_chunk.data[distance_output_index.GetIndex()]);
                for (idx_t i = 0; i < fetch_chunk.size(); i++) {
                    dist_data[i] = result_distances[dist_offset + i];
                }
            }
            output_chunk.SetCardinality(fetch_chunk.size());
            DataChunk result_chunk;
            result_chunk.InitializeEmpty(output_buffer_types);
            result_chunk.Reference(output_chunk);
            for (auto &step : output_steps) {
                if (step.predicate) {
                    ExpressionExecutor predicate(context.client, *step.predicate);
                    SelectionVector selected(result_chunk.size());
                    const auto count = predicate.SelectExpression(result_chunk, selected);
                    result_chunk.Slice(selected, count);
                }
                if (!result_chunk.size()) break;
                DataChunk projected;
                projected.Initialize(context.client, step.types, output_capacity);
                ExpressionExecutor projection(context.client, step.expressions);
                projection.Execute(&result_chunk, projected);
                result_chunk.Move(projected);
            }
            if (!result_chunk.size()) {
                dist_offset += fetch_chunk.size();
                continue;
            }
            scan_state.collection->Append(result_chunk);
            dist_offset += fetch_chunk.size();
        }
        scan_state.collection->InitializeScan(scan_state.scan_state);
        scan_state.searched = true;
    }

    chunk.Reset();
    scan_state.collection->Scan(scan_state.scan_state, chunk);
    if (chunk.size() == 0) {
        return OperatorResultType::FINISHED;
    }
    // Keep the same input until the collection is drained. Asking for another
    // input after its first batch truncates large LIMIT results at 2048 rows;
    // the dummy source has no second input to trigger the next collection scan.
    return OperatorResultType::HAVE_MORE_OUTPUT;
}

OperatorFinalizeResultType PhysicalVexIndexScan::FinalExecute(ExecutionContext &context, DataChunk &chunk,
                                                              GlobalOperatorState &gstate,
                                                              OperatorState &state) const {
    (void)context;
    (void)chunk;
    (void)gstate;
    (void)state;
    return OperatorFinalizeResultType::FINISHED;
}

} // namespace duckdb
