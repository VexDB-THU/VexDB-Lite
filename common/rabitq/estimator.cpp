/**
 * Copyright (c) 2026 VexDB-THU
 * RaBitQ Distance Estimator
 */

#include "rabitq/estimator.h"

namespace rabitq {

void RaBitQEstimator::preprocess(float *query)
{
    _quantizer->rotate(query, _rotated_query);
    _query_wrapper->preprocess(_rotated_query, _quantizer);
    gen_q_to_centroids();
}

float RaBitQEstimator::get_bin_dist(int closest_cluster, char *bin_data)
{
    EstimateRecord est;
    get_bin_dist(closest_cluster, bin_data, est);
    return est.est_dist;
}

void RaBitQEstimator::get_bin_dist(int closest_cluster, char *bin_data, EstimateRecord &est)
{
    float norm = _q_to_centroids[closest_cluster];
    if (_metric == Metric::INNER_PRODUCT) {
        float error = _q_to_centroids[closest_cluster + HNSW_RABITQ_NUM_CLUSTERS];
        get_bin_dist_internal(-norm, error, bin_data, est);
    } else if (_metric == Metric::L2) {
        get_bin_dist_internal(norm * norm, norm, bin_data, est);
    }
}

void RaBitQEstimator::get_bin_dist_internal(float g_add, float g_error, char *raw_bin_data, EstimateRecord &est)
{
    perf_bincmp();
    BinDataWithFactors bin_data(raw_bin_data, _bin_code_size);
    est.ip_x0_qr = _warmup_ip_x0_q(bin_data.bin_code,
        _query_wrapper->query_bin(), _query_wrapper->delta(), _query_wrapper->vl(), _padded_dim);
    est.est_dist = *(bin_data.f_add) + g_add + *(bin_data.f_rescale) * (est.ip_x0_qr + _query_wrapper->k1xsumq());
    est.low_dist = est.est_dist - *(bin_data.f_error) * g_error;
    perf_stop_bincmp();
}

float RaBitQEstimator::get_full_dist(int closest_cluster, char *bin_data, char *ext_data)
{
    EstimateRecord est;
    get_full_dist(closest_cluster, bin_data, ext_data, est);
    return est.est_dist;
}

void RaBitQEstimator::get_full_dist(int closest_cluster, char *bin_data, char *ext_data, EstimateRecord &est)
{
    float norm = _q_to_centroids[closest_cluster];
    if (_metric == Metric::INNER_PRODUCT) {
        float error = _q_to_centroids[closest_cluster + HNSW_RABITQ_NUM_CLUSTERS];
        get_full_dist_internal(-norm, error, bin_data, ext_data, est);
    } else if (_metric == Metric::L2) {
        get_full_dist_internal(norm * norm, norm, bin_data, ext_data, est);
    }
}

void RaBitQEstimator::get_full_dist_internal(float g_add, float g_error, char *raw_bin_data, char *raw_ext_data, EstimateRecord &est)
{
    perf_fullcmp();
    BinDataWithFactors bin_data(raw_bin_data, _bin_code_size);
    ExtDataWithFactors ext_data(raw_ext_data, _ext_code_size);
    est.ip_x0_qr = _mask_ip_x0_q(_rotated_query, bin_data.bin_code, _padded_dim);
    float ip = _ip_fxi(_rotated_query, ext_data.ex_code, _padded_dim);
    est.est_dist = *(ext_data.f_add_ex) + g_add +
        (*(ext_data.f_rescale_ex) * (static_cast<float>(1 << HNSW_RABITQ_EX_BITS) * est.ip_x0_qr + ip + _query_wrapper->kbxsumq()));
    est.low_dist = est.est_dist - *(bin_data.f_error) * g_error / static_cast<float>(1 << HNSW_RABITQ_EX_BITS);
    perf_stop_fullcmp();
}

} /* namespace rabitq */
