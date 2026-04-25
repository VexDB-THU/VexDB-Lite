/**
 * Copyright ...
 */

#ifndef DISTANCE_TEMPLATE2_H
#define DISTANCE_TEMPLATE2_H

#include <type_traits>
#include <vtl/array>
#include <vtl/expr_helper>

#include "distance/pq/pq_endecode.h"

/* Policy interface requirements for PQHelper operations:
 *
 * Required static constexpr members:
 *   - batch_size: uint32 - number of codes to process in batch (4, 8, 16, etc.)
 *   - nbits: uint32 - bits per quantization index (8, 16, etc.)
 *   - M: uint32 - number of subquantizers
 *   - M_aligned: bool - true if M % k == 0 (no remainder), false otherwise
 *   - k: uint32 - loop stride (subquantizers per iteration)
 *   - ksub: uint32 - 1 << nbits (e.g., 256 for nbits=8)
 *   - use_simd: bool - whether SIMD PQ operations are supported
 *   - pq_has_simd: bool - legacy, same as use_simd
 *   - pq_width: uint32 - SIMD width (same as k)
 *   - nearest_has_simd: bool - whether SIMD nearest neighbor is supported
 *   - nearest_width: uint32 - SIMD width for nearest neighbor
 *   - metric: Metric - distance metric type (L2, INNER_PRODUCT, etc.)
 *
 * Required types:
 *   - VecT: SIMD vector type for float operations (e.g., __m256, __m512)
 *   - IndexVecT: SIMD vector type for indices (e.g., __m256i, __m512i)
 *
 * PQ batch distance methods (used in distance_batch):
 *   - static INLINE_PROP VecT zero_float_vector() - return zero float vector
 *   - static INLINE_PROP IndexVecT zero_index_vector() - return zero index vector
 *   - static INLINE_PROP IndexVecT set1_index(uint32 v) - broadcast index to all lanes
 *
 *   - static INLINE_PROP void load_codes(const uint8* const* codes, uint32 offset,
 *                                     IndexVecT (&out)[batch_size]) - load code bytes and expand to indices
 *
 *   - static INLINE_PROP void expand_and_add_offset(const IndexVecT (&code_indices)[batch_size],
 *                                            uint32 m_base,
 *                                            IndexVecT (&out)[batch_size]) - expand uint8 to uint32,
 *                                               add sequential indices with stride, add base offset
 *
 *   - static INLINE_PROP void gather_distances(const float* sim_table,
 *                                          const IndexVecT (&indices)[batch_size],
 *                                          VecT (&out)[batch_size]) - gather float values from sim_table
 *
 *   - static INLINE_PROP VecT accumulate_one(VecT acc, VecT val) - acc += val
 *
 *   - static INLINE_PROP void accumulate(VecT (&accumulators)[batch_size],
 *                                   const VecT (&values)[batch_size]) - batch accumulation
 *
 *   - static INLINE_PROP void horizontal_sum_batch(const VecT (&vectors)[batch_size],
 *                                             float* results) - reduce to scalars
 *
 *   - static INLINE_PROP void scalar_remainder(const uint8* const* codes, uint32 start_m,
 *                                           const float* sim_table,
 *                                           float* results) - handle M % k != 0 remainder
 *
 * PQ batch distance methods (used in fvec_op_ny_D):
 *   - static INLINE_PROP VecT pq_set1(float v) - broadcast scalar to all float lanes
 *   - static INLINE_PROP VecT pq_loadu(const float* v) - unaligned load of float vector
 *   - static INLINE_PROP void pq_storeu(float* dst, VecT v) - unaligned store of float vector
 *
 * Nearest neighbor methods (used in fvec_op_ny_nearest_D):
 *   - static INLINE_PROP VecT nearest_set1(float v) - broadcast scalar to all float lanes
 *   - static INLINE_PROP IndexVecT nearest_sequential_indices() - create sequential index vector 0,1,2,...
 *   - static INLINE_PROP IndexVecT nearest_set1_index(uint32 v) - broadcast index to all index lanes
 *   - static INLINE_PROP IndexVecT nearest_add_index(IndexVecT idx, uint32 delta) - add constant to all index lanes
 *   - static INLINE_PROP auto nearest_cmp_lt(VecT a, VecT b) - compare (a < b), return mask type
 *   - static INLINE_PROP VecT nearest_min(VecT a, VecT b) - element-wise minimum of float vectors
 *   - static INLINE_PROP IndexVecT nearest_blend_index(IndexVecT a, IndexVecT b, auto mask) - conditional blend of index vectors
 *   - static INLINE_PROP void nearest_store_dist(float* dst, VecT v) - store distance vector to memory
 *   - static INLINE_PROP void nearest_store_index(uint32* dst, IndexVecT v) - store index vector to memory
 *   - template<uint16 D> static INLINE_PROP uint32 nearest_transpose(const float* src, Array<VecT, D>& out) - load D-dimensional vectors and transpose them
 *
 * General SIMD vector operations (reused from existing Policy interface):
 *   - static INLINE_PROP VecT sub_vectors(VecT a, VecT b) - subtract vector b from a
 *   - static INLINE_PROP VecT madd_square(VecT a, VecT s) - multiply a*a and add to s
 *   - static INLINE_PROP VecT get_zero_vectors() - return zero vector
 *   - static INLINE_PROP VecT add_vectors(VecT a, VecT b) - add two float vectors element-wise
 *
 * Note: Op::vector() and Op::madd_vector() are required for PQ distance computations:
 *   - static INLINE_PROP VecT Op::vector(VecT a, VecT b) - single element operation
 *   - static INLINE_PROP VecT Op::madd_vector(VecT a, VecT b, VecT s) - multiply-add operation
 *   - static INLINE_PROP float Op::scalar(float a, float b) - scalar fallback operation
 */
