#include "vex_extension.hpp"
#include "vex_duckdb_compat.hpp"
#include "vex_functions.hpp"
#include "vex_graph_index.hpp"
#ifdef VEX_ENABLE_OPTIMIZER
#include "vex_optimizer.hpp"
#endif

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/scalar_function.hpp"

#ifndef VEX_GIT_HASH
#define VEX_GIT_HASH "unknown"
#endif
#ifndef VEX_BUILD_TIME
#define VEX_BUILD_TIME "unknown"
#endif

namespace duckdb {

static void VexVersionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetValue(0, StringVector::AddString(result,
	    "VexDB-Lite " VEX_GIT_HASH " (built " VEX_BUILD_TIME ")"));
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
}

static void RegisterIndexTypes(DBConfig &config) {
	IndexType graph_index_type;
	graph_index_type.name = GraphIndex::TYPE_NAME;
	graph_index_type.create_instance = GraphIndex::Create;
	graph_index_type.create_plan = GraphIndex::CreatePlan;

	// Always register directly to config (works in all contexts including unittest)
	try { config.GetIndexTypes().RegisterIndexType(graph_index_type); } catch (...) {}
}

static void LoadInternal(ExtensionLoader &loader) {
	VexFunctions::Register(loader);

	// Register vex_version() scalar function
	loader.RegisterFunction(ScalarFunction("vex_version", {}, LogicalType::VARCHAR, VexVersionFunction));

	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);

	RegisterIndexTypes(config);

#ifdef VEX_ENABLE_OPTIMIZER
	vex_compat::RegisterOptimizerExtension(config, VexOptimizerExtension());
#endif

	// Register runtime configuration options
	config.AddExtensionOption("vex_ef_search",
	                          "Search expansion factor for VEX graph index (higher = better recall, slower)",
	                          LogicalType::INTEGER, Value::INTEGER(GraphIndexConfig::DEFAULT_EF_SEARCH));
	config.AddExtensionOption("vex_brute_force_threshold",
	                          "Node count threshold below which brute-force search is used instead of graph traversal",
	                          LogicalType::UBIGINT, Value::UBIGINT(GraphIndexCore::BRUTE_FORCE_THRESHOLD));
	config.AddExtensionOption("vex_memory_budget",
	                          "Memory budget in bytes for VEX indexes (0 = unlimited)",
	                          LogicalType::BIGINT, Value::BIGINT(0));
	config.AddExtensionOption("vex_enable_eviction",
	                          "Evict clean index buffers after search to reduce memory (default: false)",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));
}

void VexExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string VexExtension::Name() {
	return "vex";
}

std::string VexExtension::Version() const {
#ifdef EXT_VERSION_VEX
	return EXT_VERSION_VEX;
#else
	return "0.1.0";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(vex, loader) {
	duckdb::LoadInternal(loader);
}
}
