/**
 * Copyright (c) 2026 VexDB-THU
 */

#ifndef GRAPH_INDEX_STORAGE_INTERFACE_H
#define GRAPH_INDEX_STORAGE_INTERFACE_H

#include <cfloat>
#include <atomic>
#include <shared_mutex>
#include <mutex>
#include <vtl/vector>
#include <vtl/hashtable>
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
#include "global_instance.h"
/* AIO deferred - #include "storage/aio_worker.h" */
#include "graph_index/graph_index_cluster.h"
#include "graph_index/graph_index.h"
#include "graph_index/graph_index_xlog.h"
/* graph_index_state.h is PG-only; included via vexdb_pg/include/ */
#include "graph_index/graph_index_state.h"
#include "module/perf_usage.h"
#include "vector_page_storage.h"
#include "ann_utils.h"
#include "floatvector.h"
#include "index_inspect.h"
#if defined(PG_VEXDB_TARGET_PG)
#include "graph_index/parallel_build_locks.h"
#endif

PERF_DECLARE_CATS(MemPerfCats, true, read, write, calc, lock);
PERF_DECLARE_CATS(DiskPerfCats, false, read_node, read_neighbor, read_vec, write_node, write_neighbor, write_vec, calc, lock, fetch);

/*
 * Graph Index storage layer: two-tier design.
 *
 * Build-time:  MemStore (all in-memory, OOM triggers flush to DiskStore)
 *   - Distance cache: embedded into basepoint_pool            — no hash lookup cost
 *   - Point locking:  MemPool's RWBitLock (per-chunk)         — fine-grained concurrency
 *   - ID assignment:  atomic fetch_add                        — lock-free
 *   - Async I/O:      not supported                           — build is pure memory
 *   - OOM handling:   flush accumulated data to DiskStore
 *
 * Runtime:     DiskStore (PostgreSQL page access)
 *   - Distance cache: UnorderedMap<Pair<T,T>, float>          — on-demand, cleared after backend_insert
 *   - Point locking:  LockPage/UnlockPage                     — PostgreSQL buffer manager
 *   - ID assignment:  FreeSpace reuse + append                — supports vacuum recycling
 *   - Vector reads:   shared_buffers + backend scratch         — no second cache
 *   - Storage:        PlainStore overflow is chained via element flags (extended / double extended)
 *
 * DiskStoreVariant (2 instantiations):
 *   U32 (1), U64 (1)
 *   Selected at runtime by create_disk_store() based on IdType.
 *
 * Entry-point lock protocol (applies to both MemStore and DiskStore):
 *   1. Acquire entry_waitlock (EXCLUSIVE)
 *   2. Acquire entry_lock (SHARED for read/insert, EXCLUSIVE if insert_level > entry_level)
 *   3. Release entry_waitlock
 *   4. Do work
 *   5. Release entry_lock
 */

template <typename IdType = uint32, typename elem_type = GraphIndexPoint>
class MemStore : public PERFER(MemPerfCats) {
    using PerfCats = MemPerfCats;
    using PlainStore = disk_container::PlainStore;
public:
    using T = IdType;
    using point_type = elem_type;
    using point_type_data = typename elem_type::Data;

    class MemPool {
        /*
         * Chunk-based memory pool for lock-free concurrent graph build.
         *
         * Each pool pre-allocates `vec` (chunk array) and `locks` with capacity
         * `pre_alloc_vec_size`, so they never reallocate during build.  This is
         * critical because `get()` reads `vec[chunk_no]` without any lock -- any
         * reallocation would cause concurrent readers to dereference freed memory.
         *
         * Memory budget accounting:
         *   - `target_chunk_size_mb` (MB): target size per chunk, fed to
         *     calculate_pow() which rounds down to a power-of-2 element count.
         *   - `total_budget_mb` (MB): total memory budget for this pool.
         *   - compute_pre_alloc() converts both to bytes and returns enough
         *     chunk slots to cover the budget:
         *       pre_alloc_vec_size = total_budget_mb * 1MB / chunk_size + 10
         *     The +10 margin covers rounding from calculate_pow().
         */
        struct Chunk {
            char *buf;
            Chunk(char *b) : buf(b) {}
        };
        uint32 elem_size; /* Bytes per elem */
        uint32 pow_elem_nums_per_chunk; /* 2^x elems in a chunk */
        uint32 one_chunk_elem_nums; /* equal to 1 << `pow_elem_nums_per_chunk` */
        size_t chunk_size; /* Bytes per chunk, equal to `elem_size` * `one_chunk_elem_nums` */
        size_t pre_alloc_vec_size;
        Vector<Chunk> vec;
        Vector<RWBitLock> locks; /* every chunk has one RWBitLock with size `one_chunk_elem_nums` */
        MemoryContext ctx;
        std::mutex mutex;

        uint32 get_chunk_no(T idx) const { return idx >> pow_elem_nums_per_chunk; }
        uint32 get_chunk_offset(T idx) const { return (idx & (one_chunk_elem_nums - 1)) * elem_size; }
        uint32 get_lock_idx(T idx) const { return idx & (one_chunk_elem_nums - 1); }

        static uint32 get_align_elem_size(uint32 store_esize)
        {
            return ((store_esize + ann_helper::vector_aligned_size - 1) /
                   ann_helper::vector_aligned_size) * ann_helper::vector_aligned_size;
        }

        /* Compute pre_alloc_vec_size for the init list */
        static size_t compute_pre_alloc(uint32 target_chunk_size_byte)
        {
            size_t work_mem_byte = ((size_t)maintenance_work_mem) * 1024;
            return (work_mem_byte * 2 / target_chunk_size_byte);
        }

        static uint32 calculate_pow(uint32 elem_size, uint32 targe_size_mb)
        {
            targe_size_mb = Max(targe_size_mb, 1);
            /* size_t arithmetic so 16 GB * 1 MB doesn't wrap in uint32:
            * 16384 * 1024 * 1024 = 2^34 overflows uint32 silently. */
            size_t target_size = (size_t)targe_size_mb * 1024ULL * 1024ULL;
            double target_count = static_cast<double>(target_size) / elem_size;
            double x_double = log2(target_count);
            int x = static_cast<int>(floor(x_double));
            return Max(x, 0);
        }

