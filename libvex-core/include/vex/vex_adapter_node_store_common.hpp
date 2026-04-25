#ifndef VEX_ADAPTER_NODE_STORE_COMMON_HPP
#define VEX_ADAPTER_NODE_STORE_COMMON_HPP

#include "vex/vex_node_store_memory.hpp"

#include <array>
#include <functional>
#include <memory>
#include <utility>

namespace vex {

class AdapterNodeBackend {
public:
    virtual ~AdapterNodeBackend() = default;

    virtual node_id_t AllocateNode(row_id_t row_id, const float *vec, uint32_t dim, uint8_t level) = 0;
    virtual void FreeNode(node_id_t node_id) = 0;

    virtual std::unique_ptr<NodeHandle> PinNode(node_id_t node_id) const = 0;
    virtual std::unique_ptr<MutableNodeHandle> PinNodeForUpdate(node_id_t node_id) = 0;

    virtual uint32_t GetDimension() const = 0;
    virtual int GetM() const = 0;
    virtual uint64_t GetNodeCount() const = 0;
    virtual void ForEachNode(std::function<void(node_id_t)> cb) const = 0;
};

struct AdapterNodeLayoutView {
    NodeHeader *header = nullptr;
    const float *vector = nullptr;

    node_id_t *level0_neighbors = nullptr;
    uint16_t *level0_count = nullptr;

    // Flattened layout: [level1 neighbors][level2 neighbors]...
    node_id_t *upper_neighbors_base = nullptr;
    uint16_t *upper_counts = nullptr;

    uint8_t *metadata = nullptr;

    // Backend-managed opaque pin token.
    void *opaque = nullptr;
};

class AdapterLowLevelBinding {
public:
    virtual ~AdapterLowLevelBinding() = default;

    virtual node_id_t AllocateNode(row_id_t row_id, const float *vec, uint32_t dim, uint8_t level) = 0;
    virtual void FreeNode(node_id_t node_id) = 0;

    // for_update=true should pin mutable buffers and return writable pointers in `out`.
    virtual bool PinNode(node_id_t node_id, bool for_update, AdapterNodeLayoutView &out) = 0;
    virtual void UnpinNode(AdapterNodeLayoutView &view) = 0;

    virtual uint32_t GetDimension() const = 0;
    virtual int GetM() const = 0;
    virtual uint64_t GetNodeCount() const = 0;
    virtual void ForEachNode(std::function<void(node_id_t)> cb) const = 0;

    // Optional graph-state persistence hooks. Main path should use backend metadata pages,
    // not core snapshot transfer. Defaults keep current behavior unchanged.
    virtual bool LoadGraphState(bool &, node_id_t &, int &, uint64_t &) const {
        return false;
    }
    virtual bool StoreGraphState(bool, node_id_t, int, uint64_t) {
        return false;
    }

    // Optional backend-specific lookup helpers for adapter-layer orchestration.
    virtual bool ResolveNodeIdByRowId(row_id_t, node_id_t &) const {
        return false;
    }
    virtual bool ResolveStorageNodeKey(node_id_t, uint64_t &) const {
        return false;
    }
};

class AdapterMemoryBackend final : public AdapterNodeBackend {
public:
    explicit AdapterMemoryBackend(uint32_t dimension, int m, uint32_t metadata_size)
        : memory_store_(dimension, m, metadata_size) {
    }

    node_id_t AllocateNode(row_id_t row_id, const float *vec, uint32_t dim, uint8_t level) override {
        return memory_store_.AllocateNode(row_id, vec, dim, level);
    }

    void FreeNode(node_id_t node_id) override {
        memory_store_.FreeNode(node_id);
    }

    std::unique_ptr<NodeHandle> PinNode(node_id_t node_id) const override {
        return memory_store_.PinNode(node_id);
    }

    std::unique_ptr<MutableNodeHandle> PinNodeForUpdate(node_id_t node_id) override {
        return memory_store_.PinNodeForUpdate(node_id);
    }

