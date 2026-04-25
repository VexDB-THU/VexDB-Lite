/**
 * Copyright ...
 */

#ifndef CONTAINER_DISK_HASHTABLE_H
#define CONTAINER_DISK_HASHTABLE_H

#include <utility>
#include <type_traits>
#include <functional>
#include <atomic>

#include <vtl/bitvector>
#include <vtl/btree>
#include <vtl/vector>
#include <vtl/optional>
#include <vtl/disk_container/disk_hashtable_dependency.hpp>
#include <vtl/disk_container/diskarray.hpp>
#include <vtl/internal/type.hpp>
#include <vtl/internal/expr.hpp>
#include <vtl/disk_container/macro.hpp>

#include "utils/elog.h"
#include "storage/indexfsm.h"

namespace disk_container {
constexpr uint16 DISK_HASHTABLE_VERSION = 1u;

struct DiskHashTableLayerStats {
    size_t nentry{0};
    size_t ten_hash_arr_count{0};
    size_t one_hash_arr_b1_count{0};
    size_t one_hash_arr_b2_count{0};
    size_t one_hash_arr_count{0};
    size_t inplace_hash_count{0};
    size_t inplace_count{0};
    size_t plain_arr_count{0};
    size_t plain_count{0};
    size_t none_slot_count{0};
    DiskHashTableLayerStats() = default;
};
struct DiskHashTableStats {
    size_t base_size;
    size_t plain_size;
    size_t inplace_size;
    size_t inplace_load;
    size_t hash_one_b1_size;
    size_t hash_one_b2_size;
    size_t hash_one_size;
    size_t hash_ten_size;
    size_t nentry{0};
    size_t nblock{0};
    Vector<DiskHashTableLayerStats> level_stats{};
    void destroy() { level_stats.destroy(); }
    void print() const
    {
        elog(NOTICE, "DiskHashTableStats: nentry=%lu, nblock=%lu, "
                     "base_size=%lu, plain_size=%lu, inplace_size=%lu, inplace_load=%lu, "
                     "hash_one_b2_size=%lu, hash_one_b1_size=%lu, hash_one_size=%lu, hash_ten_size=%lu",
             nentry, nblock, base_size, plain_size, inplace_size, inplace_load,
             hash_one_b2_size, hash_one_b1_size, hash_one_size, hash_ten_size);
        for (size_t i = 0; i < level_stats.size(); ++i) {
            const auto &stats = level_stats[i];
            elog(NOTICE, "Level %lu(%lu): ten_hash_arr_count=%lu, one_hash_arr_count=%lu, "
                         "one_hash_b1_arr_count=%lu, one_hash_b2_arr_count=%lu, inplace_hash_count=%lu, "
                         "inplace_count=%lu, plain_arr_count=%lu, plain_count=%lu, none_slot_count=%lu",
                 i, stats.nentry, stats.ten_hash_arr_count, stats.one_hash_arr_count,
                 stats.one_hash_arr_b1_count, stats.one_hash_arr_b2_count,
                 stats.inplace_hash_count, stats.inplace_count, stats.plain_arr_count,
                 stats.plain_count, stats.none_slot_count);
        }
    }
};

template <typename K, typename V, class Hasher = std::hash<K>, class Comparable = std::equal_to<K>,
          size_t BaseSize = 100ul>
class DiskHashTable {
    static constexpr bool plain_entry_use_hash = sizeof(K) > 64ul;
    static constexpr uint32 HEADER_SIZE = MAXALIGN(SizeOfPageHeaderData);
    using PlainEntry = std::conditional_t<plain_entry_use_hash,
                                          PlainEntryWithHash<K, V>, PlainEntryWithoutHash<K, V>>;
    struct PlainEntryPageData {
        uint32 magic;
        uint16 version;
        uint16 nentry;
        PlainEntry entries[FLEXIBLE_ARRAY_MEMBER];
        void init() { magic = HASH_PLAIN_DATA_MAGIC; version = DISK_HASHTABLE_VERSION; nentry = 0; }
        void remove(uint16 idx)
        {
            Assert(idx < nentry);
            const uint16 right = idx + 1u;
            const uint16 nentry_right = nentry - right;
#if defined(__GNUC__) && __GNUC__ >= 8 && __cplusplus >= 201703L
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#endif
            memmove(entries + idx, entries + right, sizeof(PlainEntry) * nentry_right);
#if defined(__GNUC__) && __GNUC__ >= 8 && __cplusplus >= 201703L
#pragma GCC diagnostic pop
#endif
            --nentry;
        }
    };
    struct HTEntryOpaqueData {
        uint16 type_id;
        uint16 page_id;
        void init() { page_id = HASH_TABLE_DATA_ID; type_id = ann_helper::GET_TYPE_ID(PlainEntry); }
    };
    using PlainEntryPage = PlainEntryPageData *;
    using HTEntryOpaque = HTEntryOpaqueData *;
    static PlainEntryPage get_pe_page(char *page)
        { return (PlainEntryPage)(page + HEADER_SIZE); }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    static constexpr uint32 pe_offset = offsetof(PlainEntryPageData, entries);
#pragma GCC diagnostic pop
    static constexpr size_t max_size = PAGE_SIZE - MAXALIGN(sizeof(HTEntryOpaqueData));
    static constexpr size_t max_entry_size = max_size - pe_offset;
    static constexpr size_t max_list_size = max_entry_size / sizeof(PlainEntry);

    template <size_t s> struct temp_struct {
        uint32 magic;
        uint16 version;
        uint16 nentry;
        BitSet<s> bitmap;
        PlainEntry entries[std::min(max_size, s)];
    };
    static constexpr size_t max_list_size_temp = (max_size - 8ul) / (sizeof(PlainEntry) + 0.25);
    template <size_t s> static constexpr bool reduced_temp_struct_size() {
        /* avoid large kv overflow */
        CONSTEXPR_IF(s >= max_list_size_temp) {
            return false;
        } else {
            return max_size >= sizeof(temp_struct<max_list_size_temp - s>);
        }
    }
    static constexpr size_t max_inplace_list_size = reduced_temp_struct_size<0>() ?
            max_list_size_temp : (reduced_temp_struct_size<1ul>() ?
            max_list_size_temp - 1ul : (reduced_temp_struct_size<2ul>() ?
            max_list_size_temp - 2ul : (reduced_temp_struct_size<3ul>() ?
            max_list_size_temp - 3ul : (reduced_temp_struct_size<4ul>() ?
            max_list_size_temp - 4ul : (reduced_temp_struct_size<5ul>() ?
            max_list_size_temp - 5ul : (reduced_temp_struct_size<6ul>() ?
            max_list_size_temp - 6ul : (reduced_temp_struct_size<7ul>() ?
            max_list_size_temp - 7ul : (reduced_temp_struct_size<8ul>() ?
            max_list_size_temp - 8ul : (reduced_temp_struct_size<9ul>() ?
            max_list_size_temp - 9ul : 0)))))))));
    static constexpr size_t max_inplace_entry_load = max_inplace_list_size * 0.75;
    struct InplaceEntryPageData {
        uint32 magic;
        uint16 version;
        uint16 nentry;
        BitSet<max_inplace_list_size * 2ul> bitmap;
        PlainEntry entries[max_inplace_list_size];

        struct iterator {
            size_t pos{size_t(-1)};
            InplaceEntryPageData &pd;
            iterator(InplaceEntryPageData &pd) : pd(pd) {}
            iterator &operator++()
            {
                pos = pd.bitmap.next(pos);
                if (pos >= max_inplace_list_size) {
                    pos = size_t(-1);
                }
                return *this;
            }
            PlainEntry &operator*()
            {
                Assert(pos >= 0 && pos < max_inplace_list_size);
                Assert(pd.bitmap[pos]);
                return pd.entries[pos];
            }
            PlainEntry *operator->()
            {
                Assert(pos >= 0 && pos < max_inplace_list_size);
                Assert(pd.bitmap[pos]);
                return pd.entries + pos;
            }
            bool operator==(const iterator &other) const { return pos == other.pos; }
            bool operator!=(const iterator &other) const { return pos != other.pos; }
            uint16 idx() const { return pos; }
        };

        void init()
        {
            magic = HASH_INPLACE_DATA_MAGIC<max_inplace_list_size>();
            version = DISK_HASHTABLE_VERSION;
            nentry = 0;
            new (&bitmap) decltype(bitmap)();
        }
        bool search(const K &key, uint32 hash_val, uint32 seed_hash_val, size_t &idx) const
        {
            idx = seed_hash_val % max_inplace_list_size;
            const PlainEntry *cur = entries + idx;
            bool has_value = bitmap[idx];
            bool deleted = bitmap[idx + max_inplace_list_size];
            for (; has_value && !deleted;) {
                if (has_value && cur->template compare<Comparable>(hash_val, key)) {
                    return true;
                }
                ++idx;
                ++cur;
                if (unlikely(idx >= max_inplace_list_size)) {
                    idx = 0;
                    cur = entries;
                }
                has_value = bitmap[idx];
                deleted = bitmap[idx + max_inplace_list_size];
            }
            return false;
        }
        template <typename ...Args>
        void insert(size_t idx, uint32 hash_val, Args &&...args)
        {
            Assert(!bitmap[idx] || bitmap[idx + max_inplace_list_size]);
            bitmap.set(idx);
            bitmap.reset(idx + max_inplace_list_size);
            entries[idx].set_hash(hash_val);
            new (&entries[idx].kv) kv_base(std::forward<Args>(args)...);
            ++nentry;
            Assert(nentry <= max_inplace_entry_load);
        }
        void erase(size_t idx)
        {
            Assert(bitmap[idx]);
            bitmap.reset(idx);
            bitmap.set(idx + max_inplace_list_size);
            --nentry;
        }
        bool full() const { return nentry >= max_inplace_entry_load; }
        uint16 size() const { return nentry; }

