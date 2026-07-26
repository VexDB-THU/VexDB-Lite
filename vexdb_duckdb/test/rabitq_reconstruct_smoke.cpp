#include "rabitq/code_distancer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

namespace {

void CheckFinite(const std::vector<float> &values, const char *label)
{
    for (size_t i = 0; i < values.size(); ++i) {
        if (!std::isfinite(values[i])) {
            throw std::runtime_error(std::string(label) + " contains a non-finite value");
        }
    }
}

void CheckRoundTrip(int dim)
{
    const int padded_dim = RABITQ_PADDED_DIM(dim);
    rabitq::RaBitQuantizer quantizer(dim, padded_dim, Metric::L2);
    std::fill(quantizer.get_centroids(),
              quantizer.get_centroids() + HNSW_RABITQ_NUM_CLUSTERS * dim,
              0.0f);
    quantizer.train();

    std::vector<float> input(dim);
    for (int i = 0; i < dim; ++i) {
        input[i] = std::sin(static_cast<float>(i + 1) * 0.37f) +
                   std::cos(static_cast<float>(i + 1) * 0.11f) * 0.25f;
    }
    std::vector<float> rotated(padded_dim, 0.0f);
    std::vector<float> restored(dim, 0.0f);
    quantizer.rotate(input.data(), rotated.data());
    quantizer.inverse_rotate(rotated.data(), restored.data());

    float max_error = 0.0f;
    for (int i = 0; i < dim; ++i) {
        max_error = std::max(max_error, std::abs(input[i] - restored[i]));
    }
    if (max_error > 2e-4f) {
        throw std::runtime_error("RaBitQ rotate/inverse round trip exceeded tolerance");
    }
}

void CheckReconstruction(int dim, Metric metric)
{
    const int padded_dim = RABITQ_PADDED_DIM(dim);
    rabitq::RaBitQuantizer quantizer(dim, padded_dim, metric);
    std::fill(quantizer.get_centroids(),
              quantizer.get_centroids() + HNSW_RABITQ_NUM_CLUSTERS * dim,
              0.0f);
    quantizer.train();

    std::vector<float> input(dim);
    for (int i = 0; i < dim; ++i) {
        input[i] = std::sin(static_cast<float>(i + 1) * 0.19f) * 3.0f +
                   std::cos(static_cast<float>(i + 1) * 0.43f);
    }
    std::vector<uint8_t> code(rabitq::CodeSize(dim), 0);
    rabitq::EncodeCode(quantizer, dim, input.data(), code.data());

    std::vector<float> reconstructed(dim, 0.0f);
    quantizer.reconstruct(code.data(), reconstructed.data());
    CheckFinite(reconstructed, "RaBitQ reconstruction");

    double squared_error = 0.0;
    double squared_norm = 0.0;
    for (int i = 0; i < dim; ++i) {
        const double diff = static_cast<double>(input[i]) - reconstructed[i];
        squared_error += diff * diff;
        squared_norm += static_cast<double>(input[i]) * input[i];
    }
    const double relative_error = std::sqrt(squared_error / squared_norm);
    if (relative_error > 0.12) {
        throw std::runtime_error("RaBitQ reconstruction relative error exceeded 0.12");
    }

    std::fill(input.begin(), input.end(), 0.0f);
    rabitq::EncodeCode(quantizer, dim, input.data(), code.data());
    quantizer.reconstruct(code.data(), reconstructed.data());
    CheckFinite(reconstructed, "zero-vector RaBitQ reconstruction");
    for (float value : reconstructed) {
        if (std::abs(value) > 1e-6f) {
            throw std::runtime_error("zero-vector RaBitQ reconstruction is not zero");
        }
    }
}

} // namespace

int main()
{
    try {
        CheckRoundTrip(8);
        CheckRoundTrip(64);
        CheckReconstruction(8, Metric::L2);
        CheckReconstruction(64, Metric::L2);
        CheckReconstruction(8, Metric::INNER_PRODUCT);
        CheckReconstruction(64, Metric::INNER_PRODUCT);
        std::puts("rabitq_reconstruct_smoke: ok");
        return 0;
    } catch (const std::exception &ex) {
        std::fprintf(stderr, "rabitq_reconstruct_smoke: fail: %s\n", ex.what());
        return 1;
    }
}
