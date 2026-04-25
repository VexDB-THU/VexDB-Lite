/**
 * Copyright ...
 */

#ifndef RABITQ_DISTANCER_H
#define RABITQ_DISTANCER_H

#include "pg_compat.h"
#include "rabitq/estimator.h"
#include "rabitq/rabitq.h"
#include "rabitq/rabitq_cache.h"
#include "graph_index/graph_index_quantizer.h"
#include <vtl/optional>

namespace rabitq {

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
struct RabitqDistancer {
#pragma GCC diagnostic pop
    static constexpr bool has_estimation_func = true;
    static constexpr bool need_refine = false;

    RabitqDistancer() : cid_size(0), bin_size(0), prepared(false) {}

    void train(Relation index, FloatVectorArray samples, int dimension, Metric metric, bool need_norm,
        int parallel_workers, int maintenance_work_mem);
    void flush(Relation index, BlockNumber qtcode_block, bool enabling = false);
    void prepare(Relation index, void *metapage);
    void process(const char *query);
    void destroy();
    size_t code_size() { return code_len; }
    void compute_code(float *vec, char *code)
    {
        char *bin_data = code + cid_size;
        char *ext_data = bin_data + bin_size;
        int cluster_id = quantizer->quantize(vec, bin_data, ext_data);
        memcpy(code, &cluster_id, cid_size);
    }

    float get_distance_est_single(const void *x, const void *y, uint16 dim) const
    {
        char *quant_data = (char *)y;
        uint16 cluster_id = *((uint16 *)quant_data);
        char *bin_data = quant_data + cid_size;
        estimator.get_bin_dist(cluster_id, bin_data, rec);
        return rec.low_dist;
    }

    float get_distance_single(const void *x, const void *y, uint16 dim) const
    {
        char *quant_data = (char *)y;
        uint16 cluster_id = *((uint16 *)quant_data);
        char *bin_data = quant_data + cid_size;
        char *ext_data = bin_data + bin_size;
        estimator.get_full_dist(cluster_id, bin_data, ext_data, rec);
        return rec.est_dist;
    }

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
    void load_rabitq_quantizer(Relation index, RaBitQMeta &rabitq_meta, RaBitQCache &cache);
    void load_rabitq_cache(Relation index, RaBitQMeta &rabitq_meta);
    void read_rabitq_data(Relation index, size_t rabitq_data_size, char *rabitq_data);
    void free_rabitq();

    mutable RaBitQEstimator estimator;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
    mutable EstimateRecord rec;
#pragma GCC diagnostic pop
    int dim;
    int padded_dim;
    Metric metric;
    Optional<RaBitQuantizer> quantizer;
    uint32 cid_size;
    uint32 bin_size;
    size_t code_len;
    BlockNumber qtcode_block;
    bool prepared;
};
}

#endif /* RABITQ_DISTANCER_H */
