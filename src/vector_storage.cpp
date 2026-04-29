/*
 * vector_storage.cpp - Vector data storage implementation
 *
 * Stores vector data in separate files named {relpath}_vec
 * Uses 1GB blocks like openGauss's VECTOR_FORKNUM
 */

#include "pg_compat.h"
#include "vector_storage.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static const size_t VECTOR_BLOCK_SIZE = (size_t)VECTOR_STORAGE_BLOCK_SIZE;

static void get_vector_path(RelFileLocator rlocator, ProcNumber backend, char *path, size_t path_len)
{
    RelPathStr rel_path = GetRelationPath(rlocator.dbOid, rlocator.spcOid,
                                          rlocator.relNumber, backend,
                                          MAIN_FORKNUM);
    snprintf(path, path_len, "%s_vec", rel_path.str);
}

VectorStorage vector_storage_open(Relation index)
{
    VectorStorage vs;
    char path[MAXPGPATH];
    struct stat st;
    int flags;
    int mode;
    ProcNumber backend;

    vs = (VectorStorage)palloc(sizeof(VectorStorageData));
    vs->rlocator = index->rd_locator;
    vs->nblocks = 0;

    if (index->rd_rel->relpersistence == RELPERSISTENCE_TEMP) {
        vs->is_temp = true;
        backend = index->rd_backend;
    } else {
        vs->is_temp = false;
        backend = INVALID_PROC_NUMBER;
    }

    get_vector_path(vs->rlocator, backend, path, sizeof(path));
    vs->path = pstrdup(path);

    if (vs->is_temp) {
        flags = O_RDWR | O_CREAT | O_TRUNC;
        mode = 0600;
    } else {
        flags = O_RDWR | O_CREAT;
        mode = 0660;
    }

    vs->fd = open(path, flags, mode);
    if (vs->fd < 0) {
        ereport(ERROR, (errcode_for_file_access(),
                        errmsg("could not open vector storage file \"%s\": %m", path)));
    }

    if (fstat(vs->fd, &st) < 0) {
        close(vs->fd);
        ereport(ERROR, (errcode_for_file_access(),
                        errmsg("could not stat vector storage file \"%s\": %m", path)));
    }

    vs->nblocks = (BlockNumber)(st.st_size / VECTOR_BLOCK_SIZE);

    return vs;
}

void vector_storage_close(VectorStorage vs)
{
    if (vs->fd >= 0) {
        close(vs->fd);
    }
    pfree(vs->path);
    pfree(vs);
}

void vector_storage_write(VectorStorage vs, uint64 offset, const char *data, size_t len)
{
    ssize_t written;

    if (vs->fd < 0) {
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                        errmsg("vector storage is not open")));
    }

    written = pwrite(vs->fd, data, len, offset);
    if (written != (ssize_t)len) {
        ereport(ERROR, (errcode_for_file_access(),
                        errmsg("could not write to vector storage file \"%s\" at offset %lu: %m",
                               vs->path, (unsigned long)offset)));
    }
}

void vector_storage_read(VectorStorage vs, uint64 offset, char *data, size_t len)
{
    ssize_t read_bytes;

    if (vs->fd < 0) {
        ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                        errmsg("vector storage is not open")));
    }

    read_bytes = pread(vs->fd, data, len, offset);
    if (read_bytes != (ssize_t)len) {
        ereport(ERROR, (errcode_for_file_access(),
                        errmsg("could not read from vector storage file \"%s\" at offset %lu: %m",
                               vs->path, (unsigned long)offset)));
    }
}

uint64 vector_storage_alloc(VectorStorage vs, size_t len)
{
    uint64 current_size;
    uint64 new_size;
    uint64 alloc_offset;

    current_size = (uint64)vs->nblocks * VECTOR_BLOCK_SIZE;

    if (len > VECTOR_BLOCK_SIZE) {
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                        errmsg("vector size %zu exceeds block size %zu", len, VECTOR_BLOCK_SIZE)));
    }

    alloc_offset = current_size;

    if (current_size + len > (uint64)vs->nblocks * VECTOR_BLOCK_SIZE) {
        new_size = current_size + VECTOR_BLOCK_SIZE;
        if (ftruncate(vs->fd, new_size) < 0) {
            ereport(ERROR, (errcode_for_file_access(),
                            errmsg("could not extend vector storage file \"%s\": %m", vs->path)));
        }
        vs->nblocks++;
    }

    return alloc_offset;
}

uint64 vector_storage_size(VectorStorage vs)
{
    return (uint64)vs->nblocks * VECTOR_BLOCK_SIZE;
}

bool vector_storage_exists(Relation index)
{
    char path[MAXPGPATH];
    struct stat st;
    ProcNumber backend;

    if (index->rd_rel->relpersistence == RELPERSISTENCE_TEMP) {
        backend = index->rd_backend;
    } else {
        backend = INVALID_PROC_NUMBER;
    }

    get_vector_path(index->rd_locator, backend, path, sizeof(path));

    if (stat(path, &st) < 0) {
        if (errno == ENOENT) {
            return false;
        }
        ereport(ERROR, (errcode_for_file_access(),
                        errmsg("could not stat vector storage file \"%s\": %m", path)));
    }

    return S_ISREG(st.st_mode);
}

void vector_storage_delete(Relation index)
{
    char path[MAXPGPATH];
    struct stat st;
    ProcNumber backend;

    if (index->rd_rel->relpersistence == RELPERSISTENCE_TEMP) {
        backend = index->rd_backend;
    } else {
        backend = INVALID_PROC_NUMBER;
    }

    get_vector_path(index->rd_locator, backend, path, sizeof(path));

    if (stat(path, &st) < 0) {
        if (errno == ENOENT) {
            return;
        }
        ereport(ERROR, (errcode_for_file_access(),
                        errmsg("could not stat vector storage file \"%s\": %m", path)));
    }

    if (unlink(path) < 0) {
        ereport(ERROR, (errcode_for_file_access(),
                        errmsg("could not delete vector storage file \"%s\": %m", path)));
    }
}
