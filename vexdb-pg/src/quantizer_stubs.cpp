#include "pg_compat.h"
#include "quantizer.h"
#include "annkmeans.h"
#include "ann_utils.h"
#include "knl/knl_variable.h"
#include "module/timer.h"
#include "rabitq/estimator.h"

namespace {

QuantizerType ParseQuantizerType(const char *qt_type)
{
    if (qt_type == nullptr || qt_type[0] == '\0') {
        return QuantizerType::NONE;
    }

    if (pg_strcasecmp(qt_type, "none") == 0 ||
        pg_strcasecmp(qt_type, "plain") == 0 ||
        pg_strcasecmp(qt_type, "plain_vector") == 0) {
        return QuantizerType::NONE;
    }
    if (pg_strcasecmp(qt_type, "pq") == 0) {
        return QuantizerType::PQ;
    }
    if (pg_strcasecmp(qt_type, "rabitq") == 0 ||
        pg_strcasecmp(qt_type, "rabit_q") == 0 ||
        pg_strcasecmp(qt_type, "ra_bit_q") == 0) {
        return QuantizerType::RABITQ;
    }
    return QuantizerType::NONE;
}

void EnsureSampleCapacity(FloatVectorArray samples, int required_count)
{
    if (samples == nullptr || required_count <= samples->maxlen) {
        return;
    }

    const size_t bytes = static_cast<size_t>(required_count) *
                         static_cast<size_t>(samples->dim) *
                         sizeof(float);
    samples->items = static_cast<float *>(repalloc(samples->items, bytes));
    std::memset(samples->items + static_cast<size_t>(samples->maxlen) * samples->dim,
                0,
                (static_cast<size_t>(required_count - samples->maxlen) * samples->dim) * sizeof(float));
    samples->maxlen = required_count;
}

} // namespace

QuantizerType extract_qt(const char *qt_type)
{
    return ParseQuantizerType(qt_type);
}

void validate_quantizer(const char *value)
{
    if (value == nullptr || value[0] == '\0') {
        return;
    }

    if (ParseQuantizerType(value) == QuantizerType::NONE &&
        pg_strcasecmp(value, "none") != 0 &&
        pg_strcasecmp(value, "plain") != 0 &&
        pg_strcasecmp(value, "plain_vector") != 0) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid quantizer value \"%s\"", value),
                 errhint("Supported quantizer values are: none, pq, rabitq.")));
    }
}

void add_quantizer_update_task(Relation index, void *params)
{
    (void)index;
    (void)params;
}

void ann_sample_rows(FloatVectorArray samples, Relation heap, Relation index,
    int dimensions, int sample_nums, bool need_norm, DistPrecisionType dist_type)
{
    (void)sample_nums;

    if (samples == nullptr || heap == nullptr || dimensions <= 0) {
        return;
    }

    samples->length = 0;

    int attnum = 1;
    if (index != nullptr && index->rd_index != nullptr) {
        attnum = index->rd_index->indkey.values[0];
    }
    if (attnum <= 0) {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("ann_sample_rows does not support expression index sampling yet")));
    }

    TableScanDesc scan = table_beginscan(heap, GetActiveSnapshot(), 0, nullptr);
    TupleTableSlot *slot = table_slot_create(heap, nullptr);
    ann_helper::vector_preprocess_func preprocess = nullptr;

    if (need_norm) {
        preprocess = ann_helper::get_vector_preprocess_func(Metric::FAST_COSINE, dist_type, dimensions);
    }

    while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        bool is_null = false;
        Datum datum = slot_getattr(slot, attnum, &is_null);
        if (is_null) {
            ExecClearTuple(slot);
            continue;
        }

        if (samples->length >= samples->maxlen) {
            const int next_capacity = samples->maxlen > 0 ? samples->maxlen * 2 : 1024;
            EnsureSampleCapacity(samples, next_capacity);
        }

        Pointer vec_p = nullptr;
        char *raw = DatumGetVector(datum, dist_type, &vec_p);
        float *dst = FloatVectorArrayGet(samples, samples->length);

        if (dist_type == DistPrecisionType::FLOAT) {
            std::memcpy(dst, raw, static_cast<size_t>(dimensions) * sizeof(float));
        } else if (dist_type == DistPrecisionType::HALF) {
            const half *src = reinterpret_cast<const half *>(raw);
            for (int i = 0; i < dimensions; ++i) {
                dst[i] = g_instance.annvec_cxt.half_to_float(src[i]);
            }
        } else {
            if (vec_p != DatumGetPointer(datum)) {
                pfree(vec_p);
            }
            ExecClearTuple(slot);
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("ann_sample_rows only supports floatvector and halfvector")));
        }

        if (need_norm && preprocess != nullptr) {
            preprocess(dst, dimensions, dst);
        }

        ++samples->length;

        if (vec_p != DatumGetPointer(datum)) {
            pfree(vec_p);
        }
        ExecClearTuple(slot);
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

void PQMetaInfo::init(uint32 dim)
{
    (void)dim;
    graph_pq = false;
    m = 0;
    k = 0;
}

void RaBitQMeta::init(uint32 dim)
{
    enabled = false;
    keep_vecs = false;
    quant_size = static_cast<int>(sizeof(uint16));
    query_rescaling_factor = 0.0;
    (void)dim;
}

void QuantizerMetaInfo::init(QuantizerType qt_type, uint32 dimension)
{
    quantizer_type = qt_type;
    num_new_data = 0;
    centroids_version = 0;
    code_version = 0;

    if (qt_type == QuantizerType::PQ) {
        metainfo.pq_metainfo.init(dimension);
        return;
    }
    if (qt_type == QuantizerType::RABITQ) {
        metainfo.rbq_meta.init(dimension);
        return;
    }
    std::memset(&metainfo, 0, sizeof(metainfo));
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
