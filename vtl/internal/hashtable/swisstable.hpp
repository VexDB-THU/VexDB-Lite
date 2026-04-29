#ifndef CONTAINER_SWISSTABLE_H
#define CONTAINER_SWISSTABLE_H

#include <functional>
#if defined(__x86_64__)
#include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <vtl/allocator>
#include <vtl/pair>
#include <vtl/internal/expr.hpp>

#if defined(VTL_TESTING) || defined(VTL_BENCHMARK)
#define VTL_TEST_REQUIRE_MOCKS
#include "mock/c.h"
#include "mock/utils/dynahash.h"
#include "mock/utils/palloc.h"
#include "mock/utils/elog.h"
#include "mock/utils/memtrace.h"
#elif defined(PG_VEXDB_TARGET_DUCK)
#else
#include "utils/palloc.h"
#endif

namespace impl {
template <typename T, typename = void>
struct DefaultHasher {
    uint32 operator()(const T &key) const noexcept {
        return static_cast<uint64>(std::hash<T>()(key));
    }
};

template <typename T>
struct DefaultHasher<T, typename std::enable_if<std::is_integral<T>::value>::type> {
    uint32 operator()(const T &key) const noexcept {
        constexpr uint64 prime = 0x9e3779b97f4a7c15;
        uint64 k = static_cast<uint64>(key);
        k ^= k >> 33;
        k *= prime;
        k ^= k >> 29;
        return k;
    }
};

template <>
struct DefaultHasher<ItemPointerData> {
    uint32 operator()(const ItemPointerData& key) const noexcept {
        uint64 combined = static_cast<uint64>(key.ip_blkid.bi_hi) << 32 |
                          static_cast<uint64>(key.ip_blkid.bi_lo) << 16 |
                          static_cast<uint64>(key.ip_posid);
        constexpr uint64 prime = 0x9e3779b97f4a7c15;
        uint64 k = combined;
        k ^= k >> 33;
        k *= prime;
        k ^= k >> 29;
        return static_cast<uint32>(k);
    }
};
template <typename T>
struct DefaultEqual : public std::equal_to<T> {};

template <>
struct DefaultEqual<ItemPointerData> {
    bool operator()(const ItemPointerData &a, const ItemPointerData &b) const {
        return ItemPointerEquals(const_cast<ItemPointerData *>(&a),
                                        const_cast<ItemPointerData *>(&b));
    }
};

template <class Param, class Hasher, class KeyEqual, class Allocator>
class HashTableImpl {
    using entry_base = typename Param::data_type;
    using key_type = typename Param::key_type;
    using self_type = HashTableImpl<Param, Hasher, KeyEqual, Allocator>;
    enum class Ctrl : int8 {
        EMPTY = -128, // 0b1000 0000
        DELETED = -2, // 0b1111 1110
        END = -1, // 0b1111 1111
    };

    constexpr static uint32 invalid_idx = UINT32_MAX;
    constexpr static uint32 group_size = 16;
    constexpr static uint32 default_capacity = 15u;
    constexpr static float max_load_factor = 0.875;

    struct _Store : public Hasher, public KeyEqual, public Allocator {
        uint32 hash_func(const key_type &k) { return Hasher::operator()(k); }
        bool cmp(const key_type &k1, const key_type &k2) { return KeyEqual::operator()(k1, k2); }

        _Store(const Hasher &h, const KeyEqual &ke, const Allocator &a)
            : Hasher(h), KeyEqual(ke), Allocator(a) {}
        _Store(const _Store &) = default;
        _Store(_Store &&) = default;
        _Store &operator=(const _Store &) = default;
        _Store &operator=(_Store &&) = default;
        friend void swap(_Store &a, _Store &b) noexcept {
            using std::swap;
            swap(static_cast<Hasher &>(a), static_cast<Hasher &>(b));
            swap(static_cast<KeyEqual &>(a), static_cast<KeyEqual &>(b));
            swap(static_cast<Allocator &>(a), static_cast<Allocator &>(b));
            swap(a._available, b._available);
        }
        uint32 _available{0};
    };

