#include "vexdb_lite_extension.hpp"

#include "vex_functions.hpp"
#include "vex_graph_index.hpp"
#include "vex_optimizer.hpp"
#include "vex_search_admission.hpp"
#include "vex_search_profile.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/scalar_function.hpp"



#ifndef VEXDB_DUCK_GIT_HASH
#define VEXDB_DUCK_GIT_HASH "unknown"
#endif

#ifndef VEXDB_DUCK_BUILD_TIME
#define VEXDB_DUCK_BUILD_TIME "unknown"
#endif

namespace duckdb {

static void VexVersionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    result.SetValue(0, StringVector::AddString(
                           result, "vexdb_lite duck extension " VEXDB_DUCK_GIT_HASH " (" VEXDB_DUCK_BUILD_TIME ")"));
    result.SetVectorType(VectorType::CONSTANT_VECTOR);
}

// Observability for the GLOBAL graph mirror pool: total bytes currently mirrored in
// RAM across all GRAPH_INDEX instances (the shared vexdb_graph_memory_limit budget).
// Allocated via DuckDB's database allocator but not tied to the buffer pool, so this
// is NOT counted in DuckDB's memory_limit.
static void VexGraphMirrorUsedFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    result.SetValue(0, Value::UBIGINT(GlobalMirrorUsedBytes().load(std::memory_order_relaxed)));
    result.SetVectorType(VectorType::CONSTANT_VECTOR);
}

static void SetSearchConcurrency(ClientContext &, SetScope scope, Value &value) {
    // DuckDB 1.5.2 RESET of an extension option defaults to SESSION even when
    // SET defaults to GLOBAL. Require explicit scope for both operations.
    if (scope != SetScope::GLOBAL) {
        throw InvalidInputException("vexdb_search_concurrency is database-wide; use SET GLOBAL or RESET GLOBAL");
    }
    if (value.IsNull() || value.GetValue<int64_t>() < -1) {
        throw InvalidInputException("vexdb_search_concurrency must be -1 (CPU count), 0 (unlimited), or positive");
    }
}

static void SetSearchQueueTimeout(ClientContext &, SetScope, Value &value) {
    if (value.IsNull()) throw InvalidInputException("vexdb_search_queue_timeout_ms cannot be NULL");
}

static LogicalType SearchQueueStateType() {
    // DuckDB's C++11 core defines its static type IDs out of line. Passing one
    // through pair's forwarding constructor can also emit a C++17 definition.
    const LogicalType counter_type(LogicalTypeId::UBIGINT);
    return LogicalType::STRUCT({{"limit", counter_type}, {"active", counter_type},
        {"waiting", counter_type}, {"admitted", counter_type},
        {"cancelled", counter_type}, {"timed_out", counter_type},
        {"peak_active", counter_type}});
}

static void SearchQueueState(DataChunk &, ExpressionState &state, Vector &result) {
    auto &db = *state.GetContext().db;
    auto admission = VexSearchAdmission::Get(db);
    const auto snapshot = admission->queue.GetSnapshot();
    result.SetValue(0, Value::STRUCT({{"limit", Value::UBIGINT(admission->GetLimit(db))},
        {"active", Value::UBIGINT(snapshot.active)}, {"waiting", Value::UBIGINT(snapshot.waiting)},
        {"admitted", Value::UBIGINT(snapshot.admitted)}, {"cancelled", Value::UBIGINT(snapshot.cancelled)},
        {"timed_out", Value::UBIGINT(snapshot.timed_out)}, {"peak_active", Value::UBIGINT(snapshot.peak_active)}}));
    result.SetVectorType(VectorType::CONSTANT_VECTOR);
}

static void RegisterIndexTypes(DBConfig &config) {
    IndexType graph_index_type;
    graph_index_type.name = GraphIndex::TYPE_NAME;
    graph_index_type.create_instance = GraphIndex::Create;
    graph_index_type.create_plan = GraphIndex::CreatePlan;
    config.GetIndexTypes().RegisterIndexType(std::move(graph_index_type));
}

