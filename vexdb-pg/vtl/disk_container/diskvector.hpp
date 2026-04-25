/**
 * Copyright ...
 */

#ifndef CONTAINER_DISK_VECTOR_H
#define CONTAINER_DISK_VECTOR_H

#include <type_traits>
#include <algorithm>
#include <atomic>

#include <vtl/internal/container.hpp>
#include <vtl/disk_container/blockmgr.hpp>
#include <vtl/internal/type.hpp>
#include <vtl/internal/expr.hpp>
#include <vtl/disk_container/macro.hpp>
#include <vtl/pair>
#include <vtl/disk_container/disk_container_xlog.hpp>

#include "utils/relcache.h"
#include "utils/palloc.h"
#include "access/heapam.h"

/* XLog codes */
#define XLOG_DISKANN_SET_ELEM 0xB0
#define XLOG_DISKANN_EXTEND_NEWPAGES 0xB1
#define XLOG_DISKANN_UPDATE_META_START_NPAGES 0xB2
#define XLOG_DISKANN_UPDATE_META_NITEM 0xB3

#define XLOG_DISKVECTOR_SET_ELEM                    XLOG_DISKANN_SET_ELEM
#define XLOG_DISKVECTOR_EXTEND_NEWPAGES             XLOG_DISKANN_EXTEND_NEWPAGES
#define XLOG_DISKVECTOR_UPDATE_META_START_NPAGES    XLOG_DISKANN_UPDATE_META_START_NPAGES
#define XLOG_DISKVECTOR_UPDATE_META_NITEM           XLOG_DISKANN_UPDATE_META_NITEM
#define XLOG_DISKVECTOR_INPLACE_FILTER_ADD_DATA     XLOG_DISKANN_INPLACE_FILTER_ADD_DATA
#define XLOG_DISKVECTOR_INPLACE_FILTER_ADD_ITEM     XLOG_DISKANN_INPLACE_FILTER_ADD_ITEM
#define XLOG_DISKVECTOR_INPLACE_FILTER_DELETE_ITEM  XLOG_DISKANN_INPLACE_FILTER_DELETE_ITEM
#define XLOG_DISKVECTOR_INPLACE_FILTER_MULTI_DELETE XLOG_DISKANN_INPLACE_FILTER_MULTI_DELETE
#define XLOG_DISKVECTOR_SET_ELEM_PARTIAL            0xC0


namespace disk_container {
constexpr size_t DEFAULT_START_IDX = 8ul;
constexpr int DEFAULT_START_CLZ_IDX = __builtin_clzl(DEFAULT_START_IDX) + 1;
constexpr uint32 DISK_VECTOR_VERSION_ONE = 1u;
constexpr uint32 DISK_VECTOR_VERSION_TWO = 2u;

struct DiskVectorOpaqueData {
    uint16 type_id;
    uint16 page_id;
};
typedef DiskVectorOpaqueData *DiskVectorOpaque;

struct DiskVectorMetaPageData {
    uint32 magic;
    uint32 version;
    BlockNumber lock_page_for_base;  /* reserved */
    size_t nitem;
    uint32 npage;
    BlockNumber item_start_pages[FLEXIBLE_ARRAY_MEMBER];

    void init(BlockNumber start_blkno)
    {
        magic = DISK_VECTOR_META_MAGIC;
        version = DISK_VECTOR_VERSION_ONE;
        nitem = 0;
        npage = 1u;
        item_start_pages[0] = start_blkno;
    }
};
typedef DiskVectorMetaPageData *DiskVectorMetaPage;

namespace vtl {
template <typename T>
struct DiskVectorDataPageData {
    uint32 magic;
    uint32 version;
    T data[FLEXIBLE_ARRAY_MEMBER];

    constexpr static size_t AvailableSize = PAGE_SIZE - 8 - MAXALIGN(sizeof(DiskVectorOpaqueData));

    void init() { magic = DISK_VECTOR_DATA_MAGIC; version = DISK_VECTOR_VERSION_ONE; }

    const T *get(uint32 idx) const { return data + idx; }
    T *get(uint32 idx) { return data + idx; }
};
template <typename T> using DiskVectorDataPage = DiskVectorDataPageData<T> *;

template <typename T>
struct VarDiskVectorDataPageData {
    uint32 magic;
    uint32 version;
    char data[FLEXIBLE_ARRAY_MEMBER];

