/**
 * Copyright ...
 */

#ifndef GRAPH_INDEX_STORAGE_INTERFACE_H
#define GRAPH_INDEX_STORAGE_INTERFACE_H

#include <cfloat>
#include <vtl/vector>
#include <vtl/holder>
#include <vtl/disk_container/diskvector.hpp>
#include <vtl/disk_container/freespace.hpp>
#include <vtl/pair>
#include <vtl/tuple>
#include <vtl/bitlock.hpp>
#include <vtl/bitvector>
#include <vtl/variant>

#include "commands/vacuum.h"
#include "utils/palloc.h"
#include "utils/lsyscache.h"
#include "knl/knl_variable.h"
/* AIO deferred - #include "storage/aio_worker.h" */
#include "graph_index/graph_index_cluster.h"
#include "graph_index/graph_index.h"
#include "graph_index/graph_index_xlog.h"
#include "module/perf_usage.h"
#include "bulkbuf_smgr.h"
#include "vector_smgr.h"
#include "ann_utils.h"
#include "halfvec.h"
#include "floatvector.h"
#include "index_inspect.h"
#include "pq.h"
#include "rabitq/rabitq_distancer.h"

PERF_DECLARE_CATS(MemPerfCats, true, read, write, calc, lock);
PERF_DECLARE_CATS(DiskPerfCats, false, read_node, read_neighbor, read_vec, write_node, write_neighbor, write_vec, calc, lock, fetch);

template <typename IdType = uint32, typename elem_type = GraphIndexPoint>
class MemStore : public PERFER(MemPerfCats) {
    using PerfCats = MemPerfCats;
    using PlainStore = disk_container::PlainStore;

    static uint32 get_base_point_size(uint_fast16_t m)
    {
        uint32 base_size = (uint32)(sizeof(T) + sizeof(float)) * m * 2;
        uint32 expand_size = TYPEALIGN(sizeof(uint) * __CHAR_BIT__, m * 2 + 1);
        return base_size + expand_size;
    }
    static uint32 get_upper_point_size(uint_fast16_t m)
    {
        uint32 base_size = (uint32)sizeof(T) * (m + 1) * 2 + (uint32)sizeof(float) * m;
        uint32 expand_size = TYPEALIGN(sizeof(uint) * __CHAR_BIT__, m + 1);
        return base_size + expand_size;
    }
    static uint32 base_target_size_mb(uint32 total, uint_fast16_t m)
    {
        uint32 base_size = get_base_point_size(m);
        uint32 upper_size = get_upper_point_size(m);
        double nlayer = 1.0 / (m - 1);
        double rate = base_size / (base_size + upper_size * nlayer);
        return total * rate;
    }
    static uint32 upper_target_size_mb(uint32 total, uint_fast16_t m)
        { return total - base_target_size_mb(total, m); }
public:
    using T = IdType;
    using point_type = elem_type;

    class MemPool {
        /* to make mempool lockfree, we need to ensure that `vec` and `lock` will not realloc,
         * if memoryused <= `chunk_size` * `pre_alloc_vec_size`, will not realloc. 
         * 20000 chunks means that we can contain 10TB vectors, which is impossible to realloc
         * during memory build... at least for now.
         */
        static constexpr size_t pre_alloc_vec_size = 20000ul;
        struct Chunk {
            char *buf;
            Chunk(char *b) : buf(b) {}
        };
        uint32 elem_size; /* Bytes per elem */
        uint32 pow_elem_nums_per_chunk; /* 2^x elems in a chunk */
        size_t chunk_size; /* Bytes per chunk, equal to `elem_size` * `one_chunk_elem_nums` */
        MemoryContext ctx;
        Vector<RWBitLock> locks; /* every chunk has one RWBitLock with size `one_chunk_elem_nums` */
        slock_t mutex;

        uint32 get_chunk_no(T idx) { return idx >> pow_elem_nums_per_chunk; }
        uint32 get_chunk_offset(T idx) { return (idx & (one_chunk_elem_nums - 1)) * elem_size; }
        uint32 get_lock_idx(T idx) { return idx & (one_chunk_elem_nums - 1); }
    public:
        uint32 one_chunk_elem_nums; /* equal to 1 << `pow_elem_nums_per_chunk` */
        Vector<Chunk> vec;

        MemPool(uint32 store_esize, uint32 target_size_mb, MemoryContext ctx)
            : ctx(ctx),
              locks(pre_alloc_vec_size),
              vec(pre_alloc_vec_size)
        {
            Assert(CurrentMemoryContext == ctx);
            elem_size = ((store_esize + ann_helper::vector_aligned_size - 1) /
                         ann_helper::vector_aligned_size) * ann_helper::vector_aligned_size;
            pow_elem_nums_per_chunk = calculate_pow(elem_size, target_size_mb);
            one_chunk_elem_nums = 1 << pow_elem_nums_per_chunk;
            chunk_size = elem_size * one_chunk_elem_nums;
            char *aligned_ptr = allocate_aligned_memory(chunk_size);
            vec.push_back(aligned_ptr);
            locks.emplace_back(one_chunk_elem_nums);
            SpinLockInit(&mutex);
        }

        void *get(size_t idx)
        {
            uint32 chunk_no = get_chunk_no(idx);
            uint32 chunk_offset = get_chunk_offset(idx);
            return &vec[chunk_no].buf[chunk_offset];
        }

        void *extend(size_t idx)
        {
            uint32 chunk_no = get_chunk_no(idx);
            uint32 chunk_offset = get_chunk_offset(idx);
            if (chunk_no >= vec.size()) { /* need append */
                Assert(chunk_no < pre_alloc_vec_size);
                SpinLockAcquire(&mutex);
                for (uint32 i = vec.size(); i < chunk_no + 1; ++i) {
                    auto old_ctx = MemoryContextSwitchTo(ctx);
                    locks.emplace_back(one_chunk_elem_nums);
                    pg_memory_barrier();
                    char *aligned_ptr = allocate_aligned_memory(chunk_size);
                    vec.push_back(aligned_ptr);
                    MemoryContextSwitchTo(old_ctx);
                }
                SpinLockRelease(&mutex);
            }
            return &vec[chunk_no].buf[chunk_offset];
        }
        void set(size_t idx, void *value)
        {
            void *dest = extend(idx);
            memcpy(dest, value, elem_size);
        }

        template <bool shared_lock>
        void lock_elem(size_t idx)
        {
            uint32 chunk_no = get_chunk_no(idx);
            uint32 lock_idx = get_lock_idx(idx);
            CONSTEXPR_IF (shared_lock) {
                locks[chunk_no].rlock(lock_idx);
            } else {
                locks[chunk_no].wlock(lock_idx);
            } 
        }

        template <bool shared_lock>
        void unlock_elem(size_t idx)
        {
            uint32 chunk_no = get_chunk_no(idx);
            uint32 lock_idx = get_lock_idx(idx);
            CONSTEXPR_IF (shared_lock) {
                locks[chunk_no].runlock(lock_idx);
            } else {
                locks[chunk_no].wunlock(lock_idx);
            } 
        }
    };

