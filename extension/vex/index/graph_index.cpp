#include "vex_graph_index.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_create_index.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/operator/filter/physical_filter.hpp"
#include "duckdb/execution/operator/schema/physical_create_graph_index.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/parser/expression_util.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/storage/table_io_manager.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace duckdb {

// ============================================================
// GraphIndex Factory Methods
// ============================================================

unique_ptr<BoundIndex> GraphIndex::Create(CreateIndexInput &input) {
	for (auto &expr : input.unbound_expressions) {
		auto &type = expr->return_type;
		if (type.id() != LogicalTypeId::ARRAY || ArrayType::GetChildType(type).id() != LogicalTypeId::FLOAT) {
			throw InvalidInputException("GRAPH_INDEX can only be created on FLOAT[N] (ARRAY(FLOAT)) columns, got %s",
			                            type.ToString());
		}
	}

	int m = GraphIndexConfig::DEFAULT_M;
	int ef_construction = GraphIndexConfig::DEFAULT_EF_CONSTRUCTION;
	bool use_pq = false;
	uint32_t pq_m = 0;

	auto m_it = input.options.find("m");
	if (m_it != input.options.end()) {
		try {
			m = m_it->second.DefaultCastAs(LogicalType::INTEGER).GetValue<int>();
		} catch (const std::exception &) {
			throw InvalidInputException("GRAPH_INDEX: 'm' must be a valid integer, got '%s'", m_it->second.ToString());
		}
		if (m < 2 || m > 128) {
			throw InvalidInputException("GRAPH_INDEX: 'm' must be between 2 and 128, got %d", m);
		}
	}
	auto ef_it = input.options.find("ef_construction");
	if (ef_it != input.options.end()) {
		try {
			ef_construction = ef_it->second.DefaultCastAs(LogicalType::INTEGER).GetValue<int>();
		} catch (const std::exception &) {
			throw InvalidInputException("GRAPH_INDEX: 'ef_construction' must be a valid integer, got '%s'",
			                            ef_it->second.ToString());
		}
		if (ef_construction < 1 || ef_construction > 10000) {
			throw InvalidInputException("GRAPH_INDEX: 'ef_construction' must be between 1 and 10000, got %d", ef_construction);
		}
	}
	auto q_it = input.options.find("quantizer");
	if (q_it != input.options.end()) {
		auto q_val = q_it->second.GetValue<string>();
		if (q_val == "pq") {
			use_pq = true;
		} else if (q_val != "none") {
			throw InvalidInputException("GRAPH_INDEX: 'quantizer' must be 'pq' or 'none', got '%s'", q_val);
		}
	}
	auto pqm_it = input.options.find("pq_m");
	if (pqm_it != input.options.end()) {
		int pq_m_int;
		try {
			pq_m_int = pqm_it->second.DefaultCastAs(LogicalType::INTEGER).GetValue<int>();
		} catch (const std::exception &) {
			throw InvalidInputException("GRAPH_INDEX: 'pq_m' must be a valid integer, got '%s'",
			                            pqm_it->second.ToString());
		}
		if (pq_m_int < 1 || pq_m_int > 256) {
			throw InvalidInputException("GRAPH_INDEX: 'pq_m' must be between 1 and 256, got %d", pq_m_int);
		}
		pq_m = static_cast<uint32_t>(pq_m_int);
	}
	uint16_t max_dedup = GraphIndexCore::DEFAULT_MAX_DEDUP;
	auto dedup_it = input.options.find("max_dedup");
	if (dedup_it != input.options.end()) {
		int val;
		try {
			val = dedup_it->second.DefaultCastAs(LogicalType::INTEGER).GetValue<int>();
		} catch (const std::exception &) {
			throw InvalidInputException("GRAPH_INDEX: 'max_dedup' must be a valid integer, got '%s'",
			                            dedup_it->second.ToString());
		}
		if (val < 1 || val > 65535) {
			throw InvalidInputException("GRAPH_INDEX: 'max_dedup' must be between 1 and 65535, got %d", val);
		}
		max_dedup = static_cast<uint16_t>(val);
	}
	vex::VexMetric metric = vex::VexMetric::L2;
	auto metric_it = input.options.find("metric");
	if (metric_it != input.options.end()) {
		metric = vex::ParseMetric(metric_it->second.GetValue<string>());
	}

	auto graph_index = make_uniq<GraphIndex>(
		input.name, input.constraint_type, input.column_ids, input.table_io_manager,
		input.unbound_expressions, input.db, m, ef_construction, metric, use_pq, pq_m);
	graph_index->graph_.max_dedup = max_dedup;

	// Deserialize from storage if available (allocator-based)
	// Only deserialize if we have actual data (dimension stored means there was data)
	if (input.storage_info.IsValid() && input.storage_info.options.count("dimension")) {
		graph_index->DeserializeFromStorage(input.storage_info);
	} else {
		// Legacy BLOB deserialization (backward compatibility)
		auto data_it = input.storage_info.options.find("graph_data");
		if (data_it != input.storage_info.options.end()) {
			auto blob = data_it->second.GetValueUnsafe<string>();
			if (!graph_index->DeserializeFromBlob(blob)) {
				throw IOException("GRAPH_INDEX: failed to deserialize from legacy BLOB — data may be corrupted");
			}
		}
	}

	return std::move(graph_index);
}

PhysicalOperator &GraphIndex::CreatePlan(PlanIndexInput &input) {
	auto &op = input.op;
	auto &planner = input.planner;

	vector<LogicalType> new_column_types;
	vector<unique_ptr<Expression>> select_list;
	for (idx_t i = 0; i < op.expressions.size(); i++) {
		new_column_types.push_back(op.expressions[i]->return_type);
		select_list.push_back(std::move(op.expressions[i]));
	}
	new_column_types.emplace_back(LogicalType::ROW_TYPE);
	select_list.push_back(make_uniq<BoundReferenceExpression>(LogicalType::ROW_TYPE, op.info->scan_types.size() - 1));

	auto &proj = planner.Make<PhysicalProjection>(new_column_types, std::move(select_list), op.estimated_cardinality);
	proj.children.push_back(input.table_scan);

	reference<PhysicalOperator> prev_op(proj);
	auto is_alter = op.alter_table_info != nullptr;
	if (!is_alter) {
		vector<LogicalType> filter_types;
		vector<unique_ptr<Expression>> filter_select_list;
		auto not_null_type = ExpressionType::OPERATOR_IS_NOT_NULL;

		for (idx_t i = 0; i < new_column_types.size() - 1; i++) {
			filter_types.push_back(new_column_types[i]);
			auto is_not_null_expr = make_uniq<BoundOperatorExpression>(not_null_type, LogicalType::BOOLEAN);
			auto bound_ref = make_uniq<BoundReferenceExpression>(new_column_types[i], i);
			is_not_null_expr->children.push_back(std::move(bound_ref));
			filter_select_list.push_back(std::move(is_not_null_expr));
		}

		prev_op = planner.Make<PhysicalFilter>(std::move(filter_types), std::move(filter_select_list),
		                                       op.estimated_cardinality);
		prev_op.get().types.emplace_back(LogicalType::ROW_TYPE);
		prev_op.get().children.push_back(proj);
	}

	auto &create_idx = planner.Make<PhysicalCreateGraphIndex>(op, op.table, op.info->column_ids, std::move(op.info),
	                                                            std::move(op.unbound_expressions), op.estimated_cardinality,
	                                                            std::move(op.alter_table_info));
	create_idx.children.push_back(prev_op);
	return create_idx;
}

