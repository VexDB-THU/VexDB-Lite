#include "vex/vex_core.h"

#include "vex/vex_graph_algo.hpp"
#include "vex/vex_node_store_memory.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

namespace {

constexpr uint32_t kIndexMagic = 0x56584944; // "VXID"
constexpr uint32_t kIndexVersion = 1;

#pragma pack(push, 1)
struct IndexSnapshotHeader {
    uint32_t magic;
    uint32_t version;
    vex_index_config_t cfg;
    uint8_t has_entry_point;
    uint32_t entry_point;
    int32_t max_level;
    uint64_t graph_node_count;
    uint64_t store_blob_size;
};
#pragma pack(pop)

} // namespace

struct vex_index {
    vex_index_config_t cfg;
    vex::MemoryNodeStore store;
    vex::HNSWGraph graph;

    explicit vex_index(const vex_index_config_t &c)
        : cfg(c),
          store(c.dimension, static_cast<int>(c.m)),
          graph(store, static_cast<vex::Metric>(c.metric), static_cast<int>(c.m),
                static_cast<int>(c.ef_construction)) {
    }
};

extern "C" {

vex_index *vex_index_create(const vex_index_config_t *config) {
    if (config == nullptr || config->dimension == 0 || config->m == 0) {
        return nullptr;
    }
    try {
        return new vex_index(*config);
    } catch (...) {
        return nullptr;
    }
}

void vex_index_destroy(vex_index *idx) {
    delete idx;
}

int vex_index_add(vex_index *idx, int64_t row_id, const float *vec, uint32_t dim) {
    if (idx == nullptr || vec == nullptr) {
        return VEX_ERROR_INVALID_ARGUMENT;
    }
    if (dim != idx->store.GetDimension()) {
        return VEX_ERROR_DIMENSION_MISMATCH;
    }
    try {
        idx->graph.AddPoint(row_id, vec, dim);
        return VEX_OK;
    } catch (...) {
        return VEX_ERROR_INTERNAL;
    }
}

int vex_index_add_batch(vex_index *idx, const int64_t *row_ids, const float *vecs,
                        uint32_t dim, uint32_t count, int threads) {
    (void)threads;
    if (idx == nullptr || row_ids == nullptr || vecs == nullptr) {
        return VEX_ERROR_INVALID_ARGUMENT;
    }
    if (dim != idx->store.GetDimension()) {
        return VEX_ERROR_DIMENSION_MISMATCH;
    }
    for (uint32_t i = 0; i < count; i++) {
        const float *vec = vecs + static_cast<size_t>(i) * dim;
        int rc = vex_index_add(idx, row_ids[i], vec, dim);
        if (rc != VEX_OK) {
            return rc;
        }
    }
    return VEX_OK;
}

int vex_index_search(vex_index *idx, const float *query, uint32_t dim, uint32_t k,
                     int ef, vex_result_t *results, uint32_t *count) {
    if (idx == nullptr || query == nullptr || results == nullptr || count == nullptr) {
        return VEX_ERROR_INVALID_ARGUMENT;
    }
    if (dim != idx->store.GetDimension()) {
        return VEX_ERROR_DIMENSION_MISMATCH;
    }

    try {
        std::vector<vex::row_id_t> row_ids;
        std::vector<float> distances;
        row_ids.reserve(k);
        distances.reserve(k);

        idx->graph.Search(query, k, ef, row_ids, distances);

        uint32_t n = std::min(static_cast<uint32_t>(row_ids.size()), k);
        for (uint32_t i = 0; i < n; i++) {
            results[i].row_id = row_ids[i];
            results[i].distance = distances[i];
        }
        *count = n;
        return VEX_OK;
    } catch (...) {
        *count = 0;
        return VEX_ERROR_INTERNAL;
    }
}

int vex_index_serialize(vex_index *idx, void **data, size_t *size) {
    if (idx == nullptr || data == nullptr || size == nullptr) {
        return VEX_ERROR_INVALID_ARGUMENT;
    }

    try {
        std::vector<uint8_t> store_blob;
        if (!idx->store.Serialize(store_blob)) {
            return VEX_ERROR_INTERNAL;
        }

        IndexSnapshotHeader hdr{};
        hdr.magic = kIndexMagic;
        hdr.version = kIndexVersion;
        hdr.cfg = idx->cfg;
        hdr.has_entry_point = idx->graph.HasEntryPoint() ? 1 : 0;
        hdr.entry_point = idx->graph.EntryPoint();
        hdr.max_level = idx->graph.MaxLevel();
        hdr.graph_node_count = idx->graph.NodeCount();
        hdr.store_blob_size = store_blob.size();

        const size_t total_size = sizeof(IndexSnapshotHeader) + store_blob.size();
        auto *buf = reinterpret_cast<uint8_t *>(::operator new(total_size));

        std::memcpy(buf, &hdr, sizeof(IndexSnapshotHeader));
        if (!store_blob.empty()) {
            std::memcpy(buf + sizeof(IndexSnapshotHeader), store_blob.data(), store_blob.size());
        }

        *data = buf;
        *size = total_size;
        return VEX_OK;
    } catch (...) {
        return VEX_ERROR_INTERNAL;
    }
}

vex_index *vex_index_deserialize(const void *data, size_t size,
                                 const vex_index_config_t *cfg) {
    if (data == nullptr || size < sizeof(IndexSnapshotHeader)) {
        return nullptr;
    }

    const auto *bytes = reinterpret_cast<const uint8_t *>(data);
    IndexSnapshotHeader hdr{};
    std::memcpy(&hdr, bytes, sizeof(IndexSnapshotHeader));

    if (hdr.magic != kIndexMagic || hdr.version != kIndexVersion) {
        return nullptr;
    }

    vex_index_config_t effective_cfg = hdr.cfg;
    if (cfg != nullptr) {
        if (cfg->dimension != hdr.cfg.dimension || cfg->m != hdr.cfg.m ||
            cfg->ef_construction != hdr.cfg.ef_construction || cfg->metric != hdr.cfg.metric) {
            return nullptr;
        }
        effective_cfg = *cfg;
    }

    if (sizeof(IndexSnapshotHeader) + hdr.store_blob_size > size) {
        return nullptr;
    }

    auto *idx = vex_index_create(&effective_cfg);
    if (idx == nullptr) {
        return nullptr;
    }

    const uint8_t *store_blob = bytes + sizeof(IndexSnapshotHeader);
    if (!idx->store.Deserialize(store_blob, static_cast<size_t>(hdr.store_blob_size))) {
        vex_index_destroy(idx);
        return nullptr;
    }

    idx->graph.LoadState(hdr.has_entry_point != 0, hdr.entry_point, hdr.max_level, hdr.graph_node_count);
    return idx;
}

void vex_free_buffer(void *ptr) {
    ::operator delete(ptr);
}

} // extern "C"
