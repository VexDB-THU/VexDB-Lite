/**
 * Copyright (c) 2026 VexDB-THU
 * RaBitQ Distance Estimator
 */

#ifndef RABITQ_ESTIMATOR_H
#define RABITQ_ESTIMATOR_H

#include "rabitq/platform.h"
#include "rabitq/query.h"
#include "rabitq/utils.h"
#include "rabitq/rabitq.h"
#include "module/perf_usage.h"

namespace rabitq {

PERF_DECLARE_CATS(RaBitQPerfCats, false, bincmp, fullcmp);

class RaBitQEstimator : public PERFER(RaBitQPerfCats) {
public:
    using PerfCats = RaBitQPerfCats;

    explicit RaBitQEstimator(int padded_dim, Metric metric, double rescaling_factor)
        : _padded_dim(padded_dim),
          _metric(metric == Metric::L2 ? Metric::L2 : Metric::INNER_PRODUCT),
          _rescaling_factor(rescaling_factor),
          _bin_code_size(RABITQ_BIN_CODE_SIZE(_padded_dim)),
          _ext_code_size(RABITQ_EXT_CODE_SIZE(_padded_dim)),
          _warmup_ip_x0_q(nullptr),
          _ip_fxi(nullptr),
          _mask_ip_x0_q(nullptr)
    {
        auto kernels = ann_helper::get_rabitq_kernels();
        _warmup_ip_x0_q = kernels.warmup_ip_x0_q;
        _ip_fxi = kernels.ip_fxi;
        _mask_ip_x0_q = kernels.mask_ip_x0_q;
        if (!_warmup_ip_x0_q || !_ip_fxi || !_mask_ip_x0_q) {
            fail("SIMD estimator kernels are unavailable");
        }
        _rotated_query = alloc_floatvector(_padded_dim, 1);
        _q_to_centroids = alloc_array_zero<float>(2 * HNSW_RABITQ_NUM_CLUSTERS);
        _l2_sqrt = ann_helper::get_general_distance_func(Metric::L2_SQRT, _padded_dim);
        _neg_dot_product = ann_helper::get_general_distance_func(Metric::INNER_PRODUCT, _padded_dim);
        _query_wrapper = make_object<QueryWrapper>(_padded_dim, _metric);
    }

    RaBitQEstimator()
        : _rotated_query(NULL),
          _q_to_centroids(NULL),
          _query_wrapper(NULL) {}
    ~RaBitQEstimator() { destroy(); }

    RaBitQEstimator(const RaBitQEstimator &) = delete;
    RaBitQEstimator &operator=(const RaBitQEstimator &) = delete;

    RaBitQuantizer *get_quantizer() { return _quantizer; }
    void set_quantizer(RaBitQuantizer *quantizer) { _quantizer = quantizer; }

    void preprocess(float *query);
    float get_bin_dist(int closest_cluster, char *bin_data);
    void get_bin_dist(int closest_cluster, char *bin_data, EstimateRecord &est);
    float get_full_dist(int closest_cluster, char *bin_data, char *ext_data);
    void get_full_dist(int closest_cluster, char *bin_data, char *ext_data, EstimateRecord &est);

    inline void perf_report() { REPORT_PERF(NOTICE); }

    void destroy()
    {
        free_vector(_rotated_query);
        _rotated_query = nullptr;
        free_mem(_q_to_centroids);
        _q_to_centroids = nullptr;
        PERF_DESTROY();
        if (_query_wrapper) {
            destroy_object(_query_wrapper);
            _query_wrapper = nullptr;
        }
    };

private:
    void get_bin_dist_internal(float g_add, float g_error, char *bin_data, EstimateRecord &est);
    void get_full_dist_internal(float g_add, float g_error, char *bin_data, char *ext_data, EstimateRecord &est);

    inline void gen_q_to_centroids()
    {
        float *centroids = _quantizer->get_rotated_centroids();
        if (_metric == Metric::L2) {
            for (int i = 0; i < HNSW_RABITQ_NUM_CLUSTERS; ++i) {
                _q_to_centroids[i] = _l2_sqrt(_rotated_query, centroids + i * _padded_dim, _padded_dim);
            }
        } else if (_metric == Metric::INNER_PRODUCT) {
            /* first half as g_add, second half as g_error */
            for (int i = 0; i < HNSW_RABITQ_NUM_CLUSTERS; ++i) {
                _q_to_centroids[i] = -_neg_dot_product(_rotated_query, centroids + i * _padded_dim, _padded_dim);
                _q_to_centroids[i + HNSW_RABITQ_NUM_CLUSTERS] =
                    _l2_sqrt(_rotated_query, centroids + i * _padded_dim, _padded_dim);
            }
        }
    }

    inline void perf_bincmp() { DO_PERF(bincmp); }
    inline void perf_fullcmp() { DO_PERF(fullcmp); }
    inline void perf_stop_bincmp() { STOP_PERF(bincmp); }
    inline void perf_stop_fullcmp() { STOP_PERF(fullcmp); }

private:
    int _padded_dim;
    Metric _metric;
    double _rescaling_factor;
    int _bin_code_size;
    int _ext_code_size;
    float *_rotated_query;
    float *_q_to_centroids;
    ann_helper::distance_func _l2_sqrt;
    ann_helper::distance_func _neg_dot_product;
    ann_helper::warmup_ip_x0_q_func _warmup_ip_x0_q;
    ann_helper::ip_fxi_func _ip_fxi;
    ann_helper::mask_ip_x0_q_func _mask_ip_x0_q;
    RaBitQuantizer *_quantizer{NULL};
    QueryWrapper *_query_wrapper;
};

} /* namespace rabitq */

#endif /* RABITQ_ESTIMATOR_H */
