#include "pg_compat.h"

#include "graph_index/graph_index_struct.h"
#include "graph_index/graph_index_storage.h"
#include "graph_index/graph_index_xlog.h"
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
    return opts != NULL && opts->qt_type_offset > 0 ?
        extract_qt((const char *)opts + opts->qt_type_offset) :
        (QuantizerType)GRAPH_INDEX_DEFAULT_QUANTIZER_TYPE;
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

void graph_index_store_qt_centroids(Relation index, BlockNumber qtcode_block, const float *center,
    size_t write_size, bool enabling)
{
    char *cur = (char *)center;
    Buffer buf = ReadBuffer(index, qtcode_block);
    Buffer new_buf = InvalidBuffer;
    for (;;) {
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
        if (enabling) {
            GraphIndexXlog xlog;
            xlog.write_centroids(buf, page);
        }
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
    FloatVectorArray samples = FloatVectorArrayInit(sample_nums, dimension);
    ann_sample_rows(samples, heap, index, dimension, parallel_workers, need_norm, precision_type);
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
