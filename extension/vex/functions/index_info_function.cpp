#include "vex_functions.hpp"
#include "vex_graph_index.hpp"
#ifdef VEX_ENABLE_HYBRID_INDEX
#include "vex_hybrid_index.hpp"
#endif

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/storage/data_table.hpp"
#include <set>

namespace duckdb {

// ============================================================
// Bind Data
// ============================================================
struct VexIndexInfoBindData : public TableFunctionData {
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<VexIndexInfoBindData>();
	}

	bool Equals(const FunctionData &other_p) const override {
		return true;
	}
};

// ============================================================
// Global State — collects all vex index info at init
// ============================================================
struct VexIndexInfoGlobalState : public GlobalTableFunctionState {
	struct IndexEntry {
		string index_name;
		string index_type;
		string table_name;
		int64_t node_count;
		int32_t max_level;
		int32_t dimension;
		int64_t row_id_map_size;
		int32_t partition_count; // 0 for GraphIndex, N for HybridIndex
	};

	std::vector<IndexEntry> entries;
	idx_t current_offset = 0;
};

// ============================================================
// Bind
// ============================================================
static unique_ptr<FunctionData> VexIndexInfoBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	names.push_back("index_name");
	return_types.push_back(LogicalType::VARCHAR);

	names.push_back("index_type");
	return_types.push_back(LogicalType::VARCHAR);

	names.push_back("table_name");
	return_types.push_back(LogicalType::VARCHAR);

	names.push_back("partition_count");
	return_types.push_back(LogicalType::INTEGER);

	names.push_back("node_count");
	return_types.push_back(LogicalType::BIGINT);

	names.push_back("max_level");
	return_types.push_back(LogicalType::INTEGER);

	names.push_back("dimension");
	return_types.push_back(LogicalType::INTEGER);

	names.push_back("row_id_map_size");
	return_types.push_back(LogicalType::BIGINT);

	return make_uniq<VexIndexInfoBindData>();
}

