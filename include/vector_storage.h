/*
 * vector_storage.h - Vector data storage interface
 *
 * Vector data is stored in separate files named {relpath}_vec
 * to work around PostgreSQL's lack of custom fork support for extensions.
 */

#ifndef VECTOR_STORAGE_H
#define VECTOR_STORAGE_H

#include "c.h"
#include "storage/block.h"
#include "storage/fd.h"

#define VECTOR_STORAGE_BLOCK_SIZE (1024 * 1024 * 1024)  /* 1GB blocks */

/*
 * Vector storage handle
 */
typedef struct VectorStorageData {
    RelFileLocator rlocator;    /* relation identifier */
    int fd;                    /* file descriptor */
    BlockNumber nblocks;        /* current size in blocks */
    char *path;                /* full file path */
    bool is_temp;              /* temporary storage */
} VectorStorageData;

typedef VectorStorageData *VectorStorage;

/*
 * Open vector storage for an index relation
 * Creates the file if it doesn't exist
 */
extern VectorStorage vector_storage_open(Relation index);

/*
 * Close vector storage and release resources
 */
extern void vector_storage_close(VectorStorage vs);

/*
 * Write data at specified offset
 * Thread-safe via pwrite
 */
extern void vector_storage_write(VectorStorage vs, uint64 offset, const char *data, size_t len);

/*
 * Read data at specified offset
 * Thread-safe via pread
 */
extern void vector_storage_read(VectorStorage vs, uint64 offset, char *data, size_t len);

/*
 * Allocate space for vector data
 * Returns the offset where data should be written
 * Handles cross-block allocation automatically
 */
extern uint64 vector_storage_alloc(VectorStorage vs, size_t len);

/*
 * Get current file size in bytes
 */
extern uint64 vector_storage_size(VectorStorage vs);

/*
 * Check if vector storage exists for a relation
 */
extern bool vector_storage_exists(Relation index);

/*
 * Delete vector storage file for a relation
 */
extern void vector_storage_delete(Relation index);

#endif /* VECTOR_STORAGE_H */