        iterator begin()
        {
            auto it = iterator(*this);
            ++it;
            return it;
        }
        iterator end() { return iterator(*this); }
    };
    using InplaceEntryPage = InplaceEntryPageData *;
    static_assert(sizeof(InplaceEntryPageData) <= max_size, "incorrect max_inplace_list_size");
// #define strcat_(x, y) x ## y
// #define strcat(x, y) strcat_(x, y)
// #define PRINT_VALUE(x) template <int> struct strcat(strcat(value_of_, x), _is); static_assert(strcat(strcat(value_of_, x), _is)<x>::x, "");
//     static constexpr size_t my_one_page_size = sizeof(InplaceEntryPageData);
//     PRINT_VALUE(my_one_page_size);
//     PRINT_VALUE(max_inplace_list_size);
//     PRINT_VALUE(max_list_size_temp);
    static InplaceEntryPage get_ip_page(char *page)
        { return (InplaceEntryPage)(page + HEADER_SIZE); }

    using base_arr = FullDiskArray<HashEntry, BaseSize>;
    using hash_one_arr = FullDiskArray<HashEntry, 1ul>;
    using hash_ten_arr = FullDiskArray<HashEntry, 10ul>;
    static constexpr size_t base_size = base_arr::size();
    static constexpr size_t hash_one_base_size = hash_one_arr::size();
    static constexpr size_t hash_one_base_b2_size = hash_one_base_size / 10ul;
    static constexpr size_t hash_one_base_b1_size = hash_one_base_b2_size / 10ul;
    static constexpr bool use_hash_one_b2 = hash_one_base_b2_size >= 10ul;
    static constexpr bool use_hash_one_b1 = use_hash_one_b2 && hash_one_base_b1_size >= 10ul;
    static constexpr size_t hash_ten_base_size = hash_ten_arr::size();
    static constexpr size_t actual_max_list_size_temp = std::min(max_list_size, 8ul);
    static constexpr bool use_inplace_hash =
        (max_inplace_list_size >= actual_max_list_size_temp * 4ul) &&
        max_inplace_entry_load >= 8ul;
    static constexpr size_t actual_max_list_size =
        use_inplace_hash ? actual_max_list_size_temp : std::min(max_list_size, 32ul);

#if __cplusplus >= 201703L
#define run_helper(f, ...) (f)(__VA_ARGS__)
#else
    template <class F, typename ...Args, typename std::enable_if<
        !IS_INVOCABLE(F, Args...)
    >::type * = nullptr> /* I have no idea why type* = nullptr works,
                         * but type = true simply does not, credits to copilot :) */
    static bool _run_helper(F &&f, Args &&...args) { return false; }   /* should not be reached */
    template <class F, typename ...Args, typename std::enable_if<
        IS_INVOCABLE_R(F, bool, Args...)
    >::type * = nullptr>
    static bool _run_helper(F &&f, Args &&...args)
        { return std::forward<F>(f)(std::forward<Args>(args)...); }
    template <class F, typename ...Args, typename std::enable_if<
        IS_INVOCABLE(F, Args...) && !IS_INVOCABLE_R(F, bool, Args...)
    >::type * = nullptr>
    static bool _run_helper(F &&f, Args &&...args)
    {
        std::forward<F>(f)(std::forward<Args>(args)...);
        return false;
    }
#define run_helper(f, ...) _run_helper(f, ##__VA_ARGS__)
#endif /* c++17 or greater */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    static constexpr uint32 ie_offset = offsetof(InplaceEntryPageData, entries);
    static constexpr uint32 ieb_offset = offsetof(InplaceEntryPageData, bitmap);
#pragma GCC diagnostic pop
    static constexpr uint32 dad_offset = offsetof(DiskArrayDataPageData<HashEntry>, data);
public:
    using kv_base = KVBase<K, V>;
    static BlockNumber get_disk_hashtable(Relation rel, bool need_wal, ForkNumber fork_num)
    {
        return base_arr::get_disk_array(rel, need_wal && RelationNeedsWAL(rel),
                                        ann_helper::GET_TYPE_ID(HashEntry), fork_num);
    }
    DiskHashTable(Relation rel, BlockNumber start_blkno, bool need_wal)
        : _base(rel, start_blkno, need_wal && RelationNeedsWAL(rel)), _buf_cache() {}
    void destroy()
    {
        _base.destroy();
        _one_arr.destroy();
        _ten_arr.destroy();
        _buf_cache.destroy();
    }

    bool get(const K &key, V &out) {
        auto f = [&out](const kv_base &kv) { out = kv.v; };
        return visit_internal<true>(key, f);
    }
    template <class F>
    bool cvisit(const K &key, F &&f) { return visit_internal<true, F>(key, f); }
    template <class F>
    bool visit(const K &key, F &&f) { return visit_internal<false, F>(key, f); }

    template <class F>
    size_t cvisit(F &&f) { return visit_internal<true, F>(f); }
    template <class F>
    size_t visit(F &&f) { return visit_internal<false, F>(f); }

    bool insert(const K &key, V &&val) { return try_emplace(key, std::move(val)); }
    bool insert(const K &key, const V &val) { return try_emplace(key, val); }
    template <typename ...Args>
    bool try_emplace(const K &key, Args &&...args)
    {
        return try_emplace_or_visit_internal<true, decltype(_do_nothing), Args...>(
            key, _do_nothing, std::forward<Args>(args)...);
    }
    template <class F, typename ...Args>
    bool try_emplace_or_cvisit(const K &key, F &&f, Args &&...args)
    {
        return try_emplace_or_visit_internal<true, F, Args...>(
            key, f, std::forward<Args>(args)...);
    }
    template <class F, typename ...Args>
    bool try_emplace_or_visit(const K &key, F &&f, Args &&...args)
    {
        return try_emplace_or_visit_internal<false, F, Args...>(
            key, f, std::forward<Args>(args)...);
    }

    template <class F>
    bool erase_cif(const K &key, F &&f) { return erase_internal<true, F>(key, f); }
    template <class F>
    bool erase_if(const K &key, F &&f) { return erase_internal<false, F>(key, f); }
    bool erase(const K &key) { return erase_cif(key, [](const kv_base &) { return true; }); }

    template <class F>
    size_t erase_cif(F &&f) { return erase_internal<true, F>(f); }
    template <class F>
    size_t erase_if(F &&f) { return erase_internal<false, F>(f); }

    Relation get_relation() { return _base.get_relation(); }
    bool need_wal() const { return _base.need_wal(); }

    DiskHashTableStats get_stats()
    {
        DiskHashTableStats stats;
        stats.level_stats.resize(1ul);
        for (size_t i = 0; i < base_size; ++i) {
            get_stats_internal(stats, _base.get_nolock(i), 0);
        }
        stats.base_size = base_size;
        stats.plain_size = actual_max_list_size;
        stats.inplace_size = max_inplace_list_size;
        stats.inplace_load = max_inplace_entry_load;
        stats.hash_one_b1_size = hash_one_base_b1_size;
        stats.hash_one_b2_size = hash_one_base_b2_size;
        stats.hash_one_size = hash_one_base_size;
        stats.hash_ten_size = hash_ten_base_size;
        return stats;
    }

private:
    base_arr _base;
    Optional<hash_one_arr> _one_arr{};
    Optional<hash_ten_arr> _ten_arr{};
    BufferCache _buf_cache{};

    void load_buffer(BlockNumber blkno) { _buf_cache.load_buffer(get_relation(), blkno); }
    PlainEntryPage get_pe_page() const { return (PlainEntryPage)_buf_cache.get_page(); }
    InplaceEntryPage get_ip_page() const { return (InplaceEntryPage)_buf_cache.get_page(); }
    template <bool exclusive>
    void lock_buffer() { _buf_cache.template lock_buffer<exclusive>(); }
    void mark_dirty() { _buf_cache.mark_dirty(); }
    void unlock_buffer() { _buf_cache.unlock_buffer(); }

    static constexpr void _do_nothing(const kv_base &) {}
    static uint32 get_hash(const K &key) { return Hasher{}(key); }
    static uint32 get_hash(const PlainEntry &entry) { return entry.template get_hash<Hasher>(); }
    static uint32 hash_combine(uint32 hash_val, uint16 seed)
    {
        return hash_val ^ (0x9e3779b9u + (hash_val << 6) + (hash_val >> 2) +
                           (((uint32)seed) >> 4) + (seed << 4));
    }
    
