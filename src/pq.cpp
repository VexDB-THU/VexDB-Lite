#include "pg_compat.h"
#include "pq.h"

void ProductQuantizer::set_basic_values(size_t dim, size_t m, size_t nbits_)
{
    d = dim;
    M = m;
    nbits = nbits_;
    set_derived_values();
}

void ProductQuantizer::set_fvec_L2sqr_ny_nearest_func()
{
    _fvec_L2sqr_ny_nearest_func = nullptr;
}

void ProductQuantizer::set_fvec_ny_distance_func(Metric metric)
{
    (void)metric;
    _fvec_ny_distance_func = nullptr;
}

void ProductQuantizer::set_derived_values()
{
    ksub = 1 << nbits;
    code_size = (nbits * M + 7) / 8;
    centroids = nullptr;
}

void ProductQuantizer::train(AnnKmeansState *kmeansSupfucs, FloatVectorArray samples, int parallelWorkers, int maintenanceWorkMem)
{
    (void)kmeansSupfucs;
    (void)samples;
    (void)parallelWorkers;
    (void)maintenanceWorkMem;
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        errmsg("PQ quantizer not implemented")));
}

void ProductQuantizer::free_resourses()
{
}

void ProductQuantizer::set_params(FloatVectorArray subcenters, int m)
{
    (void)subcenters;
    (void)m;
}

void ProductQuantizer::set_dist_code_func()
{
}

void ProductQuantizer::compute_code(const float *x, uint8_t *code) const
{
    (void)x;
    (void)code;
}

float ProductQuantizer::distance_to_code(const uint8_t *code, const float *distTable)
{
    (void)code;
    (void)distTable;
    return 0.0f;
}

void ProductQuantizer::distance_to_four_code(const float *distTable,
    const uint8_t *code0, const uint8_t *code1, const uint8_t *code2, const uint8_t *code3,
    float &result0, float &result1, float &result2, float &result3)
{
    (void)distTable;
    (void)code0; (void)code1; (void)code2; (void)code3;
    result0 = result1 = result2 = result3 = 0.0f;
}

void ProductQuantizer::compute_distance_table(const float *x, float *dist_table) const
{
    (void)x;
    (void)dist_table;
}

void PQDistancer::train(Relation index, FloatVectorArray samples, size_t dimension,
    Metric metric, bool need_norm, int parallel_workers, int maintenance_work_mem)
{
    (void)index;
    (void)samples;
    (void)dimension;
    (void)metric;
    (void)need_norm;
    (void)parallel_workers;
    (void)maintenance_work_mem;
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
        errmsg("PQ quantizer not implemented")));
}

void PQDistancer::prepare(Relation index, void *metap)
{
    (void)index;
    (void)metap;
}

void PQDistancer::process(const char *query)
{
    (void)query;
}

void PQDistancer::destroy()
{
}

void PQDistancer::flush(Relation index, BlockNumber qtcode_block, bool enabling)
{
    (void)index;
    (void)qtcode_block;
    (void)enabling;
}

void PQDistancer::hnsw_read_pq_center(Relation index, ProductQuantizer &pq, BlockNumber qtcode_block)
{
    (void)index;
    (void)pq;
    (void)qtcode_block;
}
