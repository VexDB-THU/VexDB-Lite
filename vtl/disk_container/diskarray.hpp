/**
 * Copyright ...
 */

#ifndef CONTAINER_DISKARRAY_H
#define CONTAINER_DISKARRAY_H

#include <vtl/internal/container.hpp>
#include <vtl/disk_container/blockmgr.hpp>
#include <vtl/disk_container/buffer_cache.hpp>
#include <vtl/internal/expr.hpp>
#include <vtl/disk_container/macro.hpp>

#include "c.h"
#include "storage/indexfsm.h"
#include "access/xloginsert.h"

namespace disk_container {
constexpr uint32 DISK_ARRAY_VERSION_ONE = 1u;

struct DiskArrayOpaqueData {
    uint16 type_id;
    uint16 page_id;

    /* use ann_helper::GET_TYPE_ID as input */
    void init(uint16 type_id)
    {
        page_id = DISK_ARRAT_DATA_ID;
    }
};
typedef DiskArrayOpaqueData *DiskArrayOpaque;

template <typename T>
struct DiskArrayDataPageData {
    uint32 magic;
    uint32 version;
    T data[FLEXIBLE_ARRAY_MEMBER];

    constexpr static const size_t AvailableSize =
        PAGE_SIZE - offsetof(DiskArrayDataPageData, data) - MAXALIGN(sizeof(DiskArrayOpaqueData));
    constexpr static const size_t MaxNelem = AvailableSize / sizeof(T);

    void init() { magic = DISK_ARRAY_DATA_MAGIC; version = DISK_ARRAY_VERSION_ONE; }

    const T &operator[](uint32 idx) const { return data[idx]; }
    T &operator[](uint32 idx) { return data[idx]; }
};
template <typename T>
using DiskArrayDataPage = DiskArrayDataPageData<T> *;
template <typename T>
constexpr size_t DiskArrayDataPageAvailableSize =
    PAGE_SIZE - offsetof(DiskArrayDataPageData<T>, data) - MAXALIGN(sizeof(DiskArrayOpaqueData));
template <typename T>
constexpr size_t DiskArrayDataPageMaxNelem = DiskArrayDataPageAvailableSize<T> / sizeof(T);

template <typename T, size_t N>
class DiskArray {
    static constexpr size_t elem_per_page = DiskArrayDataPageMaxNelem<T>;
public:
    using page_type_ptr = DiskArrayDataPage<T>;
    static page_type_ptr get_da_page(char *page)
        { return (page_type_ptr)(page + MAXALIGN(SizeOfPageHeaderData)); }
    static constexpr size_t size() { return N; }
    static constexpr size_t nblock() { return (N + elem_per_page - 1ul) / elem_per_page; }
    static constexpr size_t wasted_size() { return nblock() * elem_per_page - N; }
    static BlockNumber get_disk_array(Relation rel, bool need_wal, uint16 type_id,
        ForkNumber fork_num, const T &t = T())
    {
        const auto init_page = [&t](page_type_ptr page) {
            page->init();
            for (size_t i = 0; i < elem_per_page; ++i) {
                new (&(*page)[i]) T(t);
            }
        };

        if (nblock() == 1ul) {
            BlockNumber res = fork_num == MAIN_FORKNUM ? GetFreeIndexPage(rel) : InvalidBlockNumber;
            if (BlockNumberIsValid(res)) {
                Buffer buf = ReadBuffer(rel, res);
                Page page = BufferGetPage(buf);
                PageInit(page, BLCKSZ, sizeof(DiskArrayOpaqueData));
                ((PageHeader)page)->pd_lower = ((PageHeader)page)->pd_upper;
                ((DiskArrayOpaque)PageGetSpecialPointer(page))->init(type_id);
                init_page(get_da_page(page));
                MarkBufferDirty(buf);
                if (need_wal) {
                    Bm25XLogInitPage(buf, page);
                }
                ReleaseBuffer(buf);
                return res;
            }
        }
        DiskArrayOpaqueData opaque;
        opaque.init(type_id);
        page_type_ptr page = (page_type_ptr)palloc(BLCKSZ);
        init_page(page);
        BlockMgr mgr(rel);
        BlockNumber res = mgr.reserve_new_pages(nblock(), (const char *)page, (const char *)&opaque,
                                                sizeof(DiskArrayOpaqueData), fork_num);
        mgr.destroy();
        if (need_wal) {
            log_newpage_range(rel, fork_num, res, res + nblock(), true, RM_BM25_ID,
                              XLOG_BM25_APPEND_PAGES);
        }
        pfree(page);
        return res;
    }