    /* `vector_pool`: since id is unique and vec_size is determined,
     *  we can directly get the vector of a point by calculate the offset from `vector_pool`.
     *
     * `basepoint_pool`: id is unique and neighbors size is determined, same as `vector_pool`
     * 
     * `upperpoint_pool`: can not sure the num of upperpoint, have to get a upperpoint neighbor
     *  by `pool_idx`, which is assigned when insert
     */
    MemPool vector_pool;
    MemPool basepoint_pool;
    MemPool upperpoint_pool;

    GraphIndexEntryInfo entry_info;
    static constexpr size_t bitlock_size = 1'000'000'000ul; /* need 125MB, enough for memory build now... */
    static constexpr bool has_occlusion_cache = true;
    static constexpr bool clustered = std::is_same<point_type, GraphIndexCluster>::value;

    MemStore(uint_fast16_t dim, uint_fast16_t m, uint_fast32_t vec_size, size_t vectorpool_initsize,
             size_t pointpool_initsize, MemoryContext ctx)
        : vector_pool(vec_size, vectorpool_initsize, ctx),
          basepoint_pool(get_base_point_size(m), base_target_size_mb(pointpool_initsize, m), ctx),
          upperpoint_pool(get_upper_point_size(m), upper_target_size_mb(pointpool_initsize, m), ctx),
          dim(dim),
          m(m),
          vec_size(vec_size),
          num_vectors(0),
          num_uppers(0),
          elems_lock(bitlock_size)
    {
        entry_info.set(INVALID_VECTOR_ID, INVALID_VECTOR_ID, -1);
        LWLockInitialize(&elems_veclock, LWTRANCHE_EXTEND);
        LWLockInitialize(&entry_lock, LWTRANCHE_EXTEND);
        LWLockInitialize(&entry_waitlock, LWTRANCHE_EXTEND);
    }

    void destroy()
    {
        REPORT_PERF(NOTICE);
        PERF_DESTROY();
    }
    template <typename Distancer>
    void get_distance_batch(const Distancer &d, const char *query, const Vector<T> &ids, float *dists)
    {
        const uint_fast16_t num = ids.size();
        DO_PERF_COUNT(read, num);
        void *vals[num];
        void **val_cur = vals;
        for (T id : ids) {
            *val_cur = vector_pool.get(id);
            ++val_cur;
        }
        STOP_PERF(read);
        DO_PERF_COUNT(calc, num);
        d.get_distance_batch2(query, vals, dim, num, dists);
        STOP_PERF(calc);
        // for (T id : ids) {
        //     DO_PERF(read);
        //     const char *val = (const char *)vector_pool.get(id);
        //     STOP_PERF(read);
        //     DO_PERF(calc);
        //     *dists = d.get_distance_single((void *)query, (void *)val, dim);
        //     STOP_PERF(calc);
        //     ++dists;
        // }
    }

    template <typename Distancer>
    float get_distance(const Distancer &d, const char *query, T id)
    {
        DO_PERF(read);
        const char *val = (const char *)vector_pool.get(id);
        STOP_PERF(read);
        DO_PERF(calc);
        float res = d.get_distance_single((void *)query, (void *)val, dim);
        STOP_PERF(calc);
        return res;
    }

    template <typename Distancer>
    float get_distance(const Distancer &d, T query_id, T val_id)
    {
        DO_PERF(read);
        const char *query = vector_pool.get(query_id);
        const char *val = vector_pool.get(val_id);
        STOP_PERF(read);
        DO_PERF(calc);
        float res = d.get_distance_single((void *)query, (void *)val, dim);
        STOP_PERF(calc);
        return res;
    }

    template <typename Distancer>
    float get_distance(const Distancer &d, const char *query, const char *val)
    {
        DO_PERF(calc);
        float res = d.get_distance_single((void *)query, (void *)val, dim);
        STOP_PERF(calc);
        return res;
    }

    /* the usage is not locked */
    template <bool is_base_layer>
    Pair<float *, BitSpan<uint>> get_neighbor_stats(T id)
    {
        DO_PERF(read);
        float *dist;
        uint *res;
        size_t len;
        CONSTEXPR_IF (is_base_layer) {
            T *neighbors_info = (T *)basepoint_pool.get(id);
            dist = (float *)(neighbors_info + m * 2);
            res = (uint *)(dist + m * 2);
            len = m * 2 + 1;
        } else {
            T *neighbors_info = (T *)upperpoint_pool.get(id);
            dist = (float *)(neighbors_info + upperpoint_size(m));
            res = (uint *)(dist + m);
            len = m + 1;
        }
        STOP_PERF(read);
        return Pair<float *, BitSpan<uint>>(dist, res, len, 0);
    }

    static bool has_stat(BitSpan<uint> stat) { return stat[stat.size() - 1]; }
    static void set_stat(BitSpan<uint> stat) { stat.set(stat.size() - 1); }

    T get_vector_num() const { return num_vectors; }
    T get_upper_num() const { return num_uppers; }
    uint_fast16_t get_m() const { return m; }
    uint_fast16_t get_dim() const { return dim; }
    uint_fast32_t get_vecsize() const { return vec_size; }
    uint_fast32_t get_elemsize() const { return vec_size; }
    
    template <bool is_base_layer>
    T assign_vector_id()
    {
        CONSTEXPR_IF (is_base_layer) {
            return num_vectors.fetch_add(1);
        } else {
            return num_uppers.fetch_add(1);
        }
    }

    void add_upperpoint(T cur_layer_idx, T lower_layer_idx, T id, T *neighbors_info)
    {
        DO_PERF(write);
        T *dest = (T *)upperpoint_pool.extend(cur_layer_idx);
        memcpy(dest, neighbors_info, sizeof(T) * m * 2);
        dest += m * 2;
        *dest = lower_layer_idx;
        *(dest + 1) = id;
        float *temp = (float *)(dest + 2);
        BitSpan<uint> stat((uint *)(temp + m), m + 1);
        stat.reset(m);
        STOP_PERF(write);
    }

    void add_basepoint(T id, T *neighbors_id)
    {
        DO_PERF(write);
        T *dest = (T *)basepoint_pool.extend(id);
        memcpy(dest, neighbors_id, sizeof(T) * m * 2);
        float *temp = (float *)(dest + m * 2);
        BitSpan<uint> stat((uint *)(temp + m * 2), m * 2 + 1);
        stat.reset(m * 2);
        STOP_PERF(write);
    }

    void add_vector(T id, const char *query)
    {
        DO_PERF(write);
        vector_pool.set(id, (void *)query);
        STOP_PERF(write);
    }

    template <typename Distancer>
    void add_vector(T id, const char *query, Distancer &distancer)
    {
        add_vector(id, query);
    }

    template <bool is_base_layer>
    void get_neighbors(Vector<GraphIndexCandidate<T>> &neighbors, const GraphIndexCandidate<T> &point)
    {
        DO_PERF(read);
        CONSTEXPR_IF (is_base_layer) {
            T *neighbors_id = (T *)basepoint_pool.get(point.id);
            float *dist = (float *)(neighbors_id + m * 2);
            for (uint_fast16_t i = 0; i < m * 2; ++i) {
                T id = neighbors_id[i];
                if (!is_valid(id)) {
                    break;
                }
                neighbors.emplace_back(id, id, dist[i]);
            }
        } else {
            T *neighbors_info = (T *)upperpoint_pool.get(point.cur_layer_idx);
            T *neighbors_id = neighbors_info;
            T *neighbors_cur_layer_idx = neighbors_info + m;
            float *dist = (float *)(neighbors_info + upperpoint_size(m));
            for (uint_fast16_t i = 0; i < m; ++i) {
                T id = neighbors_id[i];
                if (!is_valid(id)) {
                    break;
                }
                T cur_layer_idx = neighbors_cur_layer_idx[i];
                neighbors.emplace_back(id, cur_layer_idx, dist[i]);
            }
        }
        STOP_PERF(read);
    }

