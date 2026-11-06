/**
 * Copyright (c) 2026 VexDB-THU
 */

#ifndef CONTAINER_PLAIN_STORE_H
#define CONTAINER_PLAIN_STORE_H

#include <algorithm>

#include <vtl/internal/container.hpp>
#include <vtl/internal/expr.hpp>
#include <vtl/disk_container/macro.hpp>
#include <vtl/disk_container/log_manager.hpp>

#include "utils/relcache.h"
#include "storage/itemptr.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#if defined(PG_VEXDB_TARGET_PG)
#include "graph_index/parallel_build_locks.h"
#endif
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
    /*
     * PageIndexTupleDeleteStable
     *
     * Remove one tuple from an index-like page while keeping all remaining
     * line-pointer offset numbers stable.
     */
    static void PageIndexTupleDeleteStable(Page page, OffsetNumber offnum)
    {
        PageHeader phdr = (PageHeader)page;
        char *addr = NULL;
        ItemId tup;
        Size size;
        unsigned offset;
        OffsetNumber nline;

        if (phdr->pd_lower < SizeOfPageHeaderData || phdr->pd_lower > phdr->pd_upper ||
            phdr->pd_upper > phdr->pd_special || phdr->pd_special > BLCKSZ)
            ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                            errmsg("corrupted page pointers: lower = %u, upper = %u, special = %u", phdr->pd_lower,
                                phdr->pd_upper, phdr->pd_special)));

        nline = PageGetMaxOffsetNumber(page);
        if (offnum <= 0 || offnum > nline)
            ereport(ERROR, (errcode(ERRCODE_INVALID_ROW_COUNT_IN_RESULT_OFFSET_CLAUSE),
                            errmsg("invalid index offnum: %u", offnum)));

        tup = PageGetItemId(page, offnum);
        if (!ItemIdHasStorage(tup) || !ItemIdIsUsed(tup))
            return;

        size = ItemIdGetLength(tup);
        offset = ItemIdGetOffset(tup);
        if (offset < phdr->pd_upper || (offset + size) > phdr->pd_special)
            ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                            errmsg("corrupted item pointer: offset = %u, size = %u", offset, (unsigned int)size)));

        addr = (char *)page + phdr->pd_upper;
        size = MAXALIGN(size);

        if (offset > phdr->pd_upper) {
            memmove(addr + size, addr, (int)(offset - phdr->pd_upper));
        }

        phdr->pd_upper += size;
        ItemIdSetUnused(tup);
        PageSetHasFreeLinePointers(page);

        if (!PageIsEmpty(page)) {
            for (OffsetNumber i = FirstOffsetNumber; i <= nline; i++) {
                ItemId ii = PageGetItemId(page, i);

                if (!ItemIdIsUsed(ii) || !ItemIdHasStorage(ii))
                    continue;
                if (ItemIdGetOffset(ii) <= offset)
                    ii->lp_off += size;
            }
        }
    }
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
          _need_xlog(need_xlog && RelationNeedsWAL(_rel)),
          _log_mgr(_rel)
    {
        _meta_buf = ReadBuffer(_rel, blkno);
    }
    void destroy() { ReleaseBuffer(_meta_buf); }

    static BlockNumber get_plain_store(Relation rel, bool need_xlog, ForkNumber fork = MAIN_FORKNUM)
    {
#if defined(PG_VEXDB_TARGET_PG)
        LWLock *extension_lock = vex_graph_build_extension_lock(rel);
        vex_graph_build_lock_acquire(extension_lock, LW_EXCLUSIVE);
#endif
        LockRelationForExtension(rel, ExclusiveLock);
        Buffer buffer = ReadBufferExtended(rel, fork, P_NEW, RBM_NORMAL, NULL);
        UnlockRelationForExtension(rel, ExclusiveLock);
#if defined(PG_VEXDB_TARGET_PG)
        vex_graph_build_lock_release(extension_lock);
#endif
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
        Assert(ItemIdIsUsed(item_id) && ItemIdHasStorage(item_id));
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
        Assert(ItemIdIsUsed(item_id) && ItemIdHasStorage(item_id));
        void *res = palloc(ItemIdGetLength(item_id));
        memcpy(res, PageGetItem(page, item_id), ItemIdGetLength(item_id));
        key next_ptr;
        bool has_next = false;
        OffsetNumber max_offset = PageGetMaxOffsetNumber(page);
        for (OffsetNumber i = OffsetNumberNext(offset); i <= max_offset; i = OffsetNumberNext(i)) {
            ItemId next_item_id = PageGetItemId(page, i);
            if (ItemIdIsUsed(next_item_id) && ItemIdHasStorage(next_item_id)) {
                ItemPointerSet(static_cast<ItemPointer>(&next_ptr), blkno, i);
                has_next = true;
                break;
            }
        }
        if (!has_next) {
            auto opaque = (PlainStoreOpaque)PageGetSpecialPointer(page);
            has_next = find_next_valid_key(opaque->next_blkno, FirstOffsetNumber, &next_ptr);
        }
        UnlockReleaseBuffer(buffer);
        ptr = has_next ? next_ptr : invalid_key();
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
        Assert(ItemIdIsUsed(item_id) && ItemIdHasStorage(item_id));
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
                if (!(ItemIdIsUsed(item_id) && ItemIdHasStorage(item_id))) {
                    continue;
                }
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
                if (!(ItemIdIsUsed(item_id) && ItemIdHasStorage(item_id))) {
                    continue;
                }
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
    void inspect(F &&f)
    {
        static_assert(IS_INVOCABLE(F, OffsetNumber), "F must be called by (OffsetNumber)");
        Page page = BufferGetPage(_meta_buf);
        BlockNumber blkno = reinterpret_cast<PlainStoreOpaque>(PageGetSpecialPointer(page))->next_blkno;
        while (BlockNumberIsValid(blkno)) {
            Buffer buffer = ReadBuffer(_rel, blkno);
            LockBuffer(buffer, BUFFER_LOCK_SHARE);
            page = BufferGetPage(buffer);
            OffsetNumber offset = PageGetMaxOffsetNumber(page);
            f(offset);
            blkno = ((PlainStoreOpaque)PageGetSpecialPointer(page))->next_blkno;
            UnlockReleaseBuffer(buffer);
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
                ItemId item_id = PageGetItemId(page, ptr.ip_posid);
                if (ItemIdIsUsed(item_id) && ItemIdHasStorage(item_id) && f(ptr)) {
                    OffsetNumber off = ptr.ip_posid;
                    _log_mgr.apply(buffer, _need_xlog, false, [&](Page p) {
                        PageIndexTupleDeleteStable(p, off);
                        return true;
                    });
                }
            }
            Size freespace = PageGetFreeSpace(page);
            UnlockReleaseBuffer(buffer);
            RecordPageWithFreeSpace(_rel, cur, freespace);
            cur = next;
            BlockIdSet(&ptr.ip_blkid, cur);
        }
        FreeSpaceMapVacuum(_rel);
    }

    key put(const void *data, Size size)
    {
        const Size aligned_size = MAXALIGN(size);
        BlockNumber blkno = GetPageWithFreeSpace(_rel, aligned_size);
        Buffer buffer;
        while (BlockNumberIsValid(blkno)) {
            buffer = ReadBuffer(_rel, blkno);
            LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
            Page page = BufferGetPage(buffer);
            Size cur_size = PageGetFreeSpace(page);
            if (cur_size >= aligned_size) {
                break;
            }
            UnlockReleaseBuffer(buffer);
            blkno = RecordAndGetPageWithFreeSpace(_rel, blkno, cur_size, aligned_size);
        }
        if (!BlockNumberIsValid(blkno)) {
            buffer = get_new_page_locked(blkno);
            if (unlikely(!BlockNumberIsValid(blkno))) {
                ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
                                errmsg("failed to get new page in \"%s\"", RelationGetRelationName(_rel))));
            }
        }
        OffsetNumber offset;
        _log_mgr.apply(buffer, _need_xlog, false, [&](Page page) {
#if PG_VERSION_NUM >= 190000
            offset = PageAddItem(page, data, size, InvalidOffsetNumber, false, false);
#else
            offset = PageAddItem(page, (Item)data, size, InvalidOffsetNumber, false, false);
#endif
            if (unlikely(offset == InvalidOffsetNumber)) {
                ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                                errmsg("Failed to add item to index page in \"%s\"",
                                       RelationGetRelationName(_rel))));
            }
            return true;
        });
        const Size freespace = PageGetFreeSpace(BufferGetPage(buffer));
        RecordPageWithFreeSpace(_rel, blkno, freespace);
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
        bool overwrite_ok = false;
        _log_mgr.apply(buffer, _need_xlog, false, [&](Page page) {
#if PG_VERSION_NUM >= 190000
            overwrite_ok = PageIndexTupleOverwrite(page, offset, data, size);
#else
            overwrite_ok = PageIndexTupleOverwrite(page, offset, (Item)data, size);
#endif
            return overwrite_ok;
        });
        if (overwrite_ok) {
            UnlockReleaseBuffer(buffer);
            return ptr;
        }
        _log_mgr.apply(buffer, _need_xlog, false, [&](Page page) {
            PageIndexTupleDeleteStable(page, offset);
            return true;
        });
        RecordPageWithFreeSpace(_rel, blkno, PageGetFreeSpace(BufferGetPage(buffer)));
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
        bool res;
        _log_mgr.apply(buffer, _need_xlog, false, [&](Page page) {
            ItemId item_id = PageGetItemId(page, offset);
            Assert(ItemIdIsUsed(item_id) && ItemIdHasStorage(item_id));
            res = f(PageGetItem(page, item_id), ItemIdGetLength(item_id));
            return res;
        });
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
        _log_mgr.apply(buffer, _need_xlog, false, [&](Page page) {
            PageIndexTupleDeleteStable(page, offset);
            return true;
        });
        const Size freespace = PageGetFreeSpace(BufferGetPage(buffer));
        RecordPageWithFreeSpace(_rel, blkno, freespace);
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
    bool find_next_valid_key(BlockNumber blkno, OffsetNumber offset, key *out) const
    {
        OffsetNumber start_offset = OffsetNumberIsValid(offset) ? offset : FirstOffsetNumber;
        while (BlockNumberIsValid(blkno)) {
            Buffer buffer = ReadBuffer(_rel, blkno);
            LockBuffer(buffer, BUFFER_LOCK_SHARE);
            Page page = BufferGetPage(buffer);
            OffsetNumber max_offset = PageGetMaxOffsetNumber(page);
            for (OffsetNumber i = start_offset; i <= max_offset; i = OffsetNumberNext(i)) {
                ItemId item_id = PageGetItemId(page, i);
                if (ItemIdIsUsed(item_id) && ItemIdHasStorage(item_id)) {
                    ItemPointerSet(static_cast<ItemPointer>(out), blkno, i);
                    UnlockReleaseBuffer(buffer);
                    return true;
                }
            }
            blkno = ((PlainStoreOpaque)PageGetSpecialPointer(page))->next_blkno;
            start_offset = FirstOffsetNumber;
            UnlockReleaseBuffer(buffer);
        }
        return false;
    }

    Relation _rel;
    BlockNumber _meta_blkno;
    Buffer _meta_buf;
    bool _need_xlog;
    DiskContainerLogMgr _log_mgr;

    Buffer get_new_page_locked(BlockNumber &blkno)
    {
#if defined(PG_VEXDB_TARGET_PG)
        LWLock *extension_lock = vex_graph_build_extension_lock(_rel);
        vex_graph_build_lock_acquire(extension_lock, LW_EXCLUSIVE);
#endif
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
        if (_need_xlog) {
            log_newpage_buffer(buffer, false);
        }
        END_CRIT_SECTION();
        LockBuffer(_meta_buf, BUFFER_LOCK_EXCLUSIVE);
        Page meta_page = BufferGetPage(_meta_buf);
        auto meta = (PlainStoreMetaPage)PageGetContents(meta_page);
        BlockNumber old_last_blkno = meta->last_blkno;
        blkno = BufferGetBlockNumber(buffer);
        bool is_first = (old_last_blkno == _meta_blkno);
        _log_mgr.apply(_meta_buf, _need_xlog, false, [&](Page mp) {
            auto m = (PlainStoreMetaPage)PageGetContents(mp);
            m->last_blkno = blkno;
            if (is_first) {
                auto o = (PlainStoreOpaque)PageGetSpecialPointer(mp);
                o->next_blkno = blkno;
            }
            return true;
        });
        LockBuffer(_meta_buf, BUFFER_LOCK_UNLOCK);
        if (!is_first) {
            Buffer old_last_buffer = ReadBuffer(_rel, old_last_blkno);
            LockBuffer(old_last_buffer, BUFFER_LOCK_EXCLUSIVE);
            _log_mgr.apply(old_last_buffer, _need_xlog, false, [&](Page old_last_page) {
                auto old_last_opaque = (PlainStoreOpaque)PageGetSpecialPointer(old_last_page);
                old_last_opaque->next_blkno = blkno;
                return true;
            });
            UnlockReleaseBuffer(old_last_buffer);
        }
#if defined(PG_VEXDB_TARGET_PG)
        vex_graph_build_lock_release(extension_lock);
#endif
        return buffer;
    }

    void erase(BlockNumber blkno, OffsetNumber *offsets, size_t n)
    {
        Buffer buffer = ReadBuffer(_rel, blkno);
        LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
        _log_mgr.apply(buffer, _need_xlog, true, [&](Page page) {
            for (size_t i = 0; i < n; ++i) {
                PageIndexTupleDeleteStable(page, offsets[i]);
            }
            return true;
        });
        const Size freespace = PageGetFreeSpace(BufferGetPage(buffer));
        RecordPageWithFreeSpace(_rel, blkno, freespace);
        UnlockReleaseBuffer(buffer);
    }
};
} /* namespace disk_container */

#endif /* CONTAINER_PLAIN_STORE_H */
