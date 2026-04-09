// ============================================================
// Quantizer Benchmark: PQ vs PolarQuant vs QJL
//
// Compares compression ratio, recall@k, encode/decode speed,
// and distance computation accuracy.
//
// Build:
//   cd duckdb/build/debug (or release)
//   cmake ../.. -DBUILD_VEX_BENCHMARK=ON
//   make quantizer_benchmark
//
// Or standalone:
//   clang++ -std=c++17 -O2 -I../../extension/vex/include \
//     ../../extension/vex/quantizer/product_quantizer.cpp \
//     ../../extension/vex/quantizer/polar_quantizer.cpp \
//     ../../extension/vex/quantizer/qjl_quantizer.cpp \
//     quantizer_benchmark.cpp -o quantizer_benchmark
// ============================================================

#include "vex_quantizer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

#if defined(__SSE2__)
#include <immintrin.h>
#define HAS_SIMD 1
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define HAS_NEON 1
#endif

using namespace duckdb::vex;

// ============================================================
// SIMD L2 Distance
// ============================================================

static float L2DistanceSIMD(const float *a, const float *b, uint32_t d) {
#if defined(HAS_NEON)
	float32x4_t sum = vdupq_n_f32(0.0f);
	uint32_t i = 0;
	for (; i + 4 <= d; i += 4) {
		float32x4_t va = vld1q_f32(a + i);
		float32x4_t vb = vld1q_f32(b + i);
		float32x4_t diff = vsubq_f32(va, vb);
		sum = vfmaq_f32(sum, diff, diff);
	}
	float result = vaddvq_f32(sum);
	for (; i < d; i++) {
		float diff = a[i] - b[i];
		result += diff * diff;
	}
	return result;
#elif defined(HAS_SIMD)
	__m128 sum = _mm_setzero_ps();
	uint32_t i = 0;
	for (; i + 4 <= d; i += 4) {
		__m128 va = _mm_loadu_ps(a + i);
		__m128 vb = _mm_loadu_ps(b + i);
		__m128 diff = _mm_sub_ps(va, vb);
		sum = _mm_add_ps(sum, _mm_mul_ps(diff, diff));
	}
	float tmp[4];
	_mm_storeu_ps(tmp, sum);
	float result = tmp[0] + tmp[1] + tmp[2] + tmp[3];
	for (; i < d; i++) {
		float diff = a[i] - b[i];
		result += diff * diff;
	}
	return result;
#else
	float result = 0.0f;
	for (uint32_t i = 0; i < d; i++) {
		float diff = a[i] - b[i];
		result += diff * diff;
	}
	return result;
#endif
}

// ============================================================
// Test Data Generation
// ============================================================

struct TestData {
	uint32_t n;           // number of vectors
	uint32_t d;           // dimension
	uint32_t n_queries;   // number of queries
	uint32_t k;           // top-k
	std::vector<float> vectors;
	std::vector<float> queries;

	// Ground truth: exact top-k neighbors for each query (by L2 distance)
	std::vector<std::vector<uint32_t>> ground_truth;
};

static TestData GenerateTestData(uint32_t n, uint32_t d, uint32_t n_queries, uint32_t k, uint32_t seed = 123) {
	TestData data;
	data.n = n;
	data.d = d;
	data.n_queries = n_queries;
	data.k = k;

	std::mt19937 rng(seed);
	std::normal_distribution<float> dist(0.0f, 1.0f);

	// Generate random vectors
	data.vectors.resize(static_cast<size_t>(n) * d);
	for (auto &v : data.vectors) {
		v = dist(rng);
	}

	// Generate random queries
	data.queries.resize(static_cast<size_t>(n_queries) * d);
	for (auto &v : data.queries) {
		v = dist(rng);
	}

	// Compute ground truth (brute force)
	data.ground_truth.resize(n_queries);
	for (uint32_t q = 0; q < n_queries; q++) {
		const float *query = data.queries.data() + static_cast<size_t>(q) * d;
		std::vector<std::pair<float, uint32_t>> dists(n);

		for (uint32_t i = 0; i < n; i++) {
			const float *vec = data.vectors.data() + static_cast<size_t>(i) * d;
			float dist_sq = 0.0f;
			for (uint32_t j = 0; j < d; j++) {
				float diff = query[j] - vec[j];
				dist_sq += diff * diff;
			}
			dists[i] = {dist_sq, i};
		}

		std::partial_sort(dists.begin(), dists.begin() + k, dists.end());
		data.ground_truth[q].resize(k);
		for (uint32_t i = 0; i < k; i++) {
			data.ground_truth[q][i] = dists[i].second;
		}
	}

	return data;
}

