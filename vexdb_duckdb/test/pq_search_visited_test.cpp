#include <cassert>
#define Assert(condition) assert(condition)
#include "vex_graph_index_depend_duck.hpp"
#include "vex_pq_search_visited.hpp"

#include <cstdint>
#include <iostream>
#include <random>
#include <unordered_set>

static void Check(size_t nodes, size_t hint, bool bitmap) {
    vex_duck::PQSearchVisited<uint32_t> actual(nodes, hint);
    assert(actual.uses_bitmap() == bitmap);
    if (bitmap) assert(actual.bitmap_bytes() == ((nodes + 63) / 64) * 8);
    std::unordered_set<uint32_t> expected;
    std::mt19937 random(7219);
    for (size_t i = 0; i < 100000; ++i) {
        const uint32_t id = random() % nodes;
        assert(actual.insert(id).second == expected.insert(id).second);
    }
    auto moved = std::move(actual);
    for (const auto id : expected) assert(!moved.insert(id).second);
    vex_duck::PQSearchVisited<uint32_t> independent(nodes, hint);
    for (const auto id : expected) assert(independent.insert(id).second);
    bool rejected = false;
    try { moved.insert(static_cast<uint32_t>(nodes)); }
    catch (const std::out_of_range &) { rejected = true; }
    assert(rejected);
}

int main() {
    Check(1, 2, true);
    Check(63, 2, true);
    Check(64, 2, true);
    Check(65, 4, true);
    Check(20000, 49152, true);
    Check(100000, 49152, true);
    Check(20000, 1, false);
    Check(1000000000, 1024, false);
    // Repeated sparse construction/destruction is also exercised under ASan.
    for (int i = 0; i < 1024; ++i) {
        vex_duck::PQSearchVisited<uint32_t> sparse(1000000000, 8);
        assert(sparse.insert(i).second);
    }
    std::cout << "PASS dense/sparse membership, boundaries, independent queries and lifetime\n";
}
