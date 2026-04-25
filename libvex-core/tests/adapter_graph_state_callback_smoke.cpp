#include "vex/vex_adapter_duckdb_stub.hpp"

#include <cstdint>
#include <iostream>
#include <memory>

namespace {

class CallbackOnlyBinding final : public vex::DuckDBLowLevelBinding {
public:
    explicit CallbackOnlyBinding(uint32_t dim, int m)
        : dim_(dim),
          m_(m) {
    }

    vex::node_id_t AllocateNode(vex::row_id_t, const float *, uint32_t, uint8_t) override {
        return vex::INVALID_NODE_ID;
    }

    void FreeNode(vex::node_id_t) override {
    }

    bool PinNode(vex::node_id_t, bool, vex::DuckDBNodeLayoutView &out) override {
        out = {};
        return false;
    }

    void UnpinNode(vex::DuckDBNodeLayoutView &view) override {
        view = {};
    }

    uint32_t GetDimension() const override {
        return dim_;
    }

    int GetM() const override {
        return m_;
    }

    uint64_t GetNodeCount() const override {
        return 0;
    }

    void ForEachNode(std::function<void(vex::node_id_t)> cb) const override {
        (void)cb;
    }

    bool LoadGraphState(bool &has_entry_point, vex::node_id_t &entry_point, int &max_level,
                        uint64_t &node_count) const override {
        has_entry_point = loaded_has_entry_;
        entry_point = loaded_entry_;
        max_level = loaded_level_;
        node_count = loaded_nodes_;
        return true;
    }

    bool StoreGraphState(bool has_entry_point, vex::node_id_t entry_point, int max_level,
                         uint64_t node_count) override {
        stored_called_ = true;
        stored_has_entry_ = has_entry_point;
        stored_entry_ = entry_point;
        stored_level_ = max_level;
        stored_nodes_ = node_count;
        return true;
    }

    bool StoredCalled() const {
        return stored_called_;
    }

    bool StoredHasEntry() const {
        return stored_has_entry_;
    }

    vex::node_id_t StoredEntry() const {
        return stored_entry_;
    }

    int StoredLevel() const {
        return stored_level_;
    }

    uint64_t StoredNodes() const {
        return stored_nodes_;
    }

private:
    uint32_t dim_;
    int m_;

    bool loaded_has_entry_ = true;
    vex::node_id_t loaded_entry_ = 42;
    int loaded_level_ = 3;
    uint64_t loaded_nodes_ = 99;

    bool stored_called_ = false;
    bool stored_has_entry_ = false;
    vex::node_id_t stored_entry_ = vex::INVALID_NODE_ID;
    int stored_level_ = 0;
    uint64_t stored_nodes_ = 0;
};

} // namespace

int main() {
    auto binding = std::make_shared<CallbackOnlyBinding>(4, 8);
    vex::DuckDBNodeStoreConfig cfg{};
    cfg.dimension = 4;
    cfg.m = 8;
    cfg.low_level_binding = binding;

    vex::DuckDBNodeStore store(cfg);
    (void)store;

    bool has_entry = false;
    vex::node_id_t entry = vex::INVALID_NODE_ID;
    int max_level = -1;
    uint64_t node_count = 0;

    if (!binding->LoadGraphState(has_entry, entry, max_level, node_count)) {
        std::cerr << "load graph state failed" << std::endl;
        return 1;
    }

    if (!has_entry || entry != 42 || max_level != 3 || node_count != 99) {
        std::cerr << "load graph state mismatch" << std::endl;
        return 2;
    }

    if (!binding->StoreGraphState(true, 77, 5, 1234)) {
        std::cerr << "store graph state failed" << std::endl;
        return 3;
    }

    if (!binding->StoredCalled() || !binding->StoredHasEntry() ||
        binding->StoredEntry() != 77 || binding->StoredLevel() != 5 ||
        binding->StoredNodes() != 1234) {
        std::cerr << "store graph state mismatch" << std::endl;
        return 4;
    }

    return 0;
}
