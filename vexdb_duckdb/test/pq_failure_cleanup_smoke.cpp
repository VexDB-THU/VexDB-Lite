#include "quantizer/annkmeans.h"
#include "quantizer/product_quantizer.h"

#include <cstdlib>
#include <cmath>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <vector>

namespace {

struct TrackingAllocator {
    std::unordered_map<void *, size_t> live;

    static void *Alloc(size_t bytes, void *user) {
        auto &self = *static_cast<TrackingAllocator *>(user);
        void *ptr = std::malloc(bytes);
        if (ptr) self.live.emplace(ptr, bytes);
        return ptr;
    }

    static void Free(void *ptr, void *user) {
        if (!ptr) return;
        auto &self = *static_cast<TrackingAllocator *>(user);
        self.live.erase(ptr);
        std::free(ptr);
    }

    size_t LiveBytes() const {
        size_t result = 0;
        for (const auto &entry : live) result += entry.second;
        return result;
    }
};

float ThrowingDistance(const void *, const void *, uint16_t) {
    throw vex::quantizer::VexQuantizerError("forced distance failure");
}

float L2Distance(const void *lhs, const void *rhs, uint16_t dim) {
    const auto *a = static_cast<const float *>(lhs);
    const auto *b = static_cast<const float *>(rhs);
    double sum = 0.0;
    for (uint16_t i = 0; i < dim; i++) {
        const double delta = static_cast<double>(a[i]) - b[i];
        sum += delta * delta;
    }
    return static_cast<float>(std::sqrt(sum));
}

void Require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    TrackingAllocator tracker;
    vex::quantizer::PQContext ctx;
    ctx.allocator.alloc_fn = TrackingAllocator::Alloc;
    ctx.allocator.alloc_huge_fn = TrackingAllocator::Alloc;
    ctx.allocator.free_fn = TrackingAllocator::Free;
    ctx.allocator.user = &tracker;

    constexpr size_t sample_count = 300;
    constexpr size_t dim = 4;
    constexpr size_t center_count = 256;
    std::vector<float> sample_data(sample_count * dim, 0.5f);
    std::vector<float> center_data(center_count * dim, 0.0f);
    vex::quantizer::PQFloatArray samples{
        sample_data.data(), sample_count, sample_count, dim};
    vex::quantizer::PQFloatArray centers{
        center_data.data(), 0, center_count, dim};
    vex::quantizer::KMeansState state;
    state.distance_fn = ThrowingDistance;
    state.skip_check_duplicate = true;

    bool ann_failed = false;
    try {
        vex::quantizer::AnnKmeans(state, samples, centers, 1024 * 1024, ctx);
    } catch (const vex::quantizer::VexQuantizerError &) {
        ann_failed = true;
    }
    Require(ann_failed, "AnnKmeans did not propagate the forced failure");
    Require(tracker.LiveBytes() == 0,
            "AnnKmeans leaked scratch allocations after failure");

    vex::quantizer::ProductQuantizer pq;
    pq.set_basic_values(dim, 1, 8);
    pq.set_derived_values(ctx);
    const size_t codebook_bytes = dim * center_count * sizeof(float);
    Require(tracker.LiveBytes() == codebook_bytes,
            "ProductQuantizer codebook allocation accounting is wrong");

    centers.length = 0;
    bool pq_failed = false;
    try {
        pq.train(state, samples, 1024 * 1024, ctx);
    } catch (const vex::quantizer::VexQuantizerError &) {
        pq_failed = true;
    }
    Require(pq_failed, "ProductQuantizer did not propagate the forced failure");
    Require(tracker.LiveBytes() == codebook_bytes,
            "ProductQuantizer leaked per-training allocations after failure");
    pq.free_resources(ctx);
    Require(tracker.LiveBytes() == 0,
            "ProductQuantizer did not release its codebook");

    // The outer subquantizer order stands in for different worker schedules.
    // Each subquantizer must receive its own fixed random stream.
    std::vector<float> reproducible_data(sample_count * dim);
    for (size_t i = 0; i < reproducible_data.size(); i++) {
        reproducible_data[i] = static_cast<float>(
            std::sin(static_cast<double>(i + 1) * 0.031));
    }
    vex::quantizer::PQFloatArray reproducible_samples{
        reproducible_data.data(), sample_count, sample_count, dim};
    vex::quantizer::KMeansState reproducible_state;
    reproducible_state.distance_fn = L2Distance;
    reproducible_state.skip_check_duplicate = true;

    vex::quantizer::PQContext serial_ctx;
    vex::quantizer::PQContext reverse_ctx;
    reverse_ctx.parallel.run_fn = [](
        size_t n, const vex::quantizer::PQParallelExecutor::TaskFn &body, void *) {
        for (size_t i = n; i > 0; i--) body(i - 1);
    };
    vex::quantizer::ProductQuantizer serial_pq;
    vex::quantizer::ProductQuantizer reverse_pq;
    for (auto *candidate : {&serial_pq, &reverse_pq}) {
        candidate->set_basic_values(dim, dim, 8);
    }
    serial_pq.set_derived_values(serial_ctx);
    reverse_pq.set_derived_values(reverse_ctx);
    serial_pq.train(reproducible_state, reproducible_samples, 1024 * 1024,
                    serial_ctx);
    reverse_pq.train(reproducible_state, reproducible_samples, 1024 * 1024,
                     reverse_ctx);
    const size_t reproducible_bytes = serial_pq.get_centroids_size() * sizeof(float);
    Require(std::memcmp(serial_pq.centroids, reverse_pq.centroids,
                        reproducible_bytes) == 0,
            "PQ codebook depends on subquantizer scheduling order");
    serial_pq.free_resources(serial_ctx);
    reverse_pq.free_resources(reverse_ctx);

    std::cout << "pq_failure_cleanup_smoke: ok\n";
    return 0;
}
