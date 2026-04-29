#include "vex_extension.hpp"

#include "vex_functions.hpp"
#include "vex_graph_index.hpp"
#include "vex_optimizer.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/scalar_function.hpp"

#ifndef VEXDB_DUCK_GIT_HASH
#define VEXDB_DUCK_GIT_HASH "unknown"
#endif

#ifndef VEXDB_DUCK_BUILD_TIME
#define VEXDB_DUCK_BUILD_TIME "unknown"
#endif

namespace duckdb {

static void VexVersionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    result.SetValue(0, StringVector::AddString(
                           result, "pg_vexdb duck extension " VEXDB_DUCK_GIT_HASH " (" VEXDB_DUCK_BUILD_TIME ")"));
    result.SetVectorType(VectorType::CONSTANT_VECTOR);
}

static void RegisterIndexTypes(DBConfig &config) {
    IndexType graph_index_type;
    graph_index_type.name = GraphIndex::TYPE_NAME;
    graph_index_type.create_instance = GraphIndex::Create;
    graph_index_type.create_plan = GraphIndex::CreatePlan;
    config.GetIndexTypes().RegisterIndexType(std::move(graph_index_type));
}

void LoadInternal(ExtensionLoader &loader) {
    VexFunctions::Register(loader);
    loader.RegisterFunction(ScalarFunction("vex_version", {}, LogicalType::VARCHAR, VexVersionFunction));

    auto &db = loader.GetDatabaseInstance();
    auto &config = DBConfig::GetConfig(db);

    RegisterIndexTypes(config);
    OptimizerExtension::Register(config, VexOptimizerExtension());

    config.AddExtensionOption("vex_ef_search", "Search expansion factor for GRAPH_INDEX.",
                              LogicalType::INTEGER, Value::INTEGER(64));
    config.AddExtensionOption("vex_brute_force_threshold", "Temporary brute-force threshold for GRAPH_INDEX.",
                              LogicalType::UBIGINT, Value::UBIGINT(64));
}

void VexExtension::Load(ExtensionLoader &loader) {
    LoadInternal(loader);
}

std::string VexExtension::Name() {
    return "vex";
}

std::string VexExtension::Version() const {
    return "0.1.0";
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(vex, loader) {
    duckdb::LoadInternal(loader);
}
}
