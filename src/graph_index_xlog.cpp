#include "pg_compat.h"
#include "graph_index/graph_index_xlog.h"

void graph_index_redo(XLogReaderState *record)
{
    uint8 info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
    switch (info) {
        case XLOG_GRAPH_INDEX_UNLOG_BUILD_INDEX:
        case XLOG_GRAPH_INDEX_BUILD_INDEX:
            GraphIndexXlog::graph_index_redo_build_index(record);
            break;
        case XLOG_GRAPH_INDEX_WRITE_META:
            GraphIndexXlog::redo_meta(record);
            break;
        case XLOG_GRAPH_INDEX_ADD_VECTOR:
            GraphIndexXlog::redo_add_vector(record);
            break;
        case XLOG_GRAPH_INDEX_INVALIDATE_VECTOR_CACHE:
            GraphIndexXlog::redo_invalidate_vector_cache(record);
            break;
        default:
            ereport(PANIC, (errmsg("graph_index_redo: unknown op code %u", info)));
    }
}
