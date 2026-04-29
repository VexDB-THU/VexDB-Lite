/**
 * Copyright ...
 * DiskANN RaBitQ declaration
 */

#ifndef RABITQ_QUANTIZER_H
#define RABITQ_QUANTIZER_H

#include <cfloat>

#include "pg_compat.h"
#include "rabitq/utils.h"
#include "rabitq/rotator.h"
#include "knl/knl_variable.h"
#include "rabitq/rabitq_cache.h"
#include "distance.h"

namespace rabitq {

constexpr size_t query_kNumBits = 4;

class RaBitQuantizer : public BaseObject {
public:
    RaBitQuantizer(int dim, int padded_dim, Metric metric)
        : _dim(dim),
          _padded_dim(padded_dim),
          _metric(metric == Metric::L2 ? Metric::L2 : Metric::INNER_PRODUCT),
          _bin_code_size(RABITQ_BIN_CODE_SIZE(_padded_dim)),
          _ext_code_size(RABITQ_EXT_CODE_SIZE(_padded_dim))
    {
        _centroids = (float *)RABITQ_CACHE_ALLOC_ALIGNED(HNSW_RABITQ_NUM_CLUSTERS * _dim * sizeof(float));
        _rotated_centroids = (float *)RABITQ_CACHE_ALLOC_ALIGNED(HNSW_RABITQ_NUM_CLUSTERS * _padded_dim * sizeof(float));
        _func_ptr = ann_helper::get_general_distance_func(_metric, _dim);
        _l2_norm = ann_helper::get_general_distance_func(Metric::L2_NORM, _padded_dim);
        _neg_dot_product = ann_helper::get_general_distance_func(Metric::INNER_PRODUCT, _padded_dim);
        _rotator = NEW FhtKacRotator(_dim, _padded_dim);
    }
    RaBitQuantizer() = delete;

    void train();
    int quantize(float *vec, char *bin_data, char *ext_data = NULL);
    void quantize_scalar(float *vec, float *centroid, int total_bits, uint16 *total_code,
                         float &delta, float &vl, ScalarQuantizerType sqtype = RECONSTRUCTION);

    void load(char *random_matrix, float *centroids, float *rotate_centroids)
    {
        size_t random_matrix_size = _rotator->get_random_matrix_size();
        memcpy(_rotator->get_random_matrix(), random_matrix, random_matrix_size);
        size_t centroids_size = HNSW_RABITQ_NUM_CLUSTERS * _dim * sizeof(float);
        memcpy(_centroids, centroids, centroids_size);
        size_t rotated_centroids_size = HNSW_RABITQ_NUM_CLUSTERS * _padded_dim * sizeof(float);
        memcpy(_rotated_centroids, rotate_centroids, rotated_centroids_size);
    }

    char *get_random_matrix() { return _rotator->get_random_matrix(); }
    size_t get_random_matrix_size() { return _rotator->get_random_matrix_size(); }
    float *get_centroids() { return _centroids; }
    float *get_rotated_centroids() { return _rotated_centroids; }
    void rotate(float *vec, float *rotated) { _rotator->rotate(vec, rotated); }
    double get_query_rescaling_factor() { return get_const_scaling_factors(_padded_dim, query_kNumBits - 1); }
    void set_rescaling_factor(double rescaling_factor) { _rescaling_factor = rescaling_factor; }

    void destroy()
    {
        if (_rotator) {
            RABITQ_CACHE_FREE_ALIGNED_EXT(_centroids);
            RABITQ_CACHE_FREE_ALIGNED_EXT(_rotated_centroids);
            _rotator->destroy();
            delete _rotator;
            _rotator = NULL;
        }
    }

private:
    void pack_bin_code(int *bin_code_int, uint64 __restrict__ *bin_code);
    void one_bit_code(float *vec, float *centroid, float *residual, int *bin_code_int);
    void quantize_bin_code(float *vec, float *centroid, BinDataWithFactors *bin_data);

    template <typename T> float faster_quantize_ex(float* o_abs, T* code, int ex_bits);
    template <typename T> float quantize_ex(float* o_abs, T* code, int ex_bits);
    template <typename T> float ex_bits_code(float *residual, T *ex_code_int, int ex_bits);
    void quantize_ext_code(float *vec, float *centroid, ExtDataWithFactors *ext_data);

    void vec_sub(float *x, float *y, float *res)
    {
        for (int i = 0; i < _padded_dim; ++i) {
            res[i] = x[i] - y[i];
        }
    }

    template <typename T>
    void vec_add(T *x, float y, float *res)
    {
        for (int i = 0; i < _padded_dim; ++i) {
            res[i] = static_cast<float>(x[i]) + y;
        }
    }

    float dot_product(float *x, float *y)
    {
        return -_neg_dot_product(x, y, _padded_dim);
    }

    int compute_closest_cluster(float *vec)
    {
        float dist = 0.0f;
        int closest_cluster = 0;
        float min_dist = FLT_MAX;
        for (int i = 0; i < HNSW_RABITQ_NUM_CLUSTERS; ++i) {
            dist = _func_ptr(vec, _centroids + i * _dim, _dim);
            if (dist < min_dist) {
                min_dist = dist;
                closest_cluster = i;
            }
        }
        return closest_cluster;
    }

private:
    int _dim{0};
    int _padded_dim{0};
    Metric _metric{Metric::L2};
    int _bin_code_size;
    int _ext_code_size;
    float *_centroids{nullptr};
    float *_rotated_centroids{nullptr};
    ann_helper::distance_func _func_ptr;
    ann_helper::distance_func _l2_norm;
    ann_helper::distance_func _neg_dot_product;
    FhtKacRotator *_rotator{NULL};
    double _rescaling_factor{-1.0f};
};

} /* namespace rabitq */

#endif /* RABITQ_QUANTIZER_H */
