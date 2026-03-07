#include "duckdb/execution/index/index_type_set.hpp"
#include "duckdb/execution/index/art/art.hpp"

namespace duckdb {

// ============================================================
// Global Index Type Registry for Extensions
// ============================================================

GlobalIndexTypeRegistry &GlobalIndexTypeRegistry::GetInstance() {
	static GlobalIndexTypeRegistry instance;
	return instance;
}

void GlobalIndexTypeRegistry::RegisterIndexType(const IndexType &index_type) {
	lock_guard<mutex> g(lock);
	extension_index_types[index_type.name] = index_type;
}

vector<IndexType> GlobalIndexTypeRegistry::GetExtensionIndexTypes() const {
	lock_guard<mutex> g(lock);
	vector<IndexType> result;
	result.reserve(extension_index_types.size());
	for (const auto &entry : extension_index_types) {
		result.push_back(entry.second);
	}
	return result;
}

// ============================================================
// IndexTypeSet
// ============================================================

IndexTypeSet::IndexTypeSet() {

	// Register the ART index type by default
	IndexType art_index_type;
	art_index_type.name = ART::TYPE_NAME;
	art_index_type.create_instance = ART::Create;
	art_index_type.create_plan = ART::CreatePlan;

	RegisterIndexType(art_index_type);

	// Load extension-registered index types from global registry
	auto extension_types = GlobalIndexTypeRegistry::GetInstance().GetExtensionIndexTypes();
	for (const auto &ext_type : extension_types) {
		RegisterIndexType(ext_type);
	}
}

optional_ptr<IndexType> IndexTypeSet::FindByName(const string &name) {
	lock_guard<mutex> g(lock);
	auto entry = functions.find(name);
	if (entry == functions.end()) {
		return nullptr;
	}
	return &entry->second;
}

void IndexTypeSet::RegisterIndexType(const IndexType &index_type) {
	lock_guard<mutex> g(lock);
	if (functions.find(index_type.name) != functions.end()) {
		throw CatalogException("Index type with name \"%s\" already exists!", index_type.name.c_str());
	}
	functions[index_type.name] = index_type;
}

} // namespace duckdb
