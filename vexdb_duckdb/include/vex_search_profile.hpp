#pragma once

#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/client_context_state.hpp"
#include <chrono>
#include <exception>
#include <mutex>

namespace duckdb {

// Query-local counters. Substage times are nested, not additive with search_ns.
// No query text, vectors, predicates, row IDs or unbounded history is retained.
#define VEX_SEARCH_PROFILE_FIELDS(X) \
    X(operators) X(search_calls) X(retry_calls) X(metadata_rows) X(allowed_rows) \
    X(pq_codes_scored) X(graph_candidates) X(raw_vectors_refined) \
    X(snapshot_rejected) X(exact_rows_scored) X(exact_selected_calls) X(exact_fallback_calls) X(output_rows) \
    X(operator_ns) X(prepare_ns) X(prefilter_ns) X(queue_wait_ns) X(mask_ns) \
    X(search_ns) X(retry_search_ns) X(visibility_ns) X(exact_ns) X(fetch_ns) X(output_ns) \
    X(graph_lock_ns) X(pq_prepare_ns) X(pq_navigation_ns) X(pq_distance_ns) X(pq_refine_ns) X(raw_navigation_ns)

struct VexSearchProfileData {
#define VEX_PROFILE_DECLARE(name) uint64_t name = 0;
    VEX_SEARCH_PROFILE_FIELDS(VEX_PROFILE_DECLARE)
#undef VEX_PROFILE_DECLARE
    uint32_t routes = 0;
    bool failed = false;
};

class VexProfileTimer {
public:
    explicit VexProfileTimer(uint64_t *counter) : counter(counter) {
        if (counter) start = std::chrono::steady_clock::now();
    }
    ~VexProfileTimer() { Stop(); }
    void Stop() {
        if (!counter) return;
        *counter += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
        counter = nullptr;
    }
private:
    uint64_t *counter;
    std::chrono::steady_clock::time_point start;
};

class VexSearchProfileState : public ClientContextState {
public:
    static constexpr const char *Key() { return "vexdb_search_profile_v1"; }
    void QueryBegin(ClientContext &) override {
        std::lock_guard<std::mutex> guard(mutex);
        previous = current;
        current = {};
    }
    void QueryEnd(ClientContext &, optional_ptr<ErrorData> error) override {
        std::lock_guard<std::mutex> guard(mutex);
        if (error && current.operators) current.failed = true;
    }
    void Publish(const VexSearchProfileData &data) {
        // One publication per operator, never a global lock per distance.
        std::lock_guard<std::mutex> guard(mutex);
#define VEX_PROFILE_ADD(name) current.name += data.name;
        VEX_SEARCH_PROFILE_FIELDS(VEX_PROFILE_ADD)
#undef VEX_PROFILE_ADD
        current.routes |= data.routes;
        current.failed |= data.failed;
    }
    VexSearchProfileData Previous() {
        std::lock_guard<std::mutex> guard(mutex);
        return previous;
    }
    // Native diagnostic clients can collect after Execute without adding a SQL
    // statement (which would change arrival intervals and previous-query state).
    VexSearchProfileData Completed() {
        std::lock_guard<std::mutex> guard(mutex);
        return current;
    }
private:
    std::mutex mutex;
    VexSearchProfileData current, previous;
};

class VexSearchProfileRun {
public:
    explicit VexSearchProfileRun(ClientContext &context) : exceptions(std::uncaught_exceptions()) {
        Value enabled;
        if (!context.TryGetCurrentSetting("vexdb_search_profile", enabled) || !enabled.GetValue<bool>()) return;
        state = context.registered_state->GetOrCreate<VexSearchProfileState>(VexSearchProfileState::Key());
        data.operators = 1;
        start = std::chrono::steady_clock::now();
    }
    ~VexSearchProfileRun() {
        if (!state) return;
        data.operator_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
        data.failed = std::uncaught_exceptions() > exceptions;
        state->Publish(data);
    }
    VexSearchProfileData *Get() { return state ? &data : nullptr; }
private:
    shared_ptr<VexSearchProfileState> state;
    VexSearchProfileData data;
    std::chrono::steady_clock::time_point start;
    int exceptions;
};

inline LogicalType VexSearchProfileType() {
    child_list_t<LogicalType> fields {{"scope", LogicalType::VARCHAR}, {"status", LogicalType::VARCHAR},
                                    {"route", LogicalType::VARCHAR}};
#define VEX_PROFILE_TYPE(name) fields.emplace_back(#name, LogicalType(LogicalTypeId::UBIGINT));
    VEX_SEARCH_PROFILE_FIELDS(VEX_PROFILE_TYPE)
#undef VEX_PROFILE_TYPE
    return LogicalType::STRUCT(fields);
}

inline void VexLastSearchProfile(DataChunk &, ExpressionState &expression, Vector &result) {
    auto &context = expression.GetContext();
    auto state = context.registered_state->Get<VexSearchProfileState>(VexSearchProfileState::Key());
    auto data = state ? state->Previous() : VexSearchProfileData();
    if (!data.operators) {
        result.SetValue(0, Value(VexSearchProfileType()));
    } else {
        const char *route = data.routes == 1 ? "raw-graph" : data.routes == 2 ? "pq-graph" :
                            data.routes == 4 ? "pq-full-scan" : data.routes == 8 ? "filtered-exact" :
                            data.routes == 16 ? "empty-filter" : "mixed";
        child_list_t<Value> fields {{"scope", Value("previous SQL: sum of VEXDB_INDEX_SCAN operators; excludes planning and native operators")},
                                   {"status", Value(data.failed ? "error" : "ok")}, {"route", Value(route)}};
#define VEX_PROFILE_VALUE(name) fields.emplace_back(#name, Value::UBIGINT(data.name));
        VEX_SEARCH_PROFILE_FIELDS(VEX_PROFILE_VALUE)
#undef VEX_PROFILE_VALUE
        result.SetValue(0, Value::STRUCT(fields));
    }
    result.SetVectorType(VectorType::CONSTANT_VECTOR);
}

} // namespace duckdb
