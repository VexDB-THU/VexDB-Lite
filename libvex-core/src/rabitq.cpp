#include "vex/vex_rabitq.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace vex {

namespace {

constexpr float kRabitQTightStart[9] = {
    0.0f, 0.15f, 0.20f, 0.52f, 0.59f, 0.71f, 0.75f, 0.77f, 0.81f,
};

inline void ScalarQuantize(const float *input,
                           const float *centroid,
                           int dim,
                           int total_bits,
                           std::vector<uint16_t> &codes,
                           float &delta,
                           float &vl) {
    codes.assign(static_cast<size_t>(dim), 0);
    delta = 0.0f;
    vl = 0.0f;

    if (!input || !centroid || dim <= 0 || total_bits <= 0) {
        return;
    }

    const int levels = (1 << total_bits) - 1;
    std::vector<float> residual(static_cast<size_t>(dim), 0.0f);
    for (int i = 0; i < dim; ++i) {
        residual[static_cast<size_t>(i)] = input[i] - centroid[i];
    }

    double max_abs = 0.0;
    for (float v : residual) {
        max_abs = std::max(max_abs, static_cast<double>(std::abs(v)));
    }
    if (max_abs == 0.0) {
        return;
    }

    delta = static_cast<float>((2.0 * max_abs) / std::max(1, levels));
    vl = static_cast<float>(-max_abs);
    if (delta == 0.0f) {
        return;
    }

    for (int i = 0; i < dim; ++i) {
        const double shifted = (static_cast<double>(residual[static_cast<size_t>(i)]) - vl) / delta;
        const int rounded = static_cast<int>(std::llround(shifted));
        codes[static_cast<size_t>(i)] = static_cast<uint16_t>(std::clamp(rounded, 0, levels));
    }
}

} // namespace

template <typename T>
double RabitQBestRescaleFactor(T *o_abs, int dim, int ex_bits) {
    constexpr double kEps = 1e-5;
    constexpr int kNEnum = 10;
    const double max_o = *std::max_element(o_abs, o_abs + dim);

    if (max_o <= kEps) {
        return 0.0;
    }

    const double t_end = static_cast<double>(((1 << ex_bits) - 1) + kNEnum) / max_o;
    const double t_start = t_end * kRabitQTightStart[ex_bits];

    std::vector<int> cur_o_bar(static_cast<size_t>(dim), 0);
    double sqr_denominator = static_cast<double>(dim) * 0.25;
    double numerator = 0.0;

    for (int i = 0; i < dim; ++i) {
        const int cur = static_cast<int>((t_start * o_abs[i]) + kEps);
        cur_o_bar[static_cast<size_t>(i)] = cur;
        sqr_denominator += cur * cur + cur;
        numerator += (cur + 0.5) * o_abs[i];
    }

    std::priority_queue<
        std::pair<double, size_t>,
        std::vector<std::pair<double, size_t>>,
        std::greater<>> next_t;

    for (int i = 0; i < dim; ++i) {
        if (o_abs[i] > kEps) {
            next_t.emplace(static_cast<double>(cur_o_bar[static_cast<size_t>(i)] + 1) / o_abs[i],
                           static_cast<size_t>(i));
        }
    }

    double max_ip = 0.0;
    double t = 0.0;

    while (!next_t.empty()) {
        const double cur_t = next_t.top().first;
        const size_t update_id = next_t.top().second;
        next_t.pop();

        cur_o_bar[update_id]++;
        const int update_o_bar = cur_o_bar[update_id];
        sqr_denominator += 2 * update_o_bar;
        numerator += o_abs[update_id];

        const double cur_ip = numerator / std::sqrt(sqr_denominator);
        if (cur_ip > max_ip) {
            max_ip = cur_ip;
            t = cur_t;
        }

        if (update_o_bar < (1 << ex_bits) - 1 && o_abs[update_id] > kEps) {
            const double t_next = static_cast<double>(update_o_bar + 1) / o_abs[update_id];
            if (t_next < t_end) {
                next_t.emplace(t_next, update_id);
            }
        }
    }

    return t;
}

