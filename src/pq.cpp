#include "pg_compat.h"
#include "pq.h"
#include "annkmeans.h"
#include "pq_pg_adapter.h"
#include "quantizer/annkmeans.h"
#include "quantizer/pq_endecode.h"

#include <cstring>
#include <vector>

void ProductQuantizer::set_basic_values(size_t dim, size_t m, size_t nbits_)
{
    d = dim;
    M = m;
    nbits = nbits_;
    set_derived_values();
}

void ProductQuantizer::set_derived_values()
{
    if (d == 0 || M == 0 || d % M != 0) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("PQ: dimension %zu must be a positive multiple of M=%zu", d, M)));
    }
    dsub = d / M;
    code_size = (nbits * M + 7) / 8;
    ksub = 1ULL << nbits;
    // Allow >1GB centroid tables (large d/M/ksub combos). palloc_extended +
    // memset gives the same zero-init as palloc0 without the 1GB cap.
    size_t bytes = d * ksub * sizeof(float);
    centroids = (float *)palloc_extended(bytes, MCXT_ALLOC_HUGE);
    std::memset(centroids, 0, bytes);
}

void ProductQuantizer::set_fvec_L2sqr_ny_nearest_func()
{
    _fvec_L2sqr_ny_nearest_func = ann_helper::get_fvec_L2sqr_ny_nearest_func();
}

void ProductQuantizer::set_fvec_ny_distance_func(Metric metric)
{
    _fvec_ny_distance_func = ann_helper::get_fvec_ny_distance_func(metric);
}

void ProductQuantizer::set_dist_code_func()
{
    _distance_single_code_func = ann_helper::get_distance_single_code_func((uint32)nbits);
    _distance_four_codes_func  = ann_helper::get_distance_four_codes_func((uint32)nbits);
}

void ProductQuantizer::free_resourses()
{
    if (centroids != nullptr) {
        pfree(centroids);
        centroids = nullptr;
    }
}

void ProductQuantizer::set_params(FloatVectorArray subcenters, int m)
{
    for (size_t i = 0; i < ksub; i++) {
        std::memcpy(get_centroids((size_t)m, i),
                    FloatVectorArrayGet(subcenters, i),
                    dsub * sizeof(float));
    }
}

void ProductQuantizer::train(AnnKmeansState *kmeansState, FloatVectorArray samples,
                             int parallelWorkers, int maintenanceWorkMem)
{
    (void)parallelWorkers;  // shared K-means runs serially per subquantizer; M-way parallelism is added later

    using ::vex::quantizer::KMeansState;
    using ::vex::quantizer::PQFloatArray;
    using ::vex::quantizer::PQContext;

    KMeansState shared_state;
    shared_state.skip_check_duplicate = kmeansState ? kmeansState->skipCheckDuplicate : false;
    // ann_helper::distance_func and KMeansDistanceFn are unrelated typedefs
    // that share the (const void*, const void*, uint16) ABI by construction.
    shared_state.distance_fn = kmeansState ? kmeansState->kmeansprocinfo : nullptr;
    shared_state.norm_fn     = kmeansState ? kmeansState->kmeansnormprocinfo : nullptr;
    if (shared_state.distance_fn == nullptr) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("PQ: train called without a distance function")));
    }

    PQContext ctx;
    ctx.allocator = vex_pg::PgPQAllocator();
    // TODO(stage5): wire ctx.parallel for per-subquantizer parallelism.
    // Forward shared-layer progress events to PG's ereport(NOTICE). M is
    // small (typically <= 64) so one notice per subquantizer is fine.
    ctx.progress.fn = [M_total = M](size_t done, size_t total, const char *stage) {
        (void)M_total;
        ereport(NOTICE, (errmsg("vex PQ: %s %zu/%zu",
                                stage ? stage : "progress", done, total)));
    };

    const size_t n = (size_t)samples->length;
    if (n == 0) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("PQ: train called with empty sample set")));
    }

    // Scratch buffers reused across the M subquantizers. Sizes are
    // M-invariant so a single palloc pair amortises the allocation cost.
    PQFloatArray subvecs;
    subvecs.maxlen = n;
    subvecs.length = n;
    subvecs.dim    = dsub;
    subvecs.data   = (float *)palloc(n * dsub * sizeof(float));

    PQFloatArray subcenters;
    subcenters.maxlen = ksub;
    subcenters.dim    = dsub;
    subcenters.data   = (float *)palloc(ksub * dsub * sizeof(float));

    ctx.progress.Report(0, M, "kmeans subq");
    for (size_t m = 0; m < M; m++) {
        for (size_t j = 0; j < n; j++) {
            const float *src = (const float *)FloatVectorArrayGet(samples, j);
            std::memcpy(subvecs.Get(j), src + m * dsub, dsub * sizeof(float));
        }
        subcenters.length = 0;  // AnnKmeans treats input length as "already computed centers"

        vex_pg::PgQuantizerCall([&]() {
            ::vex::quantizer::AnnKmeans(shared_state, subvecs, subcenters,
                                         maintenanceWorkMem, ctx);
        });

        for (size_t i = 0; i < ksub; i++) {
            std::memcpy(get_centroids(m, i), subcenters.Get(i),
                        dsub * sizeof(float));
        }
        ctx.progress.Report(m + 1, M, "kmeans subq");
    }

    pfree(subvecs.data);
    pfree(subcenters.data);
}

