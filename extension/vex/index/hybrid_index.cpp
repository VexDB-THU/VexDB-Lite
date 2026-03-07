#include "vex_hybrid_index.hpp"
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
// HybridIndex Factory Methods
// ============================================================

unique_ptr<BoundIndex> HybridIndex::Create(CreateIndexInput &input) {
	// Validate: first column must be ARRAY(FLOAT) (FLOATVECTOR)
	if (!input.unbound_expressions.empty()) {
		auto &vec_type = input.unbound_expressions[0]->return_type;
		if (vec_type.id() != LogicalTypeId::ARRAY || ArrayType::GetChildType(vec_type).id() != LogicalTypeId::FLOAT) {
			throw InvalidInputException("HYBRID_INDEX first column must be FLOATVECTOR (ARRAY(FLOAT)), got %s",
			                            vec_type.ToString());
		}
	}

	int m = GraphIndexConfig::DEFAULT_M;
	int ef_construction = GraphIndexConfig::DEFAULT_EF_CONSTRUCTION;

	auto m_it = input.options.find("m");
	if (m_it != input.options.end()) {
		m = m_it->second.GetValue<int>();
	}
	auto ef_it = input.options.find("ef_construction");
	if (ef_it != input.options.end()) {
		ef_construction = ef_it->second.GetValue<int>();
	}

	auto hybrid_index = make_uniq<HybridIndex>(
		input.name,
		input.constraint_type,
		input.column_ids,
		input.table_io_manager,
		input.unbound_expressions,
		input.db,
		m,
		ef_construction
	);

	auto data_it = input.storage_info.options.find("hybrid_data");
	if (data_it != input.storage_info.options.end()) {
		auto blob = data_it->second.GetValueUnsafe<string>();
		hybrid_index->DeserializeFromBlob(blob);
	}

	return std::move(hybrid_index);
}

PhysicalOperator &HybridIndex::CreatePlan(PlanIndexInput &input) {
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

HybridIndex::HybridIndex(const string &name, IndexConstraintType constraint_type,
                         const vector<column_t> &column_ids, TableIOManager &table_io_manager,
                         const vector<unique_ptr<Expression>> &unbound_expressions,
                         AttachedDatabase &db, int m, int ef_construction)
    : BoundIndex(name, HybridIndex::TYPE_NAME, constraint_type, column_ids, table_io_manager,
                 unbound_expressions, db),
      m_(m),
      ef_construction_(ef_construction),
      dimension_(0),
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

	if (column_ids.size() < 2) {
		throw InvalidInputException("HYBRID_INDEX requires at least 2 columns (vector_column, scalar_column)");
	}
}

// ============================================================
// Helpers
// ============================================================

string HybridIndex::ValueToPartitionKey(const Value &val) {
	if (val.IsNull()) {
		return "__NULL__";
	}
	return val.ToString();
}

GraphIndexCore &HybridIndex::GetOrCreatePartition(const string &key) {
	auto it = partitions_.find(key);
	if (it != partitions_.end()) {
		return it->second;
	}
	return partitions_[key];
}

int HybridIndex::GetRandomLevel() {
	double ml = GraphIndexConfig::GetMl(m_);
	int level = static_cast<int>(-std::log(dist_(rng_)) / ml);
	return std::min(level, GraphIndexConfig::GetMaxLevel(m_));
}

// ============================================================
// Build / Append
// ============================================================

