/**
 * Copyright (c) 2026 VexDB-THU
 */

#ifndef BLOCK_MGR_H
#define BLOCK_MGR_H

#include <vtl/internal/container.hpp>
#include <vtl/disk_container/macro.hpp>

#include "utils/relcache.h"
#include "storage/lmgr.h"
#include "storage/block.h"
#include "storage/bufpage.h"
#include "storage/bufmgr.h"
#include "commands/tablespace.h"

namespace disk_container {

#define CHECK_FLUSH true /* this is not a debug flag */
constexpr size_t PAGE_SIZE = BLCKSZ - MAXALIGN(SizeOfPageHeaderData);

struct PageData {
    Buffer buf;
    Page page;

    bool valid() const { return BufferIsValid(buf); }
    void set_invalid() { buf = InvalidBuffer; }
    void mark_dirty() { MarkBufferDirty(buf); }
    BlockNumber get_blkno() const { return BufferGetBlockNumber(buf); }
};

class BlockMgr {
public:
    Relation _rel;
    BlockMgr(Relation rel) : _rel(rel) {}

    static void lock_page_data_exclusive(const PageData &page_data)
        { LockBuffer(page_data.buf, BUFFER_LOCK_EXCLUSIVE); }
    static void lock_page_data_shared(const PageData &page_data)
        { LockBuffer(page_data.buf, BUFFER_LOCK_SHARE); }
    void logical_lock_page_data_exclusive(const PageData &page_data)
        { LockPage(_rel, page_data.get_blkno(), ExclusiveLock); }
    void logical_lock_page_data_shared(const PageData &page_data)
        { LockPage(_rel, page_data.get_blkno(), ShareLock); }
    void logical_unlock_page_data_exclusive(PageData &page_data)
        { UnlockPage(_rel, page_data.get_blkno(), ExclusiveLock); }
    void logical_unlock_page_data_shared(PageData &page_data)
        { UnlockPage(_rel, page_data.get_blkno(), ShareLock); }

    template <AccessorLockType lock_type>
    static void lock_page_data_custom(PageData &pd)
    {
        switch (lock_type) {
            case AccessorLockType::ReadLock:
                lock_page_data_shared(pd);
                break;
            case AccessorLockType::WriteLock:
                lock_page_data_exclusive(pd);
                break;
            case AccessorLockType::NoLockRW:
                pg_memory_barrier();
                break;
            case AccessorLockType::NoLockRead:
                pg_read_barrier();
                break;
            case AccessorLockType::NoLockWrite:
                pg_write_barrier();
                break;
            case AccessorLockType::ExternalLock:
            case AccessorLockType::NoLockUnsafe:
                /* no-op */
                break;
        }
    }
    template <AccessorLockType lock_type>
    static void unlock_page_data_custom(PageData &pd)
    {
        if (lock_type == AccessorLockType::ReadLock || lock_type == AccessorLockType::WriteLock) {
            unlock_page_data(pd);
        } else {
            /* no-op */
        }
    }
    static void unlock_page_data(PageData &page_data)
    {
        LockBuffer(page_data.buf, BUFFER_LOCK_UNLOCK);
    }
    static void release_page(const PageData &page_data)
    {
        ReleaseBuffer(page_data.buf);
    }
    static void unlock_release_page(PageData &page_data)
    {
        unlock_page_data(page_data);
        release_page(page_data);
    }

    PageData get_page_data(BlockNumber blkno, ReadBufferMode mode = RBM_NORMAL) const
    {
        Assert(blkno >= 0);
        Buffer buf = ReadBufferExtended(_rel, MAIN_FORKNUM, blkno, mode, NULL);
        Assert(BufferIsValid(buf));
        return {buf, PageGetContents(BufferGetPage(buf))};
    }

    BlockNumber reserve_new_pages(size_t num_page, const char *buf, const void *obuf,
                                  size_t opaque_size, ForkNumber fork_num = MAIN_FORKNUM) const
    {
        alignas(PG_IO_ALIGN_SIZE) char page[BLCKSZ] = {0};
        PageInit((Page)page, BLCKSZ, opaque_size);

        if (buf) {
            memcpy(PageGetContents(page), buf, PAGE_SIZE);
            ((PageHeader)page)->pd_lower = ((PageHeader)page)->pd_upper;
        }

        if (obuf) {
            memcpy(PageGetSpecialPointer(page), obuf, opaque_size);
        }

        /* Main-fork graph pages are read through PostgreSQL's shared buffer
         * manager, so they must also be extended through it.  Direct
         * smgrextend() leaves other parallel build backends with stale relation
         * length state: a block number can be published in DiskVector metadata
         * before ReadBuffer() in another backend can see that block.  Hold the
         * extension lock for the whole batch to keep the returned range
         * contiguous, matching the DiskVector layout contract. */
        LockRelationForExtension(_rel, ExclusiveLock);
        BlockNumber res = InvalidBlockNumber;
        for (size_t n = 0; n < num_page; ++n) {
            Buffer buffer = ReadBufferExtended(_rel, fork_num, P_NEW, RBM_NORMAL, NULL);
            LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
            BlockNumber blkno = BufferGetBlockNumber(buffer);
            if (n == 0) {
                res = blkno;
            } else {
                Assert(blkno == res + n);
            }
            memcpy(BufferGetPage(buffer), page, BLCKSZ);
            MarkBufferDirty(buffer);
            UnlockReleaseBuffer(buffer);
        }
        UnlockRelationForExtension(_rel, ExclusiveLock);
        return res;
    }

    void destroy() {}
};

} /* namespace disk_container */

#endif /* BLOCK_MGR_H */
