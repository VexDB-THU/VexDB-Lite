#ifndef GRAPH_INDEX_CORE_NODE_STORE_BRIDGE_READONLY_H
#define GRAPH_INDEX_CORE_NODE_STORE_BRIDGE_READONLY_H

#include "graph_index/graph_index_storage.h"
#include "vex/vex_adapter_graph_state.hpp"
#include "vex/vex_adapter_pg_stub.hpp"

#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

template <typename T>
static inline uint16 PgBridgeCountValidNeighbors(const T *neighbors, uint16 cap)
{
    uint16 count = 0;
    while (count < cap && neighbors[count] != static_cast<T>(INVALID_VECTOR_ID)) {
        count++;
    }
    return count;
}

template <typename Store>
class PgCoreReadOnlyBinding final : public vex::PGLowLevelBinding {
public:
    using store_id_t = typename Store::T;
    using point_type = typename Store::point_type;

    explicit PgCoreReadOnlyBinding(Store &store)
        : store_(store),
          dim_(store.get_dim()),
          m_(store.get_m())
    {
        auto [entry_info, unused_shared] = store_.template get_entry<false>();
        state_.has_entry_point = entry_info.id != INVALID_VECTOR_ID && entry_info.level >= 0;
        state_.entry_point = state_.has_entry_point
            ? static_cast<vex::node_id_t>(entry_info.id)
            : vex::INVALID_NODE_ID;
        state_.max_level = state_.has_entry_point ? static_cast<int>(entry_info.level) : 0;
        state_.node_count = static_cast<uint64_t>(store_.get_vector_num());
        BuildUpperChains();
    }

    vex::node_id_t AllocateNode(vex::row_id_t, const float *, uint32_t, uint8_t) override
    {
        return vex::INVALID_NODE_ID;
    }

    void FreeNode(vex::node_id_t) override
    {
    }

    bool PinNode(vex::node_id_t node_id, bool for_update, vex::PGNodeLayoutView &out) override
    {
        out = {};
        if (for_update || node_id >= state_.node_count) {
            return false;
        }

        auto token = std::make_unique<PinnedNodeToken>();
        token->vector_buf = store_.pin_vector_buffer(static_cast<store_id_t>(node_id));
        token->level0_neighbors.assign(static_cast<size_t>(m_) * 2, vex::INVALID_NODE_ID);
        token->upper_neighbors.assign(static_cast<size_t>(m_) * static_cast<size_t>(vex::HNSW_MAX_UPPER_LEVELS),
                                      vex::INVALID_NODE_ID);
        token->upper_counts.fill(0);

        std::vector<store_id_t> base_raw(static_cast<size_t>(m_) * 2, static_cast<store_id_t>(INVALID_VECTOR_ID));
        store_.base_layer.template get_n<disk_container::AccessorLockType::ReadLock>(
            static_cast<store_id_t>(node_id), 1,
            reinterpret_cast<GraphIndexDiskBasePoint<store_id_t> *>(base_raw.data()));
        token->header.level0_count = PgBridgeCountValidNeighbors(base_raw.data(), static_cast<uint16>(m_ * 2));
        for (uint16 i = 0; i < token->header.level0_count; ++i) {
            token->level0_neighbors[i] = static_cast<vex::node_id_t>(base_raw[i]);
        }

        auto chain_it = upper_chain_by_node_.find(node_id);
        if (chain_it != upper_chain_by_node_.end()) {
            token->header.level = static_cast<uint8>(chain_it->second.size());
            for (size_t level_idx = 0; level_idx < chain_it->second.size(); ++level_idx) {
                std::vector<store_id_t> upper_raw(static_cast<size_t>(m_ + 1) * 2,
                                                  static_cast<store_id_t>(INVALID_VECTOR_ID));
                store_.upper_layer.template get_n<disk_container::AccessorLockType::ReadLock>(
                    static_cast<store_id_t>(chain_it->second[level_idx]), 1,
                    reinterpret_cast<GraphIndexDiskUpperPoint<store_id_t> *>(upper_raw.data()));

                auto *neighbor_ids = upper_raw.data() + 2;
                auto count = PgBridgeCountValidNeighbors(neighbor_ids, static_cast<uint16>(m_));
                token->upper_counts[level_idx] = count;
                auto *dst = token->upper_neighbors.data() + level_idx * static_cast<size_t>(m_);
                for (uint16 i = 0; i < count; ++i) {
                    dst[i] = static_cast<vex::node_id_t>(neighbor_ids[i]);
                }
            }
        } else {
            token->header.level = 0;
        }

        token->header.row_id = static_cast<vex::row_id_t>(node_id);
        token->header.deleted = 0;
        token->header.extra_row_count = 0;
        token->header.reserved = 0;
        token->header.upper_offset = 0;
        token->header.metadata_offset = 0;

        store_.get_itempointer(static_cast<store_id_t>(node_id), [&](const point_type *elem) {
            token->header.deleted = (elem->is_deleted() || elem->empty()) ? 1 : 0;
        });

        out.header = &token->header;
        out.vector = reinterpret_cast<const float *>(token->vector_buf.get_vecbuf());
        out.level0_neighbors = token->level0_neighbors.data();
        out.level0_count = &token->header.level0_count;
        out.upper_neighbors_base = token->upper_neighbors.data();
        out.upper_counts = token->upper_counts.data();
        out.metadata = nullptr;
        out.opaque = token.release();
        return true;
    }

