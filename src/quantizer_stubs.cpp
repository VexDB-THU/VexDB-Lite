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
    (void)dist_type;
    (void)need_norm;
    if (heap == NULL || samples == NULL) return;

    // Sequential scan, take first sample_nums non-null vectors. Not random
    // sampling — fine for codebook training when the table is in roughly
    // arbitrary insertion order. Random sampling can land here later.
    samples->length = 0;
    Snapshot snap = GetActiveSnapshot();
    TableScanDesc scan = table_beginscan(heap, snap, 0, NULL, 0);
    TupleTableSlot *slot = table_slot_create(heap, NULL);
    int collected = 0;
    while (collected < sample_nums &&
           table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        // Index relation has the indexed expressions. For our case the
        // indexed column is column #1 of the underlying table at attno
        // stored in index->rd_index->indkey.values[0].
        AttrNumber attno = index->rd_index->indkey.values[0];
        bool isnull;
        Datum d = slot_getattr(slot, attno, &isnull);
        if (isnull) continue;
        FloatVector *fv = DatumGetFloatVector(d);
        if (fv->dim != dimensions) continue;
        std::memcpy(FloatVectorArrayGet(samples, collected), fv->x,
                    sizeof(float) * dimensions);
        collected++;
        samples->length = collected;
    }
    ExecDropSingleTupleTableSlot(slot);
    table_endscan(scan);
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
