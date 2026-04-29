/**
 * Copyright ...
 * read write implementation for vector forknum
 * Copied from openGauss src/gausskernel/storage/access/annvector/module/vector_smgr.cpp
 * Adapted for PostgreSQL: uses VECTOR_FORKNUM (VISIBILITYMAP_FORKNUM) for vector storage.
 * We use VM fork because this index doesn't need visibility map,
 * so we can reuse it for vector storage without additional mechanisms.
 */

#include "pg_compat.h"

#include <algorithm>
#include <atomic>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <cfloat>

#ifndef BOOST_ASSERT_IS_VOID
#define BOOST_ASSERT_IS_VOID
#endif
#ifndef BOOST_UNORDERED_DISABLE_REENTRANCY_CHECK
#define BOOST_UNORDERED_DISABLE_REENTRANCY_CHECK
#endif
#ifndef BOOST_NO_EXCEPTIONS
#define BOOST_NO_EXCEPTIONS
#endif
#include <boost/unordered/concurrent_flat_map.hpp>
#include <boost/lockfree/queue.hpp>

#include <vtl/pair>
#include <vtl/hashtable>
#include <vtl/holder>
#include <vtl/vector>

#include "vector_smgr.h"
#include "vector_buffer_manager.h"
#include "distance/core/distance.h"
#include "module/parallel_counter.h"
#include "module/size_format.h"
#include "knl/knl_variable.h"
#include "vecbuf_shared.h"

extern "C" {
#include "funcapi.h"
}

using namespace ann_helper;

bool vector_shutdown_requested = false;

/*
 * MdfdVec - same layout as _MdfdVec in md.c
 * Since _MdfdVec is private to md.c, we define our own with matching layout
 * and cast the pointers from SMgrRelationData.md_seg_fds.
 */
typedef struct MdfdVec
{
    File        mdfd_vfd;       /* fd number in fd.c's pool */
    BlockNumber mdfd_segno;     /* segment number, from 0 */
} MdfdVec;

/* Behavior flags for vec_getseg - copied from md.c */
#define VEC_EXTENSION_FAIL             (1 << 0)
#define VEC_EXTENSION_RETURN_NULL      (1 << 1)
#define VEC_EXTENSION_CREATE           (1 << 2)

/* Access the segment array with proper casting */
#define VEC_SEG_ARRAY(reln) ((MdfdVec *)(reln)->md_seg_fds[VECTOR_FORKNUM])

/*
 * Get segment path for visibility map fork
 */
static RelPathStr
vec_segment_path(SMgrRelation reln, BlockNumber segno)
{
    return relpath(reln->smgr_rlocator, VECTOR_FORKNUM);
}

/*
 * Open a segment file for the visibility map fork.
 * Returns the file descriptor, or -1 on failure.
 */
static File
vec_openseg(SMgrRelation reln, BlockNumber segno, int oflags)
{
    File fd;
    RelPathStr path;
    char fullpath[REL_PATH_STR_MAXLEN + 1 + OIDCHARS + 1];
    
    path = relpath(reln->smgr_rlocator, VECTOR_FORKNUM);
    
    if (segno > 0)
        snprintf(fullpath, sizeof(fullpath), "%s.%u", path.str, segno);
    else
        strlcpy(fullpath, path.str, sizeof(fullpath));
    
    fd = PathNameOpenFile(fullpath, O_RDWR | PG_BINARY | oflags);
    
    return fd;
}

/*
 * Get the segment containing the specified block.
 * This is a simplified version of _mdfd_getseg() from md.c.
 * 
 * For reads: use VEC_EXTENSION_RETURN_NULL to return NULL if segment doesn't exist
 * For writes: use VEC_EXTENSION_CREATE to create segments as needed
 */
static MdfdVec *
vec_getseg(SMgrRelation reln, BlockNumber blkno, int behavior)
{
    BlockNumber targetseg = blkno / RELSEG_SIZE;
    MdfdVec *segarray;
    
    /* If segment is already open, return it */
    if (targetseg < (BlockNumber)reln->md_num_open_segs[VECTOR_FORKNUM]) {
        segarray = VEC_SEG_ARRAY(reln);
        return &segarray[targetseg];
    }
    
    /* Need to open segment(s) */
    if (behavior & VEC_EXTENSION_RETURN_NULL) {
        /* For reads - just try to open the specific segment */
        File fd = vec_openseg(reln, targetseg, 0);
        if (fd < 0)
            return NULL;
        
        /* Allocate space for segment array */
        if (reln->md_num_open_segs[VECTOR_FORKNUM] == 0) {
            reln->md_seg_fds[VECTOR_FORKNUM] = 
                (struct _MdfdVec *)MemoryContextAlloc(TopMemoryContext, 
                                                       sizeof(MdfdVec) * (targetseg + 1));
        } else if (targetseg >= (BlockNumber)reln->md_num_open_segs[VECTOR_FORKNUM]) {
            reln->md_seg_fds[VECTOR_FORKNUM] = 
                (struct _MdfdVec *)repalloc(reln->md_seg_fds[VECTOR_FORKNUM],
                                             sizeof(MdfdVec) * (targetseg + 1));
        }
        
        segarray = VEC_SEG_ARRAY(reln);
        
        /* Fill in all segments up to target */
        for (BlockNumber i = reln->md_num_open_segs[VECTOR_FORKNUM]; i < targetseg; i++) {
            File prev_fd = vec_openseg(reln, i, 0);
            segarray[i].mdfd_vfd = prev_fd;
            segarray[i].mdfd_segno = i;
        }
        
        segarray[targetseg].mdfd_vfd = fd;
        segarray[targetseg].mdfd_segno = targetseg;
        reln->md_num_open_segs[VECTOR_FORKNUM] = targetseg + 1;
        
        return &segarray[targetseg];
    }
    
    if (behavior & VEC_EXTENSION_CREATE) {
        /* For writes - create segments as needed */
        for (BlockNumber segno = reln->md_num_open_segs[VECTOR_FORKNUM]; 
             segno <= targetseg; segno++) {
            int oflags = (segno == targetseg) ? O_CREAT : 0;
            File fd = vec_openseg(reln, segno, oflags);
            
            if (fd < 0 && segno == targetseg) {
                /* Try again with O_CREAT for target segment */
                fd = vec_openseg(reln, segno, O_CREAT);
                if (fd < 0)
                    return NULL;
            }
            
            /* Allocate space if needed */
            if (reln->md_num_open_segs[VECTOR_FORKNUM] == 0) {
                reln->md_seg_fds[VECTOR_FORKNUM] = 
                    (struct _MdfdVec *)MemoryContextAlloc(TopMemoryContext, 
                                                           sizeof(MdfdVec) * (targetseg + 1));
            } else if (segno >= (BlockNumber)reln->md_num_open_segs[VECTOR_FORKNUM]) {
                reln->md_seg_fds[VECTOR_FORKNUM] = 
                    (struct _MdfdVec *)repalloc(reln->md_seg_fds[VECTOR_FORKNUM],
                                                 sizeof(MdfdVec) * (targetseg + 1));
            }
            
            segarray = VEC_SEG_ARRAY(reln);
            segarray[segno].mdfd_vfd = fd;
            segarray[segno].mdfd_segno = segno;
            reln->md_num_open_segs[VECTOR_FORKNUM] = segno + 1;
        }
        
        segarray = VEC_SEG_ARRAY(reln);
        return &segarray[targetseg];
    }
    
    return NULL;
}

