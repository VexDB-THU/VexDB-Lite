#include "vex_functions.hpp"
#include "vex_graph_index.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/append_state.hpp"

#include <set>

namespace duckdb {

struct VexIndexInfoBindData : public TableFunctionData {
    unique_ptr<FunctionData> Copy() const override { return make_uniq<VexIndexInfoBindData>(); }
    bool Equals(const FunctionData &other_p) const override { return true; }
};

struct VexIndexInfoGlobalState : public GlobalTableFunctionState {
    struct IndexEntry {
        string index_name;
        string index_type;
        string table_name;
        int32_t partition_count;
        int64_t node_count;
        int32_t max_level;
        int32_t dimension;
        int64_t row_id_map_size;
        int32_t m;
        int32_t ef_construction;
        string metric;
        bool use_pq;
        int32_t pq_m;
        int64_t memory_bytes;
        int64_t pq_codes_bytes;
        int64_t pq_codebook_bytes;
        string memory_mode;
    };
    std::vector<IndexEntry> entries;
    idx_t current_offset = 0;
};

static const char *MetricToString(VexMetric metric) {
    switch (metric) {
    case VexMetric::L2:            return "l2";
    case VexMetric::INNER_PRODUCT: return "ip";
    case VexMetric::COSINE:        return "cosine";
    }
    return "unknown";
}

static unique_ptr<FunctionData> VexIndexInfoBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
    (void)context; (void)input;
    auto add = [&](const char *name, LogicalType ty) {
        names.emplace_back(name);
        return_types.push_back(std::move(ty));
    };
    add("index_name",       LogicalType::VARCHAR);
    add("index_type",       LogicalType::VARCHAR);
    add("table_name",       LogicalType::VARCHAR);
    add("partition_count",  LogicalType::INTEGER);
    add("node_count",       LogicalType::BIGINT);
    add("max_level",        LogicalType::INTEGER);
    add("dimension",        LogicalType::INTEGER);
    add("row_id_map_size",  LogicalType::BIGINT);
    add("m",                LogicalType::INTEGER);
    add("ef_construction",  LogicalType::INTEGER);
    add("metric",           LogicalType::VARCHAR);
    add("use_pq",           LogicalType::BOOLEAN);
    add("pq_m",             LogicalType::INTEGER);
    add("memory_bytes",     LogicalType::BIGINT);
    add("pq_codes_bytes",   LogicalType::BIGINT);
    add("pq_codebook_bytes",LogicalType::BIGINT);
    add("memory_mode",      LogicalType::VARCHAR);
    return make_uniq<VexIndexInfoBindData>();
}

static unique_ptr<GlobalTableFunctionState> VexIndexInfoInit(ClientContext &context, TableFunctionInitInput &input) {
    (void)input;
    auto state = make_uniq<VexIndexInfoGlobalState>();

    struct Target { string schema_name, table_name, index_name; };
    vector<Target> targets;

    auto schemas = Catalog::GetAllSchemas(context);
    for (auto &schema_ref : schemas) {
        auto &schema = schema_ref.get();
        schema.Scan(context, CatalogType::INDEX_ENTRY, [&](CatalogEntry &entry) {
            auto &index_entry = entry.Cast<IndexCatalogEntry>();
            if (index_entry.index_type == GraphIndex::TYPE_NAME) {
                Target t;
                t.schema_name = index_entry.GetSchemaName();
                t.table_name  = index_entry.GetTableName();
                t.index_name  = index_entry.name;
                targets.push_back(std::move(t));
            }
        });
    }

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
            if (bound_index.GetIndexType() != GraphIndex::TYPE_NAME) {
                continue;
            }
            auto &graph_idx = bound_index.Cast<GraphIndex>();

            VexIndexInfoGlobalState::IndexEntry e;
            e.index_name      = target.index_name;
            e.index_type      = "GRAPH_INDEX";
            e.table_name      = target.table_name;
            e.partition_count = 0;
            e.node_count      = static_cast<int64_t>(graph_idx.GetNodeCount());
            e.max_level       = 0;
            e.dimension       = static_cast<int32_t>(graph_idx.GetDimension());
            e.row_id_map_size = static_cast<int64_t>(graph_idx.GetRowIdCount());
            e.m               = graph_idx.GetM();
            e.ef_construction = graph_idx.GetEfConstruction();
            e.metric          = MetricToString(graph_idx.GetMetric());
            e.use_pq          = false;
            e.pq_m            = 0;
            {
                IndexLock mem_lock;
                graph_idx.InitializeLock(mem_lock);
                e.memory_bytes = static_cast<int64_t>(graph_idx.GetInMemorySize(mem_lock));
            }
            e.pq_codes_bytes    = 0;
            e.pq_codebook_bytes = 0;
            e.memory_mode       = "memory";
            state->entries.push_back(std::move(e));
            break;
        }
    }
    return std::move(state);
}

