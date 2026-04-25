#ifndef GRAPH_INDEX_CORE_NODE_STORE_BRIDGE_LIVE_H
#define GRAPH_INDEX_CORE_NODE_STORE_BRIDGE_LIVE_H

#include "graph_index/core_node_store_bridge.h"
#include "graph_index/core_node_store_bridge_readonly.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

template <typename Store>
class PgCoreLiveBinding final : public vex::PGLowLevelBinding {
public:
    using store_id_t = typename Store::T;
    using point_type = typename Store::point_type;

    PgCoreLiveBinding(Store &store, const PgCoreBindingConfig &cfg)
        : store_(store),
          cfg_(cfg),
          dim_(store.get_dim()),
          m_(store.get_m())
    {
        auto [entry_info, unused_shared] = store_.template get_entry<false>();
        state_.has_entry_point = entry_info.id != INVALID_VECTOR_ID && entry_info.level >= 0;
        state_.entry_point = state_.has_entry_point
            ? static_cast<vex::node_id_t>(entry_info.id)
            : vex::INVALID_NODE_ID;
        state_.max_level = state_.has_entry_point ? static_cast<int>(entry_info.level) : 0;
        slot_limit_ = static_cast<uint64_t>(store_.get_vector_num());
        state_.node_count = slot_limit_;
        BuildUpperChains();
        BootstrapRowMappings();

        if (cfg_.load_graph_state_cb) {
            PgCoreGraphState loaded_state{};
            if (cfg_.load_graph_state_cb(loaded_state)) {
                state_.has_entry_point = loaded_state.has_entry_point;
                state_.entry_point = loaded_state.entry_point;
                state_.max_level = loaded_state.max_level;
                state_.node_count = loaded_state.node_count;
            }
        }
    }

    struct AccessStats {
        uint64_t pin_read_calls = 0;
        uint64_t pin_write_calls = 0;
        uint64_t unpin_calls = 0;
        double pin_read_ms = 0.0;
        double pin_write_ms = 0.0;
        double unpin_ms = 0.0;
        double pin_vector_ms = 0.0;
        double pin_header_ms = 0.0;
        double pin_level0_ms = 0.0;
        double pin_upper_ms = 0.0;
        double pin_metadata_ms = 0.0;
    };

    const AccessStats &GetAccessStats() const
    {
        return access_stats_;
    }

    void ResetAccessStats()
    {
        access_stats_ = {};
    }

    vex::node_id_t AllocateNode(vex::row_id_t row_id, const float *vec, uint32_t dim, uint8_t level) override
    {
        if (!vec || dim != dim_) {
            return vex::INVALID_NODE_ID;
        }

        auto base_id_raw = store_.template assign_vector_id<true>();
        auto base_id = ToNodeId(base_id_raw);

        std::vector<store_id_t> base_neighbors(static_cast<size_t>(m_) * 2,
                                               static_cast<store_id_t>(INVALID_VECTOR_ID));
        store_.add_basepoint(base_id_raw, base_neighbors.data());
        store_.add_vector(base_id_raw, reinterpret_cast<const char *>(vec));

        std::vector<vex::node_id_t> upper_slots;
        upper_slots.reserve(level);
        store_id_t lower_layer_idx = base_id_raw;
        for (uint8_t level_idx = 0; level_idx < level; ++level_idx) {
            auto cur_layer_idx = store_.template assign_vector_id<false>();
            upper_slots.push_back(ToNodeId(cur_layer_idx));

            std::vector<store_id_t> upper_neighbors(static_cast<size_t>(m_) * 2,
                                                    static_cast<store_id_t>(INVALID_VECTOR_ID));
            store_.add_upperpoint(cur_layer_idx, lower_layer_idx, base_id_raw, upper_neighbors.data());
            lower_layer_idx = cur_layer_idx;
        }

        upper_chain_by_node_[base_id] = upper_slots;

        vex::NodeHeader header{};
        header.row_id = row_id;
        header.level = level;
        header.deleted = 0;
        header.level0_count = 0;
        header.extra_row_count = 0;
        header.reserved = 0;
        header.upper_offset = 0;
        header.metadata_offset = 0;
        header_cache_[base_id] = header;
        row_to_node_id_[row_id] = base_id;
        metadata_cache_[base_id].assign(cfg_.metadata_size, 0);

        if (cfg_.on_allocate_node_cb && !cfg_.on_allocate_node_cb(base_id, row_id, level)) {
            ereport(ERROR, (errmsg("PG core live binding: node allocation hook failed")));
        }

        slot_limit_ = static_cast<uint64_t>(store_.get_vector_num());
        state_.node_count++;
        return base_id;
    }