/* Global VecBufferManager pointer - defined in vector_smgr.cpp */
VecBufferManager *VecBufMgr = nullptr;

#undef NVecBuf
#define NVecBuf (uint32(g_instance.diskann_cxt.vector_buffers) >> 10)

/* MAX_RANDOM_VALUE - PostgreSQL random() returns long in range [0, 2^31-1] */
#ifndef MAX_RANDOM_VALUE
#define MAX_RANDOM_VALUE (0x7FFFFFFF)
#endif

/* VectorBufferLock is declared extern in vecbuf_shared.h */

constexpr static bool verify_file_content(const char *content)
{
    return true;
}

static void report_read_vector_error(SMGR_READ_STATUS status, Relation rel, size_t loc)
{
    if (status == SMGR_RD_NO_BLOCK) {
        ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                        errmsg("could not read vector at loc %lu in relation \"%s\"",
                               loc, RelationGetRelationName(rel))));
    } else if (status == SMGR_RD_CRC_ERROR) {
        ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                        errmsg("CRC error reading vector at loc %lu in relation \"%s\"",
                               loc, RelationGetRelationName(rel))));
    }
}

/* VecBufferPool constructor */
VecBufferPool::VecBufferPool(MemoryContext in_ctx) : ctx(in_ctx)
{
    freelist.emplace(cqueue_ctx(ctx));
    constexpr uint32 init_size = cqueue_capacity * 2u;
    locmap.emplace(init_size, bufmap_ctx(ctx));
    SpinLockInit(&stats.lock);
    stats.blocks = (uint32 *)MemoryContextAllocZero(ctx, NVecBuf * sizeof(uint32));
}

/* VecBufferPool methods */
void VecBufferPool::destroy()
{
    Assert(stats.nblock == 0);
    freelist->~cqueue();
    locmap->~bufmap();
    pfree(stats.blocks);
    MemoryContextDelete(ctx);
}

void VecBufferPool::wait_freelist_freeze()
{
    Assert(freezing_block > 0);
    uint32 target_block = freezing_block - 1;
    const uint32 total = nfreeze.load(RELAXED_ORDER) + cqueue_edge;
    UnorderedSet<VecBufferLoc> seen(total);
    uint32 count = 0;
    VecBufferLoc loc;
    while (freelist->pop(loc)) {
        if (seen.contains(loc)) {
            for (uint32 k = 0; !freelist->push(loc); ++k) {
                pg_yield(k);
            }
            break;
        }
        if (loc.buf_offset != target_block) {
            seen.insert(loc);
            for (uint32 k = 0; !freelist->push(loc); ++k) {
                pg_yield(k);
            }
        } else {
            nfreed.fetch_add(1u, RELAXED_ORDER);
            nfreeze.fetch_sub(1u, RELAXED_ORDER);
        }
        if (++count >= total) {
            break;
        }
    }
    ann_helper::optional_destroy(seen);
}

/* VecBufferManager methods */
void VecBufferManager::init_buffer()
{
    Assert(!buffer_inited);
    auto old_ctx = MemoryContextSwitchTo(vecbuf_shared_ctx);
    const size_t nvecbuf = NVecBuf;
    tag_locks = (slock_t *)palloc(nvecbuf * sizeof(slock_t));
    tag = (VecBufferTag **)palloc0(nvecbuf * sizeof(VecBufferTag *));
    Size buf_size = nvecbuf * vec_block_size + ann_helper::vector_aligned_size;
    void *temp = palloc(buf_size);
    if (!temp || !tag ||
        !std::align(ann_helper::vector_aligned_size, nvecbuf * vec_block_size,
                    temp, buf_size)) {
        ereport(PANIC, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("Vector shared buffer init failed")));
    }
    buf = (float *)temp;
    MemoryContextSwitchTo(old_ctx);
    buffer_inited = true;
}

void VecBufferManager::remove_block(int16 pool_offset, uint32 block)
{
    Assert(pool[pool_offset]);
    VecBufferPool &cur_pool = *pool[pool_offset];
    Assert(cur_pool.freezing_block == block + 1u);
    Assert(cur_pool.stats.nblock > 0);
    SpinLockAcquire(&cur_pool.stats.lock);
    uint32 *pos = std::find(cur_pool.stats.blocks, cur_pool.stats.blocks + cur_pool.stats.nblock, block);
    Assert(pos != cur_pool.stats.blocks + cur_pool.stats.nblock);
    *pos = cur_pool.stats.blocks[cur_pool.stats.nblock - 1u];
    --cur_pool.stats.nblock;
    cur_pool.stats.ndata -= get_pool_max_offset(pool_offset);
    SpinLockRelease(&cur_pool.stats.lock);
    SpinLockAcquire(tag_locks + block);
    pfree(tag[block]);
    SpinLockRelease(tag_locks + block);
}

bool VecBufferManager::do_evict(int16 pool_offset)
{
    if (!pool[pool_offset]) {
        return false;
    }
    auto &cur_pool = *pool[pool_offset];
    if (cur_pool.accepting_block || cur_pool.stats.nblock == 0) {
        return false;
    }
    SpinLockAcquire(&cur_pool.stats.lock);
    uint32 block = cur_pool.stats.get_rand_block();
    if (block == cur_pool.freezing_block - 1u) {
        SpinLockRelease(&cur_pool.stats.lock);
        return false;
    }
    SpinLockAcquire(tag_locks + block);
    SpinLockRelease(&cur_pool.stats.lock);
    if (!tag[block]) {
        SpinLockRelease(tag_locks + block);
        return false;
    }
    const uint32 max_offset = get_pool_max_offset(pool_offset);
    uint32 offset = random() % max_offset;
    bool can_push = true;
    for (uint32 count = 0; count < max_offset; ++count) {
        Assert(tag[block]);
        BufferSignature &sig = tag[block][offset].sig;
        if (!OidIsValid(sig.rel_id)) {
            offset = (offset + 1) % max_offset;
            continue;
        }
        if (cur_pool.locmap->erase_if(sig, [&](auto &x) -> bool {
            if (x.second.empty()) {
                can_push = false;
                return true;
            }
            if (!x.second.valid()) {
                sig.rel_id = InvalidOid;
                if (x.second.valid_buf_offset() == cur_pool.freezing_block - 1u) {
                    can_push = false;
                    cur_pool.nfreed.fetch_add(1u, RELAXED_ORDER);
                    return true;
                }
                VecBufferLoc loc{x.second.valid_buf_offset(), x.second.offset};
                can_push = cur_pool.try_push_freelist(loc);
                return can_push;
            }
            const VecBufferTag &t = tag[x.second.buf_offset][x.second.offset];
            if (t.ref_count.load(ACQUIRE_ORDER) > 0) {
                return false;
            }
            sig.rel_id = InvalidOid;
            if (x.second.buf_offset == cur_pool.freezing_block - 1u) {
                can_push = false;
                cur_pool.nfreed.fetch_add(1u, RELAXED_ORDER);
                return true;
            }
            cur_pool.verify_loc(x.second);
            can_push = cur_pool.try_push_freelist(x.second);
            return can_push;
        }) > 0) {
            SpinLockRelease(tag_locks + block);
            return can_push;
        }
        if (!can_push) {
            SpinLockRelease(tag_locks + block);
            return false;
        }
        offset = (offset + 1) % max_offset;
    }
    SpinLockRelease(tag_locks + block);
    return false;
}