    template <bool read_only, class F, typename ...Args>
    bool try_emplace_or_visit_internal(const K &key, F &f, Args &&...args)
    {
        using process_type = std::conditional_t<read_only, const kv_base &, kv_base &>;
        static_assert(read_only || IS_INVOCABLE_R(F, bool, process_type),
                      "F must be invocable with kv_base & and return bool");
        static_assert(!read_only || IS_INVOCABLE(F, process_type),
                      "F must be invocable with const kv_base &");
        bool res;
        BufferCache entry_holder;
        const auto process_value = [&f](process_type &kv, auto &&post_f) {
            CONSTEXPR_IF(read_only) {
                f(kv);
            } else {
                if (run_helper(f, kv)) {
                    post_f();
                }
            }
        };

        uint32 hash_val = get_hash(key);
        size_t entry_offset;
        size_t one_hash_arr_entry_offset = SIZE_MAX;
        HashEntry *entry = _base.get_nolock(hash_val % base_size, entry_holder, entry_offset);
        HashEntry *one_hash_arr_entry = NULL;
        BufferCache one_hash_arr_entry_holder;
        HashEntryType one_type = HashEntryType::None;
        constexpr uint32 max_loop = 512u;
        for (uint32 loop_counter = 0; loop_counter < max_loop; ++loop_counter) {
            const auto type = entry->get_type();
            switch (type) {
                case HashEntryType::None:
                    if (one_hash_arr_entry) {
                        /* entry_holder is locked if parent is a one_arr */
                        entry_holder.unlock_buffer();
                        upgrade<HashEntryType::None, false>(entry_holder, entry, entry_offset);
                        entry = one_hash_arr_entry;
                        entry_offset = one_hash_arr_entry_offset;
                        one_hash_arr_entry = NULL;
                        one_hash_arr_entry_holder.swap(entry_holder);
                    } else {
                        upgrade<HashEntryType::None, true>(entry_holder, entry, entry_offset);
                    }
                    continue;
                case HashEntryType::Plain: {
                    load_buffer(entry->blkno);
                    lock_buffer<!read_only>();
                    if (entry->get_type() != HashEntryType::Plain) {
                        unlock_buffer();
                        continue;
                    }
                    PlainEntryPage page = get_pe_page();
                    const auto try_visit = [&]() -> bool {
                        for (uint16 i = 0; i < page->nentry; ++i) {
                            if (!page->entries[i].template compare<Comparable>(hash_val, key)) {
                                continue;
                            }
                            auto& entry = page->entries[i];
                            process_value(page->entries[i].kv, [this, i, &entry](){
                                mark_dirty();
                                if (need_wal()) {
                                    xl_bm25_add_data xl_rec(
                                        HEADER_SIZE + pe_offset + i * sizeof(entry), sizeof(entry));
                                    Bm25XLogAddData(_buf_cache.get_buffer(),
                                                    _buf_cache.get_row_page(), xl_rec);
                                }
                            });
                            return false;
                        }
                        return true;
                    };
                    res = try_visit();
                    if (read_only) {
                        unlock_buffer();
                        if (!res) {
                            goto exit_forloop;
                        }
                        lock_buffer<true>();
                        if (entry->get_type() != HashEntryType::Plain) {
                            unlock_buffer();
                            continue;
                        }
                        res = try_visit();
                    }
                    if (!res) {
                        unlock_buffer();
                        goto exit_forloop;
                    }
                    if (page->nentry >= actual_max_list_size) {
                        if (one_hash_arr_entry) {
                            bool need_upgrade_to_arr = !use_inplace_hash;
                            CONSTEXPR_IF(use_inplace_hash) {
                                entry_holder.unlock_buffer();
                                need_upgrade_to_arr = !upgrade<HashEntryType::Plain, false>(
                                    entry_holder, entry, entry_offset);
                                entry_holder.lock_buffer<true>();
                            }
                            unlock_buffer();
                            if (need_upgrade_to_arr) {
                                upgrade_one_hash_arr(entry_holder,
                                    std::move(one_hash_arr_entry_holder), entry, one_hash_arr_entry,
                                    one_type, entry_offset, one_hash_arr_entry_offset);
                            }
                        } else {
                            upgrade<HashEntryType::Plain, true>(entry_holder, entry, entry_offset);
                            unlock_buffer();
                        }
                        continue;
                    }
                    page->entries[page->nentry].set_hash(hash_val);
                    new (&page->entries[page->nentry].kv) kv_base(key, std::forward<Args>(args)...);
                    ++page->nentry;
                    mark_dirty();
                    if (need_wal()) {
                        xl_bm25_insert_entry xl_rec(
                            HEADER_SIZE + pe_offset + (page->nentry - 1) * sizeof(page->entries[0]),
                            sizeof(page->entries[0]), page->nentry);
                        Bm25XLogInsertEntry(_buf_cache.get_buffer(), _buf_cache.get_row_page(),
                                            xl_rec);
                    }
                    unlock_buffer();
                } goto exit_forloop;
                case HashEntryType::InplaceHash: {
                    Assume(use_inplace_hash);
                    const uint32 seed_hash = hash_combine(hash_val, entry->get_seed());
                    load_buffer(entry->blkno);
                    lock_buffer<!read_only>();
                    if (entry->get_type() != HashEntryType::InplaceHash) {
                        unlock_buffer();
                        continue;
                    }
                    InplaceEntryPage inplace_page = get_ip_page();
                    size_t idx;
                    if (read_only) {
                        if (inplace_page->search(key, hash_val, seed_hash, idx)) {
                            f(inplace_page->entries[idx].kv);
                            unlock_buffer();
                            res = false;
                            goto exit_forloop;
                        }
                        unlock_buffer();
                        lock_buffer<true>();
                        if (entry->get_type() != HashEntryType::InplaceHash) {
                            unlock_buffer();
                            continue;
                        }
                    }
                    if (inplace_page->search(key, hash_val, seed_hash, idx)) {
                        auto& entry = inplace_page->entries[idx];
                        process_value(inplace_page->entries[idx].kv, [this, idx, &entry](){
                            mark_dirty();
                            if (need_wal()) {
                                xl_bm25_add_data xl_rec(
                                    HEADER_SIZE + ie_offset + idx * sizeof(entry), sizeof(entry));
                                Bm25XLogAddData(_buf_cache.get_buffer(),
                                                _buf_cache.get_row_page(), xl_rec);
                            }
                        });
                        unlock_buffer();
                        res = false;
                        goto exit_forloop;
                    }
                    if (!inplace_page->full()) {
                        inplace_page->insert(idx, hash_val, key, std::forward<Args>(args)...);
                        mark_dirty();
                        if (need_wal()) {
                            xl_bm25_insert_inplace_entry xl_rec(HEADER_SIZE + ieb_offset,
                                ie_offset - ieb_offset,
                                HEADER_SIZE + ie_offset + idx * sizeof(inplace_page->entries[0]),
                                sizeof(inplace_page->entries[0]), inplace_page->nentry);
                            Bm25XLogInsertInplaceEntry(_buf_cache.get_buffer(),
                                                       _buf_cache.get_row_page(), xl_rec);
                        }
                        unlock_buffer();
                        res = true;
                        goto exit_forloop;
                    }
                    if (one_hash_arr_entry) {
                        unlock_buffer();
                        upgrade_one_hash_arr(entry_holder, std::move(one_hash_arr_entry_holder),
                                             entry, one_hash_arr_entry, one_type, entry_offset,
                                             one_hash_arr_entry_offset);
                    } else {
                        upgrade<HashEntryType::InplaceHash, true>(entry_holder, entry,
                                                                  entry_offset);
                        unlock_buffer();
                    }
                } continue;
                case HashEntryType::ExtensiveHashOneB1:
                    Assume(use_hash_one_b1);
                case HashEntryType::ExtensiveHashOneB2:
                    Assume(use_hash_one_b2);
                case HashEntryType::ExtensiveHashOne: {
                    const size_t one_base_size = type == HashEntryType::ExtensiveHashOneB1 ?
                        hash_one_base_b1_size : (type == HashEntryType::ExtensiveHashOneB2 ?
                        hash_one_base_b2_size : hash_one_base_size);
                    const size_t idx = hash_combine(hash_val, entry->get_seed()) % one_base_size;
                    _one_arr.emplace(get_relation(), entry->blkno, need_wal());
                    size_t new_entry_offset;
                    auto *new_entry =
                        _one_arr->get_nolock(idx, one_hash_arr_entry_holder, new_entry_offset);
                    one_hash_arr_entry_holder.template lock_buffer<false>();
                    if (entry->get_type() != type) {
                        one_hash_arr_entry_holder.unlock_buffer();
                        one_hash_arr_entry_holder.destroy();
                        continue;
                    }
                    /* keep entry_holder for one_hash_arr read locked */
                    one_hash_arr_entry = entry;
                    one_hash_arr_entry_offset = entry_offset;
                    one_hash_arr_entry_holder.swap(entry_holder);
                    one_type = type;
                    entry = new_entry;
                    entry_offset = new_entry_offset;
                } break;
                case HashEntryType::ExtensiveHashTen: {
                    _ten_arr.emplace(get_relation(), entry->blkno, need_wal());
                    const size_t idx =
                        hash_combine(hash_val, entry->get_seed()) % hash_ten_base_size;
                    entry = _ten_arr->get_nolock(idx, entry_holder, entry_offset);
                } break;
                default:
                    ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                        errmsg("Invalid HashTable entry type %hu", entry->get_type_u8()),
                        errhint("Index may be corrupted.")));
            }
        }
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                        errmsg("Deadloop in hashmap iteration, potential index corruption.")));
exit_forloop:
        if (one_hash_arr_entry_holder.valid()) {
            entry_holder.unlock_buffer();
            one_hash_arr_entry_holder.destroy();
        }
        entry_holder.destroy();
        return res;
    }

    template <bool read_only, class F>
    bool visit_internal(const K &key, F &f)
    {
        using process_type = std::conditional_t<read_only, const kv_base &, kv_base &>;
        static_assert(read_only || IS_INVOCABLE_R(F, bool, process_type),
                      "F must be invocable with kv_base & and return bool");
        static_assert(!read_only || IS_INVOCABLE(F, process_type),
                      "F must be invocable with const kv_base &");
        const auto process_value = [&f](process_type kv, auto &&post_f) {
            CONSTEXPR_IF(read_only) {
                f(kv);
            } else {
                if (run_helper(f, kv)) {
                    post_f();
                }
            }
        };
        uint32 hash_val = get_hash(key);
        HashEntry *entry = _base.get_nolock(hash_val % base_size);
        bool hold_one_arr_lock = false;
        bool res = false;
        constexpr uint32 max_loop = 64u;
        for (uint32 loop_counter = 0; loop_counter < max_loop; ++loop_counter) {
            switch (entry->get_type()) {
                case HashEntryType::None:
                    res = false;
                    goto exit_forloop;
                case HashEntryType::Plain: {
                    load_buffer(entry->blkno);
                    lock_buffer<!read_only>();
                    if (entry->get_type() != HashEntryType::Plain) {
                        unlock_buffer();
                        continue;
                    }
                    res = false;
                    PlainEntryPage page = get_pe_page();
                    for (uint16 i = 0; i < page->nentry; ++i) {
                        if (page->entries[i].template compare<Comparable>(hash_val, key)) {
                            auto& entry = page->entries[i];
                            process_value(page->entries[i].kv, [this, i, &entry](){
                                mark_dirty();
                                if (need_wal()) {
                                    xl_bm25_add_data xl_rec(
                                        HEADER_SIZE + pe_offset + i * sizeof(entry), sizeof(entry));
                                    Bm25XLogAddData(_buf_cache.get_buffer(),
                                                    _buf_cache.get_row_page(), xl_rec);
                                }
                            });
                            res = true;
                            break;
                        }
                    }
                    unlock_buffer();
                } goto exit_forloop;
                case HashEntryType::InplaceHash: {
                    Assume(use_inplace_hash);
                    load_buffer(entry->blkno);
                    const uint32 seed_hash = hash_combine(hash_val, entry->get_seed());
                    lock_buffer<!read_only>();
                    if (entry->get_type() != HashEntryType::InplaceHash) {
                        unlock_buffer();
                        continue;
                    }
                    InplaceEntryPage inplace_page = get_ip_page();
                    size_t idx;
                    if (inplace_page->search(key, hash_val, seed_hash, idx)) {
                        auto& entry = inplace_page->entries[idx];
                        process_value(inplace_page->entries[idx].kv, [this, idx, &entry](){
                            mark_dirty();
                            if (need_wal()) {
                                xl_bm25_add_data xl_rec(
                                    HEADER_SIZE + ie_offset + idx * sizeof(entry), sizeof(entry));
                                Bm25XLogAddData(_buf_cache.get_buffer(), _buf_cache.get_row_page(),
                                                xl_rec);
                            }
                        });
                        res = true;
                    } else {
                        res = false;
                    }
                    unlock_buffer();
                } goto exit_forloop;
                case HashEntryType::ExtensiveHashOneB1:
                    Assume(use_hash_one_b1);
                case HashEntryType::ExtensiveHashOneB2:
                    Assume(use_hash_one_b2);
                case HashEntryType::ExtensiveHashOne: {
                    const auto type = entry->get_type();
                    const size_t one_base_size = type == HashEntryType::ExtensiveHashOneB1 ?
                        hash_one_base_b1_size : (type == HashEntryType::ExtensiveHashOneB2 ?
                        hash_one_base_b2_size : hash_one_base_size);
                    _one_arr.emplace(get_relation(), entry->blkno, false);
                    const size_t idx = hash_combine(hash_val, entry->get_seed()) % one_base_size;
                    auto *new_entry = _one_arr->get_nolock(idx);
                    _one_arr->template lock_buffer<false>();
                    if (entry->get_type() != type) {
                        _one_arr->unlock_buffer();
                        continue;
                    }
                    hold_one_arr_lock = true;
                    entry = new_entry;
                } break;
                case HashEntryType::ExtensiveHashTen: {
                    _ten_arr.emplace(get_relation(), entry->blkno, false);
                    const size_t idx =
                        hash_combine(hash_val, entry->get_seed()) % hash_ten_base_size;
                    entry = _ten_arr->get_nolock(idx);
                } break;
                default:
                    ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                                    errmsg("Invalid HashTable entry type %hu", entry->get_type_u8()),
                                    errhint("Index may be corrupted.")));
            }
        }
        ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                        errmsg("Deadloop in hashmap iteration, potential index corruption.")));