        static char *allocate_aligned_memory(size_t alloc_bytes, MemoryContext ctx)
        {
            constexpr size_t extra = ann_helper::vector_aligned_size + sizeof(void *);
            char *original_ptr = (char *)MemoryContextAllocHuge(ctx, alloc_bytes + extra);
            uint64 raw_addr = (uint64)original_ptr + sizeof(void *);
            uint64 aligned_addr = (raw_addr + ann_helper::vector_aligned_size - 1) & ~(ann_helper::vector_aligned_size - 1);
            return reinterpret_cast<char *>(aligned_addr);
        }
    public:
        MemPool(uint32 store_esize, bool need_align, size_t target_chunk_size_mb,
                MemoryContext build_ctx, bool is_shared)
            : elem_size(need_align ? get_align_elem_size(store_esize) : store_esize),
              pow_elem_nums_per_chunk(calculate_pow(elem_size, Max(target_chunk_size_mb, 1u))),
              one_chunk_elem_nums(1 << pow_elem_nums_per_chunk),
              chunk_size(elem_size * one_chunk_elem_nums),
              pre_alloc_vec_size(compute_pre_alloc(chunk_size)),
              vec(pre_alloc_vec_size),
              locks(pre_alloc_vec_size),
              ctx(AllocSetContextCreate(build_ctx, "mempool context", ALLOCSET_DEFAULT_SIZES))
        {
            /* 显式传 ctx 分配, 不用 MemoryContextSwitchTo 改全局 CurrentMemoryContext —
             * 并行 build 时多 worker 在 extend() 并发 SwitchTo 全局会 race, AllocHuge 串到
             * 错误 ctx → 析构 MemoryContextDelete bad-free。 */
            char *aligned_ptr = allocate_aligned_memory(chunk_size, ctx);
            memset(aligned_ptr, 0xFF, chunk_size); /* fill with INVALID_VECTOR_ID */
            vec.push_back(aligned_ptr);
            locks.emplace_back(one_chunk_elem_nums);
        }

        const void *get(size_t idx) const
        {
            uint32 chunk_no = get_chunk_no(idx);
            uint32 chunk_offset = get_chunk_offset(idx);
            return &vec[chunk_no].buf[chunk_offset];
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
            size_t vec_size = vec.size();
            if (chunk_no >= vec_size) { /* need append */
                Assert(chunk_no < pre_alloc_vec_size);
                mutex.lock();
                vec_size = vec.size();
                for (uint32 i = vec_size; i < chunk_no + 1; ++i) {
                    /* 显式传 ctx, 不 SwitchTo 全局 CurrentMemoryContext: 多 worker 并发
                     * extend 时 SwitchTo 全局 race → AllocHuge 串到错误 ctx → bad-free。 */
                    char *aligned_ptr = NULL;
                    PG_TRY(); {
                        aligned_ptr = allocate_aligned_memory(chunk_size, ctx);
                    }
                    PG_CATCH(); {
                        mutex.unlock();
                        PG_RE_THROW();
                    }
                    PG_END_TRY();
                    memset(aligned_ptr, 0xFF, chunk_size); /* fill with INVALID_VECTOR_ID */
                    /*
                     * Both vec and locks use manual push_back with a store barrier between
                     * construct and _end publication.  Vector::push_back / emplace_back does
                     * construct(_end++, val) which on aarch64's weak memory model can make the
                     * _end increment visible to other threads before the element is fully
                     * constructed.  A concurrent reader in extend() (vec) or lock_elem() (locks)
                     * may then see the new chunk slot but read a stale buf (nullptr) / a not-yet
                     * constructed RWBitLock, causing a segfault in memcpy or heap corruption.
                     *
                     * Fixed by: construct → release fence → ++_end, so the element is globally
                     * visible before _end is published.  Both vec AND locks must do this — locks
                     * was previously emplace_back'd without the barrier (parallel build heap
                     * corruption under pw=64 on aarch64).  The barrier only fires when
                     * allocating a new chunk, so the cost is negligible.
                     */
                    {
                        size_t lsize = locks.size();
                        locks.expand_to(lsize + 1);
                        auto &lock_end = locks.get_end();
                        locks.get_allocator().construct(lock_end, one_chunk_elem_nums);
                        std::atomic_thread_fence(std::memory_order_release);
                        ++lock_end;
                    }
                    vec.expand_to(vec_size + 1);
                    auto &end = vec.get_end();
                    vec.get_allocator().construct(end, (Chunk)aligned_ptr);
                    std::atomic_thread_fence(std::memory_order_release);
                    ++end;
                    vec_size = vec.size();
                }
                mutex.unlock();
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

        uint32 get_one_chunk_elem_nums() const { return one_chunk_elem_nums; }
        uint32 get_elem_size() const { return elem_size; }
        Vector<Chunk> *get_vec() { return &vec; }

        /* ctx contains most of memory, delete it to free memory for quantizer code flush */
        void destroy() { MemoryContextDelete(ctx); }
    };

    /* 
     * `vector_pool`: since id is unique and vec_size is determined,
     *  we can directly get the vector of a point by calculate the offset from `vector_pool`.

     * `basepoint_pool`: id is unique and neighbors size is determined, same as `vector_pool`
     * 
     * `upperpoint_pool`: can not sure the num of upperpoint, have to get a upperpoint neighbor
     *  by `cur_layer_idx`, which is assigned when insert
     */
    Holder<MemPool> vector_pool;
    Holder<MemPool> basepoint_pool;
    Holder<MemPool> upperpoint_pool;

    GraphIndexEntryInfo entry_info;
    static constexpr size_t bitlock_size = 1'000'000'000ul; /* need 125MB, enough for memory build now... */
    static constexpr bool use_dist_cache = false;

    MemStore(uint_fast16_t dim, uint_fast16_t m, uint_fast32_t vec_size, MemoryContext ctx, bool is_shared)
        : dim(dim),
          m(m),
          vec_size(vec_size),
          num_vectors(0),
          num_uppers(0),
          elems_lock(bitlock_size)
    {
        entry_info.set(INVALID_VECTOR_ID, INVALID_VECTOR_ID, -1);

        /* init mempool */
        size_t basepoint_size = get_base_point_size(m);
        size_t upperpoint_size = get_upper_point_size(m);
        double nlayer = 1.0 / (m - 1);
        size_t upperpoint_size_factor = upperpoint_size * nlayer;
        size_t factor = vec_size + basepoint_size + upperpoint_size_factor;

        constexpr size_t mempool_init_size_mb = 100;
        size_t vector_chunk_size = mempool_init_size_mb * vec_size / factor;
        size_t base_chunk_size = mempool_init_size_mb * basepoint_size / factor;
        size_t upper_chunk_size = mempool_init_size_mb * upperpoint_size_factor / factor;

        vector_pool.emplace(vec_size, true, vector_chunk_size, ctx, is_shared);
        basepoint_pool.emplace(basepoint_size, false, base_chunk_size, ctx, is_shared);
        upperpoint_pool.emplace(upperpoint_size, false, upper_chunk_size, ctx, is_shared);
    }

    void destroy()
    {
        vector_pool->destroy();
        basepoint_pool->destroy();
        upperpoint_pool->destroy();
        elems.destroy();
        REPORT_PERF(NOTICE);
        PERF_DESTROY();
    }

    template <typename Distancer, typename VecAlloc = DEFAULT_ALLOCATOR<T>>
    void get_distance_batch(const Distancer &d, const char *query, const Vector<T, VecAlloc> &ids, float *dists)
    {
        const uint_fast16_t num = ids.size();
        DO_PERF_COUNT(read, num);
        void *vals[num];
        void **val_cur = vals;
        for (T id : ids) {
            *val_cur = vector_pool->get(id);
            ++val_cur;
        }
        STOP_PERF(read);
        DO_PERF_COUNT(calc, num);
        d.get_distance_batch2(query, vals, dim, num, dists);
        STOP_PERF(calc);
    }

    template <typename Distancer>
    float get_distance(const Distancer &d, const char *query, T id)
    {
        DO_PERF(read);
        const char *val = (const char *)vector_pool->get(id);
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
        const char *query = vector_pool->get(query_id);
        const char *val = vector_pool->get(val_id);
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
            T *neighbors_info = (T *)basepoint_pool->get(id);
            dist = (float *)(neighbors_info + m * 2);
            res = (uint *)(dist + m * 2);
            len = m * 2 + 1;
        } else {
            T *neighbors_info = (T *)upperpoint_pool->get(id);
            dist = (float *)(neighbors_info + upperpoint_size(m));
            res = (uint *)(dist + m);
            len = m + 1;
        }
        STOP_PERF(read);
        return Pair<float *, BitSpan<uint>>(dist, res, len, 0);
    }

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
        T *dest = (T *)upperpoint_pool->extend(cur_layer_idx);
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
        T *dest = (T *)basepoint_pool->extend(id);
        memcpy(dest, neighbors_id, sizeof(T) * m * 2);
        float *temp = (float *)(dest + m * 2);
        BitSpan<uint> stat((uint *)(temp + m * 2), m * 2 + 1);
        stat.reset(m * 2);
        STOP_PERF(write);
    }

