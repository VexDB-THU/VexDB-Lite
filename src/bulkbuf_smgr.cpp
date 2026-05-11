#include "pg_compat.h"
#include "bulkbuf_smgr.h"
#include "knl/knl_variable.h"
#include <cmath>
#include <pthread.h>

namespace ann_helper {
    constexpr size_t vector_aligned_size = 64;
}

using namespace ann_helper;

uint32 calculate_pow(uint32 elem_size, uint32 targe_size_mb)
{
    targe_size_mb = Max(targe_size_mb, 1);
    // 64-bit math: targe_size_mb >= 4096 would overflow uint32 here.
    size_t target_size = (size_t)targe_size_mb * 1024ULL * 1024ULL;
    double target_count = static_cast<double>(target_size) / elem_size;
    double x_double = log2(target_count);
    int x = static_cast<int>(floor(x_double));
    return Max(x, 0);
}

char *allocate_aligned_memory(size_t alloc_bytes, MemoryContext ctx)
{
    constexpr size_t extra = vector_aligned_size + sizeof(void *);
    char *original_ptr = (char *)MemoryContextAllocHuge(ctx, alloc_bytes + extra);
    uint64 raw_addr = (uint64)original_ptr + sizeof(void *);
    uint64 aligned_addr = (raw_addr + vector_aligned_size - 1) & ~(vector_aligned_size - 1);
    return reinterpret_cast<char *>(aligned_addr);
}

BulkBuffer::BulkBuffer(Relation r, MemoryContext ctx, size_t e_num, uint32 store_esize, VecStorageType type)
    : id(0), ctx(ctx), store_elem_size(store_esize), vec_storage_type(type)
{
    (void)r;
    (void)e_num;
    elem_size = ((store_elem_size + vector_aligned_size - 1) / vector_aligned_size) * vector_aligned_size;
    total_elem_nums = 0;
    pow_elem_nums_per_chunk = calculate_pow(elem_size, 24);
    one_chunk_elem_nums = 1 << pow_elem_nums_per_chunk;
    chunk_size = elem_size * one_chunk_elem_nums;
}

char *BulkBuffer::get(size_t idx)
{
    (void)idx;
    return nullptr;
}

void BulkBuffer::update(size_t idx, const char *value)
{
    (void)idx;
    (void)value;
}

void BulkBuffer::acquire() {}
void BulkBuffer::release() {}

void BulkBuffer::destroy()
{
    vec.destroy();
}

BulkBufferInspect BulkBuffer::get_inspect()
{
    return BulkBufferInspect(nullptr, nullptr, nullptr, 0, 0, 0);
}

uint32 BulkBuffer::get_chunk_no(size_t idx)
{
    return idx >> pow_elem_nums_per_chunk;
}

uint32 BulkBuffer::get_chunk_offset(size_t idx)
{
    return idx & (one_chunk_elem_nums - 1);
}

void BulkBuffer::load_one_chunk(Relation index, size_t chunk_idx)
{
    (void)index;
    (void)chunk_idx;
}

BulkBufferManager::BulkBufferManager()
{
    pthread_rwlock_init(&visit_map_lock, NULL);
}

bool BulkBufferManager::index_load(Relation index, const char *ctx_name)
{
    (void)index;
    (void)ctx_name;
    return false;
}

void BulkBufferManager::index_load(Relation index, Oid part_oid)
{
    (void)index;
    (void)part_oid;
}

bool BulkBufferManager::index_release(Relation index, bool need_notice)
{
    (void)index;
    (void)need_notice;
    return false;
}

bool BulkBufferManager::index_release(Oid rel_id, bool need_notice)
{
    (void)rel_id;
    (void)need_notice;
    return false;
}

bool BulkBufferManager::auto_index_release(Relation index)
{
    (void)index;
    return false;
}

void BulkBufferManager::auto_partindex_release(Oid partindex_oid)
{
    (void)partindex_oid;
}

void BulkBufferManager::auto_partindex_release(Relation parttable_rel, Oid part_oid)
{
    (void)parttable_rel;
    (void)part_oid;
}

void BulkBufferManager::rename_ctx(Oid rel_id, const char *new_name)
{
    (void)rel_id;
    (void)new_name;
}

BulkBuffer *BulkBufferManager::get_bulkbuf(Relation index)
{
    (void)index;
    return nullptr;
}

Vector<BulkBufferInspect> BulkBufferManager::get_inspect()
{
    return Vector<BulkBufferInspect>();
}

bool BulkBufferManager::get_array(Relation index, const char *ctx_name, size_t e_num, uint32 store_esize, VecStorageType vec_storage_type)
{
    (void)index;
    (void)ctx_name;
    (void)e_num;
    (void)store_esize;
    (void)vec_storage_type;
    return false;
}

void BulkBufferManager::destroy()
{
}

void init_bulkbuf_smgr()
{
}

Datum index_memory_load_oid(PG_FUNCTION_ARGS)
{
    PG_RETURN_BOOL(false);
}

Datum index_memory_load_name(PG_FUNCTION_ARGS)
{
    PG_RETURN_BOOL(false);
}

Datum index_memory_release_oid(PG_FUNCTION_ARGS)
{
    PG_RETURN_BOOL(false);
}

Datum index_memory_release_name(PG_FUNCTION_ARGS)
{
    PG_RETURN_BOOL(false);
}

Datum bulkbuffer_inspect(PG_FUNCTION_ARGS)
{
    PG_RETURN_NULL();
}
