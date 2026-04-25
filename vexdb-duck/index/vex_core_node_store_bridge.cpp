#include "vex_core_node_store_bridge.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/execution/index/bound_index.hpp"
#include "vex_hnsw_node.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace duckdb {

namespace {

struct DuckDBPinnedNodeToken {
    explicit DuckDBPinnedNodeToken(SegmentHandle node_handle_p, SegmentHandle vector_handle_p, bool writable_p)
        : node_handle(std::move(node_handle_p)),
          vector_handle(std::move(vector_handle_p)),
          writable(writable_p) {
        upper_counts.fill(0);
    }

    SegmentHandle node_handle;
    SegmentHandle vector_handle;
    std::unique_ptr<SegmentHandle> upper_handle;
    std::unique_ptr<SegmentHandle> meta_handle;

    ::vex::NodeHeader header{};
    std::vector<::vex::node_id_t> level0_neighbors;
    std::vector<::vex::node_id_t> upper_neighbors;
    std::array<uint16_t, ::vex::HNSW_MAX_UPPER_LEVELS> upper_counts {};
    uint8_t *metadata = nullptr;
    bool writable = false;
};

static bool PointerFitsNodeId(uint64_t raw_ptr) {
    return raw_ptr <= static_cast<uint64_t>(std::numeric_limits<::vex::node_id_t>::max());
}

static bool TranslateDuckPointerToNodeId(
    const unordered_map<idx_t, ::vex::node_id_t> &ptr_to_node_id, const IndexPointer &ptr, ::vex::node_id_t &out) {
    if (!ptr.Get()) {
        out = ::vex::INVALID_NODE_ID;
        return true;
    }
    auto it = ptr_to_node_id.find(ptr.Get());
    if (it == ptr_to_node_id.end()) {
        return false;
    }
    out = it->second;
    return true;
}

static bool TranslateNodeIdToDuckPointer(const std::vector<IndexPointer> &node_id_to_ptr, ::vex::node_id_t node_id,
                                         IndexPointer &out) {
    if (node_id == ::vex::INVALID_NODE_ID) {
        out = IndexPointer();
        return true;
    }
    if (node_id >= node_id_to_ptr.size()) {
        return false;
    }
    out = node_id_to_ptr[node_id];
    return out.Get() != 0;
}

static bool TryBootstrapDuckDBGraphStateFromStorage(const DuckDBCoreBindingConfig &cfg,
                                                    const unordered_map<idx_t, ::vex::node_id_t> &ptr_to_node_id,
                                                    DuckDBCoreGraphState &state) {
    if (!cfg.storage_info) {
        return false;
    }

    DuckDBStorageGraphState raw{};
    if (!LoadDuckDBRawGraphStateFromStorage(*cfg.storage_info, raw)) {
        return false;
    }

    state.has_entry_point = false;
    state.entry_point = ::vex::INVALID_NODE_ID;
    state.max_level = raw.max_level;
    state.node_count = raw.node_count;

    if (!raw.has_entry_point || raw.entry_point_raw == 0) {
        return true;
    }

    IndexPointer raw_ptr;
    raw_ptr.Set(raw.entry_point_raw);
    ::vex::node_id_t entry_point = ::vex::INVALID_NODE_ID;
    if (!TranslateDuckPointerToNodeId(ptr_to_node_id, raw_ptr, entry_point)) {
        return false;
    }

    state.has_entry_point = true;
    state.entry_point = entry_point;
    return true;
}

static bool TryStoreDuckDBGraphStateToStorage(const DuckDBCoreBindingConfig &cfg, const std::vector<IndexPointer> &node_id_to_ptr,
                                              const DuckDBCoreGraphState &state) {
    if (!cfg.storage_info) {
        return false;
    }

    DuckDBStorageGraphState raw{};
    raw.has_entry_point = state.has_entry_point;
    raw.max_level = state.max_level;
    raw.node_count = state.node_count;
    if (state.has_entry_point) {
        if (state.entry_point == ::vex::INVALID_NODE_ID || state.entry_point >= node_id_to_ptr.size()) {
            return false;
        }
        raw.entry_point_raw = node_id_to_ptr[state.entry_point].Get();
    }
    return StoreDuckDBRawGraphStateToStorage(*cfg.storage_info, raw);
}

static bool BuildDuckDBNodeIdMapping(const DuckDBCoreBindingConfig &cfg, std::vector<IndexPointer> &node_id_to_ptr,
                                     unordered_map<idx_t, ::vex::node_id_t> &ptr_to_node_id) {
    node_id_to_ptr.clear();
    ptr_to_node_id.clear();

    auto *row_id_map = cfg.live_storage.row_id_map;
    if (!row_id_map) {
        return false;
    }

    vector<idx_t> raw_ptrs;
    raw_ptrs.reserve(row_id_map->size());
    unordered_set<idx_t> seen;
    for (const auto &entry : *row_id_map) {
        const auto raw_ptr = entry.second.Get();
        if (!raw_ptr || !seen.insert(raw_ptr).second) {
            continue;
        }
        raw_ptrs.push_back(raw_ptr);
    }

    std::sort(raw_ptrs.begin(), raw_ptrs.end());
    node_id_to_ptr.reserve(raw_ptrs.size());
    ptr_to_node_id.reserve(raw_ptrs.size());
    for (idx_t i = 0; i < raw_ptrs.size(); i++) {
        if (i > static_cast<idx_t>(std::numeric_limits<::vex::node_id_t>::max())) {
            throw NotImplementedException("DuckDB core bridge: node id space exceeds libvex-core 32-bit node_id_t");
        }
        IndexPointer ptr;
        ptr.Set(raw_ptrs[i]);
        auto node_id = static_cast<::vex::node_id_t>(i);
        node_id_to_ptr.push_back(ptr);
        ptr_to_node_id.emplace(raw_ptrs[i], node_id);
    }
    return !node_id_to_ptr.empty();
}

} // namespace