// ============================================================
// Recall Computation
// ============================================================

static float ComputeRecall(const std::vector<uint32_t> &retrieved, const std::vector<uint32_t> &truth) {
	uint32_t hits = 0;
	for (auto idx : retrieved) {
		for (auto t : truth) {
			if (idx == t) {
				hits++;
				break;
			}
		}
	}
	return static_cast<float>(hits) / truth.size();
}

// ============================================================
// Timer
// ============================================================

struct Timer {
	std::chrono::high_resolution_clock::time_point start;
	void Start() { start = std::chrono::high_resolution_clock::now(); }
	double ElapsedMs() {
		auto end = std::chrono::high_resolution_clock::now();
		return std::chrono::duration<double, std::milli>(end - start).count();
	}
};

// ============================================================
// Reconstruction Error
// ============================================================

static float ComputeMSE(const float *original, const float *reconstructed, uint32_t d) {
	float mse = 0.0f;
	for (uint32_t i = 0; i < d; i++) {
		float diff = original[i] - reconstructed[i];
		mse += diff * diff;
	}
	return mse / d;
}

// ============================================================
// Benchmark: Product Quantizer
// ============================================================

struct PQResult {
	float recall;
	double encode_ms;
	double search_ms;
	float avg_mse;
	uint32_t code_size;
	float bits_per_dim;
};

static PQResult BenchmarkPQ(const TestData &data) {
	PQResult result;

	ProductQuantizer pq;
	uint32_t m = ProductQuantizer::AutoSelectM(data.d);
	pq.Init(data.d, m);

	// Train
	uint32_t train_n = std::min(data.n, 10000u);
	pq.Train(data.vectors.data(), train_n);

	result.code_size = pq.CodeSize();
	result.bits_per_dim = static_cast<float>(result.code_size * 8) / data.d;

	// Encode all vectors
	std::vector<uint8_t> codes(static_cast<size_t>(data.n) * result.code_size);
	Timer timer;
	timer.Start();
	for (uint32_t i = 0; i < data.n; i++) {
		pq.Encode(data.vectors.data() + static_cast<size_t>(i) * data.d,
		          codes.data() + static_cast<size_t>(i) * result.code_size);
	}
	result.encode_ms = timer.ElapsedMs();

	// Compute reconstruction MSE
	float total_mse = 0.0f;
	std::vector<float> decoded(data.d);
	uint32_t mse_samples = std::min(data.n, 1000u);
	for (uint32_t i = 0; i < mse_samples; i++) {
		pq.Decode(codes.data() + static_cast<size_t>(i) * result.code_size, decoded.data());
		total_mse += ComputeMSE(data.vectors.data() + static_cast<size_t>(i) * data.d, decoded.data(), data.d);
	}
	result.avg_mse = total_mse / mse_samples;

	// Search: for each query, find top-k by PQ distance
	timer.Start();
	float total_recall = 0.0f;
	std::vector<float> dist_table(static_cast<size_t>(m) * ProductQuantizer::KSUB);

	for (uint32_t q = 0; q < data.n_queries; q++) {
		const float *query = data.queries.data() + static_cast<size_t>(q) * data.d;
		pq.ComputeDistanceTable(query, dist_table.data());

		std::vector<std::pair<float, uint32_t>> dists(data.n);
		for (uint32_t i = 0; i < data.n; i++) {
			float d = ProductQuantizer::DistanceFromTable(
			    codes.data() + static_cast<size_t>(i) * result.code_size, dist_table.data(), m);
			dists[i] = {d, i};
		}

		std::partial_sort(dists.begin(), dists.begin() + data.k, dists.end());
		std::vector<uint32_t> retrieved(data.k);
		for (uint32_t i = 0; i < data.k; i++) {
			retrieved[i] = dists[i].second;
		}

		total_recall += ComputeRecall(retrieved, data.ground_truth[q]);
	}
	result.search_ms = timer.ElapsedMs();
	result.recall = total_recall / data.n_queries;

	return result;
}

// ============================================================
// Benchmark: PolarQuant
// ============================================================

struct PolarResult {
	float recall;
	double encode_ms;
	double search_ms;
	double decode_ms;    // batch decode time
	float avg_mse;
	uint32_t code_size;
	float bits_per_dim;
	const char *method;
};

// Shared setup for PolarQuant benchmarks
struct PolarSetup {
	PolarQuantizer pq;
	uint32_t padded_d;
	std::vector<float> padded_vectors;
	std::vector<uint8_t> codes;
	uint32_t code_size;
	double encode_ms;
	float avg_mse;
	float bits_per_dim;
};

