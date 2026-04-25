/**
 * Copyright ...
 * High-performance parallel counter using sharded atomics.
 */

#ifndef MODULE_PARALLEL_COUNTER_H
#define MODULE_PARALLEL_COUNTER_H

#include <atomic>
#include "c.h"

namespace ann_helper {

struct ParaCounter {
    constexpr static size_t k_cache_line_size = 64;
    constexpr static size_t nshards = 64;

    struct alignas(k_cache_line_size) Shard {
        std::atomic<int64> count{0};
    };

    ParaCounter() = default;

    void inc(int64 delta = 1)
    {
        thread_local auto s = (++get_thread_counter()) % nshards;
        _shards[s].count.fetch_add(delta, std::memory_order_relaxed);
    }
    void inc(const ParaCounter &other)
    {
        for (size_t i = 0; i < nshards; ++i) {
            _shards[i].count.fetch_add(other._shards[i].count, std::memory_order_relaxed);
        }
    }

    int64 value()
    {
        int64 total = 0;
        for (size_t i = 0; i < nshards; ++i) {
            total += _shards[i].count.load(std::memory_order_relaxed);
        }
        return total;
    }

    void reset()
    {
        for (size_t i = 0; i < nshards; ++i) {
            _shards[i].count = 0;
        }
    }

private:
    Shard _shards[nshards];

    static std::atomic<size_t> &get_thread_counter()
    {
        static std::atomic<size_t> counter{0};
        return counter;
    }
};

struct Counter {
    Counter() = default;
    void inc(int64 delta = 1) { counter += delta; }
    void inc(const Counter &other) { counter += other.counter; }
    int64 value() { return counter; }
    void reset() { counter = 0; }
private:
    int64 counter{0};
};

} /* namespace ann_helper */
#endif /* MODULE_PARALLEL_COUNTER_H */
 