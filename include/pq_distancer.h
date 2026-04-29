#ifndef PQ_DISTANCER_H
#define PQ_DISTANCER_H

#include "pg_compat.h"

struct PQDistancer {
    static constexpr bool has_estimation_func = false;
    static constexpr bool need_refine = true;
    
    PQDistancer() : dist_table(nullptr), prepared(false) {}
    void train(Relation, FloatVectorArray, size_t, Metric, bool, int, int) {}
    void prepare(Relation, void *) {}
    void process(const char *) {}
    void destroy() {}
    void flush(Relation, BlockNumber, bool = false) {}
    size_t code_size() { return 0; }
    void compute_code(float *, char *) {}
    float get_distance_precise(const void *, const void *, uint16) const { return 0; }
    float get_distance_single(const void *, const void *, uint16) const { return 0; }
    void get_distance_batch2(const void *, void *const *, uint16, uint16, float *) const {}
    
private:
    float *dist_table;
    bool prepared;
};

#endif
