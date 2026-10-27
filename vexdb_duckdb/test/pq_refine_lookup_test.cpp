#include "vex_pq_refine_lookup.hpp"
#include "vex_pq_node_codes.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using Lookup = vex_duck::PQRefineLookup<int64_t, uint32_t>;

static void ConcurrentColdQueries() {
    Lookup lookup;
    std::shared_mutex graph_lock;
    std::atomic<int> ready {0}, builds {0};
    std::atomic<bool> start {false};
    std::vector<std::thread> readers;
    for (int worker = 0; worker < 16; ++worker) {
        readers.emplace_back([&] {
            std::shared_lock<std::shared_mutex> graph_guard(graph_lock);
            ++ready;
            while (!start.load()) std::this_thread::yield();
            for (int query = 0; query < 100; ++query) {
                const auto &map = lookup.GetOrBuild([&](auto &entries) {
                    ++builds;
                    entries.reserve(65536);
                    for (uint32_t id = 0; id < 65536; ++id) entries[id * 3] = id;
                });
                assert(map.size() == 65536);
                assert(map.at(196605) == 65535);
                assert(map.find(1) == map.end());
            }
        });
    }
    while (ready.load() != 16) std::this_thread::yield();
    start = true;
    for (auto &reader : readers) reader.join();
    assert(builds == 1);
    std::cout << "PASS 16 concurrent cold readers, 1600 lookups, one build\n";
}

static void QueryAndWriteEpochs() {
    Lookup lookup;
    std::shared_mutex graph_lock;
    uint32_t epoch = 0;
    std::atomic<bool> start {false};
    std::vector<std::thread> readers;
    for (int worker = 0; worker < 8; ++worker) {
        readers.emplace_back([&] {
            while (!start.load()) std::this_thread::yield();
            for (int query = 0; query < 1000; ++query) {
                std::shared_lock<std::shared_mutex> graph_guard(graph_lock);
                const auto &map = lookup.GetOrBuild([&](auto &entries) {
                    entries[7] = epoch;
                    entries[17] = epoch + 1000;
                });
                assert(map.at(7) == epoch);
                assert(map.at(17) == epoch + 1000);
                assert(map.size() == 2);
            }
        });
    }
    start = true;
    for (int write = 0; write < 100; ++write) {
        std::unique_lock<std::shared_mutex> graph_guard(graph_lock);
        ++epoch;
        lookup.Invalidate();
    }
    for (auto &reader : readers) reader.join();
    std::shared_lock<std::shared_mutex> graph_guard(graph_lock);
    const auto &last = lookup.GetOrBuild([&](auto &entries) { entries[7] = epoch; entries[17] = epoch + 1000; });
    assert(last.at(7) == 100);
    std::cout << "PASS 8000 concurrent reads with 100 write invalidations\n";
}

static void RebuildChangesStoreIds() {
    Lookup lookup;
    assert(lookup.GetOrBuild([](auto &entries) { entries[100] = 5; entries[200] = 9; }).at(100) == 5);
    lookup.Invalidate();
    const auto &rebuilt = lookup.GetOrBuild([](auto &entries) { entries[100] = 2; entries[300] = 8; });
    assert(rebuilt.at(100) == 2);
    assert(rebuilt.find(200) == rebuilt.end());
    assert(rebuilt.at(300) == 8);
    std::cout << "PASS replacement store drops obsolete row ids and store ids\n";
}

static void FailedBuildIsRetried() {
    Lookup lookup;
    bool threw = false;
    try {
        lookup.GetOrBuild([](auto &entries) {
            entries[999] = 999;
            throw std::runtime_error("simulated build failure");
        });
    } catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);
    const auto &retried = lookup.GetOrBuild([](auto &entries) { entries[123] = 12; });
    assert(retried.size() == 1 && retried.at(123) == 12);
    assert(retried.find(999) == retried.end());
    std::cout << "PASS partial failure is retried without publishing partial entries\n";
}

static void NodeCodesRemainStableAcrossWritesAndFailedBuilds() {
    vex_duck::PQNodeCodes<uint64_t> codes;
    std::shared_mutex graph_lock;
    uint64_t epoch = 0;
    std::atomic<int> ready {0}, builds {0};
    std::atomic<bool> start {false};
    auto build = [&](auto &slots) { ++builds; slots.assign(1000, epoch); };
    std::vector<std::thread> readers;
    for (int worker = 0; worker < 16; ++worker) {
        readers.emplace_back([&] {
            ++ready;
            while (!start.load()) std::this_thread::yield();
            for (int q = 0; q < 1000; ++q) {
                std::shared_lock<std::shared_mutex> guard(graph_lock);
                const auto &slots = codes.GetOrBuild(build);
                assert(slots.size() == 1000 && slots.front() == epoch && slots.back() == epoch);
                assert(codes.AllocatedBytes() >= 8000);
            }
        });
    }
    while (ready.load() != 16) std::this_thread::yield();
    start = true;
    for (int write = 0; write < 100; ++write) {
        std::unique_lock<std::shared_mutex> guard(graph_lock);
        ++epoch;
        codes.Invalidate();
    }
    for (auto &reader : readers) reader.join();
    assert(builds >= 1 && builds <= 101);
    codes.Invalidate();
    bool threw = false;
    try {
        codes.GetOrBuild([](auto &slots) { slots.assign(23, 999); throw std::runtime_error("allocation failure"); });
    } catch (const std::runtime_error &) { threw = true; }
    assert(threw);
    const auto &retried = codes.GetOrBuild([](auto &slots) { slots.assign(7, 123); });
    assert(retried.size() == 7 && retried.front() == 123 && retried.back() == 123);
    std::cout << "PASS node-code cache: 16000 concurrent reads, 100 writes, failed build retried\n";
}

int main() {
    ConcurrentColdQueries();
    QueryAndWriteEpochs();
    RebuildChangesStoreIds();
    FailedBuildIsRetried();
    NodeCodesRemainStableAcrossWritesAndFailedBuilds();
}
