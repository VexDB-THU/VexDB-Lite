#pragma once

#include "duckdb/storage/index.hpp"
#include "vex_filter_predicate.hpp"
#include "vex_graph_index_core.hpp"
#include "vex_core_node_store_bridge.hpp"
#include "vex/vex_graph_algo.hpp"

#include <memory>

namespace duckdb {

void ExportBridgeGraphState(IndexStorageInfo &storage, const GraphIndexCore &graph);
bool ImportBridgeGraphState(const IndexStorageInfo &storage, GraphIndexCore &graph);
::vex::Metric ToCoreMetric(vex::VexMetric metric);
std::unique_ptr<::vex::FilterPredicate> CloneFilterToCore(const vex::FilterPredicate &filter);

} // namespace duckdb