template <typename Policy>
struct PQHelper {
    using VecT = typename Policy::VecT;

    template <typename Op, uint16 D>
    static INLINE_PROP void fvec_op_ny_D(
        float *dis, const float *x, const float *y, uint32 ny)
    {
        uint32 i = 0;
        if constexpr (Policy::pq_has_simd) {
            constexpr uint32 width = Policy::pq_width;
            Array<VecT, D> xb;
            ann_helper::unroll<D>([&](auto idx) -> void {
                xb[idx] = Policy::pq_set1(x[idx]);
            });
            for (; i + width <= ny; i += width) {
                Array<VecT, D> yv = pq_load_transposed<D>(y, yv);
                y += width * D;
                VecT acc = Op::vector(xb[0], yv[0]);
                ann_helper::unroll<D - 1>([&](auto idx) -> void {
                    constexpr uint16 lane = static_cast<uint16>(idx) + 1u;
                    acc = Op::madd_vector(xb[lane], yv[lane], acc);
                });
                Policy::pq_storeu(dis + i, acc);
            }
        }
        for (; i < ny; ++i) {
            float res = 0.0f;
            ann_helper::unroll<D>([&](auto idx) -> void {
                res += Op::scalar(x[idx], y[idx]);
            });
            dis[i] = res;
            y += D;
        }
    }

    template <uint16 D>
    static INLINE_PROP Array<VecT, D> pq_load_transposed(const float *y)
    {
        Array<VecT, D> out;
        if constexpr (D == 1) {
            out[0] = Policy::pq_loadu(y);
            return out;
        }
        constexpr uint32 width = Policy::pq_width;
        Array<Array<float, width>, D> tmp{};
        for (uint32 lane = 0; lane < width; ++lane) {
            const float *row = y + lane * D;
            ann_helper::unroll<D>([&](auto idx) -> void {
                tmp[idx][lane] = row[idx];
            });
        }
        ann_helper::unroll<D>([&](auto idx) -> void {
            out[idx] = Policy::pq_loadu(tmp[idx].data());
        });
        return out;
    }