    Ctrl *_ctrl;
    entry_base *_data;
    mutable _Store _store;
    uint32 _capacity{0}; // must be 2^n - 1, thus lower bit is all 1, eg:127 = 0x0000 FFFF.

public:
    using data_type = entry_base;
    using allocator_type = Allocator;

    HashTableImpl(uint32 capacity = default_capacity, const Hasher &hasher = Hasher(),
              const KeyEqual &equal = KeyEqual(), const Allocator &alloc = Allocator())
        :  _store(hasher, equal, alloc)
    {
        if (capacity == 0) {
            _capacity = default_capacity;
        } else {
            capacity = capacity / max_load_factor;
            uint32 v = capacity + 1;
            uint32 n = v - 1;
            n |= n >> 1;
            n |= n >> 2;
            n |= n >> 4;
            n |= n >> 8;
            n |= n >> 16;
            uint32 power = n + 1;
            _capacity = power - 1;
        }
        uint32 ctrl_size = sizeof(Ctrl) * (_capacity + group_size);
        uint32 align_ctrl_size = align(ctrl_size, group_size);
        uint32 slot_size = sizeof(entry_base) * _capacity;
        uint32 all_size = align_ctrl_size + align(slot_size, group_size);
        _store._available = _capacity * max_load_factor;
        char *p = get_rebind_allocator<char>().allocate(all_size);
        _ctrl = (Ctrl *)p;
        _data = (entry_base *)(p + align_ctrl_size);
        memset(_ctrl, (int8)Ctrl::EMPTY, ctrl_size);
        _ctrl[_capacity] = Ctrl::END;
    }

    HashTableImpl(const HashTableImpl &other) : _store(other._store), _capacity(other._capacity) {
        uint32 ctrl_size = sizeof(Ctrl) * (_capacity + group_size);
        uint32 align_ctrl_size = align(ctrl_size, group_size);
        uint32 slot_size = sizeof(entry_base) * _capacity;
        uint32 all_size = align_ctrl_size + align(slot_size, group_size);
        char *p = get_rebind_allocator<char>().allocate(all_size);
        _ctrl = (Ctrl *)p;
        _data = (entry_base *)(p + align_ctrl_size);
        memcpy(_ctrl, other._ctrl, ctrl_size);
        for (uint32 i = 0; i < _capacity; ++i) {
            if (other._ctrl[i] != Ctrl::EMPTY) {
                new (&_data[i]) entry_base(other._data[i]);
            }
        }
    }
 
    HashTableImpl(HashTableImpl &&other) noexcept
        : _ctrl(other._ctrl),
          _data(other._data),
          _store(std::move(other._store)),
          _capacity(other._capacity)
    {
        other._ctrl = NULL;
        other._data = NULL;
        other._store._available = 0;
        other._capacity = 0;
    }

    HashTableImpl &operator=(const HashTableImpl &) = delete;
    HashTableImpl &operator=(HashTableImpl &&other)
    {
        if (this != &other) {
            destroy();
            swap(other);
        }
        return *this;
    }

    void swap(HashTableImpl &other) noexcept {
        using std::swap;
        swap(_ctrl, other._ctrl);
        swap(_data, other._data);
        swap(_store, other._store);
        swap(_capacity, other._capacity);
    }

    void clear() {
        destroy_used_slots();
        uint32 ctrl_size = sizeof(Ctrl) * (_capacity + group_size);
        memset(_ctrl, (int8)Ctrl::EMPTY, ctrl_size);
        _ctrl[_capacity] = Ctrl::END;
        _store._available = _capacity * max_load_factor;
    }
    
    void clear_no_destroy() {
        uint32 ctrl_size = sizeof(Ctrl) * (_capacity + group_size);
        memset(_ctrl, (int8)Ctrl::EMPTY, ctrl_size);
        _ctrl[_capacity] = Ctrl::END;
        _store._available = _capacity * max_load_factor;
    }

    size_t size() const { return (uint32)(_capacity * max_load_factor) - _store._available; }
    size_t capacity() const { return (uint32)(_capacity * max_load_factor); }
    static constexpr size_t max_size()
        { return Allocator::max_size() > UINT32_MAX ? UINT32_MAX : Allocator::max_size(); }
    bool empty() const { return size() == 0; }

