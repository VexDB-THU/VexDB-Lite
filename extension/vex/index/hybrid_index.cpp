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
#include "duckdb/storage/table_io_manager.hpp"
#include "duckdb/storage/partial_block_manager.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace duckdb {

// ============================================================
// HybridIndex Factory Methods
// ============================================================

unique_ptr<BoundIndex> HybridIndex::Create(CreateIndexInput &input) {
	// Validate: first column must be ARRAY(FLOAT)
	if (!input.unbound_expressions.empty()) {
		auto &vec_type = input.unbound_expressions[0]->return_type;
		if (vec_type.id() != LogicalTypeId::ARRAY || ArrayType::GetChildType(vec_type).id() != LogicalTypeId::FLOAT) {
			throw InvalidInputException("HYBRID_INDEX first column must be FLOAT[N] (ARRAY(FLOAT)), got %s",
			                            vec_type.ToString());
		}
	}

	int m = GraphIndexConfig::DEFAULT_M;
	int ef_construction = GraphIndexConfig::DEFAULT_EF_CONSTRUCTION;

	auto m_it = input.options.find("m");
	if (m_it != input.options.end()) {
		try {
			m = m_it->second.DefaultCastAs(LogicalType::INTEGER).GetValue<int>();
		} catch (const std::exception &) {
			throw InvalidInputException("HYBRID_INDEX: 'm' must be a valid integer, got '%s'", m_it->second.ToString());
		}
		if (m < 2 || m > 128) {
			throw InvalidInputException("HYBRID_INDEX: 'm' must be between 2 and 128, got %d", m);
		}
	}
	auto ef_it = input.options.find("ef_construction");
	if (ef_it != input.options.end()) {
		try {
			ef_construction = ef_it->second.DefaultCastAs(LogicalType::INTEGER).GetValue<int>();
		} catch (const std::exception &) {
			throw InvalidInputException("HYBRID_INDEX: 'ef_construction' must be a valid integer, got '%s'",
			                            ef_it->second.ToString());
		}
		if (ef_construction < 1 || ef_construction > 10000) {
			throw InvalidInputException("HYBRID_INDEX: 'ef_construction' must be between 1 and 10000, got %d", ef_construction);
		}
	}
	vex::VexMetric metric = vex::VexMetric::L2;
	auto metric_it = input.options.find("metric");
	if (metric_it != input.options.end()) {
		metric = vex::ParseMetric(metric_it->second.GetValue<string>());
	}

	uint16_t max_dedup = GraphIndexCore::DEFAULT_MAX_DEDUP;
	auto dedup_it = input.options.find("max_dedup");
	if (dedup_it != input.options.end()) {
		int val = dedup_it->second.GetValue<int>();
		if (val < 1 || val > 65535) {
			throw InvalidInputException("max_dedup must be between 1 and 65535, got %d", val);
		}
		max_dedup = static_cast<uint16_t>(val);
	}

	auto hybrid_index = make_uniq<HybridIndex>(
		input.name,
		input.constraint_type,
		input.column_ids,
		input.table_io_manager,
		input.unbound_expressions,
		input.db,
		m,
		ef_construction,
		metric,
		max_dedup
	);

	// Deserialize from allocator-based storage if available
	if (input.storage_info.IsValid() && input.storage_info.options.count("dimension")) {
		hybrid_index->DeserializeFromStorage(input.storage_info);
	} else {
		// Legacy BLOB deserialization (backward compatibility)
		auto data_it = input.storage_info.options.find("hybrid_data");
		if (data_it != input.storage_info.options.end()) {
			auto blob = data_it->second.GetValueUnsafe<string>();
			if (!hybrid_index->DeserializeFromBlob(blob)) {
				throw IOException("HYBRID_INDEX: failed to deserialize from legacy BLOB — data may be corrupted");
			}
		}
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
                         AttachedDatabase &db, int m, int ef_construction,
                         vex::VexMetric metric, uint16_t max_dedup)
    : BoundIndex(name, HybridIndex::TYPE_NAME, constraint_type, column_ids, table_io_manager,
                 unbound_expressions, db),
      m_(m),
      ef_construction_(ef_construction),
      dimension_(0),
      max_dedup_(max_dedup),
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

void HybridIndex::EnsurePartitionAllocators(GraphIndexCore &partition) {
	if (!partition.node_alloc) {
		auto &block_manager = table_io_manager.GetIndexBlockManager();
		partition.dimension = dimension_;
		partition.m = m_;
		partition.InitAllocators(block_manager);
	}
}

GraphIndexCore &HybridIndex::GetOrCreatePartition(const string &key) {
	auto it = partitions_.find(key);
	if (it != partitions_.end()) {
		return it->second;
	}
	auto &partition = partitions_[key];
	partition.max_dedup = max_dedup_;
	if (dimension_ > 0) {
		EnsurePartitionAllocators(partition);
	}
	return partition;
}

int HybridIndex::GetRandomLevel() {
	std::lock_guard<std::mutex> lock(rng_mutex_);
	double ml = GraphIndexConfig::GetMl(m_);
	double r = dist_(rng_);
	if (r == 0.0) r = std::numeric_limits<double>::min();
	int level = static_cast<int>(-std::log(r) / ml);
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
	row_ids.Flatten(count);
	vec_vector.Flatten(count);
	key_vector.Flatten(count);
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

	std::vector<float> norm_buf;
	if (metric_ == vex::VexMetric::COSINE) {
		norm_buf.resize(dimension_);
	}

	auto &vec_validity = FlatVector::Validity(vec_vector);

	for (idx_t i = 0; i < count; i++) {
		// Skip NULL vectors
		if (!vec_validity.RowIsValid(i)) continue;

		auto key_val = key_vector.GetValue(i);
		string partition_key = ValueToPartitionKey(key_val);

		const float *vector_ptr = vec_data + (i * dimension_);

		// For cosine metric, normalize vectors before storing in index
		if (metric_ == vex::VexMetric::COSINE) {
			std::memcpy(norm_buf.data(), vector_ptr, dimension_ * sizeof(float));
			vex::NormalizeVector(norm_buf.data(), dimension_);
			vector_ptr = norm_buf.data();
		}

		auto &partition = GetOrCreatePartition(partition_key);
		EnsurePartitionAllocators(partition);

		// Try deduplication first
		if (partition.max_dedup > 1 && partition.TryDedup(row_id_data[i], vector_ptr, dimension_, distance_func_)) {
			row_partition_map_[row_id_data[i]] = partition_key;
			continue; // Merged into existing node
		}

		int level = GetRandomLevel();
		IndexPointer new_node = partition.AllocateNode(row_id_data[i], vector_ptr, dimension_, static_cast<uint8_t>(level));
		partition.InsertNode(new_node, m_, ef_construction_, distance_func_);

		// Track row -> partition for O(1) delete
		row_partition_map_[row_id_data[i]] = partition_key;
	}
}

ErrorData HybridIndex::Append(IndexLock &l, DataChunk &chunk, Vector &row_ids) {
	// CREATE INDEX: chunk has only indexed columns already in index order.
	// INSERT: chunk has ALL table columns in table order — need to remap via column_ids.
	// When column counts match, check if first column type matches index expectation.
	bool needs_remap = (chunk.ColumnCount() != logical_types.size()) ||
	                   (chunk.data[0].GetType().id() != logical_types[0].id());
	if (needs_remap) {
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
	idx_t count = entries.size();
	row_identifiers.Flatten(count);
	auto row_id_data = FlatVector::GetData<row_t>(row_identifiers);

	// Group deletions by partition
	std::unordered_map<string, std::vector<row_t>> partition_deletes;

	for (idx_t i = 0; i < count; i++) {
		row_t row_id = row_id_data[i];

		auto map_it = row_partition_map_.find(row_id);
		if (map_it == row_partition_map_.end()) {
			continue;
		}

		partition_deletes[map_it->second].push_back(row_id);
		row_partition_map_.erase(map_it);
	}

	// Process deletions per partition
	for (auto &pd_kv : partition_deletes) {
		auto &pkey = pd_kv.first;
		auto &row_ids_to_delete = pd_kv.second;
		auto part_it = partitions_.find(pkey);
		if (part_it == partitions_.end()) continue;
		auto &partition = part_it->second;

		// Classify each row_id as primary or extra (dedup)
		unordered_set<row_t> deleted_row_ids; // row_ids of fully deleted graph nodes
		std::vector<IndexPointer> nodes_to_free;
		for (auto row_id : row_ids_to_delete) {
			auto node_it = partition.row_id_map.find(row_id);
			if (node_it == partition.row_id_map.end()) continue;

			IndexPointer node_ptr = node_it->second;
			auto *header = partition.GetNode(node_ptr);

			if (header->row_id == row_id) {
				// Primary row_id
				if (header->extra_row_count > 0) {
					// Promote an extra to primary
					idx_t node_key = node_ptr.Get();
					auto dit = partition.dedup_map_.find(node_key);
					if (dit != partition.dedup_map_.end() && !dit->second.empty()) {
						row_t new_primary = dit->second.back();
						dit->second.pop_back();
						header->extra_row_count--;
						header->row_id = new_primary;
						partition.row_id_map.erase(row_id);
						if (dit->second.empty()) {
							partition.dedup_map_.erase(dit);
						}
						continue; // Node stays alive
					}
				}
				// No extras: mark for full deletion
				header->deleted = 1;
				deleted_row_ids.insert(row_id);
				nodes_to_free.push_back(node_ptr);
			} else {
				// Extra (dedup) row_id — remove from dedup_map
				idx_t node_key = node_ptr.Get();
				auto dit = partition.dedup_map_.find(node_key);
				if (dit != partition.dedup_map_.end()) {
					auto &extras = dit->second;
					extras.erase(std::remove(extras.begin(), extras.end(), row_id), extras.end());
					header->extra_row_count = static_cast<uint16_t>(extras.size());
					if (extras.empty()) {
						partition.dedup_map_.erase(dit);
					}
				}
				partition.row_id_map.erase(row_id);
			}
		}

		if (nodes_to_free.size() <= partition.node_count) {
			partition.node_count -= nodes_to_free.size();
		} else {
			partition.node_count = 0;
		}

		// Phase 2: Update entry_point if deleted
		auto *ep_header = partition.entry_point.Get() ? partition.GetNode(partition.entry_point) : nullptr;
		if (ep_header && ep_header->deleted) {
			partition.entry_point.Clear();
			partition.max_level = 0;
			for (auto &rm_kv : partition.row_id_map) {
				auto nptr = rm_kv.second;
				auto *h = partition.GetNode(nptr);
				if (!h->deleted && static_cast<int>(h->level) > partition.max_level) {
					partition.entry_point = nptr;
					partition.max_level = h->level;
				}
			}
		}

		// Phase 3: Clean neighbor lists
		for (auto &rm_kv : partition.row_id_map) {
			auto nptr = rm_kv.second;
			auto *header = partition.GetNode(nptr);
			if (header->deleted) continue;

			for (int level = 0; level <= static_cast<int>(header->level); level++) {
				IndexPointer *neighbors;
				uint16_t nb_count;
				partition.GetNeighbors(nptr, level, neighbors, nb_count);
				if (!neighbors) continue;

				uint16_t write_idx = 0;
				for (uint16_t j = 0; j < nb_count; j++) {
					if (!neighbors[j].Get()) continue;
					auto *nb_h = partition.GetNode(neighbors[j]);
					if (nb_h->deleted || deleted_row_ids.count(nb_h->row_id) > 0) {
						continue;
					}
					neighbors[write_idx++] = neighbors[j];
				}
				// Clear remaining slots
				for (uint16_t j = write_idx; j < nb_count; j++) {
					neighbors[j].Clear();
				}
				if (write_idx != nb_count) {
					partition.SetNeighborCount(nptr, level, write_idx);
				}
			}
		}

		// Free deleted nodes and clean up
		for (auto &del_ptr : nodes_to_free) {
			auto *header = partition.GetNode(del_ptr);
			idx_t node_key = del_ptr.Get();
			partition.dedup_map_.erase(node_key);
			partition.row_id_map.erase(header->row_id);
			partition.FreeNode(del_ptr);
		}
	}
}

// ============================================================
// Search
// ============================================================

void HybridIndex::FilteredSearch(const string &partition_key, const float *query_vec, idx_t k, int ef,
                                 std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
                                 idx_t brute_force_threshold) {
	auto it = partitions_.find(partition_key);
	if (it == partitions_.end()) {
		return;
	}

	// For cosine metric, normalize query vector
	std::vector<float> norm_query;
	const float *search_vec = query_vec;
	if (metric_ == vex::VexMetric::COSINE && dimension_ > 0) {
		norm_query.assign(query_vec, query_vec + dimension_);
		vex::NormalizeVector(norm_query.data(), dimension_);
		search_vec = norm_query.data();
	}

	it->second.Search(search_vec, k, ef, out_row_ids, out_distances, distance_func_, brute_force_threshold);
}

void HybridIndex::GlobalSearch(const float *query_vec, idx_t k, int ef,
                               std::vector<row_t> &out_row_ids, std::vector<float> &out_distances,
                               idx_t brute_force_threshold) {
	// For cosine metric, normalize query vector
	std::vector<float> norm_query;
	const float *search_vec = query_vec;
	if (metric_ == vex::VexMetric::COSINE && dimension_ > 0) {
		norm_query.assign(query_vec, query_vec + dimension_);
		vex::NormalizeVector(norm_query.data(), dimension_);
		search_vec = norm_query.data();
	}

	std::vector<std::pair<row_t, float>> all_results;

	for (auto &kv : partitions_) {
		std::vector<row_t> ids;
		std::vector<float> dists;
		kv.second.Search(search_vec, k, ef, ids, dists, distance_func_, brute_force_threshold);
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
		metric_ = other.metric_;
		distance_func_ = vex::GetDistanceFunc(metric_);
	}

	for (auto &kv : other.partitions_) {
		auto &my_partition = GetOrCreatePartition(kv.first);
		EnsurePartitionAllocators(my_partition);

		auto &other_partition = kv.second;

		unordered_set<idx_t> seen_nodes;

		for (auto &rm_kv : other_partition.row_id_map) {
			auto nptr = rm_kv.second;
			if (!seen_nodes.insert(nptr.Get()).second) continue;

			auto *header = other_partition.GetNode(nptr);
			if (header->deleted) continue;

			auto *vec = other_partition.GetVector(header->vector_ptr);

			std::vector<row_t> all_row_ids;
			other_partition.CollectNodeRowIds(nptr, all_row_ids);

			row_t primary_rid = all_row_ids[0];

			// Insert primary: try dedup first, fall back to full insert
			if (!(my_partition.max_dedup > 1 && my_partition.TryDedup(primary_rid, vec, dimension_, distance_func_))) {
				IndexPointer new_node = my_partition.AllocateNode(primary_rid, vec, dimension_, header->level);
				my_partition.InsertNode(new_node, m_, ef_construction_, distance_func_);
			}
			row_partition_map_[primary_rid] = kv.first;

			// Insert extras: try dedup, fall back to new node
			for (idx_t r = 1; r < all_row_ids.size(); r++) {
				if (!my_partition.TryDedup(all_row_ids[r], vec, dimension_, distance_func_)) {
					IndexPointer extra_node = my_partition.AllocateNode(all_row_ids[r], vec, dimension_, 0);
					my_partition.InsertNode(extra_node, m_, ef_construction_, distance_func_);
				}
				row_partition_map_[all_row_ids[r]] = kv.first;
			}
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
		if (it->second.node_count == 0) {
			it = partitions_.erase(it);
		} else {
			++it;
		}
	}

	// Clean up row_partition_map_ for deleted partitions
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
// Serialization (still using BLOB for HybridIndex)
// ============================================================

string HybridIndex::SerializeToBlob() {
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

		// Collect only live (non-deleted) nodes and build index map
		std::vector<std::pair<row_t, IndexPointer>> live_nodes;
		std::unordered_map<row_t, uint32_t> node_index_map;
		for (auto &rm_kv : partition.row_id_map) {
			auto rid = rm_kv.first;
			auto nptr = rm_kv.second;
			auto *header = partition.GetNode(nptr);
			if (!header->deleted) {
				uint32_t idx = static_cast<uint32_t>(live_nodes.size());
				node_index_map[rid] = idx;
				live_nodes.push_back(std::make_pair(rid, nptr));
			}
		}

		uint32_t key_len = static_cast<uint32_t>(key.size());
		blob.append(reinterpret_cast<const char *>(&key_len), sizeof(key_len));
		blob.append(key.data(), key_len);

		uint32_t pnode_count = static_cast<uint32_t>(live_nodes.size());
		int pmax_level = 0;
		for (auto &ln_kv : live_nodes) {
			auto *h = partition.GetNode(ln_kv.second);
			if (static_cast<int>(h->level) > pmax_level) {
				pmax_level = static_cast<int>(h->level);
			}
		}
		blob.append(reinterpret_cast<const char *>(&pnode_count), sizeof(pnode_count));
		blob.append(reinterpret_cast<const char *>(&pmax_level), sizeof(pmax_level));

		int32_t ep_idx = -1;
		if (partition.entry_point.Get()) {
			auto *ep_h = partition.GetNode(partition.entry_point);
			if (!ep_h->deleted) {
				auto nit = node_index_map.find(ep_h->row_id);
				if (nit != node_index_map.end()) {
					ep_idx = static_cast<int32_t>(nit->second);
				}
			}
		}
		blob.append(reinterpret_cast<const char *>(&ep_idx), sizeof(ep_idx));

		for (auto &ln_kv : live_nodes) {
			auto nptr = ln_kv.second;
			auto *header = partition.GetNode(nptr);
			auto *vec = partition.GetVector(header->vector_ptr);

			blob.append(reinterpret_cast<const char *>(&header->row_id), sizeof(header->row_id));
			blob.append(reinterpret_cast<const char *>(&header->level), sizeof(header->level));
			blob.append(reinterpret_cast<const char *>(vec), sizeof(float) * dimension_);

			for (uint8_t l = 0; l <= header->level; l++) {
				IndexPointer *neighbors;
				uint16_t nb_count;
				partition.GetNeighbors(nptr, l, neighbors, nb_count);

				// Count valid neighbors first
				uint16_t valid_count = 0;
				if (neighbors) {
					for (uint16_t j = 0; j < nb_count; j++) {
						if (!neighbors[j].Get()) continue;
						auto *nb_h = partition.GetNode(neighbors[j]);
						if (!nb_h->deleted && node_index_map.count(nb_h->row_id) > 0) {
							valid_count++;
						}
					}
				}
				blob.append(reinterpret_cast<const char *>(&valid_count), sizeof(valid_count));

				if (neighbors) {
					for (uint16_t j = 0; j < nb_count; j++) {
						if (!neighbors[j].Get()) continue;
						auto *nb_h = partition.GetNode(neighbors[j]);
						if (nb_h->deleted) continue;
						auto nit = node_index_map.find(nb_h->row_id);
						if (nit == node_index_map.end()) continue;
						uint32_t nidx = nit->second;
						blob.append(reinterpret_cast<const char *>(&nidx), sizeof(nidx));
					}
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

		auto &partition = GetOrCreatePartition(key);
		EnsurePartitionAllocators(partition);
		partition.max_level = pmax_level;

		// First pass: allocate all nodes and store temporary neighbor indices
		struct TempNeighborInfo {
			std::vector<std::vector<uint32_t>> level_neighbors;
		};
		std::vector<TempNeighborInfo> temp_info(pnode_count);
		std::vector<IndexPointer> node_ptrs(pnode_count);

		for (uint32_t i = 0; i < pnode_count; i++) {
			row_t row_id;
			uint8_t level;
			READ_VAL(row_id); READ_VAL(level);

			if (ptr + sizeof(float) * dimension_ > end) return false;
			const float *vec_data = reinterpret_cast<const float *>(ptr);
			ptr += sizeof(float) * dimension_;

			node_ptrs[i] = partition.AllocateNode(row_id, vec_data, dimension_, level);

			temp_info[i].level_neighbors.resize(level + 1);
			for (uint8_t l = 0; l <= level; l++) {
				uint16_t neighbor_count;
				READ_VAL(neighbor_count);
				for (uint16_t n = 0; n < neighbor_count; n++) {
					uint32_t nidx;
					READ_VAL(nidx);
					if (nidx < pnode_count) {
						temp_info[i].level_neighbors[l].push_back(nidx);
					}
				}
			}

			// Rebuild reverse map
			row_partition_map_[row_id] = key;
		}

		// Second pass: fix up neighbor pointers
		for (uint32_t i = 0; i < pnode_count; i++) {
			auto *header = partition.GetNode(node_ptrs[i]);
			for (uint8_t l = 0; l <= header->level; l++) {
				IndexPointer *neighbors;
				uint16_t dummy;
				partition.GetNeighbors(node_ptrs[i], l, neighbors, dummy);
				if (!neighbors) continue;

				auto &indices = temp_info[i].level_neighbors[l];
				uint16_t count = static_cast<uint16_t>(indices.size());
				for (uint16_t j = 0; j < count; j++) {
					neighbors[j] = node_ptrs[indices[j]];
				}
				partition.SetNeighborCount(node_ptrs[i], l, count);
			}
		}

		if (ep_idx >= 0 && static_cast<uint32_t>(ep_idx) < pnode_count) {
			partition.entry_point = node_ptrs[ep_idx];
		}
	}

#undef READ_VAL

	return true;
}

// ============================================================
// Deserialization from allocator-based storage
// ============================================================

void HybridIndex::DeserializeFromStorage(const IndexStorageInfo &info) {
	// Read metadata
	auto m_it = info.options.find("m");
	if (m_it != info.options.end()) {
		m_ = m_it->second.GetValue<int>();
	}
	auto ef_it = info.options.find("ef_construction");
	if (ef_it != info.options.end()) {
		ef_construction_ = ef_it->second.GetValue<int>();
	}
	auto dim_it = info.options.find("dimension");
	if (dim_it != info.options.end()) {
		dimension_ = dim_it->second.GetValue<uint32_t>();
	}
	auto metric_opt = info.options.find("metric");
	if (metric_opt != info.options.end()) {
		metric_ = static_cast<vex::VexMetric>(metric_opt->second.GetValue<uint8_t>());
		distance_func_ = vex::GetDistanceFunc(metric_);
	}

	auto np_it = info.options.find("num_partitions");
	if (np_it == info.options.end()) return;
	uint32_t num_partitions = np_it->second.GetValue<uint32_t>();
	if (num_partitions == 0) return;

	// Parse partition metadata blob
	auto meta_it = info.options.find("partition_meta");
	if (meta_it == info.options.end()) return;
	auto meta_blob = meta_it->second.GetValueUnsafe<string>();
	const char *ptr = meta_blob.data();
	const char *end = ptr + meta_blob.size();

	struct PartitionMeta {
		string key;
		uint64_t node_count;
		int32_t max_level;
		uint64_t entry_point;
		std::vector<std::pair<row_t, uint64_t>> row_entries; // row_id -> IndexPointer raw
		uint16_t max_dedup = GraphIndexCore::DEFAULT_MAX_DEDUP;
		unordered_map<idx_t, std::vector<row_t>> dedup_entries;
	};
	std::vector<PartitionMeta> partition_metas;
	partition_metas.reserve(num_partitions);

	for (uint32_t p = 0; p < num_partitions; p++) {
		if (ptr + sizeof(uint32_t) > end) return;
		uint32_t key_len;
		memcpy(&key_len, ptr, sizeof(key_len));
		ptr += sizeof(key_len);
		if (ptr + key_len > end) return;
		string key(ptr, key_len);
		ptr += key_len;

		if (ptr + sizeof(uint64_t) + sizeof(int32_t) + sizeof(uint64_t) > end) return;
		PartitionMeta pm;
		pm.key = std::move(key);
		memcpy(&pm.node_count, ptr, sizeof(pm.node_count));
		ptr += sizeof(pm.node_count);
		memcpy(&pm.max_level, ptr, sizeof(pm.max_level));
		ptr += sizeof(pm.max_level);
		memcpy(&pm.entry_point, ptr, sizeof(pm.entry_point));
		ptr += sizeof(pm.entry_point);

		// Read row_id_map entries
		if (ptr + sizeof(uint64_t) > end) return;
		uint64_t num_entries;
		memcpy(&num_entries, ptr, sizeof(num_entries));
		ptr += sizeof(num_entries);
		pm.row_entries.reserve(static_cast<size_t>(num_entries));
		for (uint64_t i = 0; i < num_entries; i++) {
			if (ptr + sizeof(row_t) + sizeof(uint64_t) > end) return;
			row_t rid;
			uint64_t ptr_val;
			memcpy(&rid, ptr, sizeof(rid));
			ptr += sizeof(rid);
			memcpy(&ptr_val, ptr, sizeof(ptr_val));
			ptr += sizeof(ptr_val);
			pm.row_entries.push_back(std::make_pair(rid, ptr_val));
		}

		// Read max_dedup and dedup_map_ (added in dedup persistence fix)
		if (ptr + sizeof(uint16_t) <= end) {
			memcpy(&pm.max_dedup, ptr, sizeof(pm.max_dedup));
			ptr += sizeof(pm.max_dedup);
		}
		if (ptr + sizeof(uint64_t) <= end) {
			uint64_t num_dedup_nodes;
			memcpy(&num_dedup_nodes, ptr, sizeof(num_dedup_nodes));
			ptr += sizeof(num_dedup_nodes);
			for (uint64_t d = 0; d < num_dedup_nodes && ptr + 2 * sizeof(uint64_t) <= end; d++) {
				uint64_t node_key, num_extras;
				memcpy(&node_key, ptr, sizeof(node_key));
				ptr += sizeof(node_key);
				memcpy(&num_extras, ptr, sizeof(num_extras));
				ptr += sizeof(num_extras);
				std::vector<row_t> extras;
				extras.reserve(static_cast<size_t>(num_extras));
				for (uint64_t j = 0; j < num_extras && ptr + sizeof(row_t) <= end; j++) {
					row_t extra_rid;
					memcpy(&extra_rid, ptr, sizeof(extra_rid));
					ptr += sizeof(extra_rid);
					extras.push_back(extra_rid);
				}
				if (!extras.empty()) {
					pm.dedup_entries[static_cast<idx_t>(node_key)] = std::move(extras);
				}
			}
		}

		partition_metas.push_back(std::move(pm));
	}

	// Verify we have enough allocator_infos (3 per partition)
	if (info.allocator_infos.size() < num_partitions * 3) return;

	// Recreate partitions and initialize allocators from storage
	auto &block_manager = table_io_manager.GetIndexBlockManager();
	for (uint32_t p = 0; p < num_partitions; p++) {
		auto &pm = partition_metas[p];
		auto &partition = partitions_[pm.key];
		partition.dimension = dimension_;
		partition.m = m_;
		partition.node_count = pm.node_count;
		partition.max_level = pm.max_level;
		partition.entry_point.Set(pm.entry_point);

		// Create allocators WITHOUT slot-0 reservation.
		// The serialized bitmask already has slot 0 reserved from the original InitAllocators().
		// Calling InitAllocators() here would reserve slot 0 again, creating a stale entry
		// in buffers_with_free_space that can cause "Invalid bitmask for FixedSizeAllocator"
		// when a subsequent New() tries to allocate from a full on-disk buffer.
		partition.node_alloc = make_uniq<FixedSizeAllocator>(vex::HNSWNodeHeader::SegmentSize(m_), block_manager);
		partition.vector_alloc = make_uniq<FixedSizeAllocator>(static_cast<idx_t>(dimension_) * sizeof(float), block_manager);
		partition.upper_alloc = make_uniq<FixedSizeAllocator>(vex::HNSWUpperLevel::SegmentSize(m_), block_manager);

		// Load allocator data from storage info
		idx_t base_idx = static_cast<idx_t>(p) * 3;
		partition.node_alloc->Init(info.allocator_infos[base_idx]);
		partition.vector_alloc->Init(info.allocator_infos[base_idx + 1]);
		partition.upper_alloc->Init(info.allocator_infos[base_idx + 2]);

		// Restore row_id_map from serialized entries
		partition.row_id_map_built = true;
		for (auto &entry : pm.row_entries) {
			IndexPointer nptr;
			nptr.Set(entry.second);
			partition.row_id_map[entry.first] = nptr;
			row_partition_map_[entry.first] = pm.key;
		}

		// Restore dedup state
		partition.max_dedup = pm.max_dedup;
		partition.dedup_map_ = std::move(pm.dedup_entries);
		for (auto &dd_kv : partition.dedup_map_) {
			for (auto &extra_rid : dd_kv.second) {
				row_partition_map_[extra_rid] = pm.key;
			}
		}
	}
}

void HybridIndex::Clear() {
	for (auto &kv : partitions_) {
		kv.second.Clear();
	}
	partitions_.clear();
	row_partition_map_.clear();
	dimension_ = 0;
}

static void SerializePartitionDedupMap(const GraphIndexCore &partition, string &blob) {
	uint16_t p_max_dedup = partition.max_dedup;
	blob.append(reinterpret_cast<const char *>(&p_max_dedup), sizeof(p_max_dedup));
	uint64_t num_dedup_nodes = partition.dedup_map_.size();
	blob.append(reinterpret_cast<const char *>(&num_dedup_nodes), sizeof(num_dedup_nodes));
	for (auto &dd_kv : partition.dedup_map_) {
		uint64_t node_key = dd_kv.first;
		uint64_t num_extras = dd_kv.second.size();
		blob.append(reinterpret_cast<const char *>(&node_key), sizeof(node_key));
		blob.append(reinterpret_cast<const char *>(&num_extras), sizeof(num_extras));
		for (auto &rid : dd_kv.second) {
			blob.append(reinterpret_cast<const char *>(&rid), sizeof(rid));
		}
	}
}

IndexStorageInfo HybridIndex::SerializeToDisk(QueryContext context, const case_insensitive_map_t<Value> &options) {
	IndexStorageInfo info;
	info.name = name;

	if (partitions_.empty() || dimension_ == 0) {
		return info;
	}

	// Store index metadata
	info.options["m"] = Value::INTEGER(m_);
	info.options["ef_construction"] = Value::INTEGER(ef_construction_);
	info.options["dimension"] = Value::UINTEGER(dimension_);
	info.options["metric"] = Value::UTINYINT(static_cast<uint8_t>(metric_));

	// Build partition metadata blob containing:
	// For each partition: key_len, key, node_count, max_level, entry_point,
	//   num_row_entries, [row_id, IndexPointer]*
	uint32_t num_partitions = 0;
	string partition_meta_blob;
	for (auto &kv : partitions_) {
		if (kv.second.node_count == 0) continue;
		num_partitions++;

		uint32_t key_len = static_cast<uint32_t>(kv.first.size());
		partition_meta_blob.append(reinterpret_cast<const char *>(&key_len), sizeof(key_len));
		partition_meta_blob.append(kv.first.data(), key_len);

		uint64_t nc = kv.second.node_count;
		int32_t ml = kv.second.max_level;
		uint64_t ep = kv.second.entry_point.Get();
		partition_meta_blob.append(reinterpret_cast<const char *>(&nc), sizeof(nc));
		partition_meta_blob.append(reinterpret_cast<const char *>(&ml), sizeof(ml));
		partition_meta_blob.append(reinterpret_cast<const char *>(&ep), sizeof(ep));

		// Serialize partition's row_id_map (row_id -> IndexPointer)
		uint64_t num_entries = kv.second.row_id_map.size();
		partition_meta_blob.append(reinterpret_cast<const char *>(&num_entries), sizeof(num_entries));
		for (auto &rm_kv : kv.second.row_id_map) {
			row_t rid = rm_kv.first;
			uint64_t ptr_val = rm_kv.second.Get();
			partition_meta_blob.append(reinterpret_cast<const char *>(&rid), sizeof(rid));
			partition_meta_blob.append(reinterpret_cast<const char *>(&ptr_val), sizeof(ptr_val));
		}

		SerializePartitionDedupMap(kv.second, partition_meta_blob);
	}
	info.options["num_partitions"] = Value::UINTEGER(num_partitions);
	if (!partition_meta_blob.empty()) {
		info.options["partition_meta"] = Value::BLOB(const_data_ptr_cast(partition_meta_blob.data()),
		                                             partition_meta_blob.size());
	}

	// Serialize each partition's 3 allocators to disk
	auto &block_manager = table_io_manager.GetIndexBlockManager();
	PartialBlockManager partial_block_manager(context, block_manager, PartialBlockType::FULL_CHECKPOINT);

	for (auto &kv : partitions_) {
		if (kv.second.node_count == 0) continue;
		auto &partition = kv.second;
		partition.node_alloc->SerializeBuffers(partial_block_manager);
		partition.vector_alloc->SerializeBuffers(partial_block_manager);
		partition.upper_alloc->SerializeBuffers(partial_block_manager);
	}
	partial_block_manager.FlushPartialBlocks();

	// Collect allocator infos (3 per partition, in partition order)
	for (auto &kv : partitions_) {
		if (kv.second.node_count == 0) continue;
		auto &partition = kv.second;
		info.allocator_infos.push_back(partition.node_alloc->GetInfo());
		info.allocator_infos.push_back(partition.vector_alloc->GetInfo());
		info.allocator_infos.push_back(partition.upper_alloc->GetInfo());
	}

	return info;
}

IndexStorageInfo HybridIndex::SerializeToWAL(const case_insensitive_map_t<Value> &options) {
	IndexStorageInfo info;
	info.name = name;

	if (partitions_.empty() || dimension_ == 0) {
		return info;
	}

	// Store index metadata (same as SerializeToDisk)
	info.options["m"] = Value::INTEGER(m_);
	info.options["ef_construction"] = Value::INTEGER(ef_construction_);
	info.options["dimension"] = Value::UINTEGER(dimension_);
	info.options["metric"] = Value::UTINYINT(static_cast<uint8_t>(metric_));

	// Build partition metadata blob (same format as SerializeToDisk)
	uint32_t num_partitions = 0;
	string partition_meta_blob;
	for (auto &kv : partitions_) {
		if (kv.second.node_count == 0) continue;
		num_partitions++;

		uint32_t key_len = static_cast<uint32_t>(kv.first.size());
		partition_meta_blob.append(reinterpret_cast<const char *>(&key_len), sizeof(key_len));
		partition_meta_blob.append(kv.first.data(), key_len);

		uint64_t nc = kv.second.node_count;
		int32_t ml = kv.second.max_level;
		uint64_t ep = kv.second.entry_point.Get();
		partition_meta_blob.append(reinterpret_cast<const char *>(&nc), sizeof(nc));
		partition_meta_blob.append(reinterpret_cast<const char *>(&ml), sizeof(ml));
		partition_meta_blob.append(reinterpret_cast<const char *>(&ep), sizeof(ep));

		// Serialize partition's row_id_map
		uint64_t num_entries = kv.second.row_id_map.size();
		partition_meta_blob.append(reinterpret_cast<const char *>(&num_entries), sizeof(num_entries));
		for (auto &rm_kv : kv.second.row_id_map) {
			row_t rid = rm_kv.first;
			uint64_t ptr_val = rm_kv.second.Get();
			partition_meta_blob.append(reinterpret_cast<const char *>(&rid), sizeof(rid));
			partition_meta_blob.append(reinterpret_cast<const char *>(&ptr_val), sizeof(ptr_val));
		}

		SerializePartitionDedupMap(kv.second, partition_meta_blob);
	}
	info.options["num_partitions"] = Value::UINTEGER(num_partitions);
	if (!partition_meta_blob.empty()) {
		info.options["partition_meta"] = Value::BLOB(const_data_ptr_cast(partition_meta_blob.data()),
		                                             partition_meta_blob.size());
	}

	// WAL serialization: include buffer data
	for (auto &kv : partitions_) {
		if (kv.second.node_count == 0) continue;
		auto &partition = kv.second;
		info.buffers.push_back(partition.node_alloc->InitSerializationToWAL());
		info.buffers.push_back(partition.vector_alloc->InitSerializationToWAL());
		info.buffers.push_back(partition.upper_alloc->InitSerializationToWAL());
	}

	// Collect allocator infos
	for (auto &kv : partitions_) {
		if (kv.second.node_count == 0) continue;
		auto &partition = kv.second;
		info.allocator_infos.push_back(partition.node_alloc->GetInfo());
		info.allocator_infos.push_back(partition.vector_alloc->GetInfo());
		info.allocator_infos.push_back(partition.upper_alloc->GetInfo());
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
		auto &partition = kv.second;
		if (partition.node_alloc) size += partition.node_alloc->GetInMemorySize();
		if (partition.vector_alloc) size += partition.vector_alloc->GetInMemorySize();
		if (partition.upper_alloc) size += partition.upper_alloc->GetInMemorySize();
		static constexpr idx_t HASH_ENTRY_OVERHEAD = 32; // unordered_map per-entry overhead
		size += partition.row_id_map.size() * (sizeof(row_t) + sizeof(IndexPointer) + HASH_ENTRY_OVERHEAD);
	}
	static constexpr idx_t HASH_ENTRY_OVERHEAD = 32;
	static constexpr idx_t AVG_PARTITION_KEY_SIZE = 16;
	size += row_partition_map_.size() * (sizeof(row_t) + AVG_PARTITION_KEY_SIZE + HASH_ENTRY_OVERHEAD);
	return size;
}

void HybridIndex::Verify(IndexLock &l) {
}

string HybridIndex::ToString(IndexLock &l, bool display_ascii) {
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

constexpr const char *HybridIndex::TYPE_NAME;

} // namespace duckdb
