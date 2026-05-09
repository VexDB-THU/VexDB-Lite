#ifndef PQ_H
#define PQ_H

#include <stddef.h>
#include <cstdint>
#include "postgres.h"
#include "utils/relcache.h"
#include "floatvector.h"
#include "distance/core/distance.h"

struct AnnKmeansState;

inline void pq_set_param(uint32 dim, uint16 &m, uint16 &k)
{
    if (dim % 4u == 0) {
        m = dim / 4u;
    } else if (dim % 3u == 0) {
        m = dim / 3u;
    } else if (dim % 5u == 0) {
        m = dim / 5u;
    } else if (dim % 2u == 0) {
        m = dim / 2u;
    } else {
        m = dim;
    }
    k = 256u;
}

/** Product Quantizer. Implemented only for METRIC_L2 */
struct ProductQuantizer {
    size_t d;         ///< dimension size of the input vectors
    size_t code_size; ///< bytes per indexed vector
    size_t M;     ///< number of subquantizers
    size_t nbits; ///< number of bits per quantization index

    // values derived from the above
    size_t dsub;  ///< dimensionality of each subvector
    size_t ksub;  ///< number of centroids for each subquantizer

    ann_helper::fvec_L2sqr_ny_nearest_func _fvec_L2sqr_ny_nearest_func;
    ann_helper::fvec_ny_distance_func _fvec_ny_distance_func;
    ann_helper::distance_single_code_func _distance_single_code_func;
    ann_helper::distance_four_codes_func _distance_four_codes_func;

    /// Centroid table, size M * ksub * dsub.
    /// Layout: (M, ksub, dsub)
    float* centroids;

    size_t get_centroids_size() { return d * ksub; }
    /// return the centroids associated with subvector m
    float* get_centroids(size_t m, size_t i) { return &centroids[(m * ksub + i) * dsub]; }
    const float* get_centroids(size_t m, size_t i) const { return &centroids[(m * ksub + i) * dsub]; }

    // Train the product quantizer on a set of points. A clustering
    // can be set on input to define non-default clustering parameters
    void train(AnnKmeansState *kmeansSupfucs, FloatVectorArray samples, int parallelWorkers, int maintenanceWorkMem);
    void free_resourses();
    void set_basic_values(size_t dim, size_t m , size_t nbits_);
    void set_fvec_L2sqr_ny_nearest_func();
    void set_fvec_ny_distance_func(Metric metric);
    void set_dist_code_func();
    /// compute derived values when d, M and nbits have been set
    void set_derived_values();
    /// Define the centroids for subquantizer m
    void set_params(FloatVectorArray subcenters, int m);
    /// Quantize one vector with the product quantizer
    void compute_code(const float* x, uint8_t* code) const;
    float distance_to_code(const uint8_t* code, const float *distTable);
    void distance_to_four_code(const float* distTable,
                            // codes
                            const uint8_t* code0,
                            const uint8_t* code1,
                            const uint8_t* code2,
                            const uint8_t* code3,
                            // computed distances
                            float& result0,
                            float& result1,
                            float& result2,
                            float& result3);

    /** Compute distance table for one vector.
     *
     * The distance table for x = [x_0 x_1 .. x_(M-1)] is a M * ksub
     * matrix that contains
     *
     *   dist_table (m, j) = || x_m - c_(m, j)||^2
     *   for m = 0..M-1 and j = 0 .. ksub - 1
     *
     * where c_(m, j) is the centroid no j of sub-quantizer m.
     *
     * @param x         input vector size d
     * @param dist_table output table, size M * ksub
     */
    void compute_distance_table(const float* x, float* dist_table) const;
    // void compute_inner_prod_table(const float* x, float* dist_table) const;
};

struct PQDistancer {
    static constexpr bool has_estimation_func = false;
    static constexpr bool need_refine = true;

    PQDistancer() : dist_table(NULL), prepared(false) {}
    void train(Relation index, FloatVectorArray samples, size_t dimension, Metric metric,
               bool need_norm, int parallel_workers, int maintenance_work_mem);
    void prepare(Relation index, void *metap);
    void process(const char *query);
    void destroy();
    void flush(Relation index, BlockNumber qtcode_block, bool enabling = false);
    size_t code_size() { return pq.M; }
    void compute_code(float *vec, char *code) { pq.compute_code(vec, (uint8 *)code); }
    float get_distance_precise(const void *x, const void *y, uint16 dim) const
        { return _get_distance_precise_func(x, y, dim); }
    float get_distance_single(const void *x, const void *y, uint16 dim) const
        { return flag * pq.distance_to_code((const uint8 *)y, dist_table); }
    void get_distance_batch2(const void *x, void *const *y, uint16 dim, uint16 y_size, float *out) const
    {
        const uint8 *const *code = (const uint8 * const *)y;
        uint16 i = 0;
        for (; i + 4 < y_size; i += 4) {
            pq.distance_to_four_code(dist_table, code[i], code[i + 1], code[i + 2], code[i + 3],
                                      out[i], out[i + 1], out[i + 2], out[i + 3]);
        }
        for (; i < y_size; ++i) {
            out[i] = pq.distance_to_code(code[i], dist_table);
        }
    }
    void hnsw_read_pq_center(Relation index, ProductQuantizer &pq, BlockNumber qtcode_block);
    // Cache the trained centroids in a process-local map keyed by the index
    // OID so PQDistancer instances created later (in scan/insert paths) can
    // reload them without re-training. Persistence to qtcode_block is a
    // follow-up; for now PQ state is per-process and lost on restart.
    void stash_to_cache(Relation index);
    bool load_from_cache(Relation index, Metric metric);
private:
    mutable ProductQuantizer pq;
    ann_helper::distance_func _get_distance_precise_func;
    float *dist_table;
    float flag;
    bool prepared;
};

#endif