    template <bool is_base_layer>
    tuple<T *, T, T> get_point_info(T cur_layer_idx)
    {
        DO_PERF(read);
        CONSTEXPR_IF (is_base_layer) {
            T *neighbors_id = (T *)basepoint_pool.get(cur_layer_idx);
            STOP_PERF(read);
            return {neighbors_id, INVALID_VECTOR_ID, INVALID_VECTOR_ID};
        }
        T *neighbors_info = (T *)upperpoint_pool.get(cur_layer_idx);
        STOP_PERF(read);
        return {neighbors_info, neighbors_info[m * 2], INVALID_VECTOR_ID};
    }

    template <bool is_base_layer>
    void set_neighbor(T cur_layer_idx, uint16 update_nbr_idx, T newpoint_id, T newpoint_cur_layer_idx)
    {
        DO_PERF(write);
        CONSTEXPR_IF (is_base_layer) {
            T *neighbors_id = (T *)basepoint_pool.get(cur_layer_idx);
            neighbors_id[update_nbr_idx] = newpoint_id;
        } else {
            T *neighbors_info = (T *)upperpoint_pool.get(cur_layer_idx);
            T *neighbors_id = neighbors_info;
            T *neighbors_cur_layer_idx = neighbors_info + m;
            neighbors_id[update_nbr_idx] = newpoint_id;
            neighbors_cur_layer_idx[update_nbr_idx] = newpoint_cur_layer_idx;
        }
        STOP_PERF(write);
    }

    template <bool with_lock, bool force_share_flag = false>
    Pair<GraphIndexEntryInfo, bool> get_entry(int_fast8_t insert_level = 0)
    {
        DO_PERF(lock);
        bool shared = true;

        LWLockAcquire(&entry_waitlock, LW_EXCLUSIVE);
        LWLockRelease(&entry_waitlock);
        /* Get entry point */
        LWLockAcquire(&entry_lock, LW_SHARED);
        GraphIndexEntryInfo entry = entry_info;
        if ((!force_share_flag && unlikely(insert_level > entry.level)) ||
            unlikely(entry.level < 0)) {
            LWLockRelease(&entry_lock);

            LWLockAcquire(&entry_waitlock, LW_EXCLUSIVE);
            LWLockAcquire(&entry_lock, LW_EXCLUSIVE);
            LWLockRelease(&entry_waitlock);
            /* we don't re-examine whether level has been changed, even if so,
             * we want to ensure first several points to connect sequentially */

            /* Get latest entry point after lock is acquired */
            shared = false;
            entry = entry_info;
        }
        STOP_PERF(lock);
        return {entry, shared};
    }

    void release_entry_lock(bool shared) { LWLockRelease(&entry_lock); }

    void set_entrypoint(size_t id, size_t cur_layer_idx, int_fast8_t level)
    {
        DO_PERF(write);
        entry_info.set(id, cur_layer_idx, level);
        STOP_PERF(write);
    }

    void add_elem(PointExtensionContext &ctx, T id, const ItemPointerData &tid)
    {
        DO_PERF(write);
        DO_PERF(lock);
        LWLockAcquire(&elems_veclock, LW_EXCLUSIVE);
        STOP_PERF(lock);
        elems.expand_size(id + 1);
        new (elems.at(id)) point_type(ctx, tid);
        LWLockRelease(&elems_veclock);
        STOP_PERF(write);
    }

    template <typename F>
    auto apply_elem(T id, F &&f)
    {
        DO_PERF(write);
        DO_PERF(lock);
        LWLockAcquire(&elems_veclock, LW_SHARED);
        elems_lock.lock(id);
        STOP_PERF(lock);
        auto res = f(elems[id]);
        elems_lock.unlock(id);
        LWLockRelease(&elems_veclock);
        STOP_PERF(write);
        return res;
    }

    template <bool is_base_layer, bool shared_lock>
    void lock_point(T cur_layer_idx)
    {
        DO_PERF(lock);
        CONSTEXPR_IF (is_base_layer) {
            basepoint_pool.template lock_elem<shared_lock>(cur_layer_idx);
        } else {
            upperpoint_pool.template lock_elem<shared_lock>(cur_layer_idx);
        }
        STOP_PERF(lock);
    }

    template <bool is_base_layer, bool shared_lock>
    void unlock_point(T cur_layer_idx)
    {
        CONSTEXPR_IF (is_base_layer) {
            basepoint_pool.template unlock_elem<shared_lock>(cur_layer_idx);
        } else {
            upperpoint_pool.template unlock_elem<shared_lock>(cur_layer_idx);
        }
    }

    char *get_data(T id) { return (char *)vector_pool.get(id); }

    void flush_points(Relation index, BlockNumber meta_blkno)
    {
        disk_container::DiskVector<point_type> dv{index, meta_blkno, false};
        dv.push_back_n(elems.data(), elems.size());
        dv.destroy();
    }

    static constexpr void reset_neighbors_val_pool() {}
    template <typename Distancer>
    static float get_distance_precise(const Distancer &, const char *, const char *)
        { __builtin_unreachable(); }
    template <typename Distancer>
    static float get_distance_est(const Distancer &, const char *, T) { __builtin_unreachable(); }
    static char *fetch_vec_from_heap(T) { __builtin_unreachable(); }
private:
    uint_fast16_t dim;
    uint_fast16_t m;
    uint_fast32_t vec_size;
    std::atomic<T> num_vectors;
    std::atomic<T> num_uppers;
    LWLock entry_lock;
    LWLock entry_waitlock;
    Vector<point_type, HUGE_ALLOCATOR<point_type>> elems;
    LWLock elems_veclock; /* used for `elems` realloc */
    BitLock elems_lock; /* every elem has one lock, used when find duplicate point */
    bool is_valid(T id) { return likely(id != (T)INVALID_VECTOR_ID); }
    static constexpr size_t upperpoint_size(uint_fast16_t m) { return (m + 1) * 2; }
};

template <typename IdType, bool WithBulkbuf, typename elem_type = GraphIndexPoint>
class DiskStore : public PERFER(DiskPerfCats) {
    using PerfCats = DiskPerfCats;
    using PlainStore = disk_container::PlainStore;
    using AccessorLockType = disk_container::AccessorLockType;
    static constexpr BlockNumber metablkno = GRAPH_INDEX_METAPAGE_BLKNO;
public:
    using T = IdType;
    using point_type = elem_type;
    static constexpr bool has_occlusion_cache = false;
    static constexpr bool clustered = std::is_same<point_type, GraphIndexCluster>::value;

