/**
 * Copyright ...
 * vector database macro definitions
 */

#ifndef VTL_MACRO_H
#define VTL_MACRO_H

#include "c.h"

namespace disk_container {
constexpr uint16 DISK_VECTOR_META_ID = 0x4321;
constexpr uint16 DISK_VECTOR_DATA_ID = 0xdcba;
constexpr uint16 DISK_VECTOR_DUMMY_ID = 0xace1;
constexpr uint16 DISK_ARRAT_DATA_ID = 0xdcb1;
constexpr uint16 PLAIN_STORE_META_ID = 0xa10b;
constexpr uint16 PLAIN_STORE_DATA_ID = 0xb888;
constexpr uint16 HASH_TABLE_DATA_ID = 0x888b;
constexpr uint32 DISK_VECTOR_META_MAGIC = 0x1a2b3c4d;
constexpr uint32 DISK_VECTOR_DATA_MAGIC = 0x0912ab34;
constexpr uint32 DISK_ARRAY_DATA_MAGIC = 0x3912ab34;
constexpr uint32 PLAIN_STORE_META_MAGIC = 0x6a9b874f;
constexpr uint32 VECTOR_INDEX_META_MAGIC = 0x5643494d;
constexpr uint32 VECTOR_INDEX_NODE_MAGIC = 0x5643494e;
constexpr uint32 HASH_PLAIN_DATA_MAGIC = 0x94d68ea0;
template <size_t N> 
constexpr uint32 HASH_INPLACE_DATA_MAGIC() { return 0xacdb0000 + (N & 0xffff); }
inline bool is_hash_inplace_data_magic(uint32 magic) { return (magic & 0xffff0000) == 0xacdb0000; }

enum class AccessorLockType {
    ExternalLock,   /* no-op, lock is managed by external code */
    NoLockUnsafe,   /* should be no-op */
    NoLockRW,       /* no lock applied, but still do synchronization */
    NoLockRead,     /* read version nolock */
    NoLockWrite,    /* write version nolock */
    ReadLock,       /* shared lock */
    WriteLock,      /* exclusive lock */
};
} /* namespace disk_container */

#endif /* VTL_MACRO_H */
