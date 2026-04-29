/**
 * Copyright ...
 */

#ifndef CONTAINER_PLAIN_STORE_H
#define CONTAINER_PLAIN_STORE_H

#include <algorithm>

#include <vtl/internal/container.hpp>
#include <vtl/internal/expr.hpp>
#include <vtl/disk_container/macro.hpp>
#include <vtl/disk_container/disk_container_xlog.hpp>

#include "utils/relcache.h"
#include "storage/itemptr.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"

namespace disk_container {
constexpr uint32 PLAIN_STORE_VERSION_ONE = 1;
struct PlainStoreMetaPageData {
    uint32 magic;
    uint32 version;
    BlockNumber last_blkno;

    void init(BlockNumber blkno)
    {
        magic = PLAIN_STORE_META_MAGIC;
        version = PLAIN_STORE_VERSION_ONE;
        last_blkno = blkno;
    }
};
typedef PlainStoreMetaPageData *PlainStoreMetaPage;

struct PlainStoreOpaqueData {
    uint16 unused;
    uint16 page_id;
    BlockNumber next_blkno;

    void init()
    {
        page_id = PLAIN_STORE_DATA_ID;
        next_blkno = InvalidBlockNumber;
    }
};
typedef PlainStoreOpaqueData *PlainStoreOpaque;

class PlainStore : public BaseObject {
public:
    constexpr static size_t max_size =
        BLCKSZ - SizeOfPageHeaderData - sizeof(PlainStoreOpaqueData) - 16ul;
    struct key : ItemPointerData {
        key() = default;
        key(const key &) = default;
        key(key &&) = default;
        key(const ItemPointerData &other) : ItemPointerData(other) {}
        key &operator=(const key &) = default;
        key &operator=(key &&) = default;
        bool operator!=(const key &rhs) const
        {
            BlockNumber lhs_blkno = BlockIdGetBlockNumber(&ip_blkid);
            BlockNumber rhs_blkno = BlockIdGetBlockNumber(&rhs.ip_blkid);
            if (BlockNumberIsValid(lhs_blkno) && BlockNumberIsValid(rhs_blkno)) {
                return lhs_blkno != rhs_blkno || ip_posid != rhs.ip_posid;
            }
            return !BlockNumberIsValid(lhs_blkno) && !BlockNumberIsValid(rhs_blkno);
        }
        bool operator<(const key &rhs) const
        {
            BlockNumber lhs_blkno = BlockIdGetBlockNumber(&ip_blkid);
            BlockNumber rhs_blkno = BlockIdGetBlockNumber(&rhs.ip_blkid);
            return lhs_blkno < rhs_blkno || (lhs_blkno == rhs_blkno && ip_posid < rhs.ip_posid);
        }
        bool valid() const { return BlockNumberIsValid(BlockIdGetBlockNumber(&ip_blkid)); }
    };
    static constexpr key invalid_key() {
        key res{};
        res.ip_blkid.bi_hi = InvalidBlockNumber >> 16;
        res.ip_blkid.bi_lo = InvalidBlockNumber &0xffff;
        res.ip_posid = InvalidOffsetNumber;
        return res;
    }

    PlainStore(Relation rel, BlockNumber blkno, bool need_xlog)
        : _rel(rel),
          _meta_blkno(blkno),
          _need_xlog(need_xlog && RelationNeedsWAL(_rel))
    {
        _meta_buf = ReadBuffer(_rel, blkno);
    }
    void destroy() { ReleaseBuffer(_meta_buf); }

    static BlockNumber get_plain_store(Relation rel, bool need_xlog, ForkNumber fork = MAIN_FORKNUM)
    {
        LockRelationForExtension(rel, ExclusiveLock);
        Buffer buffer = ReadBufferExtended(rel, fork, P_NEW, RBM_NORMAL, NULL);
        UnlockRelationForExtension(rel, ExclusiveLock);
        LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
        Page page = BufferGetPage(buffer);
        PageInit(page, BLCKSZ, sizeof(PlainStoreOpaqueData));
        auto opaque = (PlainStoreOpaque)PageGetSpecialPointer(page);
        opaque->init();
        opaque->page_id = PLAIN_STORE_META_ID;
        auto meta = (PlainStoreMetaPage)PageGetContents(page);
        BlockNumber res = BufferGetBlockNumber(buffer);
        meta->init(res);
        START_CRIT_SECTION();
        MarkBufferDirty(buffer);
        if (need_xlog && RelationNeedsWAL(rel)) {
            log_newpage_buffer(buffer, false);
        }
        END_CRIT_SECTION();
        UnlockReleaseBuffer(buffer);
        return res;
    }