    FORCE_INLINE const Allocator &get_allocator() const { return _store; }
    FORCE_INLINE Allocator &get_allocator() { return _store; }
    template <class U>
    typename Allocator::template rebind<U>::other get_rebind_allocator() const {
        using rebound_alloc_type = typename Allocator::template rebind<U>::other;
        return rebound_alloc_type(get_allocator());
    }

    template <typename reference, typename pointer>
    struct iterator_template {
        using iterator_category = std::forward_iterator_tag;
        using value_type = entry_base;

        iterator_template(HashTableImpl *table, uint32 group_idx, uint32 slot_idx)
            : _table(table), _group_idx(group_idx), _slot_idx(slot_idx) {}
        iterator_template() : _table(NULL), _group_idx(invalid_idx), _slot_idx(invalid_idx) {}
        reference operator*() { return *get_data_ptr(); }
        pointer operator->() { return get_data_ptr(); }

        iterator_template &operator++() {
            if (unlikely(_group_idx == invalid_idx)) {
                return *this;
            }
            // search in current group
            uint16 used_slot = _table->match_full(_group_idx);
            used_slot = (used_slot >> (_slot_idx + 1)) << (_slot_idx + 1);
            if (used_slot) {
                _slot_idx = __builtin_ctz(used_slot);
                if (unlikely(_slot_idx + _group_idx > _table->_capacity)) {
                    _table = NULL;
                    _slot_idx = invalid_idx;
                    _group_idx = invalid_idx;
                }
                return *this;
            }
            // search the next group
            for (uint32 gidx = _group_idx + group_size; gidx < _table->_capacity; gidx += group_size) {
                uint16 used_slot = _table->match_full(gidx);
                if (used_slot) {
                    _group_idx = gidx;
                    _slot_idx = __builtin_ctz(used_slot);
                    if (unlikely(_slot_idx + _group_idx > _table->_capacity)) {
                        _table = NULL;
                        _slot_idx = invalid_idx;
                        _group_idx = invalid_idx;
                    }
                    return *this;
                }
            }
            _table = NULL;
            _group_idx = invalid_idx;
            _slot_idx = invalid_idx;
            return *this;
        }

        iterator_template &operator--() = delete;
        bool operator==(const iterator_template &other) const {
            return _table == other._table && _group_idx == other._group_idx &&
                   _slot_idx == other._slot_idx;
        }
        bool operator!=(const iterator_template &other) const {
            return !operator==(other);
        }

    private:
        friend class HashTableImpl;
        HashTableImpl *_table;
        uint32 _group_idx; /* idx of a group in the whole _data, range[0, _capacity] */
        uint32 _slot_idx; /* idx of a slot in a group, range [0, 15] */

        entry_base *get_data_ptr() const {
            if (_group_idx == invalid_idx || _slot_idx == invalid_idx) {
                return NULL;
            }
            uint32 true_idx = _table->get_true_idx(_group_idx, _slot_idx);
            return &(_table->_data[true_idx]);
        }
    };

    using iterator = iterator_template<entry_base &, entry_base *>;
    using const_iterator = iterator_template<const entry_base &, const entry_base *>;

    const_iterator begin() const { return cbegin(); }
    iterator begin() {
        if (empty()) {
            return end();
        }
        for (uint32 gidx = 0; gidx < _capacity; gidx += group_size) {
            uint16 used_slot = match_full(gidx);
            if (used_slot) {
                int slot_idx = __builtin_ctz(used_slot);
                return iterator(this, gidx, slot_idx);
            }
        }
        __builtin_unreachable();
    }

    const_iterator cbegin() const {
        if (empty()) {
            return cend();
        }
        for (uint32 gidx = 0; gidx < _capacity; gidx += group_size) {
            uint16 used_slot = match_full(gidx);
            if (used_slot) {
                int slot_idx = __builtin_ctz(used_slot);
                return const_iterator(const_cast<self_type *>(this), gidx, slot_idx);
            }
        }
        __builtin_unreachable();
    }

    constexpr const_iterator end() const { return const_iterator(NULL, invalid_idx, invalid_idx); }
    constexpr iterator end() { return iterator(NULL, invalid_idx, invalid_idx); }
    constexpr const_iterator cend() const { return const_iterator(NULL, invalid_idx, invalid_idx); }