namespace {

template <class Encoder>
void compute_code_generic(const ProductQuantizer &pq, const float *x, uint8_t *code,
                          float *distances_scratch)
{
    Encoder encoder(code, (int)pq.nbits);
    for (size_t m = 0; m < pq.M; m++) {
        const float *xsub = x + m * pq.dsub;
        uint64_t idxm = pq._fvec_L2sqr_ny_nearest_func(
            distances_scratch, xsub, pq.get_centroids(m, 0),
            (uint32)pq.dsub, (uint32)pq.ksub);
        encoder.encode(idxm);
    }
    encoder.restore_code();
}

} // namespace

void ProductQuantizer::compute_code(const float *x, uint8_t *code) const
{
    using ::vex::quantizer::PQEncoder8;
    using ::vex::quantizer::PQEncoder16;
    using ::vex::quantizer::PQEncoderGeneric;
    if (ksub <= 4096) {
        float distances[4096];
        switch (nbits) {
            case 8:  compute_code_generic<PQEncoder8>(*this, x, code, distances); break;
            case 16: compute_code_generic<PQEncoder16>(*this, x, code, distances); break;
            default: compute_code_generic<PQEncoderGeneric>(*this, x, code, distances); break;
        }
        return;
    }
    std::vector<float> distances(ksub);
    switch (nbits) {
        case 8:  compute_code_generic<PQEncoder8>(*this, x, code, distances.data()); break;
        case 16: compute_code_generic<PQEncoder16>(*this, x, code, distances.data()); break;
        default: compute_code_generic<PQEncoderGeneric>(*this, x, code, distances.data()); break;
    }
}

float ProductQuantizer::distance_to_code(const uint8_t *code, const float *distTable)
{
    return _distance_single_code_func((uint32)M, (uint32)nbits, distTable, code);
}

void ProductQuantizer::distance_to_four_code(const float *distTable,
    const uint8_t *code0, const uint8_t *code1, const uint8_t *code2, const uint8_t *code3,
    float &result0, float &result1, float &result2, float &result3)
{
    _distance_four_codes_func((uint32)M, (uint32)nbits, distTable,
                               code0, code1, code2, code3,
                               result0, result1, result2, result3);
}

void ProductQuantizer::compute_distance_table(const float *x, float *dist_table) const
{
    for (size_t m = 0; m < M; m++) {
        _fvec_ny_distance_func(dist_table + m * ksub, x + m * dsub,
                               get_centroids(m, 0),
                               (uint32)dsub, (uint32)ksub);
    }
}

void PQDistancer::train(Relation index, FloatVectorArray samples, size_t dimension,
    Metric metric, bool need_norm, int parallel_workers, int maintenance_work_mem)
{
    (void)index;

    uint16 m = 0;
    uint16 k = 0;
    pq_set_param((uint32)dimension, m, k);
    pq.set_basic_values(dimension, (size_t)m, 8 /*nbits*/);
    pq.set_fvec_L2sqr_ny_nearest_func();
    pq.set_fvec_ny_distance_func(metric);
    pq.set_dist_code_func();

    AnnKmeansState kstate;
    std::memset(&kstate, 0, sizeof(kstate));
    kstate.skipCheckDuplicate = false;
    kstate.metric = metric;
    kstate.kmeansprocinfo     = ann_helper::get_general_distance_func(metric);
    kstate.kmeansnormprocinfo = nullptr;
    (void)need_norm;

    pq.train(&kstate, samples, parallel_workers, maintenance_work_mem);
    _get_distance_precise_func = ann_helper::get_general_distance_func(metric);
    flag = (metric == Metric::INNER_PRODUCT) ? -1.0f : 1.0f;
    prepared = false;
    dist_table = nullptr;
}

void PQDistancer::prepare(Relation index, void *metap)
{
    (void)index;
    (void)metap;
    if (dist_table == nullptr) {
        // compute_distance_table writes every entry, so palloc (no zero) suffices.
        dist_table = (float *)palloc(pq.M * pq.ksub * sizeof(float));
    }
    prepared = true;
}

void PQDistancer::process(const char *query)
{
    if (!prepared || dist_table == nullptr) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("PQDistancer::process called before prepare")));
    }
    pq.compute_distance_table((const float *)query, dist_table);
}

void PQDistancer::destroy()
{
    if (dist_table != nullptr) {
        pfree(dist_table);
        dist_table = nullptr;
    }
    pq.free_resourses();
    prepared = false;
}

void PQDistancer::flush(Relation index, BlockNumber qtcode_block, bool enabling)
{
    // PG HNSW persistence path is dormant — graph_index_build.cpp declares
    // Variant<PQDistancer,...> but never instantiates the PQ branch.
    (void)index;
    (void)qtcode_block;
    (void)enabling;
}

void PQDistancer::hnsw_read_pq_center(Relation index, ProductQuantizer &target,
                                      BlockNumber qtcode_block)
{
    (void)index;
    (void)target;
    (void)qtcode_block;
}
