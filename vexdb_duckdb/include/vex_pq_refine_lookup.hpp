#pragma once

#include <mutex>
#include <unordered_map>

namespace vex_duck {

// The caller holds the graph's shared lock while using the returned map, and
// its exclusive lock (or an unpublished build/reload phase) when invalidating.
// That keeps the published map stable for the whole refinement query. This
// additional mutex only serializes lazy construction between concurrent readers.
template <class RowId, class StoreId>
class PQRefineLookup {
public:
    using Map = std::unordered_map<RowId, StoreId>;

    template <class Builder>
    const Map &GetOrBuild(Builder &&build) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (dirty_) {
            entries_.clear();
            build(entries_);
            // Keep dirty_ true if allocation or the builder throws, so the next
            // reader retries instead of observing a partially populated map.
            dirty_ = false;
        }
        return entries_;
    }

    void Invalidate() {
        std::lock_guard<std::mutex> guard(mutex_);
        dirty_ = true;
    }

private:
    std::mutex mutex_;
    Map entries_;
    bool dirty_ = true;
};

} // namespace vex_duck