void VecBufferManager::expand_or_recollect_space(int16 pool_offset, bool &evicted)
{
    const bool zero_buf = pool[pool_offset] == NULL;
    evicted = false;
    if (nalloced < NVecBuf || zero_buf) {
        if (!LWLockConditionalAcquire(VectorBufferLock, LW_EXCLUSIVE)) {
            if (zero_buf) {
                constexpr long delay = 5000l;
                pg_usleep(delay);
            }
            return;
        }
        if (!buffer_inited) {
            init_buffer();
        }
        if (nalloced < NVecBuf) {
            SpinLockInit(tag_locks + nalloced);
            append_block(pool_offset, nalloced);
            ++nalloced;
        } else if (zero_buf) {
            try_init_pool(pool_offset);
            int16 src_pool_offset, unused;
            find_eviction_min_max_freq_offset(src_pool_offset, unused);
            if (src_pool_offset != -1 && src_pool_offset != pool_offset) {
                redistribute_block(src_pool_offset, pool_offset);
                pool[pool_offset]->stats.first_evict_time = GetCurrentTimestamp();
            }
        }
        LWLockRelease(VectorBufferLock);
        return;
    }
    if (!zero_buf) {
        evicted = do_evict(pool_offset);
    }
}

void VecBufferManager::try_redistribute_block()
{
    if (nalloced < NVecBuf) {
        return;
    }
    if (!LWLockConditionalAcquire(VectorBufferLock, LW_EXCLUSIVE)) {
        return;
    }
    int16 src_pool_offset, dest_pool_offset;
    if (find_eviction_min_max_freq_offset(src_pool_offset, dest_pool_offset)) {
        redistribute_block(src_pool_offset, dest_pool_offset);
    }
    LWLockRelease(VectorBufferLock);
}

bool VecBufferManager::find_eviction_min_max_freq_offset(int16 &min_offset, int16 &max_offset)
{
    bool do_redist = false;
    min_offset = -1;
    max_offset = -1;
    double min_score = DBL_MAX;
    double max_score = 0;
    for (int16 i = 0; i < NVecPool; ++i) {
        if (!pool[i]) {
            continue;
        }
        auto &cur_pool = *pool[i];
        if (cur_pool.stats.nblock == 0) {
            continue;
        }
        if (cur_pool.freezing_block != 0) {
            continue;
        }
        long sec;
        int usec;
        TimestampTz cur_time = GetCurrentTimestamp();
        TimestampDifference(cur_pool.stats.first_evict_time, cur_time, &sec, &usec);
        if (sec <= eviction_time_interval + 1l) {
            do_redist = true;
        } else if (cur_pool.stats.nblock > 2u && sec > 2 * eviction_time_interval) {
            min_offset = i;
            min_score = -1.0;
            continue;
        }
        double freq = cur_pool.stats.nevict.load(RELAXED_ORDER) / std::max(1e-5, sec + (double)usec / 1e6);
        double score = freq / std::pow(cur_pool.stats.nblock, 0.75);
        if (score > max_score) {
            max_score = score;
            max_offset = i;
        }
        constexpr long min_sec_to_empty = 30l * 60l + 997l;
        if (score < min_score && (cur_pool.stats.nblock > 2u || sec > min_sec_to_empty)) {
            min_score = score;
            min_offset = i;
        }
    }
    constexpr double min_score_threshold = 0.25;
    return do_redist && min_offset != -1 && max_offset != -1 &&
        min_score * (1 + min_score_threshold) < max_score;
}

void VecBufferManager::redistribute_block(int16 src_pool_offset, int16 dest_pool_offset)
{
    auto &src_pool = *pool[src_pool_offset];
    auto &dest_pool = *pool[dest_pool_offset];
    Assert(!dest_pool.accepting_block);
    START_CRIT_SECTION();
    dest_pool.accepting_block = true;
    pg_read_barrier();
    if (dest_pool.nfreeze + get_pool_max_offset(dest_pool_offset) <= cqueue_capacity) {
        uint32 block = src_pool.stats.get_rand_block();
        if (src_pool.wait_freeze(block, get_pool_max_offset(src_pool_offset))) {
            remove_block(src_pool_offset, block);
            append_block(dest_pool_offset, block);
            if (src_pool.stats.nblock > 0) {
                src_pool.freezing_block = 0;
                src_pool.nfreed = 0;
            } else {
                VecBufferPool *cur_pool = pool[src_pool_offset];
                pool[src_pool_offset] = NULL;
                constexpr long sleep_interval = 32768l;
                pg_usleep(sleep_interval);
                cur_pool->destroy();
            }
        }
    }
    dest_pool.accepting_block = false;
    END_CRIT_SECTION();
}

void VecBufferManager::async_expand_or_recollect_space(int16 pool_offset)
{
    if (!vecbuf_shared_state)
        return;
    
    pg_atomic_write_u32(&vecbuf_shared_state->pool_offset_to_write, pool_offset);
    
    /* Signal the worker if it's running */
    Latch *latch = vecbuf_shared_state->worker_latch;
    if (latch) {
        SetLatch(latch);
    }
}

