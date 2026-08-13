/*
 * graph_index_vacuum.cpp - Graph index vacuum implementation (single-process)
 */

#include "pg_compat.h"

#include "graph_index/graph_index.h"
#include "graph_index/graph_index_storage.h"
#include "graph_index/graph_index_algorithm.h"
#include "graph_index/graph_index_struct.h"
#include "ann_utils.h"
#include "distance/core/distance_dispatcher.h"

class GraphIndexVacuum {
public:
    GraphIndexVacuum(BlockNumber metablkno, Relation index, IndexBulkDeleteResult *stats,
        IndexBulkDeleteCallback callback, void *callback_state)
        : metabuf(ReadBuffer(index, metablkno)),
          metap(GRAPH_INDEX_PAGE_GET_META(BufferGetPage(metabuf))),
          index(index),
          stats(stats ? stats : (IndexBulkDeleteResult *)palloc0(sizeof(IndexBulkDeleteResult))),
          callback(callback),
          callback_state(callback_state),
          basepoint_num(0),
          upperpoint_num(0)
    {
        if (metap->magic_number != GRAPH_INDEX_MAGIC_NUMBER) {
            UnlockReleaseBuffer(metabuf);
            metabuf = InvalidBuffer;
            return;
        }
        heap = table_open(index->rd_index->indrelid, AccessShareLock);
    }

    void vacuum()
    {
        if (!BufferIsValid(metabuf)) {
            return;
        }

        MemoryContext vacuum_ctx = AllocSetContextCreate(CurrentMemoryContext,
            "GraphIndex vacuum temporary context", ALLOCSET_DEFAULT_SIZES);
        MemoryContext old_ctx = MemoryContextSwitchTo(vacuum_ctx);

        UnorderedSet<size_t> deleted;

        /* Stage 1: Remove heap TIDs */
        size_t remove_elem_num = remove_heaptids(deleted);

        /* Stage 2: Repair graph */
        repair_graph(deleted);
        ann_helper::optional_destroy(deleted);

        /* Stage 3: Mark as deleted */
        mark_deleted();

        MemoryContextSwitchTo(old_ctx);
        MemoryContextDelete(vacuum_ctx);
    }

    void destroy()
    {
        if (BufferIsValid(metabuf)) {
            table_close(heap, AccessShareLock);
            ReleaseBuffer(metabuf);
        }
    }

private:
    Buffer metabuf;
    GraphIndexMetaPage metap;
    Relation index;
    Relation heap;
    IndexBulkDeleteResult *stats;
    IndexBulkDeleteCallback callback;
    void *callback_state;
    size_t basepoint_num;
    size_t upperpoint_num;

    size_t remove_heaptids(UnorderedSet<size_t> &deleted)
    {
        PointExtensionContext ctx{index, GRAPH_INDEX_PS_BLKNO, true};
        DiskStoreVariant disk_store;
        create_disk_store(disk_store, index, heap, metabuf, true);
        size_t remove_elem_num = visit([&](auto &store) -> size_t {
            return store.remove_heaptids(ctx, deleted, stats, callback, callback_state);
        }, disk_store);
        ctx.destroy();
        disk_store.destroy();
        return remove_elem_num;
    }

    template <typename Algo>
    static void repair_base_range(Algo &algo, PointExtensionContext &ctx,
        size_t start, size_t num, const UnorderedSet<size_t> &deleted)
    {
        for (size_t i = 0; i < num; ++i) {
            CHECK_FOR_INTERRUPTS();
            size_t id = start + i;
            if (deleted.contains(id)) {
                continue;
            }
            algo.repair_basepoint(ctx, id, deleted);
        }
    }

    template <typename Algo>
    static void repair_upper_range(Algo &algo, PointExtensionContext &ctx,
        size_t start, size_t num, const UnorderedSet<size_t> &deleted)
    {
        for (size_t i = 0; i < num; ++i) {
            CHECK_FOR_INTERRUPTS();
            size_t cur_layer_idx = start + i;
            algo.repair_upperpoint(ctx, cur_layer_idx, deleted);
        }
    }

    void repair_graph(const UnorderedSet<size_t> &deleted)
    {
        PointExtensionContext ctx{index, GRAPH_INDEX_PS_BLKNO, true};
        DiskStoreVariant disk_store;
        create_disk_store(disk_store, index, heap, metabuf, true);
        auto visitor = [&](auto &store) -> void {
            constexpr DispatcherMode mode = DispatcherMode::DEFAULT;
            DispatchRunner<true,
                MetricList<Metric::L2, Metric::INNER_PRODUCT, Metric::FAST_COSINE>,
                DistPrecisionTypeList<
                    DistPrecisionType::FLOAT
                >, mode>::call(metap, [&](auto &distancer) {
                    distancer.prepare(index, metap);
                    GraphIndexAlgorithm algo{metap, store, distancer};

                    /* Repair entry point if deleted */
                    algo.repair_entry(ctx, deleted);

                    /* Step 2.1: Repair existing points [0, basepoint_num) */
                    GIStateInput input;
                    input.bool_val.val = true;
                    graph_index_get_state(index, GIStateOper::SET_UNDER_VACUUM, input);
                    algo.init_dist_cache();
                    auto [bp_num, up_num, u, u1] = algo.get_repair_info();
                    (void)u; (void)u1;
                    basepoint_num = bp_num;
                    upperpoint_num = up_num;
                    repair_base_range(algo, ctx, 0, basepoint_num, deleted);
                    repair_upper_range(algo, ctx, 0, upperpoint_num, deleted);

                    /* Step 2.2: Exclusive lock, repair final newest points */
                    algo.get_entry_with_exclusive_lock();
                    auto [final_bp_num, final_up_num, u2, u3] = algo.get_repair_info();
                    (void)u2; (void)u3;
                    if (final_bp_num > basepoint_num) {
                        repair_base_range(algo, ctx, basepoint_num,
                            final_bp_num - basepoint_num, deleted);
                    }
                    if (final_up_num > upperpoint_num) {
                        repair_upper_range(algo, ctx, upperpoint_num,
                            final_up_num - upperpoint_num, deleted);
                    }
                    algo.release_exclusive_lock();

                    algo.destroy();
                    input.bool_val.val = false;
                    graph_index_get_state(index, GIStateOper::SET_UNDER_VACUUM, input);
                }
            );
        };
        visit(visitor, disk_store);
        ctx.destroy();
        disk_store.destroy();
    }

    void mark_deleted()
    {
        DiskStoreVariant disk_store;
        create_disk_store(disk_store, index, heap, metabuf, true);
        visit([&](auto &store) -> void {
            store.mark_deleted(basepoint_num, upperpoint_num);
        }, disk_store);
        disk_store.destroy();
    }
};

IndexBulkDeleteResult *graph_index_bulkdelete_internal(Relation index, IndexBulkDeleteResult *stats,
    int nparallel, IndexBulkDeleteCallback callback, void *callback_state, BlockNumber metablkno)
{
    (void)nparallel;

    GraphIndexVacuum vacuum{metablkno, index, stats, callback, callback_state};
    vacuum.vacuum();
    vacuum.destroy();
    FreeSpaceMapVacuum(index);
    return stats;
}

IndexBulkDeleteResult *graph_index_vacuumcleanup_internal(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
    if (info->analyze_only) {
        return stats;
    }
    if (stats == NULL) {
        return NULL;
    }
    stats->num_pages = RelationGetNumberOfBlocks(info->index);
    return stats;
}
