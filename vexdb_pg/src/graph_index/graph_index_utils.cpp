#include "pg_compat.h"

#include "graph_index/graph_index_struct.h"
#include "graph_index/graph_index_storage.h"
#include "graph_index/graph_index_xlog.h"
#include "graph_index/graph_index_state.h"
#include "annkmeans.h"

int graph_index_get_m(Relation index)
{
    GraphIndexOptions *opts = (GraphIndexOptions *)index->rd_options;
    return opts ? opts->m : GRAPH_INDEX_DEFAULT_M;
}

int graph_index_get_ef_construction(Relation index)
{
    GraphIndexOptions *opts = (GraphIndexOptions *)index->rd_options;
    return opts ? opts->ef_construction : GRAPH_INDEX_DEFAULT_EF_CONSTRUCTION;
}

int graph_index_get_build_parallel(Relation index)
{
    GraphIndexOptions *opts = (GraphIndexOptions *)index->rd_options;
    return opts ? opts->parallel_workers : 0;
}

QuantizerType graph_index_get_quantizer_type(Relation index)
{
    GraphIndexOptions *opts = (GraphIndexOptions *)index->rd_options;
    if (opts != NULL && opts->qt_type_offset > 0) {
        return extract_qt((const char *)opts + opts->qt_type_offset);
    }

    /* Match the DuckDB contract: compact without an explicit quantizer uses
     * PQ.  RaBitQ compact remains explicit because choosing it changes the
     * approximation and training requirements. */
    if (graph_index_get_compact_mode(index)) {
        return QuantizerType::PQ;
    }

    return (QuantizerType)GRAPH_INDEX_DEFAULT_QUANTIZER_TYPE;
}

int graph_index_get_cluster_rate(Relation index)
{
    if (!index->rd_options) {
        return 0;
    }
    return ((GraphIndexOptions *)index->rd_options)->cluster_rate;
}

IdType graph_index_get_id_type(Relation index)
{
    GraphIndexOptions *opts = (GraphIndexOptions *)index->rd_options;
    return IdType::U32;
}

bool graph_index_get_enable_async_insert(Relation index)
{
    GraphIndexOptions *opts = (GraphIndexOptions *)index->rd_options;
    return opts ? opts->enable_async_insert : false;
}

/* Stage 4 duck-parity reloption accessors. */
int graph_index_get_pq_m(Relation index)
{
    GraphIndexOptions *opts = (GraphIndexOptions *)index->rd_options;
    return opts ? opts->pq_m : 0;
}

bool graph_index_get_compact_mode(Relation index)
{
    GraphIndexOptions *opts = (GraphIndexOptions *)index->rd_options;
    if (!opts || opts->memory_mode_offset <= 0) {
        return false;
    }
    const char *s = (const char *)opts + opts->memory_mode_offset;
    return pg_strcasecmp(s, "compact") == 0;
}

int graph_index_get_threads(Relation index)
{
    GraphIndexOptions *opts = (GraphIndexOptions *)index->rd_options;
    if (!opts) return 1;
    if (opts->threads > 0) return opts->threads;
    return opts->parallel_workers > 0 ? opts->parallel_workers : 1;
}

const char *graph_index_get_metric_str(Relation index)
{
    GraphIndexOptions *opts = (GraphIndexOptions *)index->rd_options;
    if (!opts || opts->metric_offset <= 0) {
        return "l2";
    }
    return (const char *)opts + opts->metric_offset;
}

FmgrInfo *graph_index_optional_proc_info(Relation rel, uint16 procnum)
{
    if (!OidIsValid(index_getprocid(rel, 1, procnum)))
        return NULL;

    return index_getprocinfo(rel, 1, procnum);
}

void graph_index_init_page(Buffer buf, Page page)
{
    PageInit(page, BufferGetPageSize(buf), sizeof(GraphIndexPageOpaqueData));
    GRAPH_INDEX_PAGE_GET_OPAQUE(page)->nextblkno = InvalidBlockNumber;
    GRAPH_INDEX_PAGE_GET_OPAQUE(page)->page_id = GRAPH_INDEX_PAGE_ID;
}