    DiskStore(Relation index, Relation heap, Buffer metabuf, BulkBuffer *bulkbuf, bool need_wal)
        : index(index),
          heap(heap),
          metap(GRAPH_INDEX_PAGE_GET_META(BufferGetPage(metabuf))),
          metabuf(metabuf),
          need_wal(need_wal),
          m(metap->m),
          dim(metap->dimension),
          qt_type(metap->quantizer_metainfo.get_type()),
          st(qt_type == QuantizerType::NONE ? VecStorageType::PureVec : VecStorageType::PureCode),
          vec_size(dim * VEC_ELEM_SIZE(metap->precision_type)),
          bulkbuf(bulkbuf),
          elems(index, metap->elems_block, need_wal),
          base_layer(index, metap->base_block, need_wal, m * 2 * sizeof(T)),
          upper_layer(index, metap->upper_block, need_wal, (m + 1) * 2 * sizeof(T))
    {
        if (need_wal) {
            xlog.init(index, metabuf, BufferGetPage(metabuf));
        }
        if (qt_type == QuantizerType::PQ) {
            elem_size = metap->quantizer_metainfo.get_pq_metainfo().code_size();
        } else if (qt_type == QuantizerType::RABITQ) {
            elem_size = metap->quantizer_metainfo.get_rabitq_meta().quant_size;
        } else {
            elem_size = vec_size;
        }
        point_info_buf = (T *)palloc(upperpoint_size());
    }

    void destroy()
    {
        base_layer.destroy();
        upper_layer.destroy();
        elems.destroy();
        CONSTEXPR_IF (WithBulkbuf) {
            bulkbuf->release();
        } else {
            neighbors_val_pool.destroy();
        }
        pfree(point_info_buf);
        REPORT_PERF(NOTICE);
        PERF_DESTROY();
    }

    bool use_async_io() const
    {
        CONSTEXPR_IF (WithBulkbuf) {
            return false;
        } else {
            return dim_cached(elem_size) && is_aio_beneficial();
        }
    }

    template <typename Distancer>
    void get_distance_batch(const Distancer &d, const char *query, const Vector<T> &ids, float *dists)
    {
        const uint_fast16_t num = ids.size();
        CONSTEXPR_IF (WithBulkbuf) {
            void *vals[num];
            void **val_cur = vals;
            DO_PERF_COUNT(read_vec, num);
            for (T id : ids) {
                *val_cur = bulkbuf->get(id);
                ++val_cur;
            }
            STOP_PERF(read_vec);
            DO_PERF_COUNT(calc, num);
            d.get_distance_batch2(query, vals, dim, num, dists);
            STOP_PERF(calc);
            return;
        } else if (!use_async_io() || num < MIN_ASYNC_IO_BATCH_NUM) {
            for (T id : ids) {
                DO_PERF(read_vec);
                VecBuffer vec_buf = vec_read_buffer(index, id, elem_size, st);
                char *val = vec_buf.get_vecbuf();
                STOP_PERF(read_vec);
                DO_PERF(calc);
                *dists = d.get_distance_single((void *)query, (void *)val, dim);
                STOP_PERF(calc);
                ++dists;
                vec_buf.release();
            }
            return;
        }
        VecReadRequest *requests = (VecReadRequest *)palloc0(sizeof(VecReadRequest) * num);
        for (size_t i = 0; i < num; i++) {
            requests[i].loc = ids[i];
            requests[i].rel = index;
        }
        async_vec_read_batch(index, st, elem_size, requests, num);
        uint16 uncompleted_indices[num];
        uint16 uncompleted_count = 0;
        for (uint_fast16_t i = 0; i < num; ++i) {
            if (requests[i].io_ready) {
                float dist = d.get_distance_single((void *)query, (void *)requests[i].buf, dim);
                dists[i] = dist;
                requests[i].release();
            } else {
                uncompleted_indices[uncompleted_count++] = i;
            }
        }

        uint16 completed_indices[uncompleted_count];
        while (uncompleted_count > 0) {
            uint_fast16_t n = async_vec_wait_batch(requests, completed_indices,
                                                   uncompleted_indices, &uncompleted_count);
            for (uint_fast16_t i = 0; i < n; ++i) {
                VecReadRequest &req = requests[completed_indices[i]];
                float dist = d.get_distance_single((void *)query, (void *)req.buf, dim);
                req.release();
                dists[completed_indices[i]] = dist;
            }
        }
        pfree(requests);
    }

    template <typename Distancer>
    float get_distance(const Distancer &d, const char *query, T id)
    {
        float dist = 0;
        CONSTEXPR_IF (WithBulkbuf) {
            DO_PERF(read_vec);
            const char *val = bulkbuf->get(id);
            STOP_PERF(read_vec);
            DO_PERF(calc);
            dist = d.get_distance_single((void *)query, (void *)val, dim);
            STOP_PERF(calc);
        } else {
            DO_PERF(read_vec);
            VecBuffer vec_buf = vec_read_buffer(index, id, elem_size, st);
            char *val = vec_buf.get_vecbuf();
            STOP_PERF(read_vec);
            DO_PERF(calc);
            dist = d.get_distance_single((void *)query, (void *)val, dim);
            STOP_PERF(calc);
            vec_buf.release();
        }
        return dist;
    }

    template <typename Distancer>
    float get_distance(const Distancer &d, const char *query, const char *val)
    {
        DO_PERF(calc);
        float res = d.get_distance_single((void *)query, (void *)val, dim);
        STOP_PERF(calc);
        return res;
    }

    template <typename Distancer>
    float get_distance_precise(const Distancer &d, const char *query, const char *val)
        { return d.get_distance_precise((void *)query, (void *)val, dim); }

    template <typename Distancer>
    float get_distance_est(const Distancer &d, const char *query, T id)
    {
        float dist = 0;
        CONSTEXPR_IF (WithBulkbuf) {
            DO_PERF(read_vec);
            const char *val = bulkbuf->get(id);
            STOP_PERF(read_vec);
            DO_PERF(calc);
            dist = d.get_distance_est_single((void *)query, (void *)val, dim);
            STOP_PERF(calc);
        } else {
            DO_PERF(read_vec);
            VecBuffer vec_buf = vec_read_buffer(index, id, elem_size, st);
            char *val = vec_buf.get_vecbuf();
            STOP_PERF(read_vec);
            DO_PERF(calc);
            dist = d.get_distance_est_single((void *)query, (void *)val, dim);
            STOP_PERF(calc);
            vec_buf.release();
        }
        return dist;
    }

    template <bool is_base_layer>
    static Pair<float *, BitSpan<uint>> get_neighbor_stats(T id)
        { return Pair<float *, BitSpan<uint>>(nullptr, nullptr, 0, 0); }
    static bool has_stat(BitSpan<uint> stat) { return false; }
    static void set_stat(BitSpan<uint> stat) {}

    T get_vector_num() const { return base_layer.size(); }
    T get_upper_num() const { return upper_layer.size(); }
    uint_fast16_t get_m() const { return m; }
    uint_fast16_t get_dim() const { return dim; }
    uint_fast32_t get_vecsize() const { return vec_size; }
    uint_fast32_t get_elemsize() const { return elem_size; }
    DistPrecisionType get_precision() const { return metap->precision_type; }
    GraphIndexStats get_stats() const { return &metap->stats; }
    void lock_stats() const {}
    void unlock_stats() const {}
    template <typename F>
    void apply_stats_meta_wal(F &&f) const {}

