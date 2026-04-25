/**
 * Copyright ...
 */

#ifndef CONTAINER_DISK_HASHTABLE_DEPENDENCY_H
#define CONTAINER_DISK_HASHTABLE_DEPENDENCY_H

#include <atomic>

#include "c.h"
#include "storage/block.h"

namespace disk_container {
enum class HashEntryType : uint16 {
    None = 0,
    Plain = 1u,
    InplaceHash = 2u,
    ExtensiveHashOneB1 = 3u,
    ExtensiveHashOneB2 = 4u,
    ExtensiveHashOne = 5u,
    ExtensiveHashTen = 6u,
    Unused = 7u,    /* should not be reached in whatever cases */
};

#pragma pack(push, 2)
struct HashEntry {
    static constexpr uint16 EntryTypeMask = 0x0007u;
    static constexpr uint16 EntryUpgradeMask = 0x0008u; /* reserved for concurrency optimization */
    static constexpr uint16 EntrySeedMask = 0xfff0u;
    std::atomic<uint16> flag;
    BlockNumber blkno;
    HashEntry() : flag(0), blkno(InvalidBlockNumber) {}
    HashEntry(const HashEntry &other) : flag(other.flag.load(std::memory_order_relaxed)), blkno(other.blkno) {}
    HashEntryType get_type(std::memory_order order = std::memory_order::memory_order_acquire) const
        { return (HashEntryType)(flag.load(order) & EntryTypeMask); }
    uint8 get_type_u8(std::memory_order order = std::memory_order::memory_order_acquire) const
        { return (uint8)get_type(order); }
    void set_type(HashEntryType type, std::memory_order order = std::memory_order::memory_order_acq_rel)
        { flag.store((flag.load(order) & ~EntryTypeMask) | (uint16)type, order); }
    uint16 get_seed() const { return flag.load(std::memory_order::memory_order_relaxed) & EntrySeedMask; }
    void set_seed(uint16 seed, std::memory_order order = std::memory_order::memory_order_acq_rel)
        { flag.store((flag.load(order) & ~EntrySeedMask) | (seed & EntrySeedMask), order); }
};

template <typename K, typename V>
struct KVBase {
    const K k;
    V v;
    KVBase() = default;
    KVBase(const KVBase &) = default;
    KVBase(KVBase &&) = default;
    KVBase(K &&k, V &&v) : k(std::move(k)), v(std::move(v)) {}
    KVBase(const K &k, V &&v) : k(k), v(std::move(v)) {}
    KVBase(K &&k, const V &v) : k(std::move(k)), v(v) {}
    KVBase(const K &k, const V &v) : k(k), v(v) {}
    template <typename ...Args>
    KVBase(K &&k, Args &&...args) : k(std::move(k)), v(std::forward<Args>(args)...) {}
    template <typename ...Args>
    KVBase(const K &k, Args &&...args) : k(k), v(std::forward<Args>(args)...) {}
};
#pragma pack(pop)

template <typename K, typename V>
struct PlainEntryWithHash {
    uint32 hash_val;
    KVBase<K, V> kv;
    template <class Comparable>
    bool compare(uint32 in_hash_val, const K &k) const
        { return hash_val == in_hash_val && Comparable{}(k, kv.k); }
    template <class unused>
    uint32 get_hash() const { return hash_val; }
    void set_hash(uint32 hash) { hash_val = hash; }
    static constexpr bool has_hash = true;
};

template <typename K, typename V>
struct PlainEntryWithoutHash {
    KVBase<K, V> kv;
    template <class Comparable>
    bool compare(uint32, const K &k) const { return Comparable{}(k, kv.k); }
    template <class Hasher>
    uint32 get_hash() const { return Hasher{}(kv.k); }
    void set_hash(uint32) const {}
    static constexpr bool has_hash = false;
};
} /* namespace disk_container */

#endif /* CONTAINER_DISK_HASHTABLE_DEPENDENCY_H */
