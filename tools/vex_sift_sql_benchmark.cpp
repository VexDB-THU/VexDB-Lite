#include "duckdb.hpp"
#include "duckdb/main/appender.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace duckdb;

struct BenchmarkMetrics {
	double total_ms = 0;
	double avg_ms = 0;
	double p50_ms = 0;
	double p95_ms = 0;
	double qps = 0;
	double recall_at_10 = 0;
};

static std::vector<float> LoadFloatBin(const std::string &path, idx_t dim) {
	std::ifstream fs(path, std::ios::binary | std::ios::ate);
	if (!fs) {
		throw std::runtime_error("cannot open float data file: " + path);
	}
	auto size = static_cast<idx_t>(fs.tellg());
	fs.seekg(0);
	std::vector<float> data(size / sizeof(float));
	fs.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(size));
	if (data.size() % dim != 0) {
		throw std::runtime_error("invalid float data size for file: " + path);
	}
	return data;
}

static std::vector<int32_t> LoadIntBin(const std::string &path, idx_t cols) {
	std::ifstream fs(path, std::ios::binary | std::ios::ate);
	if (!fs) {
		throw std::runtime_error("cannot open int data file: " + path);
	}
	auto size = static_cast<idx_t>(fs.tellg());
	fs.seekg(0);
	std::vector<int32_t> data(size / sizeof(int32_t));
	fs.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(size));
	if (data.size() % cols != 0) {
		throw std::runtime_error("invalid int data size for file: " + path);
	}
	return data;
}

template <class T>
static void RequireOK(T &result, const std::string &sql) {
	if (!result || result->HasError()) {
		auto error = result ? result->GetError() : std::string("null query result");
		throw std::runtime_error("SQL failed: " + sql + "\n" + error);
	}
}

static void Exec(Connection &con, const std::string &sql) {
	auto result = con.Query(sql);
	RequireOK(result, sql);
}

static std::string EscapePath(const std::string &path) {
	std::string escaped;
	escaped.reserve(path.size() + 8);
	for (auto ch : path) {
		if (ch == '\\' || ch == '\'') {
			escaped.push_back('\\');
		}
		escaped.push_back(ch);
	}
	return escaped;
}

static Value MakeArrayValue(const float *vec, idx_t dim) {
	duckdb::vector<Value> values;
	values.reserve(dim);
	for (idx_t i = 0; i < dim; i++) {
		values.emplace_back(Value::FLOAT(vec[i]));
	}
	return Value::ARRAY(LogicalType::FLOAT, std::move(values));
}

static std::string MakeArrayLiteral(const float *vec, idx_t dim) {
	std::ostringstream ss;
	ss << "[";
	for (idx_t i = 0; i < dim; i++) {
		if (i > 0) {
			ss << ",";
		}
		ss << std::fixed << std::setprecision(6) << vec[i];
	}
	ss << "]::FLOAT[" << dim << "]";
	return ss.str();
}

static double Percentile(const std::vector<double> &sorted_values, double ratio) {
	if (sorted_values.empty()) {
		return 0;
	}
	auto pos = static_cast<idx_t>(ratio * static_cast<double>(sorted_values.size() - 1));
	return sorted_values[pos];
}

static double RecallAt10(const std::vector<int32_t> &retrieved, const int32_t *gt_topk) {
	idx_t hits = 0;
	for (auto id : retrieved) {
		for (idx_t i = 0; i < 10; i++) {
			if (id == gt_topk[i]) {
				hits++;
				break;
			}
		}
	}
	return static_cast<double>(hits) / 10.0;
}

static std::vector<int32_t> RunTopKQuery(Connection &con, const float *query_vec, idx_t dim, idx_t k) {
	auto sql = "SELECT id FROM sift ORDER BY l2_distance(vec, " + MakeArrayLiteral(query_vec, dim) +
	           ") LIMIT " + to_string(k);
	auto result = con.Query(sql);
	RequireOK(result, sql);

	std::vector<int32_t> ids;
	ids.reserve(k);
	for (auto &row : *result) {
		ids.push_back(row.GetValue<int32_t>(0));
	}
	return ids;
}

static BenchmarkMetrics BenchmarkQueries(Connection &con, const std::vector<float> &queries,
                                         const std::vector<int32_t> &gt,
                                         idx_t dim, idx_t k, idx_t query_count) {
	std::vector<double> latencies_ms;
	latencies_ms.reserve(query_count);
	double recall_sum = 0;

	auto start_all = std::chrono::high_resolution_clock::now();
	for (idx_t q = 0; q < query_count; q++) {
		auto qptr = queries.data() + q * dim;
		auto start = std::chrono::high_resolution_clock::now();
		auto ids = RunTopKQuery(con, qptr, dim, k);
		auto end = std::chrono::high_resolution_clock::now();
		auto elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
		latencies_ms.push_back(elapsed_ms);
		recall_sum += RecallAt10(ids, gt.data() + q * 100);
	}
	auto end_all = std::chrono::high_resolution_clock::now();
	auto total_ms = std::chrono::duration<double, std::milli>(end_all - start_all).count();

	sort(latencies_ms.begin(), latencies_ms.end());
	BenchmarkMetrics metrics;
	metrics.total_ms = total_ms;
	metrics.avg_ms = total_ms / static_cast<double>(query_count);
	metrics.p50_ms = Percentile(latencies_ms, 0.50);
	metrics.p95_ms = Percentile(latencies_ms, 0.95);
	metrics.qps = static_cast<double>(query_count) / (total_ms / 1000.0);
	metrics.recall_at_10 = recall_sum / static_cast<double>(query_count);
	return metrics;
}

