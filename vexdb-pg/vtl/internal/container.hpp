/**
 * VTL Container Configuration
 */
#ifndef VTL_CONTAINER_H
#define VTL_CONTAINER_H

#include "pg_compat.h"

#define CONTAINER_USE_STL false
#define CONTAINER_USE_STL_VECTOR false
#define CONTAINER_USE_STL_TREE false
#define CONTAINER_USE_STL_HASH false
#define CONTAINER_USE_STL_PAIR false
#define CONTAINER_USE_STL_OPTIONAL false
#define CONTAINER_USE_STL_VARIANT false
#define CONTAINER_USE_STL_STRINGVIEW false
#define CONTAINER_USE_STL_STRING false
#define CONTAINER_USE_STL_SPAN false
#define CONTAINER_USE_STL_TUPLE false
#define VERIFY_DATA false
#define BTREE_VERIFY_DATA false

#include "utils/palloc.h"
#define NEW new
#define New(cxt) new

struct EmptyObject {};

template <typename T> using SAFE_CONSTRUCTOR = EmptyObject;

#endif