    uint32_t GetDimension() const override {
        return memory_store_.GetDimension();
    }

    int GetM() const override {
        return memory_store_.GetM();
    }

    uint64_t GetNodeCount() const override {
        return memory_store_.GetNodeCount();
    }

    void ForEachNode(std::function<void(node_id_t)> cb) const override {
        memory_store_.ForEachNode(std::move(cb));
    }

private:
    MemoryNodeStore memory_store_;
};

class AdapterDirectBackend final : public AdapterNodeBackend {
public:
    explicit AdapterDirectBackend(std::shared_ptr<AdapterLowLevelBinding> binding)
        : binding_(std::move(binding)) {
    }

    node_id_t AllocateNode(row_id_t row_id, const float *vec, uint32_t dim, uint8_t level) override {
        return binding_->AllocateNode(row_id, vec, dim, level);
    }

    void FreeNode(node_id_t node_id) override {
        binding_->FreeNode(node_id);
    }

    std::unique_ptr<NodeHandle> PinNode(node_id_t node_id) const override {
        AdapterNodeLayoutView view{};
        if (!binding_ || !const_cast<AdapterLowLevelBinding *>(binding_.get())->PinNode(node_id, false, view)) {
            return nullptr;
        }
        return std::unique_ptr<NodeHandle>(new DirectNodeHandle(binding_, std::move(view), binding_->GetM(), false));
    }

    std::unique_ptr<MutableNodeHandle> PinNodeForUpdate(node_id_t node_id) override {
        AdapterNodeLayoutView view{};
        if (!binding_ || !binding_->PinNode(node_id, true, view)) {
            return nullptr;
        }
        return std::unique_ptr<MutableNodeHandle>(
            new DirectNodeHandle(binding_, std::move(view), binding_->GetM(), true));
    }

    uint32_t GetDimension() const override {
        return binding_->GetDimension();
    }

    int GetM() const override {
        return binding_->GetM();
    }

    uint64_t GetNodeCount() const override {
        return binding_->GetNodeCount();
    }

    void ForEachNode(std::function<void(node_id_t)> cb) const override {
        binding_->ForEachNode(std::move(cb));
    }

private:
    class DirectNodeHandle final : public MutableNodeHandle {
    public:
        DirectNodeHandle(std::shared_ptr<AdapterLowLevelBinding> binding,
                         AdapterNodeLayoutView view,
                         int m,
                         bool writable)
            : binding_(std::move(binding)),
              view_(std::move(view)),
              m_(m),
              writable_(writable),
              fallback_upper_counts_ready_(false) {
        }

        ~DirectNodeHandle() override {
            if (binding_) {
                binding_->UnpinNode(view_);
            }
        }

        const NodeHeader *Header() const override {
            return view_.header;
        }

        const float *Vector() const override {
            return view_.vector;
        }

        const node_id_t *Level0Neighbors() const override {
            return view_.level0_neighbors;
        }

        uint16_t Level0Count() const override {
            return view_.level0_count ? *view_.level0_count : 0;
        }

        const node_id_t *UpperNeighbors(int level_idx) const override {
            if (level_idx < 0 || level_idx >= HNSW_MAX_UPPER_LEVELS || view_.upper_neighbors_base == nullptr) {
                return nullptr;
            }
            return view_.upper_neighbors_base + static_cast<size_t>(level_idx) * static_cast<size_t>(m_);
        }

        uint16_t UpperCount(int level_idx) const override {
            if (level_idx < 0 || level_idx >= HNSW_MAX_UPPER_LEVELS) {
                return 0;
            }
            auto *counts = ResolveUpperCountsConst();
            return counts ? counts[static_cast<size_t>(level_idx)] : 0;
        }

        const uint8_t *Metadata() const override {
            return view_.metadata;
        }

        NodeHeader *MutableHeader() override {
            return writable_ ? view_.header : nullptr;
        }

        node_id_t *MutableLevel0Neighbors() override {
            return writable_ ? view_.level0_neighbors : nullptr;
        }