template double RabitQBestRescaleFactor<float>(float *o_abs, int dim, int ex_bits);
template double RabitQBestRescaleFactor<double>(double *o_abs, int dim, int ex_bits);

double RabitQConstScalingFactor(int dim, int ex_bits) {
    if (dim <= 0) {
        return -1.0;
    }

    constexpr long kConstNum = 100;
    std::vector<double> matrix(static_cast<size_t>(kConstNum) * static_cast<size_t>(dim), 0.0);

    RabitQGenerateRandomMatrix(matrix.data(), static_cast<int>(kConstNum), dim);
    RabitQRowwiseNormAbs(matrix.data(), kConstNum, dim);

    double sum = 0.0;
    for (long i = 0; i < kConstNum; ++i) {
        sum += RabitQBestRescaleFactor(matrix.data() + i * dim, dim, ex_bits);
    }
    return sum / static_cast<double>(kConstNum);
}

RabitQRotator::RabitQRotator(int dim, int padded_dim)
    : dim_(dim),
      padded_dim_(padded_dim),
      flip_(static_cast<size_t>(4 * padded_dim_ / static_cast<int>(kByteLen)), 0) {
    const uint32_t bottom_log_dim = FloorLog2(static_cast<uint32_t>(dim_));
    trunc_dim_ = 1 << bottom_log_dim;
    factor_ = 1.0f / std::sqrt(static_cast<float>(trunc_dim_));
}

void RabitQRotator::Build() {
    if (flip_.empty()) {
        return;
    }
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto &v : flip_) {
        v = static_cast<uint8_t>(dist(rng));
    }
}

void RabitQRotator::Rotate(const float *data, float *rotated) const {
    if (!data || !rotated) {
        return;
    }

    std::fill(rotated, rotated + padded_dim_, 0.0f);
    for (int i = 0; i < dim_; ++i) {
        const uint8_t bits = flip_[static_cast<size_t>(i / static_cast<int>(kByteLen))];
        const bool neg = ((bits >> (i % static_cast<int>(kByteLen))) & 1U) != 0;
        rotated[i] = neg ? -data[i] : data[i];
    }
    Rescale(rotated, trunc_dim_, factor_);
}

uint32_t RabitQRotator::FloorLog2(uint32_t x) {
    uint32_t ret = 0;
    while (x > 1) {
        ++ret;
        x >>= 1;
    }
    return ret;
}

void RabitQRotator::Rescale(float *data, int dim, float factor) {
    for (int i = 0; i < dim; ++i) {
        data[i] *= factor;
    }
}

RabitQQuery::RabitQQuery(int padded_dim)
    : padded_dim_(padded_dim),
      query_bits_(static_cast<size_t>(padded_dim) * kRabitQQueryBits / 64, 0),
      centroid_(static_cast<size_t>(padded_dim), 0.0f),
      quantized_query_(static_cast<size_t>(padded_dim), 0) {
}

void RabitQQuery::Preprocess(const float *rotated_query,
                             const float *centroid,
                             float delta,
                             float vl,
                             float *out_g_add,
                             float *out_g_error) {
    if (!rotated_query) {
        std::fill(query_bits_.begin(), query_bits_.end(), 0);
        g_k1_sum_q_ = 0.0f;
        g_kb_sum_q_ = 0.0f;
        delta_ = 0.0f;
        vl_ = 0.0f;
        if (out_g_add) {
            *out_g_add = 0.0f;
        }
        if (out_g_error) {
            *out_g_error = 0.0f;
        }
        return;
    }

    if (centroid) {
        std::memcpy(centroid_.data(), centroid, static_cast<size_t>(padded_dim_) * sizeof(float));
    } else {
        std::fill(centroid_.begin(), centroid_.end(), 0.0f);
    }

    delta_ = delta;
    vl_ = vl;

    const float c1 = -static_cast<float>((1 << 1) - 1) / 2.0f;
    const float cb = -static_cast<float>((1 << (kRabitQExBits + 1)) - 1) / 2.0f;
    const float sumq = std::accumulate(rotated_query, rotated_query + padded_dim_, 0.0f);
    g_k1_sum_q_ = sumq * c1;
    g_kb_sum_q_ = sumq * cb;

    float local_delta = 0.0f;
    float local_vl = 0.0f;
    ScalarQuantize(rotated_query, centroid_.data(), padded_dim_, static_cast<int>(kRabitQQueryBits),
                   quantized_query_, local_delta, local_vl);
    if (delta_ == 0.0f) {
        delta_ = local_delta;
    }
    if (vl_ == 0.0f) {
        vl_ = local_vl;
    }
    TransposeBits(quantized_query_.data(), query_bits_.data(), static_cast<size_t>(padded_dim_));

    if (out_g_add) {
        *out_g_add = 0.0f;
    }
    if (out_g_error) {
        *out_g_error = 0.0f;
    }
}

