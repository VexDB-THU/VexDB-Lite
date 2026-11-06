#ifndef VEX_RABITQ_PLATFORM_H
#define VEX_RABITQ_PLATFORM_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "distance/core/distance.h"

#if defined(PG_VEXDB_TARGET_PG)
extern "C" {
#include "postgres.h"
#include "utils/elog.h"
}
#include <vtl/allocator>
#else
#ifndef NOTICE
#define NOTICE 0
#endif
#endif

namespace rabitq {

#if defined(PG_VEXDB_TARGET_PG)
template <typename T>
using HostAllocator = DEFAULT_ALLOCATOR<T>;
#else
template <typename T>
using HostAllocator = std::allocator<T>;
#endif

template <typename T>
inline T *alloc_array(size_t count)
{
    if (count == 0) {
        return nullptr;
    }
#if defined(PG_VEXDB_TARGET_PG)
    return static_cast<T *>(palloc(count * sizeof(T)));
#else
    void *ptr = std::malloc(count * sizeof(T));
    if (!ptr) {
        throw std::bad_alloc();
    }
    return static_cast<T *>(ptr);
#endif
}

template <typename T>
inline T *alloc_array_zero(size_t count)
{
    T *ptr = alloc_array<T>(count);
    if (ptr) {
        std::memset(ptr, 0, count * sizeof(T));
    }
    return ptr;
}

template <typename T, typename... Args>
inline T *make_object(Args &&...args)
{
#if defined(PG_VEXDB_TARGET_PG)
    void *storage = palloc(sizeof(T));
    return new (storage) T(std::forward<Args>(args)...);
#else
    return new T(std::forward<Args>(args)...);
#endif
}

template <typename T>
inline void destroy_object(T *ptr)
{
    if (!ptr) {
        return;
    }
#if defined(PG_VEXDB_TARGET_PG)
    ptr->~T();
    pfree(ptr);
#else
    delete ptr;
#endif
}

inline void free_mem(void *ptr)
{
    if (!ptr) {
        return;
    }
#if defined(PG_VEXDB_TARGET_PG)
    pfree(ptr);
#else
    std::free(ptr);
#endif
}

[[noreturn]] inline void fail(std::string_view message)
{
#if defined(PG_VEXDB_TARGET_PG)
    ereport(ERROR, (errmsg("RaBitQ: %.*s", static_cast<int>(message.size()), message.data())));
#else
    throw std::runtime_error("RaBitQ: " + std::string(message));
#endif
}

inline void random_bytes(void *dst, size_t size)
{
#if defined(PG_VEXDB_TARGET_PG)
    if (!pg_strong_random(dst, size)) {
        fail("could not generate rotation matrix");
    }
#else
    auto *out = static_cast<uint8_t *>(dst);
    std::random_device random;
    for (size_t i = 0; i < size; ++i) {
        out[i] = static_cast<uint8_t>(random());
    }
#endif
}

} // namespace rabitq

#endif
