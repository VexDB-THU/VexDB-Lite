/**
 * Copyright (c) 2025.  All rights reserved.
 *
 * Bloom Filter Implementation
 * A space-efficient probabilistic data structure for set membership testing.
 */

#ifndef VTL_BLOOM_FILTER_H
#define VTL_BLOOM_FILTER_H

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <array>
#include <algorithm>

#include "internal/container.hpp"
#include <vtl/vector>

namespace vtl {

/**
 * Bloom Filter - Probabilistic set membership test
 *
 * @tparam T The type of elements to store
 * @tparam Hash The hash function type (defaults to std::hash)
 *
 * False positive rate is approximately (1 - e^(-kn/m))^k where:
 * - m = size of bit array
 * - n = number of inserted elements
 * - k = number of hash functions
 */
template <typename T, typename Hash = std::hash<T>>
class BloomFilter {
public:
    /**
     * Construct a Bloom Filter
     * @param expected_n Expected number of elements to insert
     * @param false_positive_rate Desired false positive rate (default 0.01 = 1%)
     */
    BloomFilter(std::size_t expected_n, double false_positive_rate = 0.01)
        : expected_n_(expected_n), false_positive_rate_(false_positive_rate) {
        // Calculate optimal size: m = -n * ln(p) / (ln(2)^2)
        // And optimal hash functions: k = (m/n) * ln(2)
        double m = -static_cast<double>(expected_n) * std::log(false_positive_rate) / (std::log(2) * std::log(2));
        double k = std::log(2) * m / expected_n;

        size_ = static_cast<std::size_t>(m) + 1;
        num_hashes_ = static_cast<std::size_t>(k) + 1;

        // Allocate bit array (using bytes for simplicity)
        std::size_t num_bytes = (size_ + 7) / 8;
        bits_.resize(num_bytes, 0);
    }

    /**
     * Construct a Bloom Filter with explicit parameters
     * @param size Size of the bit array in bits
     * @param num_hashes Number of hash functions to use
     */
    BloomFilter(std::size_t size, std::size_t num_hashes)
        : size_(size), num_hashes_(num_hashes),
          expected_n_(0), false_positive_rate_(0.0) {
        std::size_t num_bytes = (size_ + 7) / 8;
        bits_.resize(num_bytes, 0);
    }

    /**
     * Default constructor - creates a filter with default capacity
     */
    BloomFilter() : BloomFilter(10000, 0.01) {}

    // Move constructor and assignment
    BloomFilter(BloomFilter&& other) noexcept
        : bits_(std::move(other.bits_)),
          size_(other.size_),
          num_hashes_(other.num_hashes_),
          expected_n_(other.expected_n_),
          false_positive_rate_(other.false_positive_rate_),
          count_(other.count_) {
        other.size_ = 0;
        other.num_hashes_ = 0;
        other.count_ = 0;
    }

    BloomFilter& operator=(BloomFilter&& other) noexcept {
        if (this != &other) {
            bits_ = std::move(other.bits_);
            size_ = other.size_;
            num_hashes_ = other.num_hashes_;
            expected_n_ = other.expected_n_;
            false_positive_rate_ = other.false_positive_rate_;
            count_ = other.count_;
            other.size_ = 0;
            other.num_hashes_ = 0;
            other.count_ = 0;
        }
        return *this;
    }

    // Disable copy
    BloomFilter(const BloomFilter&) = delete;
    BloomFilter& operator=(const BloomFilter&) = delete;