    template <typename Distancer>
    void add_vector(Distancer &d, T id, const char *query)
    {
        DO_PERF(write);
        vector_pool->set(id, (void *)query);
        STOP_PERF(write);
    }

    template <bool is_base_layer, typename VecAlloc = DEFAULT_ALLOCATOR<GraphIndexCandidate<T>>>
    void get_neighbors(Vector<GraphIndexCandidate<T>, VecAlloc> &neighbors, const GraphIndexCandidate<T> &point)
    {
        DO_PERF(read);
        CONSTEXPR_IF (is_base_layer) {
            T *neighbors_id = (T *)basepoint_pool->get(point.id);
            float *dist = (float *)(neighbors_id + m * 2);
            for (uint_fast16_t i = 0; i < m * 2; ++i) {
                T id = neighbors_id[i];
                if (!is_valid(id)) {
                    break;
                }
                neighbors.emplace_back(id, id, dist[i]);
            }
        } else {
            T *neighbors_info = (T *)upperpoint_pool->get(point.cur_layer_idx);
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
            T *neighbors_id = (T *)basepoint_pool->get(cur_layer_idx);
            STOP_PERF(read);
            return {neighbors_id, INVALID_VECTOR_ID, INVALID_VECTOR_ID};
        }
        T *neighbors_info = (T *)upperpoint_pool->get(cur_layer_idx);
        STOP_PERF(read);
        return {neighbors_info, neighbors_info[m * 2], INVALID_VECTOR_ID};
    }