// Version locking note: C++ extensions have no stable ABI, so a binary built for
// one DuckDB version must not be loaded into another. DuckDB enforces this natively
// from the extension's footer (the VERSION_FIELD stamped by build_duck.sh strip /
// fix_duckdb_footer.py): the loader rejects a version mismatch with a clear error
// before dlopen — even under allow_unsigned_extensions. No in-init guard is needed.
void LoadInternal(ExtensionLoader &loader) {
    VexFunctions::Register(loader);
    loader.RegisterFunction(ScalarFunction("vexdb_version", {}, LogicalType::VARCHAR, VexVersionFunction));
    loader.RegisterFunction(
        ScalarFunction("vexdb_graph_mirror_used", {}, LogicalType::UBIGINT, VexGraphMirrorUsedFunction));
    ScalarFunction queue_state("vexdb_search_queue_state", {}, SearchQueueStateType(), SearchQueueState);
    queue_state.stability = FunctionStability::VOLATILE;
    loader.RegisterFunction(queue_state);
    ScalarFunction search_profile("vexdb_last_search_profile", {}, VexSearchProfileType(), VexLastSearchProfile);
    search_profile.stability = FunctionStability::VOLATILE;
    loader.RegisterFunction(search_profile);

    auto &db = loader.GetDatabaseInstance();
    auto &config = DBConfig::GetConfig(db);

    RegisterIndexTypes(config);
    OptimizerExtension::Register(config, VexOptimizerExtension());

    config.AddExtensionOption("vexdb_ef_search", "Search expansion factor for GRAPH_INDEX.",
                              LogicalType::INTEGER, Value::INTEGER(40));
    config.AddExtensionOption("vexdb_search_profile",
                              "Collect connection-local stage times for VEXDB_INDEX_SCAN; inspect the previous SQL "
                              "with vexdb_last_search_profile(). Nested times are not additive. Disabled by default.",
                              LogicalType::BOOLEAN, Value::BOOLEAN(false));
    config.AddExtensionOption("vexdb_graph_bidirectional_search",
                              "Also follow reverse base-layer edges using a query-independent topology cache.",
                              LogicalType::BOOLEAN, Value::BOOLEAN(false));
    config.AddExtensionOption("vexdb_native_topn_fetch",
                              "Fetch selected native vector TopN rows directly instead of rescanning output columns.",
                              LogicalType::BOOLEAN, Value::BOOLEAN(false));
    config.AddExtensionOption("vexdb_filtered_search_mode",
                              "Scalar-filter/vector integration: 'auto' builds snapshot-visible allowed rows, "
                              "then chooses exact or filtered graph/PQ search; 'exact' retains natural filtered SQL.",
                              LogicalType::VARCHAR, Value("exact"));
    config.AddExtensionOption("vexdb_search_concurrency",
                              "Database-wide FIFO limit for VEXDB_INDEX_SCAN searches. "
                              "0 = unlimited (default), -1 = hardware CPU count, positive = explicit limit. "
                              "Requires SET GLOBAL / RESET GLOBAL. Existing searches drain on a decrease.",
                              LogicalType::BIGINT, Value::BIGINT(0), SetSearchConcurrency, SetScope::GLOBAL);
    config.AddExtensionOption("vexdb_search_queue_timeout_ms",
                              "Maximum wait for a VEXDB_INDEX_SCAN search slot in milliseconds. "
                              "0 = no wait deadline. Does not limit execution time. Interrupt() cancels waiting.",
                              LogicalType::UBIGINT, Value::UBIGINT(0), SetSearchQueueTimeout);
    config.AddExtensionOption("vexdb_brute_force_threshold",
                              "Run exact brute-force search when index row count <= threshold. "
                              "Below ~10k rows ANN offers no perf win but loses recall, so default "
                              "favors precision. Set higher to bypass the graph index after large delete+reinsert "
                              "churn where recall has degraded.",
                              LogicalType::UBIGINT, Value::UBIGINT(10000));
    // off (default): search ignores PQ, runs HNSW on raw vectors.
    // pq_only:       graph search over PQ codes using a precomputed distance
    //                table. Approximate, scalable, and useful as a way to
    //                verify PQ training is healthy.
    config.AddExtensionOption("vexdb_pq_search_mode",
                              "PQ search mode for GRAPH_INDEX: 'off', 'pq_only' (full scan), "
                              "or 'pq_graph' (graph navigation, non-compact L2/cosine).",
                              LogicalType::VARCHAR, Value("off"));
    // pq_only-mode refinement: take top k*factor by PQ distance, then re-rank
    // via raw vector L2/cosine. Trades a per-query raw-vec lookup for higher
    // recall. 1.0 disables. Compact-mode indexes ignore (no raw vec to fetch).
    config.AddExtensionOption("vexdb_pq_refine_k_factor",
                              "PQ refine factor: SearchPQ takes top k*factor candidates "
                              "and re-ranks by raw distance. 1.0 = no refine. Range [1.0, 1000.0].",
                              LogicalType::DOUBLE, Value::DOUBLE(1.0));
    // GLOBAL byte budget for the in-memory raw-vector mirror of GRAPH_INDEX, shared by
    // ALL indexes (total mirror RAM across the whole database, not per-index). Nodes
    // within the budget keep a lock-free RAM copy (vectors[]); beyond it, vectors are
    // served from the buffer manager (pageable to disk, bounded by DuckDB's memory_limit).
    // The mirror is DuckDB-allocator memory outside the buffer pool (NOT counted in
    // memory_limit) and otherwise a 2x-redundant copy of the buffer-manager tier —
    // this caps that extra RAM, trading per-query lock-free speed for bounded
    // mirror footprint. Inspect live usage via
    // vexdb_graph_mirror_used(). 0 = unlimited (full mirror; backward-compatible default).
    config.AddExtensionOption("vexdb_graph_memory_limit",
                              "Global byte budget for GRAPH_INDEX's in-memory raw-vector mirror, "
                              "shared across all indexes. 0 = unlimited (full mirror). Beyond the "
                              "budget, vectors are served from the buffer manager (pageable, bounded "
                              "by memory_limit). Inspect usage with vexdb_graph_mirror_used().",
                              LogicalType::UBIGINT, Value::UBIGINT(0));

}

void VexdbLiteExtension::Load(ExtensionLoader &loader) {
    LoadInternal(loader);
}

std::string VexdbLiteExtension::Name() {
    return "vexdb_lite";
}

std::string VexdbLiteExtension::Version() const {
    return "0.1.0";
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(vexdb_lite, loader) {
    duckdb::LoadInternal(loader);
}
}