    constexpr static size_t AvailableSize = PAGE_SIZE - 8 - MAXALIGN(sizeof(DiskVectorOpaqueData));

    void init() { magic = DISK_VECTOR_DATA_MAGIC; version = DISK_VECTOR_VERSION_TWO; }

    const T *get(uint32 idx, size_t it) const { return (const T *)(data + idx * it); }
    T *get(uint32 idx, size_t it) { return (T *)(data + idx * it); }
};

template <typename T>
struct FixedParam {
    using dpd = DiskVectorDataPageData<T>;
    using dp = DiskVectorDataPageData<T> *;

    explicit FixedParam(size_t) {}

    static constexpr bool is_var_length = false;
    static constexpr size_t data_size() { return sizeof(T); }
    static constexpr size_t n_data_per_block() { return dpd::AvailableSize / sizeof(T); }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    static constexpr size_t _get_offset(size_t i)
        { return MAXALIGN(SizeOfPageHeaderData) + offsetof(dpd, data) + i * data_size(); }
#pragma GCC diagnostic pop
    
    template <typename P>
    static auto _get(P &&p, uint32 idx) -> decltype(std::forward<P>(p)->get(idx))
        { return std::forward<P>(p)->get(idx); }

    static const T *_offset_by(const T *t, size_t i) { return t + i; }
    static T *_offset_by(T *t, size_t i) { return t + i; }
};

template <typename T>
struct VarParam {
    using dpd = VarDiskVectorDataPageData<T>;
    using dp = VarDiskVectorDataPageData<T> *;

    explicit VarParam(size_t s) : vl(s) {}

    size_t vl;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    constexpr size_t _get_offset(size_t i) const
        { return MAXALIGN(SizeOfPageHeaderData) + offsetof(dpd, data) + i * vl; }
#pragma GCC diagnostic pop
    constexpr size_t n_data_per_block() const { return dpd::AvailableSize / vl; }
    constexpr size_t data_size() const { return vl; }
    static constexpr bool is_var_length = true;

    template <typename P>
    auto _get(P &&p, uint32 idx) const -> decltype(std::forward<P>(p)->get(idx, vl))
        { return std::forward<P>(p)->get(idx, vl); }

    const T *_offset_by(const T *t, size_t i) const { return (const T *)(((const char *)t) + i * vl); }
    T *_offset_by(T *t, size_t i) const { return (T *)(((char *)t) + i * vl); }

};

/**
 * Persistent vector, thread safe for get/set, but in actual use the content must have version to achieve parallelism
 */
template <typename T, typename Param>
class DiskVector : public Param {
    using dp = typename Param::dp;
    using dpd = typename Param::dpd;
    static_assert(sizeof(T) <= dpd::AvailableSize, "data type too large for disk vector");

public:
    Relation _rel;
    DiskVectorMetaPage _meta;

    DiskVector(Relation rel, BlockNumber meta_blkno, bool is_wal, size_t elem_size = sizeof(T))
        : Param(elem_size),
          _rel(rel),
          _blkmgr(rel),
          dv_log(_rel),
          _need_wal(is_wal && RelationNeedsWAL(rel))
    {
        static_assert(std::is_standard_layout<T>::value, "Diskvector only applies to POD types");
        get_meta_page(meta_blkno);
        _lock_pd = _blkmgr.get_page_data(_meta->lock_page_for_base);
    }

    void destroy()
    {
        _blkmgr.release_page(_meta_pd);
        _blkmgr.release_page(_lock_pd);
        _blkmgr.destroy();
    }
    static BlockNumber get_disk_vector(Relation rel, bool is_wal, ForkNumber fork_num = MAIN_FORKNUM)
    {
        Assert(!is_wal || fork_num == MAIN_FORKNUM);
        char data_buf[PAGE_SIZE] = {0};
        // DiskVectorOpaqueData opaque_data = {ann_helper::GET_TYPE_ID(T), DISK_VECTOR_DATA_ID}; TD
        DiskVectorOpaqueData opaque_data = {0, DISK_VECTOR_DATA_ID};
        BlockMgr blkmgr(rel);
        DiskVectorLogger dv_logger(rel);
        reinterpret_cast<dp>(data_buf)->init();
        BlockNumber blkno = blkmgr.reserve_new_pages(DEFAULT_START_IDX, data_buf, &opaque_data,
                                                     sizeof(opaque_data), fork_num);
        if (is_wal) {
            dv_logger.xl_extend_newpages(blkno, blkno + DEFAULT_START_IDX);
        }
        auto meta = reinterpret_cast<DiskVectorMetaPage>(data_buf);
        meta->init(blkno);
        opaque_data.page_id = DISK_VECTOR_DUMMY_ID;
        blkno = blkmgr.reserve_new_pages(1ul, NULL, &opaque_data, sizeof(opaque_data), fork_num);
        if (is_wal) {
            dv_logger.xl_extend_newpages(blkno, blkno + 1u);
        }
        meta->lock_page_for_base = blkno;
        opaque_data.page_id = DISK_VECTOR_META_ID;
        blkno = blkmgr.reserve_new_pages(1ul, (char *)meta, &opaque_data, sizeof(opaque_data), fork_num);
        if (is_wal) {
            dv_logger.xl_extend_newpages(blkno, blkno + 1u);
        }
        blkmgr.destroy();
        return blkno;
    }

