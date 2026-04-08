#include "vex_extension.hpp"
#include "vex_functions.hpp"
#include "vex_graph_index.hpp"
#ifdef VEX_ENABLE_HYBRID_INDEX
#include "vex_hybrid_index.hpp"
#endif
#ifdef VEX_ENABLE_OPTIMIZER
#include "vex_optimizer.hpp"
#endif

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/execution/index/index_type_set.hpp"

namespace duckdb {

static void RegisterIndexTypes(DBConfig &config) {
	IndexType graph_index_type;
	graph_index_type.name = GraphIndex::TYPE_NAME;
	graph_index_type.create_instance = GraphIndex::Create;
	graph_index_type.create_plan = GraphIndex::CreatePlan;

#ifdef VEX_ENABLE_HYBRID_INDEX
	IndexType hybrid_index_type;
	hybrid_index_type.name = HybridIndex::TYPE_NAME;
	hybrid_index_type.create_instance = HybridIndex::Create;
	hybrid_index_type.create_plan = HybridIndex::CreatePlan;
#endif

	// Always register directly to config (works in all contexts including unittest)
	try { config.GetIndexTypes().RegisterIndexType(graph_index_type); } catch (...) {}
#ifdef VEX_ENABLE_HYBRID_INDEX
	try { config.GetIndexTypes().RegisterIndexType(hybrid_index_type); } catch (...) {}
#endif

#ifdef VEX_HAS_GLOBAL_INDEX_REGISTRY
	// Also register to global registry for FinalizeLoad() reload path
	GlobalIndexTypeRegistry::GetInstance().RegisterIndexType(graph_index_type);
#ifdef VEX_ENABLE_HYBRID_INDEX
	GlobalIndexTypeRegistry::GetInstance().RegisterIndexType(hybrid_index_type);
#endif
#endif
}

static void LoadInternal(ExtensionLoader &loader) {
	VexFunctions::Register(loader);

	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);

	RegisterIndexTypes(config);

#ifdef VEX_ENABLE_OPTIMIZER
	config.GetCallbackManager().Register(VexOptimizerExtension());
#endif

	// Register runtime configuration options
	config.AddExtensionOption("vex_ef_search",
	                          "Search expansion factor for VEX graph index (higher = better recall, slower)",
	                          LogicalType::INTEGER, Value::INTEGER(GraphIndexConfig::DEFAULT_EF_SEARCH));
	config.AddExtensionOption("vex_brute_force_threshold",
	                          "Node count threshold below which brute-force search is used instead of graph traversal",
	                          LogicalType::UBIGINT, Value::UBIGINT(GraphIndexCore::BRUTE_FORCE_THRESHOLD));
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