static bool TryBootstrapDuckDBGraphState(const DuckDBCoreBindingConfig &cfg, DuckDBCoreGraphState &state) {
    if (!cfg.load_graph_state_cb) {
        return false;
    }

    DuckDBCoreGraphState loaded{};
    if (!cfg.load_graph_state_cb(loaded)) {
        return false;
    }
    state = loaded;
    return true;
}

bool LoadDuckDBRawGraphStateFromStorage(const IndexStorageInfo &storage, DuckDBStorageGraphState &out) {
    auto nc_it = storage.options.find("node_count");
    auto ml_it = storage.options.find("max_level");
    if (nc_it == storage.options.end() || ml_it == storage.options.end()) {
        return false;
    }

    out.node_count = nc_it->second.GetValue<uint64_t>();
    out.max_level = ml_it->second.GetValue<int>();
    out.entry_point_raw = storage.root;
    out.has_entry_point = (out.node_count > 0 && out.entry_point_raw != 0);
    return true;
}

bool StoreDuckDBRawGraphStateToStorage(IndexStorageInfo &storage, const DuckDBStorageGraphState &in) {
    storage.options["node_count"] = Value::UBIGINT(in.node_count);
    storage.options["max_level"] = Value::INTEGER(in.max_level);
    storage.root = in.has_entry_point ? in.entry_point_raw : 0;
    return true;
}

bool LoadDuckDBGraphStateFromStorage(const IndexStorageInfo &storage, DuckDBCoreGraphState &out) {
    DuckDBStorageGraphState raw{};
    if (!LoadDuckDBRawGraphStateFromStorage(storage, raw)) {
        return false;
    }
    if (raw.has_entry_point && !PointerFitsNodeId(raw.entry_point_raw)) {
        return false;
    }

    out.node_count = raw.node_count;
    out.max_level = raw.max_level;
    out.entry_point = raw.has_entry_point
        ? static_cast<::vex::node_id_t>(raw.entry_point_raw)
        : ::vex::INVALID_NODE_ID;
    out.has_entry_point = raw.has_entry_point;
    return true;
}