static std::string ExplainFirstQuery(Connection &con, const std::vector<float> &queries, idx_t dim) {
	auto sql = "EXPLAIN SELECT id FROM sift ORDER BY l2_distance(vec, " +
	           MakeArrayLiteral(queries.data(), dim) + ") LIMIT 10";
	auto result = con.Query(sql);
	RequireOK(result, sql);

	std::ostringstream out;
	for (auto &row : *result) {
		out << row.GetValue<string>(0) << "\t" << row.GetValue<string>(1) << "\n";
	}
	return out.str();
}

int main(int argc, char **argv) {
	try {
		const idx_t dim = 128;
		const idx_t k = 10;
		const idx_t ann_query_count = 200;
		const idx_t exact_query_count = 20;

		auto repo_root = std::filesystem::current_path();
		auto data_dir = repo_root / "extension/vex/test/benchmark/data";
		auto extension_path = repo_root / "build/extension/vex/vex.duckdb_extension";

		if (argc > 1) {
			data_dir = std::filesystem::absolute(argv[1]);
		}
		if (argc > 2) {
			extension_path = std::filesystem::absolute(argv[2]);
		}

		auto train_path = data_dir / "sift_train_100k.fbin";
		auto query_path = data_dir / "sift_query_200.fbin";
		auto gt_path = data_dir / "sift_gt_100k_200q.ibin";

		auto train = LoadFloatBin(train_path.string(), dim);
		auto queries = LoadFloatBin(query_path.string(), dim);
		auto gt = LoadIntBin(gt_path.string(), 100);
		auto row_count = train.size() / dim;

		DuckDB db(nullptr);
		Connection con(db);

		auto thread_count = std::max<idx_t>(1, std::min<idx_t>(8, std::thread::hardware_concurrency()));
		Exec(con, "LOAD '" + EscapePath(extension_path.string()) + "'");
		Exec(con, "SET threads TO " + to_string(thread_count));
		Exec(con, "SET enable_progress_bar = false");
		Exec(con, "CREATE TABLE sift (id INTEGER, vec FLOAT[128])");

		std::cout << "SIFT benchmark data: " << train_path << "\n";
		std::cout << "Rows: " << row_count << ", Dim: " << dim << ", Queries: " << ann_query_count << "\n";
		std::cout << "DuckDB threads: " << thread_count << "\n";
		std::cout << "Extension: " << extension_path << "\n\n";

		auto insert_start = std::chrono::high_resolution_clock::now();
		Exec(con, "BEGIN TRANSACTION");
		{
			Appender appender(con, "sift");
			for (idx_t row = 0; row < row_count; row++) {
				auto vec_ptr = train.data() + row * dim;
				appender.BeginRow();
				appender.Append<int32_t>(NumericCast<int32_t>(row));
				appender.Append<Value>(MakeArrayValue(vec_ptr, dim));
				appender.EndRow();
			}
			appender.Close();
		}
		Exec(con, "COMMIT");
		auto insert_end = std::chrono::high_resolution_clock::now();
		auto insert_ms = std::chrono::duration<double, std::milli>(insert_end - insert_start).count();

		auto build_start = std::chrono::high_resolution_clock::now();
		Exec(con, "CREATE INDEX idx_sift ON sift USING GRAPH_INDEX (vec) WITH (metric='l2')");
		auto build_end = std::chrono::high_resolution_clock::now();
		auto build_ms = std::chrono::duration<double, std::milli>(build_end - build_start).count();

		auto explain_plan = ExplainFirstQuery(con, queries, dim);

		Exec(con, "RESET disabled_optimizers");
		auto ann_metrics = BenchmarkQueries(con, queries, gt, dim, k, ann_query_count);

		Exec(con, "SET disabled_optimizers='extension'");
		auto exact_metrics = BenchmarkQueries(con, queries, gt, dim, k, exact_query_count);
		Exec(con, "RESET disabled_optimizers");

		std::cout << std::fixed << std::setprecision(2);
		std::cout << "Import time:      " << insert_ms << " ms (" << (row_count / (insert_ms / 1000.0)) << " rows/s)\n";
		std::cout << "Index build time: " << build_ms << " ms (" << (row_count / (build_ms / 1000.0)) << " vec/s)\n";
		std::cout << "\nEXPLAIN (ANN query)\n" << explain_plan << "\n";

		std::cout << "ANN benchmark (" << ann_query_count << " queries, top-" << k << ")\n";
		std::cout << "  Total:    " << ann_metrics.total_ms << " ms\n";
		std::cout << "  Avg:      " << ann_metrics.avg_ms << " ms\n";
		std::cout << "  P50:      " << ann_metrics.p50_ms << " ms\n";
		std::cout << "  P95:      " << ann_metrics.p95_ms << " ms\n";
		std::cout << "  QPS:      " << ann_metrics.qps << "\n";
		std::cout << "  Recall@10:" << ann_metrics.recall_at_10 << "\n\n";

		std::cout << "Exact baseline (" << exact_query_count << " queries, optimizer disabled)\n";
		std::cout << "  Total:    " << exact_metrics.total_ms << " ms\n";
		std::cout << "  Avg:      " << exact_metrics.avg_ms << " ms\n";
		std::cout << "  P50:      " << exact_metrics.p50_ms << " ms\n";
		std::cout << "  P95:      " << exact_metrics.p95_ms << " ms\n";
		std::cout << "  QPS:      " << exact_metrics.qps << "\n";
		std::cout << "  Recall@10:" << exact_metrics.recall_at_10 << "\n";

		return 0;
	} catch (const std::exception &ex) {
		std::cerr << "benchmark failed: " << ex.what() << "\n";
		return 1;
	}
}
