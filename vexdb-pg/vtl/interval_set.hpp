/**
 * Copyright ...
 * a mimic to boost::icl::interval_set
 */

#ifndef CONTAINER_INTERVAL_SET_H
#define CONTAINER_INTERVAL_SET_H

#include <vtl/vector>
#include <vtl/btree>

#include "postgres_ext.h"

namespace disk_container {
template <typename T>
struct DiscreteInterval {
    T start{};
    T end{};

    struct DiscreteIntervalPair {
        DiscreteInterval left_ival;
        DiscreteInterval right_ival;
    };

    struct Bound {
        T val;
        bool operator<(const Bound &other) const { return val < other.val; }
        bool operator==(const Bound &other) const { return val == other.val; }
        operator char() const { return 0; } /* for bptree debug compatibility */
    };
    Bound get_left_bound() const { return Bound{start}; }
    Bound get_right_bound() const { return Bound{end}; }

    DiscreteInterval() = default;
    DiscreteInterval(const T &val) : start(val), end(val) {}
    DiscreteInterval(const Bound &left_bound, const Bound &right_bound)
        : start(left_bound.val),
          end(right_bound.val) {}
    DiscreteInterval(const T &start, const T &end) : start(start), end(end) {}
    DiscreteInterval operator&(const DiscreteInterval &other) const
        { return DiscreteInterval(std::max(start, other.start), std::min(end, other.end)); }
    DiscreteInterval operator|(const DiscreteInterval &other) const
        { return DiscreteInterval(std::min(start, other.start), std::max(end, other.end)); }
    DiscreteIntervalPair operator^(const DiscreteInterval &other) const
    {
        DiscreteIntervalPair res;
        if (start < other.start) {
            res.left_ival.start = start;
            res.left_ival.end = other.start;
        } else {
            res.left_ival.start = other.start;
            res.left_ival.end = start;
        }

        if (end < other.end) {
            res.right_ival.start = end;
            res.right_ival.end = other.end;
        } else {
            res.right_ival.start = other.end;
            res.right_ival.end = end;
        }

        return res;
    }
    DiscreteInterval &operator&=(const DiscreteInterval &other)
    {
        /* hopefully compiler is smart enough to optimize it... */
        *this = *this & other;
        return *this;
    }
    DiscreteInterval &operator|=(const DiscreteInterval &other)
    {
        /* hopefully compiler is smart enough to optimize it... */
        *this = *this | other;
        return *this;
    }
    bool operator==(const DiscreteInterval &other) const { return start == other.start && end == other.end; }
    bool empty() const { return start > end; }
    bool contains(const DiscreteInterval &other) const { return start <= other.start && end >= other.end; }
    bool touch(const DiscreteInterval &other) const
    {
        if (std::is_integral<T>::value) {
            return start - 1 <= other.end && end + 1 >= other.start;
        }
        return start <= other.end && end >= other.start;
    }
};

template <typename T>
struct ContinuousInterval {
    bool start_open{false};
    bool end_open{false};
    T start{};
    T end{};

    struct ContinuousIntervalPair {
        ContinuousInterval left_ival;
        ContinuousInterval right_ival;
    };

    struct Bound {
        T val;
        bool open;

        bool operator<(const Bound &other) const
            { return val < other.val || (val == other.val && open && !other.open); }
        bool operator==(const Bound &other) const { return val == other.val && open == other.open; }
    };
    Bound get_left_bound() const { return Bound{start, start_open}; }
    Bound get_right_bound() const { return Bound{end, end_open}; }

    ContinuousInterval() {}
    /* intensionally implicit constructor */
    ContinuousInterval(const T &val) : start_open(false), end_open(false), start(val), end(val) {}
    ContinuousInterval(const Bound &left_bound, const Bound &right_bound)
        : start_open(left_bound.open), end_open(right_bound.open), start(left_bound.val), end(right_bound.val) {}
    ContinuousInterval(const T &start, const T &end, bool start_open = false, bool end_open = false)
        : start_open(start_open), end_open(end_open), start(start), end(end) {}

