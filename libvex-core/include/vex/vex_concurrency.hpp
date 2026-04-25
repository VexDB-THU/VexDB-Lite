#ifndef VEX_CONCURRENCY_HPP
#define VEX_CONCURRENCY_HPP

#include "vex/vex_types.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>
#endif
#include <vector>

namespace vex {

#ifdef VEX_MOBILE_MODE
class SimpleRWLock {
    std::mutex mtx_;
    std::condition_variable cv_;
    int readers_ = 0;
    bool writer_ = false;
    int writer_waiters_ = 0;

public:
    void lock_shared() {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return !writer_ && writer_waiters_ == 0; });
        ++readers_;
    }

    void unlock_shared() {
        std::unique_lock<std::mutex> lk(mtx_);
        if (--readers_ == 0) {
            cv_.notify_all();
        }
    }

    void lock() {
        std::unique_lock<std::mutex> lk(mtx_);
        ++writer_waiters_;
        cv_.wait(lk, [this] { return !writer_ && readers_ == 0; });
        --writer_waiters_;
        writer_ = true;
    }

    void unlock() {
        std::unique_lock<std::mutex> lk(mtx_);
        writer_ = false;
        cv_.notify_all();
    }
};
#else
class SimpleRWLock {
    std::atomic<int> state_{0};

    static inline void cpu_pause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
    }

    static inline void backoff(int &spin) {
        if (spin < 16) {
            cpu_pause();
        } else if (spin < 128) {
            for (int i = 0; i < 8; i++) {
                cpu_pause();
            }
        } else {
            std::this_thread::yield();
        }
        spin++;
    }

public:
    void lock_shared() {
        int spin = 0;
        int s;
        do {
            s = state_.load(std::memory_order_acquire);
            while (s < 0) {
                backoff(spin);
                s = state_.load(std::memory_order_acquire);
            }
        } while (!state_.compare_exchange_weak(s, s + 1, std::memory_order_acq_rel, std::memory_order_relaxed));
    }

    void unlock_shared() {
        state_.fetch_sub(1, std::memory_order_release);
    }

    void lock() {
        int spin = 0;
        int expected = 0;
        while (!state_.compare_exchange_weak(expected, -1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            expected = 0;
            backoff(spin);
        }
    }

    void unlock() {
        state_.store(0, std::memory_order_release);
    }
};
#endif

class SharedLockGuard {
    SimpleRWLock &lock_;

public:
    explicit SharedLockGuard(SimpleRWLock &l) : lock_(l) {
        lock_.lock_shared();
    }

    ~SharedLockGuard() {
        lock_.unlock_shared();
    }

    SharedLockGuard(const SharedLockGuard &) = delete;
    SharedLockGuard &operator=(const SharedLockGuard &) = delete;
};

class VisitedSet {
public:
    explicit VisitedSet(idx_t capacity_hint = 128) {
        capacity_ = 64;
        while (capacity_ < capacity_hint * 2) {
            capacity_ <<= 1;
        }
        mask_ = capacity_ - 1;
        generation_ = 1;
        table_.resize(capacity_);
        gen_table_.resize(capacity_, 0);
    }

    void Clear() {
        ++generation_;
        if (generation_ == 0) {
            std::fill(gen_table_.begin(), gen_table_.end(), 0);
            generation_ = 1;
        }
    }

    void Reserve(idx_t capacity_hint) {
        idx_t new_cap = 64;
        while (new_cap < capacity_hint * 2) {
            new_cap <<= 1;
        }
        if (new_cap > capacity_) {
            capacity_ = new_cap;
            mask_ = capacity_ - 1;
            table_.resize(capacity_);
            gen_table_.resize(capacity_, 0);
        }
        Clear();
    }

    inline bool Insert(idx_t key) {
        idx_t idx = Hash(key) & mask_;
        while (true) {
            if (gen_table_[idx] != generation_) {
                table_[idx] = key;
                gen_table_[idx] = generation_;
                return true;
            }
            if (table_[idx] == key) {
                return false;
            }
            idx = (idx + 1) & mask_;
        }
    }

private:
    static inline idx_t Hash(idx_t x) {
        x ^= x >> 33U;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33U;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33U;
        return x;
    }

private:
    idx_t capacity_ = 0;
    idx_t mask_ = 0;
    uint32_t generation_ = 0;
    std::vector<idx_t> table_;
    std::vector<uint32_t> gen_table_;
};

} // namespace vex

#endif // VEX_CONCURRENCY_HPP