    template <AccessorLockType lock_type> T get(size_t idx) const
    {
        PageData pd;
        uint32 offset;
        navigate_page_offset(idx, pd, offset);
        _blkmgr.template lock_page_data_custom<lock_type>(pd);
        T res = *this->_get(reinterpret_cast<const dp>(pd.page), offset);
        _blkmgr.template unlock_page_data_custom<lock_type>(pd);
        _blkmgr.release_page(pd);
        return res;
    }
    template <AccessorLockType lock_type> void get_n(size_t idx, size_t n, T *dest) const
    {
        Assert(dest && n > 0);
        PageData pd;
        uint32 offset;
        uint32 read_amount;
        for (size_t i = 0; i < n; i += read_amount) {
            CHECK_FOR_INTERRUPTS();
            navigate_page_offset(idx + i, pd, offset);
            read_amount = std::min(n - i, this->n_data_per_block() - offset);
            Assert(read_amount > 0);
            _blkmgr.template lock_page_data_custom<lock_type>(pd);
            memcpy(this->_offset_by(dest, i), this->_get(reinterpret_cast<dp>(pd.page), offset),
                   read_amount * this->data_size());
            _blkmgr.template unlock_page_data_custom<lock_type>(pd);
            _blkmgr.release_page(pd);
        }
    }

    void rlock() { _blkmgr.lock_page_data_shared(_lock_pd); }
    void runlock() { _blkmgr.unlock_page_data(_lock_pd); }
    void wlock() { _blkmgr.lock_page_data_exclusive(_lock_pd); }
    void wunlock() { _blkmgr.unlock_page_data(_lock_pd); }

