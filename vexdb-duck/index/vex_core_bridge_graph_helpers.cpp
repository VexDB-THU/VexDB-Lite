#include "vex_core_bridge_graph_helpers.hpp"

#include "vex_graph_index.hpp"
#include "duckdb/common/exception.hpp"
#include "vex/vex_adapter_graph_runtime.hpp"
#include "vex/vex_adapter_quant_runtime.hpp"

#include <algorithm>
#include <cstring>

namespace duckdb {

namespace {

DuckDBStorageGraphState CaptureGraphStateFromCore(const GraphIndexCore &graph) {
	DuckDBStorageGraphState s{};
	s.has_entry_point = graph.has_entry_point;
	s.entry_point_raw = graph.entry_point.Get();
	s.max_level = graph.max_level;
	s.node_count = graph.node_count;
	return s;
}

void ApplyGraphStateToCore(GraphIndexCore &graph, const DuckDBStorageGraphState &s) {
	graph.node_count = static_cast<idx_t>(s.node_count);
	graph.max_level = s.max_level;
	if (s.has_entry_point) {
		graph.entry_point.Set(s.entry_point_raw);
		graph.has_entry_point = true;
	} else {
		graph.entry_point.Clear();
		graph.has_entry_point = false;
	}
}

::vex::TypeId ToCoreTypeId(LogicalTypeId type_id) {
	switch (type_id) {
	case LogicalTypeId::BOOLEAN:
		return ::vex::TypeId::BOOLEAN;
	case LogicalTypeId::TINYINT:
		return ::vex::TypeId::INT8;
	case LogicalTypeId::UTINYINT:
		return ::vex::TypeId::UINT8;
	case LogicalTypeId::SMALLINT:
		return ::vex::TypeId::INT16;
	case LogicalTypeId::USMALLINT:
		return ::vex::TypeId::UINT16;
	case LogicalTypeId::INTEGER:
		return ::vex::TypeId::INT32;
	case LogicalTypeId::UINTEGER:
		return ::vex::TypeId::UINT32;
	case LogicalTypeId::BIGINT:
		return ::vex::TypeId::INT64;
	case LogicalTypeId::UBIGINT:
		return ::vex::TypeId::UINT64;
	case LogicalTypeId::FLOAT:
		return ::vex::TypeId::FLOAT32;
	case LogicalTypeId::DOUBLE:
		return ::vex::TypeId::FLOAT64;
	default:
		return ::vex::TypeId::INVALID;
	}
}

} // namespace

void ExportBridgeGraphState(IndexStorageInfo &storage, const GraphIndexCore &graph) {
	StoreDuckDBRawGraphStateToStorage(storage, CaptureGraphStateFromCore(graph));
}

bool ImportBridgeGraphState(const IndexStorageInfo &storage, GraphIndexCore &graph) {
	DuckDBStorageGraphState s{};
	if (!LoadDuckDBRawGraphStateFromStorage(storage, s)) {
		return false;
	}
	ApplyGraphStateToCore(graph, s);
	return true;
}

::vex::Metric ToCoreMetric(vex::VexMetric metric) {
	switch (metric) {
	case vex::VexMetric::L2:
		return ::vex::Metric::L2;
	case vex::VexMetric::COSINE:
		return ::vex::Metric::COSINE;
	case vex::VexMetric::INNER_PRODUCT:
		return ::vex::Metric::INNER_PRODUCT;
	default:
		return ::vex::Metric::L2;
	}
}

std::unique_ptr<::vex::FilterPredicate> CloneFilterToCore(const vex::FilterPredicate &filter) {
	if (auto *eq = dynamic_cast<const vex::EqualityFilter *>(&filter)) {
		return std::unique_ptr<::vex::FilterPredicate>(
		    new ::vex::EqualityFilter(eq->offset, eq->size, eq->value.data(), eq->selectivity_));
	}
	if (auto *range = dynamic_cast<const vex::RangeFilter *>(&filter)) {
		auto result = std::unique_ptr<::vex::RangeFilter>(
		    new ::vex::RangeFilter(range->offset, range->size, ToCoreTypeId(range->type_id), range->selectivity_));
		if (range->has_min) {
			result->SetMin(range->min_val.data());
		}
		if (range->has_max) {
			result->SetMax(range->max_val.data());
		}
		return result;
	}
	if (auto *in_list = dynamic_cast<const vex::InListFilter *>(&filter)) {
		auto result = std::unique_ptr<::vex::InListFilter>(
		    new ::vex::InListFilter(in_list->offset, in_list->size, in_list->selectivity_));
		for (const auto &value : in_list->values) {
			result->AddValue(value.data());
		}
		return result;
	}
	if (auto *conjunction = dynamic_cast<const vex::ConjunctionFilter *>(&filter)) {
		auto result = std::unique_ptr<::vex::ConjunctionFilter>(new ::vex::ConjunctionFilter());
		for (const auto &child : conjunction->children) {
			auto core_child = CloneFilterToCore(*child);
			if (!core_child) {
				return nullptr;
			}
			result->AddChild(std::move(core_child));
		}
		return result;
	}
	return nullptr;
}

std::unique_ptr<GraphIndex::CoreBridgeContext> GraphIndex::CreateCoreBridgeContextOrThrow() {
	if (!graph_.node_alloc || !graph_.vector_alloc || !graph_.upper_alloc) {
		throw InternalException("DuckDB core bridge: allocators are not initialized");
	}
	if (graph_.meta_segment_size > 0 && !graph_.meta_alloc) {
		throw InternalException("DuckDB core bridge: metadata allocator is missing");
	}

	auto ctx = make_uniq<CoreBridgeContext>(name);
	ExportBridgeGraphState(ctx->storage, graph_);

	ctx->cfg.index_name = GetIndexName();
	ctx->cfg.dimension = dimension_;
	ctx->cfg.m = m_;
	ctx->cfg.metadata_size = graph_.meta_segment_size;
	ctx->cfg.storage_info = &ctx->storage;
	BindDuckDBLiveStorage(ctx->cfg, *graph_.node_alloc, *graph_.vector_alloc, *graph_.upper_alloc,
	                      graph_.meta_alloc.get(), graph_.row_id_map);

	ctx->store = CreateDuckDBCoreNodeStoreSkeleton(ctx->cfg);
	if (!ctx->store || !ctx->store->Config().low_level_binding) {
		throw InternalException("DuckDB core bridge: failed to create direct NodeStore binding");
	}
	ctx->binding = ctx->store->Config().low_level_binding.get();
	return ctx;
}

void GraphIndex::LoadCoreBridgeGraphOrThrow(CoreBridgeContext &ctx) const {
	if (!ctx.binding || !ctx.store) {
		throw InternalException("DuckDB core bridge: context is missing binding or store");
	}
	if (!::vex::CreateGraphRuntime(ctx.store->Config().low_level_binding, *ctx.store, ToCoreMetric(metric_), m_,
	                               ef_construction_, ctx.core_graph, &ctx.graph_state)) {
		throw InternalException("DuckDB core bridge: failed to load graph runtime");
	}
}

bool GraphIndex::SyncCoreBridgeGraphState(CoreBridgeContext &ctx) {
	if (!ctx.binding || !ctx.core_graph) {
		return false;
	}
	if (!::vex::PersistGraphRuntime(*ctx.binding, *ctx.core_graph, &ctx.graph_state)) {
		return false;
	}
	return ImportBridgeGraphState(ctx.storage, graph_);
}

bool GraphIndex::WriteCoreBridgeMetadata(CoreBridgeContext &ctx, uint32_t node_id,
                                         const uint8_t *metadata, const char *operation) {
	if (!ctx.binding || metadata == nullptr || graph_.meta_segment_size == 0) {
		return false;
	}

	::vex::DuckDBNodeLayoutView view{};
	if (!ctx.binding->PinNode(node_id, true, view)) {
		throw InternalException("DuckDB core bridge %s: failed to pin node for metadata write", operation);
	}
	if (!view.metadata) {
		ctx.binding->UnpinNode(view);
		throw InternalException("DuckDB core bridge %s: metadata segment is not available", operation);
	}
	std::memcpy(view.metadata, metadata, graph_.meta_segment_size);
	ctx.binding->UnpinNode(view);
	return true;
}

} // namespace duckdb