    void FreeNode(vex::node_id_t node_id) override
    {
        auto header_it = header_cache_.find(node_id);
        if (header_it != header_cache_.end()) {
            header_it->second.deleted = 1;
            if (cfg_.store_node_header_cb) {
                (void)cfg_.store_node_header_cb(node_id, header_it->second);
            }
            row_to_node_id_.erase(header_it->second.row_id);
        }

        if (cfg_.on_free_node_cb && !cfg_.on_free_node_cb(node_id)) {
            ereport(ERROR, (errmsg("PG core live binding: node free hook failed")));
        }

        metadata_cache_.erase(node_id);
        upper_chain_by_node_.erase(node_id);
        if (state_.node_count > 0) {
            state_.node_count--;
        }
    }

    bool PinNode(vex::node_id_t node_id, bool for_update, vex::PGNodeLayoutView &out) override
    {
        auto pin_start = std::chrono::steady_clock::now();
        out = {};
        if (node_id >= slot_limit_) {
            AccumulatePinStats(for_update, pin_start);
            return false;
        }

        auto pin_state = std::make_unique<NodePinState>();
        pin_state->node_id = node_id;
        pin_state->writable = for_update;
        auto stage_start = std::chrono::steady_clock::now();
        pin_state->vector_buf = store_.pin_vector_buffer(ToStoreId(node_id));
        access_stats_.pin_vector_ms += ElapsedMs(stage_start);

        pin_state->level0_neighbors.assign(static_cast<size_t>(m_) * 2, vex::INVALID_NODE_ID);
        pin_state->upper_neighbors.assign(static_cast<size_t>(m_) * static_cast<size_t>(vex::HNSW_MAX_UPPER_LEVELS),
                                          vex::INVALID_NODE_ID);
        pin_state->upper_counts.fill(0);
        pin_state->metadata.assign(cfg_.metadata_size, 0);

        stage_start = std::chrono::steady_clock::now();
        if (!LoadHeader(node_id, pin_state->header)) {
            AccumulatePinStats(for_update, pin_start);
            return false;
        }
        access_stats_.pin_header_ms += ElapsedMs(stage_start);

        stage_start = std::chrono::steady_clock::now();
        LoadMetadata(node_id, pin_state->metadata);
        access_stats_.pin_metadata_ms += ElapsedMs(stage_start);

        stage_start = std::chrono::steady_clock::now();
        LoadLevel0Neighbors(node_id, *pin_state);
        access_stats_.pin_level0_ms += ElapsedMs(stage_start);

        stage_start = std::chrono::steady_clock::now();
        LoadUpperNeighbors(node_id, *pin_state);
        access_stats_.pin_upper_ms += ElapsedMs(stage_start);

        out.header = &pin_state->header;
        out.vector = reinterpret_cast<const float *>(pin_state->vector_buf.get_vecbuf());
        out.level0_neighbors = pin_state->level0_neighbors.data();
        out.level0_count = &pin_state->header.level0_count;
        out.upper_neighbors_base = pin_state->upper_neighbors.data();
        out.upper_counts = pin_state->upper_counts.data();
        out.metadata = pin_state->metadata.empty() ? nullptr : pin_state->metadata.data();
        out.opaque = pin_state.release();
        AccumulatePinStats(for_update, pin_start);
        return true;
    }