    iterator find(const key_type &k) {
        uint32 hash_32 = get_hash32(k);
        uint32 hash_h1 = h1(hash_32);
        int8 hash_h2 = h2(hash_32);
        uint32 gidx = get_gidx(hash_h1);
        bool found;
        iterator it = find_internal<iterator>(k, gidx, hash_h2, found);
        if (found) {
            return it;
        } else {
            return end();
        }
    }

    const_iterator cfind(const key_type &k) const {
        uint32 hash_32 = get_hash32(k);
        uint32 hash_h1 = h1(hash_32);
        int8 hash_h2 = h2(hash_32);
        uint32 gidx = get_gidx(hash_h1);
        bool found;
        const_iterator it = find_internal<const_iterator>(k, gidx, hash_h2, found);
        if (found) {
            return it;
        } else {
            return cend();
        }
    }

    bool contains(const key_type& key) const { return cfind(key) != cend(); }

    template <typename ...Args>
    Pair<iterator, bool> emplace(key_type &&k, Args &&...args) {
        uint32 hash_32 = get_hash32(k);
        uint32 hash_h1 = h1(hash_32);
        int8 hash_h2 = h2(hash_32);
        uint32 gidx = get_gidx(hash_h1);
        bool found;
        iterator it = find_internal<iterator>(k, gidx, hash_h2, found);
        if (found) {
            ann_helper::optional_destroy(k);
            return {it, false};
        } else {
            if (should_grow()) {
                _rehash(next_capacity(_capacity));
                gidx = get_gidx(hash_h1);
                it = find_internal<iterator>(k, gidx, hash_h2, found);
            }
            entry_base *entry_ptr = prepare_construct(it._group_idx, it._slot_idx, hash_h2);
            get_allocator().construct(entry_ptr, std::move(k), std::forward<Args>(args)...);
            return {it, true};
        }
    }

    template <typename ...Args>
    Pair<iterator, bool> emplace(const key_type &k, Args &&...args)
    {
        uint32 hash_32 = get_hash32(k);
        uint32 hash_h1 = h1(hash_32);
        int8 hash_h2 = h2(hash_32);
        uint32 gidx = get_gidx(hash_h1);
        bool found;
        iterator it = find_internal<iterator>(k, gidx, hash_h2, found);
        if (found) {
            return {it, false};
        } else {
            if (should_grow()) {
                _rehash(next_capacity(_capacity));
                gidx = get_gidx(hash_h1);
                it = find_internal<iterator>(k, gidx, hash_h2, found);
            }
            entry_base *entry_ptr = prepare_construct(it._group_idx, it._slot_idx, hash_h2);
            get_allocator().construct(entry_ptr, k, std::forward<Args>(args)...);
            return {it, true};
        }
    }

    template <typename ...Args>
    Pair<iterator, bool> emplace(Args &&...args) {
        entry_base entry(std::forward<Args>(args)...);
        const key_type &k = Param::get_key(entry);
        uint32 hash_32 = get_hash32(k);
        uint32 hash_h1 = h1(hash_32);
        int8 hash_h2 = h2(hash_32);
        uint32 gidx = get_gidx(hash_h1);
        bool found;
        iterator it = find_internal<iterator>(k, gidx, hash_h2, found);
        if (found) {
            ann_helper::optional_destroy(entry);
            return {it, false};
        } else {
            if (should_grow()) {
                _rehash(next_capacity(_capacity));
                gidx = get_gidx(hash_h1);
                it = find_internal<iterator>(k, gidx, hash_h2, found);
            }
            entry_base *entry_ptr = prepare_construct(it._group_idx, it._slot_idx, hash_h2);
            get_allocator().construct(entry_ptr, std::move(entry));
            return {it, true};
        }
    }

protected:
    iterator insert_reserve(key_type &&k, bool &found){
        uint32 hash_32 = get_hash32(k);
        uint32 hash_h1 = h1(hash_32);
        int8 hash_h2 = h2(hash_32);
        uint32 gidx = get_gidx(hash_h1);
        iterator it = find_internal<iterator>(k, gidx, hash_h2, found);
        if (found) {
            ann_helper::optional_destroy(k);
        } else {
            if (should_grow()) {
                _rehash(next_capacity(_capacity));
                gidx = get_gidx(hash_h1);
                it = find_internal<iterator>(k, gidx, hash_h2, found);
            }
            entry_base *entry_ptr = prepare_construct(it._group_idx, it._slot_idx, hash_h2);
            get_allocator().construct((key_type *)entry_ptr, std::move(k));
        }
        return it;
    }