    static ContinuousInterval right_open(T start, T end) { return ContinuousInterval(start, end, false, true); }
    static ContinuousInterval open_it(T start, T end) { return ContinuousInterval(start, end, true, true); }
    static ContinuousInterval closed(T start, T end) { return ContinuousInterval(start, end, false, false); }
    ContinuousInterval operator&(const ContinuousInterval &other) const
    {
        ContinuousInterval res;
        if (start < other.start) {
            res.start = other.start;
            res.start_open = other.start_open;
        } else if (start == other.start) {
            res.start = start;
            res.start_open = start_open && other.start_open;
        } else {
            res.start = start;
            res.start_open = start_open;
        }

        if (end < other.end) {
            res.end = end;
            res.end_open = end_open;
        } else if (end == other.end) {
            res.end = end;
            res.end_open = end_open && other.end_open;
        } else {
            res.end = other.end;
            res.end_open = other.end_open;
        }

        return res;
    }
    ContinuousInterval operator|(const ContinuousInterval &other) const
    {
        ContinuousInterval res;
        if (start < other.start) {
            res.start = start;
            res.start_open = start_open;
        } else if (start == other.start) {
            res.start = start;
            res.start_open = start_open || other.start_open;
        } else {
            res.start = other.start;
            res.start_open = other.start_open;
        }

        if (end < other.end) {
            res.end = other.end;
            res.end_open = other.end_open;
        } else if (end == other.end) {
            res.end = end;
            res.end_open = end_open || other.end_open;
        } else {
            res.end = end;
            res.end_open = end_open;
        }

        return res;
    }
    ContinuousIntervalPair operator^(const ContinuousInterval &other) const
    {
        ContinuousIntervalPair res;
        if (start < other.start) {
            res.left_ival.start = start;
            res.left_ival.start_open = start_open;
            res.left_ival.end = other.start;
            res.left_ival.end_open = !other.start_open;
        } else if (start == other.start) {
            res.left_ival.start = start;
            res.left_ival.start_open = false;
            res.left_ival.end = start;
            res.left_ival.end_open = start_open ^ other.start_open;
        } else {
            res.left_ival.start = other.start;
            res.left_ival.start_open = !other.start_open;
            res.left_ival.end = start;
            res.left_ival.end_open = start_open;
        }

        if (end < other.end) {
            res.right_ival.start = end;
            res.right_ival.start_open = end_open;
            res.right_ival.end = other.end;
            res.right_ival.end_open = !other.end_open;
        } else if (end == other.end) {
            res.right_ival.start = end;
            res.right_ival.start_open = false;
            res.right_ival.end = end;
            res.right_ival.end_open = end_open ^ other.end_open;
        } else {
            res.right_ival.start = other.end;
            res.right_ival.start_open = !other.end_open;
            res.right_ival.end = end;
            res.right_ival.end_open = end_open;
        }
        return res;
    }
    ContinuousInterval &operator&=(const ContinuousInterval &other)
    {
        /* hopefully compiler is smart enough to optimize it... */
        *this = *this & other;
        return *this;
    }
    ContinuousInterval &operator|=(const ContinuousInterval &other)
    {
        /* hopefully compiler is smart enough to optimize it... */
        *this = *this | other;
        return *this;
    }
    /* we don't handle [5,5] == [5,6) for integral types, it's hard to guess the base unit (and use ClosedInterval for that case...) */
    bool operator==(const ContinuousInterval &other) const
    {
        return start == other.start && end == other.end && start_open == other.start_open && end_open == other.end_open;
    }
    bool empty() const { return start > end || (start == end && (start_open || end_open)); }
    bool contains(const ContinuousInterval &other) const
    {
        return start <= other.start && end >= other.end &&
            (start < other.start || (start == other.start && (!start_open || other.start_open))) &&
            (end > other.end || (end == other.end && (!end_open || other.end_open)));
    }
    bool touch(const ContinuousInterval &other) const
    {
        return (start < other.end && end > other.start) || (start == other.end && (!start_open || !end_open)) ||
               (end == other.start && (!end_open || !start_open));
    }
};

template <typename T, template<typename> class IntervalType = ContinuousInterval>
class IntervalSet {
public:
    using interval_type = IntervalType<T>;
    using Bound = typename interval_type::Bound;
    using bound_vector_type = Vector<Bound, DEFAULT_ALLOCATOR<Bound>, false>;

