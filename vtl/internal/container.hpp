/**
 * VTL Container Configuration
 */
#ifndef VTL_CONTAINER_H
#define VTL_CONTAINER_H

#if defined(PG_VEXDB_TARGET_DUCK)
#include <new>
#include <cstddef>
#include <cstdint>
using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using int8 = int8_t;
using int16 = int16_t;
using int32 = int32_t;
using int64 = int64_t;
#ifndef FORCE_INLINE
#define FORCE_INLINE inline __attribute__((always_inline))
#endif
#else
#include "pg_compat.h"
#endif

#define CONTAINER_USE_STL false
#define CONTAINER_USE_STL_VECTOR false
#define CONTAINER_USE_STL_TREE false
#define CONTAINER_USE_STL_HASH false
#define CONTAINER_USE_STL_PAIR false
#if defined(PG_VEXDB_TARGET_DUCK)
#define CONTAINER_USE_STL_OPTIONAL true
#else
#define CONTAINER_USE_STL_OPTIONAL false
#endif
#define CONTAINER_USE_STL_VARIANT false
#define CONTAINER_USE_STL_STRINGVIEW false
#define CONTAINER_USE_STL_STRING false
#define CONTAINER_USE_STL_SPAN false
#if defined(PG_VEXDB_TARGET_DUCK)
#define CONTAINER_USE_STL_TUPLE true
#else
#define CONTAINER_USE_STL_TUPLE false
#endif
#define VERIFY_DATA false
#define BTREE_VERIFY_DATA false

#if defined(PG_VEXDB_TARGET_DUCK)
#define NEW new
#else
#include "utils/palloc.h"
#define NEW new
#define New(cxt) new
#endif

struct EmptyObject {};

template <typename T> using SAFE_CONSTRUCTOR = EmptyObject;

#endif
