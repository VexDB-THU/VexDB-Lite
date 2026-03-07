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
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace duckdb {

// ============================================================
// GraphIndex Factory Methods
// ============================================================

unique_ptr<BoundIndex> GraphIndex::Create(CreateIndexInput &input) {
	// Validate that the indexed column is an ARRAY(FLOAT) type (FLOATVECTOR)
	for (auto &expr : input.unbound_expressions) {
		auto &type = expr->return_type;
		if (type.id() != LogicalTypeId::ARRAY || ArrayType::GetChildType(type).id() != LogicalTypeId::FLOAT) {
			throw InvalidInputException("GRAPH_INDEX can only be created on FLOATVECTOR (ARRAY(FLOAT)) columns, got %s",
			                            type.ToString());
		}
	}

	int m = GraphIndexConfig::DEFAULT_M;
	int ef_construction = GraphIndexConfig::DEFAULT_EF_CONSTRUCTION;
	bool use_pq = false;
	uint32_t pq_m = 0;

	auto m_it = input.options.find("m");
	if (m_it != input.options.end()) {
		m = m_it->second.GetValue<int>();
	}
	auto ef_it = input.options.find("ef_construction");
	if (ef_it != input.options.end()) {
		ef_construction = ef_it->second.GetValue<int>();
	}
	auto q_it = input.options.find("quantizer");
	if (q_it != input.options.end()) {
		auto q_val = q_it->second.GetValue<string>();
		if (q_val == "pq") {
			use_pq = true;
		}
	}
	auto pqm_it = input.options.find("pq_m");
	if (pqm_it != input.options.end()) {
		pq_m = static_cast<uint32_t>(pqm_it->second.GetValue<int>());
	}

	auto graph_index = make_uniq<GraphIndex>(
		input.name,
		input.constraint_type,
		input.column_ids,
		input.table_io_manager,
		input.unbound_expressions,
		input.db,
		m,
		ef_construction,
		use_pq,
		pq_m
	);

	auto data_it = input.storage_info.options.find("graph_data");
	if (data_it != input.storage_info.options.end()) {
		auto blob = data_it->second.GetValueUnsafe<string>();
		graph_index->DeserializeFromBlob(blob);
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
                       bool use_pq, uint32_t pq_m)
    : BoundIndex(name, GraphIndex::TYPE_NAME, constraint_type, column_ids, table_io_manager,
                 unbound_expressions, db),
      m_(m),
      ef_construction_(ef_construction),
      dimension_(0),
      use_pq_(use_pq),
      pq_m_(pq_m),
      rng_(std::random_device{}()),
      dist_(0.0, 1.0) {

	distance_func_ = vex::GetL2SqrFunc();

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
}

// ============================================================
// Helper: Get Random Level
// ============================================================

int GraphIndex::GetRandomLevel() {
	double ml = GraphIndexConfig::GetMl(m_);
	int level = static_cast<int>(-std::log(dist_(rng_)) / ml);
	return std::min(level, GraphIndexConfig::GetMaxLevel(m_));
}

// ============================================================
// Build Index from Data Chunk
// ============================================================

void GraphIndex::Build(DataChunk &chunk, Vector &row_ids) {
	D_ASSERT(chunk.ColumnCount() >= 1);

	auto count = chunk.size();
	if (count == 0) {
		return;
	}

	auto &vec_vector = chunk.data[0];
	auto row_id_data = FlatVector::GetData<row_t>(row_ids);

	vec_vector.Flatten(count);

	auto &child_vec = ArrayVector::GetEntry(vec_vector);
	auto vec_data = FlatVector::GetData<float>(child_vec);
	auto &validity = FlatVector::Validity(vec_vector);

	if (dimension_ == 0 && !logical_types.empty()) {
		D_ASSERT(logical_types[0].id() == LogicalTypeId::ARRAY);
		dimension_ = ArrayType::GetSize(logical_types[0]);
	}

	for (idx_t i = 0; i < count; i++) {
		if (!validity.RowIsValid(i)) {
			continue;
		}

		row_t row_id = row_id_data[i];
		const float *vec = vec_data + i * dimension_;
		int level = GetRandomLevel();

		auto node = make_uniq<GraphNode>(row_id, vec, dimension_, static_cast<uint8_t>(level), m_);
		GraphNode *node_ptr = node.get();

		graph_.nodes.push_back(std::move(node));
		graph_.node_count++;

		graph_.InsertNode(node_ptr, m_, ef_construction_, distance_func_, dimension_);
	}
}

// ============================================================
// BoundIndex Interface Implementation
// ============================================================

ErrorData GraphIndex::Append(IndexLock &l, DataChunk &chunk, Vector &row_ids) {
	// During CREATE INDEX (Sink), key_chunk has only indexed columns
	// During INSERT (AppendToIndexes), table_chunk has ALL table columns
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

void GraphIndex::VerifyAppend(DataChunk &chunk, IndexAppendInfo &info,
                               optional_ptr<ConflictManager> manager) {
}

void GraphIndex::VerifyConstraint(DataChunk &chunk, IndexAppendInfo &info, ConflictManager &manager) {
}

void GraphIndex::Delete(IndexLock &state, DataChunk &entries, Vector &row_identifiers) {
	auto row_id_data = FlatVector::GetData<row_t>(row_identifiers);
	auto count = entries.size();

	// Phase 1: Mark nodes as deleted and collect their pointers
	unordered_set<GraphNode *> newly_deleted;
	for (idx_t i = 0; i < count; i++) {
		row_t row_id = row_id_data[i];
		for (auto &node : graph_.nodes) {
			if (node && node->row_id == row_id) {
				node->deleted = true;
				newly_deleted.insert(node.get());
				break;
			}
		}
	}

	if (newly_deleted.empty()) {
		return;
	}

	// Phase 2: Update entry_point if it was deleted (do this before neighbor cleanup)
	if (graph_.entry_point && newly_deleted.count(graph_.entry_point) > 0) {
		graph_.entry_point = nullptr;
		graph_.max_level = 0;
		for (auto &node : graph_.nodes) {
			if (node && !node->deleted && static_cast<int>(node->level) > graph_.max_level) {
				graph_.entry_point = node.get();
				graph_.max_level = node->level;
			}
		}
	}

	// Phase 3: Remove deleted nodes from neighbor lists and repair connections
	// Modeled after VexDB's RepairGraph: don't just remove — reconnect affected nodes
	std::vector<GraphNode *> needs_repair;
	for (auto &node : graph_.nodes) {
		if (!node || node->deleted) continue;
		bool affected = false;
		for (auto &level_neighbors : node->neighbors) {
			auto old_size = level_neighbors.size();
			level_neighbors.erase(
			    std::remove_if(level_neighbors.begin(), level_neighbors.end(),
			                   [&](GraphNode *n) { return !n || newly_deleted.count(n) > 0; }),
			    level_neighbors.end());
			if (level_neighbors.size() < old_size) {
				affected = true;
			}
		}
		if (affected) {
			needs_repair.push_back(node.get());
		}
	}

	// Phase 4: Repair affected nodes by finding replacement neighbors
	// This maintains graph connectivity (inspired by VexDB RepairGraphElement)
	// Only repair if entry_point is valid and we have a meaningful graph
	if (graph_.entry_point && !needs_repair.empty() && graph_.node_count > 1) {
		for (auto *node : needs_repair) {
			if (node->deleted) continue;
			for (int level = 0; level <= static_cast<int>(node->level); level++) {
				int layer_m = GraphIndexConfig::GetLayerM(m_, level);
				if (static_cast<int>(node->neighbors[level].size()) >= layer_m) {
					continue; // Already has enough neighbors
				}

				// Verify entry_point is still valid before searching
				if (!graph_.entry_point || graph_.entry_point->deleted) break;

				// Search for new candidate neighbors at this level
				unordered_set<row_t> visited;
				std::vector<GraphCandidate> candidates;
				graph_.SearchLayer(node->vector, graph_.entry_point, layer_m * 2, level,
				                   candidates, visited, distance_func_, dimension_);

				// Filter out self, existing neighbors, and deleted nodes
				unordered_set<GraphNode *> existing(node->neighbors[level].begin(),
				                                     node->neighbors[level].end());
				existing.insert(node);

				std::vector<GraphCandidate> filtered;
				for (auto &c : candidates) {
					if (c.node && !c.node->deleted && existing.count(c.node) == 0) {
						filtered.push_back(c);
					}
				}

				auto new_neighbors = graph_.SelectNeighbors(filtered, layer_m);
				for (auto *nn : new_neighbors) {
					if (!nn || nn->deleted) continue;
					if (static_cast<int>(node->neighbors[level].size()) >= layer_m) break;
					node->neighbors[level].push_back(nn);
					// Add reverse connection only if level exists
					if (level <= static_cast<int>(nn->level) &&
					    static_cast<int>(nn->neighbors[level].size()) < layer_m) {
						nn->neighbors[level].push_back(node);
					}
				}
			}
		}
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
	}

	for (auto &node_ptr : other.graph_.nodes) {
		if (!node_ptr) continue;

		auto new_node = make_uniq<GraphNode>(
			node_ptr->row_id, node_ptr->vector, dimension_, node_ptr->level, m_
		);

		graph_.InsertNode(new_node.get(), m_, ef_construction_, distance_func_, dimension_);
		graph_.nodes.push_back(std::move(new_node));
		graph_.node_count++;
	}

	return true;
}

void GraphIndex::Vacuum(IndexLock &l) {
	graph_.Vacuum();
}

// ============================================================
// Serialization
// ============================================================

static constexpr uint32_t GRAPH_INDEX_MAGIC = 0x58444947; // "GIDX"
static constexpr uint32_t GRAPH_INDEX_VERSION = 2; // v2: added PQ support

string GraphIndex::SerializeToBlob() const {
	// Collect only live (non-deleted) nodes for serialization
	std::vector<GraphNode *> live_nodes;
	for (auto &node : graph_.nodes) {
		if (node && !node->deleted) {
			live_nodes.push_back(node.get());
		}
	}

	if (live_nodes.empty()) {
		return string();
	}

	// Build index map for live nodes only
	unordered_map<const GraphNode *, uint64_t> node_to_idx;
	for (uint64_t i = 0; i < live_nodes.size(); i++) {
		node_to_idx[live_nodes[i]] = i;
	}

	// Pre-count valid (non-null, non-deleted) neighbors per level for size calculation
	std::vector<std::vector<uint32_t>> valid_neighbor_counts(live_nodes.size());
	for (uint64_t i = 0; i < live_nodes.size(); i++) {
		auto *node = live_nodes[i];
		valid_neighbor_counts[i].resize(node->level + 1);
		for (uint8_t l = 0; l <= node->level; l++) {
			uint32_t count = 0;
			for (auto *neighbor : node->neighbors[l]) {
				if (neighbor && !neighbor->deleted && node_to_idx.count(neighbor) > 0) {
					count++;
				}
			}
			valid_neighbor_counts[i][l] = count;
		}
	}

	size_t total_size = 0;
	total_size += 4 + 4 + 4 + 4 + 4 + 8 + 4 + 8; // header
	for (auto *node : live_nodes) {
		total_size += 8 + 1 + 1; // row_id + level + deleted flag
		total_size += dimension_ * sizeof(float);
	}
	for (uint64_t i = 0; i < live_nodes.size(); i++) {
		for (uint8_t l = 0; l <= live_nodes[i]->level; l++) {
			total_size += 4; // neighbor count
			total_size += valid_neighbor_counts[i][l] * 8;
		}
	}

	string blob;
	blob.resize(total_size);
	char *ptr = &blob[0];

	auto write_u32 = [&](uint32_t v) { memcpy(ptr, &v, 4); ptr += 4; };
	auto write_u64 = [&](uint64_t v) { memcpy(ptr, &v, 8); ptr += 8; };
	auto write_i32 = [&](int32_t v) { memcpy(ptr, &v, 4); ptr += 4; };
	auto write_i64 = [&](int64_t v) { memcpy(ptr, &v, 8); ptr += 8; };
	auto write_u8 = [&](uint8_t v) { *ptr = static_cast<char>(v); ptr += 1; };

	write_u32(GRAPH_INDEX_MAGIC);
	write_u32(GRAPH_INDEX_VERSION);
	write_u32(static_cast<uint32_t>(m_));
	write_u32(static_cast<uint32_t>(ef_construction_));
	write_u32(dimension_);
	write_u64(live_nodes.size());

	// Recompute max_level from live nodes
	int32_t live_max_level = 0;
	for (auto *node : live_nodes) {
		if (static_cast<int32_t>(node->level) > live_max_level) {
			live_max_level = static_cast<int32_t>(node->level);
		}
	}
	write_i32(live_max_level);

	int64_t ep_idx = -1;
	if (graph_.entry_point && !graph_.entry_point->deleted) {
		auto it = node_to_idx.find(graph_.entry_point);
		if (it != node_to_idx.end()) {
			ep_idx = static_cast<int64_t>(it->second);
		}
	}
	write_i64(ep_idx);

	for (auto *node : live_nodes) {
		write_i64(static_cast<int64_t>(node->row_id));
		write_u8(node->level);
		write_u8(0); // always alive — deleted nodes are excluded
		memcpy(ptr, node->vector, dimension_ * sizeof(float));
		ptr += dimension_ * sizeof(float);
	}

	for (uint64_t i = 0; i < live_nodes.size(); i++) {
		auto *node = live_nodes[i];
		for (uint8_t l = 0; l <= node->level; l++) {
			write_u32(valid_neighbor_counts[i][l]);
			for (auto *neighbor : node->neighbors[l]) {
				if (!neighbor || neighbor->deleted) continue;
				auto it = node_to_idx.find(neighbor);
				if (it == node_to_idx.end()) continue;
				write_u64(it->second);
			}
		}
	}

	// v2: Append PQ data
	std::vector<char> pq_blob;
	if (use_pq_ && graph_.pq.trained) {
		pq_blob.push_back(1); // has_pq flag
		graph_.pq.SerializeTo(pq_blob);
		// Append PQ codes
		uint64_t codes_size = graph_.pq_codes.size();
		size_t pos = pq_blob.size();
		pq_blob.resize(pos + 8 + codes_size);
		memcpy(pq_blob.data() + pos, &codes_size, 8);
		memcpy(pq_blob.data() + pos + 8, graph_.pq_codes.data(), codes_size);
	} else {
		pq_blob.push_back(0); // no PQ
	}
	blob.append(pq_blob.data(), pq_blob.size());

	return blob;
}

bool GraphIndex::DeserializeFromBlob(const string &blob) {
	if (blob.empty()) {
		return false;
	}

	const char *ptr = blob.data();
	const char *end = ptr + blob.size();

	auto read_u32 = [&]() -> uint32_t { uint32_t v; memcpy(&v, ptr, 4); ptr += 4; return v; };
	auto read_u64 = [&]() -> uint64_t { uint64_t v; memcpy(&v, ptr, 8); ptr += 8; return v; };
	auto read_i32 = [&]() -> int32_t { int32_t v; memcpy(&v, ptr, 4); ptr += 4; return v; };
	auto read_i64 = [&]() -> int64_t { int64_t v; memcpy(&v, ptr, 8); ptr += 8; return v; };
	auto read_u8 = [&]() -> uint8_t { uint8_t v = static_cast<uint8_t>(*ptr); ptr += 1; return v; };

	if (static_cast<size_t>(end - ptr) < 40) {
		return false;
	}
	uint32_t magic = read_u32();
	if (magic != GRAPH_INDEX_MAGIC) {
		return false;
	}
	uint32_t version = read_u32();
	if (version != 1 && version != 2) {
		return false;
	}

	m_ = static_cast<int>(read_u32());
	ef_construction_ = static_cast<int>(read_u32());
	dimension_ = read_u32();
	uint64_t node_count = read_u64();
	int32_t max_level = read_i32();
	int64_t ep_idx = read_i64();

	graph_.max_level = max_level;

	uint32_t saved_dimension = dimension_;
	Clear();
	dimension_ = saved_dimension;
	graph_.nodes.reserve(node_count);
	for (uint64_t i = 0; i < node_count; i++) {
		if (static_cast<size_t>(end - ptr) < 10 + dimension_ * sizeof(float)) {
			return false;
		}
		row_t row_id = static_cast<row_t>(read_i64());
		uint8_t level = read_u8();
		bool deleted = read_u8() != 0;

		const float *vec_data = reinterpret_cast<const float *>(ptr);
		ptr += dimension_ * sizeof(float);

		auto node = make_uniq<GraphNode>(row_id, vec_data, dimension_, level, m_);
		node->deleted = deleted;
		graph_.nodes.push_back(std::move(node));
	}

	for (uint64_t i = 0; i < node_count; i++) {
		auto &node = graph_.nodes[i];
		for (uint8_t l = 0; l <= node->level; l++) {
			uint32_t neighbor_count = read_u32();
			node->neighbors[l].clear();
			node->neighbors[l].reserve(neighbor_count);
			for (uint32_t n = 0; n < neighbor_count; n++) {
				uint64_t neighbor_idx = read_u64();
				if (neighbor_idx < graph_.nodes.size()) {
					node->neighbors[l].push_back(graph_.nodes[neighbor_idx].get());
				}
			}
		}
	}

	if (ep_idx >= 0 && static_cast<uint64_t>(ep_idx) < graph_.nodes.size()) {
		graph_.entry_point = graph_.nodes[ep_idx].get();
	} else {
		graph_.entry_point = nullptr;
	}

	graph_.node_count = graph_.nodes.size();

	// v2: Read PQ data if present
	if (version >= 2 && ptr < end) {
		uint8_t has_pq = static_cast<uint8_t>(*ptr); ptr += 1;
		if (has_pq) {
			use_pq_ = true;
			if (!graph_.pq.DeserializeFrom(ptr, end)) {
				return false;
			}
			if (ptr + 8 > end) return false;
			uint64_t codes_size;
			memcpy(&codes_size, ptr, 8); ptr += 8;
			if (ptr + codes_size > end) return false;
			graph_.pq_codes.resize(codes_size);
			memcpy(graph_.pq_codes.data(), ptr, codes_size);
			ptr += codes_size;
		}
	}

	return true;
}

IndexStorageInfo GraphIndex::SerializeToDisk(QueryContext context,
                                              const case_insensitive_map_t<Value> &options) {
	IndexStorageInfo info;
	info.name = name;
	string blob = SerializeToBlob();
	if (!blob.empty()) {
		info.options["graph_data"] = Value::BLOB(const_data_ptr_cast(blob.data()), blob.size());
	}
	return info;
}

IndexStorageInfo GraphIndex::SerializeToWAL(const case_insensitive_map_t<Value> &options) {
	IndexStorageInfo info;
	info.name = name;
	string blob = SerializeToBlob();
	if (!blob.empty()) {
		info.options["graph_data"] = Value::BLOB(const_data_ptr_cast(blob.data()), blob.size());
	}
	return info;
}

idx_t GraphIndex::GetInMemorySize(IndexLock &state) {
	idx_t size = sizeof(GraphIndex);
	size += graph_.nodes.capacity() * sizeof(GraphNode *);
	for (const auto &node : graph_.nodes) {
		size += sizeof(GraphNode);
		for (const auto &level_neighbors : node->neighbors) {
			size += level_neighbors.capacity() * sizeof(GraphNode *);
		}
	}
	return size;
}

string GraphIndex::VerifyAndToString(IndexLock &l, const bool only_verify) {
	if (only_verify) {
		if (graph_.node_count != graph_.nodes.size()) {
			return "Node count mismatch";
		}
		return "OK";
	}
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
// ANN Search (delegates to GraphIndexCore)
// ============================================================

void GraphIndex::Search(const float *query_vec, idx_t k, int ef,
                         std::vector<row_t> &out_row_ids, std::vector<float> &out_distances) {
	if (use_pq_) {
		// Lazy PQ training: train on first search if not yet trained
		if (!graph_.pq.trained && graph_.node_count > 0) {
			graph_.TrainPQ(pq_m_);
		}
		graph_.SearchWithPQ(query_vec, k, ef, out_row_ids, out_distances, distance_func_, dimension_, m_);
	} else {
		graph_.Search(query_vec, k, ef, out_row_ids, out_distances, distance_func_, dimension_, m_);
	}
}

// ============================================================
// Index Scan Interface
// ============================================================

unique_ptr<IndexScanState> GraphIndex::TryInitializeScan(const Expression &expr, const Expression &filter_expr) {
	return nullptr;
}

bool GraphIndex::Scan(IndexScanState &state, idx_t max_count, set<row_t> &row_ids) {
	auto &scan_state = state.Cast<GraphIndexScanState>();

	if (!scan_state.initialized) {
		return false;
	}

	if (scan_state.current_offset == 0 && !scan_state.query_vector.empty()) {
		Search(scan_state.query_vector.data(), scan_state.k, scan_state.ef,
		       scan_state.row_ids, scan_state.distances);
	}

	row_ids.clear();
	idx_t remaining = std::min(max_count, scan_state.row_ids.size() - scan_state.current_offset);
	for (idx_t i = 0; i < remaining; i++) {
		row_ids.insert(scan_state.row_ids[scan_state.current_offset + i]);
	}
	scan_state.current_offset += remaining;

	return scan_state.current_offset >= scan_state.row_ids.size();
}

void GraphIndex::ANNSearch(const float *query_vec, idx_t k, int ef,
                           std::vector<row_t> &out_row_ids, std::vector<float> &out_distances) {
	Search(query_vec, k, ef, out_row_ids, out_distances);
}

void GraphIndex::Clear() {
	graph_.Clear();
	dimension_ = 0;
}

} // namespace duckdb
