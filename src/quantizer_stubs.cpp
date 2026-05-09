#include "pg_compat.h"
#include "quantizer.h"
#include "annkmeans.h"
#include "module/timer.h"
#include "rabitq/estimator.h"
#include "pq.h"

QuantizerType extract_qt(const char *qt_type)
{
    if (qt_type == nullptr) return QuantizerType::NONE;
    if (strcmp(qt_type, "pq") == 0)     return QuantizerType::PQ;
    if (strcmp(qt_type, "rabitq") == 0) return QuantizerType::RABITQ;
    return QuantizerType::NONE;
}

void add_quantizer_update_task(Relation index, void *params)
{
    (void)index;
    (void)params;
}

void ann_sample_rows(FloatVectorArray samples, Relation heap, Relation index,
    int dimensions, int sample_nums, bool need_norm, DistPrecisionType dist_type)
{
    (void)samples;
    (void)heap;
    (void)index;
    (void)dimensions;
    (void)sample_nums;
    (void)need_norm;
    (void)dist_type;
}

namespace ann_helper {

Timer::Timer(size_t nloop, size_t step_size, char*, char*) 
    : _start(std::chrono::high_resolution_clock::now()),
      _nloop(nloop),
      _step_size(step_size),
      _nloop_count(0),
      _need_report(false),
      _nloop_count_unknown(false),
      _index_progress_slot(-1),
      _index_name(NULL),
      _part_index_name(NULL),
      _stage(NULL) {}
      
void Timer::destroy() {}
void Timer::set_stage(char*) {}

}

bool QtUpdateMgr::insert_updating(Oid)
{
    return false;
}

void QtUpdateMgr::erase_updating(Oid)
{
}

bool QtUpdateMgr::contain_updating(Oid)
{
    return false;
}

TimeRing *QtUpdateMgr::insert_timgring(Oid)
{
    return nullptr;
}

TimeRing *QtUpdateMgr::find_timering(Oid)
{
    return nullptr;
}

void QtUpdateMgr::erase_timering(Oid)
{
}

void QuantizerMetaInfo::init(QuantizerType qt_type, uint32 dimension)
{
    quantizer_type    = qt_type;
    centroids_version = 0;
    code_version      = 0;
    num_new_data      = 0;
    if (qt_type == QuantizerType::PQ) {
        // graph_pq stays false until PQ is actually trained; get_type() then
        // returns NONE so the build/search path falls back to plain HNSW.
        // This keeps WITH (quantizer='pq', pq_m=N) DDL accepted without
        // tripping the centroids/code version mismatch warning loop.
        metainfo.pq_metainfo.graph_pq = false;
        pq_set_param(dimension, metainfo.pq_metainfo.m, metainfo.pq_metainfo.k);
    } else if (qt_type == QuantizerType::RABITQ) {
        metainfo.rbq_meta.enabled               = false;
        metainfo.rbq_meta.keep_vecs             = false;
        metainfo.rbq_meta.quant_size            = 0;
        metainfo.rbq_meta.query_rescaling_factor = 0.0;
    }
}

namespace rabitq {

float RaBitQEstimator::get_full_dist(int, char*, char*)
{
    return 0.0f;
}

void RaBitQEstimator::get_full_dist(int, char*, char*, EstimateRecord&)
{
}

}