    iterator insert_reserve(const key_type &k, bool &found){
        uint32 hash_32 = get_hash32(k);
        uint32 hash_h1 = h1(hash_32);
        int8 hash_h2 = h2(hash_32);
        uint32 gidx = get_gidx(hash_h1);
        iterator it = find_internal<iterator>(k, gidx, hash_h2, found);
        if (!found) {
            if (should_grow()) {
                _rehash(next_capacity(_capacity));
                gidx = get_gidx(hash_h1);
                it = find_internal<iterator>(k, gidx, hash_h2, found);
            }
            entry_base *entry_ptr = prepare_construct(it._group_idx, it._slot_idx, hash_h2);
            get_allocator().construct((key_type *)entry_ptr, k);
        }
        return it;
    }

public:
    Pair<iterator, bool> insert(const entry_base &entry) {
        const key_type &k = Param::get_key(entry);
        uint32 hash_32 = get_hash32(k);
        uint32 hash_h1 = h1(hash_32);
        int8 hash_h2 = h2(hash_32);
        uint32 gidx = get_gidx(hash_h1);
        bool found;
        iterator it = find_internal<iterator>(k, gidx, hash_h2, found);
        if (found) {
            return {it, false};
        } else {
            if (should_grow()) {
                _rehash(next_capacity(_capacity));
                gidx = get_gidx(hash_h1);
                it = find_internal<iterator>(k, gidx, hash_h2, found);
            }
            entry_base *entry_ptr = prepare_construct(it._group_idx, it._slot_idx, hash_h2);
            get_allocator().construct(entry_ptr, entry);
            return {it, true};
        }
    }

    Pair<iterator, bool> insert(entry_base &&entry) {
        const key_type &k = Param::get_key(entry);
        uint32 hash_32 = get_hash32(k);
        uint32 hash_h1 = h1(hash_32);
        int8 hash_h2 = h2(hash_32);
        uint32 gidx = get_gidx(hash_h1);
        bool found;
        iterator it = find_internal<iterator>(k, gidx, hash_h2, found);
        if (found) {
            ann_helper::optional_destroy(entry);
            return {it, false};
        } else {
            if (should_grow()) {
                _rehash(next_capacity(_capacity));
                gidx = get_gidx(hash_h1);
                it = find_internal<iterator>(k, gidx, hash_h2, found);
            }
            entry_base *entry_ptr = prepare_construct(it._group_idx, it._slot_idx, hash_h2);
            get_allocator().construct(entry_ptr, std::move(entry));
            return {it, true};
        }
    }

    template <class InputIt>
    void insert(InputIt first, InputIt last) {
        for (InputIt it = first; it != last; ++it) {
            insert(*it);
        }
    }

    void insert(std::initializer_list<entry_base> ilist) {
        for (const auto &i : ilist) {
            insert(i);
        }
    }
    
    iterator erase(iterator it) {
        if (it == end()) {
            return it;
        }
        ++_store._available;
        uint32 true_idx = get_true_idx(it._group_idx, it._slot_idx);
        set_meta(true_idx, (int8)Ctrl::DELETED);
        entry_base *entry_ptr = &_data[true_idx];
        get_allocator().destroy(entry_ptr);
        ++it;
        return it;
    }

    const_iterator erase(const_iterator it) {
        if (it == cend()) {
            return it;
        }
        ++_store._available;
        uint32 true_idx = get_true_idx(it._group_idx, it._slot_idx);
        set_meta(true_idx, (int8)Ctrl::DELETED);
        entry_base *entry_ptr = &_data[true_idx];
        get_allocator().destroy(entry_ptr);
        ++it;
        return it;
    }