static PolarSetup SetupPolar(const TestData &data, uint32_t bits_per_angle) {
	PolarSetup s;

	s.padded_d = data.d;
	{
		uint32_t v = s.padded_d - 1;
		v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
		s.padded_d = v + 1;
	}

	s.pq.Init(s.padded_d, bits_per_angle);

	if (s.padded_d != data.d) {
		s.padded_vectors.resize(static_cast<size_t>(data.n) * s.padded_d, 0.0f);
		for (uint32_t i = 0; i < data.n; i++) {
			std::memcpy(s.padded_vectors.data() + static_cast<size_t>(i) * s.padded_d,
			            data.vectors.data() + static_cast<size_t>(i) * data.d,
			            data.d * sizeof(float));
		}
	}
	const float *train_data = s.padded_d != data.d ? s.padded_vectors.data() : data.vectors.data();

	uint32_t train_n = std::min(data.n, 10000u);
	s.pq.Train(train_data, train_n); // also calls BuildTrigTables()

	s.code_size = s.pq.CodeSize();
	s.bits_per_dim = s.pq.BitsPerDim();

	// Encode
	s.codes.resize(static_cast<size_t>(data.n) * s.code_size);
	Timer timer;
	timer.Start();
	for (uint32_t i = 0; i < data.n; i++) {
		const float *vec = s.padded_d != data.d
		    ? s.padded_vectors.data() + static_cast<size_t>(i) * s.padded_d
		    : data.vectors.data() + static_cast<size_t>(i) * data.d;
		s.pq.Encode(vec, s.codes.data() + static_cast<size_t>(i) * s.code_size);
	}
	s.encode_ms = timer.ElapsedMs();

	// MSE
	float total_mse = 0.0f;
	std::vector<float> decoded(s.padded_d);
	uint32_t mse_samples = std::min(data.n, 1000u);
	for (uint32_t i = 0; i < mse_samples; i++) {
		s.pq.FastDecode(s.codes.data() + static_cast<size_t>(i) * s.code_size, decoded.data());
		total_mse += ComputeMSE(
		    s.padded_d != data.d
		        ? s.padded_vectors.data() + static_cast<size_t>(i) * s.padded_d
		        : data.vectors.data() + static_cast<size_t>(i) * data.d,
		    decoded.data(), data.d);
	}
	s.avg_mse = total_mse / mse_samples;

	return s;
}

// Method 1: Original (decode per distance call, with trig)
static PolarResult BenchmarkPolar(const TestData &data, uint32_t bits_per_angle) {
	auto s = SetupPolar(data, bits_per_angle);
	PolarResult result;
	result.code_size = s.code_size;
	result.bits_per_dim = s.bits_per_dim;
	result.encode_ms = s.encode_ms;
	result.avg_mse = s.avg_mse;
	result.decode_ms = 0;
	result.method = "original";

	std::vector<float> padded_query(s.padded_d, 0.0f);
	Timer timer;
	timer.Start();
	float total_recall = 0.0f;

	for (uint32_t q = 0; q < data.n_queries; q++) {
		const float *query_orig = data.queries.data() + static_cast<size_t>(q) * data.d;
		if (s.padded_d != data.d) {
			std::memcpy(padded_query.data(), query_orig, data.d * sizeof(float));
		}
		const float *query = s.padded_d != data.d ? padded_query.data() : query_orig;

		std::vector<std::pair<float, uint32_t>> dists(data.n);
		for (uint32_t i = 0; i < data.n; i++) {
			float d = s.pq.Distance(query, s.codes.data() + static_cast<size_t>(i) * s.code_size);
			dists[i] = {d, i};
		}

		std::partial_sort(dists.begin(), dists.begin() + data.k, dists.end());
		std::vector<uint32_t> retrieved(data.k);
		for (uint32_t i = 0; i < data.k; i++) {
			retrieved[i] = dists[i].second;
		}

		total_recall += ComputeRecall(retrieved, data.ground_truth[q]);
	}
	result.search_ms = timer.ElapsedMs();
	result.recall = total_recall / data.n_queries;

	return result;
}

