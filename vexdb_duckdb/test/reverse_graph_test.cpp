#include "vex_reverse_graph.hpp"
#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>

namespace {
struct Store {
    using T = uint32_t;
    using point_type = int;
    struct Point { std::vector<T> neighbors; };
    std::vector<int> elems;
    std::vector<Point> base_points;
    uint16_t m = 2, dim = 1;
    std::vector<float> values;
    void *get_data(T id) { return &values[id]; }
    template <bool base> auto get_point_info(T id) { return std::make_tuple(base_points[id].neighbors.data(), id, id); }
};
struct Distance {
    mutable size_t calls = 0, count = 0;
    void get_distance_batch2(const char *, void **values, uint16_t dim, uint16_t n, float *out) const {
        assert(dim == 1); ++calls; count += n;
        for (size_t i = 0; i < n; ++i) out[i] = *static_cast<float *>(values[i]);
    }
};
}

int main() {
    Store store;
    constexpr size_t n = 70001; // exceeds the native SIMD batch count type
    store.elems.resize(n); store.base_points.resize(n); store.values.resize(n);
    for (size_t i = 1; i < n; ++i) store.base_points[i].neighbors.push_back(0);
    for (size_t i = 0; i < n; ++i) store.values[i] = float(i);
    vex_duck::ReverseGraph<uint32_t> cache;
    assert(cache.AllocatedBytes() == 0);
    const auto &links = cache.GetOrBuild(store);
    assert(links.sources.size() == n - 1 && links.offsets[1] == n - 1);
    vex_duck::ReverseGraphSearchStore<Store> view(store, links);
    auto [ids, lower, unused] = view.get_point_info<true>(0);
    assert(view.search_neighbor_count<true>(0) == n - 1 && ids[n - 2] == n - 1);
    std::vector<uint32_t> batch(ids, ids + n - 1);
    std::vector<float> distances(batch.size()); Distance distance;
    view.get_distance_batch(distance, nullptr, batch, distances.data());
    assert(distance.count == batch.size() && distance.calls == 2 && distances.back() == float(n - 1));
    cache.Invalidate(); // readers race to rebuild one immutable cold snapshot
    std::atomic<int> passed {0}; std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i) readers.emplace_back([&] {
        const auto &same = cache.GetOrBuild(store);
        if (&same == &links && same.sources.size() == n - 1) ++passed;
    });
    for (auto &reader : readers) reader.join();
    assert(passed == 8 && cache.AllocatedBytes() >= (n + 1) * sizeof(size_t));
    store.base_points[1].neighbors.clear();
    cache.Invalidate();
    assert(cache.GetOrBuild(store).sources.size() == n - 2);
    store.base_points[1].neighbors = {uint32_t(n)}; cache.Invalidate();
    bool rejected = false;
    try { cache.GetOrBuild(store); } catch (const std::runtime_error &) { rejected = true; }
    assert(rejected);
    std::cout << "PASS reverse graph, high-degree batches, concurrent readers and invalidation\n";
}
