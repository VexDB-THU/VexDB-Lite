// ============================================================
// Ablation Study: PolarQuant preconditioning modes + PQ-NormSep
//
// Experiments:
// 1. PolarQuant + sign_flip (original)
// 2. PolarQuant + Hadamard (SRHT, paper-recommended)
// 3. PolarQuant + none (no preconditioning)
// 4. PQ baseline
// 5. PQ + norm separation (inspired by PolarQuant)
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

static std::vector<float> LoadFloatBin(const std::string &path, uint32_t dim) {
	std::ifstream fs(path, std::ios::binary | std::ios::ate);
	if (!fs) { fprintf(stderr, "Cannot open %s\n", path.c_str()); return {}; }
	size_t size = fs.tellg(); fs.seekg(0);
	std::vector<float> data(size / sizeof(float));
	fs.read(reinterpret_cast<char*>(data.data()), size);
	return data;
}

static std::vector<int32_t> LoadInt32Bin(const std::string &path) {
	std::ifstream fs(path, std::ios::binary | std::ios::ate);
	if (!fs) { return {}; }
	size_t size = fs.tellg(); fs.seekg(0);
	std::vector<int32_t> data(size / sizeof(int32_t));
	fs.read(reinterpret_cast<char*>(data.data()), size);
	return data;
}

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
// PQ + Norm Separation
//
// Idea from PolarQuant: store norm separately (4 bytes, full precision),
// normalize vectors before PQ encoding. At search time:
//   ||q - x||^2 = ||q||^2 + ||x||^2 - 2 * ||x|| * <q, x/||x||>
// But simpler approach: just normalize, PQ the unit vectors,
// store norms separately. Decode = norm * PQ_decode(code).
// ============================================================

struct PQNormSep {
	ProductQuantizer pq;
	std::vector<float> norms; // per-vector norms
	uint32_t d;

	void Init(uint32_t dim, uint32_t m) {
		d = dim;
		pq.Init(dim, m);
	}

	void Train(const float *vectors, uint32_t n) {
		// Normalize training vectors
		std::vector<float> normalized(static_cast<size_t>(n) * d);
		for (uint32_t i = 0; i < n; i++) {
			const float *v = vectors + static_cast<size_t>(i) * d;
			float norm = 0;
			for (uint32_t j = 0; j < d; j++) norm += v[j] * v[j];
			norm = std::sqrt(norm);
			float inv = (norm > 1e-8f) ? 1.0f / norm : 0.0f;
			for (uint32_t j = 0; j < d; j++) {
				normalized[static_cast<size_t>(i) * d + j] = v[j] * inv;
			}
		}
		pq.Train(normalized.data(), n);
	}

	void EncodeAll(const float *vectors, uint32_t n, std::vector<uint8_t> &codes) {
		norms.resize(n);
		codes.resize(static_cast<size_t>(n) * pq.CodeSize());
		std::vector<float> unit(d);
		for (uint32_t i = 0; i < n; i++) {
			const float *v = vectors + static_cast<size_t>(i) * d;
			float norm = 0;
			for (uint32_t j = 0; j < d; j++) norm += v[j] * v[j];
			norm = std::sqrt(norm);
			norms[i] = norm;
			float inv = (norm > 1e-8f) ? 1.0f / norm : 0.0f;
			for (uint32_t j = 0; j < d; j++) unit[j] = v[j] * inv;
			pq.Encode(unit.data(), codes.data() + static_cast<size_t>(i) * pq.CodeSize());
		}
	}

	void Decode(const uint8_t *code, uint32_t idx, float *out) const {
		pq.Decode(code, out);
		float n = norms[idx];
		for (uint32_t j = 0; j < d; j++) out[j] *= n;
	}

	// Search: decode + L2 (using batch decode for fair speed comparison)
	std::vector<float> BatchDecode(const uint8_t *codes, uint32_t n) const {
		std::vector<float> decoded(static_cast<size_t>(n) * d);
		uint32_t cs = pq.CodeSize();
		for (uint32_t i = 0; i < n; i++) {
			pq.Decode(codes + static_cast<size_t>(i) * cs, decoded.data() + static_cast<size_t>(i) * d);
			float norm = norms[i];
			for (uint32_t j = 0; j < d; j++) {
				decoded[static_cast<size_t>(i) * d + j] *= norm;
			}
		}
		return decoded;
	}

