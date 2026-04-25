#include "duckdb.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace duckdb;

namespace {

struct Timer {
	std::chrono::high_resolution_clock::time_point t;

	void Start() {
		t = std::chrono::high_resolution_clock::now();
	}

	double Ms() const {
		return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t).count();
	}
};

static unique_ptr<QueryResult> ExecOrThrow(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	if (!result) {
		throw std::runtime_error("query returned null result: " + sql);
	}
	if (result->HasError()) {
		result->ThrowError("query failed: " + sql + "\n");
	}
	return result;
}

static std::vector<float> LoadFloatBin(const string &path, idx_t dim) {
	std::ifstream fs(path, std::ios::binary | std::ios::ate);
	if (!fs) {
		throw std::runtime_error("cannot open: " + path);
	}
	auto size = static_cast<size_t>(fs.tellg());
	fs.seekg(0);
	if (size % (static_cast<size_t>(dim) * sizeof(float)) != 0) {
		throw std::runtime_error("unexpected float bin size: " + path);
	}

	std::vector<float> data(size / sizeof(float));
	fs.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(size));
	return data;
}

static std::vector<int32_t> LoadIntBin(const string &path, idx_t cols) {
	std::ifstream fs(path, std::ios::binary | std::ios::ate);
	if (!fs) {
		throw std::runtime_error("cannot open: " + path);
	}
	auto size = static_cast<size_t>(fs.tellg());
	fs.seekg(0);
	if (size % (static_cast<size_t>(cols) * sizeof(int32_t)) != 0) {
		throw std::runtime_error("unexpected int bin size: " + path);
	}

	std::vector<int32_t> data(size / sizeof(int32_t));
	fs.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(size));
	return data;
}

static Value MakeArrayValue(const float *vec, idx_t dim) {
	std::vector<Value> values;
	values.reserve(dim);
	for (idx_t i = 0; i < dim; i++) {
		values.emplace_back(Value::FLOAT(vec[i]));
	}
	return Value::ARRAY(LogicalType::FLOAT, std::move(values));
}

static string MakeQueryArrayLiteral(const float *vec, idx_t dim) {
	std::ostringstream oss;
	oss << '[';
	oss << std::fixed << std::setprecision(6);
	for (idx_t i = 0; i < dim; i++) {
		if (i) {
			oss << ", ";
		}
		oss << vec[i];
	}
	oss << "]::FLOAT[" << dim << ']';
	return oss.str();
}

static std::vector<int32_t> ReadTopIds(Connection &con, const string &sql, idx_t k) {
	auto result = ExecOrThrow(con, sql);
	auto &mat = result->Cast<MaterializedQueryResult>();

	std::vector<int32_t> ids;
	ids.reserve(k);
	for (idx_t row = 0; row < mat.RowCount() && row < k; row++) {
		ids.push_back(IntegerValue::Get(mat.GetValue(0, row).DefaultCastAs(LogicalType::INTEGER)));
	}
	return ids;
}

static string ReadExplain(Connection &con, const string &sql) {
	auto result = ExecOrThrow(con, sql);
	auto &mat = result->Cast<MaterializedQueryResult>();

	string plan;
	for (idx_t row = 0; row < mat.RowCount(); row++) {
		for (idx_t col = 0; col < mat.ColumnCount(); col++) {
			plan += mat.GetValue(col, row).ToString();
			plan += '\n';
		}
	}
	return plan;
}

static double RecallAtK(const std::vector<int32_t> &ids, const int32_t *gt, idx_t k) {
	idx_t hits = 0;
	for (idx_t i = 0; i < ids.size() && i < k; i++) {
		for (idx_t j = 0; j < k; j++) {
			if (ids[i] == gt[j]) {
				hits++;
				break;
			}
		}
	}
	return static_cast<double>(hits) / static_cast<double>(k);
}

static void PrintDivider() {
	std::cout << "============================================================\n";
}