void RabitQQuery::TransposeBits(const uint16_t *quantized, uint64_t *transposed, size_t padded_dim) const {
    if (!quantized || !transposed) {
        return;
    }

    for (size_t i = 0; i < padded_dim; i += 64) {
        for (size_t j = 0; j < kRabitQQueryBits; ++j) {
            uint64_t result = 0;
            for (size_t k = 0; k < 64; ++k) {
                const uint16_t value = quantized[i + k];
                const uint16_t bit = (value >> (kRabitQQueryBits - 1 - j)) & 1U;
                result |= (static_cast<uint64_t>(bit) << (63 - k));
            }
            transposed[kRabitQQueryBits - j - 1] = result;
        }
        transposed += kRabitQQueryBits;
    }
}

RabitQQuantizer::RabitQQuantizer(int dim, int padded_dim, Metric metric)
    : dim_(dim),
      padded_dim_(padded_dim),
      metric_(metric == Metric::L2 ? Metric::L2 : Metric::INNER_PRODUCT),
      bin_code_size_(RabitQBinCodeSize(padded_dim_)),
      ext_code_size_(RabitQExtCodeSize(padded_dim_)),
      centroids_(static_cast<size_t>(kRabitQNumClusters) * static_cast<size_t>(dim_), 0.0f),
      rotated_centroids_(static_cast<size_t>(kRabitQNumClusters) * static_cast<size_t>(padded_dim_), 0.0f),
      distance_func_(GetDistanceFunc(metric_)),
      rotator_(dim_, padded_dim_) {
    if (dim_ <= 0 || padded_dim_ <= 0 || padded_dim_ < dim_) {
        throw std::invalid_argument("RabitQQuantizer requires positive dim and padded_dim >= dim");
    }
}

void RabitQQuantizer::BuildRandomRotation() {
    rotator_.Build();
}

void RabitQQuantizer::Load(const std::vector<uint8_t> &random_bits,
                           const std::vector<float> &centroids,
                           const std::vector<float> &rotated_centroids) {
    if (random_bits.size() != rotator_.RandomBitsSize()) {
        throw std::invalid_argument("RabitQQuantizer random_bits size mismatch");
    }
    if (centroids.size() != centroids_.size()) {
        throw std::invalid_argument("RabitQQuantizer centroids size mismatch");
    }
    if (rotated_centroids.size() != rotated_centroids_.size()) {
        throw std::invalid_argument("RabitQQuantizer rotated_centroids size mismatch");
    }

    rotator_.MutableRandomBits() = random_bits;
    centroids_ = centroids;
    rotated_centroids_ = rotated_centroids;
}

void RabitQQuantizer::Rotate(const float *vec, float *rotated) const {
    rotator_.Rotate(vec, rotated);
}