    void UnpinNode(vex::PGNodeLayoutView &view) override
    {
        auto unpin_start = std::chrono::steady_clock::now();
        auto *pin_state = reinterpret_cast<NodePinState *>(view.opaque);
        if (!pin_state) {
            view = {};
            access_stats_.unpin_calls++;
            access_stats_.unpin_ms += ElapsedMs(unpin_start);
            return;
        }

        if (pin_state->writable) {
            FlushNode(*pin_state);
        }

        delete pin_state;
        view = {};
        access_stats_.unpin_calls++;
        access_stats_.unpin_ms += ElapsedMs(unpin_start);
    }

    uint32_t GetDimension() const override
    {
        return dim_;
    }

    int GetM() const override
    {
        return m_;
    }

    uint64_t GetNodeCount() const override
    {
        return slot_limit_;
    }

    void ForEachNode(std::function<void(vex::node_id_t)> cb) const override
    {
        for (const auto &entry : row_to_node_id_) {
            cb(entry.second);
        }
    }

    bool LoadGraphState(bool &has_entry_point, vex::node_id_t &entry_point, int &max_level,
                        uint64_t &node_count) const override
    {
        has_entry_point = state_.has_entry_point;
        entry_point = state_.entry_point;
        max_level = state_.max_level;
        node_count = state_.node_count;
        return true;
    }

    bool StoreGraphState(bool has_entry_point, vex::node_id_t entry_point, int max_level,
                         uint64_t node_count) override
    {
        state_.has_entry_point = has_entry_point;
        state_.entry_point = entry_point;
        state_.max_level = max_level;
        state_.node_count = node_count;
        if (cfg_.store_graph_state_cb) {
            PgCoreGraphState graph_state{};
            graph_state.has_entry_point = has_entry_point;
            graph_state.entry_point = entry_point;
            graph_state.max_level = max_level;
            graph_state.node_count = node_count;
            return cfg_.store_graph_state_cb(graph_state);
        }
        return true;
    }

    bool ResolveNodeIdByRowId(vex::row_id_t row_id, vex::node_id_t &node_id) const override
    {
        if (cfg_.resolve_node_id_by_row_id_cb && cfg_.resolve_node_id_by_row_id_cb(row_id, node_id)) {
            return true;
        }
        auto it = row_to_node_id_.find(row_id);
        if (it == row_to_node_id_.end()) {
            return false;
        }
        node_id = it->second;
        return true;
    }

    bool ResolveStorageNodeKey(vex::node_id_t node_id, uint64_t &storage_key) const override
    {
        if (cfg_.resolve_storage_node_key_cb && cfg_.resolve_storage_node_key_cb(node_id, storage_key)) {
            return true;
        }
        storage_key = static_cast<uint64_t>(node_id);
        return true;
    }

private:
    static constexpr size_t kBaseNeighborScratchWidth = static_cast<size_t>(GRAPH_INDEX_MAX_M) * 2;
    static constexpr size_t kUpperNeighborScratchWidth = (static_cast<size_t>(GRAPH_INDEX_MAX_M) + 1) * 2;

    static double ElapsedMs(const std::chrono::steady_clock::time_point &start)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    void AccumulatePinStats(bool for_update, const std::chrono::steady_clock::time_point &start)
    {
        const double elapsed_ms = ElapsedMs(start);
        if (for_update) {
            access_stats_.pin_write_calls++;
            access_stats_.pin_write_ms += elapsed_ms;
        } else {
            access_stats_.pin_read_calls++;
            access_stats_.pin_read_ms += elapsed_ms;
        }
    }

    struct NodePinState {
        vex::node_id_t node_id = vex::INVALID_NODE_ID;
        bool writable = false;
        vex::NodeHeader header{};
        VecBuffer vector_buf{};
        std::vector<vex::node_id_t> level0_neighbors;
        std::vector<vex::node_id_t> upper_neighbors;
        std::array<uint16_t, vex::HNSW_MAX_UPPER_LEVELS> upper_counts{};
        std::vector<uint8_t> metadata;