void HybridIndex::Build(DataChunk &chunk, Vector &row_ids) {
	idx_t count = chunk.size();
	if (count == 0) return;

	auto &vec_vector = chunk.data[0];
	auto &key_vector = chunk.data[1];
	auto row_id_data = FlatVector::GetData<row_t>(row_ids);

	auto &vec_type = vec_vector.GetType();
	if (vec_type.id() != LogicalTypeId::ARRAY) {
		throw InvalidInputException("HYBRID_INDEX: first column must be an ARRAY type");
	}
	auto array_size = ArrayType::GetSize(vec_type);

	if (dimension_ == 0) {
		dimension_ = array_size;
	} else if (dimension_ != static_cast<uint32_t>(array_size)) {
		throw InvalidInputException("HYBRID_INDEX: vector dimension mismatch: expected %d, got %d",
		                            dimension_, array_size);
	}

	auto &array_child = ArrayVector::GetEntry(vec_vector);
	auto vec_data = FlatVector::GetData<float>(array_child);

	for (idx_t i = 0; i < count; i++) {
		auto key_val = key_vector.GetValue(i);
		string partition_key = ValueToPartitionKey(key_val);

		const float *vector_ptr = vec_data + (i * dimension_);
		int level = GetRandomLevel();
		auto node = make_uniq<GraphNode>(row_id_data[i], vector_ptr, dimension_, level, m_);
		auto *node_ptr = node.get();

		auto &partition = GetOrCreatePartition(partition_key);
		partition.InsertNode(node_ptr, m_, ef_construction_, distance_func_, dimension_);
		partition.node_count++;
		partition.nodes.push_back(std::move(node));

		// Track row -> partition for O(1) delete
		row_partition_map_[row_id_data[i]] = partition_key;
	}
}