// ============================================================
// Init — scan only VEX index entries (not all tables)
// ============================================================
static unique_ptr<GlobalTableFunctionState> VexIndexInfoInit(ClientContext &context,
                                                              TableFunctionInitInput &input) {
	auto state = make_uniq<VexIndexInfoGlobalState>();

	// Step 1: Scan INDEX_ENTRY catalog to find VEX indexes and their tables.
	// This avoids iterating all tables and calling Bind() on non-VEX tables.
	struct VexIndexTarget {
		string schema_name;
		string table_name;
		string index_name;
	};
	vector<VexIndexTarget> targets;

	auto schemas = Catalog::GetAllSchemas(context);
	for (auto &schema_ref : schemas) {
		auto &schema = schema_ref.get();
		schema.Scan(context, CatalogType::INDEX_ENTRY, [&](CatalogEntry &entry) {
			auto &index_entry = entry.Cast<IndexCatalogEntry>();
			if (index_entry.index_type == GraphIndex::TYPE_NAME
#ifdef VEX_ENABLE_HYBRID_INDEX
			    || index_entry.index_type == HybridIndex::TYPE_NAME
#endif
			    ) {
				VexIndexTarget t;
				t.schema_name = index_entry.GetSchemaName();
				t.table_name = index_entry.GetTableName();
				t.index_name = index_entry.name;
				targets.push_back(std::move(t));
			}
		});
	}

	// Step 2: Bind each unique table once, then collect metadata for all VEX indexes.
	std::set<std::pair<string, string>> bound_tables;
	for (auto &target : targets) {
		auto table_key = std::make_pair(target.schema_name, target.table_name);
		if (bound_tables.find(table_key) == bound_tables.end()) {
			auto &table_entry = Catalog::GetEntry<TableCatalogEntry>(context, INVALID_CATALOG,
			                                                          target.schema_name, target.table_name);
			auto &duck_table = table_entry.Cast<DuckTableEntry>();
			auto &data_table = duck_table.GetStorage();
			auto &index_list = data_table.GetDataTableInfo()->GetIndexes();
			index_list.Bind(context, *data_table.GetDataTableInfo());
			bound_tables.insert(table_key);
		}

		auto &table_entry = Catalog::GetEntry<TableCatalogEntry>(context, INVALID_CATALOG,
		                                                          target.schema_name, target.table_name);
		auto &duck_table = table_entry.Cast<DuckTableEntry>();
		auto &data_table = duck_table.GetStorage();
		auto &index_list = data_table.GetDataTableInfo()->GetIndexes();

		for (auto &index : index_list.Indexes()) {
			if (!index.IsBound() || index.GetIndexName() != target.index_name) {
				continue;
			}
			auto &bound_index = index.Cast<BoundIndex>();

			if (bound_index.GetIndexType() == GraphIndex::TYPE_NAME) {
				auto &graph_idx = bound_index.Cast<GraphIndex>();
				VexIndexInfoGlobalState::IndexEntry e;
				e.index_name = target.index_name;
				e.index_type = "GRAPH_INDEX";
				e.table_name = target.table_name;
				e.partition_count = 0;
				e.node_count = static_cast<int64_t>(graph_idx.GetGraphCore().node_count);
				e.max_level = graph_idx.GetGraphCore().max_level;
				e.dimension = static_cast<int32_t>(graph_idx.GetGraphCore().dimension);
				e.row_id_map_size = static_cast<int64_t>(graph_idx.GetGraphCore().row_id_map.size());
				state->entries.push_back(std::move(e));
			}
#ifdef VEX_ENABLE_HYBRID_INDEX
			else if (bound_index.GetIndexType() == HybridIndex::TYPE_NAME) {
				auto &hybrid_idx = bound_index.Cast<HybridIndex>();
				VexIndexInfoGlobalState::IndexEntry e;
				e.index_name = target.index_name;
				e.index_type = "HYBRID_INDEX";
				e.table_name = target.table_name;
				auto &partitions = hybrid_idx.GetPartitions();
				e.partition_count = static_cast<int32_t>(partitions.size());
				e.node_count = 0;
				e.max_level = 0;
				e.dimension = 0;
				e.row_id_map_size = 0;
				for (auto &kv : partitions) {
					e.node_count += static_cast<int64_t>(kv.second.node_count);
					e.row_id_map_size += static_cast<int64_t>(kv.second.row_id_map.size());
					if (kv.second.max_level > e.max_level) {
						e.max_level = kv.second.max_level;
					}
					if (e.dimension == 0) {
						e.dimension = static_cast<int32_t>(kv.second.dimension);
					}
				}
				state->entries.push_back(std::move(e));
			}
#endif
			break; // found the target index, stop scanning
		}
	}

	return std::move(state);
}

// ============================================================
// Execute
// ============================================================
static void VexIndexInfoExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<VexIndexInfoGlobalState>();

	idx_t count = 0;
	idx_t max_count = STANDARD_VECTOR_SIZE;

	auto &name_vec = output.data[0];
	auto &type_vec = output.data[1];
	auto &table_vec = output.data[2];
	auto name_data = FlatVector::GetData<string_t>(name_vec);
	auto type_data = FlatVector::GetData<string_t>(type_vec);
	auto table_data = FlatVector::GetData<string_t>(table_vec);
	auto part_count_data = FlatVector::GetData<int32_t>(output.data[3]);
	auto node_data = FlatVector::GetData<int64_t>(output.data[4]);
	auto level_data = FlatVector::GetData<int32_t>(output.data[5]);
	auto dim_data = FlatVector::GetData<int32_t>(output.data[6]);
	auto rmap_data = FlatVector::GetData<int64_t>(output.data[7]);

	while (state.current_offset < state.entries.size() && count < max_count) {
		auto &e = state.entries[state.current_offset];
		name_data[count] = StringVector::AddString(name_vec, e.index_name);
		type_data[count] = StringVector::AddString(type_vec, e.index_type);
		table_data[count] = StringVector::AddString(table_vec, e.table_name);
		part_count_data[count] = e.partition_count;
		node_data[count] = e.node_count;
		level_data[count] = e.max_level;
		dim_data[count] = e.dimension;
		rmap_data[count] = e.row_id_map_size;
		count++;
		state.current_offset++;
	}

	output.SetCardinality(count);
}

// ============================================================
// Register
// ============================================================
void VexFunctions::RegisterIndexInfoFunction(ExtensionLoader &loader) {
	TableFunction func("vex_index_info", {}, VexIndexInfoExecute, VexIndexInfoBind, VexIndexInfoInit);
	loader.RegisterFunction(func);
}

} // namespace duckdb