exit_forloop:
        if (hold_one_arr_lock) {
            _one_arr->unlock_buffer();
        }
        return res;
    }

    template <bool read_only, bool do_erase, class F>
    size_t visit_internal(F &f)
    {
        static_assert((read_only || !do_erase) || IS_INVOCABLE_R(F, bool, kv_base &, bool &),
                      "Incorrect F type for erase_if");
        static_assert((!read_only || !do_erase) || IS_INVOCABLE_R(F, bool, const kv_base &),
                      "Incorrect F type for erase_cif");
        static_assert((read_only || do_erase) || IS_INVOCABLE_R(F, bool, kv_base &),
                      "Incorrect F type for visit");
        static_assert((!read_only || do_erase) || IS_INVOCABLE(F, const kv_base &),
                      "Incorrect F type for cvisit");
        size_t res = 0;
        for (size_t i = 0; i < base_size; ++i) {
            HashEntry *entry = _base.get_nolock(i);
            res += visit_internal<read_only, do_erase, F>(entry, _base.get_buffer_cache(), f);
        }
        return res;
    }

    template <bool read_only, class F>
    size_t visit_internal(F &f)
    {
        static_assert(read_only || IS_INVOCABLE_R(F, bool, kv_base &),
                      "Incorrect F type for visit");
        static_assert(!read_only || IS_INVOCABLE(F, const kv_base &),
                      "Incorrect F type for cvisit");
        size_t res = 0;
        for (size_t i = 0; i < base_size; ++i) {
            HashEntry *entry = _base.get_nolock(i);
            res += visit_internal<read_only, false, F>(entry, _base.get_buffer_cache(), f);
        }
        return res;
    }

    template <bool read_only, class F>
    size_t erase_internal(F &f)
    {
        static_assert(read_only || IS_INVOCABLE_R(F, bool, kv_base &, bool &),
                      "Incorrect F type for erase_if");
        static_assert(!read_only || IS_INVOCABLE_R(F, bool, const kv_base &),
                      "Incorrect F type for erase_cif");
        size_t res = 0;
        for (size_t i = 0; i < base_size; ++i) {
            HashEntry *entry = _base.get_nolock(i);
            res += visit_internal<read_only, true, F>(entry, _base.get_buffer_cache(), f);
        }
        return res;
    }

    template <bool read_only, bool do_erase, class F>
    size_t visit_internal(HashEntry *entry, BufferCache &entry_holder, F &f)
    {
#if __cplusplus >= 201703L
        using process_type = std::conditional_t<read_only, const kv_base &, kv_base &>;
#else
        using process_type = kv_base &; /* I hate c++ :( */
#endif
        size_t res = 0;
        HashEntryType type;
retry:
        type = entry->get_type();
        switch (type) {
            case HashEntryType::ExtensiveHashOneB1:
                Assume(use_hash_one_b1);
            case HashEntryType::ExtensiveHashOneB2:
                Assume(use_hash_one_b2);
            case HashEntryType::ExtensiveHashOne: {
                const size_t one_base_size = type == HashEntryType::ExtensiveHashOneB1 ?
                    hash_one_base_b1_size : (type == HashEntryType::ExtensiveHashOneB2 ?
                    hash_one_base_b2_size : hash_one_base_size);
                _one_arr.emplace(get_relation(), entry->blkno, need_wal());
                _one_arr->get_nolock(0);
                _one_arr->template lock_buffer<do_erase>();
                if (entry->get_type() != type) {
                    _one_arr->unlock_buffer();
                    goto retry;
                }
                size_t count = 0;
                for (size_t i = 0; i < one_base_size; ++i) {
                    entry = _one_arr->get_nolock(i);
                    auto etype = entry->get_type();
                    CONSTEXPR_IF(do_erase) {
                        CONSTEXPR_IF(read_only) {
                            const auto f_wc = [&f, &count](process_type &kv) -> bool {
                                ++count;
                                return run_helper(f, kv);
                            };
                            res += visit_internal_helper<read_only, do_erase>(entry, etype, f_wc);
                        } else {
                            const auto f_wc = [&f, &count](process_type &kv, bool &m) -> bool {
                                ++count;
                                return run_helper(f, kv, m);
                            };
                            res += visit_internal_helper<read_only, do_erase>(entry, etype, f_wc);
                        }
                    } else {
                        res += visit_internal_helper<read_only, do_erase, F>(entry, etype, f);
                    }
                }
                CONSTEXPR_IF(do_erase) {
                    size_t rem_size = count - res;
                    if (rem_size < max_inplace_list_size) {
                        /* try to shrink to inplace */
                    } else if (use_hash_one_b1 && type != HashEntryType::ExtensiveHashOneB1 &&
                               rem_size < hash_one_base_b1_size * max_inplace_list_size / 2) {
                        /* try to shrink to B1 */
                    } else if (use_hash_one_b2 && type == HashEntryType::ExtensiveHashOne &&
                               rem_size < hash_one_base_b2_size * max_inplace_list_size / 2) {
                        /* try to shrink to B2 */
                    }
                }
                _one_arr->unlock_buffer();
            } break;
            case HashEntryType::ExtensiveHashTen: {
                _ten_arr.emplace(get_relation(), entry->blkno, need_wal());
                for (size_t i = 0; i < hash_ten_base_size; ++i) {
                    entry = _ten_arr->get_nolock(i);
                    res += visit_internal<read_only, do_erase, F>(
                        entry, _ten_arr->get_buffer_cache(), f);
                }
                /* we don't shrink ten arr */
            } break;
            default:
                res += visit_internal_helper<read_only, do_erase, F>(entry, type, f);
        }
        return res;
    }

    template <bool read_only, bool do_erase, class F>
    size_t visit_internal_inplace(InplaceEntryPage inplace_page, F &f)
    {
        size_t res = 0;
        CONSTEXPR_IF(!do_erase) {
            res += inplace_page->nentry;
        }
        const auto end = inplace_page->end();
        size_t entry_idx = 0;
        for (auto it = inplace_page->begin(); it != end; ++it, ++entry_idx) {
            CONSTEXPR_IF(!read_only) {
                CONSTEXPR_IF(do_erase) {
                    bool modified;
                    bool erased = run_helper(f, it->kv, modified);
                    if (erased) {
                        inplace_page->erase(it.idx());
                        ++res;
                        mark_dirty();
                    } else if (modified) {
                        mark_dirty();
                    }
                    if (need_wal()) {
                        if (erased) {   /* it.idx() erased */
                            xl_bm25_insert_inplace_entry xl_rec(ie_offset - ieb_offset,
                                HEADER_SIZE + ie_offset + entry_idx * sizeof(PlainEntry),
                                HEADER_SIZE + ieb_offset, sizeof(PlainEntry), inplace_page->nentry);
                            Bm25XLogInsertInplaceEntry(_buf_cache.get_buffer(),
                                                        _buf_cache.get_row_page(), xl_rec);

                        } else if (modified) {  /* it.idx() overwriten */
                            xl_bm25_add_data xl_rec(
                                HEADER_SIZE + ie_offset + entry_idx * sizeof(PlainEntry),
                                sizeof(PlainEntry));
                            Bm25XLogAddData(_buf_cache.get_buffer(),
                                            _buf_cache.get_row_page(), xl_rec);
                        }
                    }
                } else if (run_helper(f, it->kv)) {
                    mark_dirty();
                    if (need_wal()) {   /* it.idx() overwriten */
                        xl_bm25_add_data xl_rec(
                            HEADER_SIZE + ie_offset + entry_idx * sizeof(PlainEntry),
                            sizeof(PlainEntry));
                        Bm25XLogAddData(_buf_cache.get_buffer(), _buf_cache.get_row_page(), xl_rec);
                    }
                }
            } else CONSTEXPR_IF(do_erase) {
                bool erased = run_helper(f, it->kv);
                if (!erased) {
                    continue;
                }
                inplace_page->erase(it.idx());
                mark_dirty();
                if (need_wal()) {
                    xl_bm25_insert_inplace_entry xl_rec(
                        HEADER_SIZE + ieb_offset, ie_offset - ieb_offset,
                        HEADER_SIZE + ie_offset + entry_idx * sizeof(PlainEntry),
                        sizeof(PlainEntry), inplace_page->nentry);
                    Bm25XLogInsertInplaceEntry(_buf_cache.get_buffer(),
                                                _buf_cache.get_row_page(), xl_rec);
                }
                ++res;
            } else {
                run_helper(f, it->kv);
                mark_dirty();
                if (need_wal()) {
                    xl_bm25_add_data xl_rec(
                        HEADER_SIZE + ie_offset + entry_idx * sizeof(PlainEntry),
                        sizeof(PlainEntry));
                    Bm25XLogAddData(_buf_cache.get_buffer(), _buf_cache.get_row_page(), xl_rec);
                }
            }
        }
        return res;
    }

    template <bool read_only, bool do_erase, class F>
    size_t visit_internal_plain(PlainEntryPage page, F &f)
    {
        size_t res = 0;
        CONSTEXPR_IF(!do_erase) {
            res += page->nentry;
        }
        for (uint16 i = page->nentry; i > 0; --i) {
            const uint16 idx = i - 1u;
            CONSTEXPR_IF(!read_only) {
                CONSTEXPR_IF(do_erase) {
                    bool modified;
                    bool erased = run_helper(f, page->entries[idx].kv, modified);
                    if (erased) {
                        page->remove(idx);
                        ++res;
                        mark_dirty();
                    } else if (modified) {
                        mark_dirty();
                    }
                    if (need_wal()) {
                        if (erased) {           /* entry[idx] erased */
                            if ((page->nentry - idx) == 0) {
                                xl_bm25_add_data xl_rec(
                                    HEADER_SIZE + offsetof(PlainEntryPageData, nentry),
                                    sizeof(uint16));
                                Bm25XLogAddData(_buf_cache.get_buffer(),
                                                _buf_cache.get_row_page(), xl_rec);
                            } else {
                                xl_bm25_insert_entry xl_rec(
                                    HEADER_SIZE + pe_offset + idx * sizeof(PlainEntry),
                                    sizeof(PlainEntry) * (page->nentry - idx),
                                    page->nentry);
                                Bm25XLogInsertEntry(_buf_cache.get_buffer(),
                                                    _buf_cache.get_row_page(), xl_rec);
                            }
                        } else if (modified) {  /* entry[idx] overwriten */
                            xl_bm25_add_data xl_rec(
                                HEADER_SIZE + pe_offset + idx * sizeof(PlainEntry),
                                sizeof(PlainEntry));
                            Bm25XLogAddData(_buf_cache.get_buffer(),
                                            _buf_cache.get_row_page(), xl_rec);
                        }
                    }
                } else if (run_helper(f, page->entries[idx].kv)) {
                    mark_dirty();
                    if (need_wal()) {   /* entry[idx] overwriten */
                        xl_bm25_add_data xl_rec(
                            HEADER_SIZE + pe_offset + idx * sizeof(PlainEntry), sizeof(PlainEntry));
                        Bm25XLogAddData(_buf_cache.get_buffer(), _buf_cache.get_row_page(), xl_rec);
                    }
                }
            } else CONSTEXPR_IF(do_erase) {
                bool erased = run_helper(f, page->entries[idx].kv);
                if (!erased) {
                    continue;
                }
                page->remove(idx);
                mark_dirty();
                if (need_wal()) {
                    if ((page->nentry - idx) == 0) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
                        xl_bm25_add_data xl_rec(
                            HEADER_SIZE + offsetof(PlainEntryPageData, nentry),
                            sizeof(uint16));
#pragma GCC diagnostic pop
                        Bm25XLogAddData(_buf_cache.get_buffer(), _buf_cache.get_row_page(), xl_rec);
                    } else {
                        xl_bm25_insert_entry xl_rec(
                            HEADER_SIZE + pe_offset + idx * sizeof(PlainEntry),
                            sizeof(PlainEntry) * (page->nentry - idx), page->nentry);
                        Bm25XLogInsertEntry(_buf_cache.get_buffer(),
                                            _buf_cache.get_row_page(), xl_rec);
                    }
                }
                ++res;
            } else {
                run_helper(f, page->entries[idx].kv);
                mark_dirty();
                if(need_wal()) {
                    xl_bm25_add_data xl_rec(HEADER_SIZE + pe_offset + idx * sizeof(PlainEntry),
                                            sizeof(PlainEntry));
                    Bm25XLogAddData(_buf_cache.get_buffer(), _buf_cache.get_row_page(), xl_rec);
                }
            }
        }
        return res;
    }

    template <bool read_only, bool do_erase, class F>
    size_t visit_internal_helper(HashEntry *entry, HashEntryType type, F &f)
    {
        constexpr bool modify_table = !read_only || do_erase;
        CHECK_FOR_INTERRUPTS();
        size_t res = 0;
retry:
        switch (type) {
            case HashEntryType::Unused:
            case HashEntryType::None:
                /* we don't check concurrency for none entry */
                return 0;
            case HashEntryType::Plain: {
                load_buffer(entry->blkno);
                lock_buffer<modify_table>();
                auto cur_type = entry->get_type();
                if (cur_type != HashEntryType::Plain) {
                    unlock_buffer();
                    type = cur_type;
                    goto retry;
                }
                res += visit_internal_plain<read_only, do_erase, F>(get_pe_page(), f);
                unlock_buffer();
            } break;
            case HashEntryType::InplaceHash: {
                Assume(use_inplace_hash);
                load_buffer(entry->blkno);
                lock_buffer<modify_table>();
                auto cur_type = entry->get_type();
                if (cur_type != HashEntryType::InplaceHash) {
                    unlock_buffer();
                    type = cur_type;
                    goto retry;
                }
                res += visit_internal_inplace<read_only, do_erase, F>(get_ip_page(), f);
                unlock_buffer();
                /* no need to do downgrade, we have no space to save */
            } break;
            case HashEntryType::ExtensiveHashOneB2:
            case HashEntryType::ExtensiveHashOneB1:
            case HashEntryType::ExtensiveHashOne:
            case HashEntryType::ExtensiveHashTen:
                __builtin_unreachable();
            default:
                ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                                errmsg("Invalid HashTable entry type %hu", entry->get_type_u8()),
                                errhint("Index may be corrupted.")));
        }
        return res;
    }

    template <bool read_only, class F>
    bool erase_internal(const K &key, F &f)
    {
        elog(ERROR, "erase by key not implemented.");
        return false;
    }

    static uint16 generate_seed()
    {
        uint16 res;
        do {
            res = uint16(random()) & HashEntry::EntrySeedMask;
        } while (!res);
        return res;
    }

    Buffer get_new_page(BlockNumber &blkno, bool &page_is_new)
    {
        blkno = GetFreeIndexPage(get_relation());
        Buffer buf;
        if (BlockNumberIsValid(blkno)) {
            buf = ReadBuffer(get_relation(), blkno);
            page_is_new = false;
        } else {
            LockRelationForExtension(get_relation(), ExclusiveLock);
            buf = ReadBuffer(get_relation(), P_NEW);
            UnlockRelationForExtension(get_relation(), ExclusiveLock);
            blkno = BufferGetBlockNumber(buf);
            page_is_new = true;
        }
        return buf;
    }

    void init_plain_page(Buffer buf)
    {
        Page page = BufferGetPage(buf);
        PageInit(page, BLCKSZ, sizeof(HTEntryOpaqueData));
        ((PageHeader)page)->pd_lower = ((PageHeader)page)->pd_upper;
        get_pe_page(page)->init();
        ((HTEntryOpaque)PageGetSpecialPointer(page))->init();
    }

    void init_inplace_page(Buffer buf)
    {
        Page page = BufferGetPage(buf);
        PageInit(page, BLCKSZ, sizeof(HTEntryOpaqueData));
        ((PageHeader)page)->pd_lower = ((PageHeader)page)->pd_upper;
        get_ip_page(page)->init();
        ((HTEntryOpaque)PageGetSpecialPointer(page))->init();
    }

    size_t extract_data_from_plain(PlainEntryPage page, kv_base **kv_pairs, uint32 **hashes)
    {
        const uint16 npair = page->nentry;
        if (npair == 0) {
            return 0;
        }
        *kv_pairs = (kv_base *)palloc(sizeof(kv_base) * npair);
        *hashes = (uint32 *)palloc(sizeof(uint32) * npair);
        if (plain_entry_use_hash) {
            for (uint16 i = 0; i < npair; ++i) {
                (*hashes)[i] = get_hash(page->entries[i]);
                new (*kv_pairs + i) kv_base(std::move(page->entries[i].kv));
            }
        } else {
            memcpy(*kv_pairs, page->entries, sizeof(kv_base) * npair);
            for (uint16 i = 0; i < npair; ++i) {
                (*hashes)[i] = get_hash((*kv_pairs)[i].k);
            }
        }
        return npair;
    }

    size_t extract_data_from_inplace(InplaceEntryPage page,
                                     kv_base **kv_pairs, uint32 **hashes)
    {
        size_t npair = page->nentry;
        if (npair == 0) {
            *kv_pairs = NULL;
            *hashes = NULL;
            return 0;
        }
        kv_base *temp_kv_pairs = (kv_base *)palloc(sizeof(kv_base) * npair);
        uint32 *temp_hashes = (uint32 *)palloc(sizeof(uint32) * npair);
        npair = 0;
        for (size_t i = 0; i < max_inplace_list_size; ++i) {
            if (page->bitmap[i]) {
                temp_hashes[npair] = get_hash(page->entries[i]);
                new (temp_kv_pairs + npair) kv_base(std::move(page->entries[i].kv));
                ++npair;
            }
        }
        Assert(npair == page->nentry);
        *kv_pairs = temp_kv_pairs;
        *hashes = temp_hashes;
        return npair;
    }

    bool try_setup_inplace(Page page, kv_base *kv_pairs, uint32 *hashes, uint16 npair, uint16 seed)
    {
        if (npair > max_inplace_entry_load) {
            return false;
        }

        PageInit(page, BLCKSZ, sizeof(HTEntryOpaqueData));
        ((PageHeader)page)->pd_lower = ((PageHeader)page)->pd_upper;
        ((HTEntryOpaque)PageGetSpecialPointer(page))->init();
        InplaceEntryPage ipage = get_ip_page(page);
        ipage->init();
        for (uint16 i = 0; i < npair; ++i) {
            Assert(hashes[i] == get_hash(kv_pairs[i].k));
            const uint32 seed_hash = hash_combine(hashes[i], seed);
            size_t offset;
            if (ipage->search(kv_pairs[i].k, hashes[i], seed_hash, offset)) {
                ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                                errmsg("Duplicate key during inplace hash setup.")));
                return false;
            }
            ipage->insert(offset, hashes[i], std::move(kv_pairs[i]));
        }
        return true;
    }

    bool try_setup_onehash(Page page, kv_base *kv_pairs, uint32 *hashes, uint16 npair, uint16 seed)
    {
        constexpr size_t base_size = use_hash_one_b1 ?
            hash_one_base_b1_size :
            (use_hash_one_b2 ? hash_one_base_b2_size : hash_one_base_size);
        PageInit(page, BLCKSZ, sizeof(DiskArrayOpaqueData));
        ((PageHeader)page)->pd_lower = ((PageHeader)page)->pd_upper;
        ((DiskArrayOpaque)PageGetSpecialPointer(page))->init(ann_helper::GET_TYPE_ID(HashEntry));
        auto ipage = hash_one_arr::get_da_page(page);
        ipage->init();
        for (size_t i = 0; i < base_size; ++i) {
            new (ipage->data + i) HashEntry();
        }
        Vector<Vector<kv_base>> kv_vecs(base_size);
        Vector<Vector<uint32>> hash_vecs(base_size);
        kv_vecs.resize(base_size);
        hash_vecs.resize(base_size);
        for (uint16 i = 0; i < npair; ++i) {
            const uint32 idx = hash_combine(hashes[i], seed) % base_size;
            kv_vecs[idx].push_back(std::move(kv_pairs[i]));
            hash_vecs[idx].push_back(hashes[i]);
        }
        size_t nentry = 0;
        for (size_t i = 0; i < base_size; ++i) {
            Assert(kv_vecs[i].size() == hash_vecs[i].size());
            nentry += kv_vecs[i].size();
            if (!insert_to_entry(kv_vecs[i], hash_vecs[i], ipage->data[i])) {
                return false;
            }
        }
        Assert(nentry == npair);
        ann_helper::optional_destroy(kv_vecs);
        ann_helper::optional_destroy(hash_vecs);
        return true;
    }

    bool insert_to_entry(const Vector<kv_base> &kvs, const Vector<uint32> &hashes, HashEntry &entry)
    {
        if (kvs.empty()) {
            if (entry.get_type() != HashEntryType::None) {
                entry.set_type(HashEntryType::None);
                const BlockNumber blkno = entry.blkno;
                if (BlockNumberIsValid(blkno)) {
                    entry.blkno = InvalidBlockNumber;
                    /* we don't apply lock for reuse/read conflict yet */
                    RecordFreeIndexPage(get_relation(), blkno);
                }
            }
            return true;
        }

        const size_t npair = kvs.size();
        if (npair <= actual_max_list_size) {
            Buffer buf;
            bool page_is_new;
            if (BlockNumberIsValid(entry.blkno)) {
                buf = ReadBuffer(get_relation(), entry.blkno);
                page_is_new = false;
            } else {
                buf = get_new_page(entry.blkno, page_is_new);
            }
            entry.set_type(HashEntryType::Plain);
            init_plain_page(buf);
            PlainEntryPage page = get_pe_page(BufferGetPage(buf));
            for (size_t i = 0; i < npair; ++i) {
                page->entries[i].set_hash(hashes[i]);
                new (&page->entries[i].kv) kv_base(std::move(kvs[i]));
            }
            page->nentry = npair;
            MarkBufferDirty(buf);
            if (need_wal()) {
                if (page_is_new) {
                    Bm25XLogAppendPage(buf, BufferGetPage(buf));
                } else {
                    Bm25XLogInitPage(buf, BufferGetPage(buf));
                }
            }
            ReleaseBuffer(buf);
            return true;
        }

        if (npair <= max_inplace_entry_load) {
            const uint16 seed = generate_seed();
            entry.set_type(HashEntryType::InplaceHash);
            entry.set_seed(seed);
            Buffer buf;
            bool page_is_new;
            if (BlockNumberIsValid(entry.blkno)) {
                buf = ReadBuffer(get_relation(), entry.blkno);
                page_is_new = false;
            } else {
                buf = get_new_page(entry.blkno, page_is_new);
            }
            init_inplace_page(buf);
            InplaceEntryPage ipage = get_ip_page(BufferGetPage(buf));
            for (size_t i = 0; i < npair; ++i) {
                size_t offset;
                const uint32 seed_hash = hash_combine(hashes[i], seed);
                if (ipage->search(kvs[i].k, hashes[i], seed_hash, offset)) {
                    ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                                    errmsg("Duplicate key during inplace hash setup.")));
                }
                ipage->insert(offset, hashes[i], std::move(kvs[i]));
            }
            Assert(ipage->size() == npair);
            MarkBufferDirty(buf);
            if (need_wal()) {
                if (page_is_new) {
                    Bm25XLogAppendPage(buf, BufferGetPage(buf));
                } else {
                    Bm25XLogInitPage(buf, BufferGetPage(buf));
                }
            }
            ReleaseBuffer(buf);
            return true;
        }
        return false;
    }

    template <HashEntryType upgrade_from, bool allow_upgrade_to_arr>
    bool upgrade(BufferCache &parent_buf, HashEntry *parent_entry, size_t parent_offset)
    {
        uint16 new_seed = 0;
        HashEntryType new_type = HashEntryType::None;
        uint32 *hashes = NULL;
        kv_base *kv_pairs = NULL;
        uint16 npair;
        switch (upgrade_from) {
            case HashEntryType::None: {
                parent_buf.template lock_buffer<true>();
                if (parent_entry->get_type() != HashEntryType::None) {
                    parent_buf.unlock_buffer();
                    return false;
                }
                bool page_is_new;
                Buffer buf = get_new_page(parent_entry->blkno, page_is_new);
                init_plain_page(buf);
                MarkBufferDirty(buf);
                if (need_wal()) {
                    if (page_is_new) {
                        Bm25XLogAppendPage(buf, BufferGetPage(buf));
                    } else {
                        Bm25XLogInitPage(buf, BufferGetPage(buf));
                    }
                }
                ReleaseBuffer(buf);
                parent_entry->set_type(HashEntryType::Plain);
                parent_buf.mark_dirty();
                if (need_wal()) {
                    xl_bm25_add_data xl_rec(
                        HEADER_SIZE + dad_offset + sizeof(HashEntry) * parent_offset,
                        sizeof(HashEntry));
                    Bm25XLogAddData(parent_buf.get_buffer(), parent_buf.get_row_page(), xl_rec);
                }
                parent_buf.unlock_buffer();
            } return true;
            case HashEntryType::Plain: {
                if (!use_inplace_hash && !allow_upgrade_to_arr) {
                    return false;
                }
                npair = extract_data_from_plain(get_pe_page(), &kv_pairs, &hashes);
                new_seed = generate_seed();
                if (use_inplace_hash && npair <= max_inplace_entry_load &&
                    try_setup_inplace(_buf_cache.get_row_page(), kv_pairs, hashes, npair,
                                      new_seed)) {
                    mark_dirty();
                    if (need_wal()) {
                        Bm25XLogInitPage(_buf_cache.get_buffer(), _buf_cache.get_row_page());
                    }
                    new_type = HashEntryType::InplaceHash;
                    pfree_ext(hashes);
                    pfree_ext(kv_pairs);
                    break;
                }
                if (!allow_upgrade_to_arr) {
                    pfree_ext(hashes);
                    pfree_ext(kv_pairs);
                    return false;
                }
            } /* fall through */
            case HashEntryType::InplaceHash: {
                constexpr bool from_plain = upgrade_from == HashEntryType::Plain;
                if (!from_plain) {
                    npair = extract_data_from_inplace(get_ip_page(), &kv_pairs, &hashes);
                    Assert(npair == max_inplace_entry_load);
                }
                new_seed = generate_seed();
                if (try_setup_onehash(_buf_cache.get_row_page(), kv_pairs, hashes, npair,
                                      new_seed)) {
                    pfree_ext(hashes);
                    pfree_ext(kv_pairs);
                    mark_dirty();
                    if (need_wal()) {
                        Bm25XLogInitPage(_buf_cache.get_buffer(), _buf_cache.get_row_page());
                    }
                    if (use_hash_one_b1) {
                        new_type = HashEntryType::ExtensiveHashOneB1;
                    } else if (use_hash_one_b2) {
                        new_type = HashEntryType::ExtensiveHashOneB2;
                    } else {
                        new_type = HashEntryType::ExtensiveHashOne;
                    }
                } else {
                    ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("Upgrade to one-hash array not implemented. "
                               "Index may become corrupted."),
                        errhint("Internal error.")));
                }
            } break;
            default:
                ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                                errmsg("Invalid upgrade_from type %hu", uint16(upgrade_from)),
                                errhint("Internal error.")));
        }

        /* modify parent */
        if (need_wal()) {
            parent_buf.template lock_buffer<true>();
        }
        parent_entry->set_type(new_type);
        if (new_seed != 0) {
            parent_entry->set_seed(new_seed);
        }
        parent_buf.mark_dirty();
        if (need_wal()) {
            xl_bm25_add_data xl_rec(HEADER_SIZE + dad_offset + sizeof(HashEntry) * parent_offset,
                sizeof(HashEntry));
            Bm25XLogAddData(parent_buf.get_buffer(), parent_buf.get_row_page(), xl_rec);
            parent_buf.unlock_buffer();
        }
        return true;
    }

    bool upgrade_one_hash_arr(BufferCache &parent_buf, BufferCache &&one_hash_arr_buf,
        HashEntry *&parent_entry, HashEntry *&one_hash_arr_entry, HashEntryType from_type,
        size_t &parent_entry_offset, size_t &one_hash_arr_entry_offset)
    {
        Assert(from_type == HashEntryType::ExtensiveHashOneB1 ||
               from_type == HashEntryType::ExtensiveHashOneB2 ||
               from_type == HashEntryType::ExtensiveHashOne);
        parent_buf.unlock_buffer();
        parent_buf.template lock_buffer<true>();
        if (one_hash_arr_entry->get_type() != from_type) {
            parent_buf.unlock_buffer();
            parent_buf.swap(one_hash_arr_buf);
            one_hash_arr_buf.destroy();
            parent_entry = one_hash_arr_entry;
            parent_entry_offset = one_hash_arr_entry_offset;
            one_hash_arr_entry = NULL;
            return false;
        }
        constexpr std::memory_order order = std::memory_order_relaxed;
        BlockNumber new_blkno = InvalidBlockNumber;
        const uint16 new_seed = generate_seed();
        const BlockNumber old_blkno = one_hash_arr_entry->blkno;
        auto *ipage = (hash_one_arr::page_type_ptr)parent_buf.get_page();
        Vector<Vector<kv_base>> kv_vecs;
        Vector<Vector<uint32>> hash_vecs;
        size_t base_size, new_size;
        HashEntryType new_type;
        switch (from_type) {
            case HashEntryType::ExtensiveHashOneB1: {
                base_size = hash_one_base_b1_size;
                new_size = hash_one_base_b2_size;
                new_type = HashEntryType::ExtensiveHashOneB2;
            } break;
            case HashEntryType::ExtensiveHashOneB2: {
                base_size = hash_one_base_b2_size;
                new_size = hash_one_base_size;
                new_type = HashEntryType::ExtensiveHashOne;
            } break;
            case HashEntryType::ExtensiveHashOne: {
                base_size = hash_one_base_size;
                new_size = hash_ten_base_size;
                new_type = HashEntryType::ExtensiveHashTen;
                /* 10-block will not be recollected so we always create a new one */
                new_blkno = hash_ten_arr::get_disk_array(
                    get_relation(), need_wal(), ann_helper::GET_TYPE_ID(HashEntry), MAIN_FORKNUM);
            } break;
            default:
                __builtin_unreachable();    /* keep compiler quiet */
        }
        /* it's ok for one_arr being casted as ten arr */
        hash_ten_arr new_arr(get_relation(), BlockNumberIsValid(new_blkno) ? new_blkno : old_blkno,
                             need_wal());
retry:
        kv_vecs.resize(new_size);
        hash_vecs.resize(new_size);
        for (size_t i = 0; i < base_size; ++i) {
            const auto &entry = ipage->data[i];
            if (entry.get_type(order) == HashEntryType::Plain) {
                Buffer buf = ReadBuffer(get_relation(), entry.blkno);
                PlainEntryPage page = get_pe_page(BufferGetPage(buf));
                for (uint16 j = 0; j < page->nentry; ++j) {
                    const uint32 hash_val = get_hash(page->entries[j]);
                    const uint32 idx = hash_combine(hash_val, new_seed) % new_size;
                    kv_vecs[idx].emplace_back(std::move(page->entries[j].kv));
                    hash_vecs[idx].push_back(hash_val);
                }
                ReleaseBuffer(buf);
            } else if (entry.get_type(order) == HashEntryType::InplaceHash) {
                Buffer buf = ReadBuffer(get_relation(), entry.blkno);
                InplaceEntryPage ipage = get_ip_page(BufferGetPage(buf));
                size_t counter = 0;
                for (size_t j = 0; j < max_inplace_list_size; ++j) {
                    if (!ipage->bitmap[j]) {
                        continue;
                    }
                    auto &entry = ipage->entries[j];
                    const uint32 hash_val = get_hash(entry);
                    const uint32 idx = hash_combine(hash_val, new_seed) % new_size;
                    kv_vecs[idx].emplace_back(std::move(entry.kv));
                    hash_vecs[idx].push_back(hash_val);
                    ++counter;
                }
                Assert(counter == ipage->size());
                ReleaseBuffer(buf);
            }
        }

        for (size_t i = base_size; i < new_size; ++i) {
            BufferCache new_arr_buf;
            size_t new_arr_offset;
            HashEntry *entry = BlockNumberIsValid(new_blkno) ?
                new_arr.get_nolock(i, new_arr_buf, new_arr_offset) :
                ipage->data + i;
            new (entry) HashEntry();
            if (insert_to_entry(kv_vecs[i], hash_vecs[i], *entry)) {
                if (BlockNumberIsValid(new_blkno)) {
                    new_arr.mark_dirty();
                    if (need_wal()) {
                        xl_bm25_add_data xl_rec(
                            HEADER_SIZE + dad_offset + sizeof(HashEntry) * new_arr_offset,
                            sizeof(HashEntry));
                        Bm25XLogAddData(new_arr_buf.get_buffer(), new_arr_buf.get_row_page(),
                                        xl_rec);
                    }
                } else {
                    parent_buf.mark_dirty();
                    if (need_wal()) {
                        xl_bm25_add_data xl_rec(HEADER_SIZE + dad_offset + sizeof(HashEntry) * i,
                            sizeof(HashEntry));
                        Bm25XLogAddData(parent_buf.get_buffer(), parent_buf.get_row_page(), xl_rec);
                    }
                }
                continue;
            }
            Assert(false); /* should not reach here now */
            if (from_type == HashEntryType::ExtensiveHashOne) {
                /* report error */
                return false;
            }
            if (from_type == HashEntryType::ExtensiveHashOneB1) {
                from_type = HashEntryType::ExtensiveHashOneB2;
                new_size = hash_one_base_b2_size;
            } else if (from_type == HashEntryType::ExtensiveHashOneB2) {
                from_type = HashEntryType::ExtensiveHashOne;
                new_size = hash_one_base_size;
            }
            kv_vecs.clear();
            hash_vecs.clear();
            goto retry;
        }
        for (size_t i = 0; i < base_size; ++i) {
            HashEntry *entry;
            BufferCache new_arr_buf;
            size_t new_arr_offset;
            if (BlockNumberIsValid(new_blkno)) {
                entry = new_arr.get_nolock(i, new_arr_buf, new_arr_offset);
                entry->blkno = ipage->data[i].blkno;
                entry->set_type(ipage->data[i].get_type(order), order);
            } else {
                entry = ipage->data + i;
            }
            if (insert_to_entry(kv_vecs[i], hash_vecs[i], *entry)) {
                if (BlockNumberIsValid(new_blkno)) {
                    new_arr.mark_dirty();
                    if (need_wal()) {
                        xl_bm25_add_data xl_rec(
                            HEADER_SIZE + dad_offset + sizeof(HashEntry) * new_arr_offset,
                            sizeof(HashEntry));
                        Bm25XLogAddData(new_arr_buf.get_buffer(), new_arr_buf.get_row_page(),
                                        xl_rec);
                    }
                } else {
                    parent_buf.mark_dirty();
                    if (need_wal()) {
                        xl_bm25_add_data xl_rec(
                            HEADER_SIZE + dad_offset + sizeof(HashEntry) * i, sizeof(HashEntry));
                        Bm25XLogAddData(parent_buf.get_buffer(), parent_buf.get_row_page(), xl_rec);
                    }
                }
                continue;
            }
            Assert(false); /* should not reach here now */
            if (from_type == HashEntryType::ExtensiveHashOne) {
                /* report error */
                return false;
            }
            if (from_type == HashEntryType::ExtensiveHashOneB1) {
                from_type = HashEntryType::ExtensiveHashOneB2;
                new_size = hash_one_base_b2_size;
            } else if (from_type == HashEntryType::ExtensiveHashOneB2) {
                from_type = HashEntryType::ExtensiveHashOne;
                new_size = hash_one_base_size;
            }
            kv_vecs.clear();
            hash_vecs.clear();
            goto retry;
        }
        new_arr.destroy();
        ann_helper::optional_destroy(kv_vecs);
        ann_helper::optional_destroy(hash_vecs);

        one_hash_arr_buf.template lock_buffer<true>();
        if (BlockNumberIsValid(new_blkno)) {
            START_CRIT_SECTION();
            one_hash_arr_entry->blkno = new_blkno;
        }
        one_hash_arr_entry->set_seed(new_seed);
        one_hash_arr_entry->set_type(new_type);
        if (BlockNumberIsValid(new_blkno)) {
            RecordFreeIndexPage(get_relation(), old_blkno);
            END_CRIT_SECTION();
        }
        one_hash_arr_buf.mark_dirty();
        if (need_wal()) {
            Assert(one_hash_arr_entry_offset != SIZE_MAX);
            xl_bm25_add_data xl_rec(
                HEADER_SIZE + dad_offset + sizeof(HashEntry) * one_hash_arr_entry_offset,
                sizeof(HashEntry));
            Bm25XLogAddData(one_hash_arr_buf.get_buffer(), one_hash_arr_buf.get_row_page(), xl_rec);
        }

        one_hash_arr_buf.unlock_buffer();
        parent_buf.unlock_buffer();
        parent_buf.swap(one_hash_arr_buf);
        one_hash_arr_buf.destroy();
        parent_entry = one_hash_arr_entry;
        parent_entry_offset = one_hash_arr_entry_offset;
        one_hash_arr_entry = NULL;
        return true;
    }

    void get_stats_internal(DiskHashTableStats &stats, const HashEntry *entry, size_t level)
    {
        const auto type = entry->get_type();
        switch (type) {
            case HashEntryType::None:
                ++stats.level_stats[level].none_slot_count;
                break;
            case HashEntryType::Plain:
                load_buffer(entry->blkno);
                lock_buffer<false>();
                if (type != entry->get_type()) {
                    unlock_buffer();
                    break;
                }
                stats.level_stats[level].plain_count += get_pe_page()->nentry;
                stats.nentry += get_pe_page()->nentry;
                unlock_buffer();
                ++stats.level_stats[level].plain_arr_count;
                ++stats.nblock;
                break;
            case HashEntryType::InplaceHash: {
                load_buffer(entry->blkno);
                lock_buffer<false>();
                if (type != entry->get_type()) {
                    unlock_buffer();
                    break;
                }
                const size_t inplace_count = get_ip_page()->nentry;
                unlock_buffer();
                stats.level_stats[level].inplace_count += inplace_count;
                ++stats.level_stats[level].inplace_hash_count;
                ++stats.nblock;
                stats.nentry += inplace_count;
            } break;
            case HashEntryType::ExtensiveHashOneB1:
            case HashEntryType::ExtensiveHashOneB2:
            case HashEntryType::ExtensiveHashOne: {
                ++stats.nblock;
                _one_arr.emplace(get_relation(), entry->blkno, false);
                const size_t new_level = level + 1ul;
                if (stats.level_stats.size() < new_level + 1ul) {
                    stats.level_stats.resize(new_level + 1ul);
                }
                _one_arr->get_nolock(0);
                _one_arr->template lock_buffer<false>();
                if (type != entry->get_type()) {
                    _one_arr->unlock_buffer();
                    break;
                }
                ++stats.level_stats[new_level].nentry;
                if (type == HashEntryType::ExtensiveHashOneB1) {
                    ++stats.level_stats[level].one_hash_arr_b1_count;
                    for (size_t i = 0; i < hash_one_base_b1_size; ++i) {
                        get_stats_internal(stats, _one_arr->get_nolock(i), new_level);
                    }
                } else if (type == HashEntryType::ExtensiveHashOneB2) {
                    ++stats.level_stats[level].one_hash_arr_b2_count;
                    for (size_t i = 0; i < hash_one_base_b2_size; ++i) {
                        get_stats_internal(stats, _one_arr->get_nolock(i), new_level);
                    }
                } else {
                    ++stats.level_stats[level].one_hash_arr_count;
                    for (size_t i = 0; i < hash_one_base_size; ++i) {
                        get_stats_internal(stats, _one_arr->get_nolock(i), new_level);
                    }
                }
                _one_arr->unlock_buffer();
            } break;
            case HashEntryType::ExtensiveHashTen: {
                stats.nblock += 10ul;
                ++stats.level_stats[level].ten_hash_arr_count;
                _ten_arr.emplace(get_relation(), entry->blkno, false);
                const size_t new_level = level + 1ul;
                if (stats.level_stats.size() < new_level + 1ul) {
                    stats.level_stats.resize(new_level + 1ul);
                }
                ++stats.level_stats[new_level].nentry;
                for (size_t i = 0; i < hash_ten_base_size; ++i) {
                    get_stats_internal(stats, _ten_arr->get_nolock(i), new_level);
                }
            } break;
            default:
                ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
                                errmsg("Unknown entry type %hu", entry->get_type_u8()),
                                errhint("Index is corrupted.")));
        }
    }
#undef run_helper
};
} /* namespace disk_container */

#endif /* CONTAINER_DISK_HASHTABLE_H */
