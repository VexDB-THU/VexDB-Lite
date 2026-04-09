// ============================================================
// SIFT-128 Real Data Benchmark
//
// Measures: Recall@1/10/100, QPS, encode throughput, memory usage
// Compares: PQ vs PolarQuant (4-bit / 2-bit) with batch_decode
//
// Data files (generated from ann-benchmarks SIFT-1M):
//   data/sift_train_10k.fbin    10K x 128 float32
//   data/sift_train_100k.fbin   100K x 128 float32
//   data/sift_query_200.fbin    200 x 128 float32
//   data/sift_gt_10k_200q.ibin  200 x 100 int32
//   data/sift_gt_100k_200q.ibin 200 x 100 int32
// ============================================================

#include "vex_quantizer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define HAS_NEON 1
#elif defined(__SSE2__)
#include <immintrin.h>
#define HAS_SSE 1
#endif

using namespace duckdb::vex;

// ============================================================
// SIMD L2
// ============================================================

static float L2Sq(const float *a, const float *b, uint32_t d) {
#if defined(HAS_NEON)
	float32x4_t sum = vdupq_n_f32(0.0f);
	uint32_t i = 0;
	for (; i + 4 <= d; i += 4) {
		float32x4_t diff = vsubq_f32(vld1q_f32(a + i), vld1q_f32(b + i));
		sum = vfmaq_f32(sum, diff, diff);
	}
	float result = vaddvq_f32(sum);
	for (; i < d; i++) { float diff = a[i] - b[i]; result += diff * diff; }
	return result;
#elif defined(HAS_SSE)
	__m128 sum = _mm_setzero_ps();
	uint32_t i = 0;
	for (; i + 4 <= d; i += 4) {
		__m128 diff = _mm_sub_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i));
		sum = _mm_add_ps(sum, _mm_mul_ps(diff, diff));
	}
	float tmp[4]; _mm_storeu_ps(tmp, sum);
	float result = tmp[0] + tmp[1] + tmp[2] + tmp[3];
	for (; i < d; i++) { float diff = a[i] - b[i]; result += diff * diff; }
	return result;
#else
	float result = 0.0f;
	for (uint32_t i = 0; i < d; i++) { float diff = a[i] - b[i]; result += diff * diff; }
	return result;
#endif
}

// ============================================================
// File IO
// ============================================================

static std::vector<float> LoadFloatBin(const std::string &path, uint32_t expected_dim) {
	std::ifstream fs(path, std::ios::binary | std::ios::ate);
	if (!fs) { fprintf(stderr, "Cannot open %s\n", path.c_str()); return {}; }
	size_t size = fs.tellg();
	fs.seekg(0);
	size_t n = size / (expected_dim * sizeof(float));
	std::vector<float> data(n * expected_dim);
	fs.read(reinterpret_cast<char*>(data.data()), size);
	return data;
}

static std::vector<int32_t> LoadInt32Bin(const std::string &path, uint32_t cols) {
	std::ifstream fs(path, std::ios::binary | std::ios::ate);
	if (!fs) { fprintf(stderr, "Cannot open %s\n", path.c_str()); return {}; }
	size_t size = fs.tellg();
	fs.seekg(0);
	size_t n = size / (cols * sizeof(int32_t));
	std::vector<int32_t> data(n * cols);
	fs.read(reinterpret_cast<char*>(data.data()), size);
	return data;
}

// ============================================================
// Timer & Recall
// ============================================================

struct Timer {
	std::chrono::high_resolution_clock::time_point t;
	void Start() { t = std::chrono::high_resolution_clock::now(); }
	double Ms() { return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t).count(); }
};

static float RecallAtK(const std::vector<uint32_t> &retrieved, const int32_t *gt, uint32_t k) {
	uint32_t hits = 0;
	for (uint32_t i = 0; i < k && i < retrieved.size(); i++) {
		for (uint32_t j = 0; j < k; j++) {
			if (static_cast<int32_t>(retrieved[i]) == gt[j]) { hits++; break; }
		}
	}
	return static_cast<float>(hits) / k;
}

// ============================================================
// Brute-force search with approximate distances
// ============================================================

static std::vector<uint32_t> BruteForceSearch(
    const float *decoded_db, uint32_t n, uint32_t d,
    const float *query, uint32_t k) {

	std::vector<std::pair<float, uint32_t>> dists(n);
	for (uint32_t i = 0; i < n; i++) {
		dists[i] = {L2Sq(query, decoded_db + static_cast<size_t>(i) * d, d), i};
	}
	std::partial_sort(dists.begin(), dists.begin() + k, dists.end());
	std::vector<uint32_t> result(k);
	for (uint32_t i = 0; i < k; i++) result[i] = dists[i].second;
	return result;
}

