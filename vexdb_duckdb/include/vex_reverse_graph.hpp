#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace vex_duck {

// Topology only: no vector distances or query answers are stored. Callers keep
// the graph shared lock while using Data; invalidation requires its write lock.
template <class T> class ReverseGraph {
public:
    struct Data {
        std::vector<size_t> offsets;
        std::vector<T> sources;
    };

    template <class Store> const Data &GetOrBuild(const Store &store) const {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!dirty_) return data_;
        const auto n = store.elems.size();
        if (store.base_points.size() != n) throw std::runtime_error("reverse graph: incomplete neighbor mirror");
        Data next;
        next.offsets.assign(n + 1, 0);
        for (size_t v = 0; v < n; ++v) {
            for (auto w : store.base_points[v].neighbors) {
                if (w == std::numeric_limits<T>::max()) break;
                if (size_t(w) >= n) throw std::runtime_error("reverse graph: invalid neighbor");
                ++next.offsets[size_t(w) + 1];
            }
        }
        for (size_t v = 0; v < n; ++v) next.offsets[v + 1] += next.offsets[v];
        next.sources.resize(next.offsets.back());
        auto cursor = next.offsets;
        for (size_t v = 0; v < n; ++v) {
            for (auto w : store.base_points[v].neighbors) {
                if (w == std::numeric_limits<T>::max()) break;
                next.sources[cursor[w]++] = T(v);
            }
        }
        data_ = std::move(next);
        dirty_ = false;
        return data_;
    }

    void Invalidate() {
        std::lock_guard<std::mutex> guard(mutex_);
        dirty_ = true;
    }

    size_t AllocatedBytes() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return data_.offsets.capacity() * sizeof(size_t) + data_.sources.capacity() * sizeof(T);
    }

private:
    mutable std::mutex mutex_;
    mutable Data data_;
    mutable bool dirty_ = true;
};

// One scratch buffer per query; graph metadata stays immutable across readers.
template <class Store> class ReverseNeighbors {
public:
    using T = typename Store::T;
    ReverseNeighbors(Store &store, const typename ReverseGraph<T>::Data *reverse)
        : store_(store), reverse_(reverse) {}

    template <bool base> auto Get(T id) {
        if constexpr (base) {
            if (!reverse_) return std::make_tuple(store_.base_points[id].neighbors.data(), id, id);
            if (size_t(id) + 1 >= reverse_->offsets.size()) throw std::runtime_error("reverse graph: node outside graph");
            neighbors_.clear();
            for (auto w : store_.base_points[id].neighbors) {
                if (w == std::numeric_limits<T>::max()) break;
                neighbors_.push_back(w);
            }
            const auto begin = reverse_->offsets[id], end = reverse_->offsets[size_t(id) + 1];
            neighbors_.insert(neighbors_.end(), reverse_->sources.begin() + begin, reverse_->sources.begin() + end);
            return std::make_tuple(neighbors_.data(), id, id);
        } else {
            return store_.template get_point_info<false>(id);
        }
    }

    template <bool base> size_t Count(T) const {
        if constexpr (base) return reverse_ ? neighbors_.size() : size_t(store_.m) * 2;
        return store_.m;
    }

private:
    Store &store_;
    const typename ReverseGraph<T>::Data *reverse_;
    std::vector<T> neighbors_;
};

// Raw-metric counterpart to PQGraphSearchStore. The index shared lock excludes
// writers, so node locks are unnecessary; all metric calculation delegates to
// the existing distancer. The wrapper is used only by explicit reverse search.
template <class Store> class ReverseGraphSearchStore {
public:
    using T = typename Store::T;
    using point_type = typename Store::point_type;
    static constexpr bool use_dist_cache = false;
    ReverseGraphSearchStore(Store &store, const typename ReverseGraph<T>::Data &reverse)
        : store_(store), neighbors_(store, &reverse) {}

    template <bool base> auto get_point_info(T id) { return neighbors_.template Get<base>(id); }
    template <bool base> size_t search_neighbor_count(T id) const { return neighbors_.template Count<base>(id); }
    template <bool shared> auto get_entry(int_fast8_t level = 0) { return store_.template get_entry<shared>(level); }
    template <bool base, bool shared> void lock_point(T) { static_assert(shared, "search is read-only"); }
    template <bool base, bool shared> void unlock_point(T) { static_assert(shared, "search is read-only"); }
    template <class Fn> void get_itempointer(T id, Fn &&fn) { store_.get_itempointer(id, std::forward<Fn>(fn)); }
    template <class Distance> float get_distance(const Distance &d, const char *query, T id) {
        return store_.get_distance(d, query, id);
    }
    template <class Distance, class Ids>
    void get_distance_batch(const Distance &d, const char *query, const Ids &ids, float *out) {
        // The existing SIMD batch API uses uint16 lengths. Reverse degree can
        // exceed that type, so split by its actual limit, never truncate IDs.
        constexpr size_t limit = std::numeric_limits<uint16_t>::max();
        values_.reserve(std::min<size_t>(ids.size(), limit));
        for (size_t begin = 0; begin < ids.size(); begin += limit) {
            const auto count = std::min<size_t>(limit, ids.size() - begin);
            values_.clear();
            for (size_t i = 0; i < count; ++i) values_.push_back(store_.get_data(ids[begin + i]));
            d.get_distance_batch2(query, values_.data(), uint16_t(store_.dim), uint16_t(count), out + begin);
        }
    }

private:
    Store &store_;
    ReverseNeighbors<Store> neighbors_;
    std::vector<void *> values_;
};

} // namespace vex_duck
