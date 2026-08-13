#include "pg_compat.h"

#include "annkmeans.h"
#include "floatvector.h"
#include "graph_index/graph_index.h"
#include "graph_index/graph_index_struct.h"
#include "pq_pg_adapter.h"
#include "quantizer/annkmeans.h"
#include "rabitq/rabitq_distancer.h"

namespace rabitq {

void RabitqDistancer::destroy()
{
    if (quantizer.has_value()) {
        quantizer->destroy();
        quantizer.reset();
    }
    if (prepared) {
        estimator.perf_report();
        estimator.destroy();
    }
    prepared = false;
}

void RabitqDistancer::load_rabitq(Relation index, void *metapage)
{
    GraphIndexMetaPage metap = (GraphIndexMetaPage)metapage;
    RaBitQMeta &rabitq_meta = metap->quantizer_metainfo.get_rabitq_meta();
    load_rabitq_cache(index, rabitq_meta);
    new (&estimator) RaBitQEstimator(padded_dim, metric, rabitq_meta.query_rescaling_factor);
    estimator.set_quantizer(&*quantizer);
}

void RabitqDistancer::load_rabitq_quantizer(Relation index, RaBitQMeta &rabitq_meta,
    RaBitQCache &cache)
{
    quantizer.emplace(dim, padded_dim, metric);
    quantizer->set_rescaling_factor(rabitq_meta.query_rescaling_factor);

    size_t random_matrix_size = quantizer->get_random_matrix_size();
    size_t centroids_size = GRAPH_INDEX_RABITQ_NUM_CLUSTERS * dim;
    size_t rotated_centroids_size = GRAPH_INDEX_RABITQ_NUM_CLUSTERS * padded_dim;
    size_t total_fixed_size = random_matrix_size +
        (centroids_size + rotated_centroids_size) * sizeof(float);

    if (index != NULL) {
        cache.fixed_data = (char *)palloc(total_fixed_size);
        read_rabitq_data(index, total_fixed_size, cache.fixed_data);
    }

    char *random_matrix = cache.fixed_data;
    float *centroids = (float *)(random_matrix + random_matrix_size);
    float *rotated_centroids = centroids + centroids_size;
    quantizer->load(random_matrix, centroids, rotated_centroids);
}

void RabitqDistancer::read_rabitq_data(Relation index, size_t rabitq_data_size,
    char *rabitq_data)
{
    char *cur = rabitq_data;
    size_t remaining = rabitq_data_size;
    BlockNumber blk = qtcode_block;

    while (BlockNumberIsValid(blk) && remaining > 0) {
        Buffer buf = ReadBuffer(index, blk);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(buf);
        PageHeader phdr = (PageHeader)page;
        size_t page_size = (size_t)(page + phdr->pd_lower - PageGetContents(page));
        BlockNumber next_blk = GRAPH_INDEX_PAGE_GET_OPAQUE(page)->nextblkno;

        if (page_size > remaining) {
            UnlockReleaseBuffer(buf);
            ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                errmsg("RaBitQ metadata size mismatch: expected %zu bytes",
                    rabitq_data_size)));
        }

        memcpy(cur, PageGetContents(page), page_size);
        cur += page_size;
        remaining -= page_size;
        UnlockReleaseBuffer(buf);
        blk = next_blk;
    }

    if (remaining != 0 || BlockNumberIsValid(blk)) {
        ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
            errmsg("RaBitQ metadata size mismatch: expected %zu bytes, got %zu",
                rabitq_data_size, rabitq_data_size - remaining)));
    }
}

