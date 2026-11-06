#include "quantizer/annkmeans.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

using namespace vex::quantizer;

static PQFloatArray View(std::vector<float> &values, size_t dim) {
    PQFloatArray out;
    out.data = values.data();
    out.dim = dim;
    out.length = out.maxlen = values.size() / dim;
    return out;
}

template <class Fn>
static void Reject(Fn fn, const std::string &message) {
    try { fn(); }
    catch (const VexQuantizerError &error) {
        if (std::string(error.what()).find(message) == std::string::npos) throw;
        return;
    }
    throw std::runtime_error("missing rejection: " + message);
}

static float SquaredL2(const void *a, const void *b, uint16_t dim) {
    auto x = static_cast<const float *>(a), y = static_cast<const float *>(b);
    float distance = 0;
    for (uint16_t j = 0; j < dim; ++j) distance += (x[j] - y[j]) * (x[j] - y[j]);
    return distance;
}

int main() {
    KMeansState state;
    state.distance_fn = SquaredL2;
    state.allow_low_diversity_centers = true;
    std::vector<float> low {0, 1, 2, 0, 1, 2}, high {0, 1, 2, 3, 4, 5};
    std::vector<float> duplicate {0, 1, 2, 2}, unique {0, 1, 2, 3};
    auto samples = View(low, 1), centers = View(duplicate, 1);
    ValidateKMeansCenters(state, samples, centers);
    auto strict = state;
    strict.allow_low_diversity_centers = false;
    Reject([&] { ValidateKMeansCenters(strict, samples, centers); }, "duplicate centers");
    samples = View(high, 1);
    Reject([&] { ValidateKMeansCenters(state, samples, centers); }, "duplicate centers");
    centers = View(unique, 1);
    ValidateKMeansCenters(state, samples, centers);
    centers.length--;
    Reject([&] { ValidateKMeansCenters(state, samples, centers); }, "not enough centers");
    centers.length++;

    // The legacy explicit backend override never suppresses finite checks.
    auto legacy = state;
    legacy.skip_check_duplicate = true;
    for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        unique[0] = bad;
        Reject([&] { ValidateKMeansCenters(legacy, samples, centers); }, "detected in centers");
        unique[0] = 0;
        high[0] = bad;
        Reject([&] { ValidateKMeansCenters(state, samples, centers); }, "non-finite training sample");
        high[0] = 0;
    }
    std::vector<float> signed_zero {0.0f, -0.0f, 0.0f, -0.0f};
    std::vector<float> zero_centers {0.0f, -0.0f};
    ValidateKMeansCenters(state, View(signed_zero, 1), View(zero_centers, 1));

    // Check each subspace independently: a constant column does not disable
    // validation for a different, high-diversity column in the same dataset.
    std::vector<float> constant(512, 0.0f), diverse(512);
    for (size_t i = 0; i < diverse.size(); ++i) diverse[i] = float(i);
    std::vector<float> collapsed(256, 0.0f);
    ValidateKMeansCenters(state, View(constant, 1), View(collapsed, 1));
    Reject([&] { ValidateKMeansCenters(state, View(diverse, 1), View(collapsed, 1)); }, "duplicate centers");

    // Exercise real clustering, including the >K-samples low-diversity path.
    PQContext context;
    std::unordered_set<void *> allocated;
    context.allocator.alloc_fn = [](size_t bytes, void *user) -> void * {
        void *value = std::malloc(bytes);
        static_cast<std::unordered_set<void *> *>(user)->insert(value);
        return value;
    };
    context.allocator.free_fn = [](void *value, void *user) {
        auto &set = *static_cast<std::unordered_set<void *> *>(user);
        if (set.erase(value) != 1) std::terminate();
        std::free(value);
    };
    context.allocator.user = &allocated;
    for (auto *data : {&constant, &diverse}) {
        std::vector<float> trained(256);
        auto input = View(*data, 1), output = View(trained, 1);
        output.length = 0;
        AnnKmeans(state, input, output, 0, context);
        ValidateKMeansCenters(state, input, output);
        if (!allocated.empty()) throw std::runtime_error("K-means scratch leaked");
    }
    diverse[0] = std::numeric_limits<float>::quiet_NaN();
    auto input = View(diverse, 1);
    std::vector<float> trained(256);
    auto output = View(trained, 1);
    output.length = 0;
    Reject([&] { AnnKmeans(state, input, output, 0, context); }, "non-finite training sample");
    if (!allocated.empty()) throw std::runtime_error("invalid input allocated scratch");
    std::cout << "PASS: actual-data diversity policy, strict unexpected duplicates, signed zero, "
                 "nonfinite/count checks, per-subspace independence and real clustering\n";
}
