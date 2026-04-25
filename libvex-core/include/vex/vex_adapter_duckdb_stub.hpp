#ifndef VEX_ADAPTER_DUCKDB_STUB_HPP
#define VEX_ADAPTER_DUCKDB_STUB_HPP

#include "vex/vex_adapter_node_store_common.hpp"

#include <string>

namespace vex {

using DuckDBNodeBackend = AdapterNodeBackend;
using DuckDBNodeLayoutView = AdapterNodeLayoutView;
using DuckDBLowLevelBinding = AdapterLowLevelBinding;
using DuckDBMemoryBackend = AdapterMemoryBackend;
using DuckDBDirectBackend = AdapterDirectBackend;

struct DuckDBNodeStoreConfig : public AdapterNodeStoreConfig {
    // Reserved for future direct binding fields.
    void *duckdb_block_manager = nullptr;
    void *duckdb_buffer_manager = nullptr;
    std::string index_name;
};

class DuckDBNodeStore final : public AdapterNodeStore {
public:
    explicit DuckDBNodeStore(const DuckDBNodeStoreConfig &cfg)
        : AdapterNodeStore(cfg),
          cfg_(cfg) {
    }

    const DuckDBNodeStoreConfig &Config() const {
        return cfg_;
    }

private:
    DuckDBNodeStoreConfig cfg_;
};

// Backward-compatible name for current tests and callers.
using DuckDBNodeStoreStub = DuckDBNodeStore;

} // namespace vex

#endif // VEX_ADAPTER_DUCKDB_STUB_HPP