bool StoreDuckDBGraphStateToStorage(IndexStorageInfo &storage, const DuckDBCoreGraphState &in) {
    DuckDBStorageGraphState raw{};
    raw.has_entry_point = in.has_entry_point;
    raw.entry_point_raw = in.has_entry_point ? static_cast<uint64_t>(in.entry_point) : 0;
    raw.max_level = in.max_level;
    raw.node_count = in.node_count;
    return StoreDuckDBRawGraphStateToStorage(storage, raw);
}

void BindDuckDBGraphStateStorage(DuckDBCoreBindingConfig &cfg, IndexStorageInfo &storage) {
    cfg.storage_info = &storage;
    if (!cfg.load_graph_state_cb) {
        cfg.load_graph_state_cb = [&storage](DuckDBCoreGraphState &out) {
            return LoadDuckDBGraphStateFromStorage(storage, out);
        };
    }
    if (!cfg.store_graph_state_cb) {
        cfg.store_graph_state_cb = [&storage](const DuckDBCoreGraphState &in) {
            return StoreDuckDBGraphStateToStorage(storage, in);
        };
    }
}

void BindDuckDBLiveStorage(DuckDBCoreBindingConfig &cfg, FixedSizeAllocator &node_alloc,
                           FixedSizeAllocator &vector_alloc, FixedSizeAllocator &upper_alloc,
                           FixedSizeAllocator *meta_alloc, unordered_map<row_t, IndexPointer> &row_id_map) {
    cfg.live_storage.node_alloc = &node_alloc;
    cfg.live_storage.vector_alloc = &vector_alloc;
    cfg.live_storage.upper_alloc = &upper_alloc;
    cfg.live_storage.meta_alloc = meta_alloc;
    cfg.live_storage.row_id_map = &row_id_map;
}

DuckDBCoreLowLevelBindingSkeleton::DuckDBCoreLowLevelBindingSkeleton(const DuckDBCoreBindingConfig &cfg)
    : cfg_(cfg) {
    if (cfg_.storage_info) {
        BindDuckDBGraphStateStorage(cfg_, *cfg_.storage_info);
    }
    (void)BuildDuckDBNodeIdMapping(cfg_, node_id_to_ptr_, ptr_to_node_id_);
    active_node_count_ = static_cast<uint64_t>(node_id_to_ptr_.size());
    if (!TryBootstrapDuckDBGraphStateFromStorage(cfg_, ptr_to_node_id_, state_)) {
        (void)TryBootstrapDuckDBGraphState(cfg_, state_);
    }
}

