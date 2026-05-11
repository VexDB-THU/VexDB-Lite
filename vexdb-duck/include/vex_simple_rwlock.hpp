// SimpleRWLock - atomic-based reader-writer lock for the duck build.
//
// State encoding:
//   state_ >= 0  : reader count
//   state_ == -1 : writer active (unique holder)
//
// Single-process / thread model. Not safe across processes.

#pragma once

#include <atomic>
#include <thread>

namespace vex_duck {

namespace detail {

inline void cpu_pause() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

// Escalating backoff: cpu_pause → batched cpu_pause → std::this_thread::yield.
// Caller bumps `spin` each call; never reset.
inline void backoff(int& spin) noexcept {
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

}  // namespace detail

class SimpleRWLock {
    std::atomic<int> state_{0};

public:
    SimpleRWLock() = default;
    SimpleRWLock(const SimpleRWLock&) = delete;
    SimpleRWLock& operator=(const SimpleRWLock&) = delete;

    void lock_shared() noexcept {
        int spin = 0;
        int s;
        do {
            s = state_.load(std::memory_order_acquire);
            while (s < 0) {
                detail::backoff(spin);
                s = state_.load(std::memory_order_acquire);
            }
        } while (!state_.compare_exchange_weak(s, s + 1,
                                                std::memory_order_acquire,
                                                std::memory_order_relaxed));
    }

    bool try_lock_shared() noexcept {
        int s = state_.load(std::memory_order_acquire);
        while (s >= 0) {
            if (state_.compare_exchange_weak(s, s + 1,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void unlock_shared() noexcept {
        state_.fetch_sub(1, std::memory_order_release);
    }

    void lock() noexcept {
        int spin = 0;
        int expected = 0;
        while (!state_.compare_exchange_weak(expected, -1,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
            expected = 0;
            detail::backoff(spin);
        }
    }

    bool try_lock() noexcept {
        int expected = 0;
        return state_.compare_exchange_strong(expected, -1,
                                               std::memory_order_acquire,
                                               std::memory_order_relaxed);
    }

    void unlock_exclusive() noexcept {
        state_.store(0, std::memory_order_release);
    }

    // Unified unlock — caller does not need to know shared/exclusive mode.
    // Used by PG-style LWLockRelease(LWLock*) shim.
    //
    // Correctness:
    //   - state_ == -1: unique writer holds the lock; the calling thread
    //     must be that writer. store(0) races with no one.
    //   - state_  >  0: N readers hold the lock; no writer can coexist
    //     (lock() requires expected == 0). The calling thread is one of
    //     the readers; fetch_sub(1) is atomic among them.
    //   - reader + writer cannot coexist because lock_shared() spins on s<0.
    //
    // The load can be relaxed: the calling thread already holds the lock
    // (acquired earlier via lock_shared/lock), so prior writes from previous
    // lock-holders are already visible to this thread. Subsequent
    // store(release) / fetch_sub(release) provides publication ordering to
    // the next acquirer.
    void unlock() noexcept {
        int s = state_.load(std::memory_order_relaxed);
        if (s == -1) {
            state_.store(0, std::memory_order_release);
        } else {
            state_.fetch_sub(1, std::memory_order_release);
        }
    }
};

class SharedLockGuard {
    SimpleRWLock& lock_;
public:
    explicit SharedLockGuard(SimpleRWLock& l) : lock_(l) { lock_.lock_shared(); }
    ~SharedLockGuard() { lock_.unlock_shared(); }
    SharedLockGuard(const SharedLockGuard&) = delete;
    SharedLockGuard& operator=(const SharedLockGuard&) = delete;
};

class ExclusiveLockGuard {
    SimpleRWLock& lock_;
public:
    explicit ExclusiveLockGuard(SimpleRWLock& l) : lock_(l) { lock_.lock(); }
    ~ExclusiveLockGuard() { lock_.unlock_exclusive(); }
    ExclusiveLockGuard(const ExclusiveLockGuard&) = delete;
    ExclusiveLockGuard& operator=(const ExclusiveLockGuard&) = delete;
};

}  // namespace vex_duck