    key begin() const
    {
        LockBuffer(_meta_buf, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(_meta_buf);
        BlockNumber cur = reinterpret_cast<PlainStoreOpaque>(PageGetSpecialPointer(page))->next_blkno;
        LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK);
        key ptr;
        BlockIdSet(&ptr.ip_blkid, cur);
        ptr.ip_posid = FirstOffsetNumber;
        return ptr;
    }

    /* Return a palloc'ed copy of data.
     * To avoid copy overhead, use template <class F> void get(key ptr, F &&f)
     */
    void *get(key ptr)
    {
        Assert(ptr.valid());
        BlockNumber blkno = BlockIdGetBlockNumber(&ptr.ip_blkid);
        OffsetNumber offset = ptr.ip_posid;
        Buffer buffer = ReadBuffer(_rel, blkno);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(buffer);
        ItemId item_id = PageGetItemId(page, offset);
        uint32 size = ItemIdGetLength(item_id);
        void *res = palloc(size);
        memcpy(res, PageGetItem(page, item_id), size);
        UnlockReleaseBuffer(buffer);
        return res;
    }

    void *get_next(key &ptr)
    {
        Assert(ptr.valid());
        BlockNumber blkno = BlockIdGetBlockNumber(&ptr.ip_blkid);
        OffsetNumber offset = ptr.ip_posid;
        Buffer buffer = ReadBuffer(_rel, blkno);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(buffer);
        ItemId item_id = PageGetItemId(page, offset);
        void *res = palloc(ItemIdGetLength(item_id));
        memcpy(res, PageGetItem(page, item_id), ItemIdGetLength(item_id));
        if (offset < PageGetMaxOffsetNumber(page)) {
            ItemPointerSetOffsetNumber(&ptr, OffsetNumberNext(offset));
        } else {
            auto opaque = (PlainStoreOpaque)PageGetSpecialPointer(page);
            ItemPointerSet(&ptr, opaque->next_blkno, FirstOffsetNumber);
        }
        UnlockReleaseBuffer(buffer);
        return res;
    }

