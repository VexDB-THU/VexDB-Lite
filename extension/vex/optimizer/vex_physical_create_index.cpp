#include "vex_physical_create_index.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/execution/index/index_type_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table_io_manager.hpp"
#include "vex_graph_index.hpp"

#include <mutex>

namespace duckdb {

PhysicalVexCreateIndex::PhysicalVexCreateIndex(PhysicalPlan &physical_plan, LogicalOperator &op,
                                               TableCatalogEntry &table_p, const vector<column_t> &column_ids,
                                               unique_ptr<CreateIndexInfo> info_p,
                                               vector<unique_ptr<Expression>> unbound_expressions_p,
                                               idx_t estimated_cardinality,
                                               unique_ptr<AlterTableInfo> alter_table_info_p)
    : PhysicalOperator(physical_plan, TYPE, op.types, estimated_cardinality), table(table_p.Cast<DuckTableEntry>()),
      info(std::move(info_p)), unbound_expressions(std::move(unbound_expressions_p)),
      alter_table_info(std::move(alter_table_info_p)) {
	for (auto &column_id : column_ids) {
		storage_ids.push_back(table.GetColumns().LogicalToPhysical(LogicalIndex(column_id)).index);
	}
}

class VexCreateIndexGlobalSinkState : public GlobalSinkState {
public:
	unique_ptr<BoundIndex> global_index;
	bool is_graph_index = false;
	std::mutex buffer_mutex;
	std::vector<float> all_vectors;
	std::vector<row_t> all_row_ids;
	idx_t total_count = 0;
	uint32_t dimension = 0;
	std::vector<uint8_t> all_metadata;
	uint32_t meta_segment_size = 0;
};

class VexCreateIndexLocalSinkState : public LocalSinkState {
public:
	explicit VexCreateIndexLocalSinkState(ClientContext &context) {
	}

