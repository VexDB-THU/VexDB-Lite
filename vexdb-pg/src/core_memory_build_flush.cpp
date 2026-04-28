#include "graph_index/core_memory_build_flush.hpp"

#include <vector>

#include <vtl/disk_container/diskvector.hpp>

#include "graph_index/graph_index_cluster.h"
#include "graph_index/graph_index_storage.h"
#include "module/timer.h"
#include "vex/vex_adapter_graph_state.hpp"

using namespace disk_container;

namespace pgvexdb {

void FlushCoreMemoryBuildToDisk(Relation index, Buffer metabuf, GraphIndexMetaPage metap,
                                uint_fast16_t dimension, uint_fast16_t m, size_t vector_size,
                                const CoreMemoryBuildFlushInput &input)
{
    ann_helper::Timer flush_timer{0, 500'000, "", ""};
    flush_timer.set_stage("Flush Graph Index");

    auto &store = input.node_store;
    vex::AdapterGraphState state = vex::CaptureGraphState(input.core_graph);
    const uint32 num_vectors = static_cast<uint32>(state.node_count);
    metap->num_vectors = static_cast<size_t>(num_vectors);

    std::vector<uint32> upper_slot_by_node(num_vectors * vex::HNSW_MAX_UPPER_LEVELS,
                                           static_cast<uint32>(INVALID_VECTOR_ID));
    uint32 next_upper_slot = 0;
    for (uint32 node_id = 0; node_id < num_vectors; ++node_id) {
        auto handle = store.PinNode(node_id);
        if (!handle || !handle->Header() || handle->Header()->deleted) {
            continue;
        }
        const uint8_t level = handle->Header()->level;
        for (uint8_t level_idx = 0; level_idx < level; ++level_idx) {
            upper_slot_by_node[static_cast<size_t>(node_id) * vex::HNSW_MAX_UPPER_LEVELS + level_idx] =
                next_upper_slot++;
        }
    }

    metap->entry_level = state.has_entry_point ? static_cast<int8>(state.max_level) : -1;
    metap->entrypoint_id = state.has_entry_point
        ? static_cast<size_t>(state.entry_point)
        : static_cast<size_t>(INVALID_VECTOR_ID);
    if (!state.has_entry_point || state.max_level <= 0) {
        metap->entry_cur_layer_idx = state.has_entry_point
            ? static_cast<size_t>(state.entry_point)
            : static_cast<size_t>(INVALID_VECTOR_ID);
    } else {
        metap->entry_cur_layer_idx = upper_slot_by_node[
            static_cast<size_t>(state.entry_point) * vex::HNSW_MAX_UPPER_LEVELS +
            static_cast<size_t>(state.max_level - 1)];
    }

    if (metap->num_vectors > 0) {
        flush_timer.report("Flushing Elems");
        PointExtensionContext elem_ctx(index, GRAPH_INDEX_PS_BLKNO, false);
        DiskVector<GraphIndexPoint> dv{index, metap->elems_block, false};
        std::vector<GraphIndexPoint> batch;
        batch.reserve(1024);
        for (uint32 node_id = 0; node_id < num_vectors; ++node_id) {
            GraphIndexPoint::Data tid = input.heap_tids_by_node_id[node_id];
            batch.emplace_back(elem_ctx, Span<const GraphIndexPoint::Data>(&tid, 1));
            if (batch.size() >= 1024) {
                dv.push_back_n(batch.data(), batch.size());
                batch.clear();
            }
        }
        if (!batch.empty()) {
            dv.push_back_n(batch.data(), batch.size());
        }
        dv.destroy();
        elem_ctx.destroy();

        flush_timer.report("Flushing Basepoint");
        constexpr size_t copybuf_size = 10 * 1024 * 1024;
        uint32 *copybuf = (uint32 *)palloc(copybuf_size);
        size_t basepoint_size = sizeof(uint32) * m * 2;
        size_t copy_num = copybuf_size / basepoint_size;
        VarDiskVector<GraphIndexDiskBasePoint<uint32>> base_layer{index, metap->base_block, false, basepoint_size};
        for (uint32 i = 0; i <= num_vectors / copy_num; ++i) {
            size_t batch_offset = static_cast<size_t>(i) * copy_num;
            size_t actual_copy_num = Min(copy_num, static_cast<size_t>(num_vectors) - batch_offset);
            if (actual_copy_num == 0) {
                break;
            }
            for (size_t j = 0; j < actual_copy_num; ++j) {
                auto handle = store.PinNode(static_cast<uint32>(batch_offset + j));
                const vex::node_id_t *src = handle->Level0Neighbors();
                for (size_t k = 0; k < static_cast<size_t>(m) * 2; ++k) {
                    copybuf[j * (m * 2) + k] = src[k] == vex::INVALID_NODE_ID
                        ? static_cast<uint32>(INVALID_VECTOR_ID)
                        : static_cast<uint32>(src[k]);
                }
            }
            base_layer.push_back_n((const GraphIndexDiskBasePoint<uint32> *)copybuf, actual_copy_num);
        }
        base_layer.destroy();

        flush_timer.report("Flushing Upperpoint");
        size_t upperpoint_size = (m + 1) * 2 * sizeof(uint32);
        VarDiskVector<GraphIndexDiskUpperPoint<uint32>> upper_layer{index, metap->upper_block, false,
                                                                    upperpoint_size};
        std::vector<uint32> upper_rows;
        upper_rows.reserve(static_cast<size_t>(next_upper_slot) * (2 + m * 2));
        for (uint32 node_id = 0; node_id < num_vectors; ++node_id) {
            auto handle = store.PinNode(node_id);
            if (!handle || !handle->Header() || handle->Header()->deleted) {
                continue;
            }
            uint32 lower_layer_idx = node_id;
            for (uint8_t level_idx = 0; level_idx < handle->Header()->level; ++level_idx) {
                upper_rows.push_back(lower_layer_idx);
                upper_rows.push_back(node_id);
                const vex::node_id_t *neighbors = handle->UpperNeighbors(level_idx);
                for (int k = 0; k < m; ++k) {
                    vex::node_id_t nbr = neighbors[k];
                    upper_rows.push_back(nbr == vex::INVALID_NODE_ID
                        ? static_cast<uint32>(INVALID_VECTOR_ID)
                        : static_cast<uint32>(nbr));
                }
                for (int k = 0; k < m; ++k) {
                    vex::node_id_t nbr = neighbors[k];
                    if (nbr == vex::INVALID_NODE_ID) {
                        upper_rows.push_back(static_cast<uint32>(INVALID_VECTOR_ID));
                        continue;
                    }
                    uint32 slot = upper_slot_by_node[static_cast<size_t>(nbr) *
                                                      vex::HNSW_MAX_UPPER_LEVELS + level_idx];
                    upper_rows.push_back(slot);
                }
                lower_layer_idx = upper_slot_by_node[static_cast<size_t>(node_id) *
                                                     vex::HNSW_MAX_UPPER_LEVELS + level_idx];
            }
        }
        if (!upper_rows.empty()) {
            upper_layer.push_back_n((const GraphIndexDiskUpperPoint<uint32> *)upper_rows.data(),
                                    upper_rows.size() / (2 + m * 2));
        }
        upper_layer.destroy();
        pfree(copybuf);
    }

    flush_timer.report("Flushing Vector");
    elog(LOG, "pg memory flush: create_vec_data begin");
    ereport(NOTICE, (errmsg("PG memory flush stage"), errdetail("stage=create_vec_data_begin")));
    create_vec_data(index, true);
    elog(LOG, "pg memory flush: create_vec_data done");
    ereport(NOTICE, (errmsg("PG memory flush stage"), errdetail("stage=create_vec_data_done")));
    constexpr size_t batch_rows = 1024;
    std::vector<float> vector_batch(static_cast<size_t>(batch_rows) * dimension);
    for (uint32 batch_offset = 0; batch_offset < num_vectors; batch_offset += batch_rows) {
        size_t actual_copy_num = Min(static_cast<size_t>(batch_rows),
                                     static_cast<size_t>(num_vectors - batch_offset));
        for (size_t j = 0; j < actual_copy_num; ++j) {
            auto handle = store.PinNode(batch_offset + static_cast<uint32>(j));
            memcpy(vector_batch.data() + j * dimension, handle->Vector(), sizeof(float) * dimension);
        }
        off_t offset = static_cast<off_t>(batch_offset) * vector_size;
        int nbytes = static_cast<int>(actual_copy_num * vector_size);
        vec_write(index->rd_smgr, offset, nbytes, reinterpret_cast<const char *>(vector_batch.data()),
                  false, VecStorageType::PureVec);
    }
    elog(LOG, "pg memory flush: vector flush done");
    ereport(NOTICE, (errmsg("PG memory flush stage"), errdetail("stage=vector_flush_done")));

    flush_timer.report("Flush Finished");
    elog(LOG, "pg memory flush: mark metapage begin");
    ereport(NOTICE, (errmsg("PG memory flush stage"), errdetail("stage=mark_metapage_begin")));
    LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);
    MarkBufferDirty(metabuf);
    LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);
    elog(LOG, "pg memory flush: mark metapage done");
    ereport(NOTICE, (errmsg("PG memory flush stage"), errdetail("stage=mark_metapage_done")));
    flush_timer.destroy();
}

} // namespace pgvexdb
