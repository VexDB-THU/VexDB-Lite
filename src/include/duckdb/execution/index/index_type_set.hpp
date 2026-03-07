//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/index_type_set.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/index/index_type.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include <memory>

namespace duckdb {

// Global index type registry for extensions to register index types that
// should be available in all DatabaseInstances
class GlobalIndexTypeRegistry {
public:
	static GlobalIndexTypeRegistry &GetInstance();

	void RegisterIndexType(const IndexType &index_type);
	vector<IndexType> GetExtensionIndexTypes() const;

private:
	GlobalIndexTypeRegistry() = default;

	mutable mutex lock;
	case_insensitive_map_t<IndexType> extension_index_types;
};

class IndexTypeSet {
	mutex lock;
	case_insensitive_tree_t<IndexType> functions;

public:
	IndexTypeSet();
	DUCKDB_API optional_ptr<IndexType> FindByName(const string &name);
	DUCKDB_API void RegisterIndexType(const IndexType &index_type);
};

} // namespace duckdb