::vex::node_id_t DuckDBCoreLowLevelBindingSkeleton::AllocateNode(::vex::row_id_t row_id, const float *vec, uint32_t dim,
                                                                 uint8_t level) {
    if (!cfg_.live_storage.node_alloc || !cfg_.live_storage.vector_alloc || !cfg_.live_storage.upper_alloc ||
        !cfg_.live_storage.row_id_map) {
        throw InternalException("DuckDB core direct binding: live allocators are not available");
    }
    if (vec == nullptr) {
        throw InternalException("DuckDB core direct binding: AllocateNode received null vector");
    }
    if (dim != cfg_.dimension) {
        throw InternalException("DuckDB core direct binding: vector dimension mismatch");
    }

    auto node_ptr = cfg_.live_storage.node_alloc->New();
    auto node_handle = cfg_.live_storage.node_alloc->GetHandle(node_ptr);
    auto *header = node_handle.GetPtr<vex::HNSWNodeHeader>();
    std::memset(header, 0, vex::HNSWNodeHeader::SegmentSize(cfg_.m));

    auto vec_ptr = cfg_.live_storage.vector_alloc->New();
    auto vec_handle = cfg_.live_storage.vector_alloc->GetHandle(vec_ptr);
    auto *vec_dst = vec_handle.GetPtr<float>();
    std::memcpy(vec_dst, vec, static_cast<size_t>(dim) * sizeof(float));

    header->row_id = row_id;
    header->level = level;
    header->deleted = 0;
    header->level0_count = 0;
    header->extra_row_count = 0;
    header->reserved = 0;
    header->vector_ptr = vec_ptr;
    header->upper_ptr = IndexPointer();
    header->metadata_ptr = IndexPointer();

    if (level > 0) {
        auto upper_ptr = cfg_.live_storage.upper_alloc->New();
        auto upper_handle = cfg_.live_storage.upper_alloc->GetHandle(upper_ptr);
        auto *upper = upper_handle.GetPtr<vex::HNSWUpperLevel>();
        std::memset(upper, 0, vex::HNSWUpperLevel::SegmentSize(cfg_.m));
        header->upper_ptr = upper_ptr;
    }

    if (cfg_.metadata_size > 0 && cfg_.live_storage.meta_alloc) {
        auto meta_ptr = cfg_.live_storage.meta_alloc->New();
        auto meta_handle = cfg_.live_storage.meta_alloc->GetHandle(meta_ptr);
        std::memset(meta_handle.GetPtr(), 0, cfg_.metadata_size);
        header->metadata_ptr = meta_ptr;
    }

    cfg_.live_storage.row_id_map->operator[](static_cast<row_t>(row_id)) = node_ptr;

    if (node_id_to_ptr_.size() > static_cast<size_t>(std::numeric_limits<::vex::node_id_t>::max())) {
        throw NotImplementedException("DuckDB core bridge: node id space exceeds libvex-core 32-bit node_id_t");
    }
    auto node_id = static_cast<::vex::node_id_t>(node_id_to_ptr_.size());
    node_id_to_ptr_.push_back(node_ptr);
    ptr_to_node_id_[node_ptr.Get()] = node_id;
    active_node_count_++;
    state_.node_count = active_node_count_;
    return node_id;
}

void DuckDBCoreLowLevelBindingSkeleton::FreeNode(::vex::node_id_t node_id) {
    if (!cfg_.live_storage.node_alloc || !cfg_.live_storage.vector_alloc || !cfg_.live_storage.upper_alloc ||
        node_id >= node_id_to_ptr_.size()) {
        return;
    }

    auto node_ptr = node_id_to_ptr_[node_id];
    if (!node_ptr.Get()) {
        return;
    }

    auto node_handle = cfg_.live_storage.node_alloc->GetHandle(node_ptr);
    auto *header = node_handle.GetPtr<vex::HNSWNodeHeader>();
    const auto row_id = header->row_id;
    const auto vector_ptr = header->vector_ptr;
    const auto upper_ptr = header->upper_ptr;
    const auto metadata_ptr = header->metadata_ptr;

    if (vector_ptr.Get()) {
        cfg_.live_storage.vector_alloc->Free(vector_ptr);
    }
    if (upper_ptr.Get()) {
        cfg_.live_storage.upper_alloc->Free(upper_ptr);
    }
    if (metadata_ptr.Get() && cfg_.live_storage.meta_alloc) {
        cfg_.live_storage.meta_alloc->Free(metadata_ptr);
    }
    cfg_.live_storage.node_alloc->Free(node_ptr);

    if (cfg_.live_storage.row_id_map) {
        auto it = cfg_.live_storage.row_id_map->find(row_id);
        if (it != cfg_.live_storage.row_id_map->end() && it->second.Get() == node_ptr.Get()) {
            cfg_.live_storage.row_id_map->erase(it);
        } else {
            for (auto rid_it = cfg_.live_storage.row_id_map->begin(); rid_it != cfg_.live_storage.row_id_map->end();) {
                if (rid_it->second.Get() == node_ptr.Get()) {
                    rid_it = cfg_.live_storage.row_id_map->erase(rid_it);
                } else {
                    ++rid_it;
                }
            }
        }
    }

    ptr_to_node_id_.erase(node_ptr.Get());
    node_id_to_ptr_[node_id] = IndexPointer();
    if (active_node_count_ > 0) {
        active_node_count_--;
    }
    state_.node_count = active_node_count_;
}

