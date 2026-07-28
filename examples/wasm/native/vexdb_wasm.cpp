#include "sqlite3.h"
#include "vexdb_sqlite.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define VEXDB_WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define VEXDB_WASM_EXPORT
#endif

namespace {

sqlite3 *g_db = nullptr;
std::string g_output;
std::string g_error;

std::string JsonEscape(const char *value) {
    std::string out;
    if (!value) return out;
    for (const unsigned char ch : std::string(value)) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                out += buf;
            } else {
                out += static_cast<char>(ch);
            }
        }
    }
    return out;
}

int Fail(const char *context, int rc = SQLITE_ERROR) {
    g_error = context ? context : "未知错误";
    if (g_db && sqlite3_errmsg(g_db)) {
        g_error += ": ";
        g_error += sqlite3_errmsg(g_db);
    }
    return rc;
}

bool Exec(const std::string &sql, const char *context) {
    char *message = nullptr;
    const int rc = sqlite3_exec(g_db, sql.c_str(), nullptr, nullptr, &message);
    if (rc == SQLITE_OK) return true;
    g_error = context;
    if (message) {
        g_error += ": ";
        g_error += message;
        sqlite3_free(message);
    }
    return false;
}

const char *TableForScope(const char *scope) {
    if (!scope) return nullptr;
    if (std::strcmp(scope, "bundled_text") == 0) return "bundled_text_vectors";
    if (std::strcmp(scope, "user_text") == 0) return "user_text_vectors";
    if (std::strcmp(scope, "bundled_media") == 0) return "bundled_media_vectors";
    if (std::strcmp(scope, "user_media") == 0) return "user_media_vectors";
    return nullptr;
}

bool EnsureDatabase() {
    if (g_db) return true;
    int rc = sqlite3_open(":memory:", &g_db);
    if (rc != SQLITE_OK) {
        Fail("无法打开浏览器内存数据库", rc);
        return false;
    }
    rc = vexdb_sqlite_register(g_db);
    if (rc != SQLITE_OK) {
        Fail("无法注册 VexDB Lite", rc);
        sqlite3_close(g_db);
        g_db = nullptr;
        return false;
    }
    return true;
}

int ResetScope(const char *scope, int dimensions, const char *requested_metric) {
    g_error.clear();
    g_output.clear();
    const char *table = TableForScope(scope);
    if (!table) {
        g_error = "未知的向量索引类型";
        return SQLITE_MISUSE;
    }
    if (dimensions <= 0 || dimensions > 65535) {
        g_error = "向量维度不正确";
        return SQLITE_MISUSE;
    }
    const std::string metric = requested_metric ? requested_metric : "cosine";
    if (metric != "cosine" && metric != "l2") {
        g_error = "距离类型只支持 cosine 或 l2";
        return SQLITE_MISUSE;
    }
    if (!EnsureDatabase()) return SQLITE_ERROR;

    std::ostringstream sql;
    sql << "BEGIN IMMEDIATE; DROP TABLE IF EXISTS " << table << "; "
        << "CREATE VIRTUAL TABLE " << table << " USING GRAPH_INDEX("
        << "embedding FLOAT[" << dimensions << "], "
        << "title TEXT, content TEXT, metric=" << metric << ", m=16, "
        << "ef_construction=100, ef_search=64, brute_force_threshold=0); COMMIT;";
    if (Exec(sql.str(), "无法创建向量索引")) return SQLITE_OK;
    const std::string saved_error = g_error;
    sqlite3_exec(g_db, "ROLLBACK", nullptr, nullptr, nullptr);
    g_error = saved_error;
    return SQLITE_ERROR;
}

std::string QueryPlan(const char *table, const char *query_json, int k) {
    std::ostringstream sql;
    sql << "EXPLAIN QUERY PLAN SELECT rowid, title, content, distance FROM " << table
        << " WHERE embedding MATCH ?1 AND k = ?2 ORDER BY distance";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql.str().c_str(), -1, &stmt, nullptr) != SQLITE_OK) return "";
    sqlite3_bind_text(stmt, 1, query_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, k);
    std::string plan;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!plan.empty()) plan += " | ";
        const char *detail = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
        if (detail) plan += detail;
    }
    sqlite3_finalize(stmt);
    return plan;
}

}  // namespace

