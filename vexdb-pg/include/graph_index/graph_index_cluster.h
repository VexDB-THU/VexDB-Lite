/**
 * Copyright ...
 */

#ifndef GRAPH_INDEX_CLUSTER_H
#define GRAPH_INDEX_CLUSTER_H

#include <vtl/vector>
#include <vtl/span>
#include <vtl/disk_container/plain_store.hpp>

#include "pg_compat.h"
#include "graph_index/graph_index_struct.h"
#include "graph_index/graph_index_stats.h"

/* use id can also directly get the element */
struct GraphIndexElementBase {
    /*
     * Flag bit layout:
     *   bit 0:     deleted flag (0x01), not used
     *   bit 1:     resesrved
     *   bits 2-5:  ntids count (0x0f << 2, max 15)
     *   bit 6:     extended (0x40)
     *   bit 7:     double extended (0x80)
     */
    uint8 flag;
    ItemPointerData heaptids[GRAPH_INDEX_MAX_HEAPTIDS];

    const uint8 &get_flag() const { return flag; }
    uint8 &get_flag() { return flag; }
    const ItemPointer get_heaptids() const { return (const ItemPointer)heaptids; }
    ItemPointer get_heaptids() { return heaptids; }

    void init() { get_flag() = 0; }
    bool is_deleted() const { return get_flag() & 0x01; }
    void set_deleted() { get_flag() |= 0x01; }
    bool is_extended() const { return get_flag() & 0x40; }
    void set_extended() { get_flag() |= 0x40; }
    bool is_double_extended() const { return get_flag() & 0x80; }
    void set_double_extended(bool v = true)
    {
        if (v) {
            get_flag() |= 0x80;
        } else {
            get_flag() &= 0x7f;
        }
    }
    uint8 ntids() const { return (get_flag() >> 2) & 0x0f; }
    /* make sure you know what is going on when you call this func */
    void set_ntids(uint8 n) { get_flag() = (get_flag() & 0xc3) | (n << 2); }
    bool empty() const { return ntids() == 0; }

protected:
    using PlainStore = disk_container::PlainStore;
};

struct PointExtensionContext {
    PointExtensionContext(Relation index, BlockNumber ps_blkno, bool need_wal)
        : ps(index, ps_blkno, need_wal) {}
    disk_container::PlainStore ps;
    uint32 code_size{0};

    void destroy() { ps.destroy(); }
};

struct GraphIndexPoint : public GraphIndexElementBase {
    using Data = ItemPointerData;

