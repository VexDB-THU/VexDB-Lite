#ifndef BULKBUFFER_SMGR_H
#define BULKBUFFER_SMGR_H

#include <atomic>
#include <vtl/vector>
#include <vtl/hashtable>

#include "postgres.h"
#include "utils/palloc.h"
#include "buffer_base.h"
#include "module/parallel_counter.h"

/* Deferred - BULKBUF_CTX (g_instance.diskann_cxt.bulk_buf_ctx) */
struct Chunk {
    char *buf;
    Chunk(char *b) : buf(b) {}
};

#define STATISTIC TRUE

struct BulkBufferInspect {
    char *name;
    char *used_space;
    char *elem_size;
    size_t elem_nums;
    size_t visit_count;
    TimestampTz load_time;
    BulkBufferInspect(char *n, char *used, char *size, size_t nums, size_t visits, TimestampTz time)
        : name(n), used_space(used), elem_size(size), elem_nums(nums), visit_count(visits), load_time(time) {}
};

class BulkBuffer : public BaseObject {
public:
    Oid id;
    pthread_rwlock_t lock;
    MemoryContext ctx;
    
    BulkBuffer(Relation r, MemoryContext ctx, size_t e_num, uint32 store_esize, VecStorageType type);
    char *get(size_t idx);
    void update(size_t idx, const char *value);
    void acquire();
    void release();
    void destroy();
    BulkBufferInspect get_inspect();
private:
    Vector<Chunk> vec;
    slock_t mutex;
    std::atomic<size_t> total_elem_nums;
    uint32 store_elem_size; /* true elem size, store in disk, equal to `elem_typelen` * `dim` */
    uint32 elem_size; /* Bytes per elem, equal to std::min(`store_elem_size` , `vector_aligned_size`) */
    uint32 pow_elem_nums_per_chunk; /* 2^x elems in a chunk */
    uint32 one_chunk_elem_nums; /* equal to 1 << `pow_elem_nums_per_chunk` */
    size_t chunk_size; /* Bytes per chunk, equal to `elem_size` * `one_chunk_elem_nums` */
    VecStorageType vec_storage_type;
    ann_helper::ParaCounter visit_count;
    TimestampTz load_time;

    uint32 get_chunk_no(size_t idx);
    uint32 get_chunk_offset(size_t idx);
    void load_one_chunk(Relation index, size_t chunk_idx);
};

class BulkBufferManager {
public:
    BulkBufferManager();
    bool index_load(Relation index, const char *ctx_name);
    void index_load(Relation index, Oid part_oid);
    bool index_release(Relation index, bool need_notice = false);
    bool index_release(Oid rel_id, bool need_notice = false);
    bool auto_index_release(Relation index);
    void auto_partindex_release(Oid partindex_oid);
    void auto_partindex_release(Relation parttable_rel, Oid part_oid);
    void rename_ctx(Oid rel_id, const char *new_name);
    BulkBuffer *get_bulkbuf(Relation index);
    Vector<BulkBufferInspect> get_inspect();
private:
    bool get_array(Relation index, const char *ctx_name, size_t e_num, uint32 store_esize,
        VecStorageType vec_storage_type);
    void destroy();

    UnorderedMap<Oid, BulkBuffer *> visit_map;
    pthread_rwlock_t visit_map_lock;
};

/* Deferred - g_instance.diskann_cxt.bulkbuf_mgr not yet implemented */
/* #define BULKBUF_MGR ((BulkBufferManager *)g_instance.diskann_cxt.bulkbuf_mgr) */

uint32 calculate_pow(uint32 elem_size, uint32 targe_size_mb);
char *allocate_aligned_memory(size_t alloc_bytes, MemoryContext ctx = CurrentMemoryContext);

void init_bulkbuf_smgr();
extern Datum index_memory_load_oid(PG_FUNCTION_ARGS);
extern Datum index_memory_load_name(PG_FUNCTION_ARGS);
extern Datum index_memory_release_oid(PG_FUNCTION_ARGS);
extern Datum index_memory_release_name(PG_FUNCTION_ARGS);
extern Datum bulkbuffer_inspect(PG_FUNCTION_ARGS);


#endif /* BulkBuffer_SMGR_H */