    static INLINE_PROP void fvec_ny_distance(float *dis, const float *x, const float *y,
        uint32 d, uint32 ny)
    {
        constexpr Metric metric = Policy::metric == Metric::L2
            ? Metric::L2
            : Metric::INNER_PRODUCT;
        using Op = typename Policy::PqOp;
        bool handled = false;
        static constexpr Array<uint16, 5> kDims = {1, 2, 3, 4, 5};
        ann_helper::unroll<kDims.size()>([&](auto idx) -> bool {
            constexpr uint16 dim = kDims[idx];
            if (d != dim) {
                return true;
            }
            fvec_op_ny_D<Op, dim>(dis, x, y, ny);
            handled = true;
            return false;
        });
        if (handled) {
            return;
        }
        constexpr auto func = DistanceHelper<Policy>::template get_function<metric>();
        for (uint32 i = 0; i < ny; ++i) {
            dis[i] = func(x, y, static_cast<uint16>(d));
            y += d;
        }
    }

    template <uint16 D>
    static INLINE_PROP uint32 fvec_op_ny_nearest_D(const float *x,
        const float *y, uint32 ny)
    {
        uint32 i = 0;
        float current_min_distance = HUGE_VALF;
        uint32 current_min_index = 0;
        
        if constexpr (Policy::nearest_has_simd) {
            constexpr uint32 width = Policy::nearest_width;
            const uint32 ny_width = ny / width;
            
            if (ny_width > 0) {
                VecT min_distances = Policy::nearest_set1(HUGE_VALF);
                typename Policy::IndexVecT min_indices = Policy::nearest_sequential_indices();
                typename Policy::IndexVecT current_indices = Policy::nearest_sequential_indices();
                const typename Policy::IndexVecT indices_increment = Policy::nearest_set1_index(width);
                
                Array<VecT, D> m;
                ann_helper::unroll<D>([&](auto idx) {
                    m[idx] = Policy::nearest_set1(x[idx]);
                });
                

                for (; i < ny_width * width; i += width) {
                    Array<VecT, D> v;
                    y += Policy::template nearest_transpose<D>(y, v);
                    
                    VecT distances = Policy::sub_vectors(m[0], v[0]);
                    distances = Policy::madd_square(distances, Policy::get_zero_vectors());
                    
                    ann_helper::unroll<D - 1>([&](auto idx) {
                        constexpr uint16 lane = static_cast<uint16>(idx) + 1;
                        VecT diff = Policy::sub_vectors(m[lane], v[lane]);
                        distances = Policy::madd_square(diff, distances);
                    });
                    
                    auto comparison = Policy::nearest_cmp_lt(distances, min_distances);
                    min_distances = Policy::nearest_min(distances, min_distances);
                    min_indices = Policy::nearest_blend_index(min_indices, current_indices, comparison);
                    current_indices = Policy::nearest_add_index(current_indices, width);
                }
                
                Array<float, 16> min_distances_scalar{};
                Array<uint32, 16> min_indices_scalar{};
                Policy::nearest_store_dist(min_distances_scalar.data(), min_distances);
                Policy::nearest_store_index(min_indices_scalar.data(), min_indices);
                
                ann_helper::unroll<16>([&](auto j) {
                    if (current_min_distance > min_distances_scalar[j]) {
                        current_min_distance = min_distances_scalar[j];
                        current_min_index = min_indices_scalar[j];
                    }
                });
            }
        }

        for (; i < ny; i++) {
            float res = 0.0f;
            ann_helper::unroll<D>([&](auto idx) {
                float diff = x[idx] - y[idx];
                res += diff * diff;
            });
            if (current_min_distance > res) {
                current_min_distance = res;
                current_min_index = i;
            }
            y += D;
        }

        return current_min_index;
    }