// Method 2: FastDecode (trig table lookup per distance call)
static PolarResult BenchmarkPolarFastDecode(const TestData &data, uint32_t bits_per_angle) {
	auto s = SetupPolar(data, bits_per_angle);
	PolarResult result;
	result.code_size = s.code_size;
	result.bits_per_dim = s.bits_per_dim;
	result.encode_ms = s.encode_ms;
	result.avg_mse = s.avg_mse;
	result.decode_ms = 0;
	result.method = "fast_decode";

	// Use DistanceFromTable: precondition query once, then fast-decode each code
	std::vector<float> dist_table(s.pq.DistanceTableSize());
	std::vector<float> padded_query(s.padded_d, 0.0f);

	Timer timer;
	timer.Start();
	float total_recall = 0.0f;

	for (uint32_t q = 0; q < data.n_queries; q++) {
		const float *query_orig = data.queries.data() + static_cast<size_t>(q) * data.d;
		if (s.padded_d != data.d) {
			std::memcpy(padded_query.data(), query_orig, data.d * sizeof(float));
		}
		const float *query = s.padded_d != data.d ? padded_query.data() : query_orig;

		// Precondition query once
		s.pq.ComputeDistanceTable(query, dist_table.data());

		std::vector<std::pair<float, uint32_t>> dists(data.n);
		for (uint32_t i = 0; i < data.n; i++) {
			float d = s.pq.DistanceFromTable(
			    s.codes.data() + static_cast<size_t>(i) * s.code_size, dist_table.data());
			dists[i] = {d, i};
		}

		std::partial_sort(dists.begin(), dists.begin() + data.k, dists.end());
		std::vector<uint32_t> retrieved(data.k);
		for (uint32_t i = 0; i < data.k; i++) {
			retrieved[i] = dists[i].second;
		}

		total_recall += ComputeRecall(retrieved, data.ground_truth[q]);
	}
	result.search_ms = timer.ElapsedMs();
	result.recall = total_recall / data.n_queries;

	return result;
}

// Method 3: Batch decode — pre-decode all vectors, then pure float L2 scan
static PolarResult BenchmarkPolarBatchDecode(const TestData &data, uint32_t bits_per_angle) {
	auto s = SetupPolar(data, bits_per_angle);
	PolarResult result;
	result.code_size = s.code_size;
	result.bits_per_dim = s.bits_per_dim;
	result.encode_ms = s.encode_ms;
	result.avg_mse = s.avg_mse;
	result.method = "batch_decode";

	// Batch decode all vectors (one-time cost)
	Timer timer;
	timer.Start();
	std::vector<float> decoded = s.pq.BatchDecode(s.codes.data(), data.n);
	result.decode_ms = timer.ElapsedMs();

	// Search: pure float L2 scan
	timer.Start();
	float total_recall = 0.0f;

	for (uint32_t q = 0; q < data.n_queries; q++) {
		const float *query = data.queries.data() + static_cast<size_t>(q) * data.d;

		std::vector<std::pair<float, uint32_t>> dists(data.n);
		for (uint32_t i = 0; i < data.n; i++) {
			const float *vec = decoded.data() + static_cast<size_t>(i) * s.padded_d;
			float d = L2DistanceSIMD(query, vec, data.d);
			dists[i] = {d, i};
		}

		std::partial_sort(dists.begin(), dists.begin() + data.k, dists.end());
		std::vector<uint32_t> retrieved(data.k);
		for (uint32_t i = 0; i < data.k; i++) {
			retrieved[i] = dists[i].second;
		}

		total_recall += ComputeRecall(retrieved, data.ground_truth[q]);
	}
	result.search_ms = timer.ElapsedMs();
	result.recall = total_recall / data.n_queries;

	return result;
}

// ============================================================
// Benchmark: QJL
// ============================================================

struct QJLResult {
	float recall;
	double encode_ms;
	double search_ms;
	uint32_t code_size;
	float bits_per_dim;
};

static QJLResult BenchmarkQJL(const TestData &data, uint32_t proj_dim) {
	QJLResult result;

	QJLQuantizer qjl;
	qjl.Init(data.d, proj_dim);

	result.code_size = qjl.CodeSize();
	result.bits_per_dim = static_cast<float>(result.code_size * 8) / data.d;

	// Encode
	std::vector<uint8_t> codes(static_cast<size_t>(data.n) * result.code_size);
	Timer timer;
	timer.Start();
	for (uint32_t i = 0; i < data.n; i++) {
		qjl.Encode(data.vectors.data() + static_cast<size_t>(i) * data.d,
		           codes.data() + static_cast<size_t>(i) * result.code_size);
	}
	result.encode_ms = timer.ElapsedMs();

	// Search using inner product estimator
	// For L2 recall test, we use: ||q-x||^2 = ||q||^2 + ||x||^2 - 2*IP(q,x)
	// Since ||q||^2 is constant per query, we rank by -IP(q,x) as proxy
	timer.Start();
	float total_recall = 0.0f;

	for (uint32_t q = 0; q < data.n_queries; q++) {
		const float *query = data.queries.data() + static_cast<size_t>(q) * data.d;

		std::vector<std::pair<float, uint32_t>> scores(data.n);
		for (uint32_t i = 0; i < data.n; i++) {
			float ip = qjl.EstimateInnerProduct(query, codes.data() + static_cast<size_t>(i) * result.code_size);
			scores[i] = {-ip, i}; // negate for min-heap ordering (want max IP)
		}

		std::partial_sort(scores.begin(), scores.begin() + data.k, scores.end());
		std::vector<uint32_t> retrieved(data.k);
		for (uint32_t i = 0; i < data.k; i++) {
			retrieved[i] = scores[i].second;
		}

		total_recall += ComputeRecall(retrieved, data.ground_truth[q]);
	}
	result.search_ms = timer.ElapsedMs();
	result.recall = total_recall / data.n_queries;

	return result;
}