bool DuckDBCoreLowLevelBindingSkeleton::PinNode(::vex::node_id_t node_id, bool for_update,
                                                ::vex::DuckDBNodeLayoutView &out) {
    out = {};
    if (!cfg_.live_storage.node_alloc || !cfg_.live_storage.vector_alloc || !cfg_.live_storage.upper_alloc) {
        return false;
    }
    if (node_id >= node_id_to_ptr_.size()) {
        return false;
    }

    auto node_ptr = node_id_to_ptr_[node_id];
    if (!node_ptr.Get()) {
        return false;
    }
    auto node_handle = cfg_.live_storage.node_alloc->GetHandle(node_ptr);
    auto *storage_header = node_handle.GetPtr<vex::HNSWNodeHeader>();
    if (!storage_header) {
        return false;
    }
    if (!storage_header->vector_ptr.Get()) {
        return false;
    }

    auto vector_handle = cfg_.live_storage.vector_alloc->GetHandle(storage_header->vector_ptr);
    auto token = std::unique_ptr<DuckDBPinnedNodeToken>(
        new DuckDBPinnedNodeToken(std::move(node_handle), std::move(vector_handle), for_update));

    token->header.row_id = storage_header->row_id;
    token->header.level = storage_header->level;
    token->header.deleted = storage_header->deleted;
    token->header.level0_count = storage_header->level0_count;
    token->header.extra_row_count = storage_header->extra_row_count;
    token->header.reserved = storage_header->reserved;
    token->header.upper_offset = 0;
    token->header.metadata_offset = 0;

    token->level0_neighbors.resize(static_cast<size_t>(cfg_.m) * 2, ::vex::INVALID_NODE_ID);
    auto *level0_src = storage_header->GetLevel0Neighbors();
    auto level0_count = std::min<uint16_t>(storage_header->level0_count,
                                           static_cast<uint16_t>(token->level0_neighbors.size()));
    token->header.level0_count = level0_count;
    for (uint16_t i = 0; i < level0_count; i++) {
        auto neighbor_ptr = level0_src[i];
        ::vex::node_id_t neighbor_id = ::vex::INVALID_NODE_ID;
        if (TranslateDuckPointerToNodeId(ptr_to_node_id_, neighbor_ptr, neighbor_id)) {
            token->level0_neighbors[i] = neighbor_id;
        }
    }

    if (storage_header->upper_ptr.Get()) {
        auto upper_handle = cfg_.live_storage.upper_alloc->GetHandle(storage_header->upper_ptr);
        auto *upper = upper_handle.GetPtr<vex::HNSWUpperLevel>();
        token->upper_handle = std::unique_ptr<SegmentHandle>(new SegmentHandle(std::move(upper_handle)));
        token->upper_neighbors.resize(static_cast<size_t>(::vex::HNSW_MAX_UPPER_LEVELS) *
                                      static_cast<size_t>(cfg_.m), ::vex::INVALID_NODE_ID);

        for (int level_idx = 0; level_idx < ::vex::HNSW_MAX_UPPER_LEVELS; level_idx++) {
            auto count = std::min<uint16_t>(upper->counts[level_idx], static_cast<uint16_t>(cfg_.m));
            token->upper_counts[static_cast<size_t>(level_idx)] = count;
            auto *src_neighbors = upper->GetNeighbors(level_idx, cfg_.m);
            auto *dst_neighbors = token->upper_neighbors.data() + static_cast<size_t>(level_idx) * static_cast<size_t>(cfg_.m);
            for (uint16_t i = 0; i < count; i++) {
                ::vex::node_id_t neighbor_id = ::vex::INVALID_NODE_ID;
                if (TranslateDuckPointerToNodeId(ptr_to_node_id_, src_neighbors[i], neighbor_id)) {
                    dst_neighbors[i] = neighbor_id;
                }
            }
        }
    }

    if (storage_header->metadata_ptr.Get() && cfg_.live_storage.meta_alloc) {
        auto meta_handle = cfg_.live_storage.meta_alloc->GetHandle(storage_header->metadata_ptr);
        token->metadata = meta_handle.GetPtr<uint8_t>();
        token->meta_handle = std::unique_ptr<SegmentHandle>(new SegmentHandle(std::move(meta_handle)));
    }

    out.header = &token->header;
    out.vector = token->vector_handle.GetPtr<float>();
    out.level0_neighbors = token->level0_neighbors.empty() ? nullptr : token->level0_neighbors.data();
    out.level0_count = &token->header.level0_count;
    out.upper_neighbors_base = token->upper_neighbors.empty() ? nullptr : token->upper_neighbors.data();
    out.upper_counts = token->upper_neighbors.empty() ? nullptr : token->upper_counts.data();
    out.metadata = token->metadata;
    out.opaque = token.release();
    return true;
}