        ~NodePinState()
        {
            vector_buf.release();
        }
    };

    bool IsPinnedNeighborAddressable(store_id_t raw_id) const
    {
        return raw_id != static_cast<store_id_t>(INVALID_VECTOR_ID) &&
               static_cast<vex::node_id_t>(raw_id) < slot_limit_;
    }

    void LoadLevel0Neighbors(vex::node_id_t node_id, NodePinState &pin_state)
    {
        std::array<store_id_t, kBaseNeighborScratchWidth> base_raw{};
        base_raw.fill(static_cast<store_id_t>(INVALID_VECTOR_ID));
        store_.base_layer.template get_n<disk_container::AccessorLockType::ReadLock>(
            ToStoreId(node_id), 1,
            reinterpret_cast<GraphIndexDiskBasePoint<store_id_t> *>(base_raw.data()));
        pin_state.header.level0_count = PgBridgeCountValidNeighbors(base_raw.data(), static_cast<uint16>(m_ * 2));
        for (uint16 i = 0; i < pin_state.header.level0_count; ++i) {
            const store_id_t raw_id = base_raw[static_cast<size_t>(i)];
            pin_state.level0_neighbors[static_cast<size_t>(i)] =
                IsPinnedNeighborAddressable(raw_id)
                    ? static_cast<vex::node_id_t>(raw_id)
                    : vex::INVALID_NODE_ID;
        }
    }

    void LoadUpperNeighbors(vex::node_id_t node_id, NodePinState &pin_state)
    {
        auto chain_it = upper_chain_by_node_.find(node_id);
        if (chain_it == upper_chain_by_node_.end()) {
            pin_state.header.level = 0;
            return;
        }

        pin_state.header.level = static_cast<uint8_t>(chain_it->second.size());
        std::array<store_id_t, kUpperNeighborScratchWidth> upper_raw{};
        for (size_t level_idx = 0; level_idx < chain_it->second.size(); ++level_idx) {
            upper_raw.fill(static_cast<store_id_t>(INVALID_VECTOR_ID));
            store_.upper_layer.template get_n<disk_container::AccessorLockType::ReadLock>(
                ToStoreId(chain_it->second[level_idx]), 1,
                reinterpret_cast<GraphIndexDiskUpperPoint<store_id_t> *>(upper_raw.data()));

            auto *neighbor_ids = upper_raw.data() + 2;
            auto count = PgBridgeCountValidNeighbors(neighbor_ids, static_cast<uint16>(m_));
            pin_state.upper_counts[level_idx] = count;
            auto *dst = pin_state.upper_neighbors.data() + level_idx * static_cast<size_t>(m_);
            for (uint16 i = 0; i < count; ++i) {
                const store_id_t raw_id = neighbor_ids[static_cast<size_t>(i)];
                dst[static_cast<size_t>(i)] = IsPinnedNeighborAddressable(raw_id)
                    ? static_cast<vex::node_id_t>(raw_id)
                    : vex::INVALID_NODE_ID;
            }
        }
    }

    static vex::node_id_t ToNodeId(store_id_t id)
    {
        return static_cast<vex::node_id_t>(id);
    }

    static store_id_t ToStoreId(vex::node_id_t node_id)
    {
        return static_cast<store_id_t>(node_id);
    }

    bool LoadHeader(vex::node_id_t node_id, vex::NodeHeader &header)
    {
        auto it = header_cache_.find(node_id);
        if (it != header_cache_.end()) {
            header = it->second;
        } else {
            header = {};
            header.row_id = static_cast<vex::row_id_t>(node_id);
            header.level = 0;
            header.deleted = 0;
            header.level0_count = 0;
            header.extra_row_count = 0;
            header.reserved = 0;
            header.upper_offset = 0;
            header.metadata_offset = 0;
        }

        auto chain_it = upper_chain_by_node_.find(node_id);
        header.level = chain_it == upper_chain_by_node_.end()
            ? 0
            : static_cast<uint8_t>(chain_it->second.size());

        if (!cfg_.trust_live_header_cache_for_read) {
            store_.get_itempointer(ToStoreId(node_id), [&](const point_type *elem) {
                header.deleted = (elem->is_deleted() || elem->empty()) ? 1 : 0;
            });
        }

        if (cfg_.load_node_header_cb) {
            if (!cfg_.load_node_header_cb(node_id, header)) {
                return false;
            }
        }
        header_cache_[node_id] = header;
        return true;
    }

