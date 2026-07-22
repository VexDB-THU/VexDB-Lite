/**
 * Copyright (c) 2026 VexDB-THU
 * DiskANN RaBitQ implementation
 */

#include <cmath>
#include "rabitq/rabitq.h"

namespace rabitq {
constexpr float kConstEpsilon = 1.9f;

static float compute_error_factor(float l2_sqr, float l2_norm,
    float quantized_norm_sqr, float inner_product, int padded_dim)
{
    if (padded_dim <= 1) {
        return 0.0f;
    }
    double ratio = (static_cast<double>(l2_sqr) * quantized_norm_sqr) /
        (static_cast<double>(inner_product) * inner_product) - 1.0;
    double radicand = std::max(0.0, ratio / (padded_dim - 1));
    return l2_norm * kConstEpsilon * static_cast<float>(std::sqrt(radicand));
}

void RaBitQuantizer::train()
{
    _rotator->build();
    for (int i = 0; i < HNSW_RABITQ_NUM_CLUSTERS; ++i) {
        _rotator->rotate(_centroids + i * _dim, _rotated_centroids + i * _padded_dim);
    }
}

int RaBitQuantizer::quantize(float *vec, char *bin_data, char *ext_data)
{
    /* rotate the raw vector */
    float *rotated_vec = alloc_floatvector(_padded_dim, 1);
    _rotator->rotate(vec, rotated_vec);

    /* get the closest centroid */
    int closest_cluster = compute_closest_cluster(vec);
    float *rotated_centroid = _rotated_centroids + closest_cluster * _padded_dim;

    BinDataWithFactors bin_data_with_factors(bin_data, _bin_code_size);
    quantize_bin_code(rotated_vec, rotated_centroid, &bin_data_with_factors);

    ExtDataWithFactors ext_data_with_factors(ext_data, _ext_code_size);
    quantize_ext_code(rotated_vec, rotated_centroid, &ext_data_with_factors);

    free_vector(rotated_vec);
    return closest_cluster;
}

void RaBitQuantizer::pack_bin_code(int *bin_code_int, uint64 *__restrict__ bin_code)
{
    constexpr int kTypeBits = sizeof(uint64) * 8;
    for (int i = 0; i < _padded_dim; i += kTypeBits) {
        uint64 cur = 0;
        for (int j = 0; j < kTypeBits; ++j) {
            cur |= (static_cast<uint64>(bin_code_int[i + j]) << (kTypeBits - 1 - j));
        }
        *bin_code = cur;
        ++bin_code;
    }
}

void RaBitQuantizer::one_bit_code(float *vec, float *centroid, float *residual, int *bin_code_int)
{
    vec_sub(vec, centroid, residual);
    for (int i = 0; i < _padded_dim; ++i) {
        bin_code_int[i] = residual[i] > 0 ? 1 : 0;
    }
}

void RaBitQuantizer::quantize_bin_code(float *vec, float *centroid, BinDataWithFactors *bin_data)
{
    /* residual = P^-1 * (or - c), bin_data = xb */
    float *residual = alloc_array<float>(_padded_dim);
    int *bin_code_int = alloc_array<int>(_padded_dim);

    one_bit_code(vec, centroid, residual, bin_code_int);
    pack_bin_code(bin_code_int, bin_data->bin_code);

    /* xu_cb = x_u + cb, xu_cb has same direction and different length with x_bar */
    float cb = -((1 << 1) - 1) / 2.0f;
    float *xu_cb = alloc_array<float>(_padded_dim);
    vec_add<int>(bin_code_int, cb, xu_cb);

    /* distance to centroid */
    float l2_sqr = dot_product(residual, residual); /* ||or - c||^2 */
    float l2_norm = std::sqrt(l2_sqr); /* ||or - c|| */

    /* dot product between residual and xu_cb */
    float ip_resi_xucb = dot_product(residual, xu_cb);
    /* dot product between centroid and xu_cb */
    float ip_cent_xucb = dot_product(centroid, xu_cb);

    /* A vector can equal its centroid exactly. Keep that valid case exact
     * instead of producing sqrt(-1) and poisoning graph comparisons with NaN. */
    if (!(l2_sqr > 0.0f) || !(ip_resi_xucb > 0.0f)) {
        *(bin_data->f_add) = _metric == Metric::L2 ? 0.0f : 1.0f;
        *(bin_data->f_rescale) = 0.0f;
        *(bin_data->f_error) = 0.0f;
        free_mem(bin_code_int);
        free_mem(residual);
        free_mem(xu_cb);
        return;
    }

    /* We use unnormalized vector to get error factor. To be more specific,
     * sqrt((1 - <o, o_bar>^2) / <o, o_bar>^2) / sqrt(padded_dim - 1) = 3rd item in following expression
     */
    float tmp_error = compute_error_factor(l2_sqr, l2_norm,
        dot_product(xu_cb, xu_cb), ip_resi_xucb, _padded_dim);

    /* 3 factors used for distance estimation, please refer to document for more info.
     * For f_rescale and 2nd item of f_add, we use the dot product of raw residual (rather
     * than the normalized one) as the denominator, thus we need to multiply another l2norm.
     * For (ip_cent_xucb / ip_resi_xucb), the norm of xucb does not matter since it is also
     * in numerator.
     */
    if (_metric == Metric::L2) {
        *(bin_data->f_add) = l2_sqr + 2 * l2_sqr * ip_cent_xucb / ip_resi_xucb;
        *(bin_data->f_rescale) = -2 * l2_sqr / ip_resi_xucb;
        *(bin_data->f_error) = 2 * tmp_error;
    } else if (_metric == Metric::INNER_PRODUCT) {
        *(bin_data->f_add) = 1 - dot_product(residual, centroid) + l2_sqr * ip_cent_xucb / ip_resi_xucb;
        *(bin_data->f_rescale) = -l2_sqr / ip_resi_xucb;
        *(bin_data->f_error) = 1 * tmp_error;
    } else {
        fail("unsupported metric in quantization");
    }

    free_mem(bin_code_int);
    free_mem(residual);
    free_mem(xu_cb);
}

template <typename T>
float RaBitQuantizer::faster_quantize_ex(float* o_abs, T* code, int ex_bits)
{
    constexpr double kEps = 1e-5;
    double ipnorm = 0;

    int *tmp_code = alloc_array<int>(_padded_dim);
    const int max_code = (1 << ex_bits) - 1;
    for (int i = 0; i < _padded_dim; i++) {
        // compute and store code
        const double scaled = (_rescaling_factor * o_abs[i]) + kEps;
        tmp_code[i] = !std::isfinite(scaled) || scaled >= max_code
            ? max_code
            : (scaled <= 0.0 ? 0 : static_cast<int>(scaled));
        code[i] = static_cast<T>(tmp_code[i]);

        // ip * norm = unnormalized ip
        ipnorm += (tmp_code[i] + 0.5) * o_abs[i];
    }

    float ipnorm_inv = static_cast<double>(1 / ipnorm);
    if (!std::isnormal(ipnorm_inv)) {
        ipnorm_inv = 1.0f;
    }

    free_mem(tmp_code);

    return ipnorm_inv;
}

template <typename T>
float RaBitQuantizer::quantize_ex(float* o_abs, T* code, int ex_bits)
{
    constexpr double kEps = 1e-5;
    double t = best_rescale_factor<float>(o_abs, _padded_dim, ex_bits);
    double ipnorm = 0;

    int *tmp_code = alloc_array<int>(_padded_dim);
    const int max_code = (1 << ex_bits) - 1;
    for (int i = 0; i < _padded_dim; i++) {
        // compute and store code
        const double scaled = (t * o_abs[i]) + kEps;
        tmp_code[i] = !std::isfinite(scaled) || scaled >= max_code
            ? max_code
            : (scaled <= 0.0 ? 0 : static_cast<int>(scaled));
        code[i] = static_cast<T>(tmp_code[i]);

        // ip * norm = unnormalized ip
        ipnorm += (tmp_code[i] + 0.5) * o_abs[i];
    }

    float ipnorm_inv = static_cast<double>(1 / ipnorm);
    if (!std::isnormal(ipnorm_inv)) {
        ipnorm_inv = 1.0f;
    }

    free_mem(tmp_code);

    return ipnorm_inv;
}

template <typename T>
float RaBitQuantizer::ex_bits_code(float *residual, T *ex_code_int, int ex_bits,
    bool use_const_rescaling)
{
    float *abs_res = alloc_array<float>(_padded_dim);
    memcpy(abs_res, residual, _padded_dim * sizeof(float));
    rowwise_norm_abs<float>(abs_res, 1, _padded_dim);

    float ipnorm_inv;
    if (use_const_rescaling && _rescaling_factor > 0) {
        ipnorm_inv = faster_quantize_ex(abs_res, ex_code_int, ex_bits);
    } else {
        ipnorm_inv = quantize_ex(abs_res, ex_code_int, ex_bits);
    }

    // revert codes for negative padded_dims
    int32 mask = (1 << ex_bits) - 1;
    for (int i = 0; i < _padded_dim; ++i) {
        if (residual[i] < 0) {
            T tmp = ex_code_int[i];
            ex_code_int[i] = (~tmp) & mask;
        }
    }

    free_mem(abs_res);

    return ipnorm_inv;
}

void RaBitQuantizer::quantize_ext_code(float *vec, float *centroid, ExtDataWithFactors *ext_data)
{
    uint8 *ex_code_int = ext_data->ex_code;
    float *residual = alloc_array<float>(_padded_dim);
    vec_sub(vec, centroid, residual);

    /* Stored vectors use the per-vector optimum. The persisted constant is
     * intentionally query-only, so post-build INSERT uses the same encoding
     * as the initial index build. */
    float ipnorm_inv = ex_bits_code<uint8>(residual, ex_code_int,
        HNSW_RABITQ_EX_BITS, false);

    int *total_code = alloc_array<int>(_padded_dim);
    for (int i = 0; i < _padded_dim; ++i) {
        total_code[i] = ex_code_int[i];
    }
    for (int i = 0; i < _padded_dim; ++i) {
        total_code[i] += static_cast<int>(residual[i] >= 0) << HNSW_RABITQ_EX_BITS;
    }

    // Factors are similar to those in one_bit_code_with_factor(),
    // please refer to document for detailed info.
    constexpr float cb = -(static_cast<float>(1 << HNSW_RABITQ_EX_BITS) - 0.5f);
    float *xu_cb = alloc_array<float>(_padded_dim);
    vec_add<int>(total_code, cb, xu_cb);

    float l2_sqr = dot_product(residual, residual);
    float l2_norm = std::sqrt(l2_sqr);

    float ip_resi_xucb = dot_product(residual, xu_cb);
    float ip_cent_xucb = dot_product(centroid, xu_cb);

    if (!(l2_sqr > 0.0f) || !(ip_resi_xucb > 0.0f)) {
        *(ext_data->f_add_ex) = _metric == Metric::L2 ? 0.0f : 1.0f;
        *(ext_data->f_rescale_ex) = 0.0f;
        *(ext_data->f_error_ex) = 0.0f;
        free_mem(total_code);
        free_mem(residual);
        free_mem(xu_cb);
        return;
    }

    float tmp_error = compute_error_factor(l2_sqr, l2_norm,
        dot_product(xu_cb, xu_cb), ip_resi_xucb, _padded_dim);

    if (_metric == Metric::L2) {
        *(ext_data->f_add_ex) = l2_sqr + 2 * l2_sqr * ip_cent_xucb / ip_resi_xucb;
        *(ext_data->f_rescale_ex) = ipnorm_inv * -2 * l2_norm;
        *(ext_data->f_error_ex) = 2 * tmp_error;
    } else if (_metric == Metric::INNER_PRODUCT) {
        *(ext_data->f_add_ex) = 1 - dot_product(residual, centroid) + l2_sqr * ip_cent_xucb / ip_resi_xucb;
        *(ext_data->f_rescale_ex) = ipnorm_inv * -l2_norm;
        *(ext_data->f_error_ex) = 1 * tmp_error;
    } else {
        fail("unsupported metric in quantization");
    }

    free_mem(total_code);
    free_mem(residual);
    free_mem(xu_cb);
}

void RaBitQuantizer::quantize_scalar(float *vec, float *centroid, int total_bits,
    uint16 *total_code, float &delta, float &vl, ScalarQuantizerType sqtype)
{
    int *binary_code = alloc_array<int>(_padded_dim);
    int ex_bits = total_bits - 1;

    float *residual = alloc_array<float>(_padded_dim);
    one_bit_code(vec, centroid, residual, binary_code);

    if (ex_bits <= 0) {
        fail("scalar quantization requires at least two bits");
    }
    ex_bits_code<uint16>(residual, total_code, ex_bits, true);

    // merge 2 one_bit code and ex_bits code
    for (int i = 0; i < _padded_dim; ++i) {
        total_code[i] += static_cast<uint16>(binary_code[i]) << ex_bits;
    }

    float cb = -(static_cast<float>(1 << ex_bits) - 0.5f);
    float *u_cb = alloc_array<float>(_padded_dim);
    vec_add<uint16>(total_code, cb, u_cb);

    float norm_data = _l2_norm(residual, residual, _padded_dim);
    float norm_quan = _l2_norm(u_cb, u_cb, _padded_dim);
    if (!(norm_data > 0.0f) || !(norm_quan > 0.0f)) {
        delta = 0.0f;
        vl = 0.0f;
        free_mem(residual);
        free_mem(u_cb);
        free_mem(binary_code);
        return;
    }

    float cos_similarity = dot_product(residual, u_cb) / (norm_data * norm_quan);
    if (!(cos_similarity > 0.0f) || !std::isfinite(cos_similarity)) {
        delta = 0.0f;
        vl = 0.0f;
        free_mem(residual);
        free_mem(u_cb);
        free_mem(binary_code);
        return;
    }

    if (sqtype == ScalarQuantizerType::RECONSTRUCTION) {
        delta = norm_data / norm_quan * cos_similarity;
    } else if (sqtype == ScalarQuantizerType::UNBIASED_ESTIMATION) {
        delta = norm_data / norm_quan / cos_similarity;
    } else if (sqtype == ScalarQuantizerType::PLAIN) {
        delta = norm_data / norm_quan;
    }

    vl = delta * cb;

    free_mem(residual);
    free_mem(u_cb);
    free_mem(binary_code);
}

} /* namespace rabitq */