    void add_async_id(T id)
    {
        disk_container::FreeSpace<T> freespace{index, metap->async_id_list_block, need_wal};
        freespace.insert(id);
        freespace.destroy();
    }

    size_t get_async_id_count()
    {
        disk_container::DiskVector<T> async_list{index, metap->async_id_list_block, false};
        size_t count = async_list.size();
        async_list.destroy();
        return count;
    }

    template <typename F>
    void for_each_async_id(F &&callback)
    {
        disk_container::DiskVector<T> async_list{index, metap->async_id_list_block, false};
        size_t count = async_list.size();
        if (count == 0) {
            async_list.destroy();
            return;
        }
        auto visitor = async_list.template visit<AccessorLockType::ReadLock>(
            [&callback](const T *data, size_t n) {
                for (size_t i = 0; i < n; ++i) {
                    callback(data[i]);
                }
            }
        );
        visitor(0, count);
        async_list.destroy();
    }

    template <bool is_base_layer>
    T assign_vector_id()
    {
        DO_PERF(write_node);
        T id = (T)INVALID_VECTOR_ID;
        LockBuffer(metabuf, BUFFER_LOCK_SHARE);
        bool can_reuse = !metap->vacuum_flag;
        LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
        CONSTEXPR_IF (is_base_layer) {
            if (can_reuse) {
                disk_container::FreeSpace<T> freespace{index, metap->free_id_list_block, need_wal};
                can_reuse = freespace.pop(id);
                freespace.destroy();
            }
            if (!can_reuse) {
                id = base_layer.append();
                LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
                metap->num_vectors = id + 1;
                MarkBufferDirty(metabuf);
                if (need_wal) {
                    xlog.update_num_vector(metap->num_vectors);
                }
                LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
            }
        } else {
            if (can_reuse) {
                disk_container::FreeSpace<T> freespace{index, metap->free_upper_list_block, need_wal};
                can_reuse = freespace.pop(id);
                freespace.destroy();
            }
            if (!can_reuse) {
                id = upper_layer.append();
            }
        }
        STOP_PERF(write_node);
        return id;
    }

    void add_upperpoint(T cur_layer_idx, T lower_layer_idx, T id, T *neighbors_info)
    {
        DO_PERF(write_node);
        const auto set_point = [&](GraphIndexDiskUpperPoint<T> *upperpoint) -> bool {
            upperpoint->lower_layer_idx = lower_layer_idx;
            upperpoint->id = id;
            memcpy(upperpoint->neighbors_info, neighbors_info, neighbors_size());
            return true;
        };
        upper_layer.template apply<AccessorLockType::WriteLock>(set_point)(cur_layer_idx);
        STOP_PERF(write_node);
    }

    void add_basepoint(T id, T *neighbors_id)
    {
        DO_PERF(write_node);
        const auto set_point = [&](GraphIndexDiskBasePoint<T> *basepoint) -> bool {
            memcpy(basepoint->neighbors_id, neighbors_id, neighbors_size());
            return true;
        };
        base_layer.template apply<AccessorLockType::WriteLock>(set_point)(id);
        STOP_PERF(write_node);
    }

    void add_vector(T id, const char *query)
    {
        DO_PERF(write_vec);
        vec_write(index->rd_smgr, elem_size * id, elem_size, query, false, st);
        CONSTEXPR_IF (WithBulkbuf) {
            bulkbuf->update(id, query);
        }
        if (need_wal) {
            xlog.add_vector(query, elem_size * id, elem_size, st);
        }
        STOP_PERF(write_vec);
    }

    template <typename Distancer>
    void add_vector(T id, const char *query, Distancer &distancer)
    {
        if (st != VecStorageType::PureCode) {
            add_vector(id, query);
            return;
        }

        DO_PERF(write_vec);
        char *code = alloc_vector(elem_size);
        distancer.compute_code((float *)query, code);
        vec_write(index->rd_smgr, elem_size * id, elem_size, code, false, st);
        CONSTEXPR_IF (WithBulkbuf) {
            bulkbuf->update(id, code);
        }
        if (need_wal) {
            xlog.add_vector(code, elem_size * id, elem_size, st);
        }
        free_vector(code);
        STOP_PERF(write_vec);
    }

    template <bool is_base_layer>
    void get_neighbors(Vector<GraphIndexCandidate<T>> &neighbors, const GraphIndexCandidate<T> &point)
    {
        DO_PERF(read_neighbor);
        CONSTEXPR_IF (is_base_layer) {
            const auto fill_neighbors = [&](const GraphIndexDiskBasePoint<T> *basepoint) -> void {
                const T *neighbors_id = basepoint->neighbors_id;
                for (uint_fast16_t i = 0; i < m * 2; ++i) {
                    T id = neighbors_id[i];
                    if (!is_valid(id)) {
                        break;
                    }
                    neighbors.emplace_back(id, id, INVALID_VECTOR_ID, INVALID_DIST, nullptr);
                }
            };
            base_layer.template visit<AccessorLockType::ReadLock>(fill_neighbors)(point.id);
        } else {
            const auto fill_neighbors = [&](const GraphIndexDiskUpperPoint<T> *upperpoint) -> void {
                const T *neighbors_id = upperpoint->neighbors_info;
                const T *neighbors_cur_layer_idx = neighbors_id + m;
                for (uint_fast16_t i = 0; i < m; ++i) {
                    T id = neighbors_id[i];
                    if (!is_valid(id)) {
                        break;
                    }
                    T cur_layer_idx = neighbors_cur_layer_idx[i];
                    neighbors.emplace_back(id, cur_layer_idx, INVALID_VECTOR_ID, INVALID_DIST, nullptr);
                }
            };
            upper_layer.template visit<AccessorLockType::ReadLock>(fill_neighbors)(point.cur_layer_idx);
        }
        STOP_PERF(read_neighbor);
    }

    template <bool is_base_layer>
    tuple<T *, T, T> get_point_info(T cur_layer_idx)
    {
        DO_PERF(read_neighbor);
        CONSTEXPR_IF (is_base_layer) {
            base_layer.template get_n<AccessorLockType::ReadLock>(cur_layer_idx, 1, (GraphIndexDiskBasePoint<T> *)point_info_buf);
            STOP_PERF(read_neighbor);
            return {point_info_buf, INVALID_VECTOR_ID, INVALID_VECTOR_ID};
        }
        upper_layer.template get_n<AccessorLockType::ReadLock>(cur_layer_idx, 1, (GraphIndexDiskUpperPoint<T> *)point_info_buf);
        T *neighbors_info = point_info_buf + 2;
        T lower_layer_idx = point_info_buf[0];
        T id = point_info_buf[1];
        STOP_PERF(read_neighbor);
        return {neighbors_info, lower_layer_idx, id};
    }

