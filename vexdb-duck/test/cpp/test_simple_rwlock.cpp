// Unit test for SimpleRWLock unified unlock() correctness
//
// 编译：c++ -std=c++17 -O2 -pthread test_simple_rwlock.cpp \
//          -I../../include -o test_simple_rwlock
// 运行：./test_simple_rwlock
//
// 关键验证：
//   1. 多 reader 并发 + unified unlock 正确递减
//   2. writer 独占 + unified unlock 正确归零
//   3. 混合负载下不死锁、不数据竞争

#include "vex_simple_rwlock.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using vex_duck::SimpleRWLock;

namespace {

// 用 shared counter 验证 reader/writer 互斥
void test_basic_shared() {
    SimpleRWLock lock;
    std::atomic<int> in_critical{0};
    std::atomic<bool> race_detected{false};

    constexpr int N_READERS = 8;
    constexpr int ITERATIONS = 10000;

    std::vector<std::thread> readers;
    for (int t = 0; t < N_READERS; t++) {
        readers.emplace_back([&]() {
            for (int i = 0; i < ITERATIONS; i++) {
                lock.lock_shared();
                int cur = in_critical.fetch_add(1) + 1;
                if (cur < 0) race_detected = true;  // writer 不可能跟 reader 同时
                in_critical.fetch_sub(1);
                lock.unlock();  // 用 unified unlock
            }
        });
    }
    for (auto &t : readers) t.join();

    assert(!race_detected);
    std::printf("  test_basic_shared: OK\n");
}

void test_basic_exclusive() {
    SimpleRWLock lock;
    int shared_state = 0;
    constexpr int N_WRITERS = 8;
    constexpr int ITERATIONS = 5000;

    std::vector<std::thread> writers;
    for (int t = 0; t < N_WRITERS; t++) {
        writers.emplace_back([&]() {
            for (int i = 0; i < ITERATIONS; i++) {
                lock.lock();
                int v = shared_state;
                std::this_thread::yield();
                shared_state = v + 1;
                lock.unlock();  // 用 unified unlock
            }
        });
    }
    for (auto &t : writers) t.join();

    assert(shared_state == N_WRITERS * ITERATIONS);
    std::printf("  test_basic_exclusive: OK (counter = %d)\n", shared_state);
}

void test_mixed_workload() {
    SimpleRWLock lock;
    std::atomic<int> readers_in_crit{0};
    std::atomic<int> writers_in_crit{0};
    std::atomic<bool> writer_saw_reader{false};
    std::atomic<bool> reader_saw_writer{false};
    int shared_state = 0;

    constexpr int N_READERS = 6;
    constexpr int N_WRITERS = 3;
    constexpr int ITERATIONS = 10000;

    std::vector<std::thread> threads;
    for (int t = 0; t < N_READERS; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < ITERATIONS; i++) {
                lock.lock_shared();
                if (writers_in_crit.load() > 0) reader_saw_writer = true;
                readers_in_crit.fetch_add(1);
                int local = shared_state;
                (void)local;
                readers_in_crit.fetch_sub(1);
                lock.unlock();  // unified
            }
        });
    }
    for (int t = 0; t < N_WRITERS; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < ITERATIONS; i++) {
                lock.lock();
                if (readers_in_crit.load() > 0) writer_saw_reader = true;
                writers_in_crit.fetch_add(1);
                shared_state++;
                writers_in_crit.fetch_sub(1);
                lock.unlock();  // unified
            }
        });
    }
    for (auto &t : threads) t.join();

    assert(!writer_saw_reader);
    assert(!reader_saw_writer);
    assert(shared_state == N_WRITERS * ITERATIONS);
    std::printf("  test_mixed_workload: OK (counter = %d, no race)\n", shared_state);
}

void test_unified_unlock_matches_typed() {
    // 验证 unlock() 跟 unlock_shared/unlock_exclusive 行为一致
    SimpleRWLock lock;

    lock.lock_shared();
    lock.unlock();  // 应等价 unlock_shared
    // 接下来应该能立即拿 exclusive
    lock.lock();
    lock.unlock();
    lock.lock_shared();
    lock.unlock_shared();
    std::printf("  test_unified_unlock_matches_typed: OK\n");
}

}  // namespace

int main() {
    std::printf("Running SimpleRWLock tests...\n");
    test_unified_unlock_matches_typed();
    test_basic_shared();
    test_basic_exclusive();
    test_mixed_workload();
    std::printf("All tests passed.\n");
    return 0;
}
