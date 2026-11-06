#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <vector>

namespace vex_duck {

// Query-local membership only. Sparse/local transaction row IDs retain a hash
// set; a dense range uses at most as many bytes as the input row-ID array.
template <class Row> class RowIdFilter {
public:
    template <class Rows> explicit RowIdFilter(const Rows &rows) {
        if (rows.empty()) return;
        const auto limits = std::minmax_element(rows.begin(), rows.end());
        low_ = *limits.first;
        high_ = *limits.second;
        if (low_ >= 0) {
            const auto words = (uint64_t(high_) - uint64_t(low_)) / 64 + 1;
            if (words <= rows.size() && words <= bits_.max_size()) {
                bits_.assign(size_t(words), 0);
                for (auto row : rows) {
                    const auto offset = uint64_t(row) - uint64_t(low_);
                    bits_[offset / 64] |= uint64_t(1) << (offset % 64);
                }
                return;
            }
        }
        sparse_.reserve(rows.size());
        sparse_.insert(rows.begin(), rows.end());
    }

    bool Contains(Row row) const {
        if (bits_.empty()) return sparse_.find(row) != sparse_.end();
        if (row < low_ || row > high_) return false;
        const auto offset = uint64_t(row) - uint64_t(low_);
        return (bits_[offset / 64] & (uint64_t(1) << (offset % 64))) != 0;
    }

    size_t DenseBytes() const { return bits_.size() * sizeof(uint64_t); }

private:
    Row low_ = 0, high_ = 0;
    std::vector<uint64_t> bits_;
    std::unordered_set<Row> sparse_;
};

} // namespace vex_duck