    template <AccessorLockType lock_type> void set(size_t idx, const T &elem)
    {
        PageData pd;
        uint32 offset;
        navigate_page_offset(idx, pd, offset);
        _blkmgr.template lock_page_data_custom<lock_type>(pd);
        *this->_get(reinterpret_cast<dp>(pd.page), offset) = elem;
        pd.mark_dirty();
        if (_need_wal) {
            dv_log.xl_add_elem(pd, (char *)&elem, this->data_size(),
                                          this->_get_offset(offset));
        }
        _blkmgr.template unlock_page_data_custom<lock_type>(pd);
        _blkmgr.release_page(pd);
    }
    template <AccessorLockType lock_type> void set_n(size_t idx, size_t n, const T *elem)
    {
        Assert(elem || n == 0);
        PageData pd;
        uint32 offset;
        uint32 write_amount;
        for (size_t i = 0; i < n; i += write_amount) {
            CHECK_FOR_INTERRUPTS();
            navigate_page_offset(idx + i, pd, offset);
            write_amount = std::min(n - i, this->n_data_per_block() - offset);
            Assert(write_amount > 0);
            _blkmgr.template lock_page_data_custom<lock_type>(pd);
            memcpy(this->_get(reinterpret_cast<dp>(pd.page), offset),
                this->_offset_by(elem, i),
                write_amount * this->data_size());
            pd.mark_dirty();
            if (_need_wal) {
                dv_log.xl_add_elem(pd, (char*)this->_offset_by(elem, offset),
                    write_amount * this->data_size(), this->_get_offset(offset));
            }
            _blkmgr.template unlock_page_data_custom<lock_type>(pd);
            _blkmgr.release_page(pd);
        }
    }
    template <AccessorLockType lock_type> void set_n(size_t idx, size_t n, const T &elem)
    {
        static_assert(!Param::is_var_length, "This function can only be called by DiskVector");
        PageData pd;
        uint32 offset;
        uint32 write_amount;
        for (size_t i = 0; i < n; i += write_amount) {
            CHECK_FOR_INTERRUPTS();
            navigate_page_offset(idx + i, pd, offset);
            write_amount = std::min(n - i, this->n_data_per_block() - offset);
            Assert(write_amount > 0);
            _blkmgr.template lock_page_data_custom<lock_type>(pd);
            T *dest = this->_get(reinterpret_cast<dp>(pd.page), offset);
            std::fill_n(dest, write_amount, elem);
            pd.mark_dirty();
            if (_need_wal) {
                dv_log.xl_add_elem(pd, (char *)dest, write_amount * this->data_size(), this->_get_offset(offset));
            }
            _blkmgr.template unlock_page_data_custom<lock_type>(pd);
            _blkmgr.release_page(pd);
        }
    }
    template <AccessorLockType lock_type, class F> struct Applier {
        static constexpr bool reference_input = IS_INVOCABLE_R(F, bool, T &);
        using PartialPair = Pair<char *, size_t>;
        static constexpr bool partial_return = IS_INVOCABLE_R(F, PartialPair, T *);
        Applier(DiskVector &vector, uint16 partial_size, F &&func)
            : _vector(vector), _partial_size(partial_size), _apply_func(std::forward<F>(func))
        {
            static_assert(IS_INVOCABLE_R(F, bool, T &) || IS_INVOCABLE_R(F, bool, T *) ||
                          IS_INVOCABLE_R(F, void, T *, size_t, bool *) ||
                          (partial_return && __cplusplus >= 201703L),
                          "F must be invocable with T &/T * returning bool, (T *, size_t, bool *) returning void, "
                          "or T * returning Pair<char*, size_t>");
        }

        bool operator()(size_t idx)
        {
            CONSTEXPR_IF (IS_INVOCABLE_R(F, bool, T &) || IS_INVOCABLE_R(F, bool, T *)) {
                PageData pd;
                uint32 offset;
                _vector.navigate_page_offset(idx, pd, offset);
                _vector._blkmgr.template lock_page_data_custom<lock_type>(pd);
                auto data = reinterpret_cast<dp>(pd.page);
                bool res;
                CONSTEXPR_IF (reference_input) {
                    res = _apply_func(*_vector._get(data, offset));
                } else {
#if __cplusplus >= 201703L
                    res = _apply_func(_vector._get(data, offset));
#else
                    Assume(false); /* cannot compile under c++14 or less */
#endif
                }
                if (res) {
                    pd.mark_dirty();
                    if (_vector._need_wal) {
                        _vector.dv_log.xl_add_elem(pd, (char *)_vector._get(data, offset),
                            _vector.data_size(), _vector._get_offset(offset));
                    }
                }
                _vector._blkmgr.template unlock_page_data_custom<lock_type>(pd);
                _vector._blkmgr.release_page(pd);
                return res;
            }
#if __cplusplus >= 201703L
            else CONSTEXPR_IF (partial_return) {
                PageData pd;
                uint32 offset;
                _vector.navigate_page_offset(idx, pd, offset);
                _vector._blkmgr.template lock_page_data_custom<lock_type>(pd);
                auto data = reinterpret_cast<dp>(pd.page);
                auto [partial_data, partial_len] = _apply_func(_vector._get(data, offset));
                if (partial_data != NULL) {
                    pd.mark_dirty();
                    if (_vector._need_wal) {
                        _vector.dv_log.xl_set_elem_partial(pd, partial_data, partial_len,
                            partial_data - (char *)data);
                    }
                }
                _vector._blkmgr.template unlock_page_data_custom<lock_type>(pd);
                _vector._blkmgr.release_page(pd);
                return partial_data != nullptr;
            }
#endif
            else {
                Assume(false);
            }
        }

#if __cplusplus >= 201703L
        void operator()(size_t idx, size_t n)
        {
            CONSTEXPR_IF (IS_INVOCABLE(F, T *, size_t, bool *)) {
                PageData pd;
                uint32 offset;
                uint32 batch_size;
                for (; idx < n; idx += batch_size) {
                    CHECK_FOR_INTERRUPTS();
                    _vector.navigate_page_offset(idx, pd, offset);
                    batch_size = std::min(n - idx, _vector.n_data_per_block() - offset);
                    Assert(batch_size > 0);
                    _vector._blkmgr.template lock_page_data_custom<lock_type>(pd);
                    T *data = _vector._get(reinterpret_cast<dp>(pd.page), offset);
                    bool dirty[256] = {};
                    Assert(batch_size <= 256);
                    _apply_func(data, batch_size, dirty);
                    bool has_dirty = false;
                    for (size_t i = 0; i < batch_size; ++i) {
                        if (dirty[i]) {
                            has_dirty = true; 
                            break;
                        }
                    }
                    if (has_dirty) {
                        pd.mark_dirty();
                        if (_vector._need_wal) {
                            for (size_t i = 0; i < batch_size; ++i) {
                                if (!dirty[i]) {
                                    continue;
                                }    
                                char *elem_ptr = (char *)_vector._offset_by(data, i);
                                uint32 elem_offset = _vector._get_offset(offset + i);
                                if (_partial_size > 0) {
                                    _vector.dv_log.xl_set_elem_partial(pd, elem_ptr, _partial_size, elem_offset);
                                } else {
                                    _vector.dv_log.xl_add_elem(pd, elem_ptr, _vector.data_size(), elem_offset);
                                }     
                            }
                        }
                    }
                    _vector._blkmgr.template unlock_page_data_custom<lock_type>(pd);
                    _vector._blkmgr.release_page(pd);
                }
            } else {
                Assume(false);
            }
        }
#endif

        friend class DiskVector;
    private:
        DiskVector &_vector;
        uint16 _partial_size;
        F _apply_func;
    };
    template <AccessorLockType lock_type, class F> Applier<lock_type, F> apply(F &&func, uint16 partial_size = 0)
        { return Applier<lock_type, F>(*this, partial_size, std::forward<F>(func)); }


