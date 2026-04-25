#include "vex_functions.hpp"
#include "vex_duckdb_compat.hpp"
#include "vex_graph_index.hpp"
#include "vex_distance.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/append_state.hpp"
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
		int32_t partition_count; // reserved (always 0)
		// Build parameters
		int32_t m;
		int32_t ef_construction;
		string metric;
		bool use_pq;
		int32_t pq_m;
		int64_t memory_bytes;
		string memory_mode;
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

	names.push_back("m");
	return_types.push_back(LogicalType::INTEGER);

	names.push_back("ef_construction");
	return_types.push_back(LogicalType::INTEGER);

	names.push_back("metric");
	return_types.push_back(LogicalType::VARCHAR);

	names.push_back("use_pq");
	return_types.push_back(LogicalType::BOOLEAN);

	names.push_back("pq_m");
	return_types.push_back(LogicalType::INTEGER);

	names.push_back("memory_bytes");
	return_types.push_back(LogicalType::BIGINT);

	names.push_back("memory_mode");
	return_types.push_back(LogicalType::VARCHAR);

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
			if (index_entry.index_type == GraphIndex::TYPE_NAME) {
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

		vex_compat::ForEachTableIndex(index_list, [&](Index &index) {
			if (!index.IsBound() || index.GetIndexName() != target.index_name) {
				return false;
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
				e.m = graph_idx.GetM();
				e.ef_construction = graph_idx.GetEfConstruction();
				e.metric = vex::MetricName(graph_idx.GetMetric());
				e.use_pq = graph_idx.GetUsePQ();
				e.pq_m = static_cast<int32_t>(graph_idx.GetPQM());
				{
					IndexLock mem_lock;
					graph_idx.InitializeLock(mem_lock);
					e.memory_bytes = static_cast<int64_t>(graph_idx.GetInMemorySize(mem_lock));
				}
				e.memory_mode = graph_idx.GetMemoryMode();
				state->entries.push_back(std::move(e));
			}
			return true; // found the target index, stop scanning
		});
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
	auto m_data = FlatVector::GetData<int32_t>(output.data[8]);
	auto ef_data = FlatVector::GetData<int32_t>(output.data[9]);
	auto &metric_vec = output.data[10];
	auto metric_data = FlatVector::GetData<string_t>(metric_vec);
	auto pq_data = FlatVector::GetData<bool>(output.data[11]);
	auto pqm_data = FlatVector::GetData<int32_t>(output.data[12]);
	auto mem_bytes_data = FlatVector::GetData<int64_t>(output.data[13]);
	auto &mm_vec = output.data[14];
	auto mm_data = FlatVector::GetData<string_t>(mm_vec);

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
		m_data[count] = e.m;
		ef_data[count] = e.ef_construction;
		metric_data[count] = StringVector::AddString(metric_vec, e.metric);
		pq_data[count] = e.use_pq;
		pqm_data[count] = e.pq_m;
		mem_bytes_data[count] = e.memory_bytes;
		mm_data[count] = StringVector::AddString(mm_vec, e.memory_mode);
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
