#include "vex_graph_index.hpp"

#define main quantization_build_benchmark_main
#include "vex_quantization_product_benchmark.cpp"
#undef main

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"

#include <set>

int main(int argc, char **argv) {
    if (argc != 5 || std::filesystem::exists(argv[4])) {
        std::cerr << "usage: audit_text_candidate_pools <extension> <saved_db> <data_dir> <new_output>\n";
        return 2;
    }
    try {
        constexpr size_t dim = 1536, nq = 200;
        const auto floats = Read<float>(std::string(argv[3]) + "/queries.f32", nq * dim);
        const auto truth = Read<int32_t>(std::string(argv[3]) + "/truth-100000.i32", nq * 10);
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", true);
        config.SetOptionByName("access_mode", Value("read_only"));
        config.SetOptionByName("memory_limit", Value("4GB"));
        DuckDB db(argv[2], &config);
        Connection con(db);
        Exec(con, "LOAD " + Quote(std::filesystem::absolute(argv[1]).string()));
        Configure(con, "pq_only", 256, 16);
        Exec(con, "SELECT * FROM vexdb_index_info()");
        con.BeginTransaction();
        auto &table = Catalog::GetEntry(*con.context, CatalogType::TABLE_ENTRY, INVALID_CATALOG,
                                        DEFAULT_SCHEMA, "vectors").Cast<TableCatalogEntry>().Cast<DuckTableEntry>();
        auto &indexes = table.GetStorage().GetDataTableInfo()->GetIndexes();
        std::filesystem::create_directories(argv[4]);
        std::ofstream output(std::filesystem::path(argv[4]) / "pools.csv");
        output << "point,query,codes_scored,pool_hits,final_hits,missing_from_pool,in_pool_not_returned,final_ids,pool_ids\n";
        bool found = false;
        for (auto &index : indexes.Indexes()) {
            if (!index.IsBound() || index.GetIndexType() != GraphIndex::TYPE_NAME) continue;
            auto &graph = index.Cast<BoundIndex>().Cast<GraphIndex>();
            for (const auto &point : std::vector<std::pair<std::string, int>> {
                     {"full-r16", 0}, {"pq-ef256-r16", 256}, {"pq-ef512-r16", 512}}) {
                size_t missed = 0, rerank_missed = 0;
                for (size_t q = 0; q < nq; ++q) {
                    std::vector<row_t> pool, final;
                    std::vector<float> approximate, exact;
                    PQSearchMetrics pool_metrics, final_metrics;
                    // k=160, factor=1 reaches the same PQ heap as k=10,
                    // factor=16; target k only affects the later refinement.
                    graph.SearchPQ(floats.data() + q * dim, 160, pool, approximate, 1.0, point.second, &pool_metrics);
                    graph.SearchPQ(floats.data() + q * dim, 10, final, exact, 16.0, point.second, &final_metrics);
                    const std::set<row_t> pool_set(pool.begin(), pool.end()), final_set(final.begin(), final.end());
                    if (pool_set.size() != 160 || final_set.size() != 10 ||
                        pool_metrics.codes_scored != final_metrics.codes_scored ||
                        pool_metrics.raw_vectors_refined != 0 || final_metrics.raw_vectors_refined != 160) {
                        throw std::runtime_error("candidate-pool equivalence or size check failed");
                    }
                    for (auto id : final_set) {
                        if (!pool_set.count(id)) throw std::runtime_error("final row is outside its candidate pool");
                    }
                    int pool_hits = 0, final_hits = 0, missing = 0, rank_loss = 0;
                    for (int k = 0; k < 10; ++k) {
                        const auto id = truth[q * 10 + k];
                        const bool in_pool = pool_set.count(id), in_final = final_set.count(id);
                        pool_hits += in_pool;
                        final_hits += in_final;
                        missing += !in_pool;
                        rank_loss += in_pool && !in_final;
                    }
                    output << point.first << ',' << q << ',' << pool_metrics.codes_scored << ','
                           << pool_hits << ',' << final_hits << ',' << missing << ',' << rank_loss << ',';
                    for (auto id : final) output << id << ' ';
                    output << ',';
                    for (auto id : pool) output << id << ' ';
                    output << '\n';
                    missed += missing;
                    rerank_missed += rank_loss;
                }
                std::cout << point.first << " missing_from_pool=" << missed << " lost_in_refinement=" << rerank_missed << '\n';
            }
            found = true;
        }
        con.Rollback();
        if (!found) throw std::runtime_error("bound graph index missing");
        std::cout << "COMPLETE untimed candidate-pool audit; no QPS measured\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAILED " << e.what() << '\n';
        return 1;
    }
}
