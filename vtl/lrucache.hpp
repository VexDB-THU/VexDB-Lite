/**
 * Copyright ...
 */

#ifndef CONTAINER_LRU_CACHE_H
#define CONTAINER_LRU_CACHE_H

#include <vtl/list.hpp>

#include "utils/hsearch.h"
#include "utils/palloc.h"

namespace vtl {
template <typename Key, typename Value>
class LRUCache : public BaseObject {
public:
    using pair_type = struct Pair { Key key; Value value; };
    using list_type = DLList<pair_type>;
    using list_pointer = typename list_type::iterator;
    using iterator = typename list_type::iterator;
    using const_iterator = typename list_type::const_iterator;
    using map_type = struct MapPair {
        Key key;
        list_pointer it;
        MapPair(const Key &key, list_pointer it) : key(key), it(it) {}
    };

    LRUCache(MemoryContext ctx, size_t cache_size) : _max_size(cache_size), _list(ctx)
    {
        HASHCTL ctl;
        memset(&ctl, 0, sizeof(ctl));
        ctl.keysize = sizeof(Key);
        ctl.entrysize = sizeof(map_type);
        ctl.hash = sizeof(Key) == sizeof(Oid) ? oid_hash : tag_hash;
        ctl.hcxt = ctx;
        _map = hash_create("LRU cache", cache_size, &ctl, HASH_ELEM | HASH_FUNCTION | HASH_SHRCTX);
        SpinLockInit(&_lock);
    }
    LRUCache(LRUCache &&other)
    {
        _map = other._map;
        other._map = NULL;
        _list = std::move(other._list);
        _lock = other._lock;
    }
    LRUCache &operator=(LRUCache &&other)
    {
        if (this != &other) {
            _map = other._map;
            other._map = NULL;
            _list = std::move(other._list);
            _lock = other._lock;
        }
        return *this;
    }

    void put(const Key &key, const Value &value)
    {
        bool found;
        auto *it = (map_type *)hash_search(_map, &key, HASH_FIND, &found);
        if (found) {
            _list.move_back(it->it);
        } else {
            if (size_t(hash_get_num_entries(_map)) > _max_size) {
                evict();
            }
            map_type *entry = (map_type *)hash_search(_map, &key, HASH_ENTER, &found);
            entry->it = _list.push_back({key, std::move(value)});
        }
    }
    void put(const Key &key, Value &&value)
    {
        bool found;
        auto *it = (map_type *)hash_search(_map, &key, HASH_FIND, &found);
        if (found) {
            _list.move_back(it->it);
        } else {
            if (size_t(hash_get_num_entries(_map)) > _max_size) {
                evict();
            }
            map_type *entry = (map_type *)hash_search(_map, &key, HASH_ENTER, &found);
            entry->it = _list.push_back({key, std::move(value)});
        }
    }
    void insert(const Key &key, const Value &value) { put(key, value); }
    void insert(const Key &key, Value &&value) { put(key, std::move(value)); }

    Value *get(const Key &key)
    {
        bool found;
        auto *entry = (map_type *)hash_search(_map, &key, HASH_FIND, &found);
        if (!found) {
            return NULL;
        }
        SpinLockAcquire(&_lock);
        _list.move_back(entry->it);
        SpinLockRelease(&_lock);
        return &entry->it->value;
    }
    Value &operator[](const Key &key)
    {
        auto *entry = (map_type *)hash_search(_map, &key, HASH_FIND, NULL);
        Assert(entry);
        SpinLockAcquire(&_lock);
        _list.move_back(entry->it);
        SpinLockRelease(&_lock);
        return entry->it->value;
    }
    bool contains(const Key &key) const
    {
        bool found;
        hash_search(_map, &key, HASH_FIND, &found);
        return found;
    }

    bool erase(const Key &key)
    {
        bool found;
        auto *entry = (map_type *)hash_search(_map, &key, HASH_REMOVE, &found);
        if (!found) {
            return false;
        }
        _list.erase(entry->it); /* the content in entry is still usable */
        return true;
    }
    void evict()
    {
        auto it = _list.begin();
        Assert(it != _list.end());
        hash_search(_map, &it->key, HASH_REMOVE, NULL);
        _list.erase(it);
    }

    iterator begin() { return _list.begin(); }
    iterator end() { return _list.end(); }
    const_iterator cbegin() const { return _list.cbegin(); }
    const_iterator cend() const { return _list.cend(); }

    size_t size() const { return hash_get_num_entries(_map); }
    bool empty() const { return size() == 0; }
    void destroy()
    {
        hash_destroy(_map);
        ann_helper::optional_destroy(_list);
        SpinLockFree(&_lock);
    }
    
    void set_capacity(size_t new_size)
    {
        while (size_t(hash_get_num_entries(_map)) > new_size) {
            evict();
        }
        _max_size = new_size;
    }
private:
    /* no mem cxt is required since all mem alloc is done at the container level */
    size_t _max_size;
    HTAB *_map;
    list_type _list;
    slock_t _lock;

    LRUCache(const LRUCache &other) = delete;
    LRUCache operator=(const LRUCache &other) = delete;
};
} /* namespace vtl */

#endif /* CONTAINER_LRU_CACHE_H */

