#pragma once

#include <mutex>
#include <vector>

namespace vex_duck {

// As with PQRefineLookup, callers retain the graph shared lock while using
// this vector; invalidation requires its exclusive lock or an unpublished index.
template <class Slot>
class PQNodeCodes {
public:
    template <class Builder>
    const std::vector<Slot> &GetOrBuild(Builder &&build) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (dirty_) {
            std::vector<Slot> next;
            build(next);
            slots_.swap(next);
            dirty_ = false;
        }
        return slots_;
    }

    void Invalidate() {
        std::lock_guard<std::mutex> guard(mutex_);
        dirty_ = true;
    }

    size_t AllocatedBytes() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return slots_.capacity() * sizeof(Slot);
    }

private:
    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
    bool dirty_ = true;
};

} // namespace vex_duck