void RabitqDistancer::train(Relation index, FloatVectorArray samples, int dimension,
    Metric metric_arg, bool need_norm, int parallel_workers, int maintenance_work_mem)
{
    (void)need_norm;
    (void)parallel_workers;

    dim = dimension;
    padded_dim = RABITQ_PADDED_DIM(dim);
    metric = metric_arg;
    precise_distance = ann_helper::get_general_distance_func(metric);
    cid_size = kCodeHeaderSize;
    bin_size = RABITQ_BIN_DATA_SIZE(padded_dim);
    size_t ext_size = RABITQ_EXT_DATA_SIZE(padded_dim);
    code_len = cid_size + bin_size + ext_size;

    quantizer.emplace(dim, padded_dim, metric);

    AnnKmeansState pg_state{};
    setupKmeansState(metric, index, &pg_state, dimension, false, false);

    vex::quantizer::KMeansState state;
    state.distance_fn = pg_state.kmeansprocinfo;
    state.norm_fn = pg_state.kmeansnormprocinfo;
    state.skip_check_duplicate = pg_state.skipCheckDuplicate;

    vex::quantizer::PQFloatArray sample_view;
    sample_view.data = samples->items;
    sample_view.length = samples->length;
    sample_view.maxlen = samples->maxlen;
    sample_view.dim = samples->dim;

    vex::quantizer::PQFloatArray centers;
    centers.data = quantizer->get_centroids();
    centers.length = 0;
    centers.maxlen = GRAPH_INDEX_RABITQ_NUM_CLUSTERS;
    centers.dim = dimension;

    vex::quantizer::PQContext ctx;
    ctx.allocator = vex_pg::PgPQAllocator();
    vex_pg::PgQuantizerCall([&]() {
        vex::quantizer::AnnKmeans(state, sample_view, centers,
            maintenance_work_mem, ctx);
    });

    quantizer->train();
}

void RabitqDistancer::load_rabitq_cache(Relation index, RaBitQMeta &rabitq_meta)
{
    /*
     * Keep the first migration correct and PG-memory-context-safe. The fixed
     * data is loaded once per distancer, copied into the quantizer, then freed.
     * A process-local cache can be added later without changing the disk format.
     */
    RaBitQCache cache;
    cache.oid = RelationGetRelid(index);
    load_rabitq_quantizer(index, rabitq_meta, cache);
    pfree(cache.fixed_data);
    cache.fixed_data = NULL;
}

void RabitqDistancer::prepare(Relation index, void *metapage)
{
    if (prepared) {
        return;
    }

    GraphIndexMetaPage metap = (GraphIndexMetaPage)metapage;
    if (index == NULL || metap == NULL) {
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
            errmsg("RaBitQ metadata unavailable; rebuild the index")));
    }

    dim = metap->dimension;
    padded_dim = RABITQ_PADDED_DIM(dim);
    metric = metap->metric;
    precise_distance = ann_helper::get_general_distance_func(metric);
    cid_size = kCodeHeaderSize;
    bin_size = RABITQ_BIN_DATA_SIZE(padded_dim);
    size_t ext_size = RABITQ_EXT_DATA_SIZE(padded_dim);
    code_len = cid_size + bin_size + ext_size;
    qtcode_block = metap->qtcode_block;

    const RaBitQMeta &rabitq_meta = metap->quantizer_metainfo.get_rabitq_meta();
    if (!BlockNumberIsValid(qtcode_block) || !rabitq_meta.enabled ||
        rabitq_meta.storage_version != 2 || rabitq_meta.quant_size != (int)code_len) {
        ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
            errmsg("invalid RaBitQ metadata; rebuild the index")));
    }

    load_rabitq(index, metapage);
    prepared = true;
}

void RabitqDistancer::process(const char *query)
{
    estimator.preprocess((float *)query);
}

void RabitqDistancer::flush(Relation index, BlockNumber qtcode_block_arg, bool enabling)
{
    (void)enabling;
    size_t centroids_size = GRAPH_INDEX_RABITQ_NUM_CLUSTERS * dim * sizeof(float);
    size_t random_matrix_size = quantizer->get_random_matrix_size();
    size_t rotated_centroids_size =
        GRAPH_INDEX_RABITQ_NUM_CLUSTERS * padded_dim * sizeof(float);
    size_t total_size = random_matrix_size + centroids_size + rotated_centroids_size;

    char *rabitq_data = (char *)palloc(total_size);
    char *centroids = rabitq_data + random_matrix_size;
    char *rotated_centroids = centroids + centroids_size;
    memcpy(rabitq_data, quantizer->get_random_matrix(), random_matrix_size);
    memcpy(centroids, quantizer->get_centroids(), centroids_size);
    memcpy(rotated_centroids, quantizer->get_rotated_centroids(),
        rotated_centroids_size);
    graph_index_store_qt_centroids(index, qtcode_block_arg,
        (float *)rabitq_data, total_size);
    pfree(rabitq_data);
}

} /* namespace rabitq */
