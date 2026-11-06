#include "vex_graph_index.hpp"
#define main quantization_build_benchmark_main
#include "vex_quantization_product_benchmark.cpp"
#undef main
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include <map>
#include <set>

// Development-only candidate acquisition diagnostic. No throughput is measured.
// Base vectors below score a union of actual returned IDs; no truth ID is ever
// added to that union. Product SQL speed must be measured separately.
int main(int argc, char **argv) {
    if (argc != 5 || std::filesystem::exists(argv[4])) {
        std::cerr << "usage: probe <extension> <saved_db> <data_dir> <new_output>\n";
        return 2;
    }
    try {
        constexpr size_t dim = 1536, nq = 200, n = 100000;
        const auto base = Read<float>(std::string(argv[3]) + "/base.f32", n * dim);
        const auto queries = Read<float>(std::string(argv[3]) + "/queries.f32", nq * dim);
        const auto truth = Read<int32_t>(std::string(argv[3]) + "/truth-100000.i32", nq * 10);
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", true);
        config.SetOptionByName("access_mode", Value("read_only"));
        config.SetOptionByName("memory_limit", Value("4GB"));
        DuckDB db(argv[2], &config);
        Connection con(db);
        Exec(con, "LOAD " + Quote(std::filesystem::absolute(argv[1]).string()));
        Configure(con, "pq_graph", 256, 16);
        Exec(con, "SELECT * FROM vexdb_index_info()");
        con.BeginTransaction();
        auto &table = Catalog::GetEntry(*con.context, CatalogType::TABLE_ENTRY, INVALID_CATALOG,
                                        DEFAULT_SCHEMA, "vectors").Cast<TableCatalogEntry>().Cast<DuckTableEntry>();
        auto &indexes = table.GetStorage().GetDataTableInfo()->GetIndexes();
        std::filesystem::create_directories(argv[4]);
        std::ofstream out(std::filesystem::path(argv[4]) / "ids.csv");
        out << "point,query,hits,codes_scored,raw_vectors_refined,ids\n";
        std::map<std::string, std::vector<std::vector<row_t>>> returned;
        bool found = false;
        auto save = [&](const std::string &name, size_t q, const std::vector<row_t> &ids,
                        idx_t codes, idx_t refined) {
            if (ids.size() != 10 || std::set<row_t>(ids.begin(), ids.end()).size() != 10) {
                throw std::runtime_error("invalid native result cardinality");
            }
            int hits = 0;
            for (auto id : ids) {
                for (int j = 0; j < 10; ++j) if (id == truth[q * 10 + j]) { ++hits; break; }
            }
            out << name << ',' << q << ',' << hits << ',' << codes << ',' << refined << ',';
            for (auto id : ids) out << id << ' ';
            out << '\n';
            returned[name].push_back(ids);
            return hits;
        };
        for (auto &index : indexes.Indexes()) {
            if (!index.IsBound() || index.GetIndexType() != GraphIndex::TYPE_NAME) continue;
            auto &graph = index.Cast<BoundIndex>().Cast<GraphIndex>();
            for (bool pq : {false, true}) {
                for (int ef : pq ? std::vector<int>{256,512,768,1024,1536}
                                 : std::vector<int>{40,64,128,256,512,768}) {
                    const auto name = std::string(pq ? "pq-ef" : "raw-ef") + std::to_string(ef);
                    size_t hits = 0, codes = 0;
                    for (size_t q = 0; q < nq; ++q) {
                        std::vector<row_t> ids;
                        std::vector<float> distances;
                        PQSearchMetrics metrics;
                        if (pq) graph.SearchPQ(queries.data() + q * dim, 10, ids, distances, 16, ef, &metrics);
                        else graph.SearchANN(queries.data() + q * dim, 10, ef, ids, distances);
                        hits += save(name, q, ids, metrics.codes_scored, metrics.raw_vectors_refined);
                        codes += metrics.codes_scored;
                    }
                    std::cout << name << " hits=" << hits << "/2000 mean_codes=" << double(codes)/nq << std::endl;
                }
            }
            for (int pq_ef : {256,512}) {
                for (int raw_ef : {40,64}) {
                    const auto name = "union-pq" + std::to_string(pq_ef) + "-raw" + std::to_string(raw_ef);
                    size_t hits = 0;
                    for (size_t q = 0; q < nq; ++q) {
                        std::set<row_t> pool;
                        for (const auto &source : {"pq-ef" + std::to_string(pq_ef), "raw-ef" + std::to_string(raw_ef)}) {
                            for (auto id : returned.at(source).at(q)) pool.insert(id);
                        }
                        std::vector<std::pair<double,row_t>> ranked;
                        for (auto id : pool) {
                            double distance = 0;
                            for (size_t d = 0; d < dim; ++d) {
                                const double delta = double(queries[q*dim+d]) - double(base[size_t(id)*dim+d]);
                                distance += delta * delta;
                            }
                            ranked.emplace_back(distance,id);
                        }
                        std::sort(ranked.begin(),ranked.end());
                        std::vector<row_t> ids;
                        for (size_t j = 0; j < 10; ++j) ids.push_back(ranked[j].second);
                        hits += save(name,q,ids,0,pool.size());
                    }
                    std::cout << name << " hits=" << hits << "/2000 (cached merge diagnostic only)" << std::endl;
                }
            }
            found = true;
        }
        con.Rollback();
        if (!found) throw std::runtime_error("bound graph missing");
        std::cout << "COMPLETE diagnostic; no SQL QPS measured\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAILED " << e.what() << '\n';
        return 1;
    }
}
