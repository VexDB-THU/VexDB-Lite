#include "pg_compat.h"
#include "quantizer.h"
#include "annkmeans.h"
#include "module/timer.h"
#include "rabitq/estimator.h"

QuantizerType extract_qt(const char *qt_type)
{
    (void)qt_type;
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

void QuantizerMetaInfo::init(QuantizerType, uint32)
{
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
