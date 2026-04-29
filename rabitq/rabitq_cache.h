/**
 * Copyright ...
 * RaBitQ Cache
 */

#ifndef RABITQ_CACHE_H
#define RABITQ_CACHE_H

#include <new>
#include "pg_compat.h"

#define RABITQ_CTX (g_instance.rabitq_ctx)

#define RABITQ_CACHE_ALLOC(size)    \
    MemoryContextAllocDebug(RABITQ_CTX.cache_ctx, (size), __FILE__, __LINE__)
#define RABITQ_CACHE_FREE(ptr) pfree(ptr)
#define RABITQ_CACHE_FREE_EXT(ptr)  \
    do {                            \
        if (ptr) {                  \
            RABITQ_CACHE_FREE(ptr); \
            ptr = NULL;             \
        }                           \
    } while (0)

#define RABITQ_CACHE_ALLOC_ALIGNED(size) mem_align_alloc((ann_helper::vector_aligned_size), (size))
#define RABITQ_CACHE_FREE_ALIGNED(ptr) mem_align_free(ptr)
#define RABITQ_CACHE_FREE_ALIGNED_EXT(ptr)  \
    do {                                    \
        if (ptr) {                          \
            RABITQ_CACHE_FREE_ALIGNED(ptr); \
            ptr = NULL;                     \
        }                                   \
    } while (0)

constexpr int RABITQ_MAX_CACHE_SIZE = 64;
constexpr int RABITQ_LRU_CACHE_SIZE = 128;

namespace rabitq {

typedef struct RaBitQCache
{
    Oid oid{InvalidOid}; /* index oid */
    char *fixed_data{NULL}; /* random matrix + centroids + rotated_centroids */

    RaBitQCache() = default;
    void destroy();

    RaBitQCache(const RaBitQCache &other)
    {
        oid = other.oid;
        fixed_data = other.fixed_data;
    }

    RaBitQCache(RaBitQCache &&other)
    {
        oid = other.oid;
        fixed_data = other.fixed_data;
        other.oid = InvalidOid;
        other.fixed_data = NULL;
    }

    inline void operator=(const RaBitQCache &other)
    {
        new (this) RaBitQCache(other);
    }

    inline void operator=(RaBitQCache &&other)
    {
        new (this) RaBitQCache(other);
    }
}   RaBitQCache;

/* thread unsafe, use rwlock for access */
const RaBitQCache *get_rabitq_cache(Oid index_oid);
bool set_rabitq_cache(RaBitQCache cache);
bool clear_rabitq_cache(Oid index_oid);
void resize_rabitq_cache(int new_size);

} /* namespace rabitq */

#endif /* RABITQ_CACHE_H */
