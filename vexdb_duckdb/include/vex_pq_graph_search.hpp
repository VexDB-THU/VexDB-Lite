#pragma once

namespace vex_duck {

struct PQGraphDistance {
    static constexpr bool need_refine = false;
    static constexpr bool has_estimation_func = false;
};

// Read-only view of the existing graph. The caller holds the index shared lock
// for the entire traversal, so per-node locks are unnecessary. Topology comes
// from Store; navigation distances read PQ codes only, never raw vectors.
template <class Store, class Quantizer, class Slots>
class PQGraphSearchStore {
public:
    using T = typename Store::T;
    using point_type = typename Store::point_type;
    static constexpr bool use_dist_cache = false;

    PQGraphSearchStore(Store &store, Quantizer &quantizer, const Slots &slots,
                       const uint8_t *codes, const float *table)
        : store_(store), quantizer_(quantizer), slots_(slots), codes_(codes), table_(table) {}

    template <bool base_layer>
    auto get_point_info(T id) { return store_.template get_point_info<base_layer>(id); }

    template <bool base_layer, bool shared>
    void lock_point(T) { static_assert(shared, "PQ graph search is read-only"); }
    template <bool base_layer, bool shared>
    void unlock_point(T) { static_assert(shared, "PQ graph search is read-only"); }

    float get_distance(const PQGraphDistance &, const char *, T id) {
        ++codes_scored;
        return quantizer_.distance_to_code(Code(id), table_);
    }

    template <class Ids>
    void get_distance_batch(const PQGraphDistance &, const char *, const Ids &ids, float *out) {
        codes_scored += ids.size();
        size_t i = 0;
        for (; i + 4 <= ids.size(); i += 4) {
            quantizer_.distance_to_four_code(table_, Code(ids[i]), Code(ids[i + 1]),
                                             Code(ids[i + 2]), Code(ids[i + 3]),
                                             out[i], out[i + 1], out[i + 2], out[i + 3]);
        }
        for (; i < ids.size(); ++i) out[i] = quantizer_.distance_to_code(Code(ids[i]), table_);
    }

    size_t codes_scored = 0; // distance evaluations, including repeat visits across layers

private:
    const uint8_t *Code(T id) const {
        if (id >= slots_.size() || slots_[id] == std::numeric_limits<typename Slots::value_type>::max()) {
            throw duckdb::InvalidInputException("pq_graph: visited graph node has no PQ code; rebuild the index");
        }
        return codes_ + slots_[id] * quantizer_.code_size;
    }
    Store &store_;
    Quantizer &quantizer_;
    const Slots &slots_;
    const uint8_t *codes_;
    const float *table_;
};

} // namespace vex_duck
