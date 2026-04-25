/*
 * graph_index_vacuum.cpp - Graph index vacuum implementation
 */

#include "pg_compat.h"

#include "graph_index/graph_index.h"
#include "graph_index/graph_index_struct.h"
#include "graph_index/graph_index_storage.h"
#include "ann_utils.h"
#include "graph_index/core_node_store_bridge_runtime.hpp"

#include <type_traits>

IndexBulkDeleteResult *graph_index_bulkdelete_internal(Relation index, IndexBulkDeleteResult *stats,
    int nparallel, IndexBulkDeleteCallback callback, void *callback_state, BlockNumber metablkno)
{
    (void)nparallel;
    
    /* Allocate stats if not provided */
    if (!stats) {
        stats = (IndexBulkDeleteResult *)palloc0(sizeof(IndexBulkDeleteResult));
    }

    if (callback == NULL) {
        stats->num_pages = RelationGetNumberOfBlocks(index);
        return stats;
    }

    Buffer metabuf = ReadBuffer(index, metablkno);
    LockBuffer(metabuf, BUFFER_LOCK_SHARE);
    Page metapage = BufferGetPage(metabuf);
    GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(metapage);

    if (metap->magic_number != GRAPH_INDEX_MAGIC_NUMBER) {
        UnlockReleaseBuffer(metabuf);
        return stats;
    }

    LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

    PointExtensionContext ctx(index, GRAPH_INDEX_PS_BLKNO, true);
    DiskStoreVariant disk_store;
    create_disk_store(disk_store, index, NULL, metabuf, NULL, true);

    auto visitor = [&](auto &store) -> void {
        using store_t = std::decay_t<decltype(store)>;

        store.set_vacuum_flag(true);

        UnorderedSet<size_t> deleted(metap->num_vectors > 0 ? metap->num_vectors : 16);
        size_t basepoint_num = store.base_layer.size();
        size_t upperpoint_num = store.upper_layer.size();

        store.remove_heaptids(ctx, deleted, stats, callback, callback_state);
        if (!deleted.empty()) {
            store.mark_deleted(basepoint_num, upperpoint_num);
            pgvexdb::RefreshEntryStateAfterVacuum<store_t>(store, index, metablkno);
        }
        ann_helper::optional_destroy(deleted);
    };
    visit(visitor, disk_store);

    disk_store.destroy();
    ctx.destroy();
    ReleaseBuffer(metabuf);

    stats->num_pages = RelationGetNumberOfBlocks(index);
    return stats;
}

IndexBulkDeleteResult *graph_index_vacuumcleanup_internal(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
    /* If analyze only, nothing to do */
    if (info->analyze_only) {
        return stats;
    }
    
    /* Stats is NULL if ambulkdelete not called */
    if (stats == NULL) {
        stats = (IndexBulkDeleteResult *)palloc0(sizeof(IndexBulkDeleteResult));
    }

    Buffer metabuf = ReadBuffer(info->index, GRAPH_INDEX_METAPAGE_BLKNO);
    LockBuffer(metabuf, BUFFER_LOCK_SHARE);
    GraphIndexMetaPage metap = GRAPH_INDEX_PAGE_GET_META(BufferGetPage(metabuf));
    bool valid = metap->magic_number == GRAPH_INDEX_MAGIC_NUMBER;
    LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

    if (valid) {
        DiskStoreVariant disk_store;
        create_disk_store(disk_store, info->index, NULL, metabuf, NULL, true);
        auto visitor = [&](auto &store) -> void {
            store.set_vacuum_flag(false);
        };
        visit(visitor, disk_store);
        disk_store.destroy();
    }

    ReleaseBuffer(metabuf);
    stats->num_pages = RelationGetNumberOfBlocks(info->index);
    return stats;
}
