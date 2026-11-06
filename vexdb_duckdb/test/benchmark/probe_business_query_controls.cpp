// Counterfactual controls for the business SQL preflight, not a speed test.
#define main historical_product_benchmark_main
#include "vex_quantization_product_benchmark.cpp"
#undef main

int main(int argc, char **argv) {
    if (argc != 6 || std::filesystem::exists(argv[5])) return 2;
    try {
        const int dim = std::stoi(argv[4]);
        const auto input = Read<float>(argv[3], size_t(dim) * 8);
        std::vector<Value> queries;
        for (int q = 0; q < 8; ++q) {
            vector<Value> v;
            for (int d = 0; d < dim; ++d) v.push_back(Value::FLOAT(input[q * size_t(dim) + d]));
            queries.push_back(Value::ARRAY(LogicalType::FLOAT, std::move(v)));
        }
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", true);
        config.SetOptionByName("access_mode", Value("read_only"));
        DuckDB db(argv[2], &config);
        Connection con(db);
        Exec(con, "LOAD " + Quote(std::filesystem::absolute(argv[1]).string()));
        const std::filesystem::path out = argv[5];
        std::filesystem::create_directories(out);
        std::ofstream raw(out / "requests.csv");
        raw << "case,mode,query,index_plan,admitted_delta,ids\n";
        const auto distance = "l2_distance(vec,$1::FLOAT[" + std::to_string(dim) + "])";
        const auto plain = "SELECT id FROM vectors ORDER BY " + distance + " LIMIT 10";
        const auto view = "SELECT id FROM business ORDER BY " + distance + " LIMIT 10";
        auto probe = [&](const std::string &name, const std::string &sql) {
            std::ofstream(out / (name + ".sql")) << sql << ";\n";
            for (const auto &mode : {"off", "pq_graph"}) {
                Configure(con, mode, 1536, 16);
                Exec(con, "SET GLOBAL vexdb_search_concurrency=0");
                auto statement = con.Prepare(sql), explain = con.Prepare("EXPLAIN " + sql);
                if (statement->HasError() || explain->HasError()) throw std::runtime_error("prepare failed");
                for (int q = 0; q < 8; ++q) {
                    vector<Value> params {queries[q]};
                    auto plan = explain->Execute(params, false);
                    if (plan->HasError()) throw std::runtime_error(plan->GetError());
                    const auto text = plan->ToString();
                    std::ofstream(out / (name + "-" + mode + "-q" + std::to_string(q) + ".plan.txt")) << text;
                    const auto before = Exec(con, "SELECT (vexdb_search_queue_state()).admitted")->GetValue(0, 0).GetValue<uint64_t>();
                    auto result = statement->Execute(params, false);
                    if (result->HasError()) throw std::runtime_error(result->GetError());
                    auto &mat = result->Cast<MaterializedQueryResult>();
                    const auto after = Exec(con, "SELECT (vexdb_search_queue_state()).admitted")->GetValue(0, 0).GetValue<uint64_t>();
                    raw << name << ',' << mode << ',' << q << ',' << (text.find("VEXDB_INDEX_SCAN") != std::string::npos) << ',' << after - before << ',';
                    for (idx_t r = 0; r < mat.RowCount(); ++r) raw << mat.GetValue(0, r).GetValue<int32_t>() << ' ';
                    raw << '\n' << std::flush;
                }
            }
        };
        probe("direct_before_views", plain);
        Exec(con, "CREATE TEMP VIEW business AS SELECT id,vec,(id%100)::INTEGER tenant_id,"
                  "repeat('proxy-payload-'||id::VARCHAR||' ',32) body FROM vectors");
        probe("direct_after_view", plain);
        probe("view_ids_before_temp_write", view);
        probe("direct_payload_before_temp_write", "SELECT id,repeat('proxy-payload-'||id::VARCHAR||' ',32) body FROM vectors ORDER BY " + distance + " LIMIT 10");
        Exec(con, "CREATE TEMP TABLE business_metadata AS SELECT id,tenant_id,body FROM business");
        probe("direct_after_temp_write", plain);
        probe("view_ids_after_temp_write", view);
        std::cout << "DONE six counterfactual SQL shapes; no throughput claim\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAILED " << e.what() << '\n'; return 1; }
}
