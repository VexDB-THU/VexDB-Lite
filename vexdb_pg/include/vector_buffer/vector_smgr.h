/**
 * Copyright (c) 2026 VexDB-THU
 * Vector storage interface
 */

#ifndef VECTOR_SMGR_H
#define VECTOR_SMGR_H

#include <vtl/definition>

#ifdef __cplusplus
extern "C" {
#endif
#include "storage/smgr.h"
#include "storage/lwlock.h"
#ifdef __cplusplus
}
#endif

#include "vector_buffer/buffer_base.h"
#include "vector_buffer/vector_buffer_manager.h"

#define VERIFY_BUFFER false
#define VEX_VEC_WRITE_LOCK_STRIPES 128

/* Cross-process locks for aligned-block read/modify/write in vec_write. */
extern LWLockPadded *VexVecWriteLocks;

/* SMGR_READ_STATUS enum */
enum SMGR_READ_STATUS {
    SMGR_RD_OK = 0,
    SMGR_RD_NO_BLOCK,
    SMGR_RD_CRC_ERROR
};

/* BufferParams for VecBufferLoc constructor */
struct BufferParams {
    Relation rel;
    size_t loc;
    size_t elem_size;
    int16 pool_offset;
    VecStorageType storage_type;
    uint32 buf_offset;
    uint32 offset;
    SMGR_READ_STATUS status;
};

/* VecBufferLoc constructor from BufferParams is declared in vector_buffer_manager.h */
/* VecBufferLoc struct is defined in vector_buffer_manager.h */

/* VecBuffer - buffer holding a vector */
struct VecBuffer {
    int16 pool_offset;
    VecBufferLoc loc;
    char *buf;

    VecBuffer();
    VecBuffer(int16 pool_offset, uint32 buf_offset, uint32 offset, char *buf);
    char *get_vecbuf();
    void release();
    void set_io_state(const VecBufIOState state);
    bool get_io_ready();
    bool get_io_failed();
};

/* VecReadRequest for async I/O */
struct VecReadRequest {
    bool buf_from_cache;
    bool io_ready;
    size_t loc;
    char *buf;
    Relation rel;
    void *aio_handle;
    VecBuffer vector_buf;

    void release();
};

/* Initialization */
extern void init_vector_smgr();
extern bool enable_vec_buffer_manager();
extern bool vexdb_lite_is_preloaded();

/* Main API */
extern VecBuffer vec_read_buffer(Relation rel, size_t loc, size_t vec_size, VecStorageType st = VecStorageType::PureVec);

/* Low-level I/O */
extern SMGR_READ_STATUS vec_read(SMgrRelation reln, off_t offset, size_t nbytes, char *buffer, VecStorageType st = VecStorageType::PureVec);
extern void vec_write(SMgrRelation reln, off_t offset, size_t nbytes, const char *buffer, bool skip_fsync, VecStorageType st = VecStorageType::PureVec);

/* Helper functions */
extern void read_vec_buf(Relation rel, size_t loc, size_t elem_size, char *buf, VecStorageType st = VecStorageType::PureVec);
extern void write_vector(Relation rel, size_t loc, size_t elem_size, const char *buf, VecStorageType st = VecStorageType::PureVec);

/* File management */
extern void create_vec_data(Relation rel, bool need_wal);
extern void truncate_vector_file(Relation rel);
extern void truncate_vector_file_to(Relation rel, size_t nbytes);

/* Cache management */
extern void vec_invalidate_buffer_cache(Oid relNode, size_t loc, size_t elem_size);
extern void vec_invalidate_buffer_cache(Oid relNode, size_t elem_size);

/* Async I/O */
extern void async_vec_read_batch(Relation rel, VecStorageType st, size_t elem_size, VecReadRequest *requests, int count);
extern uint16 async_vec_wait_batch(VecReadRequest *requests, uint16 *completed_indices, uint16 *uncompleted_indices, uint16 *uncompleted_count);

/* Buffer verification */
extern size_t vec_buffer_verify(size_t elem_size, size_t &total_slot);

/* Internal helpers */
extern void release_vector_buffer(const VecBufferLoc &loc);

/* AIO helpers */
inline bool is_aio_beneficial()
{
    return false;
}

#endif /* VECTOR_SMGR_H */
