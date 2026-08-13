/**
 * Copyright (c) 2026 VexDB-THU
 *
 * PostgreSQL vector storage implemented with standard pages, shared_buffers,
 * and Generic WAL.  Logical vector bytes are packed into the usable payload
 * area of each page; page headers remain valid for checksums, backup and WAL.
 */

#include "pg_compat.h"

#include "vector_page_storage.h"

#include "distance/core/distance.h"
#include "graph_index/parallel_build_locks.h"

extern "C" {
#include "access/generic_xlog.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
}

static inline BlockNumber
vector_logical_block(off_t offset)
{
    return (BlockNumber)((uint64)offset / VEX_VECTOR_PAGE_PAYLOAD_SIZE);
}

static inline size_t
vector_page_offset(off_t offset)
{
    return (size_t)((uint64)offset % VEX_VECTOR_PAGE_PAYLOAD_SIZE);
}

static inline char *
vector_page_payload(Page page)
{
    return page + VEX_VECTOR_PAGE_HEADER_SIZE;
}

static void
vector_set_page_high_water(Page page, size_t end_offset)
{
    PageHeader header = (PageHeader)page;
    const size_t lower = VEX_VECTOR_PAGE_HEADER_SIZE + end_offset;
    if (header->pd_lower < lower) {
        header->pd_lower = (LocationIndex)lower;
    }
}

static void
vector_init_page(Relation rel, Buffer buffer, bool need_wal)
{
    if (need_wal && RelationNeedsWAL(rel)) {
        GenericXLogState *state = GenericXLogStart(rel);
        Page page = GenericXLogRegisterBuffer(state, buffer,
                                              GENERIC_XLOG_FULL_IMAGE);
        PageInit(page, BLCKSZ, 0);
        GenericXLogFinish(state);
    } else {
        PageInit(BufferGetPage(buffer), BLCKSZ, 0);
        MarkBufferDirty(buffer);
    }
}

/*
 * Make every page through target_block exist and contain a valid page header.
 * The named lock also serializes PostgreSQL parallel build workers, which are
 * members of the leader's heavyweight-lock group and therefore cannot rely on
 * the relation extension lock alone.
 */
static void
vector_ensure_block(Relation rel, BlockNumber target_block, bool need_wal)
{
    BlockNumber nblocks = RelationGetNumberOfBlocksInFork(rel, VECTOR_FORKNUM);
    if (target_block < nblocks) {
        return;
    }

    LWLock *parallel_lock = vex_graph_build_extension_lock(rel);
    vex_graph_build_lock_acquire(parallel_lock, LW_EXCLUSIVE);
    LockRelationForExtension(rel, ExclusiveLock);

    PG_TRY();
    {
        nblocks = RelationGetNumberOfBlocksInFork(rel, VECTOR_FORKNUM);
        while (nblocks <= target_block) {
            Buffer buffer = ReadBufferExtended(rel, VECTOR_FORKNUM, P_NEW,
                                               RBM_ZERO_AND_LOCK, NULL);
            vector_init_page(rel, buffer, need_wal);
            UnlockReleaseBuffer(buffer);
            ++nblocks;
        }
    }
    PG_CATCH();
    {
        UnlockRelationForExtension(rel, ExclusiveLock);
        vex_graph_build_lock_release(parallel_lock);
        PG_RE_THROW();
    }
    PG_END_TRY();

    UnlockRelationForExtension(rel, ExclusiveLock);
    vex_graph_build_lock_release(parallel_lock);
}

VectorScratch::VectorScratch() : buf(nullptr) {}

VectorScratch::VectorScratch(char *data) : buf(data) {}

char *
VectorScratch::get_vecbuf()
{
    return buf;
}

void
VectorScratch::release()
{
    if (buf != nullptr) {
        free_vector(buf);
        buf = nullptr;
    }
}