    uint32 erase(const key_type &k){
        iterator it = find(k);
        if (it == end()) {
            return 0;
        }
        ++_store._available;
        uint32 true_idx = get_true_idx(it._group_idx, it._slot_idx);
        set_meta(true_idx, (int8)Ctrl::DELETED);
        entry_base *entry_ptr = &_data[true_idx];
        get_allocator().destroy(entry_ptr);
        return 1ul;
    }

    void reserve(size_t count)
    {
        if (_capacity >= count) {
            return;
        }
        _rehash(count);
    }

    void destroy() {
        destroy_used_slots();
        destroy_table();
    }

    template <typename Callback>
    void ctraverse(const_iterator start, const_iterator dest, Callback &&callback) const {
        size_t start_true_idx = 0;
        size_t dest_true_idx = 0;
        if (start == cend()) {
            return;
        }
        start_true_idx = get_true_idx(start._group_idx, start._slot_idx); 
        if (dest == cend()) {
            dest_true_idx = _capacity;
        } else {
            dest_true_idx = get_true_idx(dest._group_idx, dest._slot_idx);
        }
        if (start_true_idx > dest_true_idx) {
            return;
        }
        size_t gidx = start_true_idx;
        uint16_t used_slot = 0;
        size_t sidx = 0;
        for (; gidx < dest_true_idx; gidx += group_size) {
            used_slot = match_full(gidx);
            while(used_slot) {
                sidx = __builtin_ctz(used_slot);
                if (unlikely(gidx + sidx > dest_true_idx) || unlikely(gidx + sidx == _capacity)) {
                    return;
                }
                entry_base &entry_ptr = _data[gidx + sidx];
                callback(entry_ptr);
                used_slot &= used_slot - 1;
            }
        }
        return;
    }
    template <typename Callback>
    void ctraverse(Callback &&callback) const {
        ctraverse(cbegin(), cend(), std::forward<Callback>(callback));
    }

private:
    uint32 align(uint32 x, uint32 a) { return (x + a - 1) & (~(a - 1)); }
    uint32 get_hash32(const key_type &key) const { return _store.hash_func(key); }
    bool cmp_key(const key_type &k1, const key_type &k2) const { return _store.cmp(k1, k2); }
    uint32 h1(const uint32 hash) const { return (hash >> 7) & 0x7FFFFFFF; }
    int8 h2(const uint32 hash) const { return (int8)(hash & 0x7F); }
    bool is_full(int8 meta) const { return meta >= 0; }
    uint32 next_capacity(uint32 capacity) { return 2 * capacity + 1; }
    bool should_grow() { return _store._available == 0; }
    uint32 get_groupnum() const { return _capacity / group_size + 1; }
    uint32 get_gidx(const uint32 hash_h1) const { return hash_h1 & _capacity; }
    
    uint32 get_allsize() {
        return align(sizeof(Ctrl) * (_capacity + group_size), group_size)
             + align(sizeof(entry_base) * _capacity, group_size);
    }
    /* idx of a slot in the whole _data. */
    uint32 get_true_idx(uint32 group_idx, uint32 slot_idx) const {
        return (group_idx + slot_idx) & _capacity;
    }

    void set_meta(uint32 true_idx, int8 value) {
        _ctrl[true_idx] = (Ctrl)value;
        if (true_idx < group_size - 1) {
            _ctrl[true_idx + _capacity + 1] = (Ctrl)value;
        }
    }

    uint16 match_full(uint32 group_idx) const {
    #if defined(__x86_64__)
        __m128i group_meta = _mm_loadu_si128((__m128i *)(_ctrl + group_idx));
        uint16 res = _mm_movemask_epi8(group_meta);
        return ~res;
    #elif defined(__ARM_NEON) || defined(__aarch64__)
        uint8x16_t group_meta = vld1q_u8(reinterpret_cast<const uint8_t*>(_ctrl + group_idx));

        static const uint8x16_t mask = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
        uint8x16_t masked = vandq_u8(mask, (uint8x16_t)vshrq_n_s8((int8x16_t)group_meta, 7));
        uint8x16_t maskedhi = vextq_u8(masked, masked, 8);
        int16 res = vaddvq_u16((uint16x8_t)vzip1q_u8(masked, maskedhi));
        return ~res;
    #else
        const int8* group_meta = _ctrl + group_idx;
        uint16 res = 0;
        for (uint32 i = 0; i < group_size; ++i) {
            if (group_meta[i] >= 0) {
                res |= (1 << i);
            }
        }
        return res;
    #endif
    }