    static INLINE_PROP uint32 fvec_L2sqr_ny_nearest(float *tmp, const float *x,
        const float *y, uint32 d, uint32 ny)
    {
        uint32 result = 0;
        static constexpr Array<uint16, 4> kDims = {2, 3, 4, 5};
        if (!ann_helper::unroll<kDims.size()>([&](auto idx) -> bool {
                constexpr uint16 dim = kDims[idx];
                if (d != dim) {
                    return true;
                }
                result = fvec_op_ny_nearest_D<dim>(x, y, ny);
                handled = true;
                return false;
            })) {
            return result;
        }
        
        fvec_ny_distance(tmp, x, y, d, ny);
        
        uint32 nearest_idx = 0;
        float min_dis = HUGE_VALF;
        
        for (uint32 i = 0; i < ny; i++) {
            if (tmp[i] < min_dis) {
                min_dis = tmp[i];
                nearest_idx = i;
            }
        }
        
        return nearest_idx;
    }

    static INLINE_PROP float distance_single_code(
        const float *sim_table,
        const uint8 *code)
    {
        float result;
        const uint8 *code_ptr = code;
        distance_batch<Policy>(sim_table, &code_ptr, &result);
        return result;
    }

    static INLINE_PROP void distance_batch(
        const float *sim_table,
        const uint8 * const* codes,
        float *results)
    {
        CONSTEXPR_IF (Policy::use_simd) {
            distance_batch_simd<Policy>(sim_table, codes, results);
        } else {
            distance_batch_scalar<Policy>(sim_table, codes, results);
        }
    }

private:
    template <typename P>
    static INLINE_PROP void distance_batch_simd(
        const float *sim_table,
        const uint8 * const* codes,
        float *results)
    {
        using VecT = typename P::VecT;
        using IndexVecT = typename P::IndexVecT;
        constexpr uint32 N = P::batch_size;
        constexpr uint32 k = P::k;
        
        VecT accumulators[N];
        ann_helper::unroll<N>([&](auto i) {
            accumulators[i] = P::zero_float_vector();
        });
        
        const float *tab = sim_table;
        uint32 m_full = (P::M / k) * k;
        
        for (uint32 m = 0; m < m_full; m += k) {
            IndexVecT code_indices[N];
            P::load_codes(codes, m, code_indices);
            
            IndexVecT table_indices[N];
            P::expand_and_add_offset(code_indices, m, table_indices);
            
            VecT distances[N];
            P::gather_distances(tab, table_indices, distances);
            
            P::accumulate(accumulators, distances);
            tab += k * P::ksub;
        }
        
        P::horizontal_sum_batch(accumulators, results);
        
        if constexpr (!P::M_aligned) {
            P::scalar_remainder(codes, m_full, tab, results);
        }
    }
    
    template <typename P>
    static INLINE_PROP void distance_batch_scalar(
        const float *sim_table,
        const uint8 * const* codes,
        float *results)
    {
        constexpr uint32 N = P::batch_size;
        constexpr uint32 nbits = P::nbits;
        
        using Decoder = std::conditional_t<nbits == 8, PQDecoder8,
                         std::conditional_t<nbits == 16, PQDecoder16, 
                         PQDecoderGeneric>>;
        
        ann_helper::unroll<N>([&](auto i) {
            Decoder decoder(codes[i], nbits);
            const float *tab = sim_table;
            results[i] = 0.0f;
            for (uint32 m = 0; m < P::M; m++) {
                results[i] += tab[decoder.decode()];
                tab += P::ksub;
            }
        });
    }
};

template <typename Policy>
struct RabitqHelper {
    using VecT = typename Policy::VecT;

    template <uint32 LogDim>
    static INLINE_PROP void fht(float *buf)
    {
        // TD
    }

    static INLINE_PROP void flip_sign(const uint8 *flip, float *data, size_t dim)
    {
        // TD
    }

    static INLINE_PROP void kacs_walk(float *data, size_t len)
    {
        // TD
    }

    static INLINE_PROP float warmup_ip_x0_q(uint64 *data, const uint64 *query,
        float delta, float vl, size_t dim)
    {
        // TD
    }

    static INLINE_PROP float ip_fxi(float *query, uint8 *data, size_t dim)
    {
        // TD
    }

    static INLINE_PROP float mask_ip_x0_q(float *query, uint64 *data, size_t dim)
    {
        // TD
    }
};

#endif /* DISTANCE_TEMPLATE2_H */