// PQ search with ADC
static std::vector<uint32_t> PQSearch(
    const ProductQuantizer &pq, const uint8_t *codes, uint32_t n,
    const float *query, uint32_t k) {

	uint32_t m = pq.m;
	std::vector<float> dist_table(static_cast<size_t>(m) * ProductQuantizer::KSUB);
	pq.ComputeDistanceTable(query, dist_table.data());

	uint32_t cs = pq.CodeSize();
	std::vector<std::pair<float, uint32_t>> dists(n);
	for (uint32_t i = 0; i < n; i++) {
		dists[i] = {ProductQuantizer::DistanceFromTable(codes + static_cast<size_t>(i) * cs, dist_table.data(), m), i};
	}
	std::partial_sort(dists.begin(), dists.begin() + k, dists.end());
	std::vector<uint32_t> result(k);
	for (uint32_t i = 0; i < k; i++) result[i] = dists[i].second;
	return result;
}

// ============================================================
// Benchmark runner
// ============================================================

struct BenchResult {
	const char *name;
	float recall_1, recall_10, recall_100;
	double qps;             // queries per second
	double encode_ms;       // total encode time
	double decode_ms;       // batch decode time (0 for PQ)
	uint32_t code_size;     // bytes per vector
	float bits_per_dim;
	size_t index_mem_bytes; // compressed codes + codebook
	size_t decode_mem_bytes; // decoded vectors cache (if used)
	float avg_mse;
};

static void PrintResult(const BenchResult &r, uint32_t n, uint32_t nq) {
	printf("  [%s]\n", r.name);
	printf("    Code:      %u B/vec (%.1f bits/dim)\n", r.code_size, r.bits_per_dim);
	printf("    Recall@1:  %.4f\n", r.recall_1);
	printf("    Recall@10: %.4f\n", r.recall_10);
	printf("    Recall@100:%.4f\n", r.recall_100);
	printf("    QPS:       %.0f  (%.2f ms / %u queries)\n", r.qps, 1000.0 * nq / r.qps, nq);
	printf("    Encode:    %.1f ms (%u vectors, %.0f vec/s)\n",
	       r.encode_ms, n, n / (r.encode_ms / 1000.0));
	if (r.decode_ms > 0) {
		printf("    Decode:    %.1f ms (batch pre-decode)\n", r.decode_ms);
	}
	printf("    MSE:       %.6f\n", r.avg_mse);
	printf("    Index mem: %.2f MB (compressed codes + codebook)\n", r.index_mem_bytes / (1024.0 * 1024.0));
	if (r.decode_mem_bytes > 0) {
		printf("    Cache mem: %.2f MB (decoded vectors)\n", r.decode_mem_bytes / (1024.0 * 1024.0));
	}
	float compression = (static_cast<float>(n) * 128 * 4) / r.index_mem_bytes;
	printf("    Compress:  %.1fx vs raw float32\n", compression);
	printf("\n");
}

// ============================================================
// Main
// ============================================================

