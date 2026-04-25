/**
 * Copyright ...
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

inline void PageSetChecksumCompat(Page page, BlockNumber blkno)
{
#if PG_VERSION_NUM >= 170000
    (void)PageSetChecksumCopy(page, blkno);
#else
    PageSetChecksum(page, blkno);
#endif
}

struct PageData {
    Buffer buf;
    Page page;

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

        LockRelationForExtension(_rel, ExclusiveLock);
        ((void)0);
        RelationGetSmgr(_rel);
        BlockNumber res = smgrnblocks(_rel->rd_smgr, fork_num);

        for (BlockNumber i = res; i < res + num_page; ++i) {
            PageSetChecksumCompat((Page)page, i);
            smgrextend(_rel->rd_smgr, fork_num, i, page, false);
        }
        UnlockRelationForExtension(_rel, ExclusiveLock);
        return res;
    }

    void destroy() {}
};

} /* namespace disk_container */

#endif /* BLOCK_MGR_H */
