#include "vex/vex_adapter_low_level_binding_mock.hpp"

#include <array>
#include <memory>
#include <utility>

namespace vex {
namespace {

struct MockPinContextBase {
    virtual ~MockPinContextBase() = default;
    virtual uint16_t *UpperCountsMutable() = 0;
    virtual const uint16_t *UpperCounts() const = 0;
};

struct MockPinContextRead final : public MockPinContextBase {
    explicit MockPinContextRead(std::unique_ptr<NodeHandle> h)
        : handle(std::move(h)) {
        upper_counts.fill(0);
        if (handle) {
            for (int i = 0; i < HNSW_MAX_UPPER_LEVELS; i++) {
                upper_counts[static_cast<size_t>(i)] = handle->UpperCount(i);
            }
        }
    }

    uint16_t *UpperCountsMutable() override {
        return upper_counts.data();
    }

    const uint16_t *UpperCounts() const override {
        return upper_counts.data();
    }

    std::unique_ptr<NodeHandle> handle;
    std::array<uint16_t, HNSW_MAX_UPPER_LEVELS> upper_counts;
};

struct MockPinContextWrite final : public MockPinContextBase {
    explicit MockPinContextWrite(std::unique_ptr<MutableNodeHandle> h)
        : handle(std::move(h)) {
    }

    uint16_t *UpperCountsMutable() override {
        return handle->MutableUpperCounts();
    }

    const uint16_t *UpperCounts() const override {
        return handle->UpperCounts();
    }

    std::unique_ptr<MutableNodeHandle> handle;
};

} // namespace

AdapterLowLevelBindingMock::AdapterLowLevelBindingMock(uint32_t dimension, int m, uint32_t metadata_size)
    : store_(dimension, m, metadata_size) {
}

node_id_t AdapterLowLevelBindingMock::AllocateNode(row_id_t row_id, const float *vec, uint32_t dim, uint8_t level) {
    auto node_id = store_.AllocateNode(row_id, vec, dim, level);
    row_to_node_id_[row_id] = node_id;
    return node_id;
}

void AdapterLowLevelBindingMock::FreeNode(node_id_t node_id) {
    auto handle = store_.PinNode(node_id);
    if (handle && handle->Header()) {
        row_to_node_id_.erase(handle->Header()->row_id);
    }
    store_.FreeNode(node_id);
}

bool AdapterLowLevelBindingMock::PinNode(node_id_t node_id, bool for_update, AdapterNodeLayoutView &out) {
    out = {};

    if (for_update) {
        auto handle = store_.PinNodeForUpdate(node_id);
        if (!handle || !handle->Header()) {
            return false;
        }

        auto ctx = std::unique_ptr<MockPinContextWrite>(new MockPinContextWrite(std::move(handle)));
        auto *mh = ctx->handle.get();

        out.header = mh->MutableHeader();
        out.vector = mh->Vector();
        out.level0_neighbors = mh->MutableLevel0Neighbors();
        out.level0_count = out.header ? &out.header->level0_count : nullptr;
        out.upper_neighbors_base = mh->MutableUpperNeighbors(0);
        out.upper_counts = ctx->UpperCountsMutable();
        out.metadata = const_cast<uint8_t *>(mh->Metadata());
        out.opaque = ctx.release();
        return true;
    }

    auto handle = store_.PinNode(node_id);
    if (!handle || !handle->Header()) {
        return false;
    }

    auto ctx = std::unique_ptr<MockPinContextRead>(new MockPinContextRead(std::move(handle)));
    auto *h = ctx->handle.get();

    out.header = const_cast<NodeHeader *>(h->Header());
    out.vector = h->Vector();
    out.level0_neighbors = const_cast<node_id_t *>(h->Level0Neighbors());
    out.level0_count = out.header ? &out.header->level0_count : nullptr;
    out.upper_neighbors_base = const_cast<node_id_t *>(h->UpperNeighbors(0));
    out.upper_counts = const_cast<uint16_t *>(ctx->UpperCounts());
    out.metadata = const_cast<uint8_t *>(h->Metadata());
    out.opaque = ctx.release();
    return true;
}

void AdapterLowLevelBindingMock::UnpinNode(AdapterNodeLayoutView &view) {
    if (!view.opaque) {
        return;
    }

    auto *ctx = reinterpret_cast<MockPinContextBase *>(view.opaque);
    delete ctx;

    view = {};
}

uint32_t AdapterLowLevelBindingMock::GetDimension() const {
    return store_.GetDimension();
}

int AdapterLowLevelBindingMock::GetM() const {
    return store_.GetM();
}

uint64_t AdapterLowLevelBindingMock::GetNodeCount() const {
    return store_.GetNodeCount();
}

void AdapterLowLevelBindingMock::ForEachNode(std::function<void(node_id_t)> cb) const {
    store_.ForEachNode(std::move(cb));
}

bool AdapterLowLevelBindingMock::ResolveNodeIdByRowId(row_id_t row_id, node_id_t &node_id) const {
    auto it = row_to_node_id_.find(row_id);
    if (it == row_to_node_id_.end()) {
        return false;
    }
    node_id = it->second;
    return true;
}

bool AdapterLowLevelBindingMock::ResolveStorageNodeKey(node_id_t node_id, uint64_t &storage_key) const {
    storage_key = static_cast<uint64_t>(node_id);
    return true;
}

} // namespace vex
