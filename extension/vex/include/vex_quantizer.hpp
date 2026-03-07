#pragma once

#include "vex_distance.hpp"

#include <cstdint>
#include <vector>

namespace duckdb {
namespace vex {

// ============================================================
// Product Quantizer (PQ)
//
// Splits D-dimensional vectors into M subvectors of dsub dimensions,
// quantizes each subvector to its nearest centroid (from 256 centroids).
// Produces M-byte codes. Distance is approximated via precomputed tables.
// ============================================================
struct ProductQuantizer {
	uint32_t d = 0;         // original dimension
	uint32_t m = 0;         // number of subquantizers
	uint32_t dsub = 0;      // dimension per subquantizer (d / m)
	bool trained = false;

	static constexpr uint32_t KSUB = 256; // centroids per subquantizer (8-bit)
	static constexpr uint32_t MAX_KMEANS_ITERS = 25;
	static constexpr uint32_t MIN_TRAINING_POINTS = 256;

	// Centroids: m * KSUB * dsub floats
	// Layout: centroids[sub * KSUB * dsub + k * dsub + j]
	//   = centroid k of subquantizer sub, dimension j
	std::vector<float> centroids;

	//! Initialize with dimension and number of subquantizers
	void Init(uint32_t dim, uint32_t num_sub);

	//! Auto-select M based on dimension
	static uint32_t AutoSelectM(uint32_t dim);

	//! Train codebook from vectors using K-means
	void Train(const float *vectors, uint32_t n);

	//! Encode a single vector to M-byte code
	void Encode(const float *x, uint8_t *code) const;

	//! Decode a code back to approximate vector
	void Decode(const uint8_t *code, float *x) const;

	//! Precompute distance table: dist_table[sub * KSUB + k] = L2sqr(query_sub, centroid[sub][k])
	void ComputeDistanceTable(const float *query, float *dist_table) const;

	//! Compute approximate L2 distance from code using precomputed table
	static float DistanceFromTable(const uint8_t *code, const float *dist_table, uint32_t m);

	//! Code size in bytes
	uint32_t CodeSize() const { return m; }

	//! Serialize centroids to binary
	void SerializeTo(std::vector<char> &out) const;

	//! Deserialize centroids from binary
	bool DeserializeFrom(const char *&ptr, const char *end);
};

} // namespace vex
} // namespace duckdb
