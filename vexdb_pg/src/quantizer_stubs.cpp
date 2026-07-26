#include "pg_compat.h"
#include "quantizer.h"
#include "annkmeans.h"
#include "module/timer.h"
#include "rabitq/estimator.h"
#include "pq.h"

#include <algorithm>
#include <cstdint>

namespace {

uint64_t NextReservoirRandom(uint64_t &state)
{
    // SplitMix64: deterministic across PG versions and platforms, with no
    // dependency on backend-global PRNG state.
    state += UINT64_C(0x9E3779B97F4A7C15);
    uint64_t z = state;
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

uint64_t ReservoirIndex(uint64_t &state, uint64_t upper_bound)
{
    return upper_bound == 0 ? 0 : NextReservoirRandom(state) % upper_bound;
}

} // namespace

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

    // One-pass reservoir sampling keeps memory bounded and gives every valid
    // row the same chance even when the heap is ordered by time, tenant or
    // cluster. A fixed seed keeps CREATE INDEX output reproducible.
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

    uint64_t seen = 0;
    uint64_t scanned = 0;
    // Do not mix the relation OID into the seed. The same ordered data should
    // train the same codebook after dump/restore or in a freshly created test
    // database, where OIDs are expected to differ.
    uint64_t random_state = UINT64_C(0x6A09E667F3BCC909) ^
                            (uint64_t)sample_nums ^ (uint64_t)dimensions;
    while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        scanned++;
        econtext->ecxt_scantuple = slot;
        FormIndexDatum(indexInfo, slot, estate, values, isnull);
        if (!isnull[0]) {
            FloatVector *fv = DatumGetFloatVector(values[0]);
            if (fv->dim == dimensions) {
                uint64_t sample_index;
                if (seen < (uint64_t)sample_nums) {
                    sample_index = seen;
                } else {
                    sample_index = ReservoirIndex(random_state, seen + 1);
                }
                if (sample_index < (uint64_t)sample_nums) {
                    float *sample = FloatVectorArrayGet(samples, (int)sample_index);
                    std::memcpy(sample, fv->x, sizeof(float) * dimensions);
                    if (norm_func != nullptr) {
                        norm_func(sample, dimensions, sample);
                    }
                }
                seen++;
            }
        }
        ResetExprContext(econtext);
        if ((scanned & UINT64_C(0xFFF)) == 0) {
            CHECK_FOR_INTERRUPTS();
        }
    }
    samples->length = (int)std::min<uint64_t>(seen, (uint64_t)sample_nums);
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