void DuckDBCoreLowLevelBindingSkeleton::UnpinNode(::vex::DuckDBNodeLayoutView &view) {
    auto *token = static_cast<DuckDBPinnedNodeToken *>(view.opaque);
    if (token && token->writable) {
        auto *storage_header = token->node_handle.GetPtr<vex::HNSWNodeHeader>();
        storage_header->row_id = token->header.row_id;
        storage_header->level = token->header.level;
        storage_header->deleted = token->header.deleted;
        storage_header->level0_count = std::min<uint16_t>(token->header.level0_count, static_cast<uint16_t>(cfg_.m * 2));
        storage_header->extra_row_count = token->header.extra_row_count;
        storage_header->reserved = token->header.reserved;

        auto *level0_dst = storage_header->GetLevel0Neighbors();
        for (int i = 0; i < cfg_.m * 2; i++) {
            IndexPointer neighbor_ptr;
            auto neighbor_id = i < storage_header->level0_count ? token->level0_neighbors[static_cast<size_t>(i)]
                                                                : ::vex::INVALID_NODE_ID;
            if (!TranslateNodeIdToDuckPointer(node_id_to_ptr_, neighbor_id, neighbor_ptr)) {
                delete token;
                view = {};
                throw InternalException("DuckDB core direct binding: invalid level-0 neighbor node id");
            }
            level0_dst[i] = neighbor_ptr;
        }

        if (token->upper_handle) {
            auto *upper = token->upper_handle->GetPtr<vex::HNSWUpperLevel>();
            for (int level_idx = 0; level_idx < ::vex::HNSW_MAX_UPPER_LEVELS; level_idx++) {
                const auto level_count = std::min<uint16_t>(token->upper_counts[static_cast<size_t>(level_idx)],
                                                            static_cast<uint16_t>(cfg_.m));
                upper->counts[level_idx] = level_count;
                auto *dst_neighbors = upper->GetNeighbors(level_idx, cfg_.m);
                auto *src_neighbors =
                    token->upper_neighbors.data() + static_cast<size_t>(level_idx) * static_cast<size_t>(cfg_.m);
                for (int i = 0; i < cfg_.m; i++) {
                    IndexPointer neighbor_ptr;
                    auto neighbor_id = i < level_count ? src_neighbors[i] : ::vex::INVALID_NODE_ID;
                    if (!TranslateNodeIdToDuckPointer(node_id_to_ptr_, neighbor_id, neighbor_ptr)) {
                        delete token;
                        view = {};
                        throw InternalException("DuckDB core direct binding: invalid upper-layer neighbor node id");
                    }
                    dst_neighbors[i] = neighbor_ptr;
                }
            }
        }
    }
    delete token;
    view = {};
}

uint32_t DuckDBCoreLowLevelBindingSkeleton::GetDimension() const {
    return cfg_.dimension;
}

int DuckDBCoreLowLevelBindingSkeleton::GetM() const {
    return cfg_.m;
}

uint64_t DuckDBCoreLowLevelBindingSkeleton::GetNodeCount() const {
    return active_node_count_;
}

void DuckDBCoreLowLevelBindingSkeleton::ForEachNode(std::function<void(::vex::node_id_t)> cb) const {
    for (::vex::node_id_t node_id = 0; node_id < node_id_to_ptr_.size(); node_id++) {
        if (node_id_to_ptr_[node_id].Get()) {
            cb(node_id);
        }
    }
}