Pair<AsyncVecBufState, VecBuffer> VecBufferManager::async_alloc_cache_slot(Relation rel, size_t loc, uint32 elem_size, VecStorageType st)
{
    uint32 dim = get_effective_dim(elem_size);
    int16 pool_offset = get_pool_offset(dim);
    if (!pool[pool_offset]) {
        bool evicted = false;
        expand_or_recollect_space(pool_offset, evicted);
        if (!pool[pool_offset]) {
            async_expand_or_recollect_space(pool_offset);
            return { AsyncVecBufState::CACHE_MISS, VecBuffer() };
        }
    }

    VecBufferPool &cur_pool = *pool[pool_offset];
    BufferSignature sig = {rel->rd_smgr->smgr_rlocator.locator.relNumber, loc};
    bool found = false;
    VecBufferLoc existing_loc;
retry:
    cur_pool.locmap->cvisit(sig, [&](const auto &x) {
        if (!x.second.valid()) {
            return;
        }
        existing_loc.buf_offset = x.second.buf_offset;
        existing_loc.offset = x.second.offset;
        found = true;
        tag[x.second.buf_offset][x.second.offset].ref_count.fetch_add(1u, ACQUIRE_ORDER);
    });

    if (found) {
        char *buf = get_vector(existing_loc.buf_offset, existing_loc.offset, dim);
        const AsyncVecBufState state = tag[existing_loc.buf_offset][existing_loc.offset].io_fly() ?
            AsyncVecBufState::CACHE_HIT_PASSIVE_READ : AsyncVecBufState::CACHE_HIT_READY;
        return { state, VecBuffer(pool_offset, existing_loc.buf_offset, existing_loc.offset, buf) };
    }

    VecBufferLoc new_loc;
    if (!cur_pool.pop_freelist(new_loc)) {
        if (cur_pool.nfreed.load(RELAXED_ORDER) <= 1ul) {
            async_expand_or_recollect_space(pool_offset);
        }
        return { AsyncVecBufState::CACHE_MISS, VecBuffer() };
    }

    cur_pool.verify_loc(new_loc);
    VecBufferTag &tag_ref = this->tag[new_loc.buf_offset][new_loc.offset];
    tag_ref.ref_count.fetch_add(1u, ACQUIRE_ORDER);
    tag_ref.sig = sig;
    tag_ref.io_state.store(VecBufIOState::IO_READ, RELAXED_ORDER);
    if (!cur_pool.locmap->try_emplace(sig, new_loc)) {
        tag_ref.ref_count.fetch_sub(1u, RELAXED_ORDER);
        tag_ref.sig.rel_id = InvalidOid;
        cur_pool.push_freelist(VecBufferLoc(new_loc.buf_offset, new_loc.offset));
        goto retry;
    }

    char *buf = get_vector(new_loc.buf_offset, new_loc.offset, dim);
    return { AsyncVecBufState::CACHE_HIT_ACTIVE_READ, VecBuffer(pool_offset, new_loc.buf_offset, new_loc.offset, buf) };
}

VecBuffer VecBufferManager::get_buffer(Relation rel, size_t loc, uint32 elem_size, VecStorageType st, bool &success)
{
    uint32 dim = get_effective_dim(elem_size);
    if (dim < min_cached_dim) {
        success = false;
        return VecBuffer();
    }
    int16 pool_offset = get_pool_offset(dim);
    if (!pool[pool_offset]) {
        bool evicted = false;
        expand_or_recollect_space(pool_offset, evicted);
    }
    if (!pool[pool_offset]) {
        async_expand_or_recollect_space(pool_offset);
        success = false;
        return VecBuffer();
    }
    VecBufferPool &cur_pool = *pool[pool_offset];
    BufferSignature sig = {rel->rd_smgr->smgr_rlocator.locator.relNumber, loc};
    BufferParams params = {rel, loc, elem_size, pool_offset, st};
    uint32 retry_count = 0;
    constexpr uint32 max_retry = 16u;
retry:
    if (cur_pool.locmap->try_emplace_or_cvisit(sig, params,
        [&](const auto &x) {
            Assert(loc == x.first.offset);
            params.buf_offset = x.second.buf_offset;
            params.offset = x.second.offset;
            if (unlikely(!x.second.valid())) {
                cur_pool.miss.inc();
                cur_pool.hit.inc(-1);
                return;
            }
#if VERIFY_BUFFER
            if (tag[x.second.buf_offset][x.second.offset].sig.rel_id != rel->rd_smgr->smgr_rlocator.locator.relNumber) {
                elog(PANIC, "Assert error on accessing (%u,%u), where id is %u which is supposed to be %u",
                            x.second.buf_offset, x.second.offset,
                            tag[x.second.buf_offset][x.second.offset].sig.rel_id,
                            rel->rd_smgr->smgr_rlocator.locator.relNumber);
            }
#endif
            tag[x.second.buf_offset][x.second.offset].ref_count.fetch_add(1u, ACQUIRE_ORDER);
        })) {
        cur_pool.miss.inc();
    } else {
        cur_pool.hit.inc();
    }
    if (unlikely(params.buf_offset & VecBufferLoc::invalid_mask)) {
        cur_pool.locmap->erase_if(sig, [](auto &x) { return x.second.empty(); });
        if (unlikely(params.status != SMGR_RD_OK)) {
            report_read_vector_error(params.status, params.rel, params.loc);
        }
        if (InterruptPending) {
            ProcessInterrupts();
            goto failed;
        }
        ++retry_count;
        if (retry_count > max_retry || !pool[pool_offset]) {
            goto failed;
        }
        if (nalloced >= NVecBuf) {
            if (do_evict(params.pool_offset)) {
                cur_pool.stats.nevict.fetch_add(1ul, RELAXED_ORDER);
            } else if (cur_pool.nfreed.load(RELAXED_ORDER) <= 1ul) {
                goto failed;
            }
        }
        goto retry;
    }
    success = true;
    while (!tag[params.buf_offset][params.offset].io_ready()) {
        pg_usleep(1);
    }
    if (tag[params.buf_offset][params.offset].io_failed()) {
        ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
            errmsg("could not read vector at loc %lu in relation \"%s\"",
               loc, RelationGetRelationName(rel))));
    }
    return VecBuffer((int16)pool_offset, params.buf_offset, params.offset,
        get_vector(params.buf_offset, params.offset, dim));
failed:
    success = false;
    return VecBuffer();
}

/* VecBufferLoc constructor from BufferParams */
VecBufferLoc::VecBufferLoc(BufferParams &params)
{
    constexpr uint32 spins_per_delay = 40u;
    constexpr uint32 max_spins = 3u * spins_per_delay;
    constexpr long min_delay_usec = 512l;
    constexpr long max_delay_usec = 65536l;
    auto &mgr = *VecBufMgr;
    uint32 spins = 0;
    long delay = min_delay_usec;
    Assert(mgr.pool[params.pool_offset]);
    auto &pool = *mgr.pool[params.pool_offset];
    VecBufferLoc loc;
    while (!pool.pop_freelist(loc)) {
        if (spins % spins_per_delay == 0) {
            if (InterruptPending || spins > max_spins) {
                loc.set_empty();
                loc.set_invalid();
                break;
            }
            mgr.async_expand_or_recollect_space(params.pool_offset);
            pg_usleep(delay);
            delay += (delay * ((double) random() / (double) MAX_RANDOM_VALUE) + 0.5);
            if (delay > max_delay_usec) {
                delay = max_delay_usec;
            }
        }
        SPIN_DELAY();
        ++spins;
    }
    buf_offset = params.buf_offset = loc.buf_offset;
    offset = params.offset = loc.offset;
    if (!loc.valid()) {
        return;
    }
    pool.verify_loc(loc);
    VecBufferTag &tag = mgr.tag[loc.buf_offset][loc.offset];
    tag.ref_count.fetch_add(1u, ACQUIRE_ORDER);
    tag.io_state.store(VecBufIOState::IO_READ, RELAXED_ORDER);
    tag.sig = {params.rel->rd_smgr->smgr_rlocator.locator.relNumber, params.loc};
    size_t dim = (params.elem_size + 3) / 4;
    char *temp = mgr.get_vector(loc.buf_offset, loc.offset, dim);
    off_t offset = (off_t)params.loc * params.elem_size;
    params.status = vec_read(params.rel->rd_smgr, offset, params.elem_size, temp, params.storage_type);
    if (unlikely(params.status != SMGR_RD_OK)) {
        if (pool.try_push_freelist(loc)) {
            loc.set_empty();
        }
        loc.set_invalid();
        buf_offset = params.buf_offset = loc.buf_offset;
        offset = params.offset = loc.offset;
        tag.io_state.store(VecBufIOState::IO_READY_FAILED);
    }
    tag.io_state.store(VecBufIOState::IO_READY_SUC);
}