    void LoadMetadata(vex::node_id_t node_id, std::vector<uint8_t> &metadata)
    {
        if (cfg_.metadata_size == 0) {
            metadata.clear();
            return;
        }
        auto it = metadata_cache_.find(node_id);
        if (it != metadata_cache_.end()) {
            metadata = it->second;
            if (metadata.size() < cfg_.metadata_size) {
                metadata.resize(cfg_.metadata_size, 0);
            }
            return;
        }
        if (metadata.size() < cfg_.metadata_size) {
            metadata.resize(cfg_.metadata_size, 0);
        }
        if (cfg_.load_node_metadata_cb && cfg_.load_node_metadata_cb(node_id, metadata.data(), cfg_.metadata_size)) {
            metadata_cache_[node_id] = metadata;
        }
    }

    void FlushNode(const NodePinState &pin_state)
    {
        std::vector<store_id_t> base_neighbors(static_cast<size_t>(m_) * 2,
                                               static_cast<store_id_t>(INVALID_VECTOR_ID));
        for (uint16_t i = 0; i < pin_state.header.level0_count; ++i) {
            base_neighbors[static_cast<size_t>(i)] = ToStoreId(pin_state.level0_neighbors[static_cast<size_t>(i)]);
        }
        store_.set_base_neighbors(ToStoreId(pin_state.node_id), base_neighbors.data());

        auto chain_it = upper_chain_by_node_.find(pin_state.node_id);
        if (chain_it != upper_chain_by_node_.end()) {
            for (size_t level_idx = 0; level_idx < chain_it->second.size(); ++level_idx) {
                std::vector<store_id_t> upper_neighbors(static_cast<size_t>(m_) * 2,
                                                        static_cast<store_id_t>(INVALID_VECTOR_ID));
                auto *src = pin_state.upper_neighbors.data() + level_idx * static_cast<size_t>(m_);
                for (uint16_t i = 0; i < pin_state.upper_counts[level_idx]; ++i) {
                    upper_neighbors[static_cast<size_t>(i)] = ToStoreId(src[static_cast<size_t>(i)]);
                    upper_neighbors[static_cast<size_t>(m_) + static_cast<size_t>(i)] =
                        ResolveUpperStorageSlot(src[static_cast<size_t>(i)], level_idx);
                }
                store_.set_upper_neighbors(ToStoreId(chain_it->second[level_idx]), upper_neighbors.data());
            }
        }

        header_cache_[pin_state.node_id] = pin_state.header;
        if (pin_state.header.deleted) {
            row_to_node_id_.erase(pin_state.header.row_id);
        } else {
            row_to_node_id_[pin_state.header.row_id] = pin_state.node_id;
        }

        if (!pin_state.metadata.empty()) {
            metadata_cache_[pin_state.node_id] = pin_state.metadata;
            if (cfg_.store_node_metadata_cb) {
                (void)cfg_.store_node_metadata_cb(pin_state.node_id, pin_state.metadata.data(), cfg_.metadata_size);
            }
        }

        if (cfg_.store_node_header_cb) {
            (void)cfg_.store_node_header_cb(pin_state.node_id, pin_state.header);
        }
    }

    store_id_t ResolveUpperStorageSlot(vex::node_id_t node_id, size_t level_idx) const
    {
        auto it = upper_chain_by_node_.find(node_id);
        if (it == upper_chain_by_node_.end() || level_idx >= it->second.size()) {
            return static_cast<store_id_t>(INVALID_VECTOR_ID);
        }
        return ToStoreId(it->second[level_idx]);
    }

