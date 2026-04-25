#include "vex_graph_index.hpp"

#include "duckdb/common/exception.hpp"
#include "vex_duckdb_compat.hpp"
#include "vex_core_bridge_graph_helpers.hpp"
#include "vex/vex_adapter_graph_runtime.hpp"
#include "vex/vex_adapter_quant_runtime.hpp"
#include "vex/vex_graph_maintenance.hpp"
#include "vex/vex_quant_distancer.hpp"

#include <algorithm>
#include <cstring>

namespace duckdb {

void GraphIndex::BuildViaCoreBridgeOrThrow(DataChunk &chunk, Vector &row_ids) {
	auto count = chunk.size();
	auto &vec_vector = chunk.data[0];
	auto row_id_data = FlatVector::GetData<row_t>(row_ids);
	auto &child_vec = ArrayVector::GetEntry(vec_vector);
	auto vec_data = FlatVector::GetData<float>(child_vec);
	auto &validity = FlatVector::Validity(vec_vector);

	auto ctx = CreateCoreBridgeContextOrThrow();
	LoadCoreBridgeGraphOrThrow(*ctx);

	std::vector<float> norm_buf;

	for (idx_t i = 0; i < count; i++) {
		if (!validity.RowIsValid(i)) {
			continue;
		}

		row_t row_id = row_id_data[i];
		const float *vec = vec_data + i * dimension_;
		vec = ::vex::PrepareQueryVector(vec, dimension_, metric_ == vex::VexMetric::COSINE, norm_buf);

		if (graph_.max_dedup > 1 && graph_.TryDedup(row_id, vec, dimension_, distance_func_)) {
			continue;
		}

		const uint8_t level = static_cast<uint8_t>(GetRandomLevel());
		auto node_id = ::vex::AddPointToGraph(*ctx->core_graph, *ctx->store, row_id, vec, dimension_, row_id, level);
		if (node_id == ::vex::INVALID_NODE_ID) {
			throw InternalException("DuckDB core bridge build: failed to add point");
		}

		if (graph_.meta_segment_size > 0 && chunk.ColumnCount() > 1) {
			std::vector<uint8_t> meta_buf(graph_.meta_segment_size, 0);
			ExtractMetadata(chunk, i, meta_buf.data());
			WriteCoreBridgeMetadata(*ctx, node_id, meta_buf.data(), "build");
		}
		if (!SyncCoreBridgeGraphState(*ctx)) {
			throw InternalException("DuckDB core bridge build: failed to persist graph state");
		}
	}

	if (!SyncCoreBridgeGraphState(*ctx)) {
		throw InternalException("DuckDB core bridge build: failed to finalize graph state");
	}
}

void GraphIndex::BuildParallelViaCoreBridgeOrThrow(const std::vector<float> &all_vectors,
                                                   const std::vector<row_t> &all_row_ids,
                                                   idx_t total_count, uint32_t dim,
                                                   const std::vector<uint8_t> &all_metadata) {
	if (total_count == 0) {
		return;
	}
	if (dimension_ == 0) {
		dimension_ = dim;
		graph_.dimension = dim;
	}
	if (dimension_ != dim) {
		throw InvalidInputException("GRAPH_INDEX: dimension mismatch in BuildParallel");
	}
	if (all_vectors.size() < static_cast<size_t>(total_count) * static_cast<size_t>(dimension_)) {
		throw InvalidInputException("GRAPH_INDEX: BuildParallel vector buffer is truncated");
	}
	if (graph_.meta_segment_size > 0 && !all_metadata.empty() &&
	    all_metadata.size() < static_cast<size_t>(total_count) * static_cast<size_t>(graph_.meta_segment_size)) {
		throw InvalidInputException("GRAPH_INDEX: BuildParallel metadata buffer is truncated");
	}

	auto ctx = CreateCoreBridgeContextOrThrow();
	LoadCoreBridgeGraphOrThrow(*ctx);

	std::vector<float> norm_buf;

	for (idx_t i = 0; i < total_count; i++) {
		const float *vec = all_vectors.data() + static_cast<size_t>(i) * static_cast<size_t>(dimension_);
		vec = ::vex::PrepareQueryVector(vec, dimension_, metric_ == vex::VexMetric::COSINE, norm_buf);

		if (graph_.max_dedup > 1 && graph_.TryDedup(all_row_ids[i], vec, dimension_, distance_func_)) {
			continue;
		}

		const uint8_t level = static_cast<uint8_t>(GetRandomLevel());
		auto node_id = ::vex::AddPointToGraph(*ctx->core_graph, *ctx->store, all_row_ids[i], vec, dimension_,
		                                      all_row_ids[i], level);
		if (node_id == ::vex::INVALID_NODE_ID) {
			throw InternalException("DuckDB core bridge build-parallel: failed to add point");
		}

		if (graph_.meta_segment_size > 0 && !all_metadata.empty()) {
			auto meta_ptr = all_metadata.data() + static_cast<size_t>(i) * static_cast<size_t>(graph_.meta_segment_size);
			WriteCoreBridgeMetadata(*ctx, node_id, meta_ptr, "build-parallel");
		}

		if (!SyncCoreBridgeGraphState(*ctx)) {
			throw InternalException("DuckDB core bridge build-parallel: failed to persist graph state");
		}
	}

	if (!SyncCoreBridgeGraphState(*ctx)) {
		throw InternalException("DuckDB core bridge build-parallel: failed to finalize graph state");
	}
}

void GraphIndex::DeleteViaCoreBridgeOrThrow(Vector &row_identifiers, idx_t count) {
	auto row_id_data = FlatVector::GetData<row_t>(row_identifiers);

	auto ctx = CreateCoreBridgeContextOrThrow();
	LoadCoreBridgeGraphOrThrow(*ctx);

	std::vector<::vex::node_id_t> newly_deleted;
	std::vector<idx_t> deleted_storage_keys;
	deleted_storage_keys.reserve(count);
	for (idx_t i = 0; i < count; i++) {
		::vex::node_id_t node_id = ::vex::INVALID_NODE_ID;
		const row_t row_id = row_id_data[i];
		if (!ctx->binding->ResolveNodeIdByRowId(row_id, node_id)) {
			continue;
		}

		uint64_t storage_key = 0;
		if (!ctx->binding->ResolveStorageNodeKey(node_id, storage_key)) {
			throw InternalException("DuckDB core bridge delete: failed to resolve storage key for node %u", node_id);
		}

		auto handle = ctx->store->PinNodeForUpdate(node_id);
		if (!handle) {
			throw InternalException("DuckDB core bridge delete: failed to pin node %u for update", node_id);
		}
		auto *header = handle->MutableHeader();
		if (!header) {
			throw InternalException("DuckDB core bridge delete: node %u has no mutable header", node_id);
		}

		if (header->row_id == row_id) {
			if (header->extra_row_count > 0) {
				auto dit = graph_.dedup_map_.find(static_cast<idx_t>(storage_key));
				if (dit != graph_.dedup_map_.end() && !dit->second.empty()) {
					row_t new_primary = dit->second.back();
					dit->second.pop_back();
					header->extra_row_count--;
					header->row_id = new_primary;
					graph_.row_id_map.erase(row_id);
					if (dit->second.empty()) {
						graph_.dedup_map_.erase(dit);
					}
					continue;
				}
			}

			header->deleted = 1;
			if (std::find(newly_deleted.begin(), newly_deleted.end(), node_id) == newly_deleted.end()) {
				newly_deleted.push_back(node_id);
				deleted_storage_keys.push_back(static_cast<idx_t>(storage_key));
			}
		} else {
			auto dit = graph_.dedup_map_.find(static_cast<idx_t>(storage_key));
			if (dit != graph_.dedup_map_.end()) {
				auto &extras = dit->second;
				extras.erase(std::remove(extras.begin(), extras.end(), row_id), extras.end());
				header->extra_row_count = static_cast<uint16_t>(extras.size());
				if (extras.empty()) {
					graph_.dedup_map_.erase(dit);
				}
			}
			graph_.row_id_map.erase(row_id);
		}
	}

	if (newly_deleted.empty()) {
		return;
	}

	::vex::DeleteNodesFromGraph(*ctx->store, ctx->graph_state, newly_deleted, m_);
	for (auto storage_key : deleted_storage_keys) {
		graph_.dedup_map_.erase(storage_key);
	}

	if (!::vex::StoreGraphStateToBinding(*ctx->binding, ctx->graph_state)) {
		throw InternalException("DuckDB core bridge delete: failed to store graph state");
	}
	if (!ImportBridgeGraphState(ctx->storage, graph_)) {
		throw InternalException("DuckDB core bridge delete: failed to import updated graph state");
	}
}

void GraphIndex::SearchViaCoreBridgeOrThrow(const float *query_vec, idx_t k, int ef,
                                            std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
                                            idx_t brute_force_threshold) {
	if (use_pq_) {
		throw InternalException("DuckDB core bridge search: unexpected PQ search routed to non-PQ path");
	}
	if (!graph_.has_entry_point || graph_.node_count == 0) {
		return;
	}
	auto *entry_header = graph_.GetNode(graph_.entry_point);
	if (!entry_header || entry_header->deleted) {
		throw InternalException("DuckDB core bridge search: entry point is invalid or deleted");
	}

	auto ctx = CreateCoreBridgeContextOrThrow();
	std::vector<row_t> core_row_ids;
	std::vector<float> core_distances;
	if (!::vex::ExecuteBindingSearch(ctx->store->Config().low_level_binding, *ctx->store, ToCoreMetric(metric_), m_,
	                                 ef_construction_, query_vec, static_cast<uint32_t>(k), ef, core_row_ids,
	                                 core_distances, brute_force_threshold, &ctx->graph_state)) {
		throw InternalException("DuckDB core bridge search: libvex-core search execution failed");
	}
	ExpandCoreBridgeResults(core_row_ids, core_distances, k, out_row_ids, out_distances);
}

void GraphIndex::PQSearchViaCoreBridgeOrThrow(const float *query_vec, idx_t k, int ef,
                                              std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
                                              idx_t brute_force_threshold) {
	if (!use_pq_ || metric_ != vex::VexMetric::L2 || graph_.pq.CodeSize() == 0) {
		throw InternalException("DuckDB core bridge PQ search: unexpected non-PQ configuration");
	}
	if (!graph_.pq.trained || graph_.pq_codes.empty()) {
		throw InternalException("DuckDB core bridge PQ search: PQ state is not ready");
	}
	if (!graph_.has_entry_point || graph_.node_count == 0) {
		return;
	}

	auto ctx = CreateCoreBridgeContextOrThrow();
	std::vector<const uint8_t *> code_by_node_id;
	if (!::vex::BuildNodeFlatCodePointerIndex(*ctx->store, graph_.row_id_map, graph_.pq_codes,
	                                          graph_.pq.CodeSize(), code_by_node_id)) {
		throw InternalException("DuckDB core bridge PQ search: failed to build PQ code index");
	}
	auto &distancer = EnsureCoreBridgePQDistancerOrThrow();

	std::vector<row_t> core_row_ids;
	std::vector<float> core_distances;
	if (!::vex::ExecuteBindingQuantizedSearch(
	        ctx->store->Config().low_level_binding, *ctx->store, ToCoreMetric(metric_), m_, ef_construction_, query_vec,
	        static_cast<uint32_t>(k), ef, core_row_ids, core_distances, distancer,
	        [&code_by_node_id](::vex::node_id_t node_id) -> const uint8_t * {
		        if (node_id >= code_by_node_id.size()) {
			        return nullptr;
		        }
		        return code_by_node_id[node_id];
	        },
	        brute_force_threshold, &ctx->graph_state)) {
		throw InternalException("DuckDB core bridge PQ search: libvex-core PQ search execution failed");
	}
	ExpandCoreBridgeResults(core_row_ids, core_distances, k, out_row_ids, out_distances);
}

::vex::PQDistancerCore &GraphIndex::EnsureCoreBridgePQDistancerOrThrow() {
	if (graph_.pq.CodeSize() == 0 || !graph_.pq.trained) {
		throw InternalException("DuckDB core bridge PQ search: quantizer state is not ready");
	}
	if (!graph_.pq_distancer_core) {
		graph_.pq_distancer_core = make_uniq<::vex::PQDistancerCore>(graph_.pq.m);
	}
	::vex::LoadQuantizerIntoDistancer(graph_.pq_distancer_core.get(), graph_.pq);
	return *graph_.pq_distancer_core;
}

void GraphIndex::FilteredSearchViaCoreBridgeOrThrow(const float *query_vec, idx_t k, int ef,
                                                    std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
                                                    const vex::FilterPredicate &filter,
                                                    idx_t brute_force_threshold) {
	if (use_pq_ || !graph_.meta_alloc || graph_.meta_segment_size == 0) {
		throw InternalException("DuckDB core bridge filtered-search: unexpected filtered-search configuration");
	}
	if (!graph_.has_entry_point || graph_.node_count == 0) {
		return;
	}
	auto *entry_header = graph_.GetNode(graph_.entry_point);
	if (!entry_header || entry_header->deleted) {
		throw InternalException("DuckDB core bridge filtered-search: entry point is invalid or deleted");
	}
	auto core_filter = CloneFilterToCore(filter);
	if (!core_filter) {
		throw InternalException("DuckDB core bridge filtered-search: failed to translate filter to core");
	}

	auto ctx = CreateCoreBridgeContextOrThrow();
	std::vector<row_t> core_row_ids;
	std::vector<float> core_distances;
	if (!::vex::ExecuteBindingFilteredSearch(ctx->store->Config().low_level_binding, *ctx->store,
	                                         ToCoreMetric(metric_), m_, ef_construction_, query_vec,
	                                         static_cast<uint32_t>(k), ef, core_row_ids, core_distances,
	                                         *core_filter, brute_force_threshold, &ctx->graph_state)) {
		throw InternalException("DuckDB core bridge filtered-search: libvex-core filtered execution failed");
	}
	ExpandCoreBridgeResults(core_row_ids, core_distances, k, out_row_ids, out_distances);
}

void GraphIndex::ExpandCoreBridgeResults(const std::vector<row_t> &core_row_ids,
                                         const std::vector<float> &core_distances,
                                         idx_t k, std::vector<row_t> &out_row_ids,
                                         std::vector<float> &out_distances) const {
	::vex::ExpandSearchResults(core_row_ids, core_distances, static_cast<size_t>(k),
	                           [&](row_t core_row_id, float core_distance, size_t remaining) -> size_t {
		size_t appended = 0;
		out_row_ids.push_back(core_row_id);
		out_distances.push_back(core_distance);
		appended++;

		auto row_it = graph_.row_id_map.find(core_row_id);
		if (row_it == graph_.row_id_map.end()) {
			return appended;
		}
		auto dedup_it = graph_.dedup_map_.find(row_it->second.Get());
		if (dedup_it == graph_.dedup_map_.end()) {
			return appended;
		}
		for (auto extra_row_id : dedup_it->second) {
			if (appended >= remaining) {
				break;
			}
			out_row_ids.push_back(extra_row_id);
			out_distances.push_back(core_distance);
			appended++;
		}
		return appended;
	});
}

} // namespace duckdb
