#include "pg_compat.h"
#include "quantizer.h"
#include "annkmeans.h"
#include "module/timer.h"
#include "rabitq/estimator.h"
#include "pq.h"

QuantizerType extract_qt(const char *qt_type)
{
    if (qt_type == nullptr) return QuantizerType::NONE;
    if (pg_strcasecmp(qt_type, "pq") == 0)     return QuantizerType::PQ;
    if (pg_strcasecmp(qt_type, "rabitq") == 0) return QuantizerType::RABITQ;
    return QuantizerType::NONE;
}


// Ported from openGauss annkmeans.cpp:setupKmeansState. For PQ training,
// cosine/inner-product collapse to L2_SQRT on (already-or-implicitly)
// normalized vectors — running kmeans directly with SPHERICAL/IP gives a
// codebook that doesn't converge to centroids matching the eventual ADC
// distance computation.
void setupKmeansState(Metric metric, Relation index, AnnKmeansState *kmeanstate,
    int dimension, bool ispq, bool pqtrain)
{
    // index is unused: vexdb-lite's AnnKmeansState has no partIndexName, so
    // we skip openGauss's populate_index_partition_name() call.
    (void)index;
    using ann_helper::get_general_distance_func;
    if (ispq) {
        if (metric == Metric::L2 || metric == Metric::COSINE || metric == Metric::FAST_COSINE) {
            kmeanstate->kmeansprocinfo     = get_general_distance_func(Metric::L2_SQRT, dimension);
            kmeanstate->kmeansnormprocinfo = nullptr;
        } else if (metric == Metric::INNER_PRODUCT) {
            if (pqtrain) {
                kmeanstate->kmeansprocinfo     = get_general_distance_func(Metric::L2_SQRT, dimension);
                kmeanstate->kmeansnormprocinfo = nullptr;
            } else {
                kmeanstate->kmeansprocinfo     = get_general_distance_func(Metric::SPHERICAL, dimension);
                kmeanstate->kmeansnormprocinfo = get_general_distance_func(Metric::L2_NORM, dimension);
            }
        } else {
            elog(ERROR, "Distance Metric type(%d) is not handled during setup kmeans state",
                 (int)metric);
        }
    } else {
        if (metric == Metric::L2) {
            kmeanstate->kmeansprocinfo     = get_general_distance_func(Metric::L2_SQRT, dimension);
            kmeanstate->kmeansnormprocinfo = nullptr;
        } else if (metric == Metric::COSINE || metric == Metric::FAST_COSINE
                   || metric == Metric::INNER_PRODUCT) {
            kmeanstate->kmeansprocinfo     = get_general_distance_func(Metric::SPHERICAL, dimension);
            kmeanstate->kmeansnormprocinfo = get_general_distance_func(Metric::L2_NORM, dimension);
        } else {
            elog(ERROR, "Distance Metric type(%d) is not handled during setup kmeans state",
                 (int)metric);
        }
    }
    kmeanstate->skipCheckDuplicate = pqtrain;
    kmeanstate->metric             = metric;
    kmeanstate->indexName[0]       = '\0';
}

void ann_sample_rows(FloatVectorArray samples, Relation heap, Relation index,
    int dimensions, int sample_nums, bool need_norm, DistPrecisionType dist_type)
{
    (void)dist_type;
    if (heap == NULL || samples == NULL) return;

    ann_helper::vector_preprocess_func norm_func = need_norm
        ? ann_helper::get_vector_preprocess_func(Metric::FAST_COSINE,
              DistPrecisionType::FLOAT, dimensions)
        : nullptr;

    // Sequential scan, take first sample_nums non-null vectors. Not random
    // sampling — fine for codebook training when the table is in roughly
    // arbitrary insertion order. Random sampling can land here later.
    samples->length = 0;
    Snapshot snap = GetActiveSnapshot();
#if PG_VERSION_NUM >= 190000
    TableScanDesc scan = table_beginscan(heap, snap, 0, NULL, 0);
#else
    TableScanDesc scan = table_beginscan(heap, snap, 0, NULL);
#endif
    TupleTableSlot *slot = table_slot_create(heap, NULL);

    // FormIndexDatum handles both plain-column (indkey != 0) and
    // function-expression (indkey == 0, real expr in rd_indexprs) cases.
    // Reading `indkey.values[0]` directly here used to feed attno == 0 into
    // slot_getattr() for any expression index and crash the build.
    IndexInfo *indexInfo = BuildIndexInfo(index);
    EState *estate = CreateExecutorState();
    ExprContext *econtext = GetPerTupleExprContext(estate);
    Datum values[INDEX_MAX_KEYS];
    bool isnull[INDEX_MAX_KEYS];

    int collected = 0;
    while (collected < sample_nums &&
           table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        econtext->ecxt_scantuple = slot;
        FormIndexDatum(indexInfo, slot, estate, values, isnull);
        if (!isnull[0]) {
            FloatVector *fv = DatumGetFloatVector(values[0]);
            if (fv->dim == dimensions) {
                float *sample = FloatVectorArrayGet(samples, collected);
                std::memcpy(sample, fv->x, sizeof(float) * dimensions);
                if (norm_func != nullptr) {
                    norm_func(sample, dimensions, sample);
                }
                collected++;
                samples->length = collected;
            }
        }
        ResetExprContext(econtext);
    }
    FreeExecutorState(estate);
    ExecDropSingleTupleTableSlot(slot);
    table_endscan(scan);
}

void RaBitQMeta::init(uint32 dim)
{
    enabled = false;
    storage_version = 2;
    size_t padded_dim = RABITQ_PADDED_DIM(dim);
    size_t cid_size = rabitq::kCodeHeaderSize;
    size_t bin_size = RABITQ_BIN_DATA_SIZE(padded_dim);
    size_t ext_size = RABITQ_EXT_DATA_SIZE(padded_dim);
    quant_size = cid_size + bin_size + ext_size;
    query_rescaling_factor = rabitq::get_const_scaling_factors(padded_dim, 3);
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

void QuantizerMetaInfo::init(QuantizerType qt_type, uint32 dimension, uint32 requested_pq_m)
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
        pq_set_param(dimension, metainfo.pq_metainfo.m, metainfo.pq_metainfo.k,
                     requested_pq_m);
    } else if (qt_type == QuantizerType::RABITQ) {
        metainfo.rbq_meta.init(dimension);
    }
}