// ============================================================
// Constructor
// ============================================================

GraphIndex::GraphIndex(const string &name, IndexConstraintType constraint_type,
                       const vector<column_t> &column_ids, TableIOManager &table_io_manager,
                       const vector<unique_ptr<Expression>> &unbound_expressions,
                       AttachedDatabase &db, int m, int ef_construction,
                       vex::VexMetric metric, bool use_pq, uint32_t pq_m)
    : BoundIndex(name, GraphIndex::TYPE_NAME, constraint_type, column_ids, table_io_manager,
                 unbound_expressions, db),
      m_(m),
      ef_construction_(ef_construction),
      dimension_(0),
      use_pq_(use_pq),
      pq_m_(pq_m),
      rng_(std::random_device{}()),
      dist_(0.0, 1.0),
      metric_(metric) {

	distance_func_ = vex::GetDistanceFunc(metric_);

	if (m_ < GraphIndexConfig::MIN_M || m_ > GraphIndexConfig::MAX_M) {
		throw InvalidInputException("M must be between %d and %d, got %d",
		                            GraphIndexConfig::MIN_M, GraphIndexConfig::MAX_M, m_);
	}
	if (ef_construction_ < GraphIndexConfig::MIN_EF_CONSTRUCTION ||
	    ef_construction_ > GraphIndexConfig::MAX_EF_CONSTRUCTION) {
		throw InvalidInputException("ef_construction must be between %d and %d, got %d",
		                            GraphIndexConfig::MIN_EF_CONSTRUCTION,
		                            GraphIndexConfig::MAX_EF_CONSTRUCTION, ef_construction_);
	}

	// Initialize graph core parameters
	graph_.m = m_;
}

// ============================================================
// Helper: ensure allocators are initialized
// ============================================================

void GraphIndex::EnsureAllocators() {
	if (!graph_.node_alloc && dimension_ > 0) {
		auto &block_manager = table_io_manager.GetIndexBlockManager();
		graph_.dimension = dimension_;
		graph_.InitAllocators(block_manager);
	}
}

// ============================================================
// Helper: Get Random Level
// ============================================================

int GraphIndex::GetRandomLevel() {
	double ml = GraphIndexConfig::GetMl(m_);
	double r = dist_(rng_);
	if (r == 0.0) r = std::numeric_limits<double>::min();
	int level = static_cast<int>(-std::log(r) * ml);
	return std::min(level, GraphIndexConfig::GetMaxLevel(m_));
}

// ============================================================
// Build Index from Data Chunk
// ============================================================

void GraphIndex::Build(DataChunk &chunk, Vector &row_ids) {
	D_ASSERT(chunk.ColumnCount() >= 1);

	auto count = chunk.size();
	if (count == 0) return;

	auto &vec_vector = chunk.data[0];

	vec_vector.Flatten(count);
	row_ids.Flatten(count);
	auto row_id_data = FlatVector::GetData<row_t>(row_ids);

	auto &child_vec = ArrayVector::GetEntry(vec_vector);
	auto vec_data = FlatVector::GetData<float>(child_vec);
	auto &validity = FlatVector::Validity(vec_vector);

	// Detect dimension from the actual data type
	if (dimension_ == 0) {
		auto &vec_type = vec_vector.GetType();
		if (vec_type.id() == LogicalTypeId::ARRAY) {
			dimension_ = ArrayType::GetSize(vec_type);
		} else if (!logical_types.empty() && logical_types[0].id() == LogicalTypeId::ARRAY) {
			dimension_ = ArrayType::GetSize(logical_types[0]);
		}
		if (dimension_ == 0) {
			throw InvalidInputException("GRAPH_INDEX: cannot determine vector dimension from type %s, logical_types size=%d",
			                            vec_vector.GetType().ToString(), static_cast<int>(logical_types.size()));
		}
		graph_.dimension = dimension_;
	}

	EnsureAllocators();

	std::vector<float> norm_buf;
	if (metric_ == vex::VexMetric::COSINE) {
		norm_buf.resize(dimension_);
	}

	for (idx_t i = 0; i < count; i++) {
		if (!validity.RowIsValid(i)) continue;

		row_t row_id = row_id_data[i];
		const float *vec = vec_data + i * dimension_;

		// For cosine metric, normalize vectors before storing in index
		if (metric_ == vex::VexMetric::COSINE) {
			std::memcpy(norm_buf.data(), vec, dimension_ * sizeof(float));
			vex::NormalizeVector(norm_buf.data(), dimension_);
			vec = norm_buf.data();
		}

		// Try deduplication first: if an identical vector exists with capacity, merge
		if (graph_.max_dedup > 1 && graph_.TryDedup(row_id, vec, dimension_, distance_func_)) {
			continue; // Merged into existing node, no new graph node needed
		}

		int level = GetRandomLevel();

		IndexPointer node_ptr = graph_.AllocateNode(row_id, vec, dimension_, static_cast<uint8_t>(level));
		graph_.InsertNode(node_ptr, m_, ef_construction_, distance_func_);
	}
}

// ============================================================
// Thread-Safe Build for Parallel Index Creation
// ============================================================