    /**
     * Insert an element into the filter
     * @param value The value to insert
     * @return true if element was definitely not present (inserted successfully)
     *         false if element might already be present (not inserted to avoid duplicate work)
     *
     * Note: This combines contains() + insert() into a single operation for efficiency.
     * Only computes hash once. Return value false could mean:
     * 1. Element actually exists (true positive)
     * 2. Element doesn't exist but bloom filter reports it might (false positive)
     */
    bool insert(const T& value) {
        // Compute hashes once
        auto hashes = compute_hashes(value);

        // Check if element might already exist (all bits are set)
        for (std::size_t i = 0; i < num_hashes_; ++i) {
            if (!get_bit(hashes[i] % size_)) {
                // At least one bit is not set - element definitely doesn't exist
                // Set all bits and return true
                for (std::size_t j = 0; j < num_hashes_; ++j) {
                    set_bit(hashes[j] % size_);
                }
                ++count_;
                return true;
            }
        }

        // All bits are already set - element might exist
        return false;
    }

    /**
     * Force insert an element without checking existence
     * Useful when you know the element doesn't exist and want to avoid the check overhead
     * @param value The value to insert
     */
    void force_insert(const T& value) {
        auto hashes = compute_hashes(value);
        for (std::size_t i = 0; i < num_hashes_; ++i) {
            set_bit(hashes[i] % size_);
        }
        ++count_;
    }

    /**
     * Check if an element might be in the filter
     * @param value The value to check
     * @return true if the element might be in the filter (may be false positive)
     *         false if the element is definitely not in the filter
     */
    bool contains(const T& value) const {
        auto hashes = compute_hashes(value);
        for (std::size_t i = 0; i < num_hashes_; ++i) {
            if (!get_bit(hashes[i] % size_)) {
                return false;  // Definitely not in set
            }
        }
        return true;  // Probably in set (may be false positive)
    }

    /**
     * Clear the filter
     */
    void clear() {
        // Reset all bits to 0
        for (std::size_t i = 0; i < bits_.size(); ++i) {
            bits_[i] = 0;
        }
        count_ = 0;
    }

    /**
     * Get the approximate number of elements in the filter
     */
    std::size_t size() const {
        return count_;
    }

    /**
     * Get the size of the bit array
     */
    std::size_t capacity() const {
        return size_;
    }

    /**
     * Get the number of hash functions used
     */
    std::size_t num_hashes() const {
        return num_hashes_;
    }

    /**
     * Estimate current false positive rate based on current load
     */
    double estimate_false_positive_rate() const {
        if (count_ == 0) return 0.0;
        double k = static_cast<double>(num_hashes_);
        double n = static_cast<double>(count_);
        double m = static_cast<double>(size_);
        return std::pow(1.0 - std::exp(-k * n / m), k);
    }

private:
    /**
     * Double hashing technique to simulate multiple hash functions
     * Uses h1(x) and h2(x) to generate: h_i(x) = h1(x) + i * h2(x)
     */
    std::array<std::size_t, 8> compute_hashes(const T& value) const {
        std::array<std::size_t, 8> hashes;

        // Use two different hash seeds
        std::size_t h1 = Hash{}(value);
        std::size_t h2 = Hash{}(value) ^ 0x5bd1e9955bd1e995ULL;

        for (std::size_t i = 0; i < 8; ++i) {
            hashes[i] = h1 + i * h2;
        }

        return hashes;
    }

    void set_bit(std::size_t index) {
        std::size_t byte_index = index / 8;
        std::size_t bit_offset = index % 8;
        bits_[byte_index] |= (1U << bit_offset);
    }

    bool get_bit(std::size_t index) const {
        std::size_t byte_index = index / 8;
        std::size_t bit_offset = index % 8;
        return (bits_[byte_index] & (1U << bit_offset)) != 0;
    }

    Vector<uint8_t, DEFAULT_ALLOCATOR<uint8_t>, false> bits_;
    std::size_t size_;          // Number of bits
    std::size_t num_hashes_;    // Number of hash functions
    std::size_t expected_n_;    // Expected number of elements
    double false_positive_rate_; // Target false positive rate
    std::size_t count_ = 0;     // Actual number of elements inserted
};

} /* namespace vtl */

#endif /* VTL_BLOOM_FILTER_H */
