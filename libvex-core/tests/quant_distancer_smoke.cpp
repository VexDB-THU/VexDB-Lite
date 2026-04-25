#include "vex/vex_quant_distancer.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

int RunPQCase(uint32_t dim, uint32_t pq_m) {
    constexpr uint32_t n = 512;

    std::vector<float> train(static_cast<size_t>(n) * dim, 0.0f);
    for (uint32_t i = 0; i < n; ++i) {
        for (uint32_t d = 0; d < dim; ++d) {
            train[static_cast<size_t>(i) * dim + d] = static_cast<float>((i + d) % 17) * 0.1f;
        }
    }

    vex::PQDistancerCore pq_distancer(pq_m);
    pq_distancer.Train(train.data(), n, dim);
    auto caps = pq_distancer.Capabilities();
    if (caps.has_estimation_func || !caps.need_refine || caps.supports_cluster_maintenance) {
        std::cerr << "unexpected PQ capability flags" << std::endl;
        return 1;
    }

    std::vector<float> query(dim, 0.0f);
    for (uint32_t d = 0; d < dim; ++d) {
        query[d] = static_cast<float>(d) * 0.2f;
    }
    pq_distancer.PrepareQuery(query.data(), dim);

    std::vector<uint8_t> code_a(pq_distancer.CodeSize(), 0);
    std::vector<uint8_t> code_b(pq_distancer.CodeSize(), 0);
    pq_distancer.ComputeCode(train.data(), code_a.data());
    pq_distancer.ComputeCode(train.data() + dim, code_b.data());

    float da = pq_distancer.DistanceSingle(code_a.data());
    float db = pq_distancer.DistanceSingle(code_b.data());
    if (!std::isfinite(da) || !std::isfinite(db)) {
        std::cerr << "PQ distance must be finite" << std::endl;
        return 2;
    }

    const uint8_t *batch_codes[2] = {code_a.data(), code_b.data()};
    float out[2] = {0.0f, 0.0f};
    pq_distancer.DistanceBatch(batch_codes, 2, out);
    if (!std::isfinite(out[0]) || !std::isfinite(out[1])) {
        std::cerr << "PQ batch distance must be finite" << std::endl;
        return 3;
    }
    if (std::fabs(out[0] - da) > 1e-5f || std::fabs(out[1] - db) > 1e-5f) {
        std::cerr << "PQ batch distance must match single distance" << std::endl;
        return 5;
    }

    return 0;
}

} // namespace

int main() {
    for (uint32_t pq_m : {2u, 4u, 8u, 16u, 32u}) {
        int rc = RunPQCase(/*dim=*/32, pq_m);
        if (rc != 0) {
            return rc;
        }
    }

    vex::RabitQDistancerStub rbq_stub;
    auto rbq_caps = rbq_stub.Capabilities();
    if (!rbq_caps.has_estimation_func || rbq_caps.need_refine || rbq_caps.supports_cluster_maintenance) {
        std::cerr << "unexpected RabitQ stub capability flags" << std::endl;
        return 4;
    }

    return 0;
}