int main(int argc, char **argv) {
	std::string data_dir = "data/";
	if (argc > 1) data_dir = std::string(argv[1]) + "/";

	// Determine which dataset to run
	bool use_100k = false;
	if (argc > 2 && std::string(argv[2]) == "100k") use_100k = true;

	uint32_t D = 128;
	uint32_t N, NQ = 200, GT_K = 100;
	std::string train_file, gt_file;

	if (use_100k) {
		N = 100000;
		train_file = data_dir + "sift_train_100k.fbin";
		gt_file = data_dir + "sift_gt_100k_200q.ibin";
	} else {
		N = 10000;
		train_file = data_dir + "sift_train_10k.fbin";
		gt_file = data_dir + "sift_gt_10k_200q.ibin";
	}
	std::string query_file = data_dir + "sift_query_200.fbin";

	printf("============================================================\n");
	printf("  SIFT-128 Benchmark: %uK vectors, %u queries\n", N / 1000, NQ);
	printf("============================================================\n\n");

	// Load data
	auto train_data = LoadFloatBin(train_file, D);
	auto query_data = LoadFloatBin(query_file, D);
	auto gt_data = LoadInt32Bin(gt_file, GT_K);

	if (train_data.empty() || query_data.empty() || gt_data.empty()) {
		fprintf(stderr, "Failed to load data files from %s\n", data_dir.c_str());
		return 1;
	}

	uint32_t actual_n = static_cast<uint32_t>(train_data.size() / D);
	uint32_t actual_nq = static_cast<uint32_t>(query_data.size() / D);
	printf("  Loaded: %u train, %u queries, %u GT neighbors\n\n", actual_n, actual_nq, GT_K);

	const float *vectors = train_data.data();
	const float *queries = query_data.data();
	const int32_t *gt = gt_data.data();

	// Raw float32 baseline memory
	size_t raw_mem = static_cast<size_t>(N) * D * sizeof(float);
	printf("  Raw float32 memory: %.2f MB\n\n", raw_mem / (1024.0 * 1024.0));

	std::vector<BenchResult> results;
	Timer timer;

	// ============================================================
	// 1. Product Quantizer
	// ============================================================
	for (uint32_t target_dsub : {4u, 8u}) {
		uint32_t m = D / target_dsub;
		char name[64];
		snprintf(name, sizeof(name), "PQ (m=%u, dsub=%u)", m, target_dsub);

		ProductQuantizer pq;
		pq.Init(D, m);

		uint32_t train_n = std::min(actual_n, 50000u);
		pq.Train(vectors, train_n);

		uint32_t cs = pq.CodeSize();
		std::vector<uint8_t> codes(static_cast<size_t>(N) * cs);

		timer.Start();
		for (uint32_t i = 0; i < N; i++) {
			pq.Encode(vectors + static_cast<size_t>(i) * D, codes.data() + static_cast<size_t>(i) * cs);
		}
		double enc_ms = timer.Ms();

		// MSE
		float total_mse = 0.0f;
		std::vector<float> decoded(D);
		uint32_t mse_n = std::min(N, 1000u);
		for (uint32_t i = 0; i < mse_n; i++) {
			pq.Decode(codes.data() + static_cast<size_t>(i) * cs, decoded.data());
			float mse = 0.0f;
			for (uint32_t j = 0; j < D; j++) {
				float diff = vectors[i * D + j] - decoded[j];
				mse += diff * diff;
			}
			total_mse += mse / D;
		}

		// Search
		float r1 = 0, r10 = 0, r100 = 0;
		timer.Start();
		for (uint32_t q = 0; q < NQ; q++) {
			auto topk = PQSearch(pq, codes.data(), N, queries + static_cast<size_t>(q) * D, 100);
			r1 += RecallAtK(topk, gt + static_cast<size_t>(q) * GT_K, 1);
			r10 += RecallAtK(topk, gt + static_cast<size_t>(q) * GT_K, 10);
			r100 += RecallAtK(topk, gt + static_cast<size_t>(q) * GT_K, 100);
		}
		double search_ms = timer.Ms();

		size_t codebook_mem = static_cast<size_t>(m) * 256 * target_dsub * sizeof(float);

		BenchResult r;
		r.name = strdup(name);
		r.recall_1 = r1 / NQ; r.recall_10 = r10 / NQ; r.recall_100 = r100 / NQ;
		r.qps = NQ / (search_ms / 1000.0);
		r.encode_ms = enc_ms;
		r.decode_ms = 0;
		r.code_size = cs;
		r.bits_per_dim = static_cast<float>(cs * 8) / D;
		r.index_mem_bytes = static_cast<size_t>(N) * cs + codebook_mem;
		r.decode_mem_bytes = 0;
		r.avg_mse = total_mse / mse_n;
		results.push_back(r);
	}

	// ============================================================
	// 2. PolarQuant (batch decode + SIMD L2 scan)
	// ============================================================
	for (uint32_t bits : {4u, 2u}) {
		char name[64];
		snprintf(name, sizeof(name), "PolarQuant (%u-bit, batch+SIMD)", bits);

		PolarQuantizer pq;
		pq.Init(D, bits); // D=128 is power of 2

		uint32_t train_n = std::min(actual_n, 50000u);
		pq.Train(vectors, train_n); // auto builds trig tables

		uint32_t cs = pq.CodeSize();
		std::vector<uint8_t> codes(static_cast<size_t>(N) * cs);

		timer.Start();
		for (uint32_t i = 0; i < N; i++) {
			pq.Encode(vectors + static_cast<size_t>(i) * D, codes.data() + static_cast<size_t>(i) * cs);
		}
		double enc_ms = timer.Ms();

		// Batch decode
		timer.Start();
		std::vector<float> decoded_all = pq.BatchDecode(codes.data(), N);
		double dec_ms = timer.Ms();

		// MSE
		float total_mse = 0.0f;
		uint32_t mse_n = std::min(N, 1000u);
		for (uint32_t i = 0; i < mse_n; i++) {
			float mse = 0.0f;
			for (uint32_t j = 0; j < D; j++) {
				float diff = vectors[i * D + j] - decoded_all[i * D + j];
				mse += diff * diff;
			}
			total_mse += mse / D;
		}

		// Search
		float r1 = 0, r10 = 0, r100 = 0;
		timer.Start();
		for (uint32_t q = 0; q < NQ; q++) {
			auto topk = BruteForceSearch(decoded_all.data(), N, D, queries + static_cast<size_t>(q) * D, 100);
			r1 += RecallAtK(topk, gt + static_cast<size_t>(q) * GT_K, 1);
			r10 += RecallAtK(topk, gt + static_cast<size_t>(q) * GT_K, 10);
			r100 += RecallAtK(topk, gt + static_cast<size_t>(q) * GT_K, 100);
		}
		double search_ms = timer.Ms();

		uint32_t num_centroids = 1u << bits;
		size_t codebook_mem = pq.num_levels * num_centroids * sizeof(float) * 2; // cos+sin tables

		BenchResult r;
		r.name = strdup(name);
		r.recall_1 = r1 / NQ; r.recall_10 = r10 / NQ; r.recall_100 = r100 / NQ;
		r.qps = NQ / (search_ms / 1000.0);
		r.encode_ms = enc_ms;
		r.decode_ms = dec_ms;
		r.code_size = cs;
		r.bits_per_dim = pq.BitsPerDim();
		r.index_mem_bytes = static_cast<size_t>(N) * cs + codebook_mem;
		r.decode_mem_bytes = static_cast<size_t>(N) * D * sizeof(float);
		r.avg_mse = total_mse / mse_n;
		results.push_back(r);
	}

	// ============================================================
	// 3. PolarQuant fast_decode (no pre-decode cache)
	// ============================================================
	{
		PolarQuantizer pq;
		pq.Init(D, 4);
		uint32_t train_n = std::min(actual_n, 50000u);
		pq.Train(vectors, train_n);

		uint32_t cs = pq.CodeSize();
		std::vector<uint8_t> codes(static_cast<size_t>(N) * cs);
		for (uint32_t i = 0; i < N; i++) {
			pq.Encode(vectors + static_cast<size_t>(i) * D, codes.data() + static_cast<size_t>(i) * cs);
		}

		// Search using DistanceFromTable (no pre-decode, saves memory)
		std::vector<float> dt(pq.DistanceTableSize());
		float r1 = 0, r10 = 0, r100 = 0;
		timer.Start();
		for (uint32_t q = 0; q < NQ; q++) {
			const float *query = queries + static_cast<size_t>(q) * D;
			pq.ComputeDistanceTable(query, dt.data());

			std::vector<std::pair<float, uint32_t>> dists(N);
			for (uint32_t i = 0; i < N; i++) {
				dists[i] = {pq.DistanceFromTable(codes.data() + static_cast<size_t>(i) * cs, dt.data()), i};
			}
			std::partial_sort(dists.begin(), dists.begin() + 100, dists.end());

			std::vector<uint32_t> topk(100);
			for (uint32_t i = 0; i < 100; i++) topk[i] = dists[i].second;

			r1 += RecallAtK(topk, gt + static_cast<size_t>(q) * GT_K, 1);
			r10 += RecallAtK(topk, gt + static_cast<size_t>(q) * GT_K, 10);
			r100 += RecallAtK(topk, gt + static_cast<size_t>(q) * GT_K, 100);
		}
		double search_ms = timer.Ms();

		uint32_t num_centroids = 1u << 4;
		size_t codebook_mem = pq.num_levels * num_centroids * sizeof(float) * 2;

		BenchResult r;
		r.name = "PolarQuant (4-bit, fast_decode, no cache)";
		r.recall_1 = r1 / NQ; r.recall_10 = r10 / NQ; r.recall_100 = r100 / NQ;
		r.qps = NQ / (search_ms / 1000.0);
		r.encode_ms = 0; r.decode_ms = 0;
		r.code_size = cs;
		r.bits_per_dim = pq.BitsPerDim();
		r.index_mem_bytes = static_cast<size_t>(N) * cs + codebook_mem;
		r.decode_mem_bytes = 0;
		r.avg_mse = 0;
		results.push_back(r);
	}

	// ============================================================
	// Print all results
	// ============================================================
	printf("============================================================\n");
	printf("  Results\n");
	printf("============================================================\n\n");

	for (auto &r : results) {
		PrintResult(r, N, NQ);
	}

	// Summary table
	printf("============================================================\n");
	printf("  Summary Table\n");
	printf("============================================================\n");
	printf("%-42s %6s %8s %8s %8s %8s %8s\n",
	       "Method", "B/vec", "R@1", "R@10", "R@100", "QPS", "Mem(MB)");
	printf("%-42s %6s %8s %8s %8s %8s %8s\n",
	       "------", "-----", "---", "----", "-----", "---", "-------");
	for (auto &r : results) {
		float total_mem = (r.index_mem_bytes + r.decode_mem_bytes) / (1024.0f * 1024.0f);
		printf("%-42s %6u %8.4f %8.4f %8.4f %8.0f %8.2f\n",
		       r.name, r.code_size, r.recall_1, r.recall_10, r.recall_100, r.qps, total_mem);
	}
	printf("\n");

	return 0;
}
