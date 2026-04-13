#include "duckdb.hpp"
#include "duckdb/main/appender.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace duckdb;

namespace {

using Clock = std::chrono::steady_clock;

struct OpSummary {
	std::string name;
	std::vector<double> latencies_ms;
	idx_t rows = 0;
	double total_ms = 0;
};

struct WindowStats {
	double window_end_sec = 0;
	idx_t insert_rows = 0;
	idx_t delete_rows = 0;
	idx_t update_rows = 0;
	double insert_ms = 0;
	double delete_ms = 0;
	double update_ms = 0;
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
	vector<Value> values;
	values.reserve(dim);
	for (idx_t i = 0; i < dim; i++) {
		values.emplace_back(Value::FLOAT(vec[i]));
	}
	return Value::ARRAY(LogicalType::FLOAT, std::move(values));
}

static double Percentile(std::vector<double> values, double ratio) {
	if (values.empty()) {
		return 0;
	}
	std::sort(values.begin(), values.end());
	auto pos = static_cast<idx_t>(ratio * static_cast<double>(values.size() - 1));
	return values[pos];
}

static std::string ZeroArrayLiteral(idx_t dim) {
	std::ostringstream ss;
	ss << "[";
	for (idx_t i = 0; i < dim; i++) {
		if (i > 0) {
			ss << ",";
		}
		ss << "0.0";
	}
	ss << "]::FLOAT[" << dim << "]";
	return ss.str();
}

static void PrintSummary(const OpSummary &summary) {
	double rows_per_sec = summary.total_ms > 0 ? static_cast<double>(summary.rows) / (summary.total_ms / 1000.0) : 0;
	double ops_per_sec =
	    summary.total_ms > 0 ? static_cast<double>(summary.latencies_ms.size()) / (summary.total_ms / 1000.0) : 0;
	double avg_ms =
	    summary.latencies_ms.empty() ? 0 : summary.total_ms / static_cast<double>(summary.latencies_ms.size());
	std::cout << summary.name << " summary\n";
	std::cout << "  batches: " << summary.latencies_ms.size() << ", rows: " << summary.rows << "\n";
	std::cout << "  avg: " << std::fixed << std::setprecision(3) << avg_ms
	          << " ms, p50: " << Percentile(summary.latencies_ms, 0.50)
	          << " ms, p95: " << Percentile(summary.latencies_ms, 0.95)
	          << " ms, p99: " << Percentile(summary.latencies_ms, 0.99) << " ms\n";
	std::cout << "  ops/s: " << std::setprecision(2) << ops_per_sec
	          << ", rows/s: " << rows_per_sec << "\n\n";
}

static void AppendStageRows(Connection &con, const std::string &table, const std::vector<int32_t> &ids,
                            const std::vector<float> &vectors, idx_t dim) {
	Appender appender(con, table);
	for (idx_t i = 0; i < ids.size(); i++) {
		appender.BeginRow();
		appender.Append<int32_t>(ids[i]);
		appender.Append<Value>(MakeArrayValue(vectors.data() + i * dim, dim));
		appender.EndRow();
	}
	appender.Close();
}

static void AppendStageIds(Connection &con, const std::string &table, const std::vector<int32_t> &ids) {
	Appender appender(con, table);
	for (auto id : ids) {
		appender.BeginRow();
		appender.Append<int32_t>(id);
		appender.EndRow();
	}
	appender.Close();
}

static double TimedExec(Connection &con, const std::string &sql) {
	auto start = Clock::now();
	Exec(con, sql);
	auto end = Clock::now();
	return std::chrono::duration<double, std::milli>(end - start).count();
}

} // namespace