static void VexIndexInfoExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    (void)context;
    auto &state = data_p.global_state->Cast<VexIndexInfoGlobalState>();
    idx_t count = 0;
    idx_t max_count = STANDARD_VECTOR_SIZE;

    auto &name_vec   = output.data[0];
    auto &type_vec   = output.data[1];
    auto &table_vec  = output.data[2];
    auto &metric_vec = output.data[10];
    auto &mm_vec     = output.data[16];

    auto name_data            = FlatVector::GetData<string_t>(name_vec);
    auto type_data            = FlatVector::GetData<string_t>(type_vec);
    auto table_data           = FlatVector::GetData<string_t>(table_vec);
    auto part_count_data      = FlatVector::GetData<int32_t>(output.data[3]);
    auto node_data            = FlatVector::GetData<int64_t>(output.data[4]);
    auto level_data           = FlatVector::GetData<int32_t>(output.data[5]);
    auto dim_data             = FlatVector::GetData<int32_t>(output.data[6]);
    auto rmap_data            = FlatVector::GetData<int64_t>(output.data[7]);
    auto m_data               = FlatVector::GetData<int32_t>(output.data[8]);
    auto ef_data              = FlatVector::GetData<int32_t>(output.data[9]);
    auto metric_data          = FlatVector::GetData<string_t>(metric_vec);
    auto pq_data              = FlatVector::GetData<bool>(output.data[11]);
    auto pqm_data             = FlatVector::GetData<int32_t>(output.data[12]);
    auto mem_bytes_data       = FlatVector::GetData<int64_t>(output.data[13]);
    auto pq_codes_bytes_data  = FlatVector::GetData<int64_t>(output.data[14]);
    auto pq_codebook_bytes_data = FlatVector::GetData<int64_t>(output.data[15]);
    auto mm_data              = FlatVector::GetData<string_t>(mm_vec);

    while (state.current_offset < state.entries.size() && count < max_count) {
        auto &e = state.entries[state.current_offset];
        name_data[count]              = StringVector::AddString(name_vec, e.index_name);
        type_data[count]              = StringVector::AddString(type_vec, e.index_type);
        table_data[count]             = StringVector::AddString(table_vec, e.table_name);
        part_count_data[count]        = e.partition_count;
        node_data[count]              = e.node_count;
        level_data[count]             = e.max_level;
        dim_data[count]               = e.dimension;
        rmap_data[count]              = e.row_id_map_size;
        m_data[count]                 = e.m;
        ef_data[count]                = e.ef_construction;
        metric_data[count]            = StringVector::AddString(metric_vec, e.metric);
        pq_data[count]                = e.use_pq;
        pqm_data[count]               = e.pq_m;
        mem_bytes_data[count]         = e.memory_bytes;
        pq_codes_bytes_data[count]    = e.pq_codes_bytes;
        pq_codebook_bytes_data[count] = e.pq_codebook_bytes;
        mm_data[count]                = StringVector::AddString(mm_vec, e.memory_mode);
        count++;
        state.current_offset++;
    }
    output.SetCardinality(count);
}

void VexFunctions::RegisterIndexInfoFunction(ExtensionLoader &loader) {
    TableFunction func("vex_index_info", {}, VexIndexInfoExecute, VexIndexInfoBind, VexIndexInfoInit);
    loader.RegisterFunction(func);
}

} // namespace duckdb