// ============================================================
// Main
// ============================================================

int main() {
	printf("============================================================\n");
	printf("  Quantizer Benchmark: PQ vs PolarQuant vs QJL\n");
	printf("============================================================\n\n");

	// Test configurations
	struct Config {
		uint32_t n;
		uint32_t d;
		uint32_t n_queries;
		uint32_t k;
		const char *name;
	};

	auto print_polar = [](const PolarResult &r, uint32_t k, uint32_t n, uint32_t n_queries) {
		printf("    Code size:    %u bytes (%.1f bits/dim)\n", r.code_size, r.bits_per_dim);
		printf("    Recall@%u:    %.4f\n", k, r.recall);
		printf("    Avg MSE:      %.6f\n", r.avg_mse);
		printf("    Encode time:  %.2f ms (%u vectors)\n", r.encode_ms, n);
		if (r.decode_ms > 0) {
			printf("    Decode time:  %.2f ms (batch pre-decode)\n", r.decode_ms);
		}
		printf("    Search time:  %.2f ms (%u queries)\n", r.search_ms, n_queries);
	};

	// Only run 10K x 128d for speed comparison
	Config configs[] = {
	    {10000, 128, 100, 10, "10K x 128d"},
	};

	for (auto &cfg : configs) {
		printf("------------------------------------------------------------\n");
		printf("Dataset: %s (k=%u)\n", cfg.name, cfg.k);
		printf("------------------------------------------------------------\n");

		auto data = GenerateTestData(cfg.n, cfg.d, cfg.n_queries, cfg.k);

		// PQ baseline
		{
			auto r = BenchmarkPQ(data);
			printf("\n  [PQ] (m=%u)\n", ProductQuantizer::AutoSelectM(cfg.d));
			printf("    Code size:    %u bytes (%.1f bits/dim)\n", r.code_size, r.bits_per_dim);
			printf("    Recall@%u:    %.4f\n", cfg.k, r.recall);
			printf("    Avg MSE:      %.6f\n", r.avg_mse);
			printf("    Encode time:  %.2f ms (%u vectors)\n", r.encode_ms, cfg.n);
			printf("    Search time:  %.2f ms (%u queries)\n", r.search_ms, cfg.n_queries);
		}

		// PolarQuant 4-bit: original (trig per call)
		{
			auto r = BenchmarkPolar(data, 4);
			printf("\n  [PolarQuant 4-bit] original (trig per distance)\n");
			print_polar(r, cfg.k, cfg.n, cfg.n_queries);
		}

		// PolarQuant 4-bit: fast decode (trig table lookup)
		{
			auto r = BenchmarkPolarFastDecode(data, 4);
			printf("\n  [PolarQuant 4-bit] fast_decode (trig tables)\n");
			print_polar(r, cfg.k, cfg.n, cfg.n_queries);
		}

		// PolarQuant 4-bit: batch decode (pre-decode + float L2)
		{
			auto r = BenchmarkPolarBatchDecode(data, 4);
			printf("\n  [PolarQuant 4-bit] batch_decode (pre-decode + L2 scan)\n");
			print_polar(r, cfg.k, cfg.n, cfg.n_queries);
		}

		printf("\n");
	}

	printf("============================================================\n");
	printf("  Speed Comparison Summary\n");
	printf("============================================================\n");
	printf("  PQ ADC:              table lookup, O(m) per distance\n");
	printf("  Polar original:      decode+L2, O(d) trig calls per distance\n");
	printf("  Polar fast_decode:   decode+L2, O(d) table lookups per distance\n");
	printf("  Polar batch_decode:  one-time decode, then O(d) float ops per distance\n");
	printf("============================================================\n");

	return 0;
}
