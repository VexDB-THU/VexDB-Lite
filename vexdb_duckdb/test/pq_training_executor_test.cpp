#include "vex_pq_training_executor.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <iostream>
#include <random>
#include <stdexcept>

static void BoundedWorkers(size_t limit) {
    vex_duck::PQTrainingExecutor executor(limit);
    std::atomic<int> active {0}, peak {0}, done {0};
    std::array<uint32_t, 32> first_random {};
    vex_duck::PQTrainingExecutor::Run(32, [&](size_t task) {
        const int count = ++active;
        int previous = peak.load();
        while (count > previous && !peak.compare_exchange_weak(previous, count)) {}
        // Match the shared quantizer's default thread-local RNG. A reused
        // worker pool would advance this state and silently change codebooks.
        thread_local std::mt19937 random(42);
        first_random[task] = random();
        for (int spin = 0; spin < 100; ++spin) std::this_thread::yield();
        // Nested K-means batches remain serial on their outer worker thread.
        const auto worker = std::this_thread::get_id();
        vex_duck::PQTrainingExecutor::Run(256, [&](size_t) {
            assert(std::this_thread::get_id() == worker);
        }, &executor);
        --active;
        ++done;
    }, &executor);
    assert(done == 32 && active == 0 && peak > 0 && size_t(peak.load()) <= limit);
    std::mt19937 expected(42);
    const auto seed_first = expected();
    for (auto value : first_random) assert(value == seed_first);
    std::cout << "PASS 32 tasks, worker limit " << limit << ", fresh RNG and no nested workers\n";
}

int main() {
    for (size_t limit : {1, 2, 4}) BoundedWorkers(limit);
    vex_duck::PQTrainingExecutor executor(4);
    const auto caller = std::this_thread::get_id();
    size_t count = 0;
    vex_duck::PQTrainingExecutor::Run(384, [&](size_t) {
        assert(std::this_thread::get_id() == caller);
        ++count;
    }, &executor);
    assert(count == 384);
    std::cout << "PASS existing large-M serial path preserved\n";
    std::atomic<int> completed {0};
    bool threw = false;
    try {
        vex_duck::PQTrainingExecutor::Run(32, [&](size_t task) {
            if (task == 0) throw std::runtime_error("training failed");
            ++completed;
        }, &executor);
    } catch (const std::runtime_error &) { threw = true; }
    assert(threw && completed == 3);
    std::cout << "PASS first failed wave joins every worker before rethrowing\n";
}
