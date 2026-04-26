#ifndef VEX_RABITQ_HPP
#define VEX_RABITQ_HPP

#include "vex/vex_distance.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <queue>
#include <random>
#include <vector>

namespace vex {

constexpr int kRabitQExBits = 8;
constexpr int kRabitQNumClusters = 16;
constexpr size_t kRabitQQueryBits = 4;

constexpr inline int RabitQPaddedDim(int dim) {
    return ((dim + 63) / 64) * 64;
}

constexpr inline int RabitQBinCodeSize(int dim) {
    return ((dim + 63) / 64) * static_cast<int>(sizeof(uint64_t));
}

constexpr inline int RabitQExtCodeSize(int dim) {
    return ((dim + 7) / 8) * kRabitQExBits * static_cast<int>(sizeof(uint8_t));
}

enum class RabitQScalarQuantizerType : uint8_t {
    RECONSTRUCTION,
    UNBIASED_ESTIMATION,
    PLAIN
};

struct RabitQEstimateRecord {
    float ip_x0_qr = 0.0f;
    float est_dist = 0.0f;
    float low_dist = 0.0f;

    bool operator<(const RabitQEstimateRecord &other) const {
        return est_dist < other.est_dist;
    }
};

template <typename T>
inline void RabitQGenerateRandomMatrix(T *matrix, int rows, int cols) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::normal_distribution<T> dist(0, 1);
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            matrix[static_cast<size_t>(i) * static_cast<size_t>(cols) + static_cast<size_t>(j)] = dist(gen);
        }
    }
}

template <typename T>
inline void RabitQRowwiseNormAbs(T *matrix, long rows, long cols) {
    for (long i = 0; i < rows; ++i) {
        T norm = 0.0f;
        for (long j = 0; j < cols; ++j) {
            norm += matrix[i * cols + j] * matrix[i * cols + j];
        }
        norm = std::sqrt(norm);
        if (norm > 0.0f) {
            for (long j = 0; j < cols; ++j) {
                matrix[i * cols + j] = std::abs(matrix[i * cols + j]) / norm;
            }
        } else {
            for (long j = 0; j < cols; ++j) {
                matrix[i * cols + j] = 0.0f;
            }
        }
    }
}

template <typename T>
double RabitQBestRescaleFactor(T *o_abs, int dim, int ex_bits);

double RabitQConstScalingFactor(int dim, int ex_bits);

class RabitQRotator {
public:
    RabitQRotator(int dim, int padded_dim);

    void Build();
    void Rotate(const float *data, float *rotated) const;

    const std::vector<uint8_t> &RandomBits() const {
        return flip_;
    }

    std::vector<uint8_t> &MutableRandomBits() {
        return flip_;
    }

    size_t RandomBitsSize() const {
        return flip_.size();
    }

    int Dimension() const {
        return dim_;
    }

    int PaddedDimension() const {
        return padded_dim_;
    }

private:
    static uint32_t FloorLog2(uint32_t x);
    static void Rescale(float *data, int dim, float factor);

private:
    static constexpr size_t kByteLen = 8;

    int dim_ = 0;
    int padded_dim_ = 0;
    int trunc_dim_ = 0;
    float factor_ = 0.0f;
    std::vector<uint8_t> flip_;
};

class RabitQQuery {
public:
    explicit RabitQQuery(int padded_dim);

    void Preprocess(const float *rotated_query,
                    const float *centroid,
                    float delta,
                    float vl,
                    float *out_g_add,
                    float *out_g_error);

    float Delta() const {
        return delta_;
    }

    float Vl() const {
        return vl_;
    }

    float K1xSumQ() const {
        return g_k1_sum_q_;
    }

    float KbxSumQ() const {
        return g_kb_sum_q_;
    }

    const uint64_t *QueryBits() const {
        return query_bits_.data();
    }

private:
    void TransposeBits(const uint16_t *quantized, uint64_t *transposed, size_t padded_dim) const;

private:
    int padded_dim_ = 0;
    float g_k1_sum_q_ = 0.0f;
    float g_kb_sum_q_ = 0.0f;
    float delta_ = 0.0f;
    float vl_ = 0.0f;
    std::vector<uint64_t> query_bits_;
    std::vector<float> centroid_;
    std::vector<uint16_t> quantized_query_;
};

class RabitQQuantizer {
public:
    RabitQQuantizer(int dim, int padded_dim, Metric metric);

    void BuildRandomRotation();

    void Load(const std::vector<uint8_t> &random_bits,
              const std::vector<float> &centroids,
              const std::vector<float> &rotated_centroids);

    void Rotate(const float *vec, float *rotated) const;

    int ClosestCluster(const float *vec) const;

    void QuantizeScalar(const float *vec,
                        const float *centroid,
                        int total_bits,
                        std::vector<uint16_t> &total_code,
                        float &delta,
                        float &vl,
                        RabitQScalarQuantizerType sqtype = RabitQScalarQuantizerType::RECONSTRUCTION) const;

    int Dimension() const {
        return dim_;
    }

    int PaddedDimension() const {
        return padded_dim_;
    }

    Metric DistanceMetric() const {
        return metric_;
    }

    int BinCodeSize() const {
        return bin_code_size_;
    }

    int ExtCodeSize() const {
        return ext_code_size_;
    }

    double QueryRescalingFactor() const {
        return rescaling_factor_;
    }

    void SetRescalingFactor(double factor) {
        rescaling_factor_ = factor;
    }

    RabitQRotator &Rotator() {
        return rotator_;
    }

    const RabitQRotator &Rotator() const {
        return rotator_;
    }

    std::vector<float> &Centroids() {
        return centroids_;
    }

    const std::vector<float> &Centroids() const {
        return centroids_;
    }

    std::vector<float> &RotatedCentroids() {
        return rotated_centroids_;
    }

    const std::vector<float> &RotatedCentroids() const {
        return rotated_centroids_;
    }

private:
    int dim_ = 0;
    int padded_dim_ = 0;
    Metric metric_ = Metric::L2;
    int bin_code_size_ = 0;
    int ext_code_size_ = 0;
    std::vector<float> centroids_;
    std::vector<float> rotated_centroids_;
    distance_func_t distance_func_ = nullptr;
    RabitQRotator rotator_;
    double rescaling_factor_ = -1.0;
};

class RabitQEstimator {
public:
    RabitQEstimator(int padded_dim, Metric metric, double rescaling_factor);

    void SetQuantizer(const RabitQQuantizer *quantizer) {
        quantizer_ = quantizer;
    }

    const RabitQQuantizer *GetQuantizer() const {
        return quantizer_;
    }

    void PrepareQuery(const float *query, uint32_t dim);

    const std::vector<float> &RotatedQuery() const {
        return rotated_query_;
    }

    const std::vector<float> &QueryToCentroids() const {
        return q_to_centroids_;
    }

    const RabitQQuery &QueryWrapper() const {
        return query_wrapper_;
    }

    Metric DistanceMetric() const {
        return metric_;
    }

    bool Prepared() const {
        return prepared_;
    }

private:
    void GenerateQueryToCentroids();

private:
    int padded_dim_ = 0;
    Metric metric_ = Metric::L2;
    double rescaling_factor_ = -1.0;
    std::vector<float> rotated_query_;
    std::vector<float> q_to_centroids_;
    distance_func_t l2_distance_ = nullptr;
    distance_func_t inner_product_ = nullptr;
    const RabitQQuantizer *quantizer_ = nullptr;
    RabitQQuery query_wrapper_;
    bool prepared_ = false;
};

} // namespace vex

#endif // VEX_RABITQ_HPP
