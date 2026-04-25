#ifndef VEX_NODE_STORE_MEMORY_HPP
#define VEX_NODE_STORE_MEMORY_HPP

#include "vex/vex_node_store.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vex {

class MemoryNodeStore : public NodeStore {
public:
    MemoryNodeStore(uint32_t dim, int m, uint32_t metadata_size = 0);

    node_id_t AllocateNode(row_id_t row_id, const float *vec, uint32_t dim, uint8_t level) override;
    void FreeNode(node_id_t node_id) override;

    std::unique_ptr<NodeHandle> PinNode(node_id_t node_id) const override;
    std::unique_ptr<MutableNodeHandle> PinNodeForUpdate(node_id_t node_id) override;

    uint32_t GetDimension() const override { return dim_; }
    int GetM() const override { return m_; }
    uint64_t GetNodeCount() const override { return node_count_; }

    void ForEachNode(std::function<void(node_id_t)> cb) const override;

    bool Serialize(std::vector<uint8_t> &out) const;
    bool Deserialize(const uint8_t *data, size_t size, std::string *err = nullptr);

private:
    struct UpperBlock {
        std::vector<uint16_t> counts;
        std::vector<node_id_t> neighbors;
    };

    class MemoryNodeHandle final : public MutableNodeHandle {
    public:
        MemoryNodeHandle(MemoryNodeStore *store, node_id_t node_id);

        const NodeHeader *Header() const override;
        const float *Vector() const override;

        const node_id_t *Level0Neighbors() const override;
        uint16_t Level0Count() const override;

        const node_id_t *UpperNeighbors(int level_idx) const override;
        uint16_t UpperCount(int level_idx) const override;

        const uint8_t *Metadata() const override;

        NodeHeader *MutableHeader() override;
        node_id_t *MutableLevel0Neighbors() override;
        void SetLevel0Count(uint16_t count) override;

        node_id_t *MutableUpperNeighbors(int level_idx) override;
        void SetUpperCount(int level_idx, uint16_t count) override;
        uint16_t *MutableUpperCounts() override;
        const uint16_t *UpperCounts() const override;

    private:
        MemoryNodeStore *store_;
        node_id_t node_id_;
    };

private:
    bool IsNodeValid(node_id_t node_id) const;

private:
    uint32_t dim_;
    int m_;
    uint32_t metadata_size_;
    uint64_t node_count_ = 0;

    std::vector<NodeHeader> headers_;
    std::vector<float> vectors_;
    std::vector<node_id_t> neighbors_l0_;
    std::vector<UpperBlock> upper_neighbors_;
    std::vector<uint8_t> metadata_;
    std::vector<uint8_t> alive_;
};

} // namespace vex

#endif // VEX_NODE_STORE_MEMORY_HPP