void GraphIndex::BuildConcurrent(DataChunk &chunk, Vector &row_ids) {
	D_ASSERT(chunk.ColumnCount() >= 1);
	auto count = chunk.size();
	if (count == 0) return;

	auto &vec_vector = chunk.data[0];
	vec_vector.Flatten(count);
	row_ids.Flatten(count);
	auto row_id_data = FlatVector::GetData<row_t>(row_ids);

	auto &child_vec = ArrayVector::GetEntry(vec_vector);
	auto vec_data = FlatVector::GetData<float>(child_vec);
	auto &validity = FlatVector::Validity(vec_vector);

	// Thread-safe one-time initialization (dimension, allocators, mutex)
	std::call_once(dimension_init_flag_, [&]() {
		auto &vec_type = vec_vector.GetType();
		if (vec_type.id() == LogicalTypeId::ARRAY) {
			dimension_ = ArrayType::GetSize(vec_type);
		} else if (!logical_types.empty() && logical_types[0].id() == LogicalTypeId::ARRAY) {
			dimension_ = ArrayType::GetSize(logical_types[0]);
		}
		if (dimension_ == 0) {
			throw InvalidInputException("GRAPH_INDEX: cannot determine vector dimension");
		}
		graph_.dimension = dimension_;
		EnsureAllocators();
		graph_.InitGraphMutex();
	});

	// ================================================================
	// Phase 1: Prepare all nodes (no lock needed)
	// Pre-normalize cosine vectors, generate random levels
	// ================================================================
	struct PendingNode {
		idx_t chunk_idx;
		row_t row_id;
		int level;
		IndexPointer node_ptr;
		bool skip;
	};
	std::vector<PendingNode> pending;
	pending.reserve(count);

	// For cosine metric, pre-normalize all vectors into contiguous buffer
	std::vector<float> norm_storage;
	if (metric_ == vex::VexMetric::COSINE) {
		norm_storage.resize(count * dimension_);
	}

	idx_t invalid_count = 0;
	for (idx_t i = 0; i < count; i++) {
		if (!validity.RowIsValid(i)) {
			invalid_count++;
			continue;
		}

		PendingNode pn;
		pn.chunk_idx = i;
		pn.row_id = row_id_data[i];
		pn.skip = false;

		if (metric_ == vex::VexMetric::COSINE) {
			float *dst = norm_storage.data() + i * dimension_;
			std::memcpy(dst, vec_data + i * dimension_, dimension_ * sizeof(float));
			vex::NormalizeVector(dst, dimension_);
		}

		{
			std::lock_guard<std::mutex> lock(rng_mutex_);
			pn.level = GetRandomLevel();
		}
		pending.push_back(pn);
	}

	// ================================================================
	// Phase 2+3: Per-node allocate + insert (interleaved for parallelism)
	// Each node: brief exclusive lock for allocation, then InsertNodeConcurrent
	// with its own shared/exclusive locking for search/connect.
	// This allows multiple threads to overlap their search phases.
	// ================================================================
	for (auto &pn : pending) {
		const float *vec = (metric_ == vex::VexMetric::COSINE)
			? (norm_storage.data() + pn.chunk_idx * dimension_)
			: (vec_data + pn.chunk_idx * dimension_);

		// Allocate under brief exclusive lock
		{
			std::lock_guard<SimpleRWLock> lock(*graph_.graph_mutex_);

			// Try dedup first: check existing graph nodes
			if (graph_.max_dedup > 1 && graph_.TryDedup(pn.row_id, vec, dimension_, distance_func_)) {
				pn.skip = true;
				continue;
			}

			// Intra-batch dedup: check previously allocated nodes in this batch
			if (graph_.max_dedup > 1) {
				bool merged = false;
				for (auto &prev : pending) {
					if (&prev == &pn) break;
					if (prev.skip) continue;
					const float *prev_vec = (metric_ == vex::VexMetric::COSINE)
						? (norm_storage.data() + prev.chunk_idx * dimension_)
						: (vec_data + prev.chunk_idx * dimension_);
					if (std::memcmp(vec, prev_vec, dimension_ * sizeof(float)) == 0) {
						auto *prev_header = graph_.GetNode(prev.node_ptr);
						if (prev_header->extra_row_count + 1 < graph_.max_dedup) {
							idx_t node_key = prev.node_ptr.Get();
							graph_.dedup_map_[node_key].push_back(pn.row_id);
							prev_header->extra_row_count++;
							graph_.row_id_map[pn.row_id] = prev.node_ptr;
							pn.skip = true;
							merged = true;
							break;
						}
					}
				}
				if (merged) continue;
			}

			pn.node_ptr = graph_.AllocateNode(pn.row_id, vec, dimension_, static_cast<uint8_t>(pn.level));
		}

		// Insert into graph — InsertNodeConcurrent uses its own locking:
		// shared lock for search (parallel), exclusive lock for connect (serialized)
		graph_.InsertNodeConcurrent(pn.node_ptr, m_, ef_construction_, distance_func_);
	}
}

// ============================================================
// BuildParallel: Allocate all nodes first, then insert in parallel
// ============================================================

void GraphIndex::BuildParallel(const std::vector<float> &all_vectors, const std::vector<row_t> &all_row_ids,
                                idx_t total_count, uint32_t dim, int num_threads) {
	if (total_count == 0) return;

	// Initialize dimension and allocators (single-threaded)
	if (dimension_ == 0) {
		dimension_ = dim;
		graph_.dimension = dim;
	}
	EnsureAllocators();
	graph_.InitGraphMutex();

	// Pre-normalize cosine vectors
	std::vector<float> norm_storage;
	const float *vec_data = all_vectors.data();
	if (metric_ == vex::VexMetric::COSINE) {
		norm_storage.resize(total_count * dimension_);
		std::memcpy(norm_storage.data(), all_vectors.data(), total_count * dimension_ * sizeof(float));
		for (idx_t i = 0; i < total_count; i++) {
			vex::NormalizeVector(norm_storage.data() + i * dimension_, dimension_);
		}
		vec_data = norm_storage.data();
	}

	// Phase 1: Generate random levels and allocate ALL nodes (single-threaded, fast)
	struct AllocatedNode {
		IndexPointer node_ptr;
		bool skip = false;
	};
	std::vector<AllocatedNode> nodes(total_count);

	for (idx_t i = 0; i < total_count; i++) {
		int level = GetRandomLevel();
		const float *vec = vec_data + i * dimension_;

		// Dedup check against existing graph nodes (only when appending to existing index)
		if (graph_.max_dedup > 1 && graph_.has_entry_point &&
		    graph_.TryDedup(all_row_ids[i], vec, dimension_, distance_func_)) {
			nodes[i].skip = true;
			continue;
		}

		// Intra-batch dedup is handled during insertion phase (InsertNodeParallel),
		// following the VexDB approach: after HNSW neighbor search finds the nearest
		// node at level 0, check if distance ≈ 0 and merge. This is O(ef_construction)
		// per node instead of the previous O(n²) brute-force scan.

		nodes[i].node_ptr = graph_.AllocateNode(all_row_ids[i], vec, dimension_, static_cast<uint8_t>(level));
	}

	// Set entry point to first non-skipped node (single-threaded)
	idx_t first_node_idx = total_count;
	for (idx_t i = 0; i < total_count; i++) {
		if (!nodes[i].skip) {
			if (!graph_.has_entry_point) {
				auto *header = graph_.GetNode(nodes[i].node_ptr);
				graph_.entry_point = nodes[i].node_ptr;
				graph_.has_entry_point = true;
				graph_.max_level = header->level;
				// Insert first node (no connections needed, it's the entry point)
				nodes[i].skip = true; // Mark as already handled
			}
			if (first_node_idx == total_count) {
				first_node_idx = i;
			}
			break;
		}
	}

	// Count nodes to insert
	std::vector<idx_t> insert_indices;
	insert_indices.reserve(total_count);
	for (idx_t i = 0; i < total_count; i++) {
		if (!nodes[i].skip) {
			insert_indices.push_back(i);
		}
	}

	if (insert_indices.empty()) return;

	// Phase 2: Insert all nodes — parallel for large datasets, single-threaded for small
	// Parallel build may slightly reduce graph quality due to concurrent neighbor updates.
	// For small datasets (< 10K nodes), single-threaded build is fast enough and produces
	// optimal graph quality. For large datasets, parallel speedup outweighs quality tradeoff.
	static constexpr idx_t PARALLEL_THRESHOLD = 10000;
	int actual_threads = std::min(num_threads, static_cast<int>(insert_indices.size()));
	if (insert_indices.size() < PARALLEL_THRESHOLD) {
		actual_threads = 1; // Force single-threaded for small datasets
	}
	// Build buffer pointer caches to bypass FixedSizeBuffer mutex/pin overhead.
	// Without caches, FixedSizeBuffer::Get() may unpin/repin buffers, causing pointer
	// instability during lock-free reads in InsertNodeParallel's search phase.
	// This is required for BOTH single-threaded and multi-threaded builds.
	graph_.BuildBufferCaches();

	if (actual_threads <= 1) {
		// Single-threaded: use InsertNode (original non-parallel version) for best graph quality
		for (auto idx : insert_indices) {
			// Try dedup before inserting (after first node is in the graph)
			if (graph_.max_dedup > 1 && graph_.has_entry_point) {
				auto *header = graph_.GetNode(nodes[idx].node_ptr);
				const float *vec = graph_.GetVector(header->vector_ptr);
				if (graph_.TryDedup(header->row_id, vec, dimension_, distance_func_)) {
					graph_.FreeNode(nodes[idx].node_ptr);
					graph_.node_count--;
					continue;
				}
			}
			graph_.InsertNode(nodes[idx].node_ptr, m_, ef_construction_, distance_func_);
		}
		graph_.ClearBufferCaches();
		return;
	}

	// Collect deduped nodes per thread for post-parallel cleanup
	std::vector<std::vector<idx_t>> deduped_per_thread(actual_threads);

	std::vector<std::thread> threads;
	threads.reserve(actual_threads);
	idx_t nodes_per_thread = insert_indices.size() / actual_threads;
	idx_t remainder = insert_indices.size() % actual_threads;

	idx_t offset = 0;
	for (int t = 0; t < actual_threads; t++) {
		idx_t count = nodes_per_thread + (t < static_cast<int>(remainder) ? 1 : 0);
		idx_t start = offset;
		idx_t end = offset + count;
		offset = end;

		threads.emplace_back([this, &nodes, &insert_indices, &deduped_per_thread, t, start, end]() {
			for (idx_t i = start; i < end; i++) {
				if (graph_.InsertNodeParallel(nodes[insert_indices[i]].node_ptr,
				                             m_, ef_construction_, distance_func_)) {
					deduped_per_thread[t].push_back(insert_indices[i]);
				}
			}
		});
	}

	for (auto &t : threads) {
		t.join();
	}

	// Clear caches — subsequent operations use normal locked access
	graph_.ClearBufferCaches();

	// Free deduped nodes (single-threaded, after parallel phase)
	for (auto &deduped : deduped_per_thread) {
		for (auto idx : deduped) {
			graph_.FreeNode(nodes[idx].node_ptr);
			graph_.node_count--;
		}
	}
}