    template <class F>
    void get(key ptr, F &&f)
    {
        static_assert(IS_INVOCABLE(F, const void *, Size), "F must be called with (const void *, Size)");
        Assert(ptr.valid());
        BlockNumber blkno = BlockIdGetBlockNumber(&ptr.ip_blkid);
        OffsetNumber offset = ptr.ip_posid;
        Buffer buffer = ReadBuffer(_rel, blkno);
        LockBuffer(buffer, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(buffer);
        ItemId item_id = PageGetItemId(page, offset);
        f(PageGetItem(page, item_id), ItemIdGetLength(item_id));
        UnlockReleaseBuffer(buffer);
    }

    template <class F>
    void citerate(key start, F &&f)
    {
        static_assert(IS_INVOCABLE_R(F, bool, const void *, Size), "F must be bool(const void *, Size)");
        BlockNumber blkno = BlockIdGetBlockNumber(&start.ip_blkid);
        OffsetNumber offset = start.ip_posid;
        while (BlockNumberIsValid(blkno)) {
            Buffer buffer = ReadBuffer(_rel, blkno);
            LockBuffer(buffer, BUFFER_LOCK_SHARE);
            Page page = BufferGetPage(buffer);
            for (OffsetNumber i = offset; i <= PageGetMaxOffsetNumber(page); i = OffsetNumberNext(i)) {
                ItemId item_id = PageGetItemId(page, i);
                bool res = f(PageGetItem(page, item_id), ItemIdGetLength(item_id));
                if (!res) {
                    UnlockReleaseBuffer(buffer);
                    return;
                }
            }
            auto opaque = (PlainStoreOpaque)PageGetSpecialPointer(page);
            blkno = opaque->next_blkno;
            UnlockReleaseBuffer(buffer);
            offset = FirstOffsetNumber;
        }
    }

    template <class F>
    void citerate(F &&f)
    {
        static_assert(IS_INVOCABLE_R(F, bool, const void *, Size), "F must be bool(const void *, Size)");
        Page page = BufferGetPage(_meta_buf);
        BlockNumber blkno = reinterpret_cast<PlainStoreOpaque>(PageGetSpecialPointer(page))->next_blkno;
        OffsetNumber offset = FirstOffsetNumber;
        while (BlockNumberIsValid(blkno)) {
            Buffer buffer = ReadBuffer(_rel, blkno);
            LockBuffer(buffer, BUFFER_LOCK_SHARE);
            page = BufferGetPage(buffer);
            for (OffsetNumber i = offset; i <= PageGetMaxOffsetNumber(page); i = OffsetNumberNext(i)) {
                ItemId item_id = PageGetItemId(page, i);
                bool res = f(PageGetItem(page, item_id), ItemIdGetLength(item_id));
                if (!res) {
                    UnlockReleaseBuffer(buffer);
                    return;
                }
            }
            auto opaque = (PlainStoreOpaque)PageGetSpecialPointer(page);
            blkno = opaque->next_blkno;
            UnlockReleaseBuffer(buffer);
            offset = FirstOffsetNumber;
        }
    }

    template <class F>
    void vacuum(F &&f)
    {
        static_assert(IS_INVOCABLE_R(F, bool, const key &), "F must be bool(const key &)");
        LockBuffer(_meta_buf, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(_meta_buf);
        BlockNumber cur = reinterpret_cast<PlainStoreOpaque>(PageGetSpecialPointer(page))->next_blkno;
        LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK);
        key ptr;
        BlockIdSet(&ptr.ip_blkid, cur);
        while (BlockNumberIsValid(cur)) {
            Buffer buffer = ReadBuffer(_rel, cur);
            LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
            Page page = BufferGetPage(buffer);
            auto opaque = (PlainStoreOpaque)PageGetSpecialPointer(page);
            BlockNumber next = opaque->next_blkno;
            for (ptr.ip_posid = FirstOffsetNumber;
                 ptr.ip_posid <= PageGetMaxOffsetNumber(page);
                 ptr.ip_posid = OffsetNumberNext(ptr.ip_posid)) {
                if (ItemIdIsUsed(PageGetItemId(page, ptr.ip_posid)) && f(ptr)) {
                    PageIndexTupleDelete(page, ptr.ip_posid);
                    MarkBufferDirty(buffer);
                    if (_need_xlog) {
                        disk_container::DiskVectorLogger dv_logger(_rel);
                        dv_logger.xl_inplace_filter_delete_item(buffer, page, ptr.ip_posid);
                    }
                }
            }
            /* Update all regardless sizes as FSM info may be lost during switchover */
            RecordPageWithFreeSpace(_rel, cur, PageGetFreeSpace(page));
            UnlockReleaseBuffer(buffer);
            cur = next;
            BlockIdSet(&ptr.ip_blkid, cur);
        }
        FreeSpaceMapVacuum(_rel);
    }

    key put(const void *data, Size size)
    {
        BlockNumber blkno = GetPageWithFreeSpace(_rel, size);
        Buffer buffer;
        while (BlockNumberIsValid(blkno)) {
            buffer = ReadBuffer(_rel, blkno);
            LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
            Page page = BufferGetPage(buffer);
            Size cur_size = PageGetFreeSpace(page);
            if (cur_size >= size) {
                break;
            }
            UnlockReleaseBuffer(buffer);
            blkno = RecordAndGetPageWithFreeSpace(_rel, blkno, cur_size, size);
        }
        if (!BlockNumberIsValid(blkno)) {
            buffer = get_new_page_locked(blkno);
            if (unlikely(!BlockNumberIsValid(blkno))) {
                ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                                errmsg("failed to get new page in \"%s\"", RelationGetRelationName(_rel))));
            }
        }
        Page page = BufferGetPage(buffer);
        OffsetNumber offset = PageAddItem(page, (const void*)data, size, InvalidOffsetNumber, false, false);
        if (unlikely(offset == InvalidOffsetNumber)) {
            ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                            errmsg("Failed to add item to index page in \"%s\"",
                                   RelationGetRelationName(_rel))));
        }
        MarkBufferDirty(buffer);
        RecordPageWithFreeSpace(_rel, blkno, PageGetFreeSpace(page));
        if (_need_xlog) {
            disk_container::DiskVectorLogger dv_logger(_rel);
            dv_logger.xl_inplace_filter_add_item(buffer, page, (char *)data, size, offset, false);
        }
        UnlockReleaseBuffer(buffer);
        key res;
        ItemPointerSet(static_cast<ItemPointer>(&res), blkno, offset);
        return res;
    }

    key set(key ptr, const void *data, Size size)
    {
        Assert(ptr.valid());
        BlockNumber blkno = BlockIdGetBlockNumber(&ptr.ip_blkid);
        OffsetNumber offset = ptr.ip_posid;
        Buffer buffer = ReadBuffer(_rel, blkno);
        LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
        Page page = BufferGetPage(buffer);
        disk_container::DiskVectorLogger dv_logger(_rel);
        if (PageIndexTupleOverwrite(page, offset, (const void*)data, size)) {
            MarkBufferDirty(buffer);
            if (_need_xlog) {
                dv_logger.xl_inplace_filter_add_item(buffer, page, (char *)data, size, offset, true);
            }
            UnlockReleaseBuffer(buffer);
            return ptr;
        }
        PageIndexTupleDelete(page, offset);
        RecordPageWithFreeSpace(_rel, blkno, PageGetFreeSpace(page));
        MarkBufferDirty(buffer);
        if (_need_xlog) {
            dv_logger.xl_inplace_filter_delete_item(buffer, page, offset);
        }
        UnlockReleaseBuffer(buffer);
        return put(data, size);
    }

    template <class F>
    bool set(key ptr, F &&f)
    {
        static_assert(IS_INVOCABLE_R(F, bool, void *, Size), "F must be bool(void *, Size)");
        Assert(ptr.valid());
        BlockNumber blkno = BlockIdGetBlockNumber(&ptr.ip_blkid);
        OffsetNumber offset = ptr.ip_posid;
        Buffer buffer = ReadBuffer(_rel, blkno);
        LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
        Page page = BufferGetPage(buffer);
        ItemId item_id = PageGetItemId(page, offset);
        void *content = PageGetItem(page, item_id);
        Size size = ItemIdGetLength(item_id);
        bool res = f(content, size);
        if (res) {
            MarkBufferDirty(buffer);
            if (_need_xlog) {
                disk_container::DiskVectorLogger dv_logger(_rel);
                dv_logger.xl_inplace_filter_add_item(buffer, page, (char *)content, offset, size, true);
            }
        }
        UnlockReleaseBuffer(buffer);
        return res;
    }

    void erase(key ptr)
    {
        Assert(ptr.valid());
        BlockNumber blkno = BlockIdGetBlockNumber(&ptr.ip_blkid);
        OffsetNumber offset = ptr.ip_posid;
        Buffer buffer = ReadBuffer(_rel, blkno);
        LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
        Page page = BufferGetPage(buffer);
        PageIndexTupleDelete(page, offset);
        RecordPageWithFreeSpace(_rel, blkno, PageGetFreeSpace(page));
        MarkBufferDirty(buffer);
        if (_need_xlog) {
            disk_container::DiskVectorLogger dv_logger(_rel);
            dv_logger.xl_inplace_filter_delete_item(buffer, page, offset);
        }
        UnlockReleaseBuffer(buffer);
    }

    void erase(key *ptr, size_t n, bool sorted = false)
    {
        if (n == 0) {
            return;
        }
        if (sorted) {
            std::sort(ptr, ptr + n);
        }
        OffsetNumber *offsets = (OffsetNumber *)palloc(sizeof(OffsetNumber) * 2048u);
        size_t start = 0;
        size_t end = 0;
        size_t i = 1;
        BlockNumber blkno = ItemPointerGetBlockNumberNoCheck(ptr);
        Assert(BlockNumberIsValid(blkno));
        offsets[0] = ItemPointerGetOffsetNumberNoCheck(ptr);
        while (i < n) {
            if (blkno != ItemPointerGetBlockNumberNoCheck(ptr + i)) {
                end = i;
                erase(blkno, offsets, end - start);
                start = end;
                blkno = ItemPointerGetBlockNumberNoCheck(ptr + start);
            }
            offsets[i - start] = ItemPointerGetOffsetNumberNoCheck(ptr + i);
            ++i;
        }
        erase(blkno, offsets, n - start);
    }
