#include "duckdb/execution/operator/schema/physical_create_graph_index.hpp"

#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/execution/index/index_type_set.hpp"
#include "duckdb/main/config.hpp"
#include "vex_graph_index.hpp"

namespace duckdb {

PhysicalCreateGraphIndex::PhysicalCreateGraphIndex(PhysicalPlan &physical_plan, LogicalOperator &op,
                                               TableCatalogEntry &table_p, const vector<column_t> &column_ids,
                                               unique_ptr<CreateIndexInfo> info,
                                               vector<unique_ptr<Expression>> unbound_expressions,
                                               idx_t estimated_cardinality,
                                               unique_ptr<AlterTableInfo> alter_table_info)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::CREATE_INDEX, op.types, estimated_cardinality),
      table(table_p.Cast<DuckTableEntry>()), info(std::move(info)), unbound_expressions(std::move(unbound_expressions)),
      alter_table_info(std::move(alter_table_info)) {

	// Convert the logical column ids to physical column ids.
	for (auto &column_id : column_ids) {
		storage_ids.push_back(table.GetColumns().LogicalToPhysical(LogicalIndex(column_id)).index);
	}
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//

class CreateGraphIndexGlobalSinkState : public GlobalSinkState {
public:
	//! We merge the local indexes into one global index.
	unique_ptr<BoundIndex> global_index;
};

class CreateGraphIndexLocalSinkState : public LocalSinkState {
public:
	explicit CreateGraphIndexLocalSinkState(ClientContext &context) {
	}

	unique_ptr<BoundIndex> local_index;
	DataChunk key_chunk;
	vector<column_t> key_column_ids;
};

unique_ptr<GlobalSinkState> PhysicalCreateGraphIndex::GetGlobalSinkState(ClientContext &context) const {
	// Create the global sink state.
	auto state = make_uniq<CreateGraphIndexGlobalSinkState>();

	// Create the global index using the registered index type
	auto &config = DBConfig::GetConfig(context);
	auto &index_types = config.GetIndexTypes();
	auto index_type_ref = index_types.FindByName(info->index_type);

	if (!index_type_ref) {
		throw InternalException("Index type '%s' not found in registry", info->index_type);
	}

	auto &index_type = *index_type_ref;

	// Prepare input for index creation
	IndexStorageInfo storage_info;
	CreateIndexInput index_input(
		TableIOManager::Get(table.GetStorage()),
		table.GetStorage().db,
		info->constraint_type,
		info->index_name,
		storage_ids,
		unbound_expressions,
		storage_info,
		info->options
	);

	state->global_index = index_type.create_instance(index_input);
	return (std::move(state));
}

unique_ptr<LocalSinkState> PhysicalCreateGraphIndex::GetLocalSinkState(ExecutionContext &context) const {
	auto state = make_uniq<CreateGraphIndexLocalSinkState>(context.client);

	// Create the local index using the registered index type
	auto &config = DBConfig::GetConfig(context.client);
	auto &index_types = config.GetIndexTypes();
	auto index_type_ref = index_types.FindByName(info->index_type);

	if (!index_type_ref) {
		throw InternalException("Index type '%s' not found in registry", info->index_type);
	}

	auto &index_type = *index_type_ref;

	// Prepare input for index creation
	IndexStorageInfo storage_info;
	CreateIndexInput index_input(
		TableIOManager::Get(table.GetStorage()),
		table.GetStorage().db,
		info->constraint_type,
		info->index_name,
		storage_ids,
		unbound_expressions,
		storage_info,
		info->options
	);

	state->local_index = index_type.create_instance(index_input);

	// Initialize the local sink state.
	state->key_chunk.Initialize(Allocator::Get(context.client), state->local_index->logical_types);
	for (idx_t i = 0; i < state->key_chunk.ColumnCount(); i++) {
		state->key_column_ids.push_back(i);
	}
	return std::move(state);
}

SinkResultType PhysicalCreateGraphIndex::Sink(ExecutionContext &context, DataChunk &chunk,
                                            OperatorSinkInput &input) const {
	D_ASSERT(chunk.ColumnCount() >= 2);
	auto &l_state = input.local_state.Cast<CreateGraphIndexLocalSinkState>();
	l_state.key_chunk.ReferenceColumns(chunk, l_state.key_column_ids);

	// Check for NULLs for PRIMARY KEY
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

	// Insert data into the local index using the BoundIndex API
	auto &local_index = l_state.local_index;
	auto &row_ids = chunk.data[chunk.ColumnCount() - 1];

	// Use Append to insert data into the index
	IndexLock lock;
	local_index->Append(lock, l_state.key_chunk, row_ids);

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalCreateGraphIndex::Combine(ExecutionContext &context,
                                                      OperatorSinkCombineInput &input) const {
	auto &g_state = input.global_state.Cast<CreateGraphIndexGlobalSinkState>();

	// Merge the local index into the global index.
	auto &l_state = input.local_state.Cast<CreateGraphIndexLocalSinkState>();

	IndexLock lock;
	if (!g_state.global_index->MergeIndexes(lock, *l_state.local_index)) {
		throw InternalException("Failed to merge local index into global index");
	}

	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType PhysicalCreateGraphIndex::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                                  OperatorSinkFinalizeInput &input) const {
	auto &state = input.global_state.Cast<CreateGraphIndexGlobalSinkState>();

	auto &storage = table.GetStorage();
	if (!storage.IsMainTable()) {
		throw TransactionException(
		    "Transaction conflict: cannot add an index to a table that has been altered or dropped");
	}

	auto &schema = table.schema;
	info->column_ids = storage_ids;

	if (!alter_table_info) {
		// Ensure that the index does not yet exist in the catalog.
		auto entry = schema.GetEntry(schema.GetCatalogTransaction(context), CatalogType::INDEX_ENTRY, info->index_name);
		if (entry) {
			if (info->on_conflict != OnCreateConflict::IGNORE_ON_CONFLICT) {
				throw CatalogException("Index with name \"%s\" already exists!", info->index_name);
			}
			// IF NOT EXISTS on existing index. We are done.
			return SinkFinalizeType::READY;
		}

		auto index_entry = schema.CreateIndex(schema.GetCatalogTransaction(context), *info, table).get();
		D_ASSERT(index_entry);
		auto &index = index_entry->Cast<DuckIndexEntry>();
		index.initial_index_size = state.global_index->GetInMemorySize();

	} else {
		// Ensure that there are no other indexes with that name on this table.
		auto &indexes = storage.GetDataTableInfo()->GetIndexes();
		indexes.Scan([&](Index &index) {
			if (index.GetIndexName() == info->index_name) {
				throw CatalogException("an index with that name already exists for this table: %s", info->index_name);
			}
			return false;
		});

		auto &catalog = Catalog::GetCatalog(context, info->catalog);
		catalog.Alter(context, *alter_table_info);
	}

	// Add the index to the storage.
	storage.AddIndex(std::move(state.global_index));
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//

SourceResultType PhysicalCreateGraphIndex::GetData(ExecutionContext &context, DataChunk &chunk,
                                                 OperatorSourceInput &input) const {
	return SourceResultType::FINISHED;
}

} // namespace duckdb