    template <bool is_base_layer>
    void set_neighbor(T cur_layer_idx, uint16 update_nbr_idx, T newpoint_id, T newpoint_cur_layer_idx)
    {
        DO_PERF(write);
        CONSTEXPR_IF (is_base_layer) {
            T *neighbors_id = (T *)basepoint_pool->get(cur_layer_idx);
            neighbors_id[update_nbr_idx] = newpoint_id;
        } else {
            T *neighbors_info = (T *)upperpoint_pool->get(cur_layer_idx);
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

        entry_waitlock.lock();
        entry_waitlock.unlock();
        /* Get entry point */
        entry_lock.lock_shared();
        GraphIndexEntryInfo entry = entry_info;
        if ((!force_share_flag && unlikely(insert_level > entry.level)) ||
            unlikely(entry.level < 0)) {
            entry_lock.unlock_shared();

            entry_waitlock.lock();
            entry_lock.lock();
            entry_waitlock.unlock();
            /* we don't re-examine whether level has been changed, even if so,
             * we want to ensure first several points to connect sequentially */

            /* Get latest entry point after lock is acquired */
            shared = false;
            entry = entry_info;
        }
        STOP_PERF(lock);
        return {entry, shared};
    }

    void release_entry_lock(bool shared) { if (shared) entry_lock.unlock_shared(); else entry_lock.unlock(); }

    void set_entrypoint(size_t id, size_t cur_layer_idx, int_fast8_t level)
    {
        DO_PERF(write);
        entry_info.set(id, cur_layer_idx, level);
        STOP_PERF(write);
    }

    void add_elem(PointExtensionContext &ctx, T id, Span<const point_type_data> data)
    {
        DO_PERF(write);
        DO_PERF(lock);
        elems_veclock.lock();
        STOP_PERF(lock);
        elems.expand_size(id + 1);
        new (elems.at(id)) point_type(ctx, data);
        elems_veclock.unlock();
        STOP_PERF(write);
    }

    template <typename F>
    auto apply_elem(T id, F &&f)
    {
        DO_PERF(write);
        DO_PERF(lock);
        elems_veclock.lock_shared();
        STOP_PERF(lock);
        elems_lock.lock(id);
        auto res = f(elems[id]);
        elems_lock.unlock(id);
        elems_veclock.unlock_shared();
        STOP_PERF(write);
        return res;
    }

    template <bool is_base_layer, bool shared_lock>
    void lock_point(T cur_layer_idx)
    {
        DO_PERF(lock);
        CONSTEXPR_IF (is_base_layer) {
            (*basepoint_pool).template lock_elem<shared_lock>(cur_layer_idx);
        } else {
            (*upperpoint_pool).template lock_elem<shared_lock>(cur_layer_idx);
        }
        STOP_PERF(lock);
    }

    template <bool is_base_layer, bool shared_lock>
    void unlock_point(T cur_layer_idx)
    {
        CONSTEXPR_IF (is_base_layer) {
            (*basepoint_pool).template unlock_elem<shared_lock>(cur_layer_idx);
        } else {
            (*upperpoint_pool).template unlock_elem<shared_lock>(cur_layer_idx);
        }
    }

    char *get_data(T id) { return (char *)vector_pool->get(id); }
    struct my_buf {
        const char *c;
        my_buf(const char *c) : c(c) {}
        char *get_vecbuf() const { return (char *)c; }
        static constexpr void release() {}
    };
    my_buf read_data(T id) { return my_buf{get_data(id)}; }

    void flush_points(Relation index, BlockNumber meta_blkno)
    {
        disk_container::DiskVector<point_type> dv{index, meta_blkno, false};
        dv.push_back_n(elems.data(), elems.size());
        dv.destroy();
    }

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

    static bool has_stat(BitSpan<uint> stat) { return stat[stat.size() - 1]; }
    static void set_stat(BitSpan<uint> stat) { stat.set(stat.size() - 1); }
    T get_vector_num() const { return num_vectors; }
    T get_upper_num() const { return num_uppers; }
    uint_fast16_t get_m() const { return m; }
    uint_fast16_t get_dim() const { return dim; }
    uint_fast32_t get_vecsize() const { return vec_size; }
    uint_fast32_t get_elemsize() const { return vec_size; }
    void update_data(T id, const char *data)
    {
        char *dest = get_data(id);
        memcpy(dest, data, vec_size);
    }

    /* no used */
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
    std::shared_mutex entry_lock;
    std::shared_mutex entry_waitlock;
    Vector<point_type, HUGE_ALLOCATOR<point_type>> elems;
    std::shared_mutex elems_veclock; /* used for `elems` realloc */
    BitLock elems_lock; /* every elem has one lock, used when find duplicate point */
    static bool is_valid(T id) { return likely(id != (T)INVALID_VECTOR_ID); }
    static constexpr size_t upperpoint_size(uint_fast16_t m) { return (m + 1) * 2; }

    static uint32 get_upper_point_size_factor(uint_fast16_t m)
    {
        double nlayer = 1.0 / (m - 1);
        return get_upper_point_size(m) * nlayer;
    }
};

template <typename IdType, typename elem_type = GraphIndexPoint>
class DiskStore : public PERFER(DiskPerfCats) {
    using PerfCats = DiskPerfCats;
    using PlainStore = disk_container::PlainStore;
    using AccessorLockType = disk_container::AccessorLockType;
    static constexpr BlockNumber metablkno = GRAPH_INDEX_METAPAGE_BLKNO;
public:
    using T = IdType;
    using point_type = elem_type;
    using point_type_data = typename elem_type::Data;
    static constexpr bool use_dist_cache = true;

    DiskStore(Relation index, Relation heap, Buffer metabuf, bool need_wal,
              bool parallel_build = false)
        : index(index),
          heap(heap),
          metap(GRAPH_INDEX_PAGE_GET_META(BufferGetPage(metabuf))),
          metabuf(metabuf),
          need_wal(need_wal),
          parallel_build(parallel_build),
          m(metap->m),
          dim(metap->dimension),
          precision_type(metap->precision_type),
          qt_type(metap->quantizer_metainfo.get_type()),
          vec_size(dim * VEC_ELEM_SIZE(metap->precision_type)),
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
        if (metap->use_cluster()) {
            Assert(BlockNumberIsValid(metap->cluster_block));
            statbuf = ReadBuffer(index, metap->cluster_block);
            statp = BufferGetPage(statbuf);
        }
    }

    void refresh_quantizer_state()
    {
        QuantizerType new_qt_type = metap->quantizer_metainfo.get_type();
        if (new_qt_type != qt_type) {
            qt_type = new_qt_type;
            if (qt_type == QuantizerType::PQ) {
                elem_size = metap->quantizer_metainfo.get_pq_metainfo().code_size();
            } else if (qt_type == QuantizerType::RABITQ) {
                elem_size = metap->quantizer_metainfo.get_rabitq_meta().quant_size;
            } else {
                elem_size = vec_size;
            }
        }
    }

    void destroy()
    {
        if (BufferIsValid(statbuf)) {
            ReleaseBuffer(statbuf);
        }
        base_layer.destroy();
        upper_layer.destroy();
        elems.destroy();
        neighbors_val_pool.destroy();
        pfree(point_info_buf);
        REPORT_PERF(NOTICE);
        PERF_DESTROY();
    }

    VectorScratch read_data(T id)
    {
        DO_PERF(read_vec);
        VectorScratch vec_buf = read_vector(id);
        STOP_PERF(read_vec);
        return vec_buf;
    }