    uint16 match_empty_or_delete(uint32 group_idx) const {
    #if defined(__x86_64__)
        __m128i group_meta = _mm_loadu_si128((__m128i *)(_ctrl + group_idx));
        __m128i special = _mm_set1_epi8(static_cast<char>(Ctrl::END)); // 0xFF (-1)
        __m128i cmp_res = _mm_cmpgt_epi8(special, group_meta); // cannot swap order!!!!
        uint16_t res = _mm_movemask_epi8(cmp_res);
        return res;
    #elif defined(__ARM_NEON) || defined(__aarch64__)
        uint8x16_t group_meta = vld1q_u8(reinterpret_cast<const uint8_t*>(_ctrl + group_idx));
        uint8x16_t special = vdupq_n_u8(static_cast<char>(Ctrl::END));
        uint8x16_t cmp_res = vcgtq_s8(vreinterpretq_s8_u8(special), vreinterpretq_s8_u8(group_meta));

        static const uint8x16_t mask = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
        uint8x16_t masked = vandq_u8(mask, (uint8x16_t)vshrq_n_s8((int8x16_t)cmp_res, 7));
        uint8x16_t maskedhi = vextq_u8(masked, masked, 8);
        int16 res = vaddvq_u16((uint16x8_t)vzip1q_u8(masked, maskedhi));
        return (uint16)res;
    #else
        const int8 *group_meta = (const int8 *)_ctrl + group_idx;
        uint16 res = 0;
        for (uint32 i = 0; i < 16; ++i) {
            const int8 meta = group_meta[i];
            if (meta < static_cast<int8>(Ctrl::END)) {
                res |= (1 << i);
            }
        }
        return res;
    #endif
    }
    
    uint16 match_meta(uint32 group_idx, int8 key) const {
    #if defined(__x86_64__)
        __m128i key_128 = _mm_set1_epi8(key);
        __m128i group_meta = _mm_loadu_si128((__m128i *)(_ctrl + group_idx));
        __m128i cmp_res = _mm_cmpeq_epi8(group_meta, key_128);
        uint16 res = _mm_movemask_epi8(cmp_res);
        return res;
    #elif defined(__ARM_NEON) || defined(__aarch64__)
        uint8x16_t key_128 = vdupq_n_u8(key);
        uint8x16_t group_meta = vld1q_u8(reinterpret_cast<const uint8_t*>(_ctrl + group_idx));
        uint8x16_t cmp_res = vceqq_u8(group_meta, key_128);

        static const uint8x16_t mask = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
        uint8x16_t masked = vandq_u8(mask, (uint8x16_t)vshrq_n_s8((int8x16_t)cmp_res, 7));
        uint8x16_t maskedhi = vextq_u8(masked, masked, 8);
        int16 res = vaddvq_u16((uint16x8_t)vzip1q_u8(masked, maskedhi));
        return (uint16)res;
    #else        
        const int8 *group_meta = (const int8 *)_ctrl + group_idx;
        uint16 res = 0;
        for (uint32 i = 0; i < group_size; ++i) {
            if (group_meta[i] == key) {
                res |= (1 << i);
            }
        }
        return res;
    #endif
    }