    GraphIndexPoint() = default; /* for GraphIndexCluster inheritance */
    GraphIndexPoint(PointExtensionContext &ctx, Span<const Data> data)
    {
        Assert(!data.empty());
        init();
        set_ntids(1u);
        heaptids[0] = data[0];
        if (data.size() > 1) {
            insert_tid(ctx, data.subspan(1));
        }
    }

protected:
    static constexpr uint32 tid_page_cap = PlainStore::max_size / sizeof(Data);
public:
    bool insert_tid(PointExtensionContext &ctx, Span<const Data> data, bool &overwriten)
    {
        Assert(!data.empty());
        if (empty()) {
            /* 
             * is empty and searchable, 
             * only if the index is under vacuum and will be mark deleted later
             */
            return false;
        }

        Assert(data.size_bytes() <= PlainStore::max_size);
        uint8 nentry = ntids();
        if (!is_extended() && nentry + data.size() <= GRAPH_INDEX_MAX_HEAPTIDS) {
            for (const Data &d : data) {
                get_heaptids()[nentry++] = d;
            }
            set_ntids(nentry);
            overwriten = true;
            return true;
        }
        bool res = true;
        if (!is_extended()) {
            set_extended();
            set_ntids(1u);
            Assert(tid_page_cap >= nentry + data.size());
            Data temp_tids[nentry + data.size()];
            for (int i = 0; i < nentry; ++i) {
                temp_tids[i] = get_heaptids()[i];
            }
            Data *temp_cur = temp_tids + nentry;
            for (const Data &d : data) {
                *temp_cur = d;
                ++temp_cur;
            }
            get_heaptids()[0] = ctx.ps.put(temp_tids, sizeof(temp_tids));
            overwriten = true;
        } else {
            Assert(nentry > 0);
            auto key = get_heaptids()[nentry - 1u];
            size_t old_size, new_size = 0;
            char buf[PlainStore::max_size];
            ctx.ps.get(key, [&](const void *in_data, Size size) {
                old_size = size;
                if (size + sizeof(Data) > PlainStore::max_size) {
                    return;
                }
                new_size = std::min(PlainStore::max_size, size + data.size_bytes());
                memcpy(buf, in_data, size);
            });
            if (new_size == 0) {
                if (likely(nentry < GRAPH_INDEX_MAX_HEAPTIDS)) {
                    get_heaptids()[nentry] = ctx.ps.put(data.data(), data.size_bytes());
                    set_ntids(nentry + 1u);
                    overwriten = true;
                } else {
                    res = false;
                }
            } else {
                Data *cur = (Data *)(buf + old_size);
                size_t space = (PlainStore::max_size - old_size) / sizeof(Data);
                size_t nnew = std::min(space, data.size());
                if (nnew < data.size() && unlikely(nentry >= GRAPH_INDEX_MAX_HEAPTIDS)) {
                    return false;
                }
                for (const Data &d : data.subspan(0, nnew)) {
                    *cur = d;
                    ++cur;
                }
                get_heaptids()[nentry - 1u] = ctx.ps.set(key, buf, new_size);
                overwriten = ItemPointerEquals(&key, get_heaptids() + nentry - 1);
                if (nnew < data.size()) {
                    auto s = data.subspan(nnew);
                    get_heaptids()[nentry] = ctx.ps.put(s.data(), s.size_bytes());
                    set_ntids(nentry + 1u);
                    overwriten = true;
                }
            }
        }
        return res;
    }
    bool insert_tid(PointExtensionContext &ctx, Span<const Data> data)
    {
        bool unused;
        return insert_tid(ctx, data, unused);
    }
    uint32 get_tids(Vector<Data> &tids, PointExtensionContext &ctx) const
    {
        uint32 res;
        if (!is_extended()) {
            res = ntids();
            tids.push_back(get_heaptids(), get_heaptids() + res);
            return res;
        }
        uint8 nkey = ntids();
        res = 0;
        for (uint8 i = 0; i < nkey; ++i) {
            ctx.ps.get(get_heaptids()[i], [&](const void *in_data, Size size) {
                const ItemPointer data = (const ItemPointer)in_data;
                uint32 ndata = size / sizeof(Data);
                tids.push_back(data, data + ndata);
                res += ndata;
            });
        }
        return res;
    }
    template <typename F>
    void apply_on_tids(PointExtensionContext &ctx, F &&f) const
    {
        bool stop = false;
        const auto do_apply = [&](const ItemPointer data, uint16 idx) {
            while (idx != 0) {
                --idx;
                if (f(data[idx])) {
                    stop = true;
                    return;
                }
            }
        };
        if (!is_extended()) {
            do_apply(get_heaptids(), ntids());
            return;
        }
        for (uint8 idx = ntids(); idx != 0 && !stop;) {
            --idx;
            ctx.ps.get(get_heaptids()[idx], [&](const void *data, Size size) {
                do_apply((const ItemPointer)data, size / sizeof(Data));
            });
        }
    }
    uint32 actual_ntids(PointExtensionContext &ctx) const
    {
        const uint8 nkey = ntids();
        if (!is_extended()) {
            return nkey;
        }
        uint32 res = 0;
        for (uint8 i = 0; i < nkey; ++i) {
            ctx.ps.get(get_heaptids()[i], [&](const void *, Size size) {
                res += size / sizeof(Data);
            });
        }
        return res;
    }
    template <typename F>
    uint32 vacuum_tids(F &&filter, PointExtensionContext &ctx, bool &dirty)
    {
        dirty = false;
        const uint8 ntid = ntids();
        if (ntid == 0) {
            /* when will happen ? */
            return 0;
        }

        if (!is_extended()) {
            uint8 start_idx = 0;
            for (uint8 i = 0; i < ntid; ++i) {
                if (filter(get_heaptids()[i])) {
                    continue;
                }
                get_heaptids()[start_idx] = get_heaptids()[i];
                ++start_idx;
            }
            dirty = start_idx != ntid;
            set_ntids(start_idx);
            return ntid - start_idx;
        }

        ItemPointer buf = (ItemPointer)palloc(sizeof(Data) * tid_page_cap);
        uint32 nremoved;
        uint32 nremain;
        ctx.ps.set(get_heaptids()[0], [&](const void *in_data, Size size) -> bool {
            ItemPointer data = (ItemPointer)in_data;
            const uint32 ndata = size / sizeof(Data);
            uint32 start_idx = 0;
            for (uint32 i = 0; i < ndata; ++i) {
                if (filter(data[i])) {
                    continue;
                }
                data[start_idx] = data[i];
                ++start_idx;
            }
            nremain = start_idx;
            nremoved = ndata - start_idx;
            memcpy(buf, in_data, start_idx * sizeof(Data));
            return nremoved > 0;
        });
        if (ntid == 1u) {
            if (nremain == 0) {
                set_ntids(0);
                dirty = true;
            }
            pfree(buf);
            return nremoved;
        }

        uint8 start_idx = 0;
        bool updated = false;
        for (uint8 i = 1u; i < ntid; ++i) {
            ctx.ps.set(get_heaptids()[i], [&](const void *in_data, Size size) -> bool {
                uint8 internal_start_idx = 0;
                uint32 ndata = size / sizeof(Data);
                uint32 old_nremain = nremain;
                ItemPointer data = (ItemPointer)in_data;
                for (uint32 i = 0; i < ndata; ++i) {
                    if (filter(data[i])) {
                        ++nremoved;
                        continue;
                    }
                    if (nremain < tid_page_cap) {
                        buf[nremain] = data[i];
                        ++nremain;
                    } else {
                        data[internal_start_idx] = data[i];
                        ++internal_start_idx;
                    }
                }
                
                if (nremain >= tid_page_cap) {
                    nremain = internal_start_idx;
                    auto key = ctx.ps.set(get_heaptids()[start_idx], buf,
                                      tid_page_cap * sizeof(Data));
                    if (key != get_heaptids()[start_idx]) {
                        dirty = true;
                        get_heaptids()[start_idx] = key;
                    }
                    ++start_idx;
                    memcpy(buf, data, nremain * sizeof(Data));
                    updated = false;
                } else if (old_nremain != nremain) {
                    updated = true;
                }
                return internal_start_idx != ndata;
            });
            if (i != start_idx) {
                get_heaptids()[start_idx] = get_heaptids()[i];
                dirty = true;
            }
        }
        if (start_idx != ntid) {
            if (updated) {
                get_heaptids()[start_idx] =
                    ctx.ps.set(get_heaptids()[start_idx], buf, nremain * sizeof(Data));
            }
            set_ntids(start_idx);
            dirty = true;
        }
        pfree(buf);
        return nremoved;
    }
};