    template <AccessorLockType lock_type, class F> struct Visitor {
        Visitor(DiskVector &vector, F &&func) : _vector(vector), _visit_func(std::forward<F>(func))
        {
            static_assert(IS_INVOCABLE_R(F, void, const T *, size_t) ||
                          IS_INVOCABLE(F, const T *) || IS_INVOCABLE(F, const T &),
                          "F must be invocable with (const T */const T &[, size_t])");
        }

        void operator()(size_t idx, size_t n)
        {
            CONSTEXPR_IF (IS_INVOCABLE_R(F, void, const T *, size_t)) {
                PageData pd;
                uint32 offset;
                uint32 read_amount;
                for (size_t i = 0; i < n; i += read_amount) {
                    CHECK_FOR_INTERRUPTS();
                    _vector.navigate_page_offset(idx + i, pd, offset);
                    read_amount = std::min(n - i, _vector.n_data_per_block() - offset);
                    Assert(read_amount > 0);
                    _vector._blkmgr.template lock_page_data_custom<lock_type>(pd);
                    _visit_func(_vector._get(reinterpret_cast<const dp>(pd.page), offset), read_amount);
                    _vector._blkmgr.template unlock_page_data_custom<lock_type>(pd);
                    _vector._blkmgr.release_page(pd);
                }
            } else {
                Assume(false);
            }
        }

#if __cplusplus >= 201703L
        auto operator()(size_t idx)
        {
            CONSTEXPR_IF (IS_INVOCABLE(F, const T *) || IS_INVOCABLE(F, const T &)) {
                PageData pd;
                uint32 offset;
                _vector.navigate_page_offset(idx, pd, offset);
                _vector._blkmgr.template lock_page_data_custom<lock_type>(pd);
                CONSTEXPR_IF (IS_INVOCABLE(F, const T *)) {
                    CONSTEXPR_IF (std::is_void<RESULT_OF(F, const T *)>::value) {
                        _visit_func(_vector._get(reinterpret_cast<const dp>(pd.page), offset));
                        _vector._blkmgr.template unlock_page_data_custom<lock_type>(pd);
                        _vector._blkmgr.release_page(pd);
                    } else {
                        auto res = _visit_func(_vector._get(reinterpret_cast<const dp>(pd.page), offset));
                        _vector._blkmgr.template unlock_page_data_custom<lock_type>(pd);
                        _vector._blkmgr.release_page(pd);
                        return res;
                    }
                } else {
                    CONSTEXPR_IF (std::is_void<RESULT_OF(F, const T &)>::value) {
                        _visit_func(*_vector._get(reinterpret_cast<const dp>(pd.page), offset));
                        _vector._blkmgr.template unlock_page_data_custom<lock_type>(pd);
                        _vector._blkmgr.release_page(pd);
                    } else {
                        auto res = _visit_func(*_vector._get(reinterpret_cast<const dp>(pd.page), offset));
                        _vector._blkmgr.template unlock_page_data_custom<lock_type>(pd);
                        _vector._blkmgr.release_page(pd);
                        return res;
                    }
                }
            } else {
                Assume(false);
            }
        }
#endif

    private:
        DiskVector &_vector;
        F _visit_func;
    };
    template <AccessorLockType lock_type, class F> Visitor<lock_type, F> visit(F &&func)
        { return Visitor<lock_type, F>(*this, std::forward<F>(func)); }

