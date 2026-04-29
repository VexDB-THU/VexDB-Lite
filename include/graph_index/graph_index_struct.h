/**
 * Graph Index Structures
 */
#ifndef GRAPH_INDEX_STRUCT_H
#define GRAPH_INDEX_STRUCT_H

#include "pg_compat.h"
#include "storage/block.h"
#include "storage/itemptr.h"

#include "graph_index/graph_index_param.h"
#include "graph_index_stats.h"
#include "distance/core/distance.h"
#include "quantizer.h"

enum class IdType : uint8 { U32 = 0, U64 };

struct GraphIndexMetaPageData {
    uint32 magic_number;
    uint32 version;
    uint16 dimension;
    uint16 m;
    uint16 ef_construction;
    Metric metric;
    DistPrecisionType precision_type;
    IdType id_type;
    uint32 num_cluster;
    BlockNumber cluster_block;
    BlockNumber base_block;
    BlockNumber upper_block;
    BlockNumber elems_block;
    BlockNumber qtcode_block;
    BlockNumber free_id_list_block;
    BlockNumber free_upper_list_block;
    BlockNumber async_id_list_block;
    int8 entry_level;
    size_t entry_cur_layer_idx;
    size_t entrypoint_id;
    size_t num_vectors;
    GraphIndexStatsData stats;
    bool vacuum_flag;
    QuantizerMetaInfo quantizer_metainfo;
};
using GraphIndexMetaPage = GraphIndexMetaPageData *;

struct GraphIndexPageOpaqueData {
    BlockNumber nextblkno;
    uint16 unused;
    uint16 page_id;
};
using GraphIndexPageOpaque = GraphIndexPageOpaqueData *;

template <typename T>
struct GraphIndexCandidate {
    T id;
    T cur_layer_idx;
    T lower_layer_idx;
    float dist;
    const char *val;
    GraphIndexCandidate() : id((T)INVALID_VECTOR_ID), cur_layer_idx((T)INVALID_VECTOR_ID), lower_layer_idx((T)INVALID_VECTOR_ID), dist(INVALID_DIST), val(nullptr) {}
    GraphIndexCandidate(T id_val, T cur_idxx, float dist_val) : id(id_val), cur_layer_idx(cur_idxx), lower_layer_idx((T)INVALID_VECTOR_ID), dist(dist_val), val(nullptr) {}
    GraphIndexCandidate(T id_val, T cur_idxx, T lower_idx, float dist_val, const char *val_ptr)
        : id(id_val), cur_layer_idx(cur_idxx), lower_layer_idx(lower_idx), dist(dist_val), val(val_ptr) {}
};

struct GraphIndexEntryInfo {
    size_t id;
    size_t cur_layer_idx;
    int_fast8_t level;
    GraphIndexEntryInfo() : id((size_t)INVALID_VECTOR_ID), cur_layer_idx((size_t)INVALID_VECTOR_ID), level(-1) {}
    GraphIndexEntryInfo(size_t id, size_t cur_layer_idx, int_fast8_t l)
        : id(id), cur_layer_idx(cur_layer_idx), level(l) {}
    GraphIndexEntryInfo(const GraphIndexEntryInfo &other) = default;
    GraphIndexEntryInfo &operator=(const GraphIndexEntryInfo &other) = default;
    void set(size_t id, size_t cur_layer_idx, int_fast8_t level)
    {
        this->id = id;
        this->cur_layer_idx = cur_layer_idx;
        this->level = level;
    }
};

struct GraphIndexOptions {
    int32 vl_len_;
    int m;
    int ef_construction;
    int parallel_workers;
    int qt_type_offset;
    int cluster_rate;
    bool enable_async_insert;
};

struct RepairGraphSharedState {
    Relation index;
    Relation heap;
    BlockNumber metablkno;
    void *deleted;
    std::atomic<size_t> *base_counter;
    std::atomic<size_t> *upper_counter;
    size_t basepoint_num;
    size_t upperpoint_num;
    size_t base_batch_size;
    size_t upper_batch_size;
};

template <typename T>
struct GraphIndexDiskBasePoint {
    T neighbors_id[1];
    void init(uint16 m)
    {
        for (uint16 i = 0; i < m * 2; ++i) {
            neighbors_id[i] = (T)INVALID_VECTOR_ID;
        }
    }
};

template <typename T>
struct GraphIndexDiskUpperPoint {
    T lower_layer_idx;
    T id;
    T neighbors_info[1];
    void init(uint16 m)
    {
        T invalid = (T)INVALID_VECTOR_ID;
        lower_layer_idx = invalid;
        id = invalid;
        for (uint16 i = 0; i < m * 2; ++i) {
            neighbors_info[i] = invalid;
        }
    }
};

/* DiskStoreVariant is defined in graph_index_storage.h */

#endif
