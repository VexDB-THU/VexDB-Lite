#include "distance/core/distance.h"

#include <cstdint>
#include <cstdlib>
#include <new>

#if defined(PG_VEXDB_TARGET_PG)
extern "C" {
#include "postgres.h"
#include "utils/palloc.h"
}
#endif

uint32 get_aligned_dim(uint32 dim)
{
    return (dim + vector_step_size - 1) / vector_step_size * vector_step_size;
}

size_t get_aligned_vec_size(size_t vec_size)
{
    return (vec_size + ann_helper::vector_aligned_size - 1) /
        ann_helper::vector_aligned_size * ann_helper::vector_aligned_size;
}

namespace {

void *alloc_aligned(size_t size)
{
#if defined(PG_VEXDB_TARGET_PG)
    return palloc_aligned(size, ann_helper::vector_aligned_size, 0);
#else
    void *ptr = nullptr;
    if (posix_memalign(&ptr, ann_helper::vector_aligned_size, size) != 0 || !ptr) {
        throw std::bad_alloc();
    }
    return ptr;
#endif
}

} // namespace

float *alloc_floatvector(uint32 dim, size_t n)
{
    return static_cast<float *>(alloc_aligned(sizeof(float) * dim * n));
}

char *alloc_vector(size_t vec_size, size_t n)
{
    return static_cast<char *>(alloc_aligned(vec_size * n));
}

bool is_aligned(const void *ptr)
{
    return reinterpret_cast<uintptr_t>(ptr) % ann_helper::vector_aligned_size == 0;
}
