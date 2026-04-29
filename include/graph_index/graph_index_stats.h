/**
 * Copyright ...
 */

#ifndef GRAPH_INDEX_STATS_H
#define GRAPH_INDEX_STATS_H

#include "pg_compat.h"

#include <cmath>
#include <atomic>
#include "c.h"

// template <typename T>
// struct ConcurrentElement {
//     ConcurrentElement(T t) : _elem(t) {}
//     operator T() const { return _elem.load(std::memory_order_relaxed); }
//     ConcurrentElement &operator=(T t)
//     {
//         _elem.store(t, std::memory_order_relaxed);
//         return *this;
//     }
//     std::atomic<T> _elem;
// };
/* all currently supported platforms handle this sh*t already */
template <typename T>
struct alignas(T) ConcurrentElement {
    T value;
    operator const T&() const { return value; }
    operator T&() { return value; }
};

/**
 * concurrent only handles read/write sanity, it means nothing about thread-safety.
 */
template <bool concurrent>
struct GIwelford {
    template <typename T>
    using data_type = typename std::conditional<concurrent, ConcurrentElement<T>, T>::type;
    data_type<double> mean{0.0};
    data_type<double> m2{0.0};
    data_type<size_t> count{0};

    GIwelford() = default;
    GIwelford(double m, double m2, size_t c) : mean(m), m2(m2), count(c) {}

    void update(float v)
    {
        //
    }
    void reverse(float v)
    {
        //
    }

    float sigma() const { return sqrt(m2); }
};

struct GraphIndexStatsData {    
    static constexpr double beta = 2.236;
    static constexpr double delta = 0.6;
    static constexpr double kappa = 5.0;
    static constexpr double sigma_floor = 1e-6;
    static constexpr double k_0 = 1.0;

    ConcurrentElement<double> lambda{0.3};
    ConcurrentElement<double> lambda_d{0.5};
    ConcurrentElement<double> rho{1.2};
    ConcurrentElement<double> mu_g{1.0};
    ConcurrentElement<double> sigma_g{1.0};

    GIwelford<true> global_welford;
    GIwelford<true> local_welford;
    ConcurrentElement<size_t> N_total{0};
    ConcurrentElement<uint32> step{0};

    static uint8 maintenance_freq(uint32 k_buckets) { return Max(5, 50 / std::log(k_buckets)); }
};

using GraphIndexStats = GraphIndexStatsData *;

#endif /* GRAPH_INDEX_STATS_H */