/* VecBufferPool::wait_locmap_freeze */
void VecBufferPool::wait_locmap_freeze(uint32 pool_max_offset)
{
    Assert(freezing_block > 0);
    const auto wait_until_no_ref = [](const VecBufferTag &tag) {
        uint32 count = 0;
        constexpr uint32 delay_per_count = 32u;
        constexpr uint32 max_spin = 8 * delay_per_count;
        constexpr long delay = 768l;
        do {
            ++count;
            if (count % delay_per_count == 0) {
                if (count > max_spin) {
                    return false;
                }
                pg_usleep(delay);
            }
            SPIN_DELAY();
        } while (tag.ref_count != 0);
        return true;
    };

    VecBufferTag *tag_arr = VecBufMgr->tag[freezing_block - 1u];
    size_t nremove = 0;
    for (uint32 i = 0; i < pool_max_offset; ++i) {
        VecBufferTag &tag = tag_arr[i];
        if (!OidIsValid(tag.sig.rel_id)) {
            continue;
        }

        for (bool removed;;) {
            removed = true;
            nremove += locmap->erase_if(tag.sig, [&](auto &x) {
                    while (tag.ref_count != 0 && !wait_until_no_ref(tag)) {
                        removed = false;
                        return false;
                    }
                    tag.sig.rel_id = InvalidOid;
                    return true;
                });
            if (removed) {
                break;
            }
            if (vector_shutdown_requested) {
                break;
            }
        }
    }
    nfreed.fetch_add(nremove, RELAXED_ORDER);
}

/* Initialization */
bool enable_vec_buffer_manager() { return pg_vexdb_is_preloaded() && vecbuf_shared_state && vecbuf_shared_state->enable_buffer_manager; }

void init_vector_smgr()
{
    if (!pg_vexdb_is_preloaded()) {
        return;
    }
    VectorBufferLock = &(GetNamedLWLockTranche("vector_buffer")->lock);
    void *mgr_mem = MemoryContextAlloc(vecbuf_shared_ctx, sizeof(VecBufferManager));
    VecBufMgr = new (mgr_mem) VecBufferManager();
    if (enable_vec_buffer_manager() && !VecBufMgr->buffer_inited) {
        VecBufMgr->init_buffer();
    }
    g_instance.diskann_cxt.vec_buffer_mgr = VecBufMgr;
}

/* Main API: vec_read_buffer */
VecBuffer vec_read_buffer(Relation rel, size_t loc, size_t vec_size, VecStorageType st)
{
    bool success;
    if (unlikely(!rel->rd_smgr)) {
        RelationGetSmgr(rel);
    }
    VecBuffer res = VecBufMgr->get_buffer(rel, loc, vec_size, st, success);
    if (!success) {
        res.pool_offset = -1;
        res.buf = alloc_vector(vec_size);
        read_vec_buf(rel, loc, vec_size, res.buf, st);
    }
    return res;
}

/* Cache invalidation */
void vec_invalidate_buffer_cache(Oid relNode, size_t loc, size_t elem_size)
{
    uint32 dim = get_effective_dim(elem_size);
    if (dim < min_cached_dim) {
        return;
    }
    int16 pool_offset = VecBufMgr->get_pool_offset(dim);
    if (!VecBufMgr->pool[pool_offset]) {
        return;
    }

    VecBufferPool &cur_pool = *VecBufMgr->pool[pool_offset];
    BufferSignature sig = {relNode, loc};
    cur_pool.locmap->erase_if(sig, [&cur_pool](auto &x) {
        if (x.second.empty()) {
            return true;
        }
        auto loc = x.second;
        loc.set_valid();
        if (VecBufMgr->tag[loc.buf_offset][loc.offset].ref_count.load(ACQUIRE_ORDER) == 0) {
            if (loc.buf_offset == cur_pool.freezing_block - 1u) {
                cur_pool.nfreed.fetch_add(1u, RELAXED_ORDER);
                return true;
            }
            if (cur_pool.freelist->push(loc)) {
                cur_pool.nfreeze.fetch_add(1u, RELAXED_ORDER);
                return true;
            }
        }
        x.second.set_invalid();
        return false;
    });
}

void vec_invalidate_buffer_cache(Oid relNode, size_t elem_size)
{
    uint32 dim = get_effective_dim(elem_size);
    if (dim < min_cached_dim) {
        return;
    }
    int16 pool_offset = VecBufMgr->get_pool_offset(dim);
    if (!VecBufMgr->pool[pool_offset]) {
        return;
    }

    VecBufferPool &cur_pool = *VecBufMgr->pool[pool_offset];
    cur_pool.locmap->erase_if([&cur_pool, relNode](auto &x) {
        if (x.second.empty()) {
            return true;
        }
        if (x.first.rel_id != relNode) {
            return false;
        }
        auto loc = x.second;
        loc.set_valid();
        if (VecBufMgr->tag[loc.buf_offset][loc.offset].ref_count.load(ACQUIRE_ORDER) == 0) {
            if (loc.buf_offset == cur_pool.freezing_block - 1u) {
                cur_pool.nfreed.fetch_add(1u, RELAXED_ORDER);
                return true;
            }
            if (cur_pool.freelist->push(loc)) {
                cur_pool.nfreeze.fetch_add(1u, RELAXED_ORDER);
                return true;
            }
        }
        x.second.set_invalid();
        return false;
    });
}

/* VecBuffer methods */
void VecBuffer::release()
{
    if (pool_offset >= 0) {
        release_vector_buffer(loc);
        return;
    }
    Assert(pool_offset == -1);
    free_vector(buf);
}

void VecBuffer::set_io_state(const VecBufIOState state)
{
    VecBufMgr->tag[loc.buf_offset][loc.offset].io_state.store(state, RELAXED_ORDER);
}

bool VecBuffer::get_io_ready()
{
    VecBufferTag &tag_ref = VecBufMgr->tag[loc.buf_offset][loc.offset];
    return tag_ref.io_ready();
}

bool VecBuffer::get_io_failed()
{
    VecBufferTag &tag_ref = VecBufMgr->tag[loc.buf_offset][loc.offset];
    return tag_ref.io_failed();
}

void release_vector_buffer(const VecBufferLoc &loc)
{
    uint32 res = VecBufMgr->tag[loc.buf_offset][loc.offset].ref_count.fetch_sub(1u, RELEASE_ORDER);
    Assert(res > 0);
}

/*
 * vec_read - read vector data directly from file
 * Returns SMGR_RD_OK on success, SMGR_RD_NO_BLOCK if file not found,
 * SMGR_RD_CRC_ERROR on read error.
 */