    size_t push_back(const T &elem)
    {
        _blkmgr.lock_page_data_exclusive(_meta_pd);
        size_t cur_size = _meta->nitem; 
        ++_meta->nitem;
        _meta_pd.mark_dirty();
        if (_need_wal) {
            dv_log.xl_update_meta_nitem(_meta_pd, cur_size + 1);
        }
        _blkmgr.unlock_page_data(_meta_pd);
        reserve(cur_size + 1);
        set<AccessorLockType::WriteLock>(cur_size, elem);
        return cur_size;
    }
    size_t push_back_n(const T &elem, size_t n)
    {
        Assert(n > 0);
        _blkmgr.lock_page_data_exclusive(_meta_pd);
        size_t cur_size = _meta->nitem;
        _meta->nitem += n;
        _meta_pd.mark_dirty();
        if (_need_wal) {
            dv_log.xl_update_meta_nitem(_meta_pd, cur_size + n);
        }
        _blkmgr.unlock_page_data(_meta_pd);
        reserve(cur_size + n);
        set_n<AccessorLockType::WriteLock>(cur_size, n, elem);
        return cur_size;
    }
    size_t push_back_n(const T *elem, size_t n)
    {
        Assert(elem && n > 0);
        _blkmgr.lock_page_data_exclusive(_meta_pd);
        size_t cur_size = _meta->nitem;
        _meta->nitem += n;
        _meta_pd.mark_dirty();
        if (_need_wal) {
            dv_log.xl_update_meta_nitem(_meta_pd, cur_size + n);
        }
        _blkmgr.unlock_page_data(_meta_pd);
        reserve(cur_size + n);
        set_n<AccessorLockType::WriteLock>(cur_size, n, elem);
        return cur_size;
    }
    template <AccessorLockType lock_type> bool pop_back(T &elem)
    {
        _blkmgr.lock_page_data_exclusive(_meta_pd);
        size_t cur_size = _meta->nitem;
        if (cur_size == 0) {
            _blkmgr.unlock_page_data(_meta_pd);
            return false;
        }
        _meta->nitem = cur_size - 1ul;
        _meta_pd.mark_dirty();
        if (_need_wal) {
            dv_log.xl_update_meta_nitem(_meta_pd, cur_size - 1);
        }
        _blkmgr.unlock_page_data(_meta_pd);

        PageData pd;
        uint32 offset;
        navigate_page_offset(cur_size - 1, pd, offset);
        _blkmgr.template lock_page_data_custom<lock_type>(pd);
        elem = *this->_get(reinterpret_cast<const dp>(pd.page), offset);
        _blkmgr.template unlock_page_data_custom<lock_type>(pd);
        _blkmgr.release_page(pd);
        return true;
    }

