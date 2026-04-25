#include "vex/vex_adapter_duckdb_mock.hpp"
#include "vex/vex_adapter_duckdb_stub.hpp"
#include "vex/vex_adapter_graph_state.hpp"
#include "vex/vex_graph_algo.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace {

struct SharedGraphState {
    bool has_entry_point = false;
    vex::node_id_t entry_point = vex::INVALID_NODE_ID;
    int max_level = 0;
    uint64_t node_count = 0;
};

class PersistentDuckDBBinding final : public vex::DuckDBLowLevelBinding {
public:
    PersistentDuckDBBinding(std::shared_ptr<vex::DuckDBLowLevelBindingMock> delegate,
                            std::shared_ptr<SharedGraphState> state)
        : delegate_(std::move(delegate)),
          state_(std::move(state)) {
    }

    vex::node_id_t AllocateNode(vex::row_id_t row_id, const float *vec, uint32_t dim, uint8_t level) override {
        return delegate_->AllocateNode(row_id, vec, dim, level);
    }

    void FreeNode(vex::node_id_t node_id) override {
        delegate_->FreeNode(node_id);
    }

    bool PinNode(vex::node_id_t node_id, bool for_update, vex::DuckDBNodeLayoutView &out) override {
        return delegate_->PinNode(node_id, for_update, out);
    }

    void UnpinNode(vex::DuckDBNodeLayoutView &view) override {
        delegate_->UnpinNode(view);
    }

    uint32_t GetDimension() const override {
        return delegate_->GetDimension();
    }

    int GetM() const override {
        return delegate_->GetM();
    }

    uint64_t GetNodeCount() const override {
        return delegate_->GetNodeCount();
    }

    void ForEachNode(std::function<void(vex::node_id_t)> cb) const override {
        delegate_->ForEachNode(std::move(cb));
    }

    bool LoadGraphState(bool &has_entry_point, vex::node_id_t &entry_point, int &max_level,
                        uint64_t &node_count) const override {
        has_entry_point = state_->has_entry_point;
        entry_point = state_->entry_point;
        max_level = state_->max_level;
        node_count = state_->node_count;
        return true;
    }

    bool StoreGraphState(bool has_entry_point, vex::node_id_t entry_point, int max_level,
                         uint64_t node_count) override {
        state_->has_entry_point = has_entry_point;
        state_->entry_point = entry_point;
        state_->max_level = max_level;
        state_->node_count = node_count;
        return true;
    }

private:
    std::shared_ptr<vex::DuckDBLowLevelBindingMock> delegate_;
    std::shared_ptr<SharedGraphState> state_;
};

} // namespace

int main() {
    auto delegate = std::make_shared<vex::DuckDBLowLevelBindingMock>(4, 8, 0);
    auto state = std::make_shared<SharedGraphState>();

    auto binding_build = std::make_shared<PersistentDuckDBBinding>(delegate, state);
    vex::DuckDBNodeStoreConfig build_cfg{};
    build_cfg.dimension = 4;
    build_cfg.m = 8;
    build_cfg.metadata_size = 0;
    build_cfg.index_name = "duckdb_graph_state_roundtrip_build";
    build_cfg.low_level_binding = binding_build;

    vex::DuckDBNodeStore build_store(build_cfg);
    vex::HNSWGraph build_graph(build_store, vex::Metric::L2, build_cfg.m, 32);

    std::vector<float> data = {
        0.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
        2.0f, 2.0f, 2.0f, 2.0f,
    };

    build_graph.AddPoint(1000, &data[0], build_cfg.dimension);
    build_graph.AddPoint(1001, &data[4], build_cfg.dimension);
    build_graph.AddPoint(1002, &data[8], build_cfg.dimension);

    if (!vex::StoreGraphStateToBinding(*binding_build, build_graph)) {
        std::cerr << "duckdb state store failed" << std::endl;
        return 1;
    }

    auto binding_reload = std::make_shared<PersistentDuckDBBinding>(delegate, state);
    vex::DuckDBNodeStoreConfig reload_cfg = build_cfg;
    reload_cfg.index_name = "duckdb_graph_state_roundtrip_reload";
    reload_cfg.low_level_binding = binding_reload;
    vex::DuckDBNodeStore reload_store(reload_cfg);
    vex::HNSWGraph reload_graph(reload_store, vex::Metric::L2, reload_cfg.m, 32);

    vex::AdapterGraphState loaded_state{};
    if (!vex::LoadGraphStateFromBinding(*binding_reload, loaded_state)) {
        std::cerr << "duckdb state load failed" << std::endl;
        return 2;
    }
    vex::ApplyGraphState(reload_graph, loaded_state);

    if (!reload_graph.HasEntryPoint() || reload_graph.NodeCount() != 3) {
        std::cerr << "duckdb graph state restore mismatch" << std::endl;
        return 3;
    }

    float query[4] = {1.1f, 1.1f, 1.1f, 1.1f};
    std::vector<vex::row_id_t> rows;
    std::vector<float> dists;
    reload_graph.Search(query, 2, 16, rows, dists);
    if (rows.empty() || rows[0] != 1001) {
        std::cerr << "duckdb graph state roundtrip search mismatch" << std::endl;
        return 4;
    }

    return 0;
}