    void UnpinNode(vex::PGNodeLayoutView &view) override
    {
        auto *token = reinterpret_cast<PinnedNodeToken *>(view.opaque);
        delete token;
        view = {};
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
        return state_.node_count;
    }

    void ForEachNode(std::function<void(vex::node_id_t)> cb) const override
    {
        for (vex::node_id_t node_id = 0; node_id < state_.node_count; ++node_id) {
            cb(node_id);
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

    bool StoreGraphState(bool, vex::node_id_t, int, uint64_t) override
    {
        return false;
    }

private:
    struct PinnedNodeToken {
        vex::NodeHeader header{};
        VecBuffer vector_buf{};
        std::vector<vex::node_id_t> level0_neighbors;
        std::vector<vex::node_id_t> upper_neighbors;
        std::array<uint16_t, vex::HNSW_MAX_UPPER_LEVELS> upper_counts{};

        ~PinnedNodeToken()
        {
            vector_buf.release();
        }
    };

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
            slots_by_node[static_cast<vex::node_id_t>(node_id)].push_back(
                {static_cast<vex::node_id_t>(cur_layer_idx), static_cast<vex::node_id_t>(lower_layer_idx)});
        }

        for (auto &entry : slots_by_node) {
            auto node_id = entry.first;
            auto &slots = entry.second;
            std::unordered_map<vex::node_id_t, vex::node_id_t> lower_by_cur;
            std::unordered_set<vex::node_id_t> referenced_lower;
            for (auto &slot : slots) {
                lower_by_cur[slot.first] = slot.second;
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
                chain_top_down.push_back(cur);
                auto it = lower_by_cur.find(cur);
                if (it == lower_by_cur.end() || it->second == node_id) {
                    break;
                }
                cur = it->second;
            }
            upper_chain_by_node_[node_id].assign(chain_top_down.rbegin(), chain_top_down.rend());
        }
    }

private:
    Store &store_;
    uint32_t dim_;
    int m_;
    vex::AdapterGraphState state_{};
    std::unordered_map<vex::node_id_t, std::vector<vex::node_id_t>> upper_chain_by_node_;
};

template <typename Store>
static inline std::shared_ptr<PgCoreReadOnlyBinding<Store>> CreatePgCoreReadOnlyBinding(Store &store)
{
    return std::make_shared<PgCoreReadOnlyBinding<Store>>(store);
}

#endif // GRAPH_INDEX_CORE_NODE_STORE_BRIDGE_READONLY_H