SMGR_READ_STATUS vec_read(SMgrRelation reln, off_t offset, size_t nbytes, 
                          char *buffer, VecStorageType vec_storage_type)
{
    size_t read_bytes = 0;
    (void)vec_storage_type;  /* unused for now */
    
    while (read_bytes < nbytes) {
        /* Calculate block number and get segment */
        BlockNumber blocknum = offset / BLCKSZ;
        MdfdVec *seg = vec_getseg(reln, blocknum, VEC_EXTENSION_RETURN_NULL);
        if (seg == NULL) {
            return SMGR_RD_NO_BLOCK;
        }
        
        /* Calculate offset within segment */
        off_t seg_offset = (off_t)(blocknum % RELSEG_SIZE) * BLCKSZ + (offset % BLCKSZ);
        off_t seg_max_offset = (off_t)RELSEG_SIZE * BLCKSZ;
        
        /* Calculate how much to read from this segment */
        size_t chunk = nbytes - read_bytes;
        if (seg_offset + chunk > seg_max_offset) {
            chunk = seg_max_offset - seg_offset;
        }
        
        const size_t io_align = PG_IO_ALIGN_SIZE;
        char *dst = buffer + read_bytes;
        size_t remaining = chunk;
        off_t cur_off = seg_offset;

        while (remaining > 0) {
            size_t head = 0;
            size_t mid;
            size_t tail;

            if ((cur_off % io_align) != 0) {
                head = Min(remaining, io_align - (size_t)(cur_off % io_align));
            }

            mid = TYPEALIGN_DOWN(io_align, remaining - head);
            tail = remaining - head - mid;

            if (head > 0) {
                alignas(PG_IO_ALIGN_SIZE) char tmp[BLCKSZ];
                off_t aligned_off = TYPEALIGN_DOWN(io_align, cur_off);
                size_t copy_off = cur_off - aligned_off;
                ssize_t nread = FileRead(seg->mdfd_vfd, tmp, io_align, aligned_off,
                                         WAIT_EVENT_DATA_FILE_READ);

                if (nread < 0)
                    return SMGR_RD_CRC_ERROR;
                if (nread == 0)
                    return SMGR_RD_NO_BLOCK;

                if ((size_t)nread <= copy_off) {
                    memset(dst, 0, head);
                } else {
                    size_t copy_size = Min(head, (size_t)nread - copy_off);
                    memcpy(dst, tmp + copy_off, copy_size);
                    if (copy_size < head)
                        memset(dst + copy_size, 0, head - copy_size);
                }

                dst += head;
                cur_off += head;
                remaining -= head;
                read_bytes += head;
                offset += head;
                continue;
            }

            if (mid > 0) {
                if (((uintptr_t)dst % io_align) == 0) {
                    ssize_t nread = FileRead(seg->mdfd_vfd, dst, mid, cur_off,
                                             WAIT_EVENT_DATA_FILE_READ);

                    if (nread < 0)
                        return SMGR_RD_CRC_ERROR;
                    if (nread == 0)
                        return SMGR_RD_NO_BLOCK;

                    if ((size_t)nread < mid)
                        memset(dst + nread, 0, mid - (size_t)nread);
                } else {
                    alignas(PG_IO_ALIGN_SIZE) char tmp[BLCKSZ * 16];
                    size_t step_max = TYPEALIGN_DOWN(io_align, sizeof(tmp));
                    size_t done = 0;

                    if (step_max == 0)
                        return SMGR_RD_CRC_ERROR;

                    while (done < mid) {
                        size_t step = Min(step_max, mid - done);
                        ssize_t nread = FileRead(seg->mdfd_vfd, tmp, step, cur_off + done,
                                                 WAIT_EVENT_DATA_FILE_READ);

                        if (nread < 0)
                            return SMGR_RD_CRC_ERROR;
                        if (nread == 0)
                            return SMGR_RD_NO_BLOCK;

                        memcpy(dst + done, tmp, nread);
                        if ((size_t)nread < step)
                            memset(dst + done + nread, 0, step - (size_t)nread);
                        done += step;
                    }
                }

                dst += mid;
                cur_off += mid;
                remaining -= mid;
                read_bytes += mid;
                offset += mid;
                continue;
            }

            if (tail > 0) {
                alignas(PG_IO_ALIGN_SIZE) char tmp[BLCKSZ];
                off_t aligned_off = TYPEALIGN_DOWN(io_align, cur_off);
                ssize_t nread = FileRead(seg->mdfd_vfd, tmp, io_align, aligned_off,
                                         WAIT_EVENT_DATA_FILE_READ);

                if (nread < 0)
                    return SMGR_RD_CRC_ERROR;
                if (nread == 0)
                    return SMGR_RD_NO_BLOCK;

                memcpy(dst, tmp, Min(tail, (size_t)nread));
                if ((size_t)nread < tail)
                    memset(dst + nread, 0, tail - (size_t)nread);

                dst += tail;
                cur_off += tail;
                remaining -= tail;
                read_bytes += tail;
                offset += tail;
            }
        }
    }
    
    return SMGR_RD_OK;
}

/*
 * vec_write - write vector data directly to file
 * Can throw ERROR on write failure.
 */
