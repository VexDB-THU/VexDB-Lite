#pragma once

#include <tuple>
#include "vex_pq_search_visited.hpp"
#include "vex_reverse_graph.hpp"
#include "vex_search_profile.hpp"

namespace vex_duck {

struct PQGraphDistance {
    static constexpr bool need_refine = false;
    static constexpr bool has_estimation_func = false;
};

// Read-only view of the existing graph. The caller holds the index shared lock
// for the entire traversal, so per-node locks are unnecessary. Topology comes
// from Store; navigation distances read PQ codes only, never raw vectors.
template <class Store, class Quantizer, class Slots, bool Measure = false>
class PQGraphSearchStore {
public:
    using T = typename Store::T;
    using point_type = typename Store::point_type;
    static constexpr bool use_dist_cache = false;

    PQGraphSearchStore(Store &store, Quantizer &quantizer, const Slots &slots,
                       const uint8_t *codes, const float *table,
                       const typename ReverseGraph<T>::Data *reverse = nullptr,
                       duckdb::VexSearchProfileData *profile = nullptr)
        : store_(store), quantizer_(quantizer), slots_(slots), codes_(codes), table_(table), neighbors_(store, reverse),
          profile_(profile) {}

    template <bool base> size_t search_neighbor_count(T id) const { return neighbors_.template Count<base>(id); }

    template <bool base_layer, typename std::enable_if<base_layer, int>::type = 0>
    PQSearchVisited<T> make_search_visited(size_t reserve_hint) {
        return PQSearchVisited<T>(store_.elems.size(), reserve_hint);
    }

    template <bool base_layer>
    auto get_point_info(T id) {
        if constexpr (base_layer) {
            // The existing neighbor mirror is updated by neighbor writes and
            // restored by both reload paths. The caller's graph shared lock
            // excludes those writers and resizes for this whole search.
            // Reading it avoids FixedSizeAllocator::GetDeprecated's per-page
            // mutex and dirty marking on every read-only traversal step.
            if (id >= store_.base_points.size() ||
                store_.base_points[id].neighbors.size() < size_t(store_.m) * 2) {
                throw duckdb::InvalidInputException("pq_graph: incomplete neighbor mirror; rebuild the index");
            }
            return neighbors_.template Get<true>(id);
        } else {
            return store_.template get_point_info<base_layer>(id);
        }
    }

    template <bool base_layer, bool shared>
    void lock_point(T) { static_assert(shared, "PQ graph search is read-only"); }
    template <bool base_layer, bool shared>
    void unlock_point(T) { static_assert(shared, "PQ graph search is read-only"); }

    float get_distance(const PQGraphDistance &, const char *, T id) {
        duckdb::VexProfileTimer timer(Measure && profile_ ? &profile_->pq_distance_ns : nullptr);
        if constexpr (Measure) {
            if (profile_) ++profile_->pq_codes_scored;
        }
        ++codes_scored;
        return quantizer_.distance_to_code(Code(id), table_);
    }

    template <class Ids>
    void get_distance_batch(const PQGraphDistance &, const char *, const Ids &ids, float *out) {
        duckdb::VexProfileTimer timer(Measure && profile_ ? &profile_->pq_distance_ns : nullptr);
        if constexpr (Measure) {
            if (profile_) profile_->pq_codes_scored += ids.size();
        }
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
    ReverseNeighbors<Store> neighbors_;
    duckdb::VexSearchProfileData *profile_;
};

} // namespace vex_duck