int RabitQQuantizer::ClosestCluster(const float *vec) const {
    if (!vec) {
        throw std::invalid_argument("RabitQQuantizer::ClosestCluster requires non-null vec");
    }
    if (!distance_func_) {
        throw std::runtime_error("RabitQQuantizer distance function is not initialized");
    }

    int closest_cluster = 0;
    float min_dist = std::numeric_limits<float>::max();
    for (int i = 0; i < kRabitQNumClusters; ++i) {
        const float dist = distance_func_(vec,
                                          centroids_.data() + static_cast<size_t>(i) * static_cast<size_t>(dim_),
                                          static_cast<uint32_t>(dim_));
        if (dist < min_dist) {
            min_dist = dist;
            closest_cluster = i;
        }
    }
    return closest_cluster;
}

void RabitQQuantizer::QuantizeScalar(const float *vec,
                                     const float *centroid,
                                     int total_bits,
                                     std::vector<uint16_t> &total_code,
                                     float &delta,
                                     float &vl,
                                     RabitQScalarQuantizerType sqtype) const {
    (void)sqtype;
    ScalarQuantize(vec, centroid, padded_dim_, total_bits, total_code, delta, vl);
}

RabitQEstimator::RabitQEstimator(int padded_dim, Metric metric, double rescaling_factor)
    : padded_dim_(padded_dim),
      metric_(metric == Metric::L2 ? Metric::L2 : Metric::INNER_PRODUCT),
      rescaling_factor_(rescaling_factor),
      rotated_query_(static_cast<size_t>(padded_dim_), 0.0f),
      q_to_centroids_(metric_ == Metric::INNER_PRODUCT
                          ? static_cast<size_t>(2 * kRabitQNumClusters)
                          : static_cast<size_t>(kRabitQNumClusters),
                      0.0f),
      l2_distance_(GetL2SqrFunc()),
      inner_product_(GetInnerProductFunc()),
      query_wrapper_(padded_dim_) {
    if (padded_dim_ <= 0) {
        throw std::invalid_argument("RabitQEstimator requires positive padded_dim");
    }
}

void RabitQEstimator::PrepareQuery(const float *query, uint32_t dim) {
    if (!query) {
        throw std::invalid_argument("RabitQEstimator::PrepareQuery requires non-null query");
    }
    if (!quantizer_) {
        throw std::runtime_error("RabitQEstimator::PrepareQuery requires quantizer");
    }
    if (dim > static_cast<uint32_t>(padded_dim_)) {
        throw std::invalid_argument("RabitQEstimator::PrepareQuery dim exceeds padded_dim");
    }

    std::fill(rotated_query_.begin(), rotated_query_.end(), 0.0f);
    quantizer_->Rotate(query, rotated_query_.data());
    GenerateQueryToCentroids();
    query_wrapper_.Preprocess(rotated_query_.data(), nullptr, 0.0f, 0.0f, nullptr, nullptr);
    prepared_ = true;
}

void RabitQEstimator::GenerateQueryToCentroids() {
    if (!quantizer_) {
        throw std::runtime_error("RabitQEstimator::GenerateQueryToCentroids requires quantizer");
    }

    const std::vector<float> &centroids = quantizer_->RotatedCentroids();
    if (metric_ == Metric::L2) {
        for (int i = 0; i < kRabitQNumClusters; ++i) {
            q_to_centroids_[static_cast<size_t>(i)] =
                std::sqrt(l2_distance_(rotated_query_.data(),
                                       centroids.data() + static_cast<size_t>(i) * static_cast<size_t>(padded_dim_),
                                       static_cast<uint32_t>(padded_dim_)));
        }
        return;
    }

    for (int i = 0; i < kRabitQNumClusters; ++i) {
        const float *centroid = centroids.data() + static_cast<size_t>(i) * static_cast<size_t>(padded_dim_);
        q_to_centroids_[static_cast<size_t>(i)] =
            -inner_product_(rotated_query_.data(), centroid, static_cast<uint32_t>(padded_dim_));
        q_to_centroids_[static_cast<size_t>(i + kRabitQNumClusters)] =
            std::sqrt(l2_distance_(rotated_query_.data(), centroid, static_cast<uint32_t>(padded_dim_)));
    }
}

} // namespace vex