ErrorData HybridIndex::Append(IndexLock &l, DataChunk &chunk, Vector &row_ids) {
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

ErrorData HybridIndex::Append(IndexLock &l, DataChunk &chunk, Vector &row_ids, IndexAppendInfo &info) {
	return Append(l, chunk, row_ids);
}

ErrorData HybridIndex::Insert(IndexLock &l, DataChunk &chunk, Vector &row_ids) {
	return Append(l, chunk, row_ids);
}

ErrorData HybridIndex::Insert(IndexLock &l, DataChunk &chunk, Vector &row_ids, IndexAppendInfo &info) {
	return Insert(l, chunk, row_ids);
}

// ============================================================
// Delete (O(1) partition lookup via row_partition_map_)
// ============================================================

void HybridIndex::Delete(IndexLock &state, DataChunk &entries, Vector &row_identifiers) {
	auto row_id_data = FlatVector::GetData<row_t>(row_identifiers);
	idx_t count = entries.size();

	// Group deletions by partition
	std::unordered_map<string, unordered_set<GraphNode *>> partition_deleted;

	for (idx_t i = 0; i < count; i++) {
		row_t row_id = row_id_data[i];

		auto map_it = row_partition_map_.find(row_id);
		if (map_it == row_partition_map_.end()) {
			continue;
		}

		auto &partition_key = map_it->second;
		auto part_it = partitions_.find(partition_key);
		if (part_it != partitions_.end()) {
			for (auto &node : part_it->second.nodes) {
				if (node && node->row_id == row_id) {
					node->deleted = true;
					partition_deleted[partition_key].insert(node.get());
					break;
				}
			}
		}

		row_partition_map_.erase(map_it);
	}

	// Clean neighbor lists in affected partitions
	for (auto &[pkey, deleted_set] : partition_deleted) {
		auto part_it = partitions_.find(pkey);
		if (part_it == partitions_.end()) continue;

		for (auto &node : part_it->second.nodes) {
			if (!node || node->deleted) continue;
			for (auto &level_neighbors : node->neighbors) {
				level_neighbors.erase(
				    std::remove_if(level_neighbors.begin(), level_neighbors.end(),
				                   [&](GraphNode *n) { return !n || deleted_set.count(n) > 0; }),
				    level_neighbors.end());
			}
		}

		// Update entry_point if deleted
		if (part_it->second.entry_point && deleted_set.count(part_it->second.entry_point) > 0) {
			part_it->second.entry_point = nullptr;
			part_it->second.max_level = 0;
			for (auto &node : part_it->second.nodes) {
				if (node && !node->deleted && static_cast<int>(node->level) > part_it->second.max_level) {
					part_it->second.entry_point = node.get();
					part_it->second.max_level = node->level;
				}
			}
		}
	}
}

// ============================================================
// Search
// ============================================================

void HybridIndex::FilteredSearch(const string &partition_key, const float *query_vec, idx_t k, int ef,
                                 std::vector<row_t> &out_row_ids, std::vector<float> &out_distances) {
	auto it = partitions_.find(partition_key);
	if (it == partitions_.end()) {
		return;
	}

	it->second.Search(query_vec, k, ef, out_row_ids, out_distances, distance_func_, dimension_, m_);
}

void HybridIndex::GlobalSearch(const float *query_vec, idx_t k, int ef,
                               std::vector<row_t> &out_row_ids, std::vector<float> &out_distances) {
	std::vector<std::pair<row_t, float>> all_results;

	for (auto &kv : partitions_) {
		std::vector<row_t> ids;
		std::vector<float> dists;
		kv.second.Search(query_vec, k, ef, ids, dists, distance_func_, dimension_, m_);
		for (idx_t i = 0; i < ids.size(); i++) {
			all_results.push_back(std::make_pair(ids[i], dists[i]));
		}
	}

	std::sort(all_results.begin(), all_results.end(),
	          [](const std::pair<row_t, float> &a, const std::pair<row_t, float> &b) { return a.second < b.second; });

	idx_t count = std::min(k, static_cast<idx_t>(all_results.size()));
	for (idx_t i = 0; i < count; i++) {
		out_row_ids.push_back(all_results[i].first);
		out_distances.push_back(all_results[i].second);
	}
}

std::vector<string> HybridIndex::GetPartitionKeys() const {
	std::vector<string> keys;
	for (auto &kv : partitions_) {
		keys.push_back(kv.first);
	}
	return keys;
}

idx_t HybridIndex::GetTotalNodeCount() const {
	idx_t total = 0;
	for (auto &kv : partitions_) {
		total += kv.second.node_count;
	}
	return total;
}

// ============================================================
// Merge
// ============================================================

bool HybridIndex::MergeIndexes(IndexLock &state, BoundIndex &other_index) {
	auto &other = other_index.Cast<HybridIndex>();

	if (dimension_ == 0) {
		dimension_ = other.dimension_;
	}

	for (auto &kv : other.partitions_) {
		auto &my_partition = GetOrCreatePartition(kv.first);
		for (auto &node : kv.second.nodes) {
			auto *node_ptr = node.get();
			for (auto &neighbors : node_ptr->neighbors) {
				neighbors.clear();
			}
			my_partition.InsertNode(node_ptr, m_, ef_construction_, distance_func_, dimension_);
			my_partition.node_count++;

			// Update reverse map
			row_partition_map_[node_ptr->row_id] = kv.first;

			my_partition.nodes.push_back(std::move(node));
		}
	}
	other.partitions_.clear();
	other.row_partition_map_.clear();
	return true;
}

// ============================================================
// Vacuum
// ============================================================

void HybridIndex::Vacuum(IndexLock &l) {
	for (auto it = partitions_.begin(); it != partitions_.end();) {
		auto &partition = it->second;
		partition.Vacuum();

		if (partition.node_count == 0) {
			it = partitions_.erase(it);
		} else {
			++it;
		}
	}

	// Clean up row_partition_map_ for deleted rows
	for (auto map_it = row_partition_map_.begin(); map_it != row_partition_map_.end();) {
		auto part_it = partitions_.find(map_it->second);
		if (part_it == partitions_.end()) {
			map_it = row_partition_map_.erase(map_it);
		} else {
			++map_it;
		}
	}
}

// ============================================================
// Serialization
// ============================================================

string HybridIndex::SerializeToBlob() const {
	string blob;
	blob.append("HIDX", 4);

	uint32_t num_partitions = static_cast<uint32_t>(partitions_.size());
	blob.append(reinterpret_cast<const char *>(&m_), sizeof(m_));
	blob.append(reinterpret_cast<const char *>(&ef_construction_), sizeof(ef_construction_));
	blob.append(reinterpret_cast<const char *>(&dimension_), sizeof(dimension_));
	blob.append(reinterpret_cast<const char *>(&num_partitions), sizeof(num_partitions));

	for (auto &kv : partitions_) {
		auto &key = kv.first;
		auto &partition = kv.second;

		// Collect only live (non-deleted) nodes
		std::vector<GraphNode *> live_nodes;
		for (auto &node : partition.nodes) {
			if (node && !node->deleted) {
				live_nodes.push_back(node.get());
			}
		}

		uint32_t key_len = static_cast<uint32_t>(key.size());
		blob.append(reinterpret_cast<const char *>(&key_len), sizeof(key_len));
		blob.append(key.data(), key_len);

		uint32_t pnode_count = static_cast<uint32_t>(live_nodes.size());
		int pmax_level = 0;
		for (auto *node : live_nodes) {
			if (static_cast<int>(node->level) > pmax_level) {
				pmax_level = static_cast<int>(node->level);
			}
		}
		blob.append(reinterpret_cast<const char *>(&pnode_count), sizeof(pnode_count));
		blob.append(reinterpret_cast<const char *>(&pmax_level), sizeof(pmax_level));

		// Build index map for live nodes
		std::unordered_map<GraphNode *, uint32_t> node_index_map;
		for (uint32_t i = 0; i < pnode_count; i++) {
			node_index_map[live_nodes[i]] = i;
		}

		int32_t ep_idx = -1;
		if (partition.entry_point && !partition.entry_point->deleted) {
			auto nit = node_index_map.find(partition.entry_point);
			if (nit != node_index_map.end()) {
				ep_idx = static_cast<int32_t>(nit->second);
			}
		}
		blob.append(reinterpret_cast<const char *>(&ep_idx), sizeof(ep_idx));

		for (auto *node : live_nodes) {
			blob.append(reinterpret_cast<const char *>(&node->row_id), sizeof(node->row_id));
			blob.append(reinterpret_cast<const char *>(&node->level), sizeof(node->level));
			blob.append(reinterpret_cast<const char *>(node->vector), sizeof(float) * dimension_);

			for (uint8_t l = 0; l <= node->level; l++) {
				// Count valid neighbors first
				uint16_t valid_count = 0;
				for (auto *neighbor : node->neighbors[l]) {
					if (neighbor && !neighbor->deleted && node_index_map.count(neighbor) > 0) {
						valid_count++;
					}
				}
				blob.append(reinterpret_cast<const char *>(&valid_count), sizeof(valid_count));
				for (auto *neighbor : node->neighbors[l]) {
					if (!neighbor || neighbor->deleted) continue;
					auto nit = node_index_map.find(neighbor);
					if (nit == node_index_map.end()) continue;
					uint32_t nidx = nit->second;
					blob.append(reinterpret_cast<const char *>(&nidx), sizeof(nidx));
				}
			}
		}
	}

	return blob;
}

bool HybridIndex::DeserializeFromBlob(const string &blob) {
	if (blob.size() < 4) return false;
	if (blob.substr(0, 4) != "HIDX") return false;

	Clear();

	const char *ptr = blob.data() + 4;
	const char *end = blob.data() + blob.size();

#define READ_VAL(val) do { \
	if (ptr + sizeof(val) > end) return false; \
	std::memcpy(&(val), ptr, sizeof(val)); \
	ptr += sizeof(val); \
} while(0)

	uint32_t num_partitions;
	READ_VAL(m_); READ_VAL(ef_construction_); READ_VAL(dimension_); READ_VAL(num_partitions);

	for (uint32_t p = 0; p < num_partitions; p++) {
		uint32_t key_len;
		READ_VAL(key_len);
		if (ptr + key_len > end) return false;
		string key(ptr, key_len);
		ptr += key_len;

		uint32_t pnode_count;
		int pmax_level;
		int32_t ep_idx;
		READ_VAL(pnode_count); READ_VAL(pmax_level); READ_VAL(ep_idx);

		auto &partition = partitions_[key];
		partition.max_level = pmax_level;
		partition.node_count = pnode_count;

		for (uint32_t i = 0; i < pnode_count; i++) {
			row_t row_id;
			uint8_t level;
			READ_VAL(row_id); READ_VAL(level);

			if (ptr + sizeof(float) * dimension_ > end) return false;
			const float *vec_data = reinterpret_cast<const float *>(ptr);
			ptr += sizeof(float) * dimension_;

			auto node = make_uniq<GraphNode>(row_id, vec_data, dimension_, level, m_);

			for (uint8_t l = 0; l <= level; l++) {
				uint16_t neighbor_count;
				READ_VAL(neighbor_count);
				node->neighbors[l].clear();
				for (uint16_t n = 0; n < neighbor_count; n++) {
					uint32_t nidx;
					READ_VAL(nidx);
					if (nidx < pnode_count) {
						node->neighbors[l].push_back(reinterpret_cast<GraphNode *>(static_cast<uintptr_t>(nidx)));
					}
				}
			}

			// Rebuild reverse map
			row_partition_map_[row_id] = key;

			partition.nodes.push_back(std::move(node));
		}

		// Fix up neighbor pointers
		for (auto &node : partition.nodes) {
			for (auto &neighbors : node->neighbors) {
				for (auto &neighbor_ptr : neighbors) {
					uint32_t idx = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(neighbor_ptr));
					if (idx < pnode_count) {
						neighbor_ptr = partition.nodes[idx].get();
					} else {
						neighbor_ptr = nullptr;
					}
				}
				neighbors.erase(
				    std::remove(neighbors.begin(), neighbors.end(), nullptr),
				    neighbors.end());
			}
		}

		if (ep_idx >= 0 && static_cast<uint32_t>(ep_idx) < pnode_count) {
			partition.entry_point = partition.nodes[ep_idx].get();
		}
	}

