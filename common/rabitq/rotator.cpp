/**
 * Copyright (c) 2026 VexDB-THU
 * RaBitQ Random Orthogonal Matrix
 */

#include <cstring>

#include "rabitq/utils.h"
#include "rabitq/rotator.h"

namespace rabitq {

void FhtKacRotator::build()
{
    random_bytes(_flip.data(), _flip.size());
}

void FhtKacRotator::rotate(float *data, float *rotated)
{
    std::memcpy(rotated, data, sizeof(float) * _dim);
    std::fill(rotated + _dim, rotated + _padded_dim, 0);

    if (_trunc_dim == _padded_dim) {
        _flip_sign(_flip.data(), rotated, _padded_dim);
        _fht_float(rotated);
        vec_rescale(rotated, _trunc_dim, _fac);

        _flip_sign(_flip.data() + (_padded_dim / kByteLen), rotated, _padded_dim);
        _fht_float(rotated);
        vec_rescale(rotated, _trunc_dim, _fac);

        _flip_sign(
            _flip.data() + (2 * _padded_dim / kByteLen), rotated, _padded_dim
        );
        _fht_float(rotated);
        vec_rescale(rotated, _trunc_dim, _fac);

        _flip_sign(
            _flip.data() + (3 * _padded_dim / kByteLen), rotated, _padded_dim
        );
        _fht_float(rotated);
        vec_rescale(rotated, _trunc_dim, _fac);

        return;
    }

    size_t start = _padded_dim - _trunc_dim;

    _flip_sign(_flip.data(), rotated, _padded_dim);
    _fht_float(rotated);
    vec_rescale(rotated, _trunc_dim, _fac);
    _kacs_walk(rotated, _padded_dim);

    _flip_sign(_flip.data() + (_padded_dim / kByteLen), rotated, _padded_dim);
    _fht_float(rotated + start);
    vec_rescale(rotated + start, _trunc_dim, _fac);
    _kacs_walk(rotated, _padded_dim);

    _flip_sign(_flip.data() + (2 * _padded_dim / kByteLen), rotated, _padded_dim);
    _fht_float(rotated);
    vec_rescale(rotated, _trunc_dim, _fac);
    _kacs_walk(rotated, _padded_dim);

    _flip_sign(_flip.data() + (3 * _padded_dim / kByteLen), rotated, _padded_dim);
    _fht_float(rotated + start);
    vec_rescale(rotated + start, _trunc_dim, _fac);
    _kacs_walk(rotated, _padded_dim);

    /* This can be removed if we don't care about the absolute value of similarities */
    vec_rescale(rotated, _padded_dim, 0.25f);
}

void FhtKacRotator::inverse_rotate(const float *rotated, float *data)
{
    float *work = alloc_floatvector(_padded_dim, 1);
    std::memcpy(work, rotated, sizeof(float) * _padded_dim);
    inverse_rotate_inplace(work, data);
    free_vector(work);
}

void FhtKacRotator::inverse_rotate_inplace(float *work, float *data)
{
    if (_trunc_dim == _padded_dim) {
        // A normalized Hadamard transform and a sign flip are both their own
        // inverse. Reverse the four stages in the opposite order.
        for (int stage = 3; stage >= 0; --stage) {
            _fht_float(work);
            vec_rescale(work, _trunc_dim, _fac);
            _flip_sign(_flip.data() + stage * (_padded_dim / kByteLen),
                       work, _padded_dim);
        }
    } else {
        // Each Kac walk K satisfies K*K = 2I. Four forward walks are normalized
        // by the final 1/4 scale, so the reversed four unscaled walks need the
        // same 1/4 scale after all stages have been undone.
        const size_t start = _padded_dim - _trunc_dim;
        for (int stage = 3; stage >= 0; --stage) {
            _kacs_walk(work, _padded_dim);
            float *segment = (stage == 1 || stage == 3) ? work + start : work;
            _fht_float(segment);
            vec_rescale(segment, _trunc_dim, _fac);
            _flip_sign(_flip.data() + stage * (_padded_dim / kByteLen),
                       work, _padded_dim);
        }
        vec_rescale(work, _padded_dim, 0.25f);
    }

    std::memcpy(data, work, sizeof(float) * _dim);
}

} /* namespace rabitq */