	uint32_t CodeSize() const { return pq.CodeSize() + sizeof(float); } // code + norm
	float BitsPerDim() const { return static_cast<float>(CodeSize() * 8) / d; }
	size_t TotalMemBytes(uint32_t n) const {
		return static_cast<size_t>(n) * pq.CodeSize()
		     + static_cast<size_t>(n) * sizeof(float) // norms
		     + static_cast<size_t>(pq.m) * 256 * (d / pq.m) * sizeof(float); // codebook
	}
};

// ============================================================
// Run one experiment
// ============================================================

struct Result {
	const char *name;
	float r1, r10, r100;
	double qps;
	float mse;
	uint32_t code_size;
	float bits_per_dim;
	size_t mem_bytes;
	size_t cache_bytes;
};

static void PrintHeader() {
	printf("%-48s %5s %7s %7s %7s %8s %6s %8s\n",
	       "Method", "B/vec", "R@1", "R@10", "R@100", "QPS", "b/dim", "Mem(MB)");
	printf("%-48s %5s %7s %7s %7s %8s %6s %8s\n",
	       "------", "-----", "---", "----", "-----", "---", "-----", "-------");
}

static void PrintRow(const Result &r) {
	float total_mem = (r.mem_bytes + r.cache_bytes) / (1024.0f * 1024.0f);
	printf("%-48s %5u %7.4f %7.4f %7.4f %8.0f %6.1f %8.2f\n",
	       r.name, r.code_size, r.r1, r.r10, r.r100, r.qps, r.bits_per_dim, total_mem);
}

