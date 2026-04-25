#ifndef DISK_CONTAINER_XLOG_H
#define DISK_CONTAINER_XLOG_H

#include "postgres.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "storage/bufmgr.h"
#include <vtl/disk_container/blockmgr.hpp>

namespace disk_container {

class DiskVectorLogger {
    using PageData = disk_container::PageData;
public:
    struct add_elem {
        uint32 offset;
        size_t elem_size;
    };
    struct set_elem_partial {
        uint32 offset;
        uint16 partial_size;
    };
    struct update_start_npages {
        uint32 num_pages;
        BlockNumber start_blkno;
    };
    struct inplace_filter_add_data {
        uint32 offset;
        size_t data_size;
    };
    struct inplace_filter_add_item {
        size_t size;
        OffsetNumber offset;
        bool isOverWrite;
    };

    Relation _rel;
    explicit DiskVectorLogger(Relation rel) : _rel(rel) {}
    
    void xl_add_elem(PageData &page_data, char *elem_data, size_t elem_size, uint32 offset) {}
    void xl_set_elem_partial(PageData &page_data, char *partial_data, uint16 partial_size, uint32 offset) {}
    void xl_extend_newpages(BlockNumber start_blkno, BlockNumber end_blkno) {}
    void xl_update_meta_start_npages(PageData &meta_buf, uint32 npage, BlockNumber start_blkno) {}
    void xl_update_meta_nitem(PageData &meta_buf, size_t nitem) {}
    void xl_inplace_filter_add_data(Buffer buf, Page page, uint32 offset, size_t data_size) {}
    void xl_inplace_filter_add_item(Buffer buf, Page page, char *item, size_t size, OffsetNumber offset, bool isOverWrite) {}
    void xl_inplace_filter_delete_item(Buffer buf, Page page, OffsetNumber offset) {}
    void xl_inplace_filter_multi_delete(Buffer buf, Page page, OffsetNumber *offsets, size_t n) {}
};

}; /* namespace disk_container */

#endif /* DISK_CONTAINER_XLOG_H */
