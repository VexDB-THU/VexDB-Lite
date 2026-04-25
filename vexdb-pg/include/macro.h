/**
 * Copyright ...
 * vector database macro definitions
 */

#ifndef ANNVECTOR_MACRO_H
#define ANNVECTOR_MACRO_H

#include "c.h"

#define USE_LEAK_CHECKER false

namespace disk_container {
constexpr uint16 DISK_BT_DATA_ID = 0x4441;
constexpr uint16 DISK_BT_META_ID = 0x4e4f;
constexpr uint32 DISK_BT_META_MAGIC = 0x0351a02e9;
} /* namespace disk_container */

#endif /* ANNVECTOR_MACRO_H */
