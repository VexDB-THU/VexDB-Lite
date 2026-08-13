/**
 * Copyright (c) 2026 VexDB-THU
 *
 * PostgreSQL vector storage backed by standard PostgreSQL pages.
 */

#ifndef VECTOR_PAGE_STORAGE_H
#define VECTOR_PAGE_STORAGE_H

#include <vtl/definition>
#include "distance/core/distance.h"

extern "C" {
#include "storage/buf.h"
#include "storage/bufpage.h"
#include "utils/rel.h"
}

/* Bytes available to vector data in every standard PostgreSQL page. */
static constexpr size_t VEX_VECTOR_PAGE_HEADER_SIZE =
    TYPEALIGN(ann_helper::vector_aligned_size, SizeOfPageHeaderData);
static constexpr size_t VEX_VECTOR_PAGE_PAYLOAD_SIZE =
    BLCKSZ - VEX_VECTOR_PAGE_HEADER_SIZE;

enum VectorReadStatus {
    VECTOR_READ_OK = 0,
    VECTOR_READ_NO_BLOCK,
    VECTOR_READ_CORRUPT,
    VECTOR_READ_COPY_REQUIRED
};

/*
 * Query/backend-local SIMD-aligned scratch memory.  It is deliberately not a
 * cache: PostgreSQL pages stay owned by shared_buffers and a vector spanning
 * pages is copied here only for the duration of one graph operation.
 */
struct VectorScratch {
    char *buf;

    VectorScratch();
    explicit VectorScratch(char *data);
    char *get_vecbuf();
    void release();
};

extern VectorScratch read_vector_scratch(Relation rel, size_t loc,
                                         size_t vector_size,
                                         BlockNumber nblocks = InvalidBlockNumber);
extern void read_vector_into(Relation rel, size_t loc, size_t vector_size,
                             char *buffer,
                             BlockNumber nblocks = InvalidBlockNumber);

/* Logical byte I/O over the page payload area. */
extern VectorReadStatus vector_storage_read(Relation rel, off_t offset,
                                            size_t nbytes, char *buffer,
                                            BlockNumber nblocks = InvalidBlockNumber);
/*
 * Pin one contiguous vector directly in shared_buffers.  On success the
 * returned buffer remains share-locked and pinned until
 * vector_storage_unpin() is called.  Vectors crossing a page boundary return
 * VECTOR_READ_COPY_REQUIRED and must use vector_storage_read().
 */
extern VectorReadStatus vector_storage_pin(Relation rel, off_t offset,
                                           size_t nbytes, BlockNumber nblocks,
                                           Buffer *buffer, const char **data);
extern void vector_storage_unpin(Buffer buffer);
extern void vector_storage_write(Relation rel, off_t offset, size_t nbytes,
                                 const char *buffer, bool need_wal);

extern void create_vector_storage(Relation rel);

#endif /* VECTOR_PAGE_STORAGE_H */
