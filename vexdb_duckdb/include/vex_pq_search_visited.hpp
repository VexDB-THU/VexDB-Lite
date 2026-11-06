#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>
#include <vtl/hashtable>

namespace vex_duck {

// Query-local membership for dense graph node IDs. Keep the existing hash
// implementation when a bitmap would cost more than the reserved hash keys.
// No pooled generations or shared mutable state are needed between queries.
template <typename T>
class PQSearchVisited {
    static_assert(std::is_unsigned<T>::value && sizeof(T) <= sizeof(uint64_t), "unsigned graph IDs required");
public:
    PQSearchVisited(size_t nodes, size_t reserve_hint) : nodes_(nodes) {
        const size_t words = nodes / 64 + (nodes % 64 != 0);
        if (words <= reserve_hint / (sizeof(uint64_t) / sizeof(T))) {
            bits_.resize(words, 0);
        } else {
            sparse_.reset(new UnorderedSet<T>(reserve_hint));
        }
    }

    PQSearchVisited(PQSearchVisited &&) = default;
    PQSearchVisited &operator=(PQSearchVisited &&) = delete;
    PQSearchVisited(const PQSearchVisited &) = delete;
    PQSearchVisited &operator=(const PQSearchVisited &) = delete;
    ~PQSearchVisited() {
        // vtl containers use explicit destruction; STL containers do not.
        if (sparse_) ann_helper::optional_destroy(*sparse_);
    }

    std::pair<T, bool> insert(T id) {
        if (static_cast<size_t>(id) >= nodes_) {
            throw std::out_of_range("pq_graph: visited node outside graph");
        }
        if (sparse_) return {id, sparse_->insert(id).second};
        auto &word = bits_[static_cast<size_t>(id) / 64];
        const uint64_t mask = uint64_t(1) << (static_cast<size_t>(id) % 64);
        const bool fresh = (word & mask) == 0;
        word |= mask;
        return {id, fresh};
    }

    bool uses_bitmap() const { return !sparse_; }
    size_t bitmap_bytes() const { return bits_.size() * sizeof(uint64_t); }

private:
    size_t nodes_;
    std::vector<uint64_t> bits_;
    std::unique_ptr<UnorderedSet<T>> sparse_;
};

} // namespace vex_duck
