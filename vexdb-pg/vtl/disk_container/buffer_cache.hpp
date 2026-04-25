/**
 * Copyright ...
 */

#ifndef CONTAINER_BUFFER_CACHE_H
#define CONTAINER_BUFFER_CACHE_H

#include <utility> /* swap */

#include "storage/block.h"
#include "storage/buf/buf.h"
#include "storage/bufmgr.h"
#include "utils/relcache.h"

namespace disk_container {
class BufferCache {
public:
    BufferCache &operator=(BufferCache &&other)
    {
        if (this == &other) {
            return *this;
        }
        destroy();
        swap(other);
        return *this;
    }
    void destroy()
    {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"  /* GCC bug for false reporting */
        if (!valid()) {
            return;
        }
        ReleaseBuffer(_cur_buf);
#pragma GCC diagnostic pop
        _cur_blkno = InvalidBlockNumber;
        _cur_buf = InvalidBuffer;
    }
    void swap(BufferCache &other)
    {
        std::swap(_cur_blkno, other._cur_blkno);
        std::swap(_cur_buf, other._cur_buf);
    }

    bool valid() const { return BlockNumberIsValid(_cur_blkno); }
    Buffer get_buffer() const { return _cur_buf; }
    BlockNumber get_blkno() const { return _cur_blkno; }

    void load_buffer(Relation rel, BlockNumber blkno)
    {
        if (blkno == _cur_blkno) {
            return;
        }
        if (valid()) {
            ReleaseBuffer(_cur_buf);
        }
        _cur_blkno = blkno;
        _cur_buf = ReadBuffer(rel, blkno);
    }

    char *get_page() const { return ((char *)get_row_page()) + MAXALIGN(SizeOfPageHeaderData); }
    Page get_row_page() const { return BufferGetPage(_cur_buf); }

    template <bool exclusive>
    void lock_buffer()
        { LockBuffer(_cur_buf, exclusive ? BUFFER_LOCK_EXCLUSIVE : BUFFER_LOCK_SHARE); }
    void mark_dirty() { MarkBufferDirty(_cur_buf); }
    void unlock_buffer() { LockBuffer(_cur_buf, BUFFER_LOCK_UNLOCK); }

private:
    Buffer _cur_buf{InvalidBuffer};
    BlockNumber _cur_blkno{InvalidBlockNumber};
};
};  /* namespace disk_container */

#endif /* CONTAINER_BUFFER_CACHE_H */
