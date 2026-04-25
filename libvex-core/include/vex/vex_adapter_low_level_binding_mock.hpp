#ifndef VEX_ADAPTER_LOW_LEVEL_BINDING_MOCK_HPP
#define VEX_ADAPTER_LOW_LEVEL_BINDING_MOCK_HPP

#include "vex/vex_adapter_node_store_common.hpp"

#include <unordered_map>

namespace vex {

// Shared in-memory mock for adapter direct-binding integration tests.
// DuckDB/PG wrappers should alias this class instead of duplicating logic.
class AdapterLowLevelBindingMock final : public AdapterLowLevelBinding {
public:
    AdapterLowLevelBindingMock(uint32_t dimension, int m, uint32_t metadata_size = 0);

    node_id_t AllocateNode(row_id_t row_id, const float *vec, uint32_t dim, uint8_t level) override;
    void FreeNode(node_id_t node_id) override;

    bool PinNode(node_id_t node_id, bool for_update, AdapterNodeLayoutView &out) override;
    void UnpinNode(AdapterNodeLayoutView &view) override;

    uint32_t GetDimension() const override;
    int GetM() const override;
    uint64_t GetNodeCount() const override;
    void ForEachNode(std::function<void(node_id_t)> cb) const override;
    bool ResolveNodeIdByRowId(row_id_t row_id, node_id_t &node_id) const override;
    bool ResolveStorageNodeKey(node_id_t node_id, uint64_t &storage_key) const override;

private:
    MemoryNodeStore store_;
    std::unordered_map<row_id_t, node_id_t> row_to_node_id_;
};

} // namespace vex

#endif // VEX_ADAPTER_LOW_LEVEL_BINDING_MOCK_HPP