// ============================================================
// BoundIndex Interface Implementation
// ============================================================

ErrorData GraphIndex::Append(IndexLock &l, DataChunk &chunk, Vector &row_ids) {
	if (chunk.ColumnCount() != logical_types.size()) {
		DataChunk key_chunk;
		key_chunk.InitializeEmpty(logical_types);
		for (idx_t i = 0; i < column_ids.size() && i < logical_types.size(); i++) {
			key_chunk.data[i].Reference(chunk.data[column_ids[i]]);
		}
		key_chunk.SetCardinality(chunk.size());
		Build(key_chunk, row_ids);
	} else {
		Build(chunk, row_ids);
	}
	return ErrorData();
}

ErrorData GraphIndex::Append(IndexLock &l, DataChunk &chunk, Vector &row_ids, IndexAppendInfo &info) {
	return Append(l, chunk, row_ids);
}

void GraphIndex::VerifyAppend(DataChunk &chunk, IndexAppendInfo &info, optional_ptr<ConflictManager> manager) {
}

void GraphIndex::VerifyConstraint(DataChunk &chunk, IndexAppendInfo &info, ConflictManager &manager) {
}

void GraphIndex::Delete(IndexLock &state, DataChunk &entries, Vector &row_identifiers) {
	auto count = entries.size();
	row_identifiers.Flatten(count);
	auto row_id_data = FlatVector::GetData<row_t>(row_identifiers);

	// Phase 1: Classify each row_id as primary or extra (dedup)
	std::vector<IndexPointer> newly_deleted; // graph nodes to fully remove
	for (idx_t i = 0; i < count; i++) {
		row_t row_id = row_id_data[i];
		auto it = graph_.row_id_map.find(row_id);
		if (it == graph_.row_id_map.end()) continue;

		IndexPointer node_ptr = it->second;
		auto *header = graph_.GetNode(node_ptr);

		if (header->row_id == row_id) {
			// This is the primary row_id for this node
			if (header->extra_row_count > 0) {
				// Promote an extra row_id to primary
				idx_t node_key = node_ptr.Get();
				auto dit = graph_.dedup_map_.find(node_key);
				if (dit != graph_.dedup_map_.end() && !dit->second.empty()) {
					row_t new_primary = dit->second.back();
					dit->second.pop_back();
					header->extra_row_count--;
					header->row_id = new_primary;
					// Update row_id_map: remove old primary, new primary already maps to this node
					graph_.row_id_map.erase(row_id);
					if (dit->second.empty()) {
						graph_.dedup_map_.erase(dit);
					}
					continue; // Node stays alive with new primary
				}
			}
			// No extras: mark for full deletion
			header->deleted = 1;
			newly_deleted.push_back(node_ptr);
		} else {
			// This is an extra (dedup) row_id — just remove from dedup_map
			idx_t node_key = node_ptr.Get();
			auto dit = graph_.dedup_map_.find(node_key);
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

	if (newly_deleted.empty()) return;

	if (newly_deleted.size() <= graph_.node_count) {
		graph_.node_count -= newly_deleted.size();
	} else {
		graph_.node_count = 0;
	}

	// Phase 2: Update entry_point if it was deleted
	if (graph_.has_entry_point) {
		auto *ep_header = graph_.GetNode(graph_.entry_point);
		if (ep_header->deleted) {
			graph_.entry_point.Clear();
			graph_.has_entry_point = false;
			graph_.max_level = 0;
			for (auto &pair : graph_.row_id_map) {
				auto *h = graph_.GetNode(pair.second);
				if (!h->deleted && static_cast<int>(h->level) > graph_.max_level) {
					graph_.entry_point = pair.second;
					graph_.has_entry_point = true;
					graph_.max_level = h->level;
				}
			}
		}
	}

	// Build set of deleted node IndexPointers for fast lookup
	unordered_set<idx_t> deleted_ptrs;
	for (auto &del_ptr : newly_deleted) {
		deleted_ptrs.insert(del_ptr.Get());
	}

	// Phase 3: Remove deleted nodes from neighbor lists
	for (auto &pair : graph_.row_id_map) {
		auto *header = graph_.GetNode(pair.second);
		if (header->deleted) continue;

		for (int level = 0; level <= static_cast<int>(header->level); level++) {
			IndexPointer *neighbors;
			uint16_t nb_count;
			graph_.GetNeighbors(pair.second, level, neighbors, nb_count);
			if (!neighbors) continue;

			uint16_t write_idx = 0;
			for (uint16_t r = 0; r < nb_count; r++) {
				if (neighbors[r].Get() && deleted_ptrs.find(neighbors[r].Get()) == deleted_ptrs.end()) {
					auto *nb_h = graph_.GetNode(neighbors[r]);
					if (!nb_h->deleted) {
						neighbors[write_idx++] = neighbors[r];
						continue;
					}
				}
			}
			if (write_idx < nb_count) {
				for (uint16_t c = write_idx; c < nb_count; c++) {
					neighbors[c].Clear();
				}
				graph_.SetNeighborCount(pair.second, level, write_idx);
			}
		}
	}

	// Free deleted nodes and clean up dedup_map entries
	for (auto &del_ptr : newly_deleted) {
		auto *header = graph_.GetNode(del_ptr);
		idx_t node_key = del_ptr.Get();
		// Remove any remaining dedup entries for this node
		graph_.dedup_map_.erase(node_key);
		graph_.row_id_map.erase(header->row_id);
		graph_.FreeNode(del_ptr);
	}
}

void GraphIndex::CommitDrop(IndexLock &index_lock) {
	Clear();
}

ErrorData GraphIndex::Insert(IndexLock &l, DataChunk &chunk, Vector &row_ids) {
	return Append(l, chunk, row_ids);
}

ErrorData GraphIndex::Insert(IndexLock &l, DataChunk &chunk, Vector &row_ids, IndexAppendInfo &info) {
	return Insert(l, chunk, row_ids);
}

bool GraphIndex::MergeIndexes(IndexLock &state, BoundIndex &other_index) {
	auto &other = other_index.Cast<GraphIndex>();

	if (dimension_ == 0) {
		dimension_ = other.dimension_;
		graph_.dimension = dimension_;
		metric_ = other.metric_;
		distance_func_ = vex::GetDistanceFunc(metric_);
	}

	EnsureAllocators();

	// Re-insert nodes from other index
	unordered_set<idx_t> seen_nodes;
	for (auto &pair : other.graph_.row_id_map) {
		// Skip extra dedup row_ids — they share the same node_ptr
		if (seen_nodes.find(pair.second.Get()) != seen_nodes.end()) continue;
		seen_nodes.insert(pair.second.Get());

		auto *other_header = other.graph_.GetNode(pair.second);
		if (other_header->deleted) continue;

		auto *other_vec = other.graph_.GetVector(other_header->vector_ptr);

		// Collect all row_ids for this node (primary + extras)
		std::vector<row_t> all_row_ids;
		other.graph_.CollectNodeRowIds(pair.second, all_row_ids);

		// Insert primary
		row_t primary_rid = all_row_ids[0];

		// Try dedup into existing node first
		if (graph_.max_dedup > 1 && graph_.TryDedup(primary_rid, other_vec, dimension_, distance_func_)) {
			// Primary merged; also try merging extras
			for (idx_t r = 1; r < all_row_ids.size(); r++) {
				if (!graph_.TryDedup(all_row_ids[r], other_vec, dimension_, distance_func_)) {
					// Can't merge extra — need a new node for remaining
					IndexPointer new_node = graph_.AllocateNode(all_row_ids[r], other_vec, dimension_, 0);
					graph_.InsertNode(new_node, m_, ef_construction_, distance_func_);
				}
			}
		} else {
			IndexPointer new_node = graph_.AllocateNode(primary_rid, other_vec, dimension_, other_header->level);
			graph_.InsertNode(new_node, m_, ef_construction_, distance_func_);
			// Add extras via dedup
			for (idx_t r = 1; r < all_row_ids.size(); r++) {
				if (!graph_.TryDedup(all_row_ids[r], other_vec, dimension_, distance_func_)) {
					IndexPointer extra_node = graph_.AllocateNode(all_row_ids[r], other_vec, dimension_, 0);
					graph_.InsertNode(extra_node, m_, ef_construction_, distance_func_);
				}
			}
		}
	}

	return true;
}

void GraphIndex::Vacuum(IndexLock &l) {
	// Deleted nodes are freed synchronously in Delete(), nothing to do here.
}

// ============================================================
// Serialization — using FixedSizeAllocator native persistence
// ============================================================

IndexStorageInfo GraphIndex::SerializeToDisk(QueryContext context,
                                              const case_insensitive_map_t<Value> &options) {
	IndexStorageInfo info;
	info.name = name;

	if (!graph_.node_alloc || graph_.node_count == 0) {
		return info;
	}

	// Store index metadata in options
	info.options["m"] = Value::INTEGER(m_);
	info.options["ef_construction"] = Value::INTEGER(ef_construction_);
	info.options["dimension"] = Value::UINTEGER(dimension_);
	info.options["node_count"] = Value::UBIGINT(graph_.node_count);
	info.options["max_level"] = Value::INTEGER(graph_.max_level);
	info.options["entry_point"] = Value::UBIGINT(graph_.entry_point.Get());
	info.options["use_pq"] = Value::BOOLEAN(use_pq_);
	info.options["metric"] = Value::UTINYINT(static_cast<uint8_t>(metric_));
	info.options["max_dedup"] = Value::USMALLINT(graph_.max_dedup);

	// Store root as entry_point data
	info.root = graph_.entry_point.Get();

	// Serialize row_id_map as BLOB (row_id -> IndexPointer pairs)
	{
		string rid_blob;
		uint64_t num_entries = graph_.row_id_map.size();
		rid_blob.append(reinterpret_cast<const char *>(&num_entries), sizeof(num_entries));
		for (auto &pair : graph_.row_id_map) {
			row_t rid = pair.first;
			uint64_t ptr_val = pair.second.Get();
			rid_blob.append(reinterpret_cast<const char *>(&rid), sizeof(rid));
			rid_blob.append(reinterpret_cast<const char *>(&ptr_val), sizeof(ptr_val));
		}
		info.options["row_id_map"] = Value::BLOB(const_data_ptr_cast(rid_blob.data()), rid_blob.size());
	}

	// Serialize dedup_map_ as BLOB
	if (!graph_.dedup_map_.empty()) {
		string dedup_blob;
		uint64_t num_nodes = graph_.dedup_map_.size();
		dedup_blob.append(reinterpret_cast<const char *>(&num_nodes), sizeof(num_nodes));
		for (auto &pair : graph_.dedup_map_) {
			uint64_t node_key = pair.first;
			uint64_t num_extras = pair.second.size();
			dedup_blob.append(reinterpret_cast<const char *>(&node_key), sizeof(node_key));
			dedup_blob.append(reinterpret_cast<const char *>(&num_extras), sizeof(num_extras));
			for (auto &rid : pair.second) {
				dedup_blob.append(reinterpret_cast<const char *>(&rid), sizeof(rid));
			}
		}
		info.options["dedup_map"] = Value::BLOB(const_data_ptr_cast(dedup_blob.data()), dedup_blob.size());
	}

	// Serialize allocator buffers to disk
	auto &block_manager = table_io_manager.GetIndexBlockManager();
	PartialBlockManager partial_block_manager(context, block_manager, PartialBlockType::FULL_CHECKPOINT);

	graph_.node_alloc->SerializeBuffers(partial_block_manager);
	graph_.vector_alloc->SerializeBuffers(partial_block_manager);
	graph_.upper_alloc->SerializeBuffers(partial_block_manager);
	partial_block_manager.FlushPartialBlocks();

	// Collect allocator infos
	info.allocator_infos.push_back(graph_.node_alloc->GetInfo());
	info.allocator_infos.push_back(graph_.vector_alloc->GetInfo());
	info.allocator_infos.push_back(graph_.upper_alloc->GetInfo());

	// PQ data stored as BLOB in options (PQ codes are relatively small)
	if (use_pq_ && graph_.pq.trained) {
		std::vector<char> pq_blob;
		pq_blob.push_back(1);
		graph_.pq.SerializeTo(pq_blob);
		uint64_t codes_size = graph_.pq_codes.size();
		size_t pos = pq_blob.size();
		pq_blob.resize(pos + 8 + codes_size);
		memcpy(pq_blob.data() + pos, &codes_size, 8);
		memcpy(pq_blob.data() + pos + 8, graph_.pq_codes.data(), codes_size);
		info.options["pq_data"] = Value::BLOB(const_data_ptr_cast(pq_blob.data()), pq_blob.size());
	}

	return info;
}

IndexStorageInfo GraphIndex::SerializeToWAL(const case_insensitive_map_t<Value> &options) {
	IndexStorageInfo info;
	info.name = name;

	if (!graph_.node_alloc || graph_.node_count == 0) {
		return info;
	}

	// Store index metadata
	info.options["m"] = Value::INTEGER(m_);
	info.options["ef_construction"] = Value::INTEGER(ef_construction_);
	info.options["dimension"] = Value::UINTEGER(dimension_);
	info.options["node_count"] = Value::UBIGINT(graph_.node_count);
	info.options["max_level"] = Value::INTEGER(graph_.max_level);
	info.options["entry_point"] = Value::UBIGINT(graph_.entry_point.Get());
	info.options["use_pq"] = Value::BOOLEAN(use_pq_);
	info.options["metric"] = Value::UTINYINT(static_cast<uint8_t>(metric_));
	info.options["max_dedup"] = Value::USMALLINT(graph_.max_dedup);

	info.root = graph_.entry_point.Get();

	// Serialize row_id_map as BLOB
	{
		string rid_blob;
		uint64_t num_entries = graph_.row_id_map.size();
		rid_blob.append(reinterpret_cast<const char *>(&num_entries), sizeof(num_entries));
		for (auto &pair : graph_.row_id_map) {
			row_t rid = pair.first;
			uint64_t ptr_val = pair.second.Get();
			rid_blob.append(reinterpret_cast<const char *>(&rid), sizeof(rid));
			rid_blob.append(reinterpret_cast<const char *>(&ptr_val), sizeof(ptr_val));
		}
		info.options["row_id_map"] = Value::BLOB(const_data_ptr_cast(rid_blob.data()), rid_blob.size());
	}

	// Serialize dedup_map_ as BLOB
	if (!graph_.dedup_map_.empty()) {
		string dedup_blob;
		uint64_t num_nodes = graph_.dedup_map_.size();
		dedup_blob.append(reinterpret_cast<const char *>(&num_nodes), sizeof(num_nodes));
		for (auto &pair : graph_.dedup_map_) {
			uint64_t node_key = pair.first;
			uint64_t num_extras = pair.second.size();
			dedup_blob.append(reinterpret_cast<const char *>(&node_key), sizeof(node_key));
			dedup_blob.append(reinterpret_cast<const char *>(&num_extras), sizeof(num_extras));
			for (auto &rid : pair.second) {
				dedup_blob.append(reinterpret_cast<const char *>(&rid), sizeof(rid));
			}
		}
		info.options["dedup_map"] = Value::BLOB(const_data_ptr_cast(dedup_blob.data()), dedup_blob.size());
	}

	// WAL serialization: include buffer data
	info.buffers.push_back(graph_.node_alloc->InitSerializationToWAL());
	info.buffers.push_back(graph_.vector_alloc->InitSerializationToWAL());
	info.buffers.push_back(graph_.upper_alloc->InitSerializationToWAL());

	info.allocator_infos.push_back(graph_.node_alloc->GetInfo());
	info.allocator_infos.push_back(graph_.vector_alloc->GetInfo());
	info.allocator_infos.push_back(graph_.upper_alloc->GetInfo());

	// PQ data
	if (use_pq_ && graph_.pq.trained) {
		std::vector<char> pq_blob;
		pq_blob.push_back(1);
		graph_.pq.SerializeTo(pq_blob);
		uint64_t codes_size = graph_.pq_codes.size();
		size_t pos = pq_blob.size();
		pq_blob.resize(pos + 8 + codes_size);
		memcpy(pq_blob.data() + pos, &codes_size, 8);
		memcpy(pq_blob.data() + pos + 8, graph_.pq_codes.data(), codes_size);
		info.options["pq_data"] = Value::BLOB(const_data_ptr_cast(pq_blob.data()), pq_blob.size());
	}

	return info;
}

// ============================================================
// Deserialization from allocator-based storage
// ============================================================

void GraphIndex::DeserializeFromStorage(const IndexStorageInfo &info) {
	// Read metadata from options
	auto m_it = info.options.find("m");
	if (m_it != info.options.end()) {
		m_ = m_it->second.GetValue<int>();
		graph_.m = m_;
	}
	auto ef_it = info.options.find("ef_construction");
	if (ef_it != info.options.end()) {
		ef_construction_ = ef_it->second.GetValue<int>();
	}
	auto dim_it = info.options.find("dimension");
	if (dim_it != info.options.end()) {
		dimension_ = dim_it->second.GetValue<uint32_t>();
		graph_.dimension = dimension_;
	}
	auto nc_it = info.options.find("node_count");
	if (nc_it != info.options.end()) {
		graph_.node_count = nc_it->second.GetValue<uint64_t>();
	}
	auto ml_it = info.options.find("max_level");
	if (ml_it != info.options.end()) {
		graph_.max_level = ml_it->second.GetValue<int>();
	}
	auto ep_it = info.options.find("entry_point");
	if (ep_it != info.options.end()) {
		graph_.entry_point.Set(ep_it->second.GetValue<uint64_t>());
	}
	graph_.has_entry_point = (graph_.node_count > 0);
	auto pq_it = info.options.find("use_pq");
	if (pq_it != info.options.end()) {
		use_pq_ = pq_it->second.GetValue<bool>();
	}
	auto metric_opt = info.options.find("metric");
	if (metric_opt != info.options.end()) {
		metric_ = static_cast<vex::VexMetric>(metric_opt->second.GetValue<uint8_t>());
		distance_func_ = vex::GetDistanceFunc(metric_);
	}

	auto dedup_opt = info.options.find("max_dedup");
	if (dedup_opt != info.options.end()) {
		graph_.max_dedup = dedup_opt->second.GetValue<uint16_t>();
	}

	// Create allocators WITHOUT slot-0 reservation.
	// The serialized bitmask already has slot 0 reserved from the original InitAllocators().
	// Calling EnsureAllocators() here would reserve slot 0 again (adding buffer 0 to
	// buffers_with_free_space), then Init() replaces buffer 0 with the on-disk version
	// but leaves the stale buffers_with_free_space entry. If the on-disk buffer is full,
	// a subsequent New() would try to allocate from a full buffer and hit
	// "Invalid bitmask for FixedSizeAllocator".
	if (!graph_.node_alloc && dimension_ > 0) {
		auto &block_manager = table_io_manager.GetIndexBlockManager();
		graph_.dimension = dimension_;
		graph_.node_alloc = make_uniq<FixedSizeAllocator>(vex::HNSWNodeHeader::SegmentSize(graph_.m), block_manager);
		graph_.vector_alloc = make_uniq<FixedSizeAllocator>(static_cast<idx_t>(dimension_) * sizeof(float), block_manager);
		graph_.upper_alloc = make_uniq<FixedSizeAllocator>(vex::HNSWUpperLevel::SegmentSize(graph_.m), block_manager);
	}

	// Initialize allocators from storage info (lazy disk loading)
	if (info.allocator_infos.size() >= 3) {
		graph_.node_alloc->Init(info.allocator_infos[0]);
		graph_.vector_alloc->Init(info.allocator_infos[1]);
		graph_.upper_alloc->Init(info.allocator_infos[2]);
	}

	// Restore row_id_map from serialized BLOB
	auto rid_it = info.options.find("row_id_map");
	if (rid_it != info.options.end()) {
		auto rid_blob = rid_it->second.GetValueUnsafe<string>();
		const char *rptr = rid_blob.data();
		const char *rend = rptr + rid_blob.size();
		if (rptr + sizeof(uint64_t) <= rend) {
			uint64_t num_entries;
			memcpy(&num_entries, rptr, sizeof(num_entries));
			rptr += sizeof(num_entries);
			for (uint64_t i = 0; i < num_entries && rptr + sizeof(row_t) + sizeof(uint64_t) <= rend; i++) {
				row_t rid;
				uint64_t ptr_val;
				memcpy(&rid, rptr, sizeof(rid));
				rptr += sizeof(rid);
				memcpy(&ptr_val, rptr, sizeof(ptr_val));
				rptr += sizeof(ptr_val);
				IndexPointer nptr;
				nptr.Set(ptr_val);
				graph_.row_id_map[rid] = nptr;
			}
		}
	}
	graph_.row_id_map_built = true;

	// Deserialize dedup_map_ from BLOB
	auto dedup_map_it = info.options.find("dedup_map");
	if (dedup_map_it != info.options.end()) {
		auto dedup_blob = dedup_map_it->second.GetValueUnsafe<string>();
		const char *dptr = dedup_blob.data();
		const char *dend = dptr + dedup_blob.size();
		if (dptr + sizeof(uint64_t) <= dend) {
			uint64_t num_nodes;
			memcpy(&num_nodes, dptr, sizeof(num_nodes));
			dptr += sizeof(num_nodes);
			for (uint64_t i = 0; i < num_nodes && dptr + 2 * sizeof(uint64_t) <= dend; i++) {
				uint64_t node_key, num_extras;
				memcpy(&node_key, dptr, sizeof(node_key));
				dptr += sizeof(node_key);
				memcpy(&num_extras, dptr, sizeof(num_extras));
				dptr += sizeof(num_extras);
				std::vector<row_t> extras;
				extras.reserve(num_extras);
				for (uint64_t j = 0; j < num_extras && dptr + sizeof(row_t) <= dend; j++) {
					row_t rid;
					memcpy(&rid, dptr, sizeof(rid));
					dptr += sizeof(rid);
					extras.push_back(rid);
				}
				if (!extras.empty()) {
					graph_.dedup_map_[static_cast<idx_t>(node_key)] = std::move(extras);
				}
			}
		}
	}

	// Deserialize PQ data
	auto pq_data_it = info.options.find("pq_data");
	if (pq_data_it != info.options.end()) {
		auto pq_blob = pq_data_it->second.GetValueUnsafe<string>();
		const char *ptr = pq_blob.data();
		const char *end = ptr + pq_blob.size();
		if (ptr < end) {
			uint8_t has_pq = static_cast<uint8_t>(*ptr); ptr += 1;
			if (has_pq) {
				use_pq_ = true;
				if (!graph_.pq.DeserializeFrom(ptr, end)) {
					use_pq_ = false;
					return; // PQ data corrupt, skip
				}
				if (ptr + 8 <= end) {
					uint64_t codes_size;
					memcpy(&codes_size, ptr, 8); ptr += 8;
					if (ptr + codes_size <= end) {
						graph_.pq_codes.resize(codes_size);
						memcpy(graph_.pq_codes.data(), ptr, codes_size);
					}
				}
			}
		}
	}
}

void GraphIndex::RebuildRowIdMap() {
	// row_id_map is now restored from serialized BLOB in DeserializeFromStorage.
	// This method is kept for interface compatibility but is a no-op.
	graph_.row_id_map_built = true;
}

// ============================================================
// Legacy BLOB deserialization (backward compatibility)
// ============================================================

static constexpr uint32_t GRAPH_INDEX_MAGIC = 0x58444947; // "GIDX"

bool GraphIndex::DeserializeFromBlob(const string &blob) {
	if (blob.empty()) return false;

	const char *ptr = blob.data();
	const char *end = ptr + blob.size();

	auto read_u32 = [&]() -> uint32_t {
		if (ptr + 4 > end) throw IOException("GIDX blob truncated at u32 read");
		uint32_t v; memcpy(&v, ptr, 4); ptr += 4; return v;
	};
	auto read_u64 = [&]() -> uint64_t {
		if (ptr + 8 > end) throw IOException("GIDX blob truncated at u64 read");
		uint64_t v; memcpy(&v, ptr, 8); ptr += 8; return v;
	};
	auto read_i32 = [&]() -> int32_t {
		if (ptr + 4 > end) throw IOException("GIDX blob truncated at i32 read");
		int32_t v; memcpy(&v, ptr, 4); ptr += 4; return v;
	};
	auto read_i64 = [&]() -> int64_t {
		if (ptr + 8 > end) throw IOException("GIDX blob truncated at i64 read");
		int64_t v; memcpy(&v, ptr, 8); ptr += 8; return v;
	};
	auto read_u8 = [&]() -> uint8_t {
		if (ptr + 1 > end) throw IOException("GIDX blob truncated at u8 read");
		uint8_t v = static_cast<uint8_t>(*ptr); ptr += 1; return v;
	};

	if (static_cast<size_t>(end - ptr) < 40) return false;
	uint32_t magic = read_u32();
	if (magic != GRAPH_INDEX_MAGIC) return false;
	uint32_t version = read_u32();
	if (version != 1 && version != 2) return false;

	m_ = static_cast<int>(read_u32());
	ef_construction_ = static_cast<int>(read_u32());
	dimension_ = read_u32();
	uint64_t node_count = read_u64();
	int32_t max_level = read_i32();
	int64_t ep_idx = read_i64();

	graph_.m = m_;
	graph_.dimension = dimension_;
	graph_.max_level = max_level;

	EnsureAllocators();

	// Read nodes: first pass - allocate and store vectors
	struct TempNode {
		IndexPointer node_ptr;
		row_t row_id;
		uint8_t level;
	};
	std::vector<TempNode> temp_nodes;
	temp_nodes.reserve(node_count);

	for (uint64_t i = 0; i < node_count; i++) {
		if (static_cast<size_t>(end - ptr) < 10 + dimension_ * sizeof(float)) return false;
		row_t row_id = static_cast<row_t>(read_i64());
		uint8_t level = read_u8();
		read_u8(); // deleted flag (always 0 in serialized data)

		const float *vec_data = reinterpret_cast<const float *>(ptr);
		ptr += dimension_ * sizeof(float);

		IndexPointer node_ptr = graph_.AllocateNode(row_id, vec_data, dimension_, level);
		temp_nodes.push_back({node_ptr, row_id, level});
	}

	// Read neighbor lists: second pass
	for (uint64_t i = 0; i < node_count; i++) {
		auto &tn = temp_nodes[i];
		auto *header = graph_.GetNode(tn.node_ptr);

		for (uint8_t l = 0; l <= header->level; l++) {
			uint32_t neighbor_count = read_u32();

			IndexPointer *neighbors;
			uint16_t dummy_count;
			graph_.GetNeighbors(tn.node_ptr, l, neighbors, dummy_count);
			if (!neighbors) {
				// Skip neighbor data (with bounds check)
				size_t skip_bytes = static_cast<size_t>(neighbor_count) * 8;
				if (ptr + skip_bytes > end) return false;
				ptr += skip_bytes;
				continue;
			}

			uint16_t actual_count = 0;
			for (uint32_t n = 0; n < neighbor_count; n++) {
				uint64_t neighbor_idx = read_u64();
				if (neighbor_idx < temp_nodes.size()) {
					neighbors[actual_count++] = temp_nodes[neighbor_idx].node_ptr;
				}
			}
			graph_.SetNeighborCount(tn.node_ptr, l, actual_count);
		}
	}

	// Set entry point
	if (ep_idx >= 0 && static_cast<uint64_t>(ep_idx) < temp_nodes.size()) {
		graph_.entry_point = temp_nodes[ep_idx].node_ptr;
		graph_.has_entry_point = true;
	}

	// v2: PQ data
	if (version >= 2 && ptr < end) {
		uint8_t has_pq = static_cast<uint8_t>(*ptr); ptr += 1;
		if (has_pq) {
			use_pq_ = true;
			graph_.pq.DeserializeFrom(ptr, end);
			if (ptr + 8 <= end) {
				uint64_t codes_size;
				memcpy(&codes_size, ptr, 8); ptr += 8;
				if (ptr + codes_size <= end) {
					graph_.pq_codes.resize(codes_size);
					memcpy(graph_.pq_codes.data(), ptr, codes_size);
				}
			}
		}
	}

	graph_.row_id_map_built = true;
	return true;
}

// ============================================================
// Other BoundIndex methods
// ============================================================

idx_t GraphIndex::GetInMemorySize(IndexLock &state) {
	idx_t size = sizeof(GraphIndex);
	if (graph_.node_alloc) size += graph_.node_alloc->GetInMemorySize();
	if (graph_.vector_alloc) size += graph_.vector_alloc->GetInMemorySize();
	if (graph_.upper_alloc) size += graph_.upper_alloc->GetInMemorySize();
	size += graph_.row_id_map.size() * (sizeof(row_t) + sizeof(IndexPointer));
	return size;
}

void GraphIndex::Verify(IndexLock &l) {
}

string GraphIndex::ToString(IndexLock &l, bool display_ascii) {
	return StringUtil::Format("GraphIndex: %d nodes, %d levels, M=%d, dim=%d",
	                         static_cast<int>(graph_.node_count),
	                         graph_.max_level, m_, dimension_);
}

void GraphIndex::VerifyAllocations(IndexLock &l) {
}

void GraphIndex::VerifyBuffers(IndexLock &l) {
}

string GraphIndex::GetConstraintViolationMessage(VerifyExistenceType verify_type, idx_t failed_index,
                                                  DataChunk &input) {
	throw NotImplementedException("Graph index does not support uniqueness constraints");
}

// ============================================================
// Search API
// ============================================================

void GraphIndex::Search(const float *query_vec, idx_t k, int ef,
                         std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
                         idx_t brute_force_threshold) {
	// For cosine metric, normalize query vector to match stored normalized vectors
	std::vector<float> norm_query;
	const float *search_vec = query_vec;
	if (metric_ == vex::VexMetric::COSINE && graph_.dimension > 0) {
		norm_query.assign(query_vec, query_vec + graph_.dimension);
		vex::NormalizeVector(norm_query.data(), graph_.dimension);
		search_vec = norm_query.data();
	}

	if (use_pq_ && metric_ == vex::VexMetric::L2) {
		if (!graph_.pq.trained && graph_.node_count > 0) {
			graph_.TrainPQ(pq_m_);
		}
		graph_.SearchWithPQ(search_vec, k, ef, out_row_ids, out_distances, distance_func_, brute_force_threshold);
	} else {
		graph_.Search(search_vec, k, ef, out_row_ids, out_distances, distance_func_, brute_force_threshold);
	}
}

unique_ptr<IndexScanState> GraphIndex::TryInitializeScan(const Expression &expr, const Expression &filter_expr) {
	return nullptr;
}

bool GraphIndex::Scan(IndexScanState &state, idx_t max_count, set<row_t> &row_ids) {
	auto &scan_state = state.Cast<GraphIndexScanState>();

	if (!scan_state.initialized) return false;

	if (scan_state.current_offset == 0 && !scan_state.query_vector.empty()) {
		Search(scan_state.query_vector.data(), scan_state.k, scan_state.ef,
		       scan_state.row_ids, scan_state.distances);
	}

	row_ids.clear();
	if (scan_state.current_offset >= scan_state.row_ids.size()) return true;
	idx_t remaining = std::min(max_count, scan_state.row_ids.size() - scan_state.current_offset);
	for (idx_t i = 0; i < remaining; i++) {
		row_ids.insert(scan_state.row_ids[scan_state.current_offset + i]);
	}
	scan_state.current_offset += remaining;

	return scan_state.current_offset >= scan_state.row_ids.size();
}

void GraphIndex::ANNSearch(const float *query_vec, idx_t k, int ef,
                           std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
                           idx_t brute_force_threshold) {
	Search(query_vec, k, ef, out_row_ids, out_distances, brute_force_threshold);
}

void GraphIndex::Clear() {
	graph_.Clear();
	dimension_ = 0;
}

constexpr const char *GraphIndex::TYPE_NAME;

} // namespace duckdb
