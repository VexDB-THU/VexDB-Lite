#pragma once

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/common/column_index.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

namespace duckdb {

//! Build output types by mapping column_ids through returned_types.
static inline vector<LogicalType> BuildOutputTypes(const vector<ColumnIndex> &column_ids,
                                                   const vector<LogicalType> &returned_types) {
	vector<LogicalType> output_types;
	output_types.reserve(column_ids.size());
	for (idx_t i = 0; i < column_ids.size(); i++) {
		if (column_ids[i].IsVirtualColumn()) {
			output_types.push_back(LogicalType::ROW_TYPE);
		} else {
			idx_t physical_idx = column_ids[i].GetPrimaryIndex();
			if (physical_idx < returned_types.size()) {
				output_types.push_back(returned_types[physical_idx]);
			} else {
				throw InternalException("VEX: column physical index %llu out of range (returned_types size %llu)",
				                        physical_idx, returned_types.size());
			}
		}
	}
	return output_types;
}

//! Fetch rows by row IDs from a table, filtering by transaction visibility.
//! Returns a ColumnDataCollection with the fetched rows (offset/limit applied if non-zero).
static inline unique_ptr<ColumnDataCollection> FetchRowsByRowIds(
    ClientContext &context, DuckTableEntry &duck_table,
    const vector<ColumnIndex> &column_ids, const vector<LogicalType> &output_types,
    const vector<row_t> &result_row_ids, idx_t limit = 0, idx_t offset = 0) {

	auto &storage = duck_table.GetStorage();
	auto &db = duck_table.ParentCatalog().GetAttached();
	auto &transaction = DuckTransaction::Get(context, db);

	vector<row_t> visible_row_ids;
	visible_row_ids.reserve(result_row_ids.size());
	for (auto &rid : result_row_ids) {
		if (storage.CanFetch(transaction, rid)) {
			visible_row_ids.push_back(rid);
		}
	}

	auto collection = make_uniq<ColumnDataCollection>(context, output_types);
	if (visible_row_ids.empty()) {
		return collection;
	}

	// Apply offset/limit if specified
	idx_t total = visible_row_ids.size();
	idx_t start = (limit > 0) ? offset : 0;
	idx_t count = (limit > 0) ? ((start >= total) ? 0 : MinValue<idx_t>(limit, total - start)) : total;
	if (count == 0) {
		return collection;
	}

	// Separate physical columns from virtual (row_id) columns
	vector<StorageIndex> fetch_col_ids;
	vector<idx_t> fetch_output_positions;
	vector<idx_t> rowid_positions;

	for (idx_t i = 0; i < column_ids.size(); i++) {
		if (column_ids[i].IsVirtualColumn()) {
			rowid_positions.push_back(i);
		} else {
			fetch_col_ids.emplace_back(column_ids[i].GetPrimaryIndex());
			fetch_output_positions.push_back(i);
		}
	}

	vector<LogicalType> fetch_types;
	for (idx_t pos : fetch_output_positions) {
		fetch_types.push_back(output_types[pos]);
	}

	ColumnFetchState fetch_state;
	DataChunk fetch_chunk;
	fetch_chunk.Initialize(context, fetch_types);
	DataChunk output_chunk;
	output_chunk.Initialize(context, output_types);

	for (idx_t off = start; off < start + count;) {
		idx_t batch = MinValue<idx_t>(STANDARD_VECTOR_SIZE, start + count - off);
		fetch_chunk.Reset();
		output_chunk.Reset();

		Vector row_id_vec(LogicalType::ROW_TYPE, batch);
		auto row_id_data = FlatVector::GetData<row_t>(row_id_vec);
		for (idx_t i = 0; i < batch; i++) {
			row_id_data[i] = visible_row_ids[off + i];
		}

		storage.Fetch(transaction, fetch_chunk, fetch_col_ids, row_id_vec, batch, fetch_state);

		for (idx_t f = 0; f < fetch_output_positions.size(); f++) {
			output_chunk.data[fetch_output_positions[f]].Reference(fetch_chunk.data[f]);
		}
		for (idx_t rid_pos : rowid_positions) {
			auto rowid_data_ptr = FlatVector::GetData<row_t>(output_chunk.data[rid_pos]);
			for (idx_t i = 0; i < batch; i++) {
				rowid_data_ptr[i] = visible_row_ids[off + i];
			}
		}

		output_chunk.SetCardinality(batch);
		collection->Append(output_chunk);
		off += batch;
	}

	return collection;
}

} // namespace duckdb
