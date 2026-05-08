/**
 * Copyright ...
 * Vector buffer manager structures
 * Copied from openGauss src/include/access/annvector/store/vector_smgr.h
 */

#ifndef VECTOR_BUFFER_MANAGER_H
#define VECTOR_BUFFER_MANAGER_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#ifndef BOOST_ASSERT_IS_VOID
#define BOOST_ASSERT_IS_VOID
#endif
#ifndef BOOST_UNORDERED_DISABLE_REENTRANCY_CHECK
#define BOOST_UNORDERED_DISABLE_REENTRANCY_CHECK
#endif
#ifndef BOOST_NO_EXCEPTIONS
#define BOOST_NO_EXCEPTIONS
#endif

#ifdef snprintf
#define PG_VEXDB_RESTORE_SNPRINTF
#undef snprintf
#endif
#ifdef vsnprintf
#define PG_VEXDB_RESTORE_VSNPRINTF
#undef vsnprintf
#endif

#include <boost/unordered/concurrent_flat_map.hpp>
#include <boost/lockfree/queue.hpp>

#ifdef PG_VEXDB_RESTORE_SNPRINTF
#define snprintf pg_snprintf
#undef PG_VEXDB_RESTORE_SNPRINTF
#endif
#ifdef PG_VEXDB_RESTORE_VSNPRINTF
#define vsnprintf pg_vsnprintf
#undef PG_VEXDB_RESTORE_VSNPRINTF
#endif

#include <vtl/hashtable>
#include <vtl/holder>
#include <vtl/shared_allocator>

#ifdef __cplusplus
extern "C" {
#endif
#include "c.h"
#include "storage/smgr.h"
#include "storage/lwlock.h"
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#include "utils/timestamp.h"
}
#endif

#include "knl/knl_variable.h"
#include "module/parallel_counter.h"
#include "distance/core/distance.h"
#include "vecbuf_shared.h"

#define RELAXED_ORDER std::memory_order_relaxed
#define RELEASE_ORDER std::memory_order_release
#define ACQUIRE_ORDER std::memory_order_acquire

/* Forward declarations */
struct VecBufferPool;
struct VecBufferManager;

/* Constants */
constexpr size_t max_file_size = RELSEG_SIZE * BLCKSZ;
constexpr uint32 vec_block_size_bits = 20u;
constexpr uint32 vec_block_size = 1u << vec_block_size_bits;
constexpr uint32 min_cached_dim = 8u;
constexpr size_t MIN_ASYNC_IO_BATCH_NUM = 4;
constexpr int16 DISKANN_MAX_DIM = 2000;

/* Note: vector_aligned_size is defined in distance.h */

#define NVecBuf(alloc) (uint32(alloc) >> 10)

enum class VecBufIOState : uint8 {
    IO_READ,
    IO_READY_SUC,
    IO_READY_FAILED
};

enum class AsyncVecBufState : uint8 {
    CACHE_MISS,
    CACHE_HIT_READY,
    CACHE_HIT_ACTIVE_READ,
    CACHE_HIT_PASSIVE_READ
};

struct __attribute__((packed)) BufferSignature {
    Oid rel_id;
    size_t offset;
    bool operator==(const BufferSignature &rhs) const noexcept
        { return offset == rhs.offset && rel_id == rhs.rel_id; }
    struct Hasher {
        size_t operator()(const BufferSignature &sig) const noexcept
        {
            return std::hash<Oid>()(sig.rel_id) + std::hash<uint32>()(uint32(sig.offset));
        }
    };
};
static_assert(sizeof(BufferSignature) == 12ul);

struct VecBufferLoc {
    uint32 buf_offset;
    uint32 offset;
    static constexpr uint32 invalid_mask = 0x80000000u;
    static constexpr uint32 empty_mask = 0x40000000u;

    VecBufferLoc() : buf_offset(0), offset(0) {}
    VecBufferLoc(uint32 bo, uint32 off) : buf_offset(bo), offset(off) {}
    VecBufferLoc(struct BufferParams &params);
    bool operator==(const VecBufferLoc &rhs) const
    {
        return buf_offset == rhs.buf_offset && offset == rhs.offset;
    }
    bool operator<(const VecBufferLoc &rhs) const
    {
        if (buf_offset != rhs.buf_offset) return buf_offset < rhs.buf_offset;
        return offset < rhs.offset;
    }
    bool valid() const { return !(buf_offset & invalid_mask); }
    void set_invalid() { buf_offset |= invalid_mask; }
    void set_valid() { buf_offset &= ~invalid_mask; }
    bool empty() const { return buf_offset == 0 && offset == 0; }
    void set_empty() { buf_offset = 0; offset = 0; }
    uint32 valid_buf_offset() const { return buf_offset & ~invalid_mask; }
};