    void reserve(size_t size)
    {
        BlockNumber blkno;
        uint32 offset;
        if (navigate_blkno_offset(size, blkno, offset)) {
            return;
        }
        _blkmgr.lock_page_data_exclusive(_meta_pd);
        if (navigate_blkno_offset(size, blkno, offset)) {
            _blkmgr.unlock_page_data(_meta_pd);
            return;
        }

        char data_buf[PAGE_SIZE] = {0};
        reinterpret_cast<dp>(data_buf)->init();
        // DiskVectorOpaqueData opaque_data = {ann_helper::GET_TYPE_ID(T), DISK_VECTOR_DATA_ID};
        DiskVectorOpaqueData opaque_data = {0, DISK_VECTOR_DATA_ID};
        do {
            size_t n_new_page = DEFAULT_START_IDX << (_meta->npage - 1);
            /* extend vector */
            BlockNumber start_blkno =
                _blkmgr.reserve_new_pages(n_new_page, data_buf, &opaque_data, sizeof(opaque_data));
            if (_need_wal) {
                dv_log.xl_extend_newpages(start_blkno, start_blkno + n_new_page);
            }
            _meta->item_start_pages[_meta->npage] = start_blkno;
            ++_meta->npage;
            if (_need_wal) {
                _meta_pd.mark_dirty();
                dv_log.xl_update_meta_start_npages(_meta_pd, _meta->npage, start_blkno);
            }
        } while (!navigate_blkno_offset(size, blkno, offset));
        _meta_pd.mark_dirty();
        _blkmgr.unlock_page_data(_meta_pd);
    }
    void extend(size_t size)
    {
        _blkmgr.lock_page_data_exclusive(_meta_pd);
        size_t nitem_size = _meta->nitem;
        if (size > nitem_size) {
            _meta->nitem = size;
            _meta_pd.mark_dirty();
            if (_need_wal) {
                dv_log.xl_update_meta_nitem(_meta_pd, size);
            }
        }
        _blkmgr.unlock_page_data(_meta_pd);
        if (size > nitem_size) {
            reserve(size);
        }
    }
    size_t append()
    {
        _blkmgr.lock_page_data_exclusive(_meta_pd);
        size_t size = _meta->nitem++;
        _meta_pd.mark_dirty();
        if (_need_wal) {
            dv_log.xl_update_meta_nitem(_meta_pd, _meta->nitem);
        }
        _blkmgr.unlock_page_data(_meta_pd);
        reserve(size + 1);
        return size;
    }
    /* only set size, no space released */
    void shrink(size_t size) { _meta->nitem = size; }
    size_t size() const { return _meta->nitem; }
    BlockNumber get_nblocks() const
    {
        BlockNumber res = 9u;   /* meta page + first 8 pages */
        for (uint32 i = 1; i < _meta->npage; ++i) {
            res += (DEFAULT_START_IDX << (i - 1));
        }
        return res;
    }
    size_t capacity() const { return size_t(get_nblocks()) * this->n_data_per_block(); }
    void prefetch(size_t start, size_t len) { /* no-op */ }

private:
    PageData _meta_pd;
    PageData _lock_pd;
    BlockMgr _blkmgr;
    DiskVectorLogger dv_log;
    bool _need_wal;

    void get_meta_page(BlockNumber meta_blkno)
    {
        _meta_pd = _blkmgr.get_page_data(meta_blkno);
        _meta = reinterpret_cast<DiskVectorMetaPage>(_meta_pd.page);
    }

    bool navigate_blkno_offset(size_t idx, BlockNumber &blkno, uint32 &offset) const
    {
        constexpr size_t SIZE_T_BYTE = sizeof(size_t) * CHAR_BIT;
        size_t page_idx = idx / this->n_data_per_block();
        /* __builtin_clzl UB when input is zero */
        int idx_clz =  idx < this->n_data_per_block() ? SIZE_T_BYTE : __builtin_clzl(page_idx);
        int target_page = DEFAULT_START_CLZ_IDX - idx_clz;
        uint32 target_page_group_idx = target_page > 0 ? uint32(target_page) : 0;
        if (unlikely(target_page_group_idx >= _meta->npage)) {
            return false;
        }
        /* no need to worry about idx_clz causing overflow, that's too large to happen */
        const size_t mask = ~(1ul << (SIZE_T_BYTE - 1 - idx_clz)) | 7ul;
        blkno = _meta->item_start_pages[target_page_group_idx] + (page_idx & mask);
        offset = idx % this->n_data_per_block();
        return true;
    }
    
    void navigate_page_offset(size_t idx, PageData &pd, uint32 &offset) const
    {
        BlockNumber blkno;
        if (navigate_blkno_offset(idx, blkno, offset)) {
            pd = _blkmgr.get_page_data(blkno);
        } else {
#if VERIFY_DATA
            ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION), errmsg("Diskvector invalid access idx: %lu", idx)));
#else
            __builtin_unreachable();
#endif /* VERIFY_DATA */
        }
    }
};
}

template <typename T>
using DiskVector = vtl::DiskVector<T, vtl::FixedParam<T>>;
template <typename T>
using VarDiskVector = vtl::DiskVector<T, vtl::VarParam<T>>;
} /* namespace disk_container */

#endif /* CONTAINER_DISK_VECTOR_H */