struct GraphIndexCluster : public GraphIndexPoint {
    struct Data {
        ItemPointerData tid;
        char *code{NULL};
    };

    /* TD: add intra-cluster stats */
    uint8 new_inserted{0};
    float R_j;
    GIwelford<false> welford;

    GraphIndexCluster(PointExtensionContext &ctx, Span<const Data> data)
    {
        init();
        size_t s = (ctx.code_size + sizeof(ItemPointerData)) * data.size();
        Assert(s <= PlainStore::max_size);
        char buf[s];
        TQHandle handler = {ctx.code_size, s, buf};
        uint8 *pq = handler.get_pq();
        ItemPointer tids = handler.get_tid();
        for (const auto &d : data) {
            memcpy(pq, d.code, ctx.code_size);
            *tids = d.tid;
            pq += ctx.code_size;
            ++tids;
        }
        heaptids[0] = ctx.ps.put(buf, s);
        set_ntids(1u);
    }

    /**
     * Tid-Quantization Handler
     * struct layout:
     * 	uint8[] pq;
     *  tid[] tids;
     *  char[] paddings;	// make sure the next struct is aligned
     */
    struct TQHandle {
        uint32 pq_len;
        Size size;
        char *buf;

        size_t pq_size() const { return pq_len * sizeof(uint8); }
        uint32 ndata() const { return size / (pq_size() + sizeof(ItemPointerData)); }
        uint8 *get_pq() { return (uint8 *)buf; }
        ItemPointer get_tid() { return (ItemPointer)(buf + pq_size() * ndata()); }
        void *try_insert(Span<const Data> &data, bool allow_partial, size_t &new_size)
        {
            const size_t item_size = pq_size() + sizeof(ItemPointerData);
            new_size = (data.size() + ndata()) * item_size;
            size_t partial_idx = data.size();
            if (new_size >= PlainStore::max_size) {
                if (!allow_partial) {
                    return NULL;
                }
                partial_idx = (PlainStore::max_size - ndata() * item_size) / item_size;
                if (partial_idx == 0) {
                    return NULL;
                }
            }
            char *temp = (char *)palloc(new_size);
            const size_t pqs = pq_size() * ndata();
            memcpy(temp, buf, pqs);
            memcpy(temp + pqs + pq_size() * data.size(), buf + pqs, sizeof(ItemPointerData) * ndata());
            char *temp_cur = temp + pqs;
            for (const Data &d : data.subspan(0, partial_idx)) {
                memcpy(temp_cur, d.code, pq_size());
                temp_cur += pq_size();
            }
            temp_cur += sizeof(ItemPointerData) * ndata();
            for (const Data &d : data.subspan(0, partial_idx)) {
                *(ItemPointer)temp_cur = d.tid;
                temp_cur += sizeof(ItemPointerData);
            }
            data = data.subspan(partial_idx);
            return temp;
        }
        static uint32 fullsize(uint32 pq_len)
        {
            return (PlainStore::max_size - 1ul) / (pq_len * sizeof(uint8) + sizeof(ItemPointerData));
        }
    };

