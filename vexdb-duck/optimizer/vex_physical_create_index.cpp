#include "vex_physical_create_index.hpp"

#include "vex_graph_index.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/execution/index/index_type_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/table_io_manager.hpp"

namespace duckdb {

struct VexCreateIndexGlobalState : public GlobalSinkState {
    unique_ptr<BoundIndex> global_index;
    bool is_graph_index = false;
    std::mutex buffer_mutex;
    std::vector<float> all_vectors;
    std::vector<row_t> all_row_ids;
    idx_t total_count = 0;
    idx_t dimension = 0;
};

struct VexCreateIndexLocalState : public LocalSinkState {
    DataChunk key_chunk;
    DataChunk row_chunk;
    vector<column_t> key_column_ids;
    vector<column_t> rowid_column;
};

PhysicalVexCreateIndex::PhysicalVexCreateIndex(PhysicalPlan &physical_plan, LogicalOperator &op,
                                               TableCatalogEntry &table_p, const vector<column_t> &column_ids,
                                               unique_ptr<CreateIndexInfo> info_p,
                                               vector<unique_ptr<Expression>> unbound_expressions_p,
                                               idx_t estimated_cardinality,
                                               unique_ptr<AlterTableInfo> alter_table_info_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::CREATE_INDEX, {}, estimated_cardinality),
      table(table_p.Cast<DuckTableEntry>()), info(std::move(info_p)),
      unbound_expressions(std::move(unbound_expressions_p)), alter_table_info(std::move(alter_table_info_p)) {
    types = op.types;
    for (auto &column_id : column_ids) {
        storage_ids.push_back(table.GetColumns().LogicalToPhysical(LogicalIndex(column_id)).index);
    }
}

SourceResultType PhysicalVexCreateIndex::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                         OperatorSourceInput &input) const {
    (void)context;
    (void)chunk;
    (void)input;
    return SourceResultType::FINISHED;
}

unique_ptr<LocalSinkState> PhysicalVexCreateIndex::GetLocalSinkState(ExecutionContext &context) const {
    auto state = make_uniq<VexCreateIndexLocalState>();
    vector<LogicalType> key_types;
    for (auto &expr : unbound_expressions) {
        key_types.push_back(expr->GetReturnType());
    }
    state->key_chunk.Initialize(Allocator::Get(context.client), key_types);
    state->row_chunk.Initialize(Allocator::Get(context.client), {LogicalType::ROW_TYPE});
    for (idx_t i = 0; i < state->key_chunk.ColumnCount(); i++) {
        state->key_column_ids.push_back(i);
    }
    state->rowid_column.push_back(unbound_expressions.size());
    return std::move(state);
}

unique_ptr<GlobalSinkState> PhysicalVexCreateIndex::GetGlobalSinkState(ClientContext &context) const {
    auto state = make_uniq<VexCreateIndexGlobalState>();
    auto &config = DBConfig::GetConfig(context);
    auto &index_types = config.GetIndexTypes();
    auto index_type_ref = index_types.FindByName(info->index_type);
    if (!index_type_ref) {
        throw InternalException("Index type '%s' not found in registry", info->index_type);
    }

    IndexStorageInfo storage_info;
    CreateIndexInput input(context, TableIOManager::Get(table.GetStorage()), table.GetStorage().db,
                           info->constraint_type, info->index_name, storage_ids, unbound_expressions, storage_info,
                           info->options);
    state->global_index = index_type_ref->create_instance(input);
    state->is_graph_index = (info->index_type == GraphIndex::TYPE_NAME);
    return std::move(state);
}

SinkResultType PhysicalVexCreateIndex::Sink(ExecutionContext &context, DataChunk &chunk,
                                            OperatorSinkInput &input) const {
    auto &g_state = input.global_state.Cast<VexCreateIndexGlobalState>();
    auto &l_state = input.local_state.Cast<VexCreateIndexLocalState>();

    chunk.Flatten();
    l_state.key_chunk.ReferenceColumns(chunk, l_state.key_column_ids);
    l_state.row_chunk.ReferenceColumns(chunk, l_state.rowid_column);

    if (g_state.is_graph_index) {
        auto count = l_state.key_chunk.size();
        if (count == 0) {
            return SinkResultType::NEED_MORE_INPUT;
        }

        auto &vec_vector = l_state.key_chunk.data[0];
        vec_vector.Flatten(count);
        auto &row_ids = l_state.row_chunk.data[0];
        row_ids.Flatten(count);

        auto &vec_type = vec_vector.GetType();
        auto dim = ArrayType::GetSize(vec_type);
        auto &validity = FlatVector::Validity(vec_vector);
        auto &child_vec = ArrayVector::GetEntry(vec_vector);
        auto vec_data = FlatVector::GetDataMutable<float>(child_vec);
        auto row_id_data = FlatVector::GetDataMutable<row_t>(row_ids);

        std::lock_guard<std::mutex> lock(g_state.buffer_mutex);
        if (g_state.dimension == 0) {
            g_state.dimension = dim;
        }

        for (idx_t i = 0; i < count; i++) {
            if (!validity.RowIsValid(i)) {
                continue;
            }
            const float *vec = vec_data + i * dim;
            g_state.all_vectors.insert(g_state.all_vectors.end(), vec, vec + dim);
            g_state.all_row_ids.push_back(row_id_data[i]);
            g_state.total_count++;
        }
    }
    return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalVexCreateIndex::Combine(ExecutionContext &context,
                                                      OperatorSinkCombineInput &input) const {
    (void)context;
    (void)input;
    return SinkCombineResultType::FINISHED;
}

SinkFinalizeType PhysicalVexCreateIndex::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                                  OperatorSinkFinalizeInput &input) const {
    auto &state = input.global_state.Cast<VexCreateIndexGlobalState>();
    auto &storage = table.GetStorage();
    if (!storage.IsMainTable()) {
        throw TransactionException(
            "Transaction conflict: cannot add an index to a table that has been altered or dropped");
    }

    if (state.is_graph_index) {
        auto &graph_index = state.global_index->Cast<GraphIndex>();
        graph_index.BuildBulk(state.all_vectors, state.all_row_ids);
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
        for (auto &index : indexes.Indexes()) {
            if (index.GetIndexName() == info->index_name) {
                throw CatalogException("an index with that name already exists for this table: %s", info->index_name);
            }
        }
        auto &catalog = Catalog::GetCatalog(context, info->catalog);
        catalog.Alter(context, *alter_table_info);
    }

    storage.AddIndex(std::move(state.global_index));
    return SinkFinalizeType::READY;
}

} // namespace duckdb
