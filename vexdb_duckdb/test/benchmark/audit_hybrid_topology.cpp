// Read-only topology export for a frozen native index. This diagnostic uses
// explicit-instantiation member access, which C++ exempts from access checks.
// It does not alter the product class, its layout, edges or database contents.
#include "vex_graph_index.hpp"
#define main historical_product_benchmark_main
#include "vex_quantization_product_benchmark.cpp"
#undef main
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"

namespace {
struct RuntimeMember {
    using type = std::unique_ptr<GraphIndexRuntimeState> GraphIndex::*;
    friend type AuditMember(RuntimeMember);
};
template <class Tag, typename Tag::type member> struct AuditAccess {
    friend typename Tag::type AuditMember(Tag) { return member; }
};
template struct AuditAccess<RuntimeMember, &GraphIndex::runtime_>;
}

int main(int argc, char **argv) {
    if (argc != 4 || std::filesystem::exists(argv[3])) return 2;
    try {
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", true);
        config.SetOptionByName("access_mode", Value("read_only"));
        DuckDB db(argv[2], &config);
        Connection con(db);
        Exec(con, "LOAD " + Quote(std::filesystem::absolute(argv[1]).string()));
        Exec(con, "SET threads=1");
        Exec(con, "SELECT * FROM vexdb_index_info()");
        con.BeginTransaction();
        auto &table = Catalog::GetEntry(*con.context, CatalogType::TABLE_ENTRY, INVALID_CATALOG,
            DEFAULT_SCHEMA, "vectors").Cast<TableCatalogEntry>().Cast<DuckTableEntry>();
        GraphIndex *selected = nullptr;
        for (auto &index : table.GetStorage().GetDataTableInfo()->GetIndexes().Indexes()) {
            if (!index.IsBound() || index.GetIndexType() != GraphIndex::TYPE_NAME) continue;
            if (selected) throw std::runtime_error("ambiguous graph");
            selected = &index.Cast<BoundIndex>().Cast<GraphIndex>();
        }
        if (!selected) throw std::runtime_error("graph missing");
        auto &state = selected->*AuditMember(RuntimeMember{});
        if (!state) throw std::runtime_error("graph runtime missing");
        // A read-only database and this live transaction keep the index alive;
        // no writer or product search is run concurrently in this process.
        const auto &store = state->store;
        const auto n = store.elems.size();
        if (store.base_points.size() != n) throw std::runtime_error("mirror size mismatch");
        std::filesystem::create_directories(argv[3]);
        const auto out = std::filesystem::path(argv[3]);
        std::ofstream meta(out / "metadata.json");
        meta << "{\"nodes\":" << n << ",\"dimension\":" << selected->GetDimension()
             << ",\"m\":" << selected->GetM() << ",\"entry\":" << store.entry_info.id
             << ",\"entry_level\":" << int(store.entry_info.level) << "}\n";
        std::ofstream nodes(out / "nodes.csv"), edges(out / "edges.csv");
        nodes << "node,row_ids\n"; edges << "source,target\n";
        for (size_t sid = 0; sid < n; ++sid) {
            nodes << sid << ',';
            for (const auto &tid : store.elems[sid].tids) nodes << tid.row_id << ' ';
            nodes << '\n';
            for (auto neighbor : store.base_points[sid].neighbors) {
                if (neighbor == std::numeric_limits<uint32_t>::max()) break;
                if (neighbor >= n) throw std::runtime_error("invalid neighbor");
                edges << sid << ',' << neighbor << '\n';
            }
        }
        con.Rollback();
        std::cout << "COMPLETE read-only topology export nodes=" << n << '\n';
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