    template <typename Distancer, typename VecAlloc = DEFAULT_ALLOCATOR<T>>
    void get_distance_batch(const Distancer &d, const char *query, const Vector<T, VecAlloc> &ids, float *dists)
    {
        const uint_fast16_t num = ids.size();
        if (num == 0) {
            return;
        }

        const size_t stride = get_aligned_vec_size(elem_size);
        char *values = nullptr;
        void *value_ptrs[num];
        Buffer pinned_buffers[num];
        for (uint_fast16_t i = 0; i < num; ++i) {
            pinned_buffers[i] = InvalidBuffer;
        }

        PG_TRY();
        {
            DO_PERF_COUNT(read_vec, num);
            for (uint_fast16_t i = 0; i < num; ++i) {
                const char *page_value = nullptr;
                refresh_vector_nblocks(ids[i]);
                const VectorReadStatus status = vector_storage_pin(
                    index, (off_t)((size_t)ids[i] * elem_size), elem_size,
                    vector_nblocks, &pinned_buffers[i],
                    &page_value);
                if (status == VECTOR_READ_OK && page_value != nullptr &&
                        is_aligned(page_value)) {
                    value_ptrs[i] = const_cast<char *>(page_value);
                } else {
                    vector_storage_unpin(pinned_buffers[i]);
                    pinned_buffers[i] = InvalidBuffer;
                    if (status != VECTOR_READ_COPY_REQUIRED &&
                            status != VECTOR_READ_OK) {
                        ereport(ERROR,
                                (errcode(ERRCODE_DATA_CORRUPTED),
                                 errmsg("could not read vector %u from index \"%s\"",
                                        (unsigned)ids[i], RelationGetRelationName(index)),
                                 errdetail("vector page read status: %d", (int)status),
                                 errhint("REINDEX the vexdb_graph index.")));
                    }
                    if (values == nullptr) {
                        values = alloc_vector(stride, num);
                    }
                    char *value = values + i * stride;
                    read_vector_into(ids[i], value);
                    value_ptrs[i] = value;
                }
            }
            STOP_PERF(read_vec);

            DO_PERF_COUNT(calc, num);
            d.get_distance_batch2(query, value_ptrs, dim, num, dists);
            STOP_PERF(calc);
            for (uint_fast16_t i = 0; i < num; ++i) {
                vector_storage_unpin(pinned_buffers[i]);
                pinned_buffers[i] = InvalidBuffer;
            }
        }
        PG_CATCH();
        {
            for (uint_fast16_t i = 0; i < num; ++i) {
                vector_storage_unpin(pinned_buffers[i]);
            }
            if (values != nullptr) {
                free_vector(values);
            }
            PG_RE_THROW();
        }
        PG_END_TRY();

        if (values != nullptr) {
            free_vector(values);
        }
    }

    template <typename Distancer>
    float get_distance(const Distancer &d, const char *query, T id)
    {
        float dist = 0;
        DO_PERF(read_vec);
        VectorScratch vec_buf = read_vector(id);
        char *val = vec_buf.get_vecbuf();
        STOP_PERF(read_vec);
        DO_PERF(calc);
        dist = d.get_distance_single((void *)query, (void *)val, dim);
        STOP_PERF(calc);
        vec_buf.release();
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
        DO_PERF(read_vec);
        VectorScratch vec_buf = read_vector(id);
        char *val = vec_buf.get_vecbuf();
        STOP_PERF(read_vec);
        DO_PERF(calc);
        dist = d.get_distance_est_single((void *)query, (void *)val, dim);
        STOP_PERF(calc);
        vec_buf.release();
        return dist;
    }

