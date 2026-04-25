#ifndef BUFFER_MANAGER_H
#define BUFFER_MANAGER_H

#include "postgres.h"
#include "bulkbuf_smgr.h"
#include "vector_smgr.h"

inline void init_vector_engine_buffer_manager() {
    init_bulkbuf_smgr();
}

#define BULKBUF_SUPPORT(index) false
#define GET_BULKBUF(index) ((BulkBuffer *)NULL)

#endif /* BUFFER_MANAGER_H */