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

static void LoadInternal(ExtensionLoader &loader) {
	// Register vector types
	VexTypes::Register(loader);

	// Register vector functions
	VexFunctions::Register(loader);

	// Register GRAPH_INDEX type with the global registry
	// This ensures it's available in all DatabaseInstances
	IndexType graph_index_type;
	graph_index_type.name = GraphIndex::TYPE_NAME;
	graph_index_type.create_instance = GraphIndex::Create;
	graph_index_type.create_plan = GraphIndex::CreatePlan;

	GlobalIndexTypeRegistry::GetInstance().RegisterIndexType(graph_index_type);

	// Register HYBRID_INDEX type
	IndexType hybrid_index_type;
	hybrid_index_type.name = HybridIndex::TYPE_NAME;
	hybrid_index_type.create_instance = HybridIndex::Create;
	hybrid_index_type.create_plan = HybridIndex::CreatePlan;

	GlobalIndexTypeRegistry::GetInstance().RegisterIndexType(hybrid_index_type);

	// Register optimizer extension for automatic ANN query optimization
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);
	config.optimizer_extensions.push_back(VexOptimizerExtension());
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
