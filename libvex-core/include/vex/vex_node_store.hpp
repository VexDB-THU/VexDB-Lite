#ifndef VEX_NODE_STORE_HPP
#define VEX_NODE_STORE_HPP

#include "vex/vex_node.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace vex {

class NodeHandle {
public:
    virtual ~NodeHandle() = default;

    virtual const NodeHeader *Header() const = 0;
    virtual const float *Vector() const = 0;

    virtual const node_id_t *Level0Neighbors() const = 0;
    virtual uint16_t Level0Count() const = 0;

    virtual const node_id_t *UpperNeighbors(int level_idx) const = 0;
    virtual uint16_t UpperCount(int level_idx) const = 0;

    virtual const uint8_t *Metadata() const = 0;
};

class MutableNodeHandle : public NodeHandle {
public:
    virtual NodeHeader *MutableHeader() = 0;

    virtual node_id_t *MutableLevel0Neighbors() = 0;
    virtual void SetLevel0Count(uint16_t count) = 0;

    virtual node_id_t *MutableUpperNeighbors(int level_idx) = 0;
    virtual void SetUpperCount(int level_idx, uint16_t count) = 0;

    // Optional direct pointer for backends that can expose persistent mapped
    // upper count storage. Default nullptr means caller should use SetUpperCount.
    virtual uint16_t *MutableUpperCounts() {
        return nullptr;
    }

    virtual const uint16_t *UpperCounts() const {
        return nullptr;
    }
};

class NodeStore {
public:
    virtual ~NodeStore() = default;

    virtual node_id_t AllocateNode(row_id_t row_id, const float *vec, uint32_t dim, uint8_t level) = 0;
    virtual void FreeNode(node_id_t node_id) = 0;

    virtual std::unique_ptr<NodeHandle> PinNode(node_id_t node_id) const = 0;
    virtual std::unique_ptr<MutableNodeHandle> PinNodeForUpdate(node_id_t node_id) = 0;

    virtual uint32_t GetDimension() const = 0;
    virtual int GetM() const = 0;
    virtual uint64_t GetNodeCount() const = 0;

    virtual void ForEachNode(std::function<void(node_id_t)> cb) const = 0;

    virtual void PrepareParallelAccess() {}
    virtual void FinishParallelAccess() {}
};

} // namespace vex

#endif // VEX_NODE_STORE_HPP
