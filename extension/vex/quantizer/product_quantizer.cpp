#include "vex_quantizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <random>

namespace duckdb {
namespace vex {

// ============================================================
// Init
// ============================================================

void ProductQuantizer::Init(uint32_t dim, uint32_t num_sub) {
	d = dim;
	m = num_sub;
	dsub = d / m;
	trained = false;
	centroids.resize(static_cast<size_t>(m) * KSUB * dsub, 0.0f);
}

uint32_t ProductQuantizer::AutoSelectM(uint32_t dim) {
	// Choose M so that dsub is a reasonable size (4-8 dimensions per subvector)
	// and dim is evenly divisible by M
	for (uint32_t target_dsub : {4u, 8u, 3u, 6u, 2u, 5u, 1u}) {
		if (dim % target_dsub == 0) {
			uint32_t candidate_m = dim / target_dsub;
			if (candidate_m >= 1 && candidate_m <= dim) {
				return candidate_m;
			}
		}
	}
	return dim; // fallback: 1 dimension per subquantizer
}

// ============================================================
// K-means clustering (simple implementation for edge scenarios)
// ============================================================

static void KMeansClustering(const float *data, uint32_t n, uint32_t dim,
                             uint32_t k, float *centroids, uint32_t max_iters) {
	if (n == 0 || dim == 0 || k == 0) return;

	// If fewer points than centroids, just copy what we have
	if (n <= k) {
		std::memcpy(centroids, data, static_cast<size_t>(n) * dim * sizeof(float));
		// Fill remaining centroids with copies of existing ones
		for (uint32_t i = n; i < k; i++) {
			std::memcpy(centroids + static_cast<size_t>(i) * dim,
			            centroids + (i % n) * dim,
			            dim * sizeof(float));
		}
		return;
	}

	// Initialize centroids with k-means++ style: pick random points
	std::mt19937 rng(42);
	std::vector<uint32_t> indices(n);
	std::iota(indices.begin(), indices.end(), 0);
	std::shuffle(indices.begin(), indices.end(), rng);

	for (uint32_t i = 0; i < k; i++) {
		std::memcpy(centroids + static_cast<size_t>(i) * dim,
		            data + static_cast<size_t>(indices[i]) * dim,
		            dim * sizeof(float));
	}

	// Assignment buffer
	std::vector<uint32_t> assignments(n, 0);
	std::vector<float> centroid_sums(static_cast<size_t>(k) * dim);
	std::vector<uint32_t> centroid_counts(k);

	for (uint32_t iter = 0; iter < max_iters; iter++) {
		// Assign each point to nearest centroid
		bool changed = false;
		for (uint32_t i = 0; i < n; i++) {
			const float *point = data + static_cast<size_t>(i) * dim;
			float best_dist = std::numeric_limits<float>::max();
			uint32_t best_k = 0;

			for (uint32_t j = 0; j < k; j++) {
				const float *cent = centroids + static_cast<size_t>(j) * dim;
				float dist = 0.0f;
				for (uint32_t d = 0; d < dim; d++) {
					float diff = point[d] - cent[d];
					dist += diff * diff;
				}
				if (dist < best_dist) {
					best_dist = dist;
					best_k = j;
				}
			}

			if (assignments[i] != best_k) {
				assignments[i] = best_k;
				changed = true;
			}
		}

		if (!changed && iter > 0) break;

		// Recompute centroids
		std::fill(centroid_sums.begin(), centroid_sums.end(), 0.0f);
		std::fill(centroid_counts.begin(), centroid_counts.end(), 0);

		for (uint32_t i = 0; i < n; i++) {
			uint32_t c = assignments[i];
			centroid_counts[c]++;
			const float *point = data + static_cast<size_t>(i) * dim;
			float *sum = centroid_sums.data() + static_cast<size_t>(c) * dim;
			for (uint32_t d = 0; d < dim; d++) {
				sum[d] += point[d];
			}
		}

		for (uint32_t j = 0; j < k; j++) {
			if (centroid_counts[j] == 0) continue;
			float *cent = centroids + static_cast<size_t>(j) * dim;
			const float *sum = centroid_sums.data() + static_cast<size_t>(j) * dim;
			float inv = 1.0f / static_cast<float>(centroid_counts[j]);
			for (uint32_t d = 0; d < dim; d++) {
				cent[d] = sum[d] * inv;
			}
		}
	}
}

// ============================================================
// Train
// ============================================================

void ProductQuantizer::Train(const float *vectors, uint32_t n) {
	if (d == 0 || m == 0) return;

	// Extract subvectors for each subquantizer and run K-means
	std::vector<float> sub_data(static_cast<size_t>(n) * dsub);

	for (uint32_t sub = 0; sub < m; sub++) {
		// Extract subvectors: for each vector, copy dimensions [sub*dsub, (sub+1)*dsub)
		for (uint32_t i = 0; i < n; i++) {
			const float *src = vectors + static_cast<size_t>(i) * d + sub * dsub;
			float *dst = sub_data.data() + static_cast<size_t>(i) * dsub;
			std::memcpy(dst, src, dsub * sizeof(float));
		}

		// Run K-means to find KSUB centroids for this subquantizer
		float *sub_centroids = centroids.data() + static_cast<size_t>(sub) * KSUB * dsub;
		KMeansClustering(sub_data.data(), n, dsub, KSUB, sub_centroids, MAX_KMEANS_ITERS);
	}

	trained = true;
}

// ============================================================
// Encode
// ============================================================

void ProductQuantizer::Encode(const float *x, uint8_t *code) const {
	for (uint32_t sub = 0; sub < m; sub++) {
		const float *x_sub = x + sub * dsub;
		const float *sub_centroids = centroids.data() + static_cast<size_t>(sub) * KSUB * dsub;

		float best_dist = std::numeric_limits<float>::max();
		uint8_t best_k = 0;

		for (uint32_t k = 0; k < KSUB; k++) {
			const float *cent = sub_centroids + static_cast<size_t>(k) * dsub;
			float dist = 0.0f;
			for (uint32_t j = 0; j < dsub; j++) {
				float diff = x_sub[j] - cent[j];
				dist += diff * diff;
			}
			if (dist < best_dist) {
				best_dist = dist;
				best_k = static_cast<uint8_t>(k);
			}
		}

		code[sub] = best_k;
	}
}

// ============================================================
// Decode
// ============================================================

void ProductQuantizer::Decode(const uint8_t *code, float *x) const {
	for (uint32_t sub = 0; sub < m; sub++) {
		uint8_t k = code[sub];
		const float *cent = centroids.data() + static_cast<size_t>(sub) * KSUB * dsub + static_cast<size_t>(k) * dsub;
		std::memcpy(x + sub * dsub, cent, dsub * sizeof(float));
	}
}

// ============================================================
// Distance Table
// ============================================================

void ProductQuantizer::ComputeDistanceTable(const float *query, float *dist_table) const {
	for (uint32_t sub = 0; sub < m; sub++) {
		const float *q_sub = query + sub * dsub;
		const float *sub_centroids = centroids.data() + static_cast<size_t>(sub) * KSUB * dsub;

		for (uint32_t k = 0; k < KSUB; k++) {
			const float *cent = sub_centroids + static_cast<size_t>(k) * dsub;
			float dist = 0.0f;
			for (uint32_t j = 0; j < dsub; j++) {
				float diff = q_sub[j] - cent[j];
				dist += diff * diff;
			}
			dist_table[static_cast<size_t>(sub) * KSUB + k] = dist;
		}
	}
}

float ProductQuantizer::DistanceFromTable(const uint8_t *code, const float *dist_table, uint32_t m) {
	float dist = 0.0f;
	for (uint32_t sub = 0; sub < m; sub++) {
		dist += dist_table[static_cast<size_t>(sub) * KSUB + code[sub]];
	}
	return dist;
}

// ============================================================
// Serialization
// ============================================================

void ProductQuantizer::SerializeTo(std::vector<char> &out) const {
	// Format: [d:u32][m:u32][dsub:u32][trained:u8][centroids_data]
	size_t start = out.size();
	out.resize(start + 4 + 4 + 4 + 1 + centroids.size() * sizeof(float));
	char *ptr = out.data() + start;

	std::memcpy(ptr, &d, 4); ptr += 4;
	std::memcpy(ptr, &m, 4); ptr += 4;
	std::memcpy(ptr, &dsub, 4); ptr += 4;
	*ptr = trained ? 1 : 0; ptr += 1;
	std::memcpy(ptr, centroids.data(), centroids.size() * sizeof(float));
}

bool ProductQuantizer::DeserializeFrom(const char *&ptr, const char *end) {
	if (ptr + 13 > end) return false;

	std::memcpy(&d, ptr, 4); ptr += 4;
	std::memcpy(&m, ptr, 4); ptr += 4;
	std::memcpy(&dsub, ptr, 4); ptr += 4;
	trained = (*ptr != 0); ptr += 1;

	// Validate deserialized values to prevent integer overflow in size computation
	if (m == 0 || dsub == 0 || m > 256 || dsub > 4096) return false;
	if (d != m * dsub) return false;

	size_t centroid_bytes = static_cast<size_t>(m) * KSUB * dsub * sizeof(float);
	if (ptr + centroid_bytes > end) return false;

	centroids.resize(static_cast<size_t>(m) * KSUB * dsub);
	std::memcpy(centroids.data(), ptr, centroid_bytes);
	ptr += centroid_bytes;

	return true;
}

} // namespace vex
} // namespace duckdb