extern "C" {

VEXDB_WASM_EXPORT int vexdb_wasm_reset_scope(const char *scope, int dimensions,
                                              const char *metric) {
    return ResetScope(scope, dimensions, metric);
}

VEXDB_WASM_EXPORT int vexdb_wasm_insert_scope(const char *scope, int rowid,
                                               const char *embedding_json,
                                               const char *title, const char *content) {
    g_error.clear();
    const char *table = TableForScope(scope);
    if (!table) return Fail("未知的向量索引类型", SQLITE_MISUSE);
    if (!g_db) return Fail("数据库尚未初始化", SQLITE_MISUSE);
    if (!embedding_json) return Fail("向量不能为空", SQLITE_MISUSE);
    std::ostringstream sql;
    sql << "INSERT INTO " << table
        << "(rowid, embedding, title, content) VALUES(?1, ?2, ?3, ?4)";
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_db, sql.str().c_str(), -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(rowid));
        sqlite3_bind_text(stmt, 2, embedding_json, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, title ? title : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, content ? content : "", -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return Fail("写入向量失败", rc);
    return SQLITE_OK;
}

VEXDB_WASM_EXPORT const char *vexdb_wasm_search_scope(const char *scope,
                                                       const char *query_json, int k) {
    g_error.clear();
    g_output.clear();
    const char *table = TableForScope(scope);
    if (!table) {
        Fail("未知的向量索引类型", SQLITE_MISUSE);
        return nullptr;
    }
    if (!g_db || !query_json || k <= 0) {
        Fail("查询参数不正确", SQLITE_MISUSE);
        return nullptr;
    }
    const std::string plan = QueryPlan(table, query_json, k);
    std::ostringstream sql;
    sql << "SELECT rowid, title, content, distance FROM " << table
        << " WHERE embedding MATCH ?1 AND k = ?2 ORDER BY distance";
    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(g_db, sql.str().c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        Fail("查询准备失败", rc);
        return nullptr;
    }
    sqlite3_bind_text(stmt, 1, query_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, k);

    const auto started = std::chrono::steady_clock::now();
    std::ostringstream rows;
    bool first = true;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (!first) rows << ',';
        first = false;
        const char *title = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        const char *content = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        rows << "{\"rowid\":" << sqlite3_column_int64(stmt, 0)
             << ",\"title\":\"" << JsonEscape(title) << "\""
             << ",\"content\":\"" << JsonEscape(content) << "\""
             << ",\"distance\":" << sqlite3_column_double(stmt, 3) << '}';
    }
    const auto finished = std::chrono::steady_clock::now();
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        Fail("向量查询失败", rc);
        return nullptr;
    }
    const double elapsed =
        std::chrono::duration<double, std::milli>(finished - started).count();
    std::ostringstream output;
    output << "{\"ok\":true,\"elapsedMs\":" << elapsed
           << ",\"plan\":\"" << JsonEscape(plan.c_str()) << "\",\"rows\":["
           << rows.str() << "]}";
    g_output = output.str();
    return g_output.c_str();
}

VEXDB_WASM_EXPORT int vexdb_wasm_count_scope(const char *scope) {
    const char *table = TableForScope(scope);
    if (!g_db || !table) return 0;
    std::ostringstream sql;
    sql << "SELECT count(*) FROM " << table;
    sqlite3_stmt *stmt = nullptr;
    int count = 0;
    if (sqlite3_prepare_v2(g_db, sql.str().c_str(), -1, &stmt, nullptr)
        == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

// 兼容旧的单表调用方；新页面统一使用带 scope 的接口。
VEXDB_WASM_EXPORT int vexdb_wasm_reset(int dimensions) {
    return ResetScope("user_text", dimensions, "cosine");
}

VEXDB_WASM_EXPORT int vexdb_wasm_reset_with_metric(int dimensions, const char *metric) {
    return ResetScope("user_text", dimensions, metric);
}

VEXDB_WASM_EXPORT int vexdb_wasm_insert(int rowid, const char *embedding_json,
                                         const char *title, const char *content) {
    return vexdb_wasm_insert_scope("user_text", rowid, embedding_json, title, content);
}

VEXDB_WASM_EXPORT const char *vexdb_wasm_search(const char *query_json, int k) {
    return vexdb_wasm_search_scope("user_text", query_json, k);
}

VEXDB_WASM_EXPORT int vexdb_wasm_count() {
    return vexdb_wasm_count_scope("user_text");
}

VEXDB_WASM_EXPORT const char *vexdb_wasm_version() {
    g_output.clear();
    if (!g_db) return "VexDB Lite WebAssembly";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, "SELECT vexdb_version()", -1, &stmt, nullptr) == SQLITE_OK
        && sqlite3_step(stmt) == SQLITE_ROW) {
        const char *value = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
        if (value) g_output = value;
    }
    sqlite3_finalize(stmt);
    return g_output.c_str();
}

VEXDB_WASM_EXPORT const char *vexdb_wasm_last_error() {
    return g_error.c_str();
}

}  // extern "C"
