#include "vex_functions.hpp"
#include "vex_graph_index.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

namespace duckdb {

// ============================================================
// Bind Data for ANN Search
// ============================================================
struct ANNSearchBindData : public TableFunctionData {
	string table_name;
	string index_name;
	vector<float> query_vector;
	idx_t k;
	int ef;

	ANNSearchBindData() : k(10), ef(0) {}

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<ANNSearchBindData>();
		copy->table_name = table_name;
		copy->index_name = index_name;
		copy->query_vector = query_vector;
		copy->k = k;
		copy->ef = ef;
		return std::move(copy);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ANNSearchBindData>();
		return table_name == other.table_name && index_name == other.index_name && k == other.k && ef == other.ef;
	}
};

// ============================================================
// Global State for ANN Search
// ============================================================
struct ANNSearchGlobalState : public GlobalTableFunctionState {
	std::vector<row_t> row_ids;
	std::vector<float> distances;
	idx_t current_offset;

	ANNSearchGlobalState() : current_offset(0) {}

	idx_t MaxThreads() const override {
		return 1;
	}
};

// ============================================================
// Helper: Extract vector from Value
// ============================================================
static void ExtractVectorFromValue(const Value &val, vector<float> &out_vec) {
	auto &val_type = val.type();
	auto val_id = val_type.id();

	if (val_id == LogicalTypeId::ARRAY) {
		auto &children = ArrayValue::GetChildren(val);
		out_vec.resize(children.size());
		for (size_t i = 0; i < children.size(); i++) {
			out_vec[i] = children[i].GetValue<float>();
		}
	} else if (val_id == LogicalTypeId::LIST) {
		auto &children = ListValue::GetChildren(val);
		out_vec.resize(children.size());
		for (size_t i = 0; i < children.size(); i++) {
			out_vec[i] = children[i].GetValue<float>();
		}
	} else {
		throw InvalidInputException("query_vector must be a FLOAT[N] (ARRAY or LIST)");
	}
}

// ============================================================
// Bind Function: Parse parameters and validate
// ============================================================
static unique_ptr<FunctionData> ANNSearchBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<ANNSearchBindData>();

	// Parameters: table_name, index_name, query_vector, k=10, ef=0 (default)
	if (input.inputs.size() < 3) {
		throw InvalidInputException("ann_search requires at least 3 parameters: table_name, index_name, query_vector");
	}

	// Get table name
	if (input.inputs[0].type().id() != LogicalTypeId::VARCHAR) {
		throw InvalidInputException("table_name must be a string");
	}
	bind_data->table_name = StringValue::Get(input.inputs[0]);

	// Get index name
	if (input.inputs[1].type().id() != LogicalTypeId::VARCHAR) {
		throw InvalidInputException("index_name must be a string");
	}
	bind_data->index_name = StringValue::Get(input.inputs[1]);

	// Get query vector
	ExtractVectorFromValue(input.inputs[2], bind_data->query_vector);

	// Get k (optional)
	if (input.inputs.size() > 3) {
		bind_data->k = NumericCast<idx_t>(BigIntValue::Get(input.inputs[3]));
	} else {
		bind_data->k = 10;
	}

	// Get ef (optional)
	if (input.inputs.size() > 4) {
		bind_data->ef = NumericCast<int32_t>(IntegerValue::Get(input.inputs[4]));
	} else {
		bind_data->ef = 0;  // Use default
	}

	// Return types: row_id, distance
	return_types.push_back(LogicalType::BIGINT);
	return_types.push_back(LogicalType::DOUBLE);
	names.push_back("row_id");
	names.push_back("distance");

	return std::move(bind_data);
}

// ============================================================
// Init Global State: Perform the ANN search
// ============================================================
static unique_ptr<GlobalTableFunctionState> ANNSearchInitGlobal(ClientContext &context,
                                                                 TableFunctionInitInput &input) {
	if (!input.bind_data) {
		throw InternalException("ann_search: bind_data is null");
	}
	auto bind_data = const_cast<ANNSearchBindData *>(static_cast<const ANNSearchBindData *>(input.bind_data.get()));
	auto state = make_uniq<ANNSearchGlobalState>();

	// Find the table and index
	auto &catalog = Catalog::GetSystemCatalog(context);

	// Look up the table
	auto &table_entry = catalog.GetEntry(context, CatalogType::TABLE_ENTRY, DEFAULT_SCHEMA, bind_data->table_name);
	auto &table_entry_ref = table_entry.Cast<TableCatalogEntry>();

	// Get the DataTableInfo - this requires accessing the table's storage
	// For simplicity, let's try a different approach using the catalog's table scan mechanism
	// We need to get the DuckTableEntry specifically to access indexes

	// Try to get the table as a DuckTableEntry
	if (table_entry_ref.type != CatalogType::TABLE_ENTRY) {
		throw InvalidInputException("'%s' is not a table", bind_data->table_name);
	}

	// The catalog entry should be a TableCatalogEntry, but we need DuckTableEntry for indexes
	// Let's use a different approach - iterate through the catalog to find indexes

	// For now, let's use the table catalog entry's methods
	// Note: This might need adjustment based on the actual DuckDB API version

	// We'll need to access the storage info which contains the indexes
	// Since GetEntry returns a CatalogEntry&, we can work with that
	throw NotImplementedException("ann_search table function: index lookup not yet fully implemented for this DuckDB version");
}

// ============================================================
// Init Local State (no-op for single-threaded)
// ============================================================
static unique_ptr<LocalTableFunctionState> ANNSearchInitLocal(ExecutionContext &context,
                                                               TableFunctionInitInput &input,
                                                               GlobalTableFunctionState *gstate) {
	return make_uniq<LocalTableFunctionState>();
}

// ============================================================
// Main Function: Return results
// ============================================================
static void ANNSearchFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	// Results are already computed in InitGlobal, just return them
}

// ============================================================
// Progress Function
// ============================================================
static double ANNSearchProgress(ClientContext &context, const FunctionData *bind_data_p,
                                 const GlobalTableFunctionState *gstate_p) {
	return 100.0;
}

// ============================================================
// Register ANN Search Table Function
// ============================================================
void VexFunctions::RegisterANNSearchFunction(ExtensionLoader &loader) {
	// For now, register a stub function that explains the limitation
	// Parameters: table_name, index_name, query_vector, k=10, ef=0
	TableFunction ann_search_func("ann_search", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY,
	                                            LogicalType::BIGINT, LogicalType::INTEGER},
	                              ANNSearchFunction);
	ann_search_func.bind = ANNSearchBind;
	ann_search_func.init_global = ANNSearchInitGlobal;
	ann_search_func.init_local = ANNSearchInitLocal;
	ann_search_func.table_scan_progress = ANNSearchProgress;

	// Make k and ef optional - only first 3 are required
	ann_search_func.arguments.resize(3);

	loader.RegisterFunction(ann_search_func);
}

} // namespace duckdb
