#include "vex_extension.hpp"
#include "vex_types.hpp"
#include "vex_functions.hpp"
#include "vex_graph_index.hpp"
#include "vex_hybrid_index.hpp"
#include "vex_optimizer.hpp"

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/execution/index/index_type_set.hpp"

namespace duckdb {

static void RegisterIndexTypes(DBConfig &config) {
	IndexType graph_index_type;
	graph_index_type.name = GraphIndex::TYPE_NAME;
	graph_index_type.create_instance = GraphIndex::Create;
	graph_index_type.create_plan = GraphIndex::CreatePlan;

	IndexType hybrid_index_type;
	hybrid_index_type.name = HybridIndex::TYPE_NAME;
	hybrid_index_type.create_instance = HybridIndex::Create;
	hybrid_index_type.create_plan = HybridIndex::CreatePlan;

#ifdef VEX_HAS_GLOBAL_INDEX_REGISTRY
	GlobalIndexTypeRegistry::GetInstance().RegisterIndexType(graph_index_type);
	GlobalIndexTypeRegistry::GetInstance().RegisterIndexType(hybrid_index_type);
#else
	config.GetIndexTypes().RegisterIndexType(graph_index_type);
	config.GetIndexTypes().RegisterIndexType(hybrid_index_type);
#endif
}

static void LoadInternal(ExtensionLoader &loader) {
	VexTypes::Register(loader);
	VexFunctions::Register(loader);

	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);

	RegisterIndexTypes(config);

	config.optimizer_extensions.push_back(VexOptimizerExtension());

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
