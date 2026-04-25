/**
 * Copyright ...
 * Hashtable/set interface.
 */

#ifndef CONTAINER_HASHTABLE_COMMON_H
#define CONTAINER_HASHTABLE_COMMON_H

#include <vtl/allocator>
#include <vtl/pair>
#include <vtl/internal/hashtable/swisstable.hpp>

namespace vtl {
namespace internal {
template <typename T>
class SetParam {
public:
    using data_type = T;
    using key_type = T;
    static constexpr const key_type &get_key(const data_type &d) { return d; }
};
template <typename K, typename V>
class MapParam {
public:
    using data_type = Pair<K, V>;
    using key_type = K;
    static constexpr const key_type &get_key(const data_type &d) { return d.first; }
};
} /* namespace internal */

template <typename Key, typename Value, class Hasher = impl::DefaultHasher<Key>, 
          class KeyEqual = impl::DefaultEqual<Key>,
          class Allocator = DEFAULT_ALLOCATOR<typename internal::MapParam<Key, Value>::data_type>
>
class HashTable : public impl::HashTableImpl<internal::MapParam<Key, Value>, Hasher, KeyEqual, Allocator> {
    using super = impl::HashTableImpl<internal::MapParam<Key, Value>, Hasher, KeyEqual, Allocator>;
public:
    using super::super;
    using iterator = typename super::iterator;
    using const_iterator = typename super::const_iterator;
    using super::operator=;

    Value &operator[](const Key &key)
    {
        bool unused;
        return this->insert_reserve(key, unused)->second;
    }
    Value &operator[](Key &&key)
    {
        bool unused;
        return this->insert_reserve(std::move(key), unused)->second;
    }
    template <typename ...Args>
    Pair<iterator, bool> try_emplace(const Key &key, Args &&...args)
    {
        bool found;
        auto it = this->insert_reserve(key, found);
        if (found) {
            return vtl::make_pair(it, false);
        }
        super::get_allocator().construct(&it->second, std::forward<Args>(args)...);
        return vtl::make_pair(it, true);
    }
    template <typename ...Args>
    Pair<iterator, bool> try_emplace(Key &&key, Args &&...args)
    {
        bool found;
        auto it = this->insert_reserve(std::move(key), found);
        if (found) {
            return vtl::make_pair(it, false);
        }
        super::get_allocator().construct(&it->second, std::forward<Args>(args)...);
        return vtl::make_pair(it, true);
    }
};

template <typename T, class Hasher = impl::DefaultHasher<T>, 
          class KeyEqual = impl::DefaultEqual<T>,
          class Allocator = DEFAULT_ALLOCATOR<typename internal::SetParam<T>::data_type>
>
class HashSet : public impl::HashTableImpl<internal::SetParam<T>, Hasher, KeyEqual, Allocator> {
    using super = impl::HashTableImpl<internal::SetParam<T>, Hasher, KeyEqual, Allocator>;
public:
    using super::super;
    template <typename InputIt>
    HashSet(InputIt first, InputIt last, const Hasher &hasher = Hasher(),
            const KeyEqual &equal = KeyEqual(), const Allocator &alloc = Allocator())
        : super(std::distance(first, last), hasher, equal, alloc)
    {
        for (auto it = first; it != last; ++it) {
            this->emplace(*it);
        }
    }
    HashSet(std::initializer_list<T> list, const Hasher &hasher = Hasher(),
            const KeyEqual &equal = KeyEqual(), const Allocator &alloc = Allocator())
        : HashSet(list.begin(), list.end(), hasher, equal, alloc) {}

    using iterator = typename super::iterator;
    using const_iterator = typename super::const_iterator;
    using super::operator=;
};
} /* namespace vtl */

#if CONTAINER_USE_STL_HASH
#include <unordered_map>
#include <unordered_set>
template <typename Key, typename Value, class Allocator = DEFAULT_ALLOCATOR<std::pair<const Key, Value>>>
using UnorderedMap = std::unordered_map<Key, Value, std::hash<Key>, std::equal_to<Key>, Allocator>;
template <typename T, class Allocator = DEFAULT_ALLOCATOR<T>>
using UnorderedSet = std::unordered_set<T, std::hash<T>, std::equal_to<T>, Allocator>;
#else
template <typename Key, typename Value,
          class Hasher = impl::DefaultHasher<Key>, 
          class KeyEqual = impl::DefaultEqual<Key>,
          class Allocator = DEFAULT_ALLOCATOR<Pair<Key, Value>>>
using UnorderedMap = vtl::HashTable<Key, Value, Hasher, KeyEqual, Allocator>;
template <typename T,
          class Hasher = impl::DefaultHasher<T>, 
          class KeyEqual = impl::DefaultEqual<T>,
          class Allocator = DEFAULT_ALLOCATOR<T>>
using UnorderedSet = vtl::HashSet<T, Hasher, KeyEqual, Allocator>;
#endif /* CONTAINER_USE_STL_HASH */
#endif /* CONTAINER_HASHTABLE_COMMON_H */
