#include "pg_compat.h"
#include "pq.h"
#include "annkmeans.h"
#include "pq_pg_adapter.h"
#include "quantizer/annkmeans.h"
#include "graph_index/graph_index.h"
#include "graph_index/graph_index_struct.h"
#include "graph_index/graph_index_param.h"

#include <algorithm>
#include <cstring>

void PQDistancer::train(Relation index, FloatVectorArray samples, size_t dimension,
    Metric metric, bool need_norm, int parallel_workers, int maintenance_work_mem,
    uint32 requested_m)
{
    uint16 m = 0;
    uint16 k = 0;
    pq_set_param((uint32)dimension, m, k, requested_m);
    configure_for_metric(dimension, (size_t)m, (size_t)(31 - __builtin_clz((uint32)k)), metric);

    AnnKmeansState *kstate = (AnnKmeansState *)palloc0(sizeof(AnnKmeansState));
    setupKmeansState(metric == Metric::INNER_PRODUCT && !need_norm
                         ? Metric::INNER_PRODUCT : Metric::L2,
                     index, kstate, (int)pq.dsub, /*ispq*/ true, /*pqtrain*/ true);
    ::vex::quantizer::KMeansState shared_state;
    shared_state.skip_check_duplicate = kstate->skipCheckDuplicate;
    shared_state.distance_fn = kstate->kmeansprocinfo;
    shared_state.norm_fn = kstate->kmeansnormprocinfo;

    ::vex::quantizer::PQFloatArray shared_samples;
    shared_samples.data = samples->items;
    shared_samples.length = (size_t)samples->length;
    shared_samples.maxlen = (size_t)samples->maxlen;
    shared_samples.dim = (size_t)samples->dim;

    ::vex::quantizer::PQContext ctx;
    ctx.allocator = vex_pg::PgPQAllocator();
    // PostgreSQL worker processes cannot safely create std::threads. The
    // shared quantizer therefore uses its serial executor in this adapter.
    (void)parallel_workers;
    pq.train(shared_state, shared_samples, maintenance_work_mem, ctx);
    FREE_ANNKEMANSTATE(kstate);

    prepared = false;
    dist_table = nullptr;
}

// Configure ProductQuantizer state (dims + dispatch funcs + metric-derived
// flag) without touching centroids data.
void PQDistancer::configure_for_metric(size_t d, size_t M, size_t nbits_, Metric metric)
{
    ::vex::quantizer::PQContext ctx;
    ctx.allocator = vex_pg::PgPQAllocator();
    pq.set_basic_values(d, M, nbits_);
    pq.set_derived_values(ctx);
    pq.set_fvec_L2sqr_ny_nearest_func();
    pq.set_fvec_ny_distance_func(metric);
    pq.set_dist_code_func();
    _get_distance_precise_func = ann_helper::get_general_distance_func(metric);
    flag = (metric == Metric::INNER_PRODUCT) ? -1.0f : 1.0f;
}

void PQDistancer::prepare(Relation index, void *metap)
{
    if (prepared && dist_table != nullptr) {
        return;
    }
    GraphIndexMetaPage mp = (GraphIndexMetaPage)metap;
    if (mp == nullptr) {
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
            errmsg("PQ centroids unavailable; rebuild the index")));
    }
    Metric m = mp->metric;
    const PQMetaInfo &pqi = mp->quantizer_metainfo.get_pq_metainfo();
    if (!BlockNumberIsValid(mp->qtcode_block) ||
        !pqi.graph_pq || pqi.m == 0 || pqi.k == 0) {
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
            errmsg("PQ centroids unavailable (qtcode_block=%u, pq_m=%u, "
                   "pq_k=%u); rebuild the index",
                   mp->qtcode_block, (unsigned)pqi.m, (unsigned)pqi.k)));
    }
    configure_for_metric(mp->dimension, pqi.m, pqi.nbits(), m);
    hnsw_read_pq_center(index, pq, mp->qtcode_block);
    dist_table = (float *)palloc(pq.M * pq.ksub * sizeof(float));
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
    ::vex::quantizer::PQContext ctx;
    ctx.allocator = vex_pg::PgPQAllocator();
    pq.free_resources(ctx);
    prepared = false;
}

void PQDistancer::flush(Relation index, BlockNumber qtcode_block, bool enabling)
{
    if (index == NULL || !BlockNumberIsValid(qtcode_block) || pq.centroids == nullptr) {
        return;
    }
    const size_t bytes = pq.get_centroids_size() * sizeof(float);
    graph_index_store_qt_centroids(index, qtcode_block, pq.centroids, bytes);
}

// Symmetric to graph_index_store_qt_centroids; `target.centroids` must
// already be allocated (set_basic_values does this) so the read length is
// well-defined.
void PQDistancer::hnsw_read_pq_center(Relation index, ProductQuantizer &target,
                                      BlockNumber qtcode_block)
{
    if (index == NULL || !BlockNumberIsValid(qtcode_block) || target.centroids == nullptr) {
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
            errmsg("vex PQ: hnsw_read_pq_center called with invalid state")));
    }
    const size_t expected = target.get_centroids_size() * sizeof(float);
    char *cur = (char *)target.centroids;
    size_t remaining = expected;
    BlockNumber blk = qtcode_block;
    while (BlockNumberIsValid(blk) && remaining > 0) {
        Buffer buf = ReadBuffer(index, blk);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(buf);
        PageHeader phdr = (PageHeader)page;
        char *contents = (char *)PageGetContents(page);
        size_t avail = (size_t)(page + phdr->pd_lower - contents);
        size_t take = std::min(avail, remaining);
        if (take > 0) {
            memcpy(cur, contents, take);
            cur += take;
            remaining -= take;
        }
        blk = GRAPH_INDEX_PAGE_GET_OPAQUE(page)->nextblkno;
        UnlockReleaseBuffer(buf);
    }
    if (remaining != 0) {
        ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
            errmsg("vex PQ: short read on qtcode_block (missing %zu of %zu bytes)",
                   remaining, expected)));
    }
}
