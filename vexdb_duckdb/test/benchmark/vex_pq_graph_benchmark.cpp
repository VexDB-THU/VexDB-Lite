#include "vex_graph_index.hpp"

// Reuse the same SQL timer and raw-ID export as the earlier product benchmark.
#define main quantization_build_benchmark_main
#include "vex_quantization_product_benchmark.cpp"
#undef main

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"

int main(int argc, char **argv) {
    if (argc != 10) {
        std::cerr << "usage: vex_pq_graph_benchmark <extension> <saved_db> <data_dir> <rows> <dim> <query_offset> <query_count> <points_file> <new_output>\n";
        return 2;
    }
    try {
        const auto extension = std::filesystem::absolute(argv[1]).string();
        const std::string database = argv[2], data = argv[3];
        const size_t n = std::stoull(argv[4]), dim = std::stoull(argv[5]), offset = std::stoull(argv[6]), nq = std::stoull(argv[7]);
        const std::filesystem::path out = argv[9];
        if (!nq || offset + nq > 200 || std::filesystem::exists(out)) throw std::runtime_error("invalid query range or output exists");
        std::filesystem::create_directories(out);
        const auto floats = Read<float>(data + "/queries.f32", (offset + nq) * dim);
        const auto all_truth = Read<int32_t>(data + "/truth-" + std::to_string(n) + ".i32", (offset + nq) * 10);
        std::vector<int32_t> truth(all_truth.begin() + offset * 10, all_truth.end());
        std::vector<Value> queries;
        for (size_t q = offset; q < offset + nq; ++q) {
            vector<Value> values;
            for (size_t j = 0; j < dim; ++j) values.push_back(Value::FLOAT(floats[q * dim + j]));
            queries.push_back(Value::ARRAY(LogicalType::FLOAT, std::move(values)));
        }
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", true);
        config.SetOptionByName("access_mode", Value("read_only"));
        DuckDB db(database, &config);
        Connection con(db);
        Exec(con, "LOAD " + Quote(extension));
        Configure(con, "pq_only", 128, 4);
        auto info = Exec(con, "SELECT * FROM vexdb_index_info()"); // bind the saved index
        std::ofstream(out / "index-info.txt") << info->ToString();
        std::ofstream summary(out / "points.csv"), counts(out / "counts.csv");
        summary << std::setprecision(12) << "point,connections,calls,recall10,qps,p50_ms,p95_ms,first_ms,rss_warm,rss_after,wall_ms\n";
        counts << "point,query,codes_scored,raw_vectors_refined,ids\n";
        std::ifstream points(argv[8]);
        std::string point, mode;
        int ef, connections;
        double factor;
        if (!points) throw std::runtime_error("cannot read points");
        while (points >> point >> mode >> ef >> factor >> connections) {
            Measure(db, con, queries, truth, dim, point, mode, ef, factor, connections, out, summary);
            if (mode == "off") continue;
            // Independent, untimed API audit. No count collection is inside SQL timing.
            con.BeginTransaction();
            auto &table = Catalog::GetEntry(*con.context, CatalogType::TABLE_ENTRY, INVALID_CATALOG,
                                            DEFAULT_SCHEMA, "vectors").Cast<TableCatalogEntry>().Cast<DuckTableEntry>();
            auto &indexes = table.GetStorage().GetDataTableInfo()->GetIndexes();
            bool found = false;
            for (auto &index : indexes.Indexes()) {
                if (!index.IsBound() || index.GetIndexType() != GraphIndex::TYPE_NAME) continue;
                auto &graph = index.Cast<BoundIndex>().Cast<GraphIndex>();
                for (size_t q = 0; q < nq; ++q) {
                    std::vector<row_t> ids;
                    std::vector<float> distances;
                    PQSearchMetrics metrics;
                    graph.SearchPQ(floats.data() + (offset + q) * dim, 10, ids, distances, factor,
                                   mode == "pq_graph" ? ef : 0, &metrics);
                    counts << point << ',' << q << ',' << metrics.codes_scored << ',' << metrics.raw_vectors_refined << ',';
                    for (auto id : ids) counts << id << ' ';
                    counts << '\n';
                }
                found = true;
            }
            con.Rollback();
            if (!found) throw std::runtime_error("bound graph index missing");
        }
        std::cout << "COMPLETE read-only saved-index query benchmark\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAILED " << error.what() << '\n';
        return 1;
    }
}
