#include "vex_search_queue.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using duckdb::VexSearchQueue;
using namespace std::chrono_literals;
template <class P> static void Until(P pred) {
    const auto end = VexSearchQueue::Clock::now() + 5s;
    while (!pred()) { if (VexSearchQueue::Clock::now() >= end) std::abort(); std::this_thread::yield(); }
}
static void Require(bool ok) { if (!ok) throw std::runtime_error("queue transition invariant failed"); }
static void Empty(VexSearchQueue &q) { auto s = q.GetSnapshot(); Require(!s.active && !s.waiting); }
int main() {
    try {
        // Suspend the default-off reservation just as another thread enables
        // admission. It must retire its reservation and join the existing FIFO.
        VexSearchQueue q;
        std::atomic<uint64_t> limit {0};
        std::atomic<int> reads {0};
        std::atomic<bool> resume {false};
        std::vector<int> order;
        std::thread a([&] {
            auto read = [&] {
                if (++reads == 2) Until([&] { return resume.load(); });
                return limit.load();
            };
            auto p = q.Acquire(read, [] { return false; }, 0);
            order.push_back(1);
        });
        Until([&] { return reads == 2; });
        Require(q.GetSnapshot().active == 1);
        limit = 1;
        std::thread b([&] {
            auto p = q.Acquire([&] { return limit.load(); }, [] { return false; }, 0);
            order.push_back(2);
        });
        Until([&] { return q.GetSnapshot().waiting == 1; });
        resume = true;
        a.join(); b.join();
        Require(order == std::vector<int>({2, 1}));
        Empty(q);
        Require(q.GetSnapshot().admitted == 2);
        std::cout << "PASS enabling during a default-off reservation preserves FIFO and accounting\n";
        for (bool cancel : {false, true}) {
            VexSearchQueue queue;
            std::atomic<int> n {0};
            bool got_error = false;
            try {
                auto read = [&]() -> uint64_t {
                    if (++n == 2 && !cancel) throw std::runtime_error("configuration read failed");
                    return 0;
                };
                auto p = queue.Acquire(read, [&] { return cancel && n == 2; }, 0);
            } catch (...) { got_error = true; }
            Require(got_error);
            Empty(queue);
            Require(!queue.GetSnapshot().admitted);
        }
        std::cout << "PASS cancelled/throwing default-off reservation is released exactly once\n";
        {
            VexSearchQueue queue;
            bool expired = false;
            try {
                auto p = queue.Acquire([] { std::this_thread::sleep_for(20ms); return 1u; }, [] { return false; }, 1);
            } catch (const VexSearchQueue::Timeout &) { expired = true; }
            Require(expired);
            Empty(queue);
        }
        std::cout << "PASS expired before first admission, no late slot\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