    explicit IntervalSet(MemoryContext ctx = CurrentMemoryContext) : _ivals(ctx)
        { static_assert(std::is_integral<T>::value, "sanity test"); }
    IntervalSet(const IntervalSet &other) : _ivals(other._ivals) {}
    IntervalSet &operator=(const IntervalSet &other)
    {
        _ivals = other._ivals;
        return *this;
    }
    IntervalSet(IntervalSet &&other) : _ivals(std::move(other._ivals)) {}
    IntervalSet &operator=(IntervalSet &&other)
    {
        _ivals = std::move(other._ivals);
        return *this;
    }

    void insert(const interval_type &ival)
    {
        auto it = _ivals.lower_bound(ival.get_left_bound());
        if (it == _ivals.cend()) {
            it = _ivals.cbegin();
        } else if (interval_type(it->first, it->second).contains(ival)) {
            return;
        }

        bound_vector_type to_delete;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
        Bound last_end;
        for (; it != _ivals.cend(); ++it) {
            if (ival.touch(interval_type(it->first, it->second))) {
                to_delete.push_back(it->first);
                last_end = it->second;
            }
            if (ival.get_right_bound() < it->first) {
                break;
            }
        }
        if (to_delete.empty()) {
            _ivals.emplace(ival.get_left_bound(), ival.get_right_bound());
            ann_helper::optional_destroy(to_delete);
            return;
        }

        interval_type new_ival = ival | interval_type(to_delete.front(), last_end);
#pragma GCC diagnostic pop /* -Wmaybe-uninitialized */
        for (const auto &bound : to_delete) {
#if VERIFY_DATA
            [[maybe_unused]] bool res = _ivals.erase(bound);
            Assert(res);
#else
            _ivals.erase(bound);
#endif /* VERIFY_DATA */
        }
        ann_helper::optional_destroy(to_delete);
        _ivals.emplace(new_ival.get_left_bound(), new_ival.get_right_bound());
    }

    bool insert_contain(const interval_type &ival)
    {
        auto it = _ivals.lower_bound(ival.get_left_bound());
        if (it == _ivals.cend()) {
            it = _ivals.cbegin();
        } else if (interval_type(it->first, it->second).contains(ival)) {
            return false;
        }

        bound_vector_type to_delete;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
        Bound last_end;
        for (; it != _ivals.cend(); ++it) {
            if (ival.touch(interval_type(it->first, it->second))) {
                to_delete.push_back(it->first);
                last_end = it->second;
            }
            if (ival.get_right_bound() < it->first) {
                break;
            }
        }
        if (to_delete.empty()) {
            _ivals.emplace(ival.get_left_bound(), ival.get_right_bound());
            ann_helper::optional_destroy(to_delete);
            return true;
        }

        interval_type new_ival = ival | interval_type(to_delete.front(), last_end);
#pragma GCC diagnostic pop /* -Wmaybe-uninitialized */
        for (const auto &bound : to_delete) {
#if VERIFY_DATA
            [[maybe_unused]] bool res = _ivals.remove(bound);
            Assert(res);
#else
            _ivals.erase(bound);
#endif /* VERIFY_DATA */
        }
        ann_helper::optional_destroy(to_delete);
        _ivals.emplace(new_ival.get_left_bound(), new_ival.get_right_bound());
        return true;
    }

    bool contains(const interval_type &ival) const
    {
        auto it = _ivals.lower_bound(ival.get_left_bound());
        if (it == _ivals.cend()) {
            return false;
        }
        interval_type cur_ival = interval_type(it->first, it->second);
        return cur_ival.contains(ival);
    }

    void destroy() { ann_helper::optional_destroy(_ivals); }
    size_t iterative_size() const { return _ivals.size(); }

private:
    using map_type =
        Map<Bound, Bound, std::less<Bound>, MapAllocator<CONTEXT_ALLOCATOR, Bound, Bound>>;
    map_type _ivals;
};
} /* namespace disk_container */

#endif /* CONTAINER_INTERVAL_SET_H */
