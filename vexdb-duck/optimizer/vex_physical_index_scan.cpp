#include "vex_physical_index_scan.hpp"

#include "vex_fetch_utils.hpp"
#include "vex_graph_index.hpp"

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/execution/operator/scan/physical_dummy_scan.hpp"

namespace duckdb {

static constexpr const char *kAlgorithmBruteForce = "brute-force";
static constexpr const char *kAlgorithmHnsw = "hnsw";

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
        phys_scan.algorithm_used = (static_cast<int64_t>(graph_index.GetNodeCount()) <= bft)
                                       ? kAlgorithmBruteForce
                                       : kAlgorithmHnsw;
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
    vector<ColumnBinding> result;
    for (idx_t i = 0; i < output_types.size(); i++) {
        result.emplace_back(table_index, i);
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
}

string PhysicalVexIndexScan::GetName() const {
    return "VEX_INDEX_SCAN";
}

InsertionOrderPreservingMap<string> PhysicalVexIndexScan::ParamsToString() const {
    InsertionOrderPreservingMap<string> result;
    result["Index"] = graph_index.GetIndexName();
    result["Top"] = to_string(k);
    if (!algorithm_used.empty()) {
        result["Algorithm"] = algorithm_used;
    }
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
		DataChunk query_input;
		if (!input.data.empty() && input.size() > 0) {
			vector<LogicalType> base_types;
			base_types.reserve(input.ColumnCount());
			for (idx_t i = 0; i < input.ColumnCount(); i++) {
				base_types.push_back(input.data[i].GetType());
			}
			query_input.Initialize(context.client, base_types);
			query_input.SetCardinality(1);
			for (idx_t i = 0; i < input.ColumnCount(); i++) {
				query_input.data[i].Reference(input.data[i]);
			}
		}
		ExpressionExecutor query_executor(context.client, *query_vec_expr);
		if (query_input.ColumnCount() > 0) {
			query_executor.SetChunk(query_input);
		}
		Vector query_result(query_vec_expr->return_type);
		query_executor.ExecuteExpression(query_result);
		query_result.Flatten(1);
		auto query_val = query_result.GetValue(0);

        vector<float> query_vec;
        if (!ExtractFloatVector(query_val, query_vec)) {
            throw InvalidInputException("VEX_INDEX_SCAN query vector expression did not evaluate to FLOAT[N]");
        }

        int ef = 40;
        Value ef_val;
        if (context.client.TryGetCurrentSetting("vex_ef_search", ef_val)) {
            ef = ef_val.GetValue<int>();
            if (ef < 1 || ef > 65535) {
                throw InvalidInputException(
                    "vex_ef_search must be in [1, 65535], got %d", ef);
            }
        }
        // bft is validated up front in TryOptimizeANN; by the time we run, the
        // optimizer has already routed sub-threshold queries to SEQ_SCAN.
        if (static_cast<int>(k) > ef) {
            ef = static_cast<int>(k) * 2;
        }

        bool pq_only = false;
        Value pq_mode_val;
        if (context.client.TryGetCurrentSetting("vex_pq_search_mode", pq_mode_val)) {
            auto mode = StringUtil::Lower(pq_mode_val.ToString());
            if (mode == "pq_only") {
                pq_only = true;
            } else if (mode != "off" && !mode.empty()) {
                throw InvalidInputException(
                    "vex_pq_search_mode must be 'off' or 'pq_only', got '%s'", pq_mode_val.ToString());
            }
        }

        vector<row_t> result_row_ids;
        vector<float> result_distances;
        if (pq_only) {
            if (!graph_index.UsesPQ()) {
                throw InvalidInputException(
                    "vex_pq_search_mode='pq_only' requires the index to be built with WITH (quantizer='pq', pq_m=N)");
            }
            double refine_factor = 1.0;
            Value refine_val;
            if (context.client.TryGetCurrentSetting("vex_pq_refine_k_factor", refine_val)) {
                refine_factor = refine_val.GetValue<double>();
                if (refine_factor < 1.0 || refine_factor > 1000.0) {
                    throw InvalidInputException(
                        "vex_pq_refine_k_factor must be in [1.0, 1000.0], got %.3f", refine_factor);
                }
            }
            graph_index.SearchPQ(query_vec.data(), k, result_row_ids, result_distances, refine_factor);
        } else {
            graph_index.SearchANN(query_vec.data(), k, ef, result_row_ids, result_distances);
        }

        auto fetch_types = BuildOutputTypes(column_ids, returned_types);
        auto fetched = FetchRowsByRowIds(context.client, table, column_ids, fetch_types, result_row_ids, k, 0);

        scan_state.collection = make_uniq<ColumnDataCollection>(context.client, types);
        ColumnDataScanState fetch_scan;
        fetched->InitializeScan(fetch_scan);
        DataChunk fetch_chunk;
        fetch_chunk.Initialize(context.client, fetch_types);
        DataChunk output_chunk;
        output_chunk.Initialize(context.client, types);
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
            scan_state.collection->Append(output_chunk);
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
    return OperatorResultType::NEED_MORE_INPUT;
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