void vec_write(SMgrRelation reln, off_t offset, size_t nbytes,
                const char *buffer, bool skip_fsync, VecStorageType vec_storage_type)
{
    size_t written = 0;
    (void)vec_storage_type;
    (void)skip_fsync;
    
    while (written < nbytes) {
        /* Calculate block number and get/create segment */
        BlockNumber blocknum = offset / BLCKSZ;
        MdfdVec *seg = vec_getseg(reln, blocknum, VEC_EXTENSION_CREATE);
        if (seg == NULL) {
            RelPathStr path = vec_segment_path(reln, blocknum / RELSEG_SIZE);
            ereport(ERROR,
                    (errcode_for_file_access(),
                     errmsg("could not open vector file \"%s\": %m", path.str)));
        }
        
        /* Calculate offset within segment */
        off_t seg_offset = (off_t)(blocknum % RELSEG_SIZE) * BLCKSZ + (offset % BLCKSZ);
        off_t seg_max_offset = (off_t)RELSEG_SIZE * BLCKSZ;
        
        /* Calculate how much to write to this segment */
        size_t chunk = nbytes - written;
        if (seg_offset + chunk > seg_max_offset) {
            chunk = seg_max_offset - seg_offset;
        }
        
        const size_t io_align = PG_IO_ALIGN_SIZE;
        const char *src = buffer + written;
        size_t remaining = chunk;
        off_t cur_off = seg_offset;

        while (remaining > 0) {
            size_t head = 0;
            size_t mid;
            size_t tail;

            if ((cur_off % io_align) != 0) {
                head = Min(remaining, io_align - (size_t)(cur_off % io_align));
            }

            mid = TYPEALIGN_DOWN(io_align, remaining - head);
            tail = remaining - head - mid;

            if (head > 0) {
                alignas(PG_IO_ALIGN_SIZE) char tmp[BLCKSZ];
                off_t aligned_off = TYPEALIGN_DOWN(io_align, cur_off);
                size_t copy_off = cur_off - aligned_off;
                ssize_t nread = FileRead(seg->mdfd_vfd, tmp, io_align, aligned_off,
                                         WAIT_EVENT_DATA_FILE_READ);

                if (nread < 0)
                    nread = 0;
                if ((size_t)nread < io_align)
                    memset(tmp + nread, 0, io_align - (size_t)nread);

                memcpy(tmp + copy_off, src, head);

                if (FileWrite(seg->mdfd_vfd, tmp, io_align, aligned_off,
                              WAIT_EVENT_DATA_FILE_WRITE) != (ssize_t)io_align) {
                    RelPathStr path = vec_segment_path(reln, blocknum / RELSEG_SIZE);
                    ereport(ERROR,
                            (errcode_for_file_access(),
                             errmsg("could not write head block to vector file \"%s\": %m", path.str)));
                }

                src += head;
                cur_off += head;
                remaining -= head;
                continue;
            }

            if (mid > 0) {
                if (((uintptr_t)src % io_align) == 0) {
                    if (FileWrite(seg->mdfd_vfd, src, mid, cur_off,
                                  WAIT_EVENT_DATA_FILE_WRITE) != (ssize_t)mid) {
                        RelPathStr path = vec_segment_path(reln, blocknum / RELSEG_SIZE);
                        ereport(ERROR,
                                (errcode_for_file_access(),
                                 errmsg("could not write middle data to vector file \"%s\": %m", path.str)));
                    }
                } else {
                    alignas(PG_IO_ALIGN_SIZE) char tmp[BLCKSZ * 16];
                    size_t step_max = TYPEALIGN_DOWN(io_align, sizeof(tmp));
                    size_t done = 0;

                    if (step_max == 0) {
                        RelPathStr path = vec_segment_path(reln, blocknum / RELSEG_SIZE);
                        ereport(ERROR,
                                (errcode_for_file_access(),
                                 errmsg("invalid aligned temp buffer for vector file \"%s\"", path.str)));
                    }

                    while (done < mid) {
                        size_t step = Min(step_max, mid - done);
                        memcpy(tmp, src + done, step);
                        if (FileWrite(seg->mdfd_vfd, tmp, step, cur_off + done,
                                      WAIT_EVENT_DATA_FILE_WRITE) != (ssize_t)step) {
                            RelPathStr path = vec_segment_path(reln, blocknum / RELSEG_SIZE);
                            ereport(ERROR,
                                    (errcode_for_file_access(),
                                     errmsg("could not write middle chunk to vector file \"%s\": %m", path.str)));
                        }
                        done += step;
                    }
                }

                src += mid;
                cur_off += mid;
                remaining -= mid;
                continue;
            }

            if (tail > 0) {
                alignas(PG_IO_ALIGN_SIZE) char tmp[BLCKSZ];
                off_t aligned_off = TYPEALIGN_DOWN(io_align, cur_off);
                ssize_t nread = FileRead(seg->mdfd_vfd, tmp, io_align, aligned_off,
                                         WAIT_EVENT_DATA_FILE_READ);

                if (nread < 0)
                    nread = 0;
                if ((size_t)nread < io_align)
                    memset(tmp + nread, 0, io_align - (size_t)nread);

                memcpy(tmp, src, tail);
                if (FileWrite(seg->mdfd_vfd, tmp, io_align, aligned_off,
                              WAIT_EVENT_DATA_FILE_WRITE) != (ssize_t)io_align) {
                    RelPathStr path = vec_segment_path(reln, blocknum / RELSEG_SIZE);
                    ereport(ERROR,
                            (errcode_for_file_access(),
                             errmsg("could not write tail block to vector file \"%s\": %m", path.str)));
                }

                src += tail;
                cur_off += tail;
                remaining -= tail;
            }
        }

        written += chunk;
        offset += chunk;
    }
}

void read_vec_buf(Relation rel, size_t loc, size_t elem_size, char *buf, VecStorageType vec_storage_type)
{
    off_t offset = loc * elem_size;
    SMGR_READ_STATUS status = vec_read(rel->rd_smgr, offset, elem_size, buf, vec_storage_type);
    if (unlikely(status != SMGR_RD_OK)) {
        report_read_vector_error(status, rel, loc);
    }
}

void write_vector(Relation rel, size_t loc, size_t elem_size, const char *buf, VecStorageType vec_storage_type)
{
    off_t offset = loc * elem_size;
    vec_write(rel->rd_smgr, offset, elem_size, buf, false, vec_storage_type);
}

/* Create/truncate vector file */
void create_vec_data(Relation rel, bool need_wal)
{
    RelationGetSmgr(rel);
    
    /* Check if file already exists - may have been created by parallel workers */
    if (smgrexists(rel->rd_smgr, VECTOR_FORKNUM)) {
        return;
    }
    
    /* Create vector fork for vector storage */
    smgrcreate(rel->rd_smgr, VECTOR_FORKNUM, false);
    
    /* Sync to disk */
    smgrimmedsync(rel->rd_smgr, VECTOR_FORKNUM);
    
    /* WAL for create is deferred */
    (void)need_wal;
}

void truncate_vector_file(Relation rel)
{
    RelationGetSmgr(rel);
    
    if (!smgrexists(rel->rd_smgr, VECTOR_FORKNUM)) {
        return;
    }
    
    /* Truncate visibility map fork */
    ForkNumber fork = VECTOR_FORKNUM;
    BlockNumber nblocks = 0;
    smgrtruncate(rel->rd_smgr, &fork, 1, &nblocks, &nblocks);
}

/* Async I/O - simplified for PostgreSQL (sync fallback) */
void async_vec_read_batch(Relation rel, VecStorageType st, size_t elem_size, VecReadRequest *requests, int count)
{
    for (int i = 0; i < count; ++i) {
        VecReadRequest *req = &requests[i];
        VecBuffer vec_buf = vec_read_buffer(rel, req->loc, elem_size, st);
        req->io_ready = true;
        req->vector_buf = vec_buf;
        req->buf = vec_buf.buf;
        req->buf_from_cache = true;
    }
}

uint16 async_vec_wait_batch(VecReadRequest *requests, uint16 *completed_indices, uint16 *uncompleted_indices, uint16 *uncompleted_count)
{
    uint16 completed_count = 0;
    for (uint16 i = 0; i < *uncompleted_count; ++i) {
        uint16 idx = uncompleted_indices[i];
        if (requests[idx].io_ready) {
            completed_indices[completed_count++] = idx;
        } else {
            uncompleted_indices[completed_count] = idx;
        }
    }
    *uncompleted_count = *uncompleted_count - completed_count;
    return completed_count;
}

/* VecReadRequest::release */
void VecReadRequest::release()
{
    if (buf_from_cache) {
        vector_buf.release();
    } else {
        free_vector(buf);
    }
}