    void BuildUpperChains()
    {
        std::unordered_map<vex::node_id_t, std::vector<std::pair<vex::node_id_t, vex::node_id_t>>> slots_by_node;
        auto upper_count = store_.get_upper_num();
        for (store_id_t cur_layer_idx = 0; cur_layer_idx < upper_count; ++cur_layer_idx) {
            std::vector<store_id_t> upper_raw(static_cast<size_t>(m_ + 1) * 2,
                                              static_cast<store_id_t>(INVALID_VECTOR_ID));
            store_.upper_layer.template get_n<disk_container::AccessorLockType::ReadLock>(
                cur_layer_idx, 1,
                reinterpret_cast<GraphIndexDiskUpperPoint<store_id_t> *>(upper_raw.data()));
            store_id_t lower_layer_idx = upper_raw[0];
            store_id_t node_id = upper_raw[1];
            if (node_id == static_cast<store_id_t>(INVALID_VECTOR_ID)) {
                continue;
            }
            slots_by_node[ToNodeId(node_id)].push_back({ToNodeId(cur_layer_idx), ToNodeId(lower_layer_idx)});
        }

        for (auto &entry : slots_by_node) {
            auto node_id = entry.first;
            auto &slots = entry.second;
            std::unordered_map<vex::node_id_t, vex::node_id_t> lower_by_cur;
            std::unordered_map<vex::node_id_t, vex::node_id_t> raw_by_cur;
            std::unordered_set<vex::node_id_t> referenced_lower;
            for (auto &slot : slots) {
                lower_by_cur[slot.first] = slot.second;
                raw_by_cur[slot.first] = slot.first;
                if (slot.second != node_id) {
                    referenced_lower.insert(slot.second);
                }
            }

            vex::node_id_t top = vex::INVALID_NODE_ID;
            for (auto &slot : slots) {
                if (referenced_lower.find(slot.first) == referenced_lower.end()) {
                    top = slot.first;
                    break;
                }
            }
            if (top == vex::INVALID_NODE_ID) {
                continue;
            }

            std::vector<vex::node_id_t> chain_top_down;
            auto cur = top;
            while (cur != vex::INVALID_NODE_ID) {
                chain_top_down.push_back(raw_by_cur[cur]);
                auto it = lower_by_cur.find(cur);
                if (it == lower_by_cur.end() || it->second == node_id) {
                    break;
                }
                cur = it->second;
            }
            upper_chain_by_node_[node_id].assign(chain_top_down.rbegin(), chain_top_down.rend());
        }
    }

    void BootstrapRowMappings()
    {
        uint64_t active_count = 0;
        for (vex::node_id_t node_id = 0; node_id < slot_limit_; ++node_id) {
            vex::NodeHeader header{};
            if (!LoadHeader(node_id, header)) {
                continue;
            }
            if (header.deleted) {
                continue;
            }
            row_to_node_id_[header.row_id] = node_id;
            active_count++;
        }
        state_.node_count = active_count;
    }

private:
    Store &store_;
    PgCoreBindingConfig cfg_;
    uint32_t dim_;
    int m_;
    PgCoreGraphState state_{};
    std::unordered_map<vex::node_id_t, vex::NodeHeader> header_cache_;
    std::unordered_map<vex::node_id_t, std::vector<uint8_t>> metadata_cache_;
    std::unordered_map<vex::row_id_t, vex::node_id_t> row_to_node_id_;
    std::unordered_map<vex::node_id_t, std::vector<vex::node_id_t>> upper_chain_by_node_;
    uint64_t slot_limit_ = 0;
    AccessStats access_stats_{};
};

template <typename Store>
static inline std::shared_ptr<PgCoreLiveBinding<Store>> CreatePgCoreLiveBinding(Store &store,
                                                                                const PgCoreBindingConfig &cfg)
{
    return std::make_shared<PgCoreLiveBinding<Store>>(store, cfg);
}

#endif // GRAPH_INDEX_CORE_NODE_STORE_BRIDGE_LIVE_H
