// Native SQL path/quality preflight, deliberately not a throughput benchmark.
// Metadata and returned bodies are explicit proxies, never customer records.
#define main historical_product_benchmark_main
#include "vex_quantization_product_benchmark.cpp"
#undef main
#include <set>

namespace {
struct BusinessCase {
    std::string name, predicate;
    int filter;
    bool payload = true, joined = false;
    int offset = 0;
};
const std::vector<BusinessCase> cases {
    {"plain_ids", "", 0, false},
    {"plain_payload", "", 0},
    {"rag_tenant_public", "b.tenant_id=$2 AND b.is_public", 1},
    {"rag_acl_recent", "b.tenant_id=$2 AND b.is_public AND b.updated_day >= $3", 2},
    {"rag_broad_public", "b.is_public", 3},
    {"rag_empty_tenant", "b.tenant_id=$2", 4},
    {"catalog_category_stock_price", "b.category_id=$2 AND b.in_stock AND b.price_cents BETWEEN $3 AND $4", 5},
    {"catalog_sparse", "b.id >= $2", 6},
    {"joined_tenant_payload", "m.tenant_id=$2 AND m.is_public", 1, true, true},
    {"joined_unfiltered_payload", "", 0, true, true},
    {"rag_pagination", "b.tenant_id=$2 AND b.is_public", 1, true, false, 10},
};

vector<Value> Parameters(const BusinessCase &c, const Value &query, size_t q, size_t rows) {
    vector<Value> p {query};
    if (c.filter == 1 || c.filter == 2 || c.filter == 4) {
        p.push_back(Value::INTEGER(c.filter == 4 ? -1 : int(q % 100)));
        if (c.filter == 2) p.push_back(Value::INTEGER(300));
    } else if (c.filter == 5) {
        p.push_back(Value::INTEGER(int((q + 9) % 10)));
        p.push_back(Value::INTEGER(1000));
        p.push_back(Value::INTEGER(9000));
    } else if (c.filter == 6) {
        p.push_back(Value::INTEGER(int(rows - 3)));
    }
    return p;
}
bool Eligible(const BusinessCase &c, size_t id, size_t q, size_t rows) {
    switch (c.filter) {
    case 0: return true;
    case 1: return id % 100 == q % 100 && id % 7 != 0;
    case 2: return id % 100 == q % 100 && id % 7 != 0 && id % 365 >= 300;
    case 3: return id % 7 != 0;
    case 4: return false;
    case 5: return id * 10 / rows == (q + 9) % 10 && id % 5 != 0 &&
                   100 + id % 10000 >= 1000 && 100 + id % 10000 <= 9000;
    case 6: return id >= rows - 3;
    default: throw std::runtime_error("unknown fixture predicate");
    }
}
std::string BusinessSQL(const BusinessCase &c, int dim) {
    return "SELECT b.id" + (c.payload ? std::string(c.joined ? ",m.body" : ",b.body") : "") +
        (c.joined ? " FROM vectors b JOIN business_metadata m ON b.id=m.id" : " FROM business b") +
        (c.predicate.empty() ? "" : " WHERE " + c.predicate) +
        " ORDER BY l2_distance(b.vec,$1::FLOAT[" + std::to_string(dim) + "]) LIMIT 10" +
        (c.offset ? " OFFSET " + std::to_string(c.offset) : "");
}
std::string Body(int32_t id) {
    const auto part = "proxy-payload-" + std::to_string(id) + " ";
    std::string body;
    for (int i = 0; i < 32; ++i) body += part;
    return body;
}
uint64_t Admitted(Connection &con) {
    return Exec(con, "SELECT (vexdb_search_queue_state()).admitted")->GetValue(0, 0).GetValue<uint64_t>();
}
void Require(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
} // namespace

int main(int argc, char **argv) {
    if (argc < 8 || argc > 12 || std::filesystem::exists(argv[7])) return 2;
    try {
        const int dim = std::stoi(argv[5]), count = std::stoi(argv[6]);
        const int raw_ef = argc == 12 ? std::stoi(argv[11]) : 1536;
        if (raw_ef < 1 || raw_ef > 65535) throw std::runtime_error("invalid raw control width");
        std::set<std::string> selected_cases;
        if (argc >= 11) {
            std::istringstream input(argv[10]); std::string name;
            while (std::getline(input, name, ',')) {
                if (name.empty() || !selected_cases.insert(name).second ||
                    std::none_of(cases.begin(), cases.end(), [&](const BusinessCase &c) { return c.name == name; }))
                    throw std::runtime_error("invalid case selection");
            }
            if (selected_cases.empty()) throw std::runtime_error("empty case selection");
        }
        Require(dim > 0 && count > 0 && count <= 512, "invalid shape");
        const auto bytes = std::filesystem::file_size(argv[3]);
        Require(bytes % (sizeof(float) * dim) == 0, "invalid base matrix length");
        const size_t rows = bytes / (sizeof(float) * dim);
        Require(rows > 10 && rows < size_t(INT32_MAX), "invalid row count");
        auto base = Read<float>(argv[3], rows * dim);
        auto input = Read<float>(argv[4], size_t(count) * dim);
        std::vector<Value> queries;
        std::vector<std::vector<double>> distances(count, std::vector<double>(rows));
        for (int q = 0; q < count; ++q) {
            vector<Value> v;
            for (int d = 0; d < dim; ++d) {
                Require(std::isfinite(input[q * size_t(dim) + d]), "nonfinite query");
                v.push_back(Value::FLOAT(input[q * size_t(dim) + d]));
            }
            queries.push_back(Value::ARRAY(LogicalType::FLOAT, std::move(v)));
            for (size_t id = 0; id < rows; ++id) {
                // Independent float64 full-vector L2, not the extension's
                // SIMD distance, PQ estimate, graph, or SQL distance function.
                double a = 0, b = 0, c = 0, e = 0;
                int d = 0;
                for (; d + 3 < dim; d += 4) {
                    double x0 = double(base[id * dim + d]) - input[q * size_t(dim) + d];
                    double x1 = double(base[id * dim + d + 1]) - input[q * size_t(dim) + d + 1];
                    double x2 = double(base[id * dim + d + 2]) - input[q * size_t(dim) + d + 2];
                    double x3 = double(base[id * dim + d + 3]) - input[q * size_t(dim) + d + 3];
                    a += x0 * x0; b += x1 * x1; c += x2 * x2; e += x3 * x3;
                }
                for (; d < dim; ++d) {
                    double x = double(base[id * dim + d]) - input[q * size_t(dim) + d];
                    a += x * x;
                }
                distances[q][id] = a + b + c + e;
                Require(std::isfinite(distances[q][id]), "nonfinite base/distance");
            }
        }
        const std::filesystem::path out = argv[7];
        std::filesystem::create_directories(out);
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", true);
        config.SetOptionByName("access_mode", Value("read_only"));
        config.SetOptionByName("memory_limit", Value("4GB"));
        DuckDB db(argv[2], &config);
        Connection con(db);
        Exec(con, "LOAD " + Quote(std::filesystem::absolute(argv[1]).string()));
        if (argc >= 9) Exec(con, "SET vexdb_filtered_search_mode=" + Quote(argv[8]));
        if (argc >= 10) {
            if (std::string(argv[9]) != "on" && std::string(argv[9]) != "off") throw std::runtime_error("invalid graph direction");
            Exec(con, "SET vexdb_graph_bidirectional_search=" + std::string(argv[9] == std::string("on") ? "true" : "false"));
        }
        Exec(con, "SET threads=1");
        auto shape = Exec(con, "SELECT count(*),min(id),max(id),count(DISTINCT id) FROM vectors");
        Require(shape->GetValue(0, 0).GetValue<uint64_t>() == rows &&
                shape->GetValue(1, 0).GetValue<int64_t>() == 0 &&
                shape->GetValue(2, 0).GetValue<uint64_t>() == rows - 1 &&
                shape->GetValue(3, 0).GetValue<uint64_t>() == rows, "ID/base alignment mismatch");
        for (auto id : {size_t(0), rows / 2, rows - 1}) {
            auto value = Exec(con, "SELECT vec[1] FROM vectors WHERE id=" + std::to_string(id));
            Require(value->GetValue(0, 0).GetValue<float>() == base[id * dim], "base sample mismatch");
        }
        std::ofstream(out / "index-info.txt") << Exec(con, "SELECT * FROM vexdb_index_info()")->ToString();
        // IDs are fixture identifiers. These expressions define synthetic
        // metadata only and never enter product search decisions.
        Exec(con, "CREATE TEMP VIEW business AS SELECT id,vec,(id%100)::INTEGER tenant_id,"
                  "(id::BIGINT*10//" + std::to_string(rows) + ")::INTEGER category_id,"
                  "id%7<>0 is_public,id%5<>0 in_stock,(100+id%10000)::INTEGER price_cents,"
                  "(id%365)::INTEGER updated_day,repeat('proxy-payload-'||id::VARCHAR||' ',32) body FROM vectors");
        Exec(con, "CREATE TEMP TABLE business_metadata AS SELECT id,tenant_id,is_public,body FROM business");
        std::ofstream raw(out / "requests.csv");
        raw << "case,mode,limit,query,index_plan,eligible,expected_k,returned,hits,illegal,payload_ok,admitted_delta,ids,truth\n";
        for (const auto &c : cases) {
            if (!selected_cases.empty() && !selected_cases.count(c.name)) continue;
            const auto sql = BusinessSQL(c, dim);
            std::ofstream(out / (c.name + ".sql")) << sql << ";\n";
            for (const auto &mode : {"off", "pq_graph"}) for (int limit : {0, 1}) {
                Configure(con, mode, std::string(mode) == "off" ? raw_ef : 1536, 16);
                Exec(con, "SET GLOBAL vexdb_search_concurrency=" + std::to_string(limit));
                auto statement = con.Prepare(sql), explain = con.Prepare("EXPLAIN " + sql);
                Require(!statement->HasError() && !explain->HasError(), "prepare failed: " + sql);
                for (int q = 0; q < count; ++q) {
                    auto params = Parameters(c, queries[q], q, rows);
                    auto plan = explain->Execute(params, false);
                    Require(plan && !plan->HasError(), "explain failed");
                    const auto text = plan->ToString();
                    const bool indexed = text.find("VEXDB_INDEX_SCAN") != std::string::npos;
                    std::ofstream(out / (c.name + "-" + mode + "-" + std::to_string(limit) + "-q" + std::to_string(q) + ".plan.txt")) << text;
                    std::vector<int32_t> truth;
                    for (size_t id = 0; id < rows; ++id) if (Eligible(c, id, q, rows)) truth.push_back(int32_t(id));
                    const auto eligible = truth.size();
                    const auto prefix = std::min(truth.size(), size_t(c.offset + 10));
                    std::partial_sort(truth.begin(), truth.begin() + prefix, truth.end(), [&](int32_t a, int32_t b) {
                        return distances[q][a] == distances[q][b] ? a < b : distances[q][a] < distances[q][b];
                    });
                    truth.resize(prefix);
                    truth.erase(truth.begin(), truth.begin() + std::min(truth.size(), size_t(c.offset)));
                    const auto before = Admitted(con);
                    auto result = statement->Execute(params, false);
                    Require(result && !result->HasError(), "execution failed: " + sql);
                    auto &mat = result->Cast<MaterializedQueryResult>();
                    const auto delta = Admitted(con) - before;
                    std::set<int32_t> ids;
                    int hits = 0, illegal = 0;
                    bool payload_ok = true;
                    for (idx_t r = 0; r < mat.RowCount(); ++r) {
                        const int32_t id = mat.GetValue(0, r).GetValue<int32_t>();
                        Require(ids.insert(id).second, "duplicate ID");
                        const bool valid = id >= 0 && size_t(id) < rows && Eligible(c, id, q, rows);
                        illegal += !valid;
                        hits += std::find(truth.begin(), truth.end(), id) != truth.end();
                        if (c.payload) payload_ok &= mat.GetValue(1, r).GetValue<std::string>() == Body(id);
                    }
                    raw << c.name << ',' << mode << ',' << limit << ',' << q << ',' << indexed << ',' << eligible << ','
                        << truth.size() << ',' << ids.size() << ',' << hits << ',' << illegal << ',' << payload_ok << ',' << delta << ',';
                    for (auto id : ids) raw << id << ' ';
                    raw << ',';
                    for (auto id : truth) raw << id << ' ';
                    raw << '\n' << std::flush;
                    Require(delta == uint64_t(indexed && eligible != 0), "queue path differs from actual plan/nonempty work");
                    Require(!illegal && payload_ok && ids.size() == truth.size(), "business result contract failed");
                }
                std::cout << "CHECK " << c.name << ' ' << mode << " limit=" << limit << std::endl;
            }
        }
        std::ofstream(out / "queue-final.txt") << Exec(con, "SELECT vexdb_search_queue_state()")->ToString();
        std::cout << "DONE native business query preflight; no throughput or production-capacity claim" << std::endl;
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAILED " << e.what() << '\n'; return 1; }
}