    template <bool is_base_layer>
    T assign_vector_id()
    {
        DO_PERF(write_node);
#if defined(PG_VEXDB_TARGET_PG)
        /*
         * One short lock protects all graph DiskVector metadata updates for
         * this index.  DiskVector::append() nests reserve() under its existing
         * heavyweight page lock; putting a non-reentrant LWLock directly in
         * that path would self-deadlock.  The outer storage lock keeps ID and
         * capacity publication serialized without covering node writes or
         * graph search.
         */
        LWLock *storage_lock = parallel_build ? vex_graph_build_storage_lock(index) : NULL;
        vex_graph_build_lock_acquire(storage_lock, LW_EXCLUSIVE);
#endif
        GIStateInput input;
        graph_index_get_state(index, GIStateOper::GET_UNDER_VACUUM, input);
        bool can_reuse = !input.bool_val.val;
        T id = (T)INVALID_VECTOR_ID;
        CONSTEXPR_IF (is_base_layer) {
            if (can_reuse) {
                disk_container::FreeSpace<T> freespace{index, metap->free_id_list_block, need_wal};
                can_reuse = freespace.pop(id);
                freespace.destroy();
            }
            if (!can_reuse) {
                id = base_layer.append();
                LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
                const size_t new_count = size_t(id) + 1;
                if (metap->num_vectors < new_count) {
                    metap->num_vectors = new_count;
                    if (need_wal) {
                        xlog.update_num_vector(metap->num_vectors);
                    }
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
#if defined(PG_VEXDB_TARGET_PG)
        vex_graph_build_lock_release(storage_lock);
#endif
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

    template <typename Distancer>
    void add_vector(Distancer &d, T id, const char *query)
    {
        DO_PERF(write_vec);
        const char *data = query;
        char *code = NULL;
        if (qt_type != QuantizerType::NONE) {
            code = (char *)palloc(elem_size);
            if (precision_type == DistPrecisionType::FLOAT) {
                d.compute_code((float *)query, code);
            } else {
                Assert(false);
                float *half2float = alloc_floatvector(dim);
                // halfs_to_floats((half *)query, half2float, dim);
                d.compute_code(half2float, code);
                free_vector(half2float);
            }
            data = code;
        }
        vector_storage_write(index, elem_size * id, elem_size, data, need_wal);
        if (code) {
            pfree(code);
        }
        STOP_PERF(write_vec);
    }

    template <bool is_base_layer, typename VecAlloc = DEFAULT_ALLOCATOR<GraphIndexCandidate<T>>>
    void get_neighbors(Vector<GraphIndexCandidate<T>, VecAlloc> &neighbors, const GraphIndexCandidate<T> &point)
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
            /* 
             * Write neighbor ID and cur_layer_idx in a single locked operation.
             *
             * Previously these were two separate apply() calls with independent WriteLocks to
             * reduce xlog record, but creating a window where a concurrent search could read a
             * valid neighbor ID but an INVALID cur_layer_idx (the old value from the empty slot).
             * This caused cur_point.cur_layer_idx to become INVALID_VECTOR_ID in search_layer
             * under concurrent insert+search.
             * 
             * Currently it have to write extra xlog record, actually need sizeof(T) * 2, but we
             * write the whole upperpoint with (m + 1) * sizeof(T). It's OK since the number of 
             * upperpoint is small compared with basepoint.
             */
            const auto set_point = [&](GraphIndexDiskUpperPoint<T> *upperpoint) -> bool {
                upperpoint->neighbors_info[update_nbr_idx] = newpoint_id;
                upperpoint->neighbors_info[m + update_nbr_idx] = newpoint_cur_layer_idx;
                return true;
            };
            upper_layer.template apply<AccessorLockType::WriteLock>(set_point)(cur_layer_idx);
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
#if defined(PG_VEXDB_TARGET_PG)
            LWLock *entry_lock = parallel_build ? vex_graph_build_entry_lock(index) : NULL;
            LWLock *entry_wait_lock = parallel_build ? vex_graph_build_entry_wait_lock(index) : NULL;
            /* Stop new shared holders from passing an exclusive upgrader. */
            vex_graph_build_lock_acquire(entry_wait_lock, LW_EXCLUSIVE);
            vex_graph_build_lock_release(entry_wait_lock);
            vex_graph_build_lock_acquire(entry_lock, LW_SHARED);
#endif
            /* wait vacuum to operate atomically */
            LockPage(index, metablkno, ShareLock);
            if ((!force_share_flag && unlikely(insert_level > metap->entry_level)) ||
                unlikely(metap->entry_level < 0)) {
                UnlockPage(index, metablkno, ShareLock);
#if defined(PG_VEXDB_TARGET_PG)
                vex_graph_build_lock_release(entry_lock);
                vex_graph_build_lock_acquire(entry_wait_lock, LW_EXCLUSIVE);
                vex_graph_build_lock_acquire(entry_lock, LW_EXCLUSIVE);
                vex_graph_build_lock_release(entry_wait_lock);
#endif
                LockPage(index, metablkno, ExclusiveLock);
                shared = false;
#if defined(PG_VEXDB_TARGET_PG)
                if (parallel_build) {
                    /*
                     * Several workers can observe the same old entry level
                     * before any of them obtains the exclusive lock.  Only
                     * the first one still needs exclusivity; keeping every
                     * stale upgrader exclusive serializes several complete
                     * high-level insertions.  Entry level is monotonic during
                     * CREATE INDEX, so after the first worker publishes the
                     * requested level it is safe to join the normal shared
                     * insertion path using the new entry point.
                     */
                    LockBuffer(metabuf, BUFFER_LOCK_SHARE);
                    bool upgrade_still_needed =
                        ((!force_share_flag && insert_level > metap->entry_level) ||
                         metap->entry_level < 0);
                    LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
                    if (!upgrade_still_needed) {
                        UnlockPage(index, metablkno, ExclusiveLock);
                        vex_graph_build_lock_release(entry_lock);
                        vex_graph_build_lock_acquire(entry_wait_lock, LW_EXCLUSIVE);
                        vex_graph_build_lock_release(entry_wait_lock);
                        vex_graph_build_lock_acquire(entry_lock, LW_SHARED);
                        LockPage(index, metablkno, ShareLock);
                        shared = true;
                    }
                }
#endif
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
#if defined(PG_VEXDB_TARGET_PG)
        if (parallel_build) {
            vex_graph_build_lock_release(vex_graph_build_entry_lock(index));
        }
#endif
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
        if (need_wal) {
            xlog.update_entry(entry_info);
        }
        LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
    }

    void add_elem(PointExtensionContext &ctx, T id, Span<const point_type_data> d, bool is_async = false)
    {
        DO_PERF(write_node);
#if defined(PG_VEXDB_TARGET_PG)
        LWLock *storage_lock = parallel_build ? vex_graph_build_storage_lock(index) : NULL;
        vex_graph_build_lock_acquire(storage_lock, LW_EXCLUSIVE);
#endif
        elems.extend(id + 1);
#if defined(PG_VEXDB_TARGET_PG)
        vex_graph_build_lock_release(storage_lock);
#endif
        point_type elem(ctx, d, is_async);
        elems.template set<AccessorLockType::WriteLock>(id, elem);
        STOP_PERF(write_node);
    }

    void set_async_point_status(T id, bool is_async)
    {
        elems.template apply<AccessorLockType::WriteLock>([&](point_type *elem) -> bool {
            elem->set_async(is_async);
            return true;
        }, 1)(id);
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
        neighbors_val_pool->reset();
    }

    char *get_data(T id)
    {
        DO_PERF(read_vec);
        if (!neighbors_val_pool.has_value()) {
            neighbors_val_pool.emplace(elem_size, metap->ef_construction);
        }
        VectorScratch vec_buf = read_vector(id);
        char *val = vec_buf.get_vecbuf();
        char *res = neighbors_val_pool->set(val);
        vec_buf.release();
        STOP_PERF(read_vec);
        return res;
    }

    void fetch_vec_via_slot(HeapTuple tuple, char *vec)
    {
        /* Function-expression indexes carry attnum == 0 and resolve through
         * rd_indexprs; plain column indexes have no FuncExpr so func_oid
         * stays Invalid and the name-dispatch block below must be skipped. */
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

        Datum d = value;
        if (OidIsValid(func_oid)) {
            char *func_name = get_func_name(func_oid);
            if (func_name != NULL) {
                if (strcmp(func_name, "array_to_floatvector") == 0) {
                    d = DirectFunctionCall2(array_to_floatvector, value, Int32GetDatum(dim));
                } else if (strcmp(func_name, "subfloatvector") == 0) {
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
                    d = DirectFunctionCall3(subfloatvector, value, arg2, arg3);
                }
            }
        }

        FloatVector *data = DatumGetFloatVector(d);
        memcpy(vec, data->x, vec_size);

        if (PointerGetDatum(data) != value) {
            pfree(data);
        }
    }

    bool fetch_vec_from_heap(ItemPointerData tid, char *dest)
    {
        DO_PERF(fetch);
        char tuple_buf[BLCKSZ] = {0};
        HeapTuple tuple = (HeapTupleData *)tuple_buf;
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
        char tuple_buf[BLCKSZ] = {0};
        HeapTuple tuple = (HeapTupleData *)tuple_buf;
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
        size_t id = 0;
        char *data_buf;
        const auto collect_deleted_tids = [&](point_type *elem) -> bool {
#if PG_VERSION_NUM >= 180000
            vacuum_delay_point(false);
#else
            vacuum_delay_point();
#endif
            bool is_dirty = false;
            uint32 num_removed = elem->vacuum_tids([&](const ItemPointerData &tid) -> bool {
                return callback((ItemPointer)(&tid), callback_state);
            }, ctx, is_dirty);
            if (elem->empty() && !elem->is_deleted() && !elem->is_async()) {
                deleted.emplace(id);
            }
            stats->tuples_removed += num_removed;
            ++id;
            return is_dirty;
        };
        elems.template apply<AccessorLockType::WriteLock>(collect_deleted_tids)(0, remove_elem_num);
        return remove_elem_num;
    }

    void mark_deleted(size_t basepoint_num, size_t upperpoint_num)
    {
        /* Pass 1: Mark elements as deleted based on their own state, collect recyclable IDs */
        size_t id = INVALID_VECTOR_ID;
        Vector<T> recycled_ids;
        UnorderedSet<T> skip_clear_ids;
        const auto mark_elem = [&](point_type *elem) -> bool {
            /*
             * skip:
             * 1. live elements
             * 2. already-deleted elements, marked by previous vacuum but not been reused
             */
            ++id;
            if (!elem->empty() || elem->is_deleted()) {
                return false;
            }
            elem->set_deleted();
            Assert(need_wal);
            recycled_ids.push_back((T)id);
            if (elem->is_async()) {
                skip_clear_ids.emplace((T)id);
            }
            return true;
        };
        elems.template apply<AccessorLockType::WriteLock>(mark_elem, 1)(0, basepoint_num);

        if (recycled_ids.empty()) {
            recycled_ids.destroy();
            return;
        }

        /* Pass 2: Clear base layer neighbors for newly-deleted elements and record clear id for upper layer */
        UnorderedSet<T> clear_set(recycled_ids.size()); /* for upper layer repair */
        for (size_t i = 0; i < recycled_ids.size(); ++i) {
            T id = recycled_ids[i];
            if (skip_clear_ids.contains(id)) {
                continue;
            }
            clear_set.emplace(id);
            base_layer.template apply<AccessorLockType::WriteLock>(
                [&](GraphIndexDiskBasePoint<T> *basepoint) -> bool {
                    basepoint->init(m);
                    return true;
                }
            )(id);
        }

        Vector<T> recycled_upper_idx;
        size_t upper_idx = INVALID_VECTOR_ID;
        upper_layer.template apply<AccessorLockType::WriteLock>(
            [&](GraphIndexDiskUpperPoint<T> *upperpoint) -> bool {
                ++upper_idx;
                if (!clear_set.contains(upperpoint->id)) {
                    return false;
                }
                upperpoint->init(m);
                recycled_upper_idx.push_back((T)upper_idx);
                return true;
            }
        )(0, upperpoint_num);
        ann_helper::optional_destroy(clear_set);

        /* Recycle deleted element IDs into free list for reuse */
        disk_container::FreeSpace<T> freespace{index, metap->free_id_list_block, need_wal};
        freespace.insert(recycled_ids.data(), recycled_ids.size());
        freespace.destroy();
        recycled_ids.destroy();

        /* Recycle deleted upper point indices into upper free list for reuse */
        disk_container::FreeSpace<T> upper_freespace{index, metap->free_upper_list_block, need_wal};
        upper_freespace.insert(recycled_upper_idx.data(), recycled_upper_idx.size());
        upper_freespace.destroy();
        recycled_upper_idx.destroy();
    }

    void inspect(IndexInspectResult &res)
    {
        res.append_attr("Neighbor Degree (m)");
        res.fill_content("%hu", m);

        /* TD: account on freelist */
        res.append_attr("Base Container Used Size");
        res.fill_content((base_layer.get_nblocks() + elems.get_nblocks() + 0) * BLCKSZ);
        res.append_attr("Base Container Required Size");
        res.fill_content(base_layer.size() * (neighbors_size() + sizeof(point_type)));
        res.append_attr("Base Container Number of Entries");
        res.fill_content("%lu", base_layer.size() - 0);
        res.append_attr("Base Container Reserved Number of Entries");
        res.fill_content("%lu", base_layer.capacity() - base_layer.size() + 0);

        res.append_attr("Upper Layer Neighbor Container Used Size");
        res.fill_content((upper_layer.get_nblocks() + 0) * BLCKSZ);
        res.append_attr("Upper Layer Neighbor Container Required Size");
        res.fill_content(upper_layer.size() * upperpoint_size());
        res.append_attr("Upper Layer Neighbor Number of Entries");
        res.fill_content("%lu", upper_layer.size() - 0);
        res.append_attr("Upper Layer Neighbor Reserved Number of Entries");
        res.fill_content("%lu", upper_layer.capacity() - upper_layer.size() + 0);
    }

    Relation get_index() const { return index; }
    Relation get_heap() const { return heap; }
    T get_vector_num() const { return base_layer.size(); }
    T get_upper_num() const { return upper_layer.size(); }
    uint_fast16_t get_m() const { return m; }
    uint_fast16_t get_dim() const { return dim; }
    uint_fast32_t get_vecsize() const { return vec_size; }
    uint_fast32_t get_elemsize() const { return elem_size; }
    uint_fast32_t get_cluster_rate() const { return metap->cluster_rate; }
    DistPrecisionType get_precision() const { return metap->precision_type; }

    /*
     * During a parallel build, readers take a shared point lock across copying
     * and scoring one neighbor list, while writers take the exclusive form
     * across the complete read/prune/write sequence.  Runtime scans keep their
     * existing buffer-lock-only fast path, so this build fix does not add one
     * named LWLock acquisition per visited node to query execution.
     */
    template <bool is_base_layer, bool shared_lock>
    void lock_point(T id)
    {
#if defined(PG_VEXDB_TARGET_PG)
        if (parallel_build) {
            LWLock *point_lock = vex_graph_build_point_lock(index, (uint64)id, is_base_layer);
            vex_graph_build_lock_acquire(point_lock, shared_lock ? LW_SHARED : LW_EXCLUSIVE);
        }
#else
        (void)id;
#endif
    }

    template <bool is_base_layer, bool shared_lock>
    void unlock_point(T id)
    {
#if defined(PG_VEXDB_TARGET_PG)
        if (parallel_build) {
            vex_graph_build_lock_release(
                vex_graph_build_point_lock(index, (uint64)id, is_base_layer));
        }
#else
        (void)id;
#endif
    }
    template <bool is_base_layer>
    static constexpr Pair<float *, BitSpan<uint>> get_neighbor_stats(T id)
        { return Pair<float *, BitSpan<uint>>(nullptr, nullptr, 0, 0); }
    static constexpr bool has_stat(BitSpan<uint> stat) { return false; }
    static constexpr void set_stat(BitSpan<uint> stat) {}
    void lock_stats() const {}
    void unlock_stats() const {}
    template <typename F>
    void apply_stats_meta_wal(F &&f) const {}
private:
    class NeighborsValPool {
    /*
     * this pool is used in `apply_arrangement` to check duplicate and `select_neighbors` to store
     * nbrs' data temporarily. It should not bigger than `ef_construction` + 2.
     */
    public:
        NeighborsValPool(size_t elem_size_val, uint_fast16_t ef_construction)
            : elem_size(get_aligned_vec_size(elem_size_val)),
#ifdef USE_ASSERT_CHECKING
              pool_size(ef_construction + 2),
#endif
              idx(0),
              pool(alloc_vector(elem_size, ef_construction + 2)) {}
        char *set(char *val)
        {
            Assert(idx < pool_size);
            char *dest = pool + idx * elem_size;
            memcpy(dest, val, elem_size);
            ++idx;
            return dest;
        }
        void reset() { idx = 0; }
        void destroy() { free_vector(pool); }
    private:
        uint32 elem_size;
#ifdef USE_ASSERT_CHECKING
        uint32 pool_size;
#endif
        uint32 idx;
        char *pool;
    };

    Relation index;
    Relation heap;
    GraphIndexMetaPage metap;
    Page statp;
    Buffer metabuf;
    Buffer statbuf{InvalidBuffer};

    bool need_wal;
    bool parallel_build;
    uint_fast16_t m;
    uint_fast16_t dim;
    DistPrecisionType precision_type;
    QuantizerType qt_type;
    uint_fast32_t vec_size;
    uint_fast32_t elem_size;
    BlockNumber vector_nblocks{InvalidBlockNumber};
    GraphIndexXlog xlog;
    GraphIndexEntryInfo entry_info;
public:
    disk_container::DiskVector<point_type> elems;
private:
    Optional<NeighborsValPool> neighbors_val_pool; /* used in algorithm:select_neighbors() */
    T *point_info_buf;

    VectorScratch read_vector(T id)
    {
        refresh_vector_nblocks(id);
        return read_vector_scratch(index, id, elem_size, vector_nblocks);
    }

    void read_vector_into(T id, char *buffer)
    {
        refresh_vector_nblocks(id);
        ::read_vector_into(index, id, elem_size, buffer, vector_nblocks);
    }

    void refresh_vector_nblocks(T id)
    {
        const uint64 vector_end = ((uint64)id + 1) * elem_size;
        const BlockNumber required_block =
            (BlockNumber)((vector_end - 1) / VEX_VECTOR_PAGE_PAYLOAD_SIZE);

        /*
         * RelationGetNumberOfBlocksInFork() reaches smgrnblocks() and may issue
         * an lseek.  A graph scan reads hundreds of vectors from the same fork,
         * so keep the observed size in this query/build-local DiskStore.  If a
         * concurrent extension (or this build) needs a newer page, refresh only
         * at that boundary.
         */
        if (!BlockNumberIsValid(vector_nblocks) ||
            required_block >= vector_nblocks) {
            RelationGetSmgr(index);
            vector_nblocks =
                RelationGetNumberOfBlocksInFork(index, VECTOR_FORKNUM);
        }
    }

    size_t neighbors_size() const { return base_layer.data_size(); }
    size_t upperpoint_size() const { return upper_layer.data_size(); }
    static bool is_valid(T id) { return likely(id != (T)INVALID_VECTOR_ID); }
public:
    disk_container::VarDiskVector<GraphIndexDiskBasePoint<T>> base_layer;
    disk_container::VarDiskVector<GraphIndexDiskUpperPoint<T>> upper_layer;
};

using DiskStoreVariant = Variant<
    DiskStore<uint32>,
    DiskStore<size_t>
>;

inline void create_disk_store(DiskStoreVariant &var, Relation index, Relation heap, Buffer metabuf,
                              bool need_wal, bool parallel_build = false)
{
    if (need_wal) {
        need_wal = RelationNeedsWAL(index);
    }
    const auto metap = GRAPH_INDEX_PAGE_GET_META(BufferGetPage(metabuf));
#if defined(PG_VEXDB_TARGET_PG)
    if (metap->version != GRAPH_INDEX_VERSION) {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("unsupported vexdb_graph index format version %u",
                        metap->version),
                 errhint("REINDEX the vexdb_graph index.")));
    }
    if (heap != NULL && heap->rd_rel->relispartition &&
        metap->quantizer_metainfo.get_type() != QuantizerType::NONE) {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("quantized vexdb_graph indexes are not supported on partitioned tables"),
                 errhint("Use a plain vexdb_graph index for every leaf partition.")));
    }
#endif
    if (metap->use_cluster()) {
        Assert(metap->id_type == IdType::U32);
        Assert(false);
    } else if (metap->id_type == IdType::U32) {
        var.template emplace<DiskStore<uint32>>(index, heap, metabuf, need_wal, parallel_build);
    } else {
        var.template emplace<DiskStore<size_t>>(index, heap, metabuf, need_wal, parallel_build);
    }
}

#endif /* GRAPH_INDEX_STORAGE_INTERFACE_H */