	unique_ptr<BoundIndex> local_index;
	DataChunk key_chunk;
	vector<column_t> key_column_ids;
};

unique_ptr<GlobalSinkState> PhysicalVexCreateIndex::GetGlobalSinkState(ClientContext &context) const {
	auto state = make_uniq<VexCreateIndexGlobalSinkState>();
	auto &config = DBConfig::GetConfig(context);
	auto &index_types = config.GetIndexTypes();
	auto index_type_ref = index_types.FindByName(info->index_type);
	if (!index_type_ref) {
		throw InternalException("Index type '%s' not found in registry", info->index_type);
	}

	IndexStorageInfo storage_info;
	CreateIndexInput index_input(context, TableIOManager::Get(table.GetStorage()), table.GetStorage().db,
	                             info->constraint_type, info->index_name, storage_ids, unbound_expressions,
	                             storage_info, info->options);

	state->global_index = index_type_ref->create_instance(index_input);
	state->is_graph_index = (info->index_type == GraphIndex::TYPE_NAME);
	return std::move(state);
}

unique_ptr<LocalSinkState> PhysicalVexCreateIndex::GetLocalSinkState(ExecutionContext &context) const {
	auto state = make_uniq<VexCreateIndexLocalSinkState>(context.client);

	vector<LogicalType> key_types;
	for (auto &expr : unbound_expressions) {
		key_types.push_back(expr->return_type);
	}
	state->key_chunk.Initialize(Allocator::Get(context.client), key_types);
	for (idx_t i = 0; i < state->key_chunk.ColumnCount(); i++) {
		state->key_column_ids.push_back(i);
	}

	if (info->index_type != GraphIndex::TYPE_NAME) {
		auto &config = DBConfig::GetConfig(context.client);
		auto &index_types = config.GetIndexTypes();
		auto index_type_ref = index_types.FindByName(info->index_type);
		if (!index_type_ref) {
			throw InternalException("Index type '%s' not found in registry", info->index_type);
		}
		IndexStorageInfo storage_info;
		CreateIndexInput index_input(context.client, TableIOManager::Get(table.GetStorage()), table.GetStorage().db,
		                             info->constraint_type, info->index_name, storage_ids, unbound_expressions,
		                             storage_info, info->options);
		state->local_index = index_type_ref->create_instance(index_input);
	}

	return std::move(state);
}

SinkResultType PhysicalVexCreateIndex::Sink(ExecutionContext &context, DataChunk &chunk,
                                            OperatorSinkInput &input) const {
	D_ASSERT(chunk.ColumnCount() >= 2);
	auto &g_state = input.global_state.Cast<VexCreateIndexGlobalSinkState>();
	auto &l_state = input.local_state.Cast<VexCreateIndexLocalSinkState>();
	l_state.key_chunk.ReferenceColumns(chunk, l_state.key_column_ids);

	if (alter_table_info) {
		auto row_count = l_state.key_chunk.size();
		for (idx_t i = 0; i < l_state.key_chunk.ColumnCount(); i++) {
			UnifiedVectorFormat vdata;
			l_state.key_chunk.data[i].ToUnifiedFormat(row_count, vdata);
			for (idx_t j = 0; j < row_count; j++) {
				auto idx = vdata.sel->get_index(j);
				if (!vdata.validity.RowIsValid(idx)) {
					throw ConstraintException("NOT NULL constraint failed: %s", info->index_name);
				}
			}
		}
	}

	auto &row_ids = chunk.data[chunk.ColumnCount() - 1];
	if (g_state.is_graph_index) {
		auto count = l_state.key_chunk.size();
		if (count == 0) {
			return SinkResultType::NEED_MORE_INPUT;
		}

		auto &vec_vector = l_state.key_chunk.data[0];
		vec_vector.Flatten(count);
		row_ids.Flatten(count);

		auto &validity = FlatVector::Validity(vec_vector);
		auto row_id_data = FlatVector::GetData<row_t>(row_ids);

		uint32_t dim = 0;
		auto &vec_type = vec_vector.GetType();
		if (vec_type.id() == LogicalTypeId::ARRAY) {
			dim = ArrayType::GetSize(vec_type);
		}

		auto &child_vec = ArrayVector::GetEntry(vec_vector);
		auto vec_data = FlatVector::GetData<float>(child_vec);

		std::lock_guard<std::mutex> lock(g_state.buffer_mutex);
		if (g_state.dimension == 0 && dim > 0) {
			g_state.dimension = dim;
		}

		auto &graph_idx = g_state.global_index->Cast<GraphIndex>();
		uint32_t meta_seg = graph_idx.GetGraphCore().meta_segment_size;
		if (g_state.meta_segment_size == 0 && meta_seg > 0) {
			g_state.meta_segment_size = meta_seg;
		}

		for (idx_t i = 0; i < count; i++) {
			if (!validity.RowIsValid(i)) {
				continue;
			}
			const float *vec = vec_data + i * g_state.dimension;
			g_state.all_vectors.insert(g_state.all_vectors.end(), vec, vec + g_state.dimension);
			g_state.all_row_ids.push_back(row_id_data[i]);

			if (g_state.meta_segment_size > 0 && l_state.key_chunk.ColumnCount() > 1) {
				size_t meta_start = g_state.all_metadata.size();
				g_state.all_metadata.resize(meta_start + g_state.meta_segment_size, 0);
				graph_idx.ExtractMetadata(l_state.key_chunk, i, g_state.all_metadata.data() + meta_start);
			}

			g_state.total_count++;
		}
	} else {
		IndexLock lock;
		l_state.local_index->InitializeLock(lock);
		l_state.local_index->Append(lock, l_state.key_chunk, row_ids);
	}

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalVexCreateIndex::Combine(ExecutionContext &context,
                                                      OperatorSinkCombineInput &input) const {
	auto &g_state = input.global_state.Cast<VexCreateIndexGlobalSinkState>();
	if (!g_state.is_graph_index) {
		auto &l_state = input.local_state.Cast<VexCreateIndexLocalSinkState>();
		IndexLock lock;
		g_state.global_index->InitializeLock(lock);
		if (!g_state.global_index->MergeIndexes(lock, *l_state.local_index)) {
			throw InternalException("Failed to merge local index into global index");
		}
	}
	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType PhysicalVexCreateIndex::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                                  OperatorSinkFinalizeInput &input) const {
	auto &state = input.global_state.Cast<VexCreateIndexGlobalSinkState>();
	auto &storage = table.GetStorage();
	if (!storage.IsMainTable()) {
		throw TransactionException(
		    "Transaction conflict: cannot add an index to a table that has been altered or dropped");
	}

	if (state.is_graph_index && state.total_count > 0) {
		auto &graph_index = state.global_index->Cast<GraphIndex>();
		int num_threads = MaxValue<int>(1, TaskScheduler::GetScheduler(context).NumberOfThreads());
		auto it = info->options.find("threads");
		if (it != info->options.end()) {
			try {
				num_threads = it->second.DefaultCastAs(LogicalType::INTEGER).GetValue<int>();
			} catch (const std::exception &) {
				throw InvalidInputException("GRAPH_INDEX: 'threads' must be a valid integer, got '%s'",
				                            it->second.ToString());
			}
			if (num_threads < 1 || num_threads > 1024) {
				throw InvalidInputException("GRAPH_INDEX: 'threads' must be between 1 and 1024, got %d",
				                            num_threads);
			}
		}
		graph_index.BuildParallel(state.all_vectors, state.all_row_ids, state.total_count, state.dimension, num_threads,
		                          state.all_metadata);
		state.all_vectors.clear();
		state.all_vectors.shrink_to_fit();
		state.all_row_ids.clear();
		state.all_row_ids.shrink_to_fit();
		state.all_metadata.clear();
		state.all_metadata.shrink_to_fit();
	}

	auto &schema = table.schema;
	info->column_ids = storage_ids;
	if (!alter_table_info) {
		auto entry = schema.GetEntry(schema.GetCatalogTransaction(context), CatalogType::INDEX_ENTRY, info->index_name);
		if (entry) {
			if (info->on_conflict != OnCreateConflict::IGNORE_ON_CONFLICT) {
				throw CatalogException("Index with name \"%s\" already exists!", info->index_name);
			}
			return SinkFinalizeType::READY;
		}

		auto index_entry = schema.CreateIndex(schema.GetCatalogTransaction(context), *info, table).get();
		D_ASSERT(index_entry);
		auto &index = index_entry->Cast<DuckIndexEntry>();
		index.initial_index_size = state.global_index->GetInMemorySize();
	} else {
		auto &indexes = storage.GetDataTableInfo()->GetIndexes();
		if (indexes.Find(info->index_name)) {
			throw CatalogException("an index with that name already exists for this table: %s", info->index_name);
		}

		auto &catalog = Catalog::GetCatalog(context, info->catalog);
		catalog.Alter(context, *alter_table_info);
	}

	storage.AddIndex(std::move(state.global_index));
	return SinkFinalizeType::READY;
}

SourceResultType PhysicalVexCreateIndex::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                         OperatorSourceInput &input) const {
	return SourceResultType::FINISHED;
}

} // namespace duckdb