int main(int argc, char **argv) {
	try {
		const idx_t dim = 128;
		const idx_t base_rows = 100000;
		const idx_t batch_size = 256;
		const double duration_sec = 180.0;
		const double report_interval_sec = 30.0;

		auto repo_root = std::filesystem::current_path();
		auto data_dir = repo_root / "extension/vex/test/benchmark/data";
		auto extension_path = repo_root / "build-release-vex/extension/vex/vex.duckdb_extension";

		if (argc > 1) {
			data_dir = std::filesystem::absolute(argv[1]);
		}
		if (argc > 2) {
			extension_path = std::filesystem::absolute(argv[2]);
		}

		auto train_path = data_dir / "sift_train_100k.fbin";
		auto train = LoadFloatBin(train_path.string(), dim);
		auto train_rows = train.size() / dim;
		if (train_rows < base_rows) {
			throw std::runtime_error("SIFT train file does not contain enough rows");
		}

		DuckDB db(nullptr);
		Connection con(db);

		auto thread_count = std::max<idx_t>(1, std::min<idx_t>(8, std::thread::hardware_concurrency()));
		Exec(con, "LOAD '" + EscapePath(extension_path.string()) + "'");
		Exec(con, "SET threads TO " + std::to_string(thread_count));
		Exec(con, "SET enable_progress_bar = false");
		Exec(con, "PRAGMA disable_profiling");

		Exec(con, "CREATE TABLE sift_mut (id INTEGER, vec FLOAT[128])");
		Exec(con, "CREATE TABLE stage_ins (id INTEGER, vec FLOAT[128])");
		Exec(con, "CREATE TABLE stage_del (id INTEGER)");
		Exec(con, "CREATE TABLE stage_upd (id INTEGER, vec FLOAT[128])");

		std::cout << "SIFT mutation benchmark\n";
		std::cout << "  data: " << train_path << "\n";
		std::cout << "  extension: " << extension_path << "\n";
		std::cout << "  base_rows: " << base_rows << ", dim: " << dim
		          << ", batch_size: " << batch_size << ", duration: " << duration_sec << " s\n";
		std::cout << "  threads: " << thread_count << "\n\n";

		auto import_start = Clock::now();
		Exec(con, "BEGIN");
		{
			Appender appender(con, "sift_mut");
			for (idx_t row = 0; row < base_rows; row++) {
				auto vec_ptr = train.data() + row * dim;
				appender.BeginRow();
				appender.Append<int32_t>(NumericCast<int32_t>(row));
				appender.Append<Value>(MakeArrayValue(vec_ptr, dim));
				appender.EndRow();
			}
			appender.Close();
		}
		Exec(con, "COMMIT");
		auto import_ms = std::chrono::duration<double, std::milli>(Clock::now() - import_start).count();

		auto build_start = Clock::now();
		Exec(con, "CREATE INDEX idx_sift_vec ON sift_mut USING GRAPH_INDEX (vec) WITH (metric='l2')");
		auto build_ms = std::chrono::duration<double, std::milli>(Clock::now() - build_start).count();

		OpSummary insert_summary {"Insert"};
		OpSummary delete_summary {"Delete"};
		OpSummary update_summary {"Update"};
		std::vector<WindowStats> windows;

		std::deque<int32_t> inserted_ids;
		int32_t next_insert_id = NumericCast<int32_t>(base_rows);
		idx_t vector_cursor = base_rows;
		idx_t update_cursor = 0;

		WindowStats current_window;
		auto bench_start = Clock::now();
		auto next_report = bench_start + std::chrono::milliseconds(static_cast<int64_t>(report_interval_sec * 1000.0));
		while (std::chrono::duration<double>(Clock::now() - bench_start).count() < duration_sec) {
			std::vector<int32_t> insert_ids(batch_size);
			std::vector<float> insert_vectors(batch_size * dim);
			for (idx_t i = 0; i < batch_size; i++) {
				insert_ids[i] = next_insert_id++;
				auto src_idx = (vector_cursor + i) % train_rows;
				auto src_ptr = train.data() + src_idx * dim;
				std::copy(src_ptr, src_ptr + dim, insert_vectors.data() + i * dim);
			}
			vector_cursor = (vector_cursor + batch_size) % train_rows;

			std::vector<int32_t> delete_ids;
			delete_ids.reserve(batch_size);
			for (idx_t i = 0; i < batch_size && !inserted_ids.empty(); i++) {
				delete_ids.push_back(inserted_ids.front());
				inserted_ids.pop_front();
			}

			std::vector<int32_t> update_ids(batch_size);
			std::vector<float> update_vectors(batch_size * dim);
			for (idx_t i = 0; i < batch_size; i++) {
				update_ids[i] = NumericCast<int32_t>(update_cursor % base_rows);
				auto src_idx = (vector_cursor + i) % train_rows;
				auto src_ptr = train.data() + src_idx * dim;
				std::copy(src_ptr, src_ptr + dim, update_vectors.data() + i * dim);
				update_cursor++;
			}
			vector_cursor = (vector_cursor + batch_size) % train_rows;

			Exec(con, "BEGIN");

			Exec(con, "DELETE FROM stage_ins");
			AppendStageRows(con, "stage_ins", insert_ids, insert_vectors, dim);
			auto insert_ms = TimedExec(con, "INSERT INTO sift_mut SELECT * FROM stage_ins");
			insert_summary.latencies_ms.push_back(insert_ms);
			insert_summary.rows += insert_ids.size();
			insert_summary.total_ms += insert_ms;
			current_window.insert_rows += insert_ids.size();
			current_window.insert_ms += insert_ms;
			for (auto id : insert_ids) {
				inserted_ids.push_back(id);
			}

			double delete_ms = 0;
			if (!delete_ids.empty()) {
				Exec(con, "DELETE FROM stage_del");
				AppendStageIds(con, "stage_del", delete_ids);
				delete_ms = TimedExec(con, "DELETE FROM sift_mut USING stage_del WHERE sift_mut.id = stage_del.id");
				delete_summary.latencies_ms.push_back(delete_ms);
				delete_summary.rows += delete_ids.size();
				delete_summary.total_ms += delete_ms;
				current_window.delete_rows += delete_ids.size();
				current_window.delete_ms += delete_ms;
			}

			Exec(con, "DELETE FROM stage_upd");
			AppendStageRows(con, "stage_upd", update_ids, update_vectors, dim);
			auto update_ms =
			    TimedExec(con, "UPDATE sift_mut SET vec = stage_upd.vec FROM stage_upd WHERE sift_mut.id = stage_upd.id");
			update_summary.latencies_ms.push_back(update_ms);
			update_summary.rows += update_ids.size();
			update_summary.total_ms += update_ms;
			current_window.update_rows += update_ids.size();
			current_window.update_ms += update_ms;

			Exec(con, "COMMIT");

			auto now = Clock::now();
			if (now >= next_report) {
				auto elapsed = std::chrono::duration<double>(now - bench_start).count();
				current_window.window_end_sec = elapsed;
				windows.push_back(current_window);

				auto secs = report_interval_sec;
				std::cout << std::fixed << std::setprecision(1);
				std::cout << "[window " << std::setw(5) << elapsed - report_interval_sec
				          << "s - " << std::setw(5) << elapsed << "s] ";
				std::cout << "insert " << std::setprecision(0)
				          << (secs > 0 ? static_cast<double>(current_window.insert_rows) / secs : 0) << " rows/s, ";
				std::cout << "delete "
				          << (secs > 0 ? static_cast<double>(current_window.delete_rows) / secs : 0) << " rows/s, ";
				std::cout << "update "
				          << (secs > 0 ? static_cast<double>(current_window.update_rows) / secs : 0) << " rows/s";
				std::cout << " | batch p95 so far (ms): ins " << std::setprecision(2)
				          << Percentile(insert_summary.latencies_ms, 0.95)
				          << ", del " << Percentile(delete_summary.latencies_ms, 0.95)
				          << ", upd " << Percentile(update_summary.latencies_ms, 0.95) << "\n";

				current_window = WindowStats();
				next_report +=
				    std::chrono::milliseconds(static_cast<int64_t>(report_interval_sec * 1000.0));
			}
		}

		auto total_bench_sec = std::chrono::duration<double>(Clock::now() - bench_start).count();

		auto count_result = con.Query("SELECT COUNT(*) FROM sift_mut");
		RequireOK(count_result, "SELECT COUNT(*) FROM sift_mut");
		auto final_count = count_result->GetValue<int64_t>(0, 0);

		auto query_result =
		    con.Query("SELECT id FROM sift_mut ORDER BY l2_distance(vec, " + ZeroArrayLiteral(dim) + ") LIMIT 10");
		RequireOK(query_result, "sanity query");

		std::cout << "\nSetup\n";
		std::cout << "  import: " << std::fixed << std::setprecision(2) << import_ms << " ms\n";
		std::cout << "  build : " << build_ms << " ms\n\n";

		std::cout << "Benchmark duration: " << total_bench_sec << " s\n";
		std::cout << "Final row count: " << final_count << "\n\n";

		PrintSummary(insert_summary);
		PrintSummary(delete_summary);
		PrintSummary(update_summary);

		std::cout << "Windowed throughput\n";
		for (auto &window : windows) {
			std::cout << "  end@" << std::fixed << std::setprecision(1) << window.window_end_sec << "s"
			          << " insert_rows/s=" << std::setprecision(0)
			          << (report_interval_sec > 0 ? static_cast<double>(window.insert_rows) / report_interval_sec : 0)
			          << " delete_rows/s="
			          << (report_interval_sec > 0 ? static_cast<double>(window.delete_rows) / report_interval_sec : 0)
			          << " update_rows/s="
			          << (report_interval_sec > 0 ? static_cast<double>(window.update_rows) / report_interval_sec : 0)
			          << "\n";
		}

		return 0;
	} catch (std::exception &ex) {
		std::cerr << "Benchmark failed: " << ex.what() << std::endl;
		return 1;
	}
}