static void RunOne(const string &data_dir, bool use_100k, const string &extension_path) {
	static constexpr idx_t D = 128;
	static constexpr idx_t QUERY_COUNT = 200;
	static constexpr idx_t GT_K = 100;

	const string train_file = data_dir + (use_100k ? "/sift_train_100k.fbin" : "/sift_train_10k.fbin");
	const string query_file = data_dir + "/sift_query_200.fbin";
	const string gt_file = data_dir + (use_100k ? "/sift_gt_100k_200q.ibin" : "/sift_gt_10k_200q.ibin");

	auto train = LoadFloatBin(train_file, D);
	auto queries = LoadFloatBin(query_file, D);
	auto gt = LoadIntBin(gt_file, GT_K);

	const idx_t n = train.size() / D;
	const idx_t nq = queries.size() / D;
	if (nq != QUERY_COUNT) {
		throw std::runtime_error("unexpected query count");
	}

	DBConfig config;
	config.SetOptionByName("allow_unsigned_extensions", true);
	DuckDB db(nullptr, &config);
	Connection con(db);

	ExecOrThrow(con, "LOAD '" + extension_path + "'");
	ExecOrThrow(con, "SET threads TO 4");
	ExecOrThrow(con, "PRAGMA explain_output='all'");
	ExecOrThrow(con, "CREATE TABLE sift (id INTEGER, vec FLOAT[128])");

	PrintDivider();
	std::cout << "SIFT SQL Benchmark: " << (use_100k ? "100k" : "10k") << " vectors, " << nq << " queries\n";
	PrintDivider();

	Timer timer;
	timer.Start();
	{
		Appender app(con, "sift");
		for (idx_t i = 0; i < n; i++) {
			app.BeginRow();
			app.Append(static_cast<int32_t>(i));
			app.Append(MakeArrayValue(train.data() + i * D, D));
			app.EndRow();

			if ((i + 1) % 20000 == 0 || i + 1 == n) {
				std::cout << "loaded rows: " << (i + 1) << "/" << n << std::endl;
			}
		}
		app.Close();
	}
	const auto load_ms = timer.Ms();

	timer.Start();
	ExecOrThrow(con, "CREATE INDEX idx_sift ON sift USING GRAPH_INDEX (vec) WITH (metric='l2')");
	const auto build_ms = timer.Ms();

	const string first_query_vec = MakeQueryArrayLiteral(queries.data(), D);
	const string first_query = "SELECT id FROM sift ORDER BY l2_distance(vec, " + first_query_vec + ") LIMIT 10";
	const string explain_plan = ReadExplain(con, "EXPLAIN " + first_query);
	const bool uses_index = explain_plan.find("VEX_INDEX_SCAN") != string::npos;

	double recall10 = 0.0;
	double recall100 = 0.0;
	timer.Start();
	for (idx_t q = 0; q < nq; q++) {
		const string query_vec = MakeQueryArrayLiteral(queries.data() + q * D, D);
		const string sql = "SELECT id FROM sift ORDER BY l2_distance(vec, " + query_vec + ") LIMIT 100";
		auto ids = ReadTopIds(con, sql, 100);
		recall10 += RecallAtK(ids, gt.data() + q * GT_K, 10);
		recall100 += RecallAtK(ids, gt.data() + q * GT_K, 100);
	}
	const auto query_ms = timer.Ms();
	const auto qps = static_cast<double>(nq) / (query_ms / 1000.0);

	std::cout << "load_ms=" << load_ms << std::endl;
	std::cout << "build_ms=" << build_ms << std::endl;
	std::cout << "query_ms=" << query_ms << std::endl;
	std::cout << "qps=" << qps << std::endl;
	std::cout << "recall@10=" << (recall10 / nq) << std::endl;
	std::cout << "recall@100=" << (recall100 / nq) << std::endl;
	std::cout << "uses_vex_index_scan=" << (uses_index ? "true" : "false") << std::endl;
	std::cout << "first_explain_has_vex_index_scan=" << (uses_index ? "yes" : "no") << std::endl;
	if (!uses_index) {
		std::cout << "----- EXPLAIN -----\n" << explain_plan;
	}
	std::cout << std::endl;
}

} // namespace

int main(int argc, char **argv) {
	string data_dir = "vexdb-duck/test/benchmark/data";
	string extension_path = "build/standalone-v144/_duckdb/extension/vex/vex.duckdb_extension";
	string dataset = "both";

	if (argc > 1) {
		data_dir = argv[1];
	}
	if (argc > 2) {
		dataset = argv[2];
	}
	if (argc > 3) {
		extension_path = argv[3];
	}

	try {
		if (dataset == "10k") {
			RunOne(data_dir, false, extension_path);
		} else if (dataset == "100k") {
			RunOne(data_dir, true, extension_path);
		} else if (dataset == "both") {
			RunOne(data_dir, false, extension_path);
			RunOne(data_dir, true, extension_path);
		} else {
			throw std::runtime_error("dataset must be one of: 10k, 100k, both");
		}
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << "FAIL: " << ex.what() << std::endl;
		return 1;
	}
}
