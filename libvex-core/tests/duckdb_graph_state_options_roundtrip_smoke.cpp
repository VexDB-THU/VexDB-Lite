#include "vex/vex_adapter_graph_state.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {

struct SimpleValue {
    enum class Kind { UBIGINT, INTEGER };

    Kind kind;
    uint64_t u64;
    int i32;

    static SimpleValue UBIGINT(uint64_t v) {
        return SimpleValue{Kind::UBIGINT, v, 0};
    }
    static SimpleValue INTEGER(int v) {
        return SimpleValue{Kind::INTEGER, 0, v};
    }

    template <typename T>
    T GetValue() const;
};

template <>
uint64_t SimpleValue::GetValue<uint64_t>() const {
    return u64;
}

template <>
int SimpleValue::GetValue<int>() const {
    return i32;
}

using SimpleOptions = std::unordered_map<std::string, SimpleValue>;

struct DuckDBCoreGraphState {
    bool has_entry_point = false;
    ::vex::node_id_t entry_point = ::vex::INVALID_NODE_ID;
    int max_level = 0;
    uint64_t node_count = 0;
};

static bool LoadDuckDBGraphStateFromOptions(const SimpleOptions &options, DuckDBCoreGraphState &out) {
    auto nc_it = options.find("node_count");
    auto ml_it = options.find("max_level");
    auto ep_it = options.find("entry_point");
    if (nc_it == options.end() || ml_it == options.end() || ep_it == options.end()) {
        return false;
    }

    out.node_count = nc_it->second.GetValue<uint64_t>();
    out.max_level = ml_it->second.GetValue<int>();
    out.entry_point = static_cast<::vex::node_id_t>(ep_it->second.GetValue<uint64_t>());
    out.has_entry_point = (out.node_count > 0 && out.entry_point != ::vex::INVALID_NODE_ID);
    return true;
}

static bool StoreDuckDBGraphStateToOptions(SimpleOptions &options, const DuckDBCoreGraphState &in) {
    options["node_count"] = SimpleValue::UBIGINT(in.node_count);
    options["max_level"] = SimpleValue::INTEGER(in.max_level);
    options["entry_point"] = SimpleValue::UBIGINT(static_cast<uint64_t>(in.entry_point));
    return true;
}

} // namespace

int main() {
    vex::AdapterGraphState src{};
    src.has_entry_point = true;
    src.entry_point = 88;
    src.max_level = 5;
    src.node_count = 1024;

    DuckDBCoreGraphState wire{};
    wire.has_entry_point = src.has_entry_point;
    wire.entry_point = src.entry_point;
    wire.max_level = src.max_level;
    wire.node_count = src.node_count;

    SimpleOptions options;
    if (!StoreDuckDBGraphStateToOptions(options, wire)) {
        std::cerr << "store options failed" << std::endl;
        return 1;
    }

    DuckDBCoreGraphState restored_wire{};
    if (!LoadDuckDBGraphStateFromOptions(options, restored_wire)) {
        std::cerr << "load options failed" << std::endl;
        return 2;
    }

    vex::AdapterGraphState restored{};
    restored.has_entry_point = restored_wire.has_entry_point;
    restored.entry_point = restored_wire.entry_point;
    restored.max_level = restored_wire.max_level;
    restored.node_count = restored_wire.node_count;

    if (restored.has_entry_point != src.has_entry_point ||
        restored.entry_point != src.entry_point ||
        restored.max_level != src.max_level ||
        restored.node_count != src.node_count) {
        std::cerr << "graph state options roundtrip mismatch" << std::endl;
        return 3;
    }

    return 0;
}
