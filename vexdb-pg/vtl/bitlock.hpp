/**
 * Copyright ...
 */

#ifndef CONTAINER_BITLOCK_H
#define CONTAINER_BITLOCK_H

#include <atomic>
#include <vtl/vector>

#include "pg_compat.h"

struct DummyLocker {
    DummyLocker(size_t size = 0) {}
    ~DummyLocker() = default;
    static void lock(size_t idx) {}
    static void unlock(size_t idx) {}
    static void resize(size_t size) {}
};

class BitLock {
    using word_type = uint16;
    static constexpr word_type word_size = sizeof(word_type) * __CHAR_BIT__;
public:
    BitLock(size_t size = 0) : _locks((size + word_size - 1) / word_size) { resize(size); }
    ~BitLock() = default;
    void lock(size_t idx)
    {
        word_type word_idx = idx / word_size;
        word_type bit_idx = idx % word_size;
        word_type mask = (word_type)1 << bit_idx;
        for (uint32 k = 0; !(_locks[word_idx].fetch_or(mask) & mask); ++k) {
            pg_yield(k);
        }
    }
    bool trylock(size_t idx)
    {
        word_type word_idx = idx / word_size;
        word_type bit_idx = idx % word_size;
        word_type mask = (word_type)1 << bit_idx;
        return _locks[word_idx].fetch_or(mask) & mask;
    }
    void unlock(size_t idx)
    {
        word_type word_idx = idx / word_size;
        word_type bit_idx = idx % word_size;
        word_type mask = ~((word_type)1 << bit_idx);
        _locks[word_idx].fetch_and(mask);
    }
    void resize(size_t size) { _locks.resize((size + word_size - 1) / word_size); }
    void destroy() { _locks.clear(); _locks.shrink_to_fit(); }

    bool locked(size_t idx)
    {
        word_type word_idx = idx / word_size;
        word_type bit_idx = idx % word_size;
        word_type mask = (word_type)1 << bit_idx;
        return _locks[word_idx].load() & mask;
    }
    void wait(size_t idx)
    {
        word_type word_idx = idx / word_size;
        word_type bit_idx = idx % word_size;
        word_type mask = (word_type)1 << bit_idx;
        for (uint32 k = 0; _locks[word_idx].load() & mask; ++k) {
            pg_yield(k);
        }
    }
private:
    Vector<std::atomic<word_type>> _locks;
};

class RWBitLock {
public:
    RWBitLock(size_t size = 0) : _rlocks(size, 0), _wlock(size) {}
    ~RWBitLock() = default;
    void destroy() { _rlocks.clear(); _rlocks.shrink_to_fit(); _wlock.destroy(); }

    void rlock(size_t idx)
    {
        for (;;) {
            _wlock.wait(idx);
            _rlocks[idx].fetch_add(1);
            if (!_wlock.locked(idx)) {
                return;
            }
            _rlocks[idx].fetch_sub(1);
        }
    }
    void wlock(size_t idx)
    {
        _wlock.lock(idx);
        for (uint32 k = 0; _rlocks[idx].load(); ++k) {
            pg_yield(k);
        }
    }
    void runlock(size_t idx) { _rlocks[idx].fetch_sub(1); }
    void wunlock(size_t idx) { _wlock.unlock(idx); }
private:
    Vector<std::atomic<uint8>> _rlocks;
    BitLock _wlock;
};
#endif /* CONTAINER_BITLOCK_H */
