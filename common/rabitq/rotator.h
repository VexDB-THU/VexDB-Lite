/**
 * Copyright (c) 2026 VexDB-THU
 * RaBitQ Random Orthogonal Matrix
 */

#ifndef RABITQ_ROTATOR_H
#define RABITQ_ROTATOR_H

#include <vector>
#include "rabitq/platform.h"
#include "distance/core/distance.h"

namespace rabitq {
class FhtKacRotator {
public:
    FhtKacRotator(int dim, int padded_dim)
        : _dim(dim),
          _padded_dim(padded_dim),
          _flip(4 * _padded_dim / kByteLen, 0)
    {
        auto kernels = ann_helper::get_rabitq_kernels();
        _flip_sign = kernels.flip_sign;
        _kacs_walk = kernels.kacs_walk;
        uint32 bottom_log_dim = floor_log2(_dim);
        _trunc_dim = 1 << bottom_log_dim;
        _fac = 1.0f / std::sqrt(static_cast<float>(_trunc_dim));
        _fht_float = ann_helper::get_fht_func(bottom_log_dim);
        if (!_fht_float) {
            fail("vector dimension is too large for FHT");
        }
        if (!_flip_sign || !_kacs_walk) {
            fail("SIMD rotation kernels are unavailable");
        }
    }

    FhtKacRotator() = delete;

    void build();
    void rotate(float *data, float *rotated);

    char *get_random_matrix() { return (char *)_flip.data(); }
    size_t get_random_matrix_size() const { return sizeof(uint8) * _flip.size(); }

    void destroy() {
        _flip.clear();
        _flip.shrink_to_fit();
    }

private:
    uint32 floor_log2(uint32 x) {
        uint32 ret = 0;
        while (x > 1) {
            ++ret;
            x >>= 1;
        }
        return ret;
    }

    void vec_rescale(float* data, int dim, float val) {
        for (int i = 0; i < dim; ++i) {
            data[i] *= val;
        }
    }

private:
    static constexpr size_t kByteLen = 8;

    uint32 _dim{0};
    uint32 _padded_dim{0};
    uint32 _trunc_dim{0};
    float _fac{0.0f};
    ann_helper::fht_func _fht_float;
    ann_helper::flip_sign_func _flip_sign;
    ann_helper::kacs_walk_func _kacs_walk;
    std::vector<uint8, HostAllocator<uint8>> _flip;
};

} /* namespace rabitq */

#endif /* RABITQ_ROTATOR_H */