        void SetLevel0Count(uint16_t count) override {
            if (writable_ && view_.level0_count) {
                *view_.level0_count = count;
            }
        }

        node_id_t *MutableUpperNeighbors(int level_idx) override {
            if (!writable_) {
                return nullptr;
            }
            return const_cast<node_id_t *>(UpperNeighbors(level_idx));
        }

        void SetUpperCount(int level_idx, uint16_t count) override {
            if (!writable_ || level_idx < 0 || level_idx >= HNSW_MAX_UPPER_LEVELS) {
                return;
            }
            auto *counts = ResolveUpperCountsMutable();
            if (!counts) {
                return;
            }
            counts[static_cast<size_t>(level_idx)] = count;
        }

        uint16_t *MutableUpperCounts() override {
            return writable_ ? ResolveUpperCountsMutable() : nullptr;
        }

        const uint16_t *UpperCounts() const override {
            return ResolveUpperCountsConst();
        }

    private:
        uint16_t *ResolveUpperCountsMutable() {
            if (view_.upper_counts) {
                return view_.upper_counts;
            }
            return const_cast<uint16_t *>(ResolveUpperCountsConst());
        }

        const uint16_t *ResolveUpperCountsConst() const {
            if (view_.upper_counts) {
                return view_.upper_counts;
            }
            if (!fallback_upper_counts_ready_) {
                fallback_upper_counts_.fill(0);
                fallback_upper_counts_ready_ = true;
            }
            return fallback_upper_counts_.data();
        }

    private:
        std::shared_ptr<AdapterLowLevelBinding> binding_;
        AdapterNodeLayoutView view_;
        int m_;
        bool writable_;
        mutable bool fallback_upper_counts_ready_;
        mutable std::array<uint16_t, HNSW_MAX_UPPER_LEVELS> fallback_upper_counts_{};
    };

private:
    std::shared_ptr<AdapterLowLevelBinding> binding_;
};

struct AdapterNodeStoreConfig {
    uint32_t dimension = 0;
    int m = 16;
    uint32_t metadata_size = 0;

    std::shared_ptr<AdapterNodeBackend> backend;
    std::shared_ptr<AdapterLowLevelBinding> low_level_binding;
};

class AdapterNodeStore : public NodeStore {
public:
    explicit AdapterNodeStore(const AdapterNodeStoreConfig &cfg) : cfg_(cfg) {
        if (cfg_.backend) {
            backend_ = cfg_.backend;
        } else if (cfg_.low_level_binding) {
            backend_ = std::make_shared<AdapterDirectBackend>(cfg_.low_level_binding);
        } else {
            backend_ = std::make_shared<AdapterMemoryBackend>(cfg_.dimension, cfg_.m, cfg_.metadata_size);
        }
    }

    node_id_t AllocateNode(row_id_t row_id, const float *vec, uint32_t dim, uint8_t level) override {
        return backend_->AllocateNode(row_id, vec, dim, level);
    }

    void FreeNode(node_id_t node_id) override {
        backend_->FreeNode(node_id);
    }

    std::unique_ptr<NodeHandle> PinNode(node_id_t node_id) const override {
        return backend_->PinNode(node_id);
    }

    std::unique_ptr<MutableNodeHandle> PinNodeForUpdate(node_id_t node_id) override {
        return backend_->PinNodeForUpdate(node_id);
    }

    uint32_t GetDimension() const override {
        return backend_->GetDimension();
    }

    int GetM() const override {
        return backend_->GetM();
    }

    uint64_t GetNodeCount() const override {
        return backend_->GetNodeCount();
    }

    void ForEachNode(std::function<void(node_id_t)> cb) const override {
        backend_->ForEachNode(std::move(cb));
    }

    const AdapterNodeStoreConfig &Config() const {
        return cfg_;
    }

private:
    AdapterNodeStoreConfig cfg_;
    std::shared_ptr<AdapterNodeBackend> backend_;
};

} // namespace vex

#endif // VEX_ADAPTER_NODE_STORE_COMMON_HPP