    template <bool is_base_layer>
    void set_neighbor(T cur_layer_idx, uint16 update_nbr_idx, T newpoint_id, T newpoint_cur_layer_idx)
    {
        DO_PERF(write_neighbor);
        CONSTEXPR_IF (is_base_layer) {
            const auto set_one_neighbor = [&](GraphIndexDiskBasePoint<T> *basepoint) -> Pair<char *, size_t> {
                basepoint->neighbors_id[update_nbr_idx] = newpoint_id;
                return {(char *)&basepoint->neighbors_id[update_nbr_idx], sizeof(T)};
            };
            base_layer.template apply<AccessorLockType::WriteLock>(set_one_neighbor)(cur_layer_idx);
        } else {
            const auto set_neighbor_id = [&](GraphIndexDiskUpperPoint<T> *upperpoint) -> Pair<char *, size_t> {
                upperpoint->neighbors_info[update_nbr_idx] = newpoint_id;
                return {(char *)&upperpoint->neighbors_info[update_nbr_idx], sizeof(T)};
            };
            upper_layer.template apply<AccessorLockType::WriteLock>(set_neighbor_id)(cur_layer_idx);
            const auto set_neighbor_cur_layer = [&](GraphIndexDiskUpperPoint<T> *upperpoint) -> Pair<char *, size_t> {
                upperpoint->neighbors_info[m + update_nbr_idx] = newpoint_cur_layer_idx;
                return {(char *)&upperpoint->neighbors_info[m + update_nbr_idx], sizeof(T)};
            };
            upper_layer.template apply<AccessorLockType::WriteLock>(set_neighbor_cur_layer)(cur_layer_idx);
        }
        STOP_PERF(write_neighbor);
    }

    void set_base_neighbors(T id, T *neighbors_id)
    {
        const auto set_neighbors = [&](GraphIndexDiskBasePoint<T> *basepoint) -> bool {
            memcpy(basepoint->neighbors_id, neighbors_id, neighbors_size());
            return true;
        };
        base_layer.template apply<AccessorLockType::WriteLock>(set_neighbors)(id);
    }

    void set_upper_neighbors(T cur_layer_idx, T *neighbors_info)
    {
        const auto set_neighbors = [&](GraphIndexDiskUpperPoint<T> *upperpoint) -> bool {
            memcpy(upperpoint->neighbors_info, neighbors_info, neighbors_size());
            return true;
        };
        upper_layer.template apply<AccessorLockType::WriteLock>(set_neighbors)(cur_layer_idx);
    }

    template <typename F>
    auto apply_elem(T id, F &&f)
        { return elems.template apply<AccessorLockType::WriteLock>(std::forward<F>(f))(id); }

    template <bool with_lock, bool force_share_flag = false>
    Pair<GraphIndexEntryInfo, bool> get_entry(int_fast8_t insert_level = 0)
    {
        DO_PERF(lock);
        bool shared = true;
        CONSTEXPR_IF (with_lock) {
            /* wait vacuum to operate atomically */
            LockPage(index, metablkno, ShareLock);
            if ((!force_share_flag && unlikely(insert_level > metap->entry_level)) ||
                unlikely(metap->entry_level < 0)) {
                UnlockPage(index, metablkno, ShareLock);
                LockPage(index, metablkno, ExclusiveLock);
                shared = false;
            }
        }
        init_entrypoint();
        STOP_PERF(lock);
        return {entry_info, shared};
    }

    void release_entry_lock(bool shared)
    {
        if (shared) {
            UnlockPage(index, metablkno, ShareLock);
        } else {
            UnlockPage(index, metablkno, ExclusiveLock);
        }
    }

    void init_entrypoint()
    {
        LockBuffer(metabuf, BUFFER_LOCK_SHARE);
        entry_info.id = metap->entrypoint_id;
        entry_info.cur_layer_idx = metap->entry_cur_layer_idx;
        entry_info.level = metap->entry_level;
        LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
    }

    void set_entrypoint(size_t id, size_t cur_layer_idx, int_fast8_t level)
    {
        DO_PERF(lock);
        LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
        STOP_PERF(lock);
        entry_info.id = id;
        entry_info.cur_layer_idx = cur_layer_idx;
        entry_info.level = level;
        metap->entrypoint_id = id;
        metap->entry_cur_layer_idx = cur_layer_idx;
        metap->entry_level = level;
        MarkBufferDirty(metabuf);
        if (need_wal) {
            xlog.update_entry(entry_info);
        }
        LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
    }

    void set_vacuum_flag(bool flag)
    {
        LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
        metap->vacuum_flag = flag;
        MarkBufferDirty(metabuf);
        if (need_wal) {
            xlog.update_vacuum_flag(flag);
        }
        LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
    }

    void add_elem(PointExtensionContext &ctx, T id, const ItemPointerData &tid)
    {
        DO_PERF(write_node);
        elems.extend(id + 1);
        if constexpr (clustered) {
            typename point_type::Data d;
            d.tid = tid;
            // TD
            point_type elem(ctx, d);
            elems.template set<AccessorLockType::WriteLock>(id, elem);
        } else {
            point_type elem(ctx, tid);
            elems.template set<AccessorLockType::WriteLock>(id, elem);
        }
        STOP_PERF(write_node);
    }

    template<typename F>
    void get_itempointer(T id, F &&func)
    {
        DO_PERF(read_node);
        elems.template visit<AccessorLockType::ReadLock>(func)(id);
        STOP_PERF(read_node);
    }

    void reset_neighbors_val_pool()
    {
        CONSTEXPR_IF (!WithBulkbuf) {
            neighbors_val_pool->reset();
        }
    }

    char *get_data(T id)
    {
        DO_PERF(read_vec);
        char *res;
        CONSTEXPR_IF (WithBulkbuf) {
            res = bulkbuf->get(id);
        } else {    
            if (!neighbors_val_pool.has_value()) {
                neighbors_val_pool.emplace(elem_size, metap->ef_construction);
            }
            VecBuffer vec_buf = vec_read_buffer(index, id, elem_size, st);
            char *val = vec_buf.get_vecbuf();
            res = neighbors_val_pool->set(val);
            vec_buf.release();
        }
        STOP_PERF(read_vec);
        return res;
    }

    VecBuffer pin_vector_buffer(T id)
    {
        CONSTEXPR_IF (WithBulkbuf) {
            char *buf = alloc_vector(elem_size);
            memcpy(buf, bulkbuf->get(id), elem_size);
            return VecBuffer(-1, 0, 0, buf);
        } else {
            return vec_read_buffer(index, id, elem_size, st);
        }
    }