VectorReadStatus
vector_storage_read(Relation rel, off_t offset, size_t nbytes, char *buffer,
                    BlockNumber nblocks)
{
    if (offset < 0) {
        return VECTOR_READ_NO_BLOCK;
    }
    if (nbytes == 0) {
        return VECTOR_READ_OK;
    }

    if (!BlockNumberIsValid(nblocks)) {
        RelationGetSmgr(rel);
        nblocks = RelationGetNumberOfBlocksInFork(rel, VECTOR_FORKNUM);
    }
    size_t copied = 0;

    while (copied < nbytes) {
        const BlockNumber block = vector_logical_block(offset);
        const size_t in_page = vector_page_offset(offset);
        const size_t chunk = Min(nbytes - copied,
                                 VEX_VECTOR_PAGE_PAYLOAD_SIZE - in_page);
        if (block >= nblocks) {
            return VECTOR_READ_NO_BLOCK;
        }

        Buffer page_buffer = ReadBufferExtended(rel, VECTOR_FORKNUM, block,
                                                RBM_NORMAL, NULL);
        LockBuffer(page_buffer, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(page_buffer);
        if (PageIsNew(page) || ((PageHeader)page)->pd_lower <
                VEX_VECTOR_PAGE_HEADER_SIZE + in_page + chunk) {
            UnlockReleaseBuffer(page_buffer);
            return VECTOR_READ_NO_BLOCK;
        }
        memcpy(buffer + copied, vector_page_payload(page) + in_page, chunk);
        UnlockReleaseBuffer(page_buffer);

        copied += chunk;
        offset += chunk;
    }

    return VECTOR_READ_OK;
}

VectorReadStatus
vector_storage_pin(Relation rel, off_t offset, size_t nbytes,
                   BlockNumber nblocks, Buffer *buffer, const char **data)
{
    *buffer = InvalidBuffer;
    *data = nullptr;
    if (offset < 0) {
        return VECTOR_READ_NO_BLOCK;
    }
    if (nbytes == 0) {
        return VECTOR_READ_OK;
    }

    const BlockNumber block = vector_logical_block(offset);
    const size_t in_page = vector_page_offset(offset);
    if (nbytes > VEX_VECTOR_PAGE_PAYLOAD_SIZE - in_page) {
        return VECTOR_READ_COPY_REQUIRED;
    }
    if (!BlockNumberIsValid(nblocks)) {
        RelationGetSmgr(rel);
        nblocks = RelationGetNumberOfBlocksInFork(rel, VECTOR_FORKNUM);
    }
    if (block >= nblocks) {
        return VECTOR_READ_NO_BLOCK;
    }

    Buffer page_buffer = ReadBufferExtended(rel, VECTOR_FORKNUM, block,
                                            RBM_NORMAL, NULL);
    LockBuffer(page_buffer, BUFFER_LOCK_SHARE);
    Page page = BufferGetPage(page_buffer);
    if (PageIsNew(page) || ((PageHeader)page)->pd_lower <
            VEX_VECTOR_PAGE_HEADER_SIZE + in_page + nbytes) {
        UnlockReleaseBuffer(page_buffer);
        return VECTOR_READ_NO_BLOCK;
    }

    *buffer = page_buffer;
    *data = vector_page_payload(page) + in_page;
    return VECTOR_READ_OK;
}

void
vector_storage_unpin(Buffer buffer)
{
    if (BufferIsValid(buffer)) {
        UnlockReleaseBuffer(buffer);
    }
}

void
vector_storage_write(Relation rel, off_t offset, size_t nbytes,
                     const char *buffer, bool need_wal)
{
    if (offset < 0) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("negative vector storage offset: %lld",
                        (long long)offset)));
    }
    if (nbytes == 0) {
        return;
    }

    RelationGetSmgr(rel);
    size_t written = 0;
    while (written < nbytes) {
        const BlockNumber block = vector_logical_block(offset);
        const size_t in_page = vector_page_offset(offset);
        const size_t chunk = Min(nbytes - written,
                                 VEX_VECTOR_PAGE_PAYLOAD_SIZE - in_page);

        vector_ensure_block(rel, block, need_wal);
        Buffer page_buffer = ReadBufferExtended(rel, VECTOR_FORKNUM, block,
                                                RBM_NORMAL, NULL);
        LockBuffer(page_buffer, BUFFER_LOCK_EXCLUSIVE);

        if (need_wal && RelationNeedsWAL(rel)) {
            GenericXLogState *state = GenericXLogStart(rel);
            Page page = GenericXLogRegisterBuffer(state, page_buffer, 0);
            memcpy(vector_page_payload(page) + in_page,
                   buffer + written, chunk);
            vector_set_page_high_water(page, in_page + chunk);
            GenericXLogFinish(state);
        } else {
            Page page = BufferGetPage(page_buffer);
            memcpy(vector_page_payload(page) + in_page,
                   buffer + written, chunk);
            vector_set_page_high_water(page, in_page + chunk);
            MarkBufferDirty(page_buffer);
        }

        UnlockReleaseBuffer(page_buffer);
        written += chunk;
        offset += chunk;
    }
}

VectorScratch
read_vector_scratch(Relation rel, size_t loc, size_t vector_size,
                    BlockNumber nblocks)
{
    char *buf = alloc_vector(vector_size);
    read_vector_into(rel, loc, vector_size, buf, nblocks);
    return VectorScratch(buf);
}

void
read_vector_into(Relation rel, size_t loc, size_t vector_size, char *buffer,
                 BlockNumber nblocks)
{
    VectorReadStatus status = vector_storage_read(
        rel, (off_t)(loc * vector_size), vector_size, buffer, nblocks);
    if (unlikely(status != VECTOR_READ_OK)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("could not read vector %zu from index \"%s\"",
                        loc, RelationGetRelationName(rel)),
                 errdetail("vector page read status: %d", (int)status),
                 errhint("REINDEX the vexdb_graph index.")));
    }
}

void
create_vector_storage(Relation rel)
{
    RelationGetSmgr(rel);
    if (!smgrexists(rel->rd_smgr, VECTOR_FORKNUM)) {
        smgrcreate(rel->rd_smgr, VECTOR_FORKNUM, false);
    }
}