private:
    Relation _rel;
    BlockNumber _meta_blkno;
    Buffer _meta_buf;
    bool _need_xlog;

    Buffer get_new_page_locked(BlockNumber &blkno)
    {
        LockRelationForExtension(_rel, ExclusiveLock);
        Buffer buffer = ReadBuffer(_rel, P_NEW);
        UnlockRelationForExtension(_rel, ExclusiveLock);
        LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
        Page page = BufferGetPage(buffer);
        PageInit(page, BLCKSZ, sizeof(PlainStoreOpaqueData));
        auto opaque = (PlainStoreOpaque)PageGetSpecialPointer(page);
        opaque->init();
        START_CRIT_SECTION();
        MarkBufferDirty(buffer);
        disk_container::DiskVectorLogger dv_logger(_rel);
        if (_need_xlog) {
            log_newpage_buffer(buffer, false);
        }
        END_CRIT_SECTION();
        LockBuffer(_meta_buf, BUFFER_LOCK_EXCLUSIVE);
        Page meta_page = BufferGetPage(_meta_buf);
        auto meta = (PlainStoreMetaPage)PageGetContents(meta_page);
        BlockNumber old_last_blkno = meta->last_blkno;
        blkno = BufferGetBlockNumber(buffer);
        meta->last_blkno = blkno;
        MarkBufferDirty(_meta_buf);
        if (_need_xlog) {
            uint32 offset = MAXALIGN(SizeOfPageHeaderData) + offsetof(PlainStoreMetaPageData, last_blkno);
            size_t data_size = sizeof(BlockNumber);
            dv_logger.xl_inplace_filter_add_data(_meta_buf, meta_page, offset, data_size);
        }
        if (old_last_blkno == _meta_blkno) {
            auto opaque = (PlainStoreOpaque)PageGetSpecialPointer(meta_page);
            opaque->next_blkno = blkno;
            MarkBufferDirty(_meta_buf);
            if (_need_xlog) {
                uint32 offset = ((PageHeader)meta_page)->pd_special + offsetof(PlainStoreOpaqueData, next_blkno);
                size_t data_size = sizeof(BlockNumber);
                dv_logger.xl_inplace_filter_add_data(_meta_buf, meta_page, offset, data_size);
            }
            LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK);
        } else {
            LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK);
            Buffer old_last_buffer = ReadBuffer(_rel, old_last_blkno);
            LockBuffer(old_last_buffer, BUFFER_LOCK_EXCLUSIVE);
            Page old_last_page = BufferGetPage(old_last_buffer);
            auto old_last_opaque = (PlainStoreOpaque)PageGetSpecialPointer(old_last_page);
            old_last_opaque->next_blkno = blkno;
            MarkBufferDirty(old_last_buffer);
            if (_need_xlog) {
                uint32 offset = ((PageHeader)old_last_page)->pd_special + offsetof(PlainStoreOpaqueData, next_blkno);
                size_t data_size = sizeof(BlockNumber);
                dv_logger.xl_inplace_filter_add_data(old_last_buffer, old_last_page, offset, data_size);
            }
            UnlockReleaseBuffer(old_last_buffer);
        }
        return buffer;
    }

    void erase(BlockNumber blkno, OffsetNumber *offsets, size_t n)
    {
        Buffer buffer = ReadBuffer(_rel, blkno);
        LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
        Page page = BufferGetPage(buffer);
        PageIndexMultiDelete(page, offsets, n);
        RecordPageWithFreeSpace(_rel, blkno, PageGetFreeSpace(page));
        MarkBufferDirty(buffer);
        if (_need_xlog) {
            disk_container::DiskVectorLogger dv_logger(_rel);
            dv_logger.xl_inplace_filter_multi_delete(buffer, page, offsets, n);
        }
        UnlockReleaseBuffer(buffer);
    }
};
} /* namespace disk_container */

#endif /* CONTAINER_PLAIN_STORE_H */
