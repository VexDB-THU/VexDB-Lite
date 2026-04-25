#include "vex/vex_quant_distancer.hpp"

#include <cstddef>
#include <cstring>

#if defined(__AVX2__) && (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86))
#include <immintrin.h>
#define VEX_PQDISTANCER_HAS_AVX2 1
#else
#define VEX_PQDISTANCER_HAS_AVX2 0
#endif

namespace vex {

namespace {

#if VEX_PQDISTANCER_HAS_AVX2
inline float HorizontalSum8(__m256 value) {
    __m128 low = _mm256_castps256_ps128(value);
    __m128 high = _mm256_extractf128_ps(value, 1);
    __m128 sum = _mm_add_ps(low, high);
    __m128 shuf = _mm_movehdup_ps(sum);
    sum = _mm_add_ps(sum, shuf);
    shuf = _mm_movehl_ps(shuf, sum);
    sum = _mm_add_ss(sum, shuf);
    return _mm_cvtss_f32(sum);
}

inline __m256 GatherDistanceChunk4(const float *dist_table, const uint8_t *code, uint32_t sub_offset) {
    uint32_t code_word = 0;
    std::memcpy(&code_word, code, sizeof(code_word));
    const __m128i code_bytes = _mm_cvtsi32_si128(static_cast<int>(code_word));
    const __m256i idx = _mm256_cvtepu8_epi32(code_bytes);
    const __m256i offsets = _mm256_setr_epi32(
        static_cast<int>((sub_offset + 0) * ProductQuantizer::KSUB),
        static_cast<int>((sub_offset + 1) * ProductQuantizer::KSUB),
        static_cast<int>((sub_offset + 2) * ProductQuantizer::KSUB),
        static_cast<int>((sub_offset + 3) * ProductQuantizer::KSUB),
        0, 0, 0, 0);
    return _mm256_i32gather_ps(dist_table, _mm256_add_epi32(idx, offsets), sizeof(float));
}

inline __m256 GatherDistanceChunk8(const float *dist_table, const uint8_t *code, uint32_t sub_offset) {
    uint64_t code_word = 0;
    std::memcpy(&code_word, code, sizeof(code_word));
    const __m128i code_bytes = _mm_cvtsi64_si128(static_cast<long long>(code_word));
    const __m256i idx = _mm256_cvtepu8_epi32(code_bytes);
    const __m256i offsets = _mm256_setr_epi32(
        static_cast<int>((sub_offset + 0) * ProductQuantizer::KSUB),
        static_cast<int>((sub_offset + 1) * ProductQuantizer::KSUB),
        static_cast<int>((sub_offset + 2) * ProductQuantizer::KSUB),
        static_cast<int>((sub_offset + 3) * ProductQuantizer::KSUB),
        static_cast<int>((sub_offset + 4) * ProductQuantizer::KSUB),
        static_cast<int>((sub_offset + 5) * ProductQuantizer::KSUB),
        static_cast<int>((sub_offset + 6) * ProductQuantizer::KSUB),
        static_cast<int>((sub_offset + 7) * ProductQuantizer::KSUB));
    return _mm256_i32gather_ps(dist_table, _mm256_add_epi32(idx, offsets), sizeof(float));
}

inline float DistanceBatchMultipleOf4AVX2(const float *dist_table, const uint8_t *code, uint32_t m) {
    float result = 0.0f;
    uint32_t sub = 0;

    for (; sub + 8 <= m; sub += 8) {
        result += HorizontalSum8(GatherDistanceChunk8(dist_table, code + sub, sub));
    }
    for (; sub + 4 <= m; sub += 4) {
        result += HorizontalSum8(GatherDistanceChunk4(dist_table, code + sub, sub));
    }
    return result;
}
#endif

} // namespace

void PQDistancerCore::Train(const float *vectors, uint32_t count, uint32_t dim) {
    if (dim == 0) {
        pq_ = ProductQuantizer{};
        dist_table_.clear();
        prepared_ = false;
        return;
    }

    uint32_t m = pq_m_ ? pq_m_ : ProductQuantizer::AutoSelectM(dim);
    if (m == 0 || dim % m != 0) {
        m = ProductQuantizer::AutoSelectM(dim);
        if (m == 0 || dim % m != 0) {
            m = dim;
        }
    }

    pq_.Init(dim, m);
    if (vectors && count > 0) {
        pq_.Train(vectors, count);
    }
    dist_table_.assign(static_cast<size_t>(pq_.m) * ProductQuantizer::KSUB, 0.0f);
    prepared_ = false;
}

void PQDistancerCore::PrepareQuery(const float *query, uint32_t dim) {
    if (!query || !pq_.trained || pq_.d != dim || pq_.m == 0) {
        prepared_ = false;
        return;
    }

    if (dist_table_.size() != static_cast<size_t>(pq_.m) * ProductQuantizer::KSUB) {
        dist_table_.assign(static_cast<size_t>(pq_.m) * ProductQuantizer::KSUB, 0.0f);
    }
    pq_.ComputeDistanceTable(query, dist_table_.data());
    prepared_ = true;
}

void PQDistancerCore::LoadQuantizer(const ProductQuantizer &pq) {
    pq_ = pq;
    prepared_ = false;
    if (pq_.m == 0) {
        dist_table_.clear();
        return;
    }
    dist_table_.assign(static_cast<size_t>(pq_.m) * ProductQuantizer::KSUB, 0.0f);
}

void PQDistancerCore::DistanceBatch(const uint8_t *const *codes, uint32_t count, float *out) const {
    if (!out || count == 0) {
        return;
    }
    if (!prepared_ || dist_table_.empty() || pq_.m == 0 || !codes) {
        for (uint32_t i = 0; i < count; ++i) {
            out[i] = 0.0f;
        }
        return;
    }
    if (TryDistanceBatchSIMD(codes, count, out)) {
        return;
    }
    DistanceBatchScalar(codes, count, out);
}

void PQDistancerCore::DistanceBatchScalar(const uint8_t *const *codes, uint32_t count, float *out) const {
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = codes[i] ? ProductQuantizer::DistanceFromTable(codes[i], dist_table_.data(), pq_.m) : 0.0f;
    }
}

bool PQDistancerCore::TryDistanceBatchSIMD(const uint8_t *const *codes, uint32_t count, float *out) const {
#if VEX_PQDISTANCER_HAS_AVX2
    if (pq_.m >= 4 && (pq_.m % 4) == 0) {
        for (uint32_t i = 0; i < count; ++i) {
            out[i] = codes[i] ? DistanceBatchMultipleOf4AVX2(dist_table_.data(), codes[i], pq_.m) : 0.0f;
        }
        return true;
    }
#else
    (void)codes;
    (void)count;
    (void)out;
#endif
    return false;
}

} // namespace vex