    static void get_disk_array(Relation rel, BlockNumber blkno, bool need_wal, uint16 type_id,
                               const T &t = T())
    {
        for (size_t i = 0; i < nblock(); ++i) {
            Buffer buf = ReadBuffer(rel, blkno + i);
            Page page = BufferGetPage(buf);
            PageInit(page, BLCKSZ, sizeof(DiskArrayOpaqueData));
            ((PageHeader)page)->pd_lower = ((PageHeader)page)->pd_upper;
            ((DiskArrayOpaque)PageGetSpecialPointer(page))->init(type_id);
            page_type_ptr arr_page = get_da_page(page);
            arr_page->init();
            for (size_t i = 0; i < elem_per_page; ++i) {
                new (&(*arr_page)[i]) T(t);
            }
            MarkBufferDirty(buf);
            if (need_wal) {
                Bm25XLogInitPage(buf, page);
            }
            ReleaseBuffer(buf);
        }
    }
    DiskArray(Relation rel, BlockNumber start_blkno, bool need_wal)
        : _rel(rel), _start_blkno(start_blkno), _need_wal(need_wal) {}
    void destroy() { _buf_cache.destroy(); }

    void get(size_t idx, T &elem)
    {
        size_t offset;
        load_buffer(get_blkno_offset(idx, offset));
        page_type_ptr page = get_page();
        lock_buffer<false>();
        elem = (*page)[offset];
        unlock_buffer();
    }

    T *get_nolock(size_t idx)
    {
        size_t offset;
        load_buffer(get_blkno_offset(idx, offset));
        page_type_ptr page = get_page();
        return &(*page)[offset];
    }

    T *get_nolock(size_t idx, BufferCache &cache, size_t &offset)
    {
        cache.load_buffer(_rel, get_blkno_offset(idx, offset));
        page_type_ptr page = (page_type_ptr)cache.get_page();
        return &(*page)[offset];
    }

    void set(size_t idx, const T &elem)
    {
        size_t offset;
        load_buffer(get_blkno_offset(idx, offset));
        page_type_ptr page = get_page();
        lock_buffer<true>();
        (*page)[offset] = elem;
        mark_dirty();
        if (need_wal()) {
            xl_bm25_add_data xl_rec;
            xl_rec.data_size = sizeof(T);
            xl_rec.offset = MAXALIGN(SizeOfPageHeaderData) +
                            offsetof(DiskArrayDataPageData<T>, data) + offset * sizeof(T);
            Bm25XLogAddData(_buf_cache.get_buffer(), _buf_cache.get_row_page(), xl_rec);
        }
        unlock_buffer();
    }
    template <typename F> bool set(size_t idx, F &&f)
    {
        static_assert(IS_INVOCABLE_R(F, bool, T &), "F must be invocable with T & and return bool");
        size_t offset;
        load_buffer(get_blkno_offset(idx, offset));
        page_type_ptr page = get_page();
        lock_buffer<true>();
        const bool res = std::forward<F>(f)((*page)[offset]);
        if (res) {
            mark_dirty();
            if (need_wal()) {
                xl_bm25_add_data xl_rec;
                xl_rec.data_size = sizeof(T);
                xl_rec.offset = MAXALIGN(SizeOfPageHeaderData) +
                                offsetof(DiskArrayDataPageData<T>, data) + offset * sizeof(T);
                Bm25XLogAddData(_buf_cache.get_buffer(), _buf_cache.get_row_page(), xl_rec);
            }
        }
        unlock_buffer();
        return res;
    }

    page_type_ptr get_page() const { return (page_type_ptr)_buf_cache.get_page(); }
    Page get_row_page() const { return _buf_cache.get_row_page(); }
    template <bool exclusive> void lock_buffer() { _buf_cache.template lock_buffer<exclusive>(); }
    void mark_dirty() { _buf_cache.mark_dirty(); }
    void unlock_buffer() { _buf_cache.unlock_buffer(); }

    bool need_wal() const { return _need_wal; }
    Relation get_relation() { return _rel; }
    BufferCache &get_buffer_cache() { return _buf_cache; }

private:
    Relation _rel;
    BlockNumber _start_blkno;
    const bool _need_wal;
    BufferCache _buf_cache{};

    BlockNumber get_blkno_offset(size_t idx, size_t &offset)
    {
        size_t page_offset = idx / elem_per_page;
        offset = idx - (page_offset * elem_per_page);
        return _start_blkno + page_offset;
    }

    void load_buffer(BlockNumber blkno) { _buf_cache.load_buffer(_rel, blkno); }
};

template <typename T, size_t NBlock>
using FullDiskArray = DiskArray<T, DiskArrayDataPageMaxNelem<T> * NBlock>;
} /* namespace disk_container */

#endif /* CONTAINER_DISKARRAY_H */
