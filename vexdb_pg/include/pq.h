#ifndef PQ_H
#define PQ_H

#include <stddef.h>
#include <cstdint>
#include "postgres.h"
#include "utils/relcache.h"
#include "floatvector.h"
#include "distance/core/distance.h"
#include "quantizer/product_quantizer.h"

struct AnnKmeansState;

inline void pq_set_param(uint32 dim, uint16 &m, uint16 &k, uint32 requested_m = 0)
{
    if (requested_m != 0) {
        if (requested_m > UINT16_MAX) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("Invalid \"pq_m\" value: %u (must be <= %u)",
                       requested_m, (unsigned)UINT16_MAX)));
        }
        if (dim == 0 || dim % requested_m != 0) {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("PQ: dimension %u should be a multiple of pq_m=%u",
                       dim, requested_m)));
        }
        m = (uint16)requested_m;
        k = 256u;
        return;
    }

    m = (uint16)::vex::quantizer::ProductQuantizer::AutoSelectM(dim);
    k = 256u;
}

// PostgreSQL and DuckDB must use the same ProductQuantizer implementation.
// PG keeps only the host adapter (sampling, palloc, page I/O and error boundary)
// in this file; all training/encoding/ADC math lives in common/quantizer.
using ProductQuantizer = ::vex::quantizer::ProductQuantizer;

struct PQDistancer {
    static constexpr bool has_estimation_func = false;
    static constexpr bool need_refine = true;
    static constexpr bool requires_aligned_storage = false;

    PQDistancer() : dist_table(NULL), flag(0.0f), prepared(false) {}
    void train(Relation index, FloatVectorArray samples, size_t dimension, Metric metric,
               bool need_norm, int parallel_workers, int maintenance_work_mem,
               uint32 requested_m = 0);
    void prepare(Relation index, void *metap);
    void process(const char *query);
    void destroy();
    void flush(Relation index, BlockNumber qtcode_block, bool enabling = false);
    size_t code_size() const { return pq.code_size; }
    void compute_code(float *vec, char *code) { pq.compute_code(vec, (uint8 *)code); }
    float get_distance_precise(const void *x, const void *y, uint16 dim) const
        { return _get_distance_precise_func(x, y, dim); }
    // ADC: x is the raw query (process()'d into dist_table), y is the
    // stored code buffer. Used by SELECT (search-only); DML INSERT prune
    // path under PQ is a known limitation in v1 (recall may degrade).
    float get_distance_single(const void *x, const void *y, uint16 dim) const
    {
        (void)x; (void)dim;
        return pq.distance_to_code((const uint8_t *)y, dist_table) * flag;
    }
    void get_distance_batch2(const void *x, void *const *y, uint16 dim, uint16 y_size, float *out) const
    {
        (void)x; (void)dim;
        uint16 i = 0;
        for (; i + 4 <= y_size; i += 4) {
            pq.distance_to_four_code(dist_table,
                (const uint8_t *)y[i], (const uint8_t *)y[i+1],
                (const uint8_t *)y[i+2], (const uint8_t *)y[i+3],
                out[i], out[i+1], out[i+2], out[i+3]);
            out[i]   *= flag;
            out[i+1] *= flag;
            out[i+2] *= flag;
            out[i+3] *= flag;
        }
        for (; i < y_size; ++i) {
            out[i] = pq.distance_to_code((const uint8_t *)y[i], dist_table) * flag;
        }
    }
    void hnsw_read_pq_center(Relation index, ProductQuantizer &pq, BlockNumber qtcode_block);
private:
    void configure_for_metric(size_t d, size_t M, size_t nbits, Metric metric);
    mutable ProductQuantizer pq;
    ann_helper::distance_func _get_distance_precise_func;
    float *dist_table;
    float flag;
    bool prepared;
};

#endif
