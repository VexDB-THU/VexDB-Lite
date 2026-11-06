// Untimed diagnostic extends audit_pq_refinement.cpp with full-PQ and partial pools.
// Graph pools may be smaller than k: preserve that evidence instead of discarding it.
// Shapes and inputs are arguments; no ground-truth IDs enter product searches.
#include "vex_graph_index.hpp"
#define main historical_product_benchmark_main
#include "vex_quantization_product_benchmark.cpp"
#undef main
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include <set>

int main(int argc, char **argv) {
    if (argc != 8 || std::filesystem::exists(argv[7])) {
        std::cerr << "usage: audit <extension> <db> <queries.f32> <dim> <nq> <points> <new_out>\n";
        return 2;
    }
    try {
        const size_t dim = std::stoull(argv[4]), nq = std::stoull(argv[5]);
        const auto queries = Read<float>(argv[3], dim * nq);
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", true);
        config.SetOptionByName("access_mode", Value("read_only"));
        DuckDB db(argv[2], &config);
        Connection con(db);
        Exec(con, "LOAD " + Quote(std::filesystem::absolute(argv[1]).string()));
        Configure(con, "pq_graph", 1536, 16);
        Exec(con, "SELECT * FROM vexdb_index_info()");
        con.BeginTransaction();
        auto &table = Catalog::GetEntry(*con.context, CatalogType::TABLE_ENTRY, INVALID_CATALOG,
            DEFAULT_SCHEMA, "vectors").Cast<TableCatalogEntry>().Cast<DuckTableEntry>();
        auto &indexes = table.GetStorage().GetDataTableInfo()->GetIndexes();
        std::filesystem::create_directories(argv[7]);
        std::ofstream out(std::filesystem::path(argv[7]) / "ids.csv");
        out << std::setprecision(9) << "point,query,codes_scored,raw_vectors_refined,ids,distances\n";
        GraphIndex *selected = nullptr;
        for (auto &index : indexes.Indexes()) {
            if (!index.IsBound() || index.GetIndexType() != GraphIndex::TYPE_NAME) continue;
            if (selected) throw std::runtime_error("multiple graph indexes unsupported by diagnostic");
            selected = &index.Cast<BoundIndex>().Cast<GraphIndex>();
        }
        // Indexes() owns the table index-list mutex. Release its iterator
        // before preparing/executing SQL that needs the same list. The read-
        // only database and live transaction keep the selected index alive.
        if (!selected) throw std::runtime_error("graph missing");
        {
            auto &graph = *selected;
            if (graph.GetDimension() != dim) throw std::runtime_error("dimension mismatch");
            std::ifstream points(argv[6]);
            if (!points) throw std::runtime_error("missing points file");
            std::string name, mode;
            size_t k;
            int ef;
            double factor;
            while (points >> name >> mode >> k >> ef >> factor) {
                Configure(con, "pq_graph", ef, factor);
                auto statement = con.Prepare(QuerySQL(dim));
                if (statement->HasError()) throw std::runtime_error(statement->GetError());
                for (size_t q = 0; q < nq; ++q) {
                    std::vector<row_t> ids;
                    std::vector<float> distances;
                    PQSearchMetrics metrics;
                    if (mode == "raw") graph.SearchANN(queries.data() + q * dim, k, ef, ids, distances);
                    else if (mode == "pq" || mode == "sqlpq" || mode == "pq-full") {
                        graph.SearchPQ(queries.data() + q * dim, k, ids, distances, factor, mode == "pq-full" ? 0 : ef, &metrics);
                        if (mode == "sqlpq") {
                            if (k != 10) throw std::runtime_error("SQL audit LIMIT is 10");
                            vector<Value> values;
                            for (size_t d = 0; d < dim; ++d) values.push_back(Value::FLOAT(queries[q*dim+d]));
                            auto actual = Search(*statement, Value::ARRAY(LogicalType::FLOAT, std::move(values)));
                            if (std::set<row_t>(actual.begin(),actual.end()) != std::set<row_t>(ids.begin(),ids.end()))
                                throw std::runtime_error("SQL/API returned IDs disagree");
                            ids.assign(actual.begin(),actual.end());
                        }
                    }
                    else throw std::runtime_error("unknown mode");
                    if (ids.size() > k || (k == 10 && ids.size() != k) || std::set<row_t>(ids.begin(), ids.end()).size() != ids.size())
                        throw std::runtime_error("incomplete or duplicate result");
                    out << name << ',' << q << ',' << metrics.codes_scored << ',' << metrics.raw_vectors_refined << ',';
                    for (auto id : ids) out << id << ' ';
                    out << ',';
                    for (auto d : distances) out << d << ' ';
                    out << '\n';
                }
                std::cout << "DONE " << name << '\n' << std::flush;
            }
        }
        con.Rollback();
        std::cout << "COMPLETE untimed diagnostic; no throughput claim\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAILED " << e.what() << '\n';
        return 1;
    }
}