bool DuckDBCoreLowLevelBindingSkeleton::LoadGraphState(bool &has_entry_point, ::vex::node_id_t &entry_point,
                                                       int &max_level, uint64_t &node_count) const {
    if (cfg_.storage_info && !ptr_to_node_id_.empty()) {
        DuckDBCoreGraphState loaded{};
        if (!TryBootstrapDuckDBGraphStateFromStorage(cfg_, ptr_to_node_id_, loaded)) {
            return false;
        }
        has_entry_point = loaded.has_entry_point;
        entry_point = loaded.entry_point;
        max_level = loaded.max_level;
        node_count = loaded.node_count;
        return true;
    }

    if (cfg_.load_graph_state_cb) {
        DuckDBCoreGraphState loaded{};
        if (!cfg_.load_graph_state_cb(loaded)) {
            return false;
        }
        has_entry_point = loaded.has_entry_point;
        entry_point = loaded.entry_point;
        max_level = loaded.max_level;
        node_count = loaded.node_count;
        return true;
    }

    has_entry_point = state_.has_entry_point;
    entry_point = state_.entry_point;
    max_level = state_.max_level;
    node_count = state_.node_count;
    return true;
}

bool DuckDBCoreLowLevelBindingSkeleton::StoreGraphState(bool has_entry_point, ::vex::node_id_t entry_point, int max_level,
                                                        uint64_t node_count) {
    DuckDBCoreGraphState out{};
    out.has_entry_point = has_entry_point;
    out.entry_point = entry_point;
    out.max_level = max_level;
    out.node_count = node_count;

    if (cfg_.storage_info && !node_id_to_ptr_.empty()) {
        if (!TryStoreDuckDBGraphStateToStorage(cfg_, node_id_to_ptr_, out)) {
            return false;
        }
    }

    if (cfg_.store_graph_state_cb && !(cfg_.storage_info && !node_id_to_ptr_.empty())) {
        if (!cfg_.store_graph_state_cb(out)) {
            return false;
        }
    }

    state_.has_entry_point = has_entry_point;
    state_.entry_point = entry_point;
    state_.max_level = max_level;
    state_.node_count = node_count;
    return true;
}

bool DuckDBCoreLowLevelBindingSkeleton::ResolveNodeIdByRowId(::vex::row_id_t row_id, ::vex::node_id_t &node_id) const {
    node_id = ::vex::INVALID_NODE_ID;
    if (!cfg_.live_storage.row_id_map) {
        return false;
    }
    auto it = cfg_.live_storage.row_id_map->find(static_cast<row_t>(row_id));
    if (it == cfg_.live_storage.row_id_map->end()) {
        return false;
    }
    return TranslateDuckPointerToNodeId(ptr_to_node_id_, it->second, node_id) && node_id != ::vex::INVALID_NODE_ID;
}

bool DuckDBCoreLowLevelBindingSkeleton::ResolveStorageNodeKey(::vex::node_id_t node_id, uint64_t &key) const {
    key = 0;
    if (node_id >= node_id_to_ptr_.size()) {
        return false;
    }
    key = node_id_to_ptr_[node_id].Get();
    return key != 0;
}

std::shared_ptr<::vex::DuckDBNodeStore> CreateDuckDBCoreNodeStoreSkeleton(const DuckDBCoreBindingConfig &cfg) {
    auto binding = std::make_shared<DuckDBCoreLowLevelBindingSkeleton>(cfg);

    ::vex::DuckDBNodeStoreConfig store_cfg{};
    store_cfg.dimension = cfg.dimension;
    store_cfg.m = cfg.m;
    store_cfg.metadata_size = cfg.metadata_size;
    store_cfg.duckdb_block_manager = cfg.block_manager;
    store_cfg.duckdb_buffer_manager = cfg.buffer_manager;
    store_cfg.index_name = cfg.index_name;
    store_cfg.low_level_binding = binding;

    return std::make_shared<::vex::DuckDBNodeStore>(store_cfg);
}

} // namespace duckdb
