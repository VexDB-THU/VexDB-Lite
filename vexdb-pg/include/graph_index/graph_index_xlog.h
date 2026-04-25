#ifndef GRAPH_INDEX_XLOG_H
#define GRAPH_INDEX_XLOG_H

#include "c.h"
#include "pg_compat.h"
#include "access/xlog.h"
#include "access/xlogreader.h"
#include "storage/bufpage.h"
#include "storage/bufmgr.h"
#include "access/xlogutils.h"

#include <vtl/internal/expr.hpp>
#include "graph_index/graph_index_struct.h"

#define XLOG_GRAPH_INDEX_BUILD_INDEX 0x00
#define XLOG_GRAPH_INDEX_UNLOG_BUILD_INDEX 0x10
#define XLOG_GRAPH_INDEX_WRITE_META 0x20
#define XLOG_GRAPH_INDEX_ADD_VECTOR 0x30
#define XLOG_GRAPH_INDEX_INVALIDATE_VECTOR_CACHE 0x40

enum class GRAPH_INDEX_META_XLOG_TYPE : uint8 {
    UPDATE_NUM_VECTOR,
    UPDATE_ENTRY_POINT,
    UPDATE_CODE_VERSION,
    UPDATE_CENTROIDS_VERSION,
    UPDATE_CENTROIDS,
    WRITE_NEWCODE,
    INVALID_ALL_CACHE,
    SET_VALID,
    UPDATE_VACUUM_FLAG
};

struct xl_graph_index_add_vec {
    Oid relid;
    BlockNumber blkno;
    OffsetNumber offset;
    int32 nbytes;
};

class GraphIndexXlog {
public:
    GraphIndexXlog() = default;

    void init(Relation index, Buffer metabuf, Page metapage)
    {
        this->index = index;
        this->metabuf = metabuf;
        this->metapage = metapage;
    }

    template<bool need_flush = false, typename record_func>
    void log_meta(GRAPH_INDEX_META_XLOG_TYPE type, record_func &&func)
    {
        if (!RelationNeedsWAL(index)) return;
        XLogBeginInsert();
        XLogRegisterData((char *)&type, sizeof(GRAPH_INDEX_META_XLOG_TYPE));
        XLogRegisterBuffer(0, metabuf, REGBUF_STANDARD);
        func();
        XLogRecPtr lsn = XLogInsert(RM_GRAPH_INDEX_ID, XLOG_GRAPH_INDEX_WRITE_META);
        PageSetLSN(metapage, lsn);
    }

    void update_num_vector(size_t num_vector)
    {
        log_meta(GRAPH_INDEX_META_XLOG_TYPE::UPDATE_NUM_VECTOR, [&]() -> void {
            XLogRegisterBufData(0, (char *)&num_vector, sizeof(size_t));
        });
    }

    void update_entry(GraphIndexEntryInfo entry)
    {
        log_meta(GRAPH_INDEX_META_XLOG_TYPE::UPDATE_ENTRY_POINT, [&]() -> void {
            XLogRegisterBufData(0, (char *)&entry, sizeof(GraphIndexEntryInfo));
        });
    }

    void update_vacuum_flag(bool flag)
    {
        log_meta(GRAPH_INDEX_META_XLOG_TYPE::UPDATE_VACUUM_FLAG, [&]() -> void {
            XLogRegisterBufData(0, (char *)&flag, sizeof(bool));
        });
    }

    void add_vector(const char *value, off_t offset, int nbytes, VecStorageType st)
    {
        if (!RelationNeedsWAL(index)) return;
        xl_graph_index_add_vec xl_rec;
        xl_rec.relid = index->rd_locator.relNumber;
        xl_rec.blkno = InvalidBlockNumber;
        xl_rec.offset = 0;
        xl_rec.nbytes = nbytes;

        XLogBeginInsert();
        XLogRegisterData((char *)&xl_rec, sizeof(xl_graph_index_add_vec));
        XLogRegisterData((char *)value, nbytes);
        XLogInsert(RM_GRAPH_INDEX_ID, XLOG_GRAPH_INDEX_ADD_VECTOR);
    }

    void log_build_index(ForkNumber fork_number) {}
    
    template<typename record_func>
    void log_add_vector(Buffer buf, Page page, char *vec, size_t size, record_func &&func)
    {
        if (!RelationNeedsWAL(index)) return;
        XLogBeginInsert();
        XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
        func();
        XLogRecPtr lsn = XLogInsert(RM_GRAPH_INDEX_ID, XLOG_GRAPH_INDEX_ADD_VECTOR);
        PageSetLSN(page, lsn);
    }

    void log_invalidate_vector_cache(size_t loc, size_t elem_size) {}
    void write_centroids(Buffer buf, Page page) {}

    static void redo_meta(XLogReaderState *record)
    {
        /*
         * PostgreSQL loadable modules cannot rely on xlog redo helper symbols
         * like XLogReadBufferForRedo being exported by the backend binary on all
         * target builds. The current PG port also does not register a separate
         * custom redo manager, so keep redo as a no-op in extension mode.
         */
        (void)record;
    }
    static void redo_add_vector(XLogReaderState *record) {}
    static void redo_invalidate_vector_cache(XLogReaderState *record) {}
    static void graph_index_redo_build_index(XLogReaderState *record) {}

    Relation index = NULL;
    Buffer metabuf = InvalidBuffer;
    Page metapage = NULL;
};

#endif /* GRAPH_INDEX_XLOG_H */
