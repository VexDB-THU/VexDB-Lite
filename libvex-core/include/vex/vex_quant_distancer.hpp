#ifndef VEX_QUANT_DISTANCER_HPP
#define VEX_QUANT_DISTANCER_HPP

#include "vex/vex_quantizer.hpp"

#include <cstdint>
#include <vector>

namespace vex {

struct QuantDistancerCaps {
    bool has_estimation_func = false;
    bool need_refine = false;
    bool supports_cluster_maintenance = false;
};

class QuantDistancer {
public:
    virtual ~QuantDistancer() = default;

    virtual QuantDistancerCaps Capabilities() const = 0;
    virtual void Train(const float *vectors, uint32_t count, uint32_t dim) = 0;
    virtual void PrepareQuery(const float *query, uint32_t dim) = 0;

    virtual uint32_t CodeSize() const = 0;
    virtual void ComputeCode(const float *vec, uint8_t *code) const = 0;
    virtual float DistanceSingle(const uint8_t *code) const = 0;

    virtual void DistanceBatch(const uint8_t *const *codes, uint32_t count, float *out) const {
        for (uint32_t i = 0; i < count; ++i) {
            out[i] = DistanceSingle(codes[i]);
        }
    }
};

class PQDistancerCore final : public QuantDistancer {
public:
    explicit PQDistancerCore(uint32_t pq_m = 0) : pq_m_(pq_m) {
    }

    QuantDistancerCaps Capabilities() const override {
        QuantDistancerCaps caps;
        caps.has_estimation_func = false;
        caps.need_refine = true;
        caps.supports_cluster_maintenance = false;
        return caps;
    }

    void Train(const float *vectors, uint32_t count, uint32_t dim) override;
    void PrepareQuery(const float *query, uint32_t dim) override;
    void LoadQuantizer(const ProductQuantizer &pq);

    uint32_t CodeSize() const override {
        return pq_.CodeSize();
    }

    void ComputeCode(const float *vec, uint8_t *code) const override {
        pq_.Encode(vec, code);
    }

    float DistanceSingle(const uint8_t *code) const override {
        if (!prepared_ || dist_table_.empty() || pq_.m == 0) {
            return 0.0f;
        }
        return ProductQuantizer::DistanceFromTable(code, dist_table_.data(), pq_.m);
    }

    void DistanceBatch(const uint8_t *const *codes, uint32_t count, float *out) const override;

    const ProductQuantizer &Quantizer() const {
        return pq_;
    }

private:
    void DistanceBatchScalar(const uint8_t *const *codes, uint32_t count, float *out) const;
    bool TryDistanceBatchSIMD(const uint8_t *const *codes, uint32_t count, float *out) const;

    ProductQuantizer pq_{};
    uint32_t pq_m_ = 0;
    std::vector<float> dist_table_;
    bool prepared_ = false;
};

class RabitQDistancerStub final : public QuantDistancer {
public:
    QuantDistancerCaps Capabilities() const override {
        // Keep behavior aligned with PG RabitQ contract surface:
        // estimation exists, refine is not required.
        QuantDistancerCaps caps;
        caps.has_estimation_func = true;
        caps.need_refine = false;
        caps.supports_cluster_maintenance = false;
        return caps;
    }

    void Train(const float *, uint32_t, uint32_t dim) override {
        dim_ = dim;
    }

    void PrepareQuery(const float *, uint32_t dim) override {
        dim_ = dim;
    }

    uint32_t CodeSize() const override {
        return 0;
    }

    void ComputeCode(const float *, uint8_t *) const override {
    }

    float DistanceSingle(const uint8_t *) const override {
        return 0.0f;
    }

private:
    uint32_t dim_ = 0;
};

} // namespace vex

#endif // VEX_QUANT_DISTANCER_HPP