    bool insert_tid(PointExtensionContext &ctx, Span<const Data> data, bool &overwriten)
    {
        if (ctx.code_size == 0) {
            size_t s = data.size();
            GraphIndexPoint::Data d[s];
            for (size_t i = 0; i < s; ++i) {
                d[i] = data[i].tid;
            }
            return GraphIndexPoint::insert_tid(ctx, {d, s}, overwriten);
        }
        Assert((ctx.code_size + sizeof(ItemPointerData)) * data.size() <= PlainStore::max_size);
        if (empty()) {
            return false;
        }
        uint8 nentry = ntids();
        void *new_data;
        size_t new_size;
        bool allow_partial = false;
        const auto get_new_data = [&](const void *in_data, Size size) {
            TQHandle handler = {ctx.code_size, size, (char *)in_data};
            new_data = handler.try_insert(data, allow_partial, new_size);
        };
        const auto generate_new_data = [&]() -> Size {
            const Size s = (ctx.code_size + sizeof(ItemPointerData)) * data.size();
            new_data = palloc(s);
            char *data_cur = (char *)new_data;
            for (const Data &d : data) {
                memcpy(data_cur, d.code, ctx.code_size);
                data_cur += ctx.code_size;
            }
            for (const Data &d : data) {
                *(ItemPointer)data_cur = d.tid;
                data_cur += sizeof(ItemPointerData);
            }
            return s;
        };
        if (!is_double_extended()) {
            allow_partial = nentry < GRAPH_INDEX_MAX_HEAPTIDS;
            ctx.ps.get(get_heaptids()[nentry - 1u], get_new_data);
            if (!new_data) {
                Size s = generate_new_data();
                auto k = ctx.ps.put(new_data, s);
                if (nentry < GRAPH_INDEX_MAX_HEAPTIDS) {
                    get_heaptids()[nentry] = k;
                    set_ntids(nentry + 1u);
                } else {
                    ItemPointerData temp_tids[GRAPH_INDEX_MAX_HEAPTIDS + 1];
                    memcpy(temp_tids, get_heaptids(), sizeof(ItemPointerData[GRAPH_INDEX_MAX_HEAPTIDS]));
                    temp_tids[GRAPH_INDEX_MAX_HEAPTIDS] = k;
                    set_double_extended();
                    set_ntids(1u);
                    get_heaptids()[0] = ctx.ps.put(temp_tids, sizeof(temp_tids));
                }
                overwriten = true;
            } else {
                auto k = ctx.ps.set(get_heaptids()[nentry - 1u], new_data, new_size);
                overwriten = !ItemPointerEquals(&k, &get_heaptids()[nentry - 1u]);
                if (overwriten) {
                    get_heaptids()[nentry - 1] = k;
                }
                if (!data.empty()) {
                    pfree(new_data);
                    Size s = generate_new_data();
                    get_heaptids()[nentry] = ctx.ps.put(new_data, s);
                    set_ntids(nentry + 1);
                }
            }
        } else {
            uint32 nptr;
            ctx.ps.get(get_heaptids()[nentry - 1u], [&](const void *in_data, Size size) {
                ItemPointer p = (ItemPointer)in_data;
                nptr = size / sizeof(ItemPointerData);
                if (nptr > 0) {
                    allow_partial = nentry < GRAPH_INDEX_MAX_HEAPTIDS || nptr < tid_page_cap;
                    ctx.ps.get(p[nptr - 1u], get_new_data);
                } else {
                    new_data = NULL;
                }
            });
            if (!new_data) {
                const Size s = generate_new_data();
                auto k = ctx.ps.put(new_data, s);
                if (nptr < tid_page_cap) {
                    ++nptr;
                    const Size ptr_size = sizeof(ItemPointerData) * nptr;
                    ItemPointer ptr = (ItemPointer)palloc(ptr_size);
                    ctx.ps.get(get_heaptids()[nentry - 1u], [&](const void *in_data, Size size) {
                        memcpy(ptr, in_data, ptr_size);
                    });
                    ptr[nptr - 1u] = k;
                    k = ctx.ps.set(get_heaptids()[nentry - 1u], ptr, ptr_size);
                    overwriten = !ItemPointerEquals(&k, get_heaptids() + nentry - 1u);
                    if (overwriten) {
                        get_heaptids()[nentry - 1u] = k;
                    }
                } else if (nentry >= GRAPH_INDEX_MAX_HEAPTIDS) {
                    pfree(new_data);
                    return false;
                } else {
                    get_heaptids()[nentry] = ctx.ps.put(&k, sizeof(ItemPointerData));
                    set_ntids(nentry + 1u);
                    overwriten = true;
                }
            } else {
                ItemPointer ptr = NULL;
                Size ptr_size;
                ctx.ps.set(get_heaptids()[nentry - 1u], [&](void *in_data, Size size) -> bool {
                    ItemPointer p = (ItemPointer)in_data;
                    uint32 n = size / sizeof(ItemPointerData);
                    auto k = ctx.ps.set(p[n - 1u], new_data, new_size);
                    bool writen = !ItemPointerEquals(&k, &p[n - 1u]);
                    if (writen) {
                        p[n - 1u] = k;
                    }
                    if (!data.empty() && n < tid_page_cap) {
                        ptr_size = sizeof(ItemPointerData) * (n + 1);
                        ptr = (ItemPointer)palloc(ptr_size);
                        memcpy(ptr, in_data, ptr_size);
                    } else {
                        new_data = NULL;
                    }
                    return writen;
                });
                if (ptr || !data.empty()) {
                    pfree(new_data);
                    const Size s = generate_new_data();
                    auto k = ctx.ps.put(new_data, s);
                    if (ptr) {
                        ptr[ptr_size / sizeof(ItemPointerData) - 1] = k;
                        k = ctx.ps.set(get_heaptids()[nentry - 1u], ptr, ptr_size);
                        overwriten = !ItemPointerEquals(&k, get_heaptids() + nentry - 1u);
                        if (overwriten) {
                            get_heaptids()[nentry - 1u] = k;
                        }
                    } else {
                        get_heaptids()[nentry] = ctx.ps.put(&k, sizeof(ItemPointerData));
                        set_ntids(nentry + 1u);
                        overwriten = true;
                    }
                } else {
                    overwriten = false;
                }
            }
        }
        pfree(new_data);
        return true;
    }
    bool insert_tid(PointExtensionContext &ctx, Span<const Data> data)
    {
        bool unused;
        return insert_tid(ctx, data, unused);
    }
    uint32 get_tids(Vector<ItemPointerData> &tids, PointExtensionContext &ctx) const
    {
        if (ctx.code_size == 0) {
            return GraphIndexPoint::get_tids(tids, ctx);
        }
        const bool double_ext = is_double_extended();
        uint32 res = 0;
        uint8 nkey = ntids();
        const auto get_data = [&](const void *in_data, Size size) {
            TQHandle handler = {ctx.code_size, size, (char *)in_data};
            const ItemPointer data = (const ItemPointer)handler.get_tid();
            uint32 ndata = handler.ndata();
            tids.push_back(data, data + ndata);
            res += ndata;
        };
        for (uint8 i = 0; i < nkey; ++i) {
            if (double_ext) {
                ctx.ps.get(get_heaptids()[i], [&](const void *in_data, Size size) {
                    const ItemPointer data = (const ItemPointer)in_data;
                    uint32 ndata = size / sizeof(ItemPointerData);
                    for (uint32 j = 0; j < ndata; ++j) {
                        ctx.ps.get(data[j], get_data);
                    }
                });
            } else {
                ctx.ps.get(get_heaptids()[i], get_data);
            }
        }
        return res;
    }
    template <typename F>
    void apply_on_tids(PointExtensionContext &ctx, F &&f) const
    {
        if (ctx.code_size == 0) {
            return GraphIndexPoint::apply_on_tids(ctx, std::forward<F>(f));
        }
        bool stop = false;
        const auto do_apply = [&](const void *in_data, Size size) {
            TQHandle handler = {ctx.code_size, size, (char *)in_data};
            const ItemPointer data = (const ItemPointer)handler.get_tid();
            uint32 idx = handler.ndata();
            while (idx != 0) {
                --idx;
                if (f(data[idx])) {
                    stop = true;
                    break;
                }
            }
        };
        const bool double_ext = is_double_extended();
        for (uint8 i = ntids(); i != 0;) {
            --i;
            if (double_ext) {
                ctx.ps.get(get_heaptids()[i], [&](const void *in_data, Size size) {
                    const ItemPointer data = (const ItemPointer)in_data;
                    for (uint32 j = size / sizeof(ItemPointerData); j != 0 && !stop;) {
                        --j;
                        ctx.ps.get(data[i], do_apply);
                    }
                });
            } else {
                ctx.ps.get(get_heaptids()[i], do_apply);
            }
        }
    }
    uint32 actual_ntids(PointExtensionContext &ctx) const
    {
        if (ctx.code_size == 0) {
            return GraphIndexPoint::actual_ntids(ctx);
        }
        const uint8 nkey = ntids();
        const size_t item_size = sizeof(uint8) * ctx.code_size + sizeof(ItemPointerData);
        uint32 res = 0;
        const auto iter = [&](const void *, Size s) {
            res += s / item_size;
        };
        if (is_double_extended()) {
            for (uint32 i = 0; i < nkey; ++i) {
                ctx.ps.get(get_heaptids()[i], [&](const void *in_data, Size size) {
                    ItemPointer k = (ItemPointer)in_data;
                    uint32 nptr = size / sizeof(ItemPointerData);
                    for (uint32 j = 0; j < nptr; ++j) {
                        ctx.ps.get(k[j], iter);
                    }
                });
            }
        } else {
            for (uint32 i = 0; i < nkey; ++i) {
                ctx.ps.get(get_heaptids()[i], iter);
            }
        }
        return res;
    }
    template <typename F>
    uint32 vacuum_tids(F &&filter, PointExtensionContext &ctx, bool &dirty)
    {
        if (ctx.code_size == 0) {
            return GraphIndexPoint::vacuum_tids(std::forward<F>(filter), ctx, dirty);
        }
        const uint8 ntid = ntids();
        if (ntid == 0) {
            dirty = false;
            return 0;
        }
        dirty = true;
        uint32 res = 0;
        Vector<ItemPointerData> tids;
        Vector<uint8> codes;
        const auto gather = [&](const void *in_data, Size size) {
            TQHandle handler = {ctx.code_size, size, (char *)in_data};
            uint32 n = handler.ndata();
            ItemPointer ptr = handler.get_tid();
            for (uint32 i = 0; i < n; ++i) {
                if (filter(ptr[i])) {
                    ++res;
                    continue;
                }
                tids.push_back(ptr[i]);
                uint8 *pq_pos = handler.get_pq() + i * ctx.code_size;
                codes.push_back(pq_pos, pq_pos + ctx.code_size);
            }
        };
        for (uint8 i = 0; i < ntid; ++i) {
            if (is_double_extended()) {
                ctx.ps.get(get_heaptids()[i], [&](const void *in_data, Size size) {
                    ItemPointer ptr = (ItemPointer)in_data;
                    uint32 n = size / sizeof(ItemPointerData);
                    for (uint32 j = 0; j < n; ++j) {
                        ctx.ps.get(ptr[j], gather);
                    }
                });
            } else {
                ctx.ps.get(get_heaptids()[i], gather);
            }
        }
        ItemPointerData temp_heaptids[GRAPH_INDEX_MAX_HEAPTIDS];
        size_t n = tids.size();
        const size_t total_size_used = n * (sizeof(uint8) * ctx.code_size + sizeof(ItemPointerData));
        constexpr double full_threshold = 0.92;
        const size_t page_cap = TQHandle::fullsize(ctx.code_size);
        char *buf = (char *)palloc(PlainStore::max_size);
        uint8 *vecs = codes.data();
        ItemPointer cur_tid = tids.data(); 
        const auto load_data = [&]() -> PlainStore::key {
            uint32 l = std::min<uint32>(n, page_cap);
            Size s = l * (sizeof(uint8) * ctx.code_size + sizeof(ItemPointerData));
            uint8 *cur = (uint8 *)buf;
            memcpy(cur, vecs, sizeof(uint8) * ctx.code_size * l);
            memcpy(cur + ctx.code_size * l, cur_tid, sizeof(ItemPointerData) * l);
            cur_tid += l;
            vecs += l * ctx.code_size;
            n -= l;
            return ctx.ps.put(buf, s);
        };

        uint8 cur_pos = 0;
        if (total_size_used > PlainStore::max_size * GRAPH_INDEX_MAX_HEAPTIDS * full_threshold) {
            set_double_extended();
            Vector<ItemPointerData> holder;
            while (n > 0) {
                for (uint32 i = 0; i < tid_page_cap && n > 0; ++i) {
                    holder.push_back(load_data());
                }
                temp_heaptids[cur_pos] = ctx.ps.put(holder.data(), holder.size() * sizeof(ItemPointerData));
                ++cur_pos;
                holder.clear();
            }
            ann_helper::optional_destroy(holder);
        } else {
            set_double_extended(false);
            while (n > 0) {
                temp_heaptids[cur_pos] = load_data();
                ++cur_pos;
            }
        }
        set_ntids(cur_pos);
        for (uint8 i = 0; i < cur_pos; ++i) {
            std::swap(get_heaptids()[i], temp_heaptids[i]);
        }
        pfree(buf);
        ann_helper::optional_destroy(tids);
        ann_helper::optional_destroy(codes);
        for (uint8 i = 0; i < ntid; ++i) {
            if (is_double_extended()) {
                ctx.ps.get(temp_heaptids[i], [&](const void *in_data, Size size) {
                    ItemPointer ptr = (ItemPointer)in_data;
                    uint32 n = size / sizeof(ItemPointerData);
                    for (uint32 i = 0; i < n; ++i) {
                        ctx.ps.erase(ptr[i]);
                    }
                });
            }
            ctx.ps.erase(temp_heaptids[i]);
        }
        return res;
    }
};


bool try_set_under_redistrib(Relation index, uint32 id);
void reset_under_redistrib(Relation index, uint32 id);

#endif /* GRAPH_INDEX_CLUSTER_H */