    void fetch_vec_via_slot(HeapTuple tuple, char *vec)
    {
        Oid func_oid = InvalidOid;
        int attnum = index->rd_index->indkey.values[0];
        if (attnum == 0) { /* function expression */
            if (!index->rd_indexprs) {
                RelationGetIndexExpressions(index);
            }
            FuncExpr *func_expr = (FuncExpr *)linitial(index->rd_indexprs);
            func_oid = func_expr->funcid;
            attnum = ((Var *)linitial(func_expr->args))->varattno;
        }

        Assert(heap->rd_tableam != NULL);
        bool is_null;
        Datum value = heap_getattr(tuple, attnum, RelationGetDescr(heap), &is_null);
        Assert(!is_null);

        Datum d;
        char *func_name = OidIsValid(func_oid) ? get_func_name(func_oid) : NULL;
        if (func_name != NULL &&
            (strcmp(func_name, "array_to_floatvector") == 0 || strcmp(func_name, "array_to_halfvector") == 0)) {
            PGFunction func = metap->precision_type == DistPrecisionType::FLOAT
                ? array_to_floatvector
                : array_to_halfvector;
            d = DirectFunctionCall2(func, value, Int32GetDatum(dim));
        } else if (func_name != NULL &&
                   (strcmp(func_name, "subfloatvector") == 0 || strcmp(func_name, "halfvector_subvector") == 0)) {
            FuncExpr *func_expr = (FuncExpr *)linitial(index->rd_indexprs);
            Assert(list_length(func_expr->args) == 3);
            ListCell *lc = list_nth_cell(func_expr->args, 1);
            Const *c = (Const *)lfirst(lc);
            Assert(IsA(c, Const) && c->consttype == INT4OID);
            Datum arg2 = c->constvalue;
            lc = list_nth_cell(func_expr->args, 2);
            c = (Const *)lfirst(lc);
            Assert(IsA(c, Const) && c->consttype == INT4OID);
            Datum arg3 = c->constvalue;
            PGFunction func = metap->precision_type == DistPrecisionType::FLOAT
                ? subfloatvector
                : halfvector_subvector;
            d = DirectFunctionCall3(func, value, arg2, arg3);
        } else {
            d = value;
        }

        if (metap->precision_type == DistPrecisionType::FLOAT) {
            FloatVector *data = DatumGetFloatVector(d);
            memcpy(vec, data->x, vec_size);

            if (PointerGetDatum(data) != d) {
                pfree(data);
            }
        } else if (metap->precision_type == DistPrecisionType::HALF) {
            HalfVector *data = DatumGetHalfVector(d);
            half *src = data->x;
            half *dst = (half *)vec;
            memcpy(dst, src, vec_size);

            if (PointerGetDatum(data) != d) {
                pfree(data);
            }
        } else {
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("fetch_vec_via_slot only supports floatvector and halfvector")));
        }

        if (func_name != NULL) {
            pfree(func_name);
        }
    }

    bool fetch_vec_from_heap(ItemPointerData tid, char *dest)
    {
        DO_PERF(fetch);
        alignas(PG_IO_ALIGN_SIZE) char tuple_buf[BLCKSZ] = {0};
        HeapTuple tuple = (HeapTupleData *)tuple_buf;
        /* openGauss: tuple->tupTableType = HEAP_TUPLE; */
        tuple->t_data = (HeapTupleHeader)((char *)tuple + HEAPTUPLESIZE);
        tuple->t_self = tid;
        SnapshotData snap_dirty;
        InitDirtySnapshot(snap_dirty);
        Buffer buf;
        bool res = heap_fetch(heap, &snap_dirty, tuple, &buf, false);
        if (res) {
            fetch_vec_via_slot(tuple, dest);
            ReleaseBuffer(buf);
        }
        STOP_PERF(fetch);
        return res;
    }
    char *fetch_vec_from_heap(ItemPointerData tid)
    {
        DO_PERF(fetch);
        alignas(PG_IO_ALIGN_SIZE) char tuple_buf[BLCKSZ] = {0};
        HeapTuple tuple = (HeapTupleData *)tuple_buf;
        /* openGauss: tuple->tupTableType = HEAP_TUPLE; */
        tuple->t_data = (HeapTupleHeader)((char *)tuple + HEAPTUPLESIZE);
        tuple->t_self = tid;
        SnapshotData snap_dirty;
        InitDirtySnapshot(snap_dirty);
        Buffer buf;
        bool fetched = heap_fetch(heap, &snap_dirty, tuple, &buf, false);
        char *res = NULL;
        if (fetched) {
            res = alloc_vector(vec_size);
            fetch_vec_via_slot(tuple, res);
            ReleaseBuffer(buf);
        }
        STOP_PERF(fetch);
        return res;
    }

    char *fetch_vec_from_heap(PointExtensionContext &ctx, T id)
    {
        return elems.template visit<AccessorLockType::ReadLock>(
            [&](const point_type &elem) -> char * {
            char *res = NULL;
            elem.apply_on_tids(ctx, [&](const ItemPointerData &tid) -> bool {
                res = fetch_vec_from_heap(tid);
                return res != NULL;
            });
            return res;
        })(id);
    }

    bool fetch_vec_from_heap(PointExtensionContext &ctx, T id, char *dest)
    {
        return elems.template visit<AccessorLockType::ReadLock>(
            [&](const point_type &elem) -> bool {
            bool res = false;
            elem.apply_on_tids(ctx, [&](const ItemPointerData &tid) -> bool {
                res = fetch_vec_from_heap(tid, dest);
                return res;
            });
            return res;
        })(id);
    }

    size_t remove_heaptids(PointExtensionContext &ctx, UnorderedSet<size_t> &deleted,
        IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, void *callback_state)
    {
        size_t remove_elem_num = elems.size();
        size_t start_idx = 0;
        const auto collect_deleted_tids = [&](GraphIndexPoint *elem, size_t batch_size, bool *dirty) -> void {
            vacuum_delay_point();
            for (size_t i = 0; i < batch_size; ++i) {
                bool is_dirty = false;
                uint32 num_removed = elem[i].vacuum_tids(
                    [callback, callback_state](const ItemPointerData &tid) -> bool {
                        return callback(const_cast<ItemPointer>(&tid), callback_state);
                    }, ctx, is_dirty);
                stats->tuples_removed += num_removed;
                dirty[i] = is_dirty;
                if (elem[i].empty() && !elem[i].is_deleted()) {
                    T id = start_idx + i;
                    deleted.emplace(id);
                }
            }
            start_idx += batch_size;
        };
        elems.template apply<AccessorLockType::WriteLock>(collect_deleted_tids)(0, remove_elem_num);
        return remove_elem_num;
    }

    void mark_deleted(size_t basepoint_num, size_t upperpoint_num)
    {
        Oid relNode = index->rd_smgr->smgr_rlocator.locator.relNumber;
        /* Pass 1: Mark elements as deleted based on their own state, collect recyclable IDs */
        size_t elem_offset = 0;
        Vector<T> recycled_ids;
        const auto mark_elem = [&](GraphIndexPoint *elem, size_t batch_size, bool *dirty) -> void {
            for (size_t i = 0; i < batch_size; ++i) {
                /*
                 * skip:
                 * 1. live elements
                 * 2. already-deleted elements, marked by previous vacuum but not been reused
                 */
                if (!elem[i].empty() || elem[i].is_deleted()) {
                    dirty[i] = false;
                    continue;
                }
                elem[i].set_deleted();
                size_t id = elem_offset + i;
                vec_invalidate_buffer_cache(relNode, id, elem_size);
                xlog.log_invalidate_vector_cache(id, elem_size);
                recycled_ids.push_back((T)(elem_offset + i));
                dirty[i] = true;
            }
            elem_offset += batch_size;
        };
        elems.template apply<AccessorLockType::WriteLock>(mark_elem, 1)(0, basepoint_num);

        if (recycled_ids.empty()) {
            recycled_ids.destroy();
            return;
        }

        /* Pass 2: Clear base layer neighbors for newly-deleted elements */
        for (size_t i = 0; i < recycled_ids.size(); ++i) {
            base_layer.template apply<AccessorLockType::WriteLock>(
                [&](GraphIndexDiskBasePoint<T> *basepoint) -> bool {
                    basepoint->init(m);
                    return true;
                }
            )(recycled_ids[i]);
        }

        /* Pass 3: Clear upper layer neighbors for newly-deleted elements, collect recyclable upper indices */
        UnorderedSet<T> deleted_set(recycled_ids.size());
        for (size_t i = 0; i < recycled_ids.size(); ++i) {
            deleted_set.emplace(recycled_ids[i]);
        }
        Vector<T> recycled_upper_indices;
        size_t upper_offset = 0;
        upper_layer.template apply<AccessorLockType::WriteLock>(
            [&](GraphIndexDiskUpperPoint<T> *upperpoint, size_t batch_size, bool *dirty) -> void {
                for (size_t i = 0; i < batch_size; ++i) {
                    if (!deleted_set.contains(upperpoint[i].id)) {
                        dirty[i] = false;
                        continue;
                    }
                    upperpoint[i].init(m);
                    recycled_upper_indices.push_back((T)(upper_offset + i));
                    dirty[i] = true;
                }
                upper_offset += batch_size;
            }
        )(0, upperpoint_num);
        deleted_set.destroy();

        /* Recycle deleted element IDs into free list for reuse */
        disk_container::FreeSpace<T> freespace{index, metap->free_id_list_block, need_wal};
        freespace.insert(recycled_ids.data(), recycled_ids.size());
        freespace.destroy();
        recycled_ids.destroy();

        /* Recycle deleted upper point indices into upper free list for reuse */
        disk_container::FreeSpace<T> upper_freespace{index, metap->free_upper_list_block, need_wal};
        upper_freespace.insert(recycled_upper_indices.data(), recycled_upper_indices.size());
        upper_freespace.destroy();
        recycled_upper_indices.destroy();
    }

    void inspect(IndexInspectResult &res)
    {
        res.append_attr("Neighbor Degree (m)");
        res.fill_content("%hu", m);
        res.append_attr("Base Layer Neighbor Container Used Size");
        res.fill_content((base_layer.get_nblocks() + 0) * BLCKSZ);
        res.append_attr("Base Layer Neighbor Container Required Size");
        res.fill_content(base_layer.size() * neighbors_size());
        res.append_attr("Base Layer Neighbor Number of Entries");
        res.fill_content("%lu", base_layer.size() - 0);
        res.append_attr("Base Layer Neighbor Reserved Number of Entries");
        res.fill_content("%lu", base_layer.capacity() - base_layer.size() + 0);

        res.append_attr("Upper Layer Neighbor Container Used Size");
        res.fill_content((upper_layer.get_nblocks() + 0) * BLCKSZ);
        res.append_attr("Upper Layer Neighbor Container Required Size");
        res.fill_content(upper_layer.size() * upperpoint_size());
        res.append_attr("Upper Layer Neighbor Number of Entries");
        res.fill_content("%lu", upper_layer.size() - 0);
        res.append_attr("Upper Layer Neighbor Reserved Number of Entries");
        res.fill_content("%lu", upper_layer.capacity() - upper_layer.size() + 0);

        res.append_attr("Elements Container Used Size");
        res.fill_content((elems.get_nblocks() + 0) * BLCKSZ);
        res.append_attr("Elements Container Required Size");
        res.fill_content(elems.size() * sizeof(point_type));
        res.append_attr("Elements Number of Entries");
        res.fill_content("%lu", elems.size() - 0);
        res.append_attr("Elements Reserved Number of Entries");
        res.fill_content("%lu", elems.capacity() - elems.size() + 0);

        res.append_attr("Async Pending Count");
        res.fill_content("%lu", get_async_id_count());
    }

    Relation get_index() const { return index; }
    Relation get_heap() const { return heap; }
    template <bool, bool> static constexpr void lock_point(T) {}
    template <bool, bool> static constexpr void unlock_point(T) {}
