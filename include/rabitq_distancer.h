#ifndef RABITQ_DISTANCER_H
#define RABITQ_DISTANCER_H

#include "pg_compat.h"
#include <vtl/optional>

namespace rabitq {

struct RabitqDistancer {
    static constexpr bool has_estimation_func = true;
    static constexpr bool need_refine = false;
    
    RabitqDistancer() : cid_size(0), bin_size(0), prepared(false) {}
    
    void train(Relation, FloatVectorArray, int, Metric, bool, int, int) {}
    void flush(Relation, BlockNumber, bool = false) {}
    void prepare(Relation, void *) {}
    void process(const char *) {}
    void destroy() {}
    size_t code_size() { return 0; }
    void compute_code(float *, char *) {}
    
    float get_distance_est_single(const void *, const void *, uint16) const { return 0; }
    float get_distance_single(const void *, const void *, uint16) const { return 0; }
    void get_distance_est_batch2(const void *, void *const *, uint16, uint16, float *) const {}
    void get_distance_batch2(const void *, void *const *, uint16, uint16, float *) const {}
    
private:
    int dim = 0;
    uint32 cid_size;
    uint32 bin_size;
    bool prepared;
};

}

#endif
