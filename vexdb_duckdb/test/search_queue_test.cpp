#include "vex_search_queue.hpp"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

using duckdb::VexSearchQueue;
using namespace std::chrono_literals;
static_assert(!std::is_copy_constructible<VexSearchQueue::Permit>::value, "single owner");
static_assert(!std::is_move_constructible<VexSearchQueue::Permit>::value, "single release");
static void Require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
template <class Predicate> static void Until(Predicate pred) {
    const auto deadline = VexSearchQueue::Clock::now() + 5s;
    while (!pred()) {
        if (VexSearchQueue::Clock::now() >= deadline) std::abort();
        std::this_thread::sleep_for(1ms);
    }
}
static void Empty(VexSearchQueue &queue) {
    const auto s = queue.GetSnapshot();
    Require(s.active == 0 && s.waiting == 0, "leaked active permit or waiter");
}

int main() {
    try {
        std::atomic<uint64_t> limit {1};
        auto read = [&] { return limit.load(); };
        auto no_cancel = [] { return false; };
        {
            VexSearchQueue queue;
            std::vector<std::thread> threads;
            std::vector<int> order;
            {
                auto held = queue.Acquire(read, no_cancel, 0);
                for (int i = 0; i < 12; ++i) {
                    threads.emplace_back([&, i] {
                        auto p = queue.Acquire(read, no_cancel, 0);
                        order.push_back(i); // only one permit, no other writer
                    });
                    Until([&] { return queue.GetSnapshot().waiting == uint64_t(i + 1); });
                }
            }
            for (auto &t : threads) t.join();
            for (int i = 0; i < 12; ++i) Require(order[i] == i, "FIFO bypass");
            Empty(queue);
            Require(queue.GetSnapshot().admitted == 13, "missing work");
            std::cout << "PASS FIFO handoff, all work admitted\n";
        }
        {
            VexSearchQueue queue;
            std::atomic<bool> stop {false}, cancelled {false}, expired {false};
            {
                auto held = queue.Acquire(read, no_cancel, 0);
                std::thread first([&] {
                    try { auto p = queue.Acquire(read, [&] { return stop.load(); }, 0); }
                    catch (const VexSearchQueue::Cancelled &) { cancelled = true; }
                });
                Until([&] { return queue.GetSnapshot().waiting == 1; });
                std::thread second([&] {
                    const auto begin = VexSearchQueue::Clock::now();
                    try { auto p = queue.Acquire(read, no_cancel, 30); }
                    catch (const VexSearchQueue::Timeout &) {
                        Require(VexSearchQueue::Clock::now() - begin >= 30ms, "early timeout");
                        expired = true;
                    }
                });
                Until([&] { return queue.GetSnapshot().waiting == 2; });
                stop = true;
                first.join(); second.join();
                Require(cancelled && expired, "cancel or timeout not delivered");
                Require(queue.GetSnapshot().active == 1, "waiting error released another permit");
            }
            { auto p = queue.Acquire(read, no_cancel, 1); }
            Empty(queue);
            Require(queue.GetSnapshot().cancelled == 1 && queue.GetSnapshot().timed_out == 1, "error counters");
            std::cout << "PASS cancelled head, expired middle, no phantom permit\n";
        }
        // Race cancellation against release repeatedly; either completion or
        // cancellation is legal, but every slot must be recovered exactly once.
        for (int i = 0; i < 200; ++i) {
            VexSearchQueue queue;
            std::atomic<bool> stop {false};
            std::thread waiter;
            {
                auto held = queue.Acquire(read, no_cancel, 0);
                waiter = std::thread([&] {
                    try { auto p = queue.Acquire(read, [&] { return stop.load(); }, 0); }
                    catch (const VexSearchQueue::Cancelled &) {}
                });
                Until([&] { return queue.GetSnapshot().waiting == 1; });
                if (i % 2) stop = true;
            }
            stop = true;
            waiter.join();
            Empty(queue);
        }
        std::cout << "PASS 200 cancellation/release races\n";
        {
            VexSearchQueue queue;
            limit = 0;
            std::atomic<bool> entered {false};
            std::thread waiter;
            {
                auto first = queue.Acquire(read, no_cancel, 0);
                {
                    auto second = queue.Acquire(read, no_cancel, 0);
                    limit = 1;
                    waiter = std::thread([&] { auto p = queue.Acquire(read, no_cancel, 0); entered = true; });
                    Until([&] { return queue.GetSnapshot().waiting == 1; });
                }
                std::this_thread::sleep_for(15ms);
                Require(!entered && queue.GetSnapshot().active == 1, "lowered cap admitted too early");
            }
            waiter.join();
            Require(entered, "lowered cap failed to drain");
            Empty(queue);
            std::cout << "PASS lowered cap drains existing work without new admission\n";
        }
        {
            VexSearchQueue queue;
            std::thread waiter;
            std::atomic<bool> entered {false};
            {
                auto held = queue.Acquire(read, no_cancel, UINT64_MAX);
                waiter = std::thread([&] { auto p = queue.Acquire(read, no_cancel, 0); entered = true; });
                Until([&] { return queue.GetSnapshot().waiting == 1; });
                limit = 2; // a new limit wakes through the regular poll
                Until([&] { return entered.load(); });
                waiter.join();
                limit = 0;
                { auto unlimited = queue.Acquire(read, no_cancel, 0); }
                limit = 1;
                Require(queue.GetSnapshot().active == 1, "disabled queue lost active accounting");
            }
            try { auto p = queue.Acquire(read, no_cancel, 0); throw std::runtime_error("execution failed"); }
            catch (const std::runtime_error &) {}
            Empty(queue);
            std::cout << "PASS resize, disable, huge deadline, exception release\n";
        }
        {
            VexSearchQueue queue;
            limit = 3;
            std::atomic<int> work {0}, in_body {0};
            std::vector<std::thread> threads;
            for (int i = 0; i < 16; ++i) threads.emplace_back([&] {
                for (int j = 0; j < 1000; ++j) {
                    auto p = queue.Acquire(read, no_cancel, 0);
                    Require(++in_body <= 3, "concurrency cap exceeded");
                    std::this_thread::yield();
                    ++work;
                    --in_body;
                }
            });
            for (auto &t : threads) t.join();
            Require(work == 16000, "dropped requests");
            Empty(queue);
            std::cout << "PASS 16000 requests, cap3, 16 producers\n";
        }
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
