#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>

namespace duckdb {
// One FIFO queue per database, acquired before graph/allocator locks.
class VexSearchQueue {
    struct Waiter { std::condition_variable ready; };
public:
    using Clock = std::chrono::steady_clock;
    struct Cancelled : std::exception {};
    struct Timeout : std::exception {};
    struct Snapshot { uint64_t active, waiting, admitted, cancelled, timed_out, peak_active; };
    class Permit {
    public:
        Permit(const Permit &) = delete;
        Permit &operator=(const Permit &) = delete;
        Permit(Permit &&) = delete;
        Permit &operator=(Permit &&) = delete;
        ~Permit() { queue.Release(fifo); }
    private:
        friend class VexSearchQueue;
        Permit(VexSearchQueue &q, bool fifo_p) noexcept : queue(q), fifo(fifo_p) {}
        VexSearchQueue &queue;
        bool fifo;
    };
    template <class Limit, class IsCancelled>
    Permit Acquire(Limit &&read_limit, IsCancelled &&is_cancelled, uint64_t timeout_ms) {
        const auto begin = Clock::now();
        auto deadline = Clock::time_point::max();
        if (timeout_ms) {
            const auto available = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - begin).count();
            if (timeout_ms < uint64_t(available)) deadline = begin + std::chrono::milliseconds(timeout_ms);
        }
        Check(is_cancelled, deadline);
        // Disabled mode avoids a shared mutex on every query. Reserve first,
        // then recheck both config and waiters: an enabling SET sees this
        // reservation, or we retire it and join FIFO. No untracked searches
        // can slip through a 0 -> enabled transition.
        const auto initial_limit = read_limit();
        if (!waiting.load() && TryReserve(initial_limit)) {
            try {
                const auto latest_limit = read_limit();
                if ((!latest_limit || latest_limit == initial_limit) && !waiting.load()) {
                    Check(is_cancelled, deadline);
                    Admit();
                    return Permit(*this, false);
                }
            } catch (...) { Release(false); throw; }
            Release(false);
        }
        std::unique_lock<std::mutex> lock(mutex);
        Check(is_cancelled, deadline);
        auto limit = read_limit();
        Check(is_cancelled, deadline);
        if (pending.empty() && TryReserve(limit)) {
            Admit();
            return Permit(*this, true);
        }
        Waiter waiter;
        pending.push_back(&waiter);
        waiting.store(pending.size());
        try {
            while (true) {
                // Cancel/expiry wins a race with release, before taking a slot.
                Check(is_cancelled, deadline);
                limit = read_limit();
                Check(is_cancelled, deadline);
                if (pending.front() == &waiter && TryReserve(limit)) {
                    pending.pop_front();
                    waiting.store(pending.size());
                    Admit();
                    NotifyFront();
                    return Permit(*this, true);
                }
                waiter.ready.wait_until(lock, std::min(deadline, Clock::now() + std::chrono::milliseconds(5)));
            }
        } catch (...) {
            pending.erase(std::find(pending.begin(), pending.end(), &waiter));
            waiting.store(pending.size());
            NotifyFront();
            throw;
        }
    }
    Snapshot GetSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex);
        return {active.load(), waiting.load(), admitted.load(), cancelled.load(), timed_out.load(), peak_active.load()};
    }
private:
    bool TryReserve(uint64_t limit) {
        if (!limit) {
            active.fetch_add(1);
            return true;
        }
        auto count = active.load();
        while (count < limit) {
            if (active.compare_exchange_weak(count, count + 1)) return true;
        }
        return false;
    }
    template <class IsCancelled>
    void Check(IsCancelled &is_cancelled, Clock::time_point deadline) {
        if (is_cancelled()) { ++cancelled; throw Cancelled(); }
        if (Clock::now() >= deadline) { ++timed_out; throw Timeout(); }
    }
    void Admit() {
        admitted.fetch_add(1, std::memory_order_relaxed);
        const auto count = active.load();
        auto peak = peak_active.load(std::memory_order_relaxed);
        while (count > peak && !peak_active.compare_exchange_weak(peak, count, std::memory_order_relaxed)) {}
    }
    void NotifyFront() { if (!pending.empty()) pending.front()->ready.notify_one(); }
    void Release(bool fifo) {
        if (fifo) {
            std::lock_guard<std::mutex> lock(mutex);
            --active;
            NotifyFront();
        } else {
            --active;
            if (waiting.load()) {
                std::lock_guard<std::mutex> lock(mutex);
                NotifyFront();
            }
        }
    }
    mutable std::mutex mutex;
    std::deque<Waiter *> pending;
    std::atomic<uint64_t> active {0}, waiting {0}, admitted {0}, cancelled {0}, timed_out {0}, peak_active {0};
};
} // namespace duckdb