void graph_index_store_qt_centroids(Relation index, BlockNumber qtcode_block, const float *center, size_t write_size)
{
    char *cur = (char *)center;
    Buffer buf = ReadBuffer(index, qtcode_block);
    Buffer new_buf = InvalidBuffer;
    for (;;) {
        LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
        Page page = BufferGetPage(buf);
        PageHeader phdr = (PageHeader)page;
        graph_index_init_page(buf, page);

        size_t fs = phdr->pd_upper - phdr->pd_lower;
        char *write_offset = page + phdr->pd_lower;
        size_t ws = std::min(fs, write_size);
        memcpy(write_offset, cur, ws);

        phdr->pd_lower += ws;
        cur += ws;
        write_size -= ws;
        if (write_size > 0) {
            GraphIndexPageOpaque opaque = GRAPH_INDEX_PAGE_GET_OPAQUE(page);
            new_buf = ReadBuffer(index, P_NEW);
            opaque->nextblkno = BufferGetBlockNumber(new_buf);
        }
        MarkBufferDirty(buf);
        LockBuffer(buf, BUFFER_LOCK_UNLOCK);
        ReleaseBuffer(buf);
        if (write_size == 0) {
            break;
        }
        buf = new_buf;
    }
}

FloatVectorArray graph_index_quantizer_sample_data(Relation heap, Relation index, size_t dimension,
    bool need_norm, DistPrecisionType precision_type, int parallel_workers, size_t sample_nums)
{
    (void)parallel_workers;
    FloatVectorArray samples = FloatVectorArrayInit(sample_nums, dimension);
    ann_sample_rows(samples, heap, index, dimension, sample_nums, need_norm, precision_type);
    return samples;
}

bool try_set_under_redistrib(Relation index, uint32 id)
{
    return true;
}

void reset_under_redistrib(Relation index, uint32 id)
{
    //
}

extern "C" {
#include "utils/hsearch.h"
#include "storage/shmem.h"
}

static HTAB *gi_state_hash = NULL;
LWLock *GraphIndexStateLock = NULL;

void graph_index_state_init(void)
{
    HASHCTL hash_ctl;
    MemSet(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = sizeof(Oid);
    hash_ctl.entrysize = sizeof(GIStateEntry);

    gi_state_hash = VexShmemInitHash(
        "Graph Index State",
        128,
        &hash_ctl,
        HASH_ELEM | HASH_BLOBS
    );

    GraphIndexStateLock = &(GetNamedLWLockTranche("graph_index_state")->lock);
}

void graph_index_get_state(Relation index, GIStateOper op, GIStateInput &input)
{
    Oid keyid = RelationGetRelid(index);
    bool found;

    switch (op) {
        case GIStateOper::GET_UNDER_VACUUM:
            LWLockAcquire(GraphIndexStateLock, LW_SHARED);
            {
                GIStateEntry *entry = (GIStateEntry *)
                    hash_search(gi_state_hash, &keyid, HASH_FIND, &found);
                input.bool_val.val = found ? entry->under_vacuum : 0;
                input.bool_val.set_result = true;
            }
            LWLockRelease(GraphIndexStateLock);
            break;

        case GIStateOper::SET_UNDER_VACUUM:
            LWLockAcquire(GraphIndexStateLock, LW_EXCLUSIVE);
            {
                if (input.bool_val.val) {
                    GIStateEntry *entry = (GIStateEntry *)
                        hash_search(gi_state_hash, &keyid, HASH_ENTER, &found);
                    if (!found) {
                        entry->index_oid = keyid;
                        entry->under_vacuum = 0;
                        entry->under_qt_update = 0;
                        entry->under_async_insert = 0;
                    }
                    entry->under_vacuum = 1;
                } else {
                    GIStateEntry *entry = (GIStateEntry *)
                        hash_search(gi_state_hash, &keyid, HASH_FIND, &found);
                    if (found) {
                        entry->under_vacuum = 0;
                        if (!entry->under_qt_update &&
                            !entry->under_async_insert)
                            hash_search(gi_state_hash, &keyid,
                                        HASH_REMOVE, NULL);
                    }
                }
                input.bool_val.set_result = true;
            }
            LWLockRelease(GraphIndexStateLock);
            break;
    }
}