struct VecBufferTag {
    std::atomic<VecBufIOState> io_state;
    std::atomic<uint32> ref_count{0};
    BufferSignature sig;

    bool io_ready() const
    {
        const VecBufIOState state = io_state.load(RELAXED_ORDER);
        return state == VecBufIOState::IO_READY_SUC || state == VecBufIOState::IO_READY_FAILED;
    }

    bool io_failed() const { return io_state.load(RELAXED_ORDER) == VecBufIOState::IO_READY_FAILED; }

    bool io_fly() const { return io_state.load(RELAXED_ORDER) == VecBufIOState::IO_READ; }
};

struct BufferPoolStats {
    slock_t lock;
    uint32 nblock{0u};
    uint32 ndata{0u};
    uint32 *blocks{NULL};
    TimestampTz first_evict_time{0};
    std::atomic<size_t> nevict{0};
    uint32 get_rand_block()
    {
        Assert(nblock > 0);
        return blocks[random() % nblock];
    }
#if VERIFY_BUFFER
    void verify_loc(const VecBufferLoc &loc) const
    {
        uint32 start_idx = nblock - 1u;
        for (int i = (int)start_idx; i >= 0; --i) {
            if (loc.buf_offset == blocks[i]) {
                return;
            }
        }
        for (uint32 i = start_idx; i < nblock; ++i) {
            if (loc.buf_offset == blocks[i]) {
                return;
            }
        }
        elog(PANIC, "got loc (%u,%u) that does not in corresponding block", loc.buf_offset, loc.offset);
    }
#else
    FORCE_INLINE static void verify_loc(const VecBufferLoc &loc) {}
#endif
};

template<>
struct std::hash<VecBufferLoc> {
    size_t operator()(const VecBufferLoc& loc) const noexcept
        { return std::hash<uint64>()((uint64(loc.buf_offset) << 32) | loc.offset); }
};

constexpr uint32 vec_block_float_size_bits = vec_block_size_bits - 2u;
constexpr uint32 cqueue_capacity = vec_block_size / sizeof(float) / min_cached_dim * 1.5;
constexpr uint32 cqueue_edge = 8u * 4u;
/* vector_step_size is defined as macro (16) in distance.h */
constexpr int16 NVecPool = (int16(DISKANN_MAX_DIM) + 16 - 1) / 16 + 1;
constexpr long eviction_time_interval = 10l;

/* Forward declaration */
struct VecBuffer;

/* Global shutdown flag */
extern bool vector_shutdown_requested;

/* Helper function for aligned dimension */
inline uint32 get_aligned_dim(uint32 dim)
{
    return (dim + ann_helper::vector_aligned_size / sizeof(float) - 1) & ~(ann_helper::vector_aligned_size / sizeof(float) - 1);
}

using bufmap_ctx = vtl::SharedCtxAllocator<std::pair<const BufferSignature, VecBufferLoc>>;
using bufmap = boost::unordered::concurrent_flat_map<
    BufferSignature, VecBufferLoc,
    BufferSignature::Hasher, std::equal_to<BufferSignature>, bufmap_ctx>;
using cqueue_ctx = vtl::SharedCtxAllocator<VecBufferLoc>;
using cqueue = boost::lockfree::queue<VecBufferLoc,
    boost::lockfree::allocator<cqueue_ctx>,
    boost::lockfree::capacity<cqueue_capacity + cqueue_edge>>;

struct VecBufferPool {
    BufferPoolStats stats{};
    uint32 freezing_block{0u};
    std::atomic<uint32> nfreeze{0u};
    std::atomic<uint32> nfreed{0u};
    MemoryContext ctx;
    Holder<cqueue> freelist{};
    Holder<bufmap> locmap{};
    bool accepting_block{false};
    ann_helper::ParaCounter hit;
    ann_helper::ParaCounter miss;

    explicit VecBufferPool(MemoryContext in_ctx);
    void destroy();
    void wait_freelist_freeze();
    void wait_locmap_freeze(uint32 pool_max_offset);
    bool wait_freeze(uint32 block, uint32 pool_max_offset)
    {
        if (freezing_block != 0u) {
            Assert(freezing_block != block + 1u);
            return false;
        }
        freezing_block = block + 1u;
        do {
            wait_freelist_freeze();
            wait_locmap_freeze(pool_max_offset);
        } while (nfreed.load(ACQUIRE_ORDER) < pool_max_offset);
        return true;
    }
    void assign_block(uint32 block, uint32 max_offset)
    {
        for (uint32 i = 0; i < max_offset; ++i) {
            VecBufferLoc loc(block, i);
            push_freelist(loc);
        }
    }

