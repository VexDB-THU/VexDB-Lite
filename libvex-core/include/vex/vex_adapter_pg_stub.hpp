#ifndef VEX_ADAPTER_PG_STUB_HPP
#define VEX_ADAPTER_PG_STUB_HPP

#include "vex/vex_adapter_node_store_common.hpp"

#include <string>

namespace vex {

using PGNodeBackend = AdapterNodeBackend;
using PGNodeLayoutView = AdapterNodeLayoutView;
using PGLowLevelBinding = AdapterLowLevelBinding;
using PGMemoryBackend = AdapterMemoryBackend;
using PGDirectBackend = AdapterDirectBackend;

struct PGNodeStoreConfig : public AdapterNodeStoreConfig {
    // Reserved for future PG direct binding fields.
    void *pg_relation = nullptr;
    void *pg_buffer_manager = nullptr;
    std::string index_name;
};

class PGNodeStore final : public AdapterNodeStore {
public:
    explicit PGNodeStore(const PGNodeStoreConfig &cfg)
        : AdapterNodeStore(cfg),
          cfg_(cfg) {
    }

    const PGNodeStoreConfig &Config() const {
        return cfg_;
    }

private:
    PGNodeStoreConfig cfg_;
};

// Backward-compatible alias for early tests and callers.
using PGNodeStoreStub = PGNodeStore;

} // namespace vex

#endif // VEX_ADAPTER_PG_STUB_HPP
