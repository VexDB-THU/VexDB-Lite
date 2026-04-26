/**
 * Copyright ...
 */

#ifndef RABITQ_DISTANCER_H
#define RABITQ_DISTANCER_H

#include "pg_compat.h"
#include "graph_index/graph_index_quantizer.h"
#include "quantizer.h"
#include "rabitq/utils.h"
#include "vex/vex_rabitq.hpp"
#include <vtl/optional>

namespace rabitq {

struct RabitqDistancer {
    static constexpr bool has_estimation_func = true;
    static constexpr bool need_refine = true;

    RabitqDistancer() : cid_size(0), bin_size(0), prepared(false) {}

    void train(Relation index, FloatVectorArray samples, int dimension, Metric metric, bool need_norm,
        int parallel_workers, int maintenance_work_mem);
    void flush(Relation index, BlockNumber qtcode_block, bool enabling = false);
    void prepare(Relation index, void *metapage);
    void process(const char *query);
    void destroy();
    size_t code_size() { return code_len; }
    double get_query_rescaling_factor() const;
    void compute_code(float *vec, char *code);
    float get_distance_precise(const void *x, const void *y, uint16 dim) const;

    float get_distance_est_single(const void *x, const void *y, uint16 dim) const;
    float get_distance_single(const void *x, const void *y, uint16 dim) const;

    void get_distance_est_batch2(const void *x, void *const *y, uint16 dim, uint16 y_size, float *out) const
    {
        for (uint16 i = 0; i < y_size; ++i) {
            out[i] = get_distance_est_single(x, y[i], dim);
        }
    }

    void get_distance_batch2(const void *x, void *const *y, uint16 dim, uint16 y_size, float *out) const
    {
        for (uint16 i = 0; i < y_size; ++i) {
            out[i] = get_distance_single(x, y[i], dim);
        }
    }
private:
    void load_rabitq(Relation index, void *metap);
    void read_rabitq_data(Relation index, size_t rabitq_data_size, char *rabitq_data) const;
    static vex::Metric to_core_metric(Metric metric);
    void init_code_layout();
    static void train_centroids(FloatVectorArray samples, int dim, std::vector<float> &centroids);
    static void encode_binary_bits(const float *rotated_vec, const float *rotated_centroid, int padded_dim, char *bin_data);
    void encode_ext_bits(const float *rotated_vec, const float *rotated_centroid, int padded_dim, char *ext_data);
    static float decode_binary_distance(const uint64 *query_bits, const uint64 *code_bits, int padded_dim);
    static float decode_ext_distance(const char *query, const char *ext_data, int dim);

    mutable EstimateRecord rec{};
    int dim;
    int padded_dim;
    Metric metric;
    Optional<vex::RabitQQuantizer> quantizer;
    Optional<vex::RabitQEstimator> estimator;
    std::vector<float> rotated_query;
    std::vector<uint16> query_scalar_code;
    std::vector<uint64> query_binary_bits;
    uint32 cid_size;
    uint32 bin_size;
    uint32 ext_size;
    size_t code_len;
    BlockNumber qtcode_block;
    bool prepared;
};
}

#endif /* RABITQ_DISTANCER_H */
