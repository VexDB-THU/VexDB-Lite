// Read saved PQ metadata without loading or mutating the extension/index.
#include "duckdb.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/execution/index/unbound_index.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

using namespace duckdb;

int main(int argc, char **argv) {
    if (argc != 3 || std::filesystem::exists(argv[2])) {
        std::cerr << "usage: export_pq_storage <database> <new_output_directory>\n";
        return 2;
    }
    try {
        DBConfig config;
        config.SetOptionByName("access_mode", Value("read_only"));
        DuckDB db(argv[1], &config);
        Connection con(db);
        con.BeginTransaction();
        auto &table = Catalog::GetEntry(*con.context, CatalogType::TABLE_ENTRY, INVALID_CATALOG,
                                        DEFAULT_SCHEMA, "vectors").Cast<TableCatalogEntry>().Cast<DuckTableEntry>();
        auto &indexes = table.GetStorage().GetDataTableInfo()->GetIndexes();
        bool found = false;
        for (auto &index : indexes.Indexes()) {
            if (index.GetIndexType() != "GRAPH_INDEX") continue;
            if (index.IsBound()) throw std::runtime_error("expected an unbound saved index");
            const auto &info = index.Cast<UnboundIndex>().GetStorageInfo();
            std::filesystem::create_directories(argv[2]);
            for (const char *name : {"pq_codebook", "pq_codes", "pq_row_order", "pq_vector_coverage_hashes"}) {
                auto field = info.options.find(name);
                if (field == info.options.end()) throw std::runtime_error(std::string("missing field ") + name);
                const auto bytes = StringValue::Get(field->second.DefaultCastAs(LogicalType::BLOB));
                const auto path = std::filesystem::path(argv[2]) / (std::string(name) + ".bin");
                std::ofstream stream(path, std::ios::binary);
                stream.write(bytes.data(), bytes.size());
                if (!stream) throw std::runtime_error("cannot export metadata");
                std::cout << name << ',' << bytes.size() << '\n';
            }
            found = true;
        }
        con.Rollback();
        if (!found) throw std::runtime_error("no GRAPH_INDEX found");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