#undef READ_VAL

	return true;
}

void HybridIndex::Clear() {
	partitions_.clear();
	row_partition_map_.clear();
	dimension_ = 0;
}

IndexStorageInfo HybridIndex::SerializeToDisk(QueryContext context, const case_insensitive_map_t<Value> &options) {
	IndexStorageInfo info;
	info.name = name;
	string blob_data = SerializeToBlob();
	if (!blob_data.empty()) {
		info.options["hybrid_data"] = Value::BLOB(const_data_ptr_cast(blob_data.data()), blob_data.size());
	}
	return info;
}

IndexStorageInfo HybridIndex::SerializeToWAL(const case_insensitive_map_t<Value> &options) {
	IndexStorageInfo info;
	info.name = name;
	string blob_data = SerializeToBlob();
	if (!blob_data.empty()) {
		info.options["hybrid_data"] = Value::BLOB(const_data_ptr_cast(blob_data.data()), blob_data.size());
	}
	return info;
}

// ============================================================
// Stubs
// ============================================================

void HybridIndex::CommitDrop(IndexLock &index_lock) {
	Clear();
}

void HybridIndex::VerifyAppend(DataChunk &chunk, IndexAppendInfo &info, optional_ptr<ConflictManager> manager) {
}

void HybridIndex::VerifyConstraint(DataChunk &chunk, IndexAppendInfo &info, ConflictManager &manager) {
}

idx_t HybridIndex::GetInMemorySize(IndexLock &state) {
	idx_t size = 0;
	for (auto &kv : partitions_) {
		for (auto &node : kv.second.nodes) {
			size += sizeof(GraphNode);
			size += sizeof(float) * dimension_;
			for (auto &neighbors : node->neighbors) {
				size += sizeof(GraphNode *) * neighbors.capacity();
			}
		}
	}
	return size;
}

string HybridIndex::VerifyAndToString(IndexLock &l, const bool only_verify) {
	string result = "HybridIndex: " + std::to_string(partitions_.size()) + " partitions, ";
	result += std::to_string(GetTotalNodeCount()) + " total nodes, dim=" + std::to_string(dimension_);
	return result;
}

void HybridIndex::VerifyAllocations(IndexLock &l) {}
void HybridIndex::VerifyBuffers(IndexLock &l) {}

string HybridIndex::GetConstraintViolationMessage(VerifyExistenceType verify_type, idx_t failed_index,
                                                   DataChunk &input) {
	return "Constraint violation in HYBRID_INDEX";
}

} // namespace duckdb