    // check whether the first slot is empty.
    // if the first slot is empty,can ensure the whole group is empty,
    // some time can help skip
    bool check_first(uint32 group_idx) const {
        Ctrl first_meta = _ctrl[group_idx];
        return first_meta == Ctrl::EMPTY;
    }
    // if exists, return the match iterator.
    // if not exists, return the iterator of an empty or delete slot.
    template <typename Iter>
    Iter find_internal(const key_type &key, uint32 group_main_idx, int8 hash_h2, bool &found) const {
        Iter it;
        bool record = false;
        uint32 group_num = get_groupnum();
        for (uint32 gidx = group_main_idx; gidx < group_main_idx + group_num * group_size; gidx += group_size) {
            uint32 group_idx = get_true_idx(gidx, 0);
            uint32 slot_idx = 0; // idx of the slot in a group, range [0, 15]
            uint32 true_idx = 0; // idx of the slot in the whole _data.
            if (check_first(group_idx)) {
                found = false;
                if (!record) {
                    it._table = const_cast<self_type *>(this);
                    it._group_idx = group_idx;
                    it._slot_idx = 0;
                }
                return it;
            }
            uint16 mask = match_meta(group_idx, hash_h2);
            while (mask) {
                slot_idx = __builtin_ctz(mask);
                true_idx = get_true_idx(group_idx, slot_idx);
                entry_base *entry_ptr = &_data[true_idx];
                if (cmp_key(Param::get_key(*entry_ptr), key)) {
                    found = true;
                    it._table = const_cast<self_type *>(this);
                    it._group_idx = group_idx;
                    it._slot_idx = slot_idx;
                    return it;
                }
                mask &= mask - 1;
            }
            // current group not find key, try to find empty or delete.
            // if find, record and don't need to find in following loop.
            if (!record) {
                mask = match_empty_or_delete(group_idx);
                if (mask) {
                    record = true;
                    slot_idx = __builtin_ctz(mask);
                    it._table = const_cast<self_type *>(this);
                    it._group_idx = group_idx;
                    it._slot_idx = slot_idx;
                }
            }
        }
        found = false;
        return it;
    }

    entry_base *prepare_construct(uint32 group_idx, uint32 slot_idx, int8 hash_h2) {
        uint32 true_idx = get_true_idx(group_idx, slot_idx);
        set_meta(true_idx, hash_h2);
        --_store._available;
        return &_data[true_idx];
    }

    void insert_internal(entry_base &&entry, uint32 group_main_idx, int8 hash_h2) {
        uint32 group_num = get_groupnum();
        for (uint32 gidx = group_main_idx; gidx < group_main_idx + group_num * group_size; gidx += group_size) {
            uint32 group_idx = get_true_idx(gidx, 0);
            uint16 free_slot = match_empty_or_delete(group_idx);
            uint32 slot_idx = 0;
            if (free_slot) {
                slot_idx = __builtin_ctz(free_slot);
                entry_base *entry_ptr = prepare_construct(group_idx, slot_idx, hash_h2);
                get_allocator().construct(entry_ptr, std::move(entry));
                return;
            }
        }
        Assert(0);
        __builtin_unreachable();
    }

    void _rehash(uint32 new_capacity) {
        HashTableImpl new_table = self_type(new_capacity, _store, _store, get_allocator()); 
        for (uint32 i = 0; i < _capacity; ++i) {
            if (is_full((int8)_ctrl[i])) {
                entry_base &entry = _data[i];
                uint32 hash_32 = get_hash32(Param::get_key(entry));
                uint32 hash_h1 = h1(hash_32);
                int8 hash_h2 = h2(hash_32);
                uint32 new_gidx = new_table.get_gidx(hash_h1);
                new_table.insert_internal(std::move(entry), new_gidx, hash_h2);
            }
        }
        destroy_table();
        swap(new_table);
    }

    void destroy_used_slots() {
        CONSTEXPR_IF(!ann_helper::internal::has_destroyer<entry_base>::value &&
                     std::is_trivially_destructible<entry_base>::value) {
            return;
        }
        uint32 true_idx = 0;
        uint16 used_slot = 0;
        int slot_idx = 0;
        for (uint32 gidx = 0; gidx < _capacity; gidx += group_size) {
            used_slot = match_full(gidx);
            while (used_slot) {
                slot_idx = __builtin_ctz(used_slot);
                true_idx = gidx + slot_idx;
                get_allocator().destroy(&_data[true_idx]); 
                used_slot &= used_slot - 1;
            }
        }
    }

    void destroy_table() {
        if (_ctrl) {
            get_rebind_allocator<char>().deallocate((char *)_ctrl, get_allsize());
            _ctrl = NULL;
            _data = NULL;
            _capacity = 0;
            _store._available = 0;
        }
    }
};
} /* namespace impl */

#endif // CONTAINER_SWISSTABLE_H