/* Buffer verify */
size_t vec_buffer_verify(size_t elem_size, size_t &total_slot)
{
    total_slot = 0;
    uint32 dim = get_effective_dim(elem_size);
    if (dim < min_cached_dim) {
        ereport(ERROR, (errmsg("Dimension %u less than the minimum dim %u.", dim, min_cached_dim)));
    }

    auto *mgr_ptr = VecBufMgr->pool[VecBufferManager::get_pool_offset(dim)];
    if (!mgr_ptr) {
        ereport(WARNING, (errmsg("Buffer pool for dimension %u is empty", dim)));
        return 0;
    }
    auto &mgr = *mgr_ptr;
    struct rel_holder {
        Relation rel;
        bool valid;
        rel_holder(Relation r, bool v) : rel(r), valid(v) {}
    };
    UnorderedMap<Oid, rel_holder> rel_cache;
    const size_t buf_size = sizeof(float) * dim;
    char *buf = (char *)palloc(buf_size);
    size_t cnt = 0;
    mgr.locmap->cvisit_all([&](const auto &kv) {
        const BufferSignature &sig = kv.first;
        const VecBufferLoc &loc = kv.second;
        if (loc.empty()) {
            ereport(WARNING, (errmsg("Found invalid buffer cache at Relation %u loc %lu.",
                                     sig.rel_id, sig.offset)));
            return;
        }
        auto it = rel_cache.find(sig.rel_id);
        if (it == rel_cache.end()) {
            Relation rel = try_relation_open(sig.rel_id, AccessShareLock);
            if (RelationIsValid(rel)) {
                RelationGetSmgr(rel);
            }
            it = rel_cache.emplace(sig.rel_id, rel, RelationIsValid(rel)).first;
        }
        if (!it->second.valid) {
            return;
        }
        Relation rel = it->second.rel;
        read_vec_buf(rel, sig.offset, elem_size, buf);
        const float *res = (float *)VecBufMgr->get_vector(loc.buf_offset, loc.offset, dim);
        if (memcmp(buf, res, buf_size) != 0) {
            ereport(WARNING, (
                errmsg("Found corrupted buffer cache at Relation %u loc %lu, at %u:%u.",
                       sig.rel_id, sig.offset, loc.buf_offset, loc.offset)));
            ++cnt;
        }
        ++total_slot;
    });
    for (auto &kv : rel_cache) {
        if (kv.second.valid) {
            relation_close(kv.second.rel, AccessShareLock);
        }
    }
    ann_helper::optional_destroy(rel_cache);
    pfree(buf);
    return cnt;
}

/* VecBuffer constructors */
VecBuffer::VecBuffer() : pool_offset(-1), loc(), buf(nullptr) {}

VecBuffer::VecBuffer(int16 pool_offset, uint32 buf_offset, uint32 offset, char *buf)
    : pool_offset(pool_offset), loc(buf_offset, offset), buf(buf) {}

char *VecBuffer::get_vecbuf() { return buf; }

/* VectorBufferInspect - for vectorbuffer_inspect() function */
struct VectorBufferInspect {
    char *used_space;
    char *elem_size;
    size_t elem_nums;
    size_t hit;
    size_t miss;
    double evict;
};

static Vector<VectorBufferInspect> get_inspect()
{
    Vector<VectorBufferInspect> result;
    if (!VecBufMgr || !VecBufMgr->buffer_inited) {
        return result;
    }
    for (int16 pool_offset = 0; pool_offset < NVecPool; ++pool_offset) {
        if (!VecBufMgr->pool[pool_offset]) {
            continue;
        }
        VecBufferPool &cur_pool = *VecBufMgr->pool[pool_offset];
        VectorBufferInspect inspect_info;

        int16 start_dim = (pool_offset - 1) * vector_step_size;
        int16 end_dim = pool_offset * vector_step_size;
        if (end_dim > DISKANN_MAX_DIM) {
            end_dim = DISKANN_MAX_DIM;
        }
        size_t min_size = start_dim * sizeof(float) + 1;
        size_t max_size = end_dim * sizeof(float);
        auto sf_min_size = ann_helper::format_size(min_size);
        auto sf_max_size = ann_helper::format_size(max_size);

        inspect_info.elem_size = psprintf("%.0f %s ~ %.0f %s",
            sf_min_size.n, sf_min_size.unit_str(), sf_max_size.n, sf_max_size.unit_str());

        size_t used_space = cur_pool.stats.nblock * vec_block_size;
        auto sf_space = ann_helper::format_size(used_space);
        inspect_info.used_space = psprintf("%.2f %s", sf_space.n, sf_space.unit_str());
        inspect_info.elem_nums = cur_pool.stats.ndata - cur_pool.nfreeze.load(RELAXED_ORDER);
        inspect_info.hit = cur_pool.hit.value();
        inspect_info.miss = cur_pool.miss.value();
        size_t nevict = cur_pool.stats.nevict.load(RELAXED_ORDER);
        if (nevict > 0) {
            long sec;
            int usec;
            TimestampTz cur_time = GetCurrentTimestamp();
            TimestampDifference(cur_pool.stats.first_evict_time, cur_time, &sec, &usec);
            inspect_info.evict = nevict / std::max(1e-5, sec + (double)usec / 1e6);
        } else {
            inspect_info.evict = 0;
        }
        result.push_back(inspect_info);
    }
    return result;
}

extern "C" {
PG_FUNCTION_INFO_V1(vectorbuffer_inspect);
}

Datum vectorbuffer_inspect(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    if (SRF_IS_FIRSTCALL()) {
        funcctx = SRF_FIRSTCALL_INIT();
        MemoryContext oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
        Vector<VectorBufferInspect> inspect_results = get_inspect();
        TupleDesc tupdesc = CreateTemplateTupleDesc(6);
        TupleDescInitEntry(tupdesc, (AttrNumber)1, "used_space", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)2, "elem_size", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)3, "elem_nums", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)4, "hit", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)5, "miss", INT8OID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber)6, "eviction_rate", FLOAT8OID, -1, 0);
        TupleDescFinalize(tupdesc);
        funcctx->tuple_desc = BlessTupleDesc(tupdesc);
        funcctx->max_calls = inspect_results.size();
        
        Vector<VectorBufferInspect> *stored_results = new Vector<VectorBufferInspect>();
        *stored_results = std::move(inspect_results);
        funcctx->user_fctx = stored_results;
        
        MemoryContextSwitchTo(oldcontext);
    }
    
    funcctx = SRF_PERCALL_SETUP();
    Vector<VectorBufferInspect> *results = (Vector<VectorBufferInspect> *)funcctx->user_fctx;
    
    if (results && funcctx->call_cntr < funcctx->max_calls) {
        VectorBufferInspect *inspect = &(*results)[funcctx->call_cntr];
        Datum values[6];
        bool nulls[6] = {false};
        values[0] = CStringGetTextDatum(inspect->used_space);
        values[1] = CStringGetTextDatum(inspect->elem_size);
        values[2] = Int64GetDatum(inspect->elem_nums);
        values[3] = Int64GetDatum(inspect->hit);
        values[4] = Int64GetDatum(inspect->miss);
        values[5] = Float8GetDatum(inspect->evict);
        
        HeapTuple tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
        if (!tuple) {
            delete results;
            SRF_RETURN_DONE(funcctx);
        }
        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
    }
    
    delete results;
    SRF_RETURN_DONE(funcctx);
}