int main(int argc, char **argv) {
	std::string data_dir = "data/";
	if (argc > 1) data_dir = std::string(argv[1]) + "/";

	bool use_100k = (argc > 2 && std::string(argv[2]) == "100k");
	uint32_t D = 128, NQ = 200, GT_K = 100;
	uint32_t N = use_100k ? 100000 : 10000;

	std::string train_file = data_dir + (use_100k ? "sift_train_100k.fbin" : "sift_train_10k.fbin");
	std::string query_file = data_dir + "sift_query_200.fbin";
	std::string gt_file = data_dir + (use_100k ? "sift_gt_100k_200q.ibin" : "sift_gt_10k_200q.ibin");

	auto train_data = LoadFloatBin(train_file, D);
	auto query_data = LoadFloatBin(query_file, D);
	auto gt_data = LoadInt32Bin(gt_file);
	if (train_data.empty() || query_data.empty() || gt_data.empty()) {
		fprintf(stderr, "Failed to load data\n"); return 1;
	}

	const float *vectors = train_data.data();
	const float *queries = query_data.data();
	const int32_t *gt = gt_data.data();
	uint32_t train_n = std::min(N, 50000u);

	printf("============================================================\n");
	printf("  Ablation Study: SIFT %uK, %u queries\n", N/1000, NQ);
	printf("============================================================\n\n");

	std::vector<Result> results;
	Timer timer;

	// Helper lambda for PolarQuant experiments
	auto run_polar = [&](const char *name, PreconditionMode mode, uint32_t bits) {
		PolarQuantizer pq;
		pq.precondition_mode = mode;
		pq.Init(D, bits);
		pq.Train(vectors, train_n);

		uint32_t cs = pq.CodeSize();
		std::vector<uint8_t> codes(static_cast<size_t>(N) * cs);
		for (uint32_t i = 0; i < N; i++) {
			pq.Encode(vectors + static_cast<size_t>(i) * D, codes.data() + static_cast<size_t>(i) * cs);
		}

		// Batch decode
		timer.Start();
		auto decoded = pq.BatchDecode(codes.data(), N);
		double dec_ms = timer.Ms();

		// MSE
		float total_mse = 0;
		uint32_t mse_n = std::min(N, 1000u);
		for (uint32_t i = 0; i < mse_n; i++) {
			float mse = 0;
			for (uint32_t j = 0; j < D; j++) {
				float diff = vectors[i * D + j] - decoded[i * D + j];
				mse += diff * diff;
			}
			total_mse += mse / D;
		}

		// Search
		float r1 = 0, r10 = 0, r100 = 0;
		timer.Start();
		for (uint32_t q = 0; q < NQ; q++) {
			const float *query = queries + static_cast<size_t>(q) * D;
			std::vector<std::pair<float, uint32_t>> dists(N);
			for (uint32_t i = 0; i < N; i++) {
				dists[i] = {L2Sq(query, decoded.data() + static_cast<size_t>(i) * D, D), i};
			}
			std::partial_sort(dists.begin(), dists.begin() + 100, dists.end());
			std::vector<uint32_t> topk(100);
			for (uint32_t i = 0; i < 100; i++) topk[i] = dists[i].second;
			r1 += RecallAtK(topk, gt + static_cast<size_t>(q) * GT_K, 1);
			r10 += RecallAtK(topk, gt + static_cast<size_t>(q) * GT_K, 10);
			r100 += RecallAtK(topk, gt + static_cast<size_t>(q) * GT_K, 100);
		}
		double search_ms = timer.Ms();

		uint32_t nc = 1u << bits;
		size_t idx_mem = static_cast<size_t>(N) * cs + pq.num_levels * nc * sizeof(float) * 2;
		size_t cache_mem = static_cast<size_t>(N) * D * sizeof(float);

		Result r;
		r.name = strdup(name);
		r.r1 = r1/NQ; r.r10 = r10/NQ; r.r100 = r100/NQ;
		r.qps = NQ / (search_ms / 1000.0);
		r.mse = total_mse / mse_n;
		r.code_size = cs;
		r.bits_per_dim = pq.BitsPerDim();
		r.mem_bytes = idx_mem;
		r.cache_bytes = cache_mem;
		results.push_back(r);

		printf("  %-45s R@10=%.4f  MSE=%.2f  (decode %.1fms)\n", name, r.r10, r.mse, dec_ms);
	};

	// Helper for PQ
	auto run_pq = [&](const char *name, uint32_t m) {
		ProductQuantizer pq;
		pq.Init(D, m);
		pq.Train(vectors, train_n);

		uint32_t cs = pq.CodeSize();
		std::vector<uint8_t> codes(static_cast<size_t>(N) * cs);
		for (uint32_t i = 0; i < N; i++) {
			pq.Encode(vectors + static_cast<size_t>(i) * D, codes.data() + static_cast<size_t>(i) * cs);
		}

		// MSE
		float total_mse = 0;
		std::vector<float> decoded(D);
		uint32_t mse_n = std::min(N, 1000u);
		for (uint32_t i = 0; i < mse_n; i++) {
			pq.Decode(codes.data() + static_cast<size_t>(i) * cs, decoded.data());
			float mse = 0;
			for (uint32_t j = 0; j < D; j++) {
				float diff = vectors[i * D + j] - decoded[j];
				mse += diff * diff;
			}
			total_mse += mse / D;
		}

		// Search (ADC)
		std::vector<float> dt(static_cast<size_t>(m) * 256);
		float r1 = 0, r10 = 0, r100 = 0;
		timer.Start();
		for (uint32_t q = 0; q < NQ; q++) {
			const float *query = queries + static_cast<size_t>(q) * D;
			pq.ComputeDistanceTable(query, dt.data());
			std::vector<std::pair<float, uint32_t>> dists(N);
			for (uint32_t i = 0; i < N; i++) {
				dists[i] = {ProductQuantizer::DistanceFromTable(codes.data() + static_cast<size_t>(i) * cs, dt.data(), m), i};
			}
			std::partial_sort(dists.begin(), dists.begin() + 100, dists.end());
			std::vector<uint32_t> topk(100);
			for (uint32_t i = 0; i < 100; i++) topk[i] = dists[i].second;
			r1 += RecallAtK(topk, gt + static_cast<size_t>(q) * GT_K, 1);
			r10 += RecallAtK(topk, gt + static_cast<size_t>(q) * GT_K, 10);
			r100 += RecallAtK(topk, gt + static_cast<size_t>(q) * GT_K, 100);
		}
		double search_ms = timer.Ms();

		uint32_t dsub = D / m;
		size_t idx_mem = static_cast<size_t>(N) * cs + static_cast<size_t>(m) * 256 * dsub * sizeof(float);

		Result r;
		r.name = strdup(name);
		r.r1 = r1/NQ; r.r10 = r10/NQ; r.r100 = r100/NQ;
		r.qps = NQ / (search_ms / 1000.0);
		r.mse = total_mse / mse_n;
		r.code_size = cs;
		r.bits_per_dim = static_cast<float>(cs * 8) / D;
		r.mem_bytes = idx_mem;
		r.cache_bytes = 0;
		results.push_back(r);

		printf("  %-45s R@10=%.4f  MSE=%.2f\n", name, r.r10, r.mse);
	};

	// Helper for PQ+NormSep
	auto run_pq_normsep = [&](const char *name, uint32_t m) {
		PQNormSep pqns;
		pqns.Init(D, m);
		pqns.Train(vectors, train_n);

		std::vector<uint8_t> codes;
		pqns.EncodeAll(vectors, N, codes);

		// Batch decode
		auto decoded = pqns.BatchDecode(codes.data(), N);

		// MSE
		float total_mse = 0;
		uint32_t mse_n = std::min(N, 1000u);
		for (uint32_t i = 0; i < mse_n; i++) {
			float mse = 0;
			for (uint32_t j = 0; j < D; j++) {
				float diff = vectors[i * D + j] - decoded[i * D + j];
				mse += diff * diff;
			}
			total_mse += mse / D;
		}

		// Search (decoded + L2)
		float r1 = 0, r10 = 0, r100 = 0;
		timer.Start();
		for (uint32_t q = 0; q < NQ; q++) {
			const float *query = queries + static_cast<size_t>(q) * D;
			std::vector<std::pair<float, uint32_t>> dists(N);
			for (uint32_t i = 0; i < N; i++) {
				dists[i] = {L2Sq(query, decoded.data() + static_cast<size_t>(i) * D, D), i};
			}
			std::partial_sort(dists.begin(), dists.begin() + 100, dists.end());
			std::vector<uint32_t> topk(100);
			for (uint32_t i = 0; i < 100; i++) topk[i] = dists[i].second;
			r1 += RecallAtK(topk, gt + static_cast<size_t>(q) * GT_K, 1);
			r10 += RecallAtK(topk, gt + static_cast<size_t>(q) * GT_K, 10);
			r100 += RecallAtK(topk, gt + static_cast<size_t>(q) * GT_K, 100);
		}
		double search_ms = timer.Ms();

		size_t idx_mem = pqns.TotalMemBytes(N);
		size_t cache_mem = static_cast<size_t>(N) * D * sizeof(float);

		Result r;
		r.name = strdup(name);
		r.r1 = r1/NQ; r.r10 = r10/NQ; r.r100 = r100/NQ;
		r.qps = NQ / (search_ms / 1000.0);
		r.mse = total_mse / mse_n;
		r.code_size = pqns.CodeSize();
		r.bits_per_dim = pqns.BitsPerDim();
		r.mem_bytes = idx_mem;
		r.cache_bytes = cache_mem;
		results.push_back(r);

		printf("  %-45s R@10=%.4f  MSE=%.2f\n", name, r.r10, r.mse);
	};

	// ============================================================
	// Run experiments
	// ============================================================

	printf("--- PQ Baselines ---\n");
	run_pq("PQ (m=32, dsub=4)", 32);
	run_pq("PQ (m=16, dsub=8)", 16);

	printf("\n--- PQ + Norm Separation (PolarQuant-inspired) ---\n");
	run_pq_normsep("PQ+NormSep (m=32, dsub=4)", 32);
	run_pq_normsep("PQ+NormSep (m=16, dsub=8)", 16);

	printf("\n--- PolarQuant 4-bit: Preconditioning Ablation ---\n");
	run_polar("PolarQuant 4-bit + sign_flip", PreconditionMode::SIGN_FLIP, 4);
	run_polar("PolarQuant 4-bit + Hadamard (SRHT)", PreconditionMode::HADAMARD, 4);
	run_polar("PolarQuant 4-bit + none", PreconditionMode::NONE, 4);

	printf("\n--- PolarQuant 2-bit: Preconditioning Ablation ---\n");
	run_polar("PolarQuant 2-bit + sign_flip", PreconditionMode::SIGN_FLIP, 2);
	run_polar("PolarQuant 2-bit + Hadamard (SRHT)", PreconditionMode::HADAMARD, 2);
	run_polar("PolarQuant 2-bit + none", PreconditionMode::NONE, 2);

	// ============================================================
	// Summary
	// ============================================================

	printf("\n============================================================\n");
	printf("  Summary\n");
	printf("============================================================\n\n");
	PrintHeader();
	for (auto &r : results) PrintRow(r);
	printf("\n");

	return 0;
}