private:
    class NeighborsValPool {
    /* 
     * this pool is used in `apply_arrangement` to check duplicate and `select_neighbors` to store
     * nbrs' id temporarily. It should not bigger than `ef_construction` + 1.
     */
    public:
        NeighborsValPool(size_t elem_size_val, uint_fast16_t ef_construction)
            : elem_size(((elem_size_val + ann_helper::vector_aligned_size - 1) /
                        ann_helper::vector_aligned_size) * ann_helper::vector_aligned_size),
              pool_size(ef_construction + 1),
              pool(alloc_vector(elem_size, pool_size)),
              idx(0) {}
        char *set(char *val)
        {
            char *dest = pool + idx * elem_size;
            Assert(idx < pool_size);
            memcpy(dest, val, elem_size);
            ++idx;
            return dest;
        }
        void reset() { idx = 0; }
        void destroy() { free_vector(pool); }
    private:
        size_t elem_size;
        uint32 pool_size;
        char *pool;
        uint16 idx;
    };

    Relation index;
    Relation heap;
    GraphIndexMetaPage metap;
    Buffer metabuf;

    bool need_wal;
    uint_fast16_t m;
    uint_fast16_t dim;
    DistPrecisionType precision_type;
    QuantizerType qt_type;
    VecStorageType st;
    uint_fast32_t vec_size;
    uint_fast32_t elem_size;
    BulkBuffer *bulkbuf;
    GraphIndexXlog xlog;
    GraphIndexEntryInfo entry_info;
    disk_container::DiskVector<point_type> elems;
    Optional<NeighborsValPool> neighbors_val_pool; /* used in algorithm:select_neighbors() */
    T *point_info_buf;

    size_t neighbors_size() const { return base_layer.data_size(); }
    size_t upperpoint_size() const { return upper_layer.data_size(); }
    bool is_valid(T id) { return likely(id != (T)INVALID_VECTOR_ID); }
public:
    disk_container::VarDiskVector<GraphIndexDiskBasePoint<T>> base_layer;
    disk_container::VarDiskVector<GraphIndexDiskUpperPoint<T>> upper_layer;
};

using DiskStoreVariant = Variant<
    DiskStore<uint32, false>,
    DiskStore<uint32, true>,
    DiskStore<size_t, false>,
    DiskStore<size_t, true>,
    DiskStore<uint32, false, GraphIndexCluster>
>;

inline void create_disk_store(DiskStoreVariant &var, Relation index, Relation heap, Buffer metabuf,
                              BulkBuffer *bulkbuf, bool need_wal)
{
    if (need_wal) {
        need_wal = RelationNeedsWAL(index);
    }
    if (GRAPH_INDEX_PAGE_GET_META(BufferGetPage(metabuf))->id_type == IdType::U32) {
        if (bulkbuf != NULL) {
            var.emplace<DiskStore<uint32, true>>(index, heap, metabuf, bulkbuf, need_wal);
        } else {
            var.emplace<DiskStore<uint32, false>>(index, heap, metabuf, nullptr, need_wal);
        }
    } else {
        if (bulkbuf != NULL) {
            var.emplace<DiskStore<size_t, true>>(index, heap, metabuf, bulkbuf, need_wal);
        } else {
            var.emplace<DiskStore<size_t, false>>(index, heap, metabuf, nullptr, need_wal);
        }
    }
}

#endif /* GRAPH_INDEX_STORAGE_INTERFACE_H */