    bool pop_freelist(VecBufferLoc &loc)
    {
        while (freelist->pop(loc)) {
            nfreeze.fetch_sub(1u, RELAXED_ORDER);
            if (loc.buf_offset != freezing_block - 1u) {
                return true;
            }
            nfreed.fetch_add(1u, RELAXED_ORDER);
        }
        return false;
    }
    bool try_push_freelist(const VecBufferLoc &loc)
    {
        bool res = freelist->push(loc);
        if (res) {
            nfreeze.fetch_add(1u, RELAXED_ORDER);
        }
        return res;
    }
    void push_freelist(const VecBufferLoc &loc)
    {
        nfreeze.fetch_add(1u, RELAXED_ORDER);
        bool res = freelist->push(loc);
        if (unlikely(!res)) {
            do {
                if (vector_shutdown_requested) {
                    return;
                }
                SPIN_DELAY();
                res = freelist->push(loc);
            } while (!res);
        }
    }
    bool need_evict()
    {
        uint32 nf = nfreeze.load(ACQUIRE_ORDER);
        if (nf >= cqueue_capacity - cqueue_edge || accepting_block) {
            return false;
        }
        return nf < stats.ndata * 0.004 + 2u && stats.nblock > (freezing_block == 0 ? 0 : 1);
    }
    FORCE_INLINE void verify_loc(const VecBufferLoc &loc) const { stats.verify_loc(loc); }
};

struct VecBufferManager {
    float *buf{NULL};
    slock_t *tag_locks;
    VecBufferTag **tag{NULL};
    VecBufferPool *pool[NVecPool];
    uint32 nalloced{0};
    bool buffer_inited{false};
    VecBufferManager()
    {
        for (size_t i = 0; i < NVecPool; ++i) {
            pool[i] = NULL;
        }
    }
    void init_buffer();
    static int16 get_pool_offset(uint32 dim)
    {
        Assert(dim >= min_cached_dim);
        return (dim + vector_step_size - 1) / vector_step_size;
    }
    static uint32 get_pool_max_offset(int16 pool_offset)
    {
        return vec_block_size /
            (pool_offset * vector_step_size) /
            sizeof(float);
    }
    char *get_vector(uint32 block, uint32 offset, uint32 dim)
    {
        return (char *)(buf + (uint64(block) << vec_block_float_size_bits) +
                     offset * get_aligned_dim(dim));
    }
    void try_init_pool(int16 pool_offset)
    {
        if (!pool[pool_offset]) {
            MemoryContext ctx = vecbuf_shared_ctx;
            void *pool_mem = MemoryContextAlloc(ctx, sizeof(VecBufferPool));
            VecBufferPool *temp = new (pool_mem) VecBufferPool(ctx);
            pg_memory_barrier();
            pool[pool_offset] = temp;
        }
    }
    void append_block(int16 pool_offset, uint32 block)
    {
        try_init_pool(pool_offset);
        VecBufferPool &cur_pool = *pool[pool_offset];
        const uint32 max_offset = get_pool_max_offset(pool_offset);
        Assert(tag[block] == NULL);
        tag[block] = (VecBufferTag *)MemoryContextAllocZero(
            vecbuf_shared_ctx, max_offset * sizeof(VecBufferTag));
        cur_pool.stats.blocks[cur_pool.stats.nblock] = block;
        pg_memory_barrier();
        ++cur_pool.stats.nblock;
        cur_pool.assign_block(block, max_offset);
        cur_pool.stats.ndata += max_offset;
    }
    void remove_block(int16 pool_offset, uint32 block);
    bool do_evict(int16 pool_offset);
    void expand_or_recollect_space(int16 pool_offset, bool &evicted);
    void try_redistribute_block();
    bool find_eviction_min_max_freq_offset(int16 &min_offset, int16 &max_offset);
    void redistribute_block(int16 src_pool_offset, int16 dest_pool_offset);
    void async_expand_or_recollect_space(int16 pool_offset);
    Pair<AsyncVecBufState, VecBuffer> async_alloc_cache_slot(Relation rel, size_t loc, uint32 elem_size, VecStorageType st);
    VecBuffer get_buffer(Relation rel, size_t loc, uint32 elem_size, VecStorageType st, bool &success);
};

extern bool vector_shutdown_requested;

inline uint32 get_effective_dim(const size_t elem_size)
{
    return (elem_size + 3u) / 4u;
}

inline bool dim_cached(const size_t elem_size)
{
    return get_effective_dim(elem_size) >= min_cached_dim;
}

#endif /* VECTOR_BUFFER_MANAGER_H */
