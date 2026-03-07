#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

struct VexTypes {
	static constexpr const char *FLOATVECTOR_TYPE_NAME = "FLOATVECTOR";
	static constexpr idx_t FLOATVECTOR_MAX_DIM = 16384;

	static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
