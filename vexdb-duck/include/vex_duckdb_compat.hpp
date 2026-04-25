#pragma once

#include "duckdb/common/types/vector.hpp"
#include "duckdb/execution/index/index_type.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/storage/table/table_index_list.hpp"

#if __has_include("duckdb/common/vector/array_vector.hpp")
#include "duckdb/common/vector/array_vector.hpp"
#define VEX_DUCKDB_HAS_ARRAY_VECTOR_HEADER 1
#else
#define VEX_DUCKDB_HAS_ARRAY_VECTOR_HEADER 0
#endif

#if __has_include("duckdb/main/extension_callback_manager.hpp")
#include "duckdb/main/extension_callback_manager.hpp"
#define VEX_DUCKDB_HAS_CALLBACK_MANAGER 1
#else
#define VEX_DUCKDB_HAS_CALLBACK_MANAGER 0
#endif

#if VEX_DUCKDB_HAS_CALLBACK_MANAGER
#define VEX_DUCKDB_HAS_VERIFY_AND_TOSTRING 0
#define VEX_DUCKDB_HAS_GET_DATA_INTERNAL 1
#else
#define VEX_DUCKDB_HAS_VERIFY_AND_TOSTRING 1
#define VEX_DUCKDB_HAS_GET_DATA_INTERNAL 0
using TableIndex = idx_t;
using ProjectionIndex = idx_t;
#endif

namespace duckdb {
namespace vex_compat {

inline void RegisterOptimizerExtension(DBConfig &config, OptimizerExtension extension) {
#if VEX_DUCKDB_HAS_CALLBACK_MANAGER
	config.GetCallbackManager().Register(std::move(extension));
#else
	config.optimizer_extensions.push_back(std::move(extension));
#endif
}

template <class Callback>
inline void ForEachTableIndex(TableIndexList &index_list, Callback &&callback) {
#if VEX_DUCKDB_HAS_CALLBACK_MANAGER
	for (auto &index : index_list.Indexes()) {
		if (callback(index)) {
			break;
		}
	}
#else
	index_list.Scan([&](Index &index) { return callback(index); });
#endif
}

inline CreateIndexInput MakeCreateIndexInput(ClientContext &context, TableIOManager &table_io_manager,
                                             AttachedDatabase &db, IndexConstraintType constraint_type,
                                             const string &name, const vector<column_t> &column_ids,
                                             const vector<unique_ptr<Expression>> &unbound_expressions,
                                             const IndexStorageInfo &storage_info,
                                             const case_insensitive_map_t<Value> &options) {
#if VEX_DUCKDB_HAS_CALLBACK_MANAGER
	return CreateIndexInput(context, table_io_manager, db, constraint_type, name, column_ids, unbound_expressions,
	                        storage_info, options);
#else
	return CreateIndexInput(table_io_manager, db, constraint_type, name, column_ids, unbound_expressions,
	                        storage_info, options);
#endif
}

} // namespace vex_compat
} // namespace duckdb
