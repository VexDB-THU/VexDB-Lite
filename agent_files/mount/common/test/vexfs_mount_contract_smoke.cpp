#include "vexfs_runtime_admin.h"
#include "sqlite3.h"
#define VEXDB_SQLITE_CORE
#include "vexdb_sqlite.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int Fail(const char *operation, const vexfs_mount_error &error) {
    std::fprintf(stderr, "VEXFS RUNTIME FAIL: %s: status=%d backend=%s native=%d %s\n",
                 operation, error.status, error.backend, error.native_code, error.message);
    return 1;
}

bool Equals(const vexfs_mount_bytes &bytes, const char *expected) {
    return bytes.size == std::strlen(expected) &&
           std::memcmp(bytes.data, expected, static_cast<size_t>(bytes.size)) == 0;
}

bool Contains(const vexfs_mount_bytes &bytes, const char *expected) {
    if (bytes.data == nullptr) return false;
    const std::string value(static_cast<const char *>(bytes.data), static_cast<size_t>(bytes.size));
    return value.find(expected) != std::string::npos;
}

int64_t JsonInteger(const vexfs_mount_bytes &bytes, const char *key) {
    const std::string value(static_cast<const char *>(bytes.data),
                            static_cast<size_t>(bytes.size));
    const std::string marker = std::string("\"") + key + "\":";
    const size_t start = value.find(marker);
    if (start == std::string::npos) return -1;
    return std::strtoll(value.c_str() + start + marker.size(), nullptr, 10);
}

std::string JsonString(const vexfs_mount_bytes &bytes, const char *key) {
    const std::string value(static_cast<const char *>(bytes.data),
                            static_cast<size_t>(bytes.size));
    const std::string marker = std::string("\"") + key + "\":\"";
    const size_t start = value.find(marker);
    if (start == std::string::npos) return {};
    const size_t value_start = start + marker.size();
    const size_t end = value.find('"', value_start);
    return end == std::string::npos ? std::string() :
        value.substr(value_start, end - value_start);
}

int Count(const vexfs_mount_bytes &bytes, const char *needle) {
    const std::string value(static_cast<const char *>(bytes.data),
                            static_cast<size_t>(bytes.size));
    int count = 0;
    size_t position = 0;
    while ((position = value.find(needle, position)) != std::string::npos) {
        ++count;
        position += std::strlen(needle);
    }
    return count;
}

int64_t Scalar(const char *path, const char *sql) {
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db != nullptr) sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt *statement = nullptr;
    int64_t value = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW) {
        value = sqlite3_column_int64(statement, 0);
    }
    if (statement != nullptr) sqlite3_finalize(statement);
    sqlite3_close(db);
    return value;
}

bool Execute(const char *path, const char *sql) {
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (db != nullptr) sqlite3_close(db);
        return false;
    }
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return rc == SQLITE_OK;
}

int RunFindBenchmark(int file_count) {
    if (file_count <= 0 || file_count > 100000) {
        std::fprintf(stderr, "find benchmark file count must be 1..100000\n");
        return 2;
    }
    char path[] = "/tmp/vexfs-find-benchmark-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) return 1;
    close(fd);
    unlink(path);

    sqlite3 *db = nullptr;
    if (sqlite3_open(path, &db) != SQLITE_OK || vexdb_sqlite_register(db) != SQLITE_OK) {
        if (db != nullptr) sqlite3_close(db);
        unlink(path);
        return 1;
    }
    const auto seed_started = std::chrono::steady_clock::now();
    if (sqlite3_exec(db,
            "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
            "PRAGMA temp_store=FILE; PRAGMA cache_size=-65536;"
            "SELECT vexfs_init();"
            "SELECT vexfs_workspace_create('find-benchmark');"
            "SELECT vexfs_mkdir('find-benchmark','/files');"
            "BEGIN IMMEDIATE;",
            nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::fprintf(stderr, "find benchmark setup: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        unlink(path);
        return 1;
    }
    sqlite3_stmt *create = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT vexfs_create('find-benchmark',?1,'file',420)",
            -1, &create, nullptr) != SQLITE_OK) {
        std::fprintf(stderr, "find benchmark prepare: %s\n", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        unlink(path);
        return 1;
    }
    bool seed_ok = true;
    for (int index = 0; index < file_count; ++index) {
        char file_path[64];
        std::snprintf(file_path, sizeof(file_path), "/files/f%06d.txt", index);
        sqlite3_bind_text(create, 1, file_path, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(create) != SQLITE_ROW) {
            std::fprintf(stderr, "find benchmark create %d: %s\n", index,
                         sqlite3_errmsg(db));
            seed_ok = false;
            break;
        }
        sqlite3_reset(create);
        sqlite3_clear_bindings(create);
    }
    sqlite3_finalize(create);
    if (!seed_ok || sqlite3_exec(db, seed_ok ? "COMMIT" : "ROLLBACK",
                                 nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::fprintf(stderr, "find benchmark commit: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        unlink(path);
        return 1;
    }
    const double seed_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - seed_started).count();
    sqlite3_close(db);

    vexfs_mount_config config{};
    config.abi_version = VEXFS_RUNTIME_ABI_VERSION;
    config.backend = VEXFS_RUNTIME_BACKEND_SQLITE;
    config.connection = path;
    config.workspace = "find-benchmark";
    config.principal = "local";
    config.operation_timeout_ms = 30000;
    config.flags = VEXFS_RUNTIME_OPEN_NO_CREATE;
    vexfs_mount_error error{};
    vexfs_mount_session *session = nullptr;
    if (vexfs_mount_session_open(&config, &session, &error) != VEXFS_MOUNT_OK)
        return Fail("find benchmark open", error);

    vexfs_mount_bytes page{};
    auto query_started = std::chrono::steady_clock::now();
    if (vexfs_mount_find(session, "/files", "*.txt", "file", 0, 0, -1, -1,
                         "", 100, &page, &error) != VEXFS_MOUNT_OK ||
        Count(page, "\"path\":") != 100) {
        return Fail("find benchmark first page", error);
    }
    const double first_page_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - query_started).count();
    const std::string cursor = JsonString(page, "next_cursor");
    vexfs_mount_free(page.data);
    if (cursor.empty()) return Fail("find benchmark cursor", error);

    page = {};
    query_started = std::chrono::steady_clock::now();
    if (vexfs_mount_find(session, "/files", "*.txt", "file", 0, 0, -1, -1,
                         cursor.c_str(), 100, &page, &error) != VEXFS_MOUNT_OK ||
        Count(page, "\"path\":") != 100) {
        return Fail("find benchmark second page", error);
    }
    const double second_page_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - query_started).count();
    vexfs_mount_free(page.data);

    char last_name[32];
    std::snprintf(last_name, sizeof(last_name), "f%06d.txt", file_count - 1);
    page = {};
    query_started = std::chrono::steady_clock::now();
    if (vexfs_mount_find(session, "/files", last_name, "file", 0, 0, -1, -1,
                         "", 10, &page, &error) != VEXFS_MOUNT_OK ||
        Count(page, "\"path\":") != 1 || !Contains(page, last_name)) {
        return Fail("find benchmark selective", error);
    }
    const double selective_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - query_started).count();
    vexfs_mount_free(page.data);
    vexfs_mount_session_close(session);

    struct stat info {};
    const int64_t database_bytes = stat(path, &info) == 0 ? info.st_size : -1;
    std::printf(
        "{\"files\":%d,\"seed_seconds\":%.6f,"
        "\"find_first_page_seconds\":%.6f,\"find_second_page_seconds\":%.6f,"
        "\"find_selective_seconds\":%.6f,\"database_bytes\":%lld}\n",
        file_count, seed_seconds, first_page_seconds, second_page_seconds,
        selective_seconds, static_cast<long long>(database_bytes));
    unlink(path);
    const std::string wal = std::string(path) + "-wal";
    const std::string shm = std::string(path) + "-shm";
    unlink(wal.c_str());
    unlink(shm.c_str());
    return 0;
}

int RunBenchmark(int file_count) {
    if (file_count <= 0 || file_count > 100000) {
        std::fprintf(stderr, "benchmark file count must be 1..100000\n");
        return 2;
    }
    char path[] = "/tmp/vexfs-contract-benchmark-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) return 1;
    close(fd);
    unlink(path);

    vexfs_mount_config config{};
    config.abi_version = VEXFS_RUNTIME_ABI_VERSION;
    config.backend = VEXFS_RUNTIME_BACKEND_SQLITE;
    config.connection = path;
    config.workspace = "benchmark";
    config.principal = "local";
    config.operation_timeout_ms = 1000;
    config.flags = VEXFS_RUNTIME_EXCLUSIVE_GATEWAY;
    vexfs_mount_error error{};
    vexfs_mount_session *session = nullptr;
    if (vexfs_mount_session_open(&config, &session, &error) != VEXFS_MOUNT_OK)
        return Fail("benchmark open", error);
    if (vexfs_mount_mkdir(session, "/files", &error) != VEXFS_MOUNT_OK)
        return Fail("benchmark mkdir", error);

    const int64_t commits_before = Scalar(path, "SELECT count(*) FROM _vexfs_commits");
    const int64_t versions_before = Scalar(path, "SELECT count(*) FROM _vexfs_file_versions");
    const int64_t requests_before = Scalar(path, "SELECT count(*) FROM _vexfs_requests");
    const char payload[] = "vexfs benchmark payload\n";
    double create_call_seconds = 0.0;
    double stat_call_seconds = 0.0;
    double xattr_call_seconds = 0.0;
    double stage_call_seconds = 0.0;
    double publish_call_seconds = 0.0;
    const auto started = std::chrono::steady_clock::now();
    for (int index = 0; index < file_count; ++index) {
        char file_path[64];
        char request[64];
        std::snprintf(file_path, sizeof(file_path), "/files/f%06d.txt", index);
        std::snprintf(request, sizeof(request), "create-%06d", index);
        vexfs_mount_bytes handle{};
        vexfs_mount_bytes item_stat{};
        auto call_started = std::chrono::steady_clock::now();
        if (vexfs_mount_handle_create_owned_stat_durable(
                session, file_path, 0644, getuid(), getgid(), request, "none",
                &handle, &item_stat, &error) != VEXFS_MOUNT_OK)
            return Fail("benchmark create", error);
        create_call_seconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - call_started).count();
        const std::string handle_id(static_cast<const char *>(handle.data),
                                    static_cast<size_t>(handle.size));
        vexfs_mount_free(handle.data);
        const int64_t inode = JsonInteger(item_stat, "inode");
        vexfs_mount_free(item_stat.data);
        call_started = std::chrono::steady_clock::now();
        if (vexfs_mount_xattr_set(session, inode, "com.apple.provenance", "p", 1,
                                  VEXFS_MOUNT_XATTR_ALWAYS_SET, &error) != VEXFS_MOUNT_OK)
            return Fail("benchmark xattr", error);
        xattr_call_seconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - call_started).count();
        int64_t generation = 0;
        std::snprintf(request, sizeof(request), "write-%06d", index);
        call_started = std::chrono::steady_clock::now();
        if (vexfs_mount_handle_stage_write_durable(
                session, handle_id.c_str(), 0, payload, sizeof(payload) - 1,
                request, "none", &generation, &error) != VEXFS_MOUNT_OK)
            return Fail("benchmark stage", error);
        stage_call_seconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - call_started).count();
    }
    const auto publish_started = std::chrono::steady_clock::now();
    int64_t batch_published = 0;
    int64_t total_published = 0;
    while (total_published < file_count) {
        if (vexfs_mount_publish_close_batch(session, "full", 64,
                                            &batch_published, &error) !=
                VEXFS_MOUNT_OK || batch_published <= 0 || batch_published > 64) {
            return Fail("benchmark bounded publish close", error);
        }
        total_published += batch_published;
    }
    if (total_published != file_count) {
        std::fprintf(stderr, "benchmark batch published=%lld expected=%d\n",
                     static_cast<long long>(total_published), file_count);
        return Fail("benchmark batch publish close", error);
    }
    publish_call_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - publish_started).count();
    vexfs_mount_bytes find_page{};
    auto find_started = std::chrono::steady_clock::now();
    if (vexfs_mount_find(session, "/files", "*.txt", "file",
                         sizeof(payload) - 1, sizeof(payload) - 1, -1, -1,
                         "", 100, &find_page, &error) != VEXFS_MOUNT_OK ||
        Count(find_page, "\"path\":") != 100) {
        return Fail("benchmark find first page", error);
    }
    const double find_first_page_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - find_started).count();
    const std::string find_cursor = JsonString(find_page, "next_cursor");
    vexfs_mount_free(find_page.data);
    if (find_cursor.empty()) return Fail("benchmark find cursor", error);
    find_page = {};
    find_started = std::chrono::steady_clock::now();
    if (vexfs_mount_find(session, "/files", "*.txt", "file",
                         sizeof(payload) - 1, sizeof(payload) - 1, -1, -1,
                         find_cursor.c_str(), 100, &find_page, &error) != VEXFS_MOUNT_OK ||
        Count(find_page, "\"path\":") != 100) {
        return Fail("benchmark find second page", error);
    }
    const double find_second_page_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - find_started).count();
    vexfs_mount_free(find_page.data);
    char last_name[32];
    std::snprintf(last_name, sizeof(last_name), "f%06d.txt", file_count - 1);
    find_page = {};
    find_started = std::chrono::steady_clock::now();
    if (vexfs_mount_find(session, "/files", last_name, "file", -1, -1, -1, -1,
                         "", 10, &find_page, &error) != VEXFS_MOUNT_OK ||
        Count(find_page, "\"path\":") != 1 || !Contains(find_page, last_name)) {
        return Fail("benchmark find selective", error);
    }
    const double find_selective_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - find_started).count();
    vexfs_mount_free(find_page.data);
    const double create_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    const int64_t commits = Scalar(path, "SELECT count(*) FROM _vexfs_commits") - commits_before;
    const int64_t versions = Scalar(path, "SELECT count(*) FROM _vexfs_file_versions") - versions_before;
    const int64_t requests = Scalar(path, "SELECT count(*) FROM _vexfs_requests") - requests_before;

    const auto sync_started = std::chrono::steady_clock::now();
    int64_t published = -1;
    if (vexfs_mount_synchronize(session, "benchmark-sync", &published, &error) !=
            VEXFS_MOUNT_OK || published != 0)
        return Fail("benchmark synchronize", error);
    const double sync_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - sync_started).count();
    vexfs_mount_bytes diagnostics{};
    if (vexfs_mount_diagnostics(session, &diagnostics, &error) != VEXFS_MOUNT_OK)
        return Fail("benchmark diagnostics", error);
    const int64_t ordinary_calls = JsonInteger(diagnostics, "ordinary_mutation_calls");
    const int64_t full_calls = JsonInteger(diagnostics, "full_boundary_calls");
    const int64_t barriers = JsonInteger(diagnostics, "durability_barriers");
    vexfs_mount_free(diagnostics.data);
    vexfs_mount_session_close(session);

    struct stat info {};
    const int64_t database_bytes = stat(path, &info) == 0 ? info.st_size : -1;
    std::printf(
        "{\"files\":%d,\"create_seconds\":%.6f,\"files_per_second\":%.3f,"
        "\"sync_seconds\":%.6f,\"commit_rows\":%lld,\"version_rows\":%lld,"
        "\"request_rows\":%lld,\"commits_per_file\":%.3f,"
        "\"versions_per_file\":%.3f,\"requests_per_file\":%.3f,"
        "\"create_call_seconds\":%.6f,\"stat_call_seconds\":%.6f,"
        "\"xattr_call_seconds\":%.6f,\"stage_call_seconds\":%.6f,"
        "\"publish_call_seconds\":%.6f,"
        "\"find_first_page_seconds\":%.6f,\"find_second_page_seconds\":%.6f,"
        "\"find_selective_seconds\":%.6f,"
        "\"ordinary_mutation_calls\":%lld,\"full_boundary_calls\":%lld,"
        "\"durability_barriers\":%lld,\"database_bytes\":%lld}\n",
        file_count, create_seconds, file_count / std::max(create_seconds, 1e-9),
        sync_seconds, static_cast<long long>(commits), static_cast<long long>(versions),
        static_cast<long long>(requests), static_cast<double>(commits) / file_count,
        static_cast<double>(versions) / file_count,
        static_cast<double>(requests) / file_count,
        create_call_seconds, stat_call_seconds, xattr_call_seconds,
        stage_call_seconds, publish_call_seconds,
        find_first_page_seconds, find_second_page_seconds, find_selective_seconds,
        static_cast<long long>(ordinary_calls), static_cast<long long>(full_calls),
        static_cast<long long>(barriers), static_cast<long long>(database_bytes));
    unlink(path);
    std::string wal = std::string(path) + "-wal";
    std::string shm = std::string(path) + "-shm";
    unlink(wal.c_str());
    unlink(shm.c_str());
    return commits == file_count && versions == file_count &&
        requests <= static_cast<int64_t>(file_count) * 2 ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 3 && std::strcmp(argv[1], "--find-benchmark") == 0) {
        return RunFindBenchmark(std::atoi(argv[2]));
    }
    if (argc == 3 && std::strcmp(argv[1], "--benchmark") == 0) {
        return RunBenchmark(std::atoi(argv[2]));
    }
    char path[] = "/tmp/vexfs-contract-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) return 1;
    close(fd);
    unlink(path);

    vexfs_mount_config config{};
    config.abi_version = VEXFS_RUNTIME_ABI_VERSION;
    config.backend = VEXFS_RUNTIME_BACKEND_SQLITE;
    config.connection = path;
    config.workspace = "default";
    config.principal = "local";
    config.operation_timeout_ms = 1000;
    vexfs_mount_error error{};
    vexfs_mount_session *session = nullptr;
    vexfs_mount_config unsupported_config = config;
    unsupported_config.backend = "postgres";
    vexfs_mount_session *unsupported_session = nullptr;
    if (vexfs_mount_session_open(&unsupported_config, &unsupported_session, &error) !=
            VEXFS_MOUNT_UNSUPPORTED || unsupported_session != nullptr)
        return Fail("sqlite rejects unsupported backend", error);
    vexfs_mount_config rejected_config = config;
    rejected_config.principal = "alice";
    vexfs_mount_session *rejected_session = nullptr;
    if (vexfs_mount_session_open(&rejected_config, &rejected_session, &error) !=
            VEXFS_MOUNT_PERMISSION_DENIED || rejected_session != nullptr)
        return Fail("sqlite rejects non-local principal", error);
    if (vexfs_mount_session_open(&config, &session, &error) != VEXFS_MOUNT_OK)
        return Fail("open", error);

    vexfs_mount_bytes diagnostics{};
    if (vexfs_mount_diagnostics(session, &diagnostics, &error) != VEXFS_MOUNT_OK)
        return Fail("diagnostics", error);
    if (!Contains(diagnostics, "\"schema_version\":\"0.9.0\"") ||
        !Contains(diagnostics, "\"workspace_exists\":1"))
        return Fail("diagnostics content", error);
    vexfs_mount_free(diagnostics.data);

    if (vexfs_mount_mkdir(session, "/agent", &error) != VEXFS_MOUNT_OK)
        return Fail("mkdir", error);
    int64_t version = 0;
    if (vexfs_mount_write_file(session, "/agent/task.txt", "draft", 5, &version, &error) !=
        VEXFS_MOUNT_OK) return Fail("write", error);

    vexfs_mount_visibility visibility{};
    if (vexfs_mount_refresh_visibility(session, &visibility, &error) != VEXFS_MOUNT_OK ||
        visibility.workspace_head <= 0 || visibility.cache_generation != 1 ||
        visibility.external_commit != 0)
        return Fail("initial cache visibility", error);
    vexfs_mount_config peer_config = config;
    vexfs_mount_session *peer_session = nullptr;
    if (vexfs_mount_session_open(&peer_config, &peer_session, &error) != VEXFS_MOUNT_OK)
        return Fail("visibility peer open", error);
    int64_t peer_version = 0;
    if (vexfs_mount_write_file(peer_session, "/agent/external.txt", "peer", 4,
                               &peer_version, &error) != VEXFS_MOUNT_OK)
        return Fail("visibility peer write", error);
    if (vexfs_mount_refresh_visibility(session, &visibility, &error) != VEXFS_MOUNT_OK ||
        visibility.cache_generation != 2 || visibility.external_commit != 1)
        return Fail("external commit visibility", error);
    const int64_t external_head = visibility.workspace_head;
    if (vexfs_mount_refresh_visibility(session, &visibility, &error) != VEXFS_MOUNT_OK ||
        visibility.workspace_head != external_head || visibility.cache_generation != 2 ||
        visibility.external_commit != 0)
        return Fail("stable cache visibility", error);
    vexfs_mount_session_close(peer_session);

    vexfs_mount_bytes versioned_list{};
    if (vexfs_mount_list_versioned(session, "/agent", &versioned_list, &error) !=
            VEXFS_MOUNT_OK || JsonInteger(versioned_list, "version") <= 0 ||
        !Contains(versioned_list, "\"name\":\"task.txt\""))
        return Fail("versioned directory list", error);
    vexfs_mount_free(versioned_list.data);
    vexfs_mount_bytes grep{};
    if (vexfs_mount_grep(session, "/agent", "RAF", VEXFS_MOUNT_GREP_IGNORE_CASE,
                         100, &grep, &error) != VEXFS_MOUNT_OK ||
        !Contains(grep, "\"path\":\"/agent/task.txt\"") ||
        JsonInteger(grep, "match_count") != 1)
        return Fail("database grep", error);
    vexfs_mount_free(grep.data);
    if (vexfs_mount_grep(session, "/agent", "draft", 8, 100, &grep, &error) !=
        VEXFS_MOUNT_INVALID_ARGUMENT) return Fail("database grep flags", error);
    vexfs_mount_bytes grep_index{};
    if (vexfs_mount_grep_index(session, "enable", &grep_index, &error) != VEXFS_MOUNT_OK ||
        !Contains(grep_index, "\"available\":true") ||
        !Contains(grep_index, "\"backend\":\"fts5-trigram\""))
        return Fail("database grep index enable", error);
    vexfs_mount_free(grep_index.data);
    grep = {};
    if (vexfs_mount_grep(session, "/agent", "draft", 0, 100, &grep, &error) !=
            VEXFS_MOUNT_OK || !Contains(grep, "\"index_used\":true") ||
            JsonInteger(grep, "match_count") != 1)
        return Fail("indexed database grep", error);
    vexfs_mount_free(grep.data);
    if (vexfs_mount_remove(session, "/agent", 0, &error) != VEXFS_MOUNT_NOT_EMPTY)
        return Fail("non-empty directory", error);
    vexfs_mount_bytes task_stat{};
    if (vexfs_mount_stat(session, "/agent/task.txt", &task_stat, &error) != VEXFS_MOUNT_OK)
        return Fail("xattr stat", error);
    const int64_t task_inode = JsonInteger(task_stat, "inode");
    vexfs_mount_free(task_stat.data);
    if (task_inode <= 0) return Fail("xattr inode", error);
    if (vexfs_mount_set_mode(session, task_inode, 0750, &error) != VEXFS_MOUNT_OK)
        return Fail("set mode", error);
    task_stat = {};
    if (vexfs_mount_stat(session, "/agent/task.txt", &task_stat, &error) != VEXFS_MOUNT_OK ||
        JsonInteger(task_stat, "mode") != 0750)
        return Fail("set mode stat", error);
    vexfs_mount_free(task_stat.data);
    if (vexfs_mount_set_mode(session, task_inode, 01000, &error) !=
        VEXFS_MOUNT_INVALID_ARGUMENT) return Fail("invalid mode", error);
    if (vexfs_mount_set_times(session, task_inode, 1700000000123LL, 1700000100456LL,
                              VEXFS_MOUNT_TIME_ACCESS | VEXFS_MOUNT_TIME_MODIFY,
                              &error) != VEXFS_MOUNT_OK)
        return Fail("set times", error);
    task_stat = {};
    if (vexfs_mount_stat(session, "/agent/task.txt", &task_stat, &error) != VEXFS_MOUNT_OK ||
        JsonInteger(task_stat, "accessed_at") != 1700000000123LL ||
        JsonInteger(task_stat, "updated_at") != 1700000100456LL)
        return Fail("set times stat", error);
    vexfs_mount_free(task_stat.data);
    if (vexfs_mount_set_times(session, task_inode, -1, 0, VEXFS_MOUNT_TIME_ACCESS,
                              &error) != VEXFS_MOUNT_INVALID_ARGUMENT)
        return Fail("invalid times", error);
    if (vexfs_mount_symlink(session, "/agent/task-link", "task.txt", 8, &error) !=
        VEXFS_MOUNT_OK) return Fail("symlink", error);
    if (vexfs_mount_symlink(session, "/agent/task-link", "duplicate", 9, &error) !=
        VEXFS_MOUNT_CONFLICT) return Fail("duplicate symlink", error);
    vexfs_mount_bytes link_stat{};
    if (vexfs_mount_stat(session, "/agent/task-link", &link_stat, &error) != VEXFS_MOUNT_OK ||
        !Contains(link_stat, "\"kind\":\"symlink\"") || JsonInteger(link_stat, "size") != 8)
        return Fail("symlink stat", error);
    const int64_t link_inode = JsonInteger(link_stat, "inode");
    vexfs_mount_free(link_stat.data);
    vexfs_mount_bytes target{};
    if (vexfs_mount_readlink(session, link_inode, &target, &error) != VEXFS_MOUNT_OK ||
        !Equals(target, "task.txt")) return Fail("readlink", error);
    vexfs_mount_free(target.data);
    target = {};
    if (vexfs_mount_readlink(session, task_inode, &target, &error) !=
        VEXFS_MOUNT_INVALID_ARGUMENT) return Fail("readlink regular file", error);
    vexfs_mount_bytes found{};
    if (vexfs_mount_find(session, "/agent", "*.txt", "file", 4, 5, -1, -1,
                         "", 1, &found, &error) != VEXFS_MOUNT_OK ||
        !Contains(found, "\"path\":\"/agent/external.txt\"") ||
        !Contains(found, "\"next_cursor\":\"/agent/external.txt\"") ||
        Contains(found, "/agent/task.txt"))
        return Fail("find first page", error);
    vexfs_mount_free(found.data);
    found = {};
    if (vexfs_mount_find(session, "/agent", "*.txt", "file", 4, 5, -1, -1,
                         "/agent/external.txt", 1, &found, &error) != VEXFS_MOUNT_OK ||
        !Contains(found, "\"path\":\"/agent/task.txt\"") ||
        !Contains(found, "\"next_cursor\":null"))
        return Fail("find next page", error);
    vexfs_mount_free(found.data);
    found = {};
    if (vexfs_mount_find(session, "/agent", "*.txt", "file", -1, -1, -1,
                         1700000200000LL, "", 10, &found, &error) != VEXFS_MOUNT_OK ||
        !Contains(found, "/agent/task.txt") || Contains(found, "/agent/external.txt"))
        return Fail("find modified time", error);
    vexfs_mount_free(found.data);
    found = {};
    if (vexfs_mount_find(session, "/agent", "", "socket", -1, -1, -1, -1,
                         "", 10, &found, &error) != VEXFS_MOUNT_INVALID_ARGUMENT)
        return Fail("find invalid kind", error);
    if (vexfs_mount_xattr_set(session, task_inode, "com.vexfs.test", "alpha", 5,
                              VEXFS_MOUNT_XATTR_MUST_CREATE, &error) != VEXFS_MOUNT_OK)
        return Fail("xattr create", error);
    if (vexfs_mount_xattr_set(session, task_inode, "com.vexfs.test", "duplicate", 9,
                              VEXFS_MOUNT_XATTR_MUST_CREATE, &error) != VEXFS_MOUNT_CONFLICT)
        return Fail("xattr duplicate create", error);
    vexfs_mount_bytes xattr{};
    if (vexfs_mount_xattr_get(session, task_inode, "com.vexfs.test", &xattr, &error) !=
            VEXFS_MOUNT_OK || !Equals(xattr, "alpha"))
        return Fail("xattr get", error);
    vexfs_mount_free(xattr.data);
    if (vexfs_mount_xattr_set(session, task_inode, "com.vexfs.test", "beta", 4,
                              VEXFS_MOUNT_XATTR_MUST_REPLACE, &error) != VEXFS_MOUNT_OK)
        return Fail("xattr replace", error);
    vexfs_mount_bytes xattr_names{};
    if (vexfs_mount_xattr_list(session, task_inode, &xattr_names, &error) != VEXFS_MOUNT_OK ||
        !Contains(xattr_names, "com.vexfs.test"))
        return Fail("xattr list", error);
    vexfs_mount_free(xattr_names.data);

    if (vexfs_mount_link(session, "/agent/task.txt", "/agent/task-copy.txt", &error) !=
        VEXFS_MOUNT_OK) return Fail("hard link", error);
    vexfs_mount_bytes hardlink_stat{};
    if (vexfs_mount_stat(session, "/agent/task-copy.txt", &hardlink_stat, &error) !=
            VEXFS_MOUNT_OK || JsonInteger(hardlink_stat, "inode") != task_inode ||
            JsonInteger(hardlink_stat, "link_count") != 2)
        return Fail("hard link stat", error);
    vexfs_mount_free(hardlink_stat.data);
    if (vexfs_mount_chown(session, task_inode, -1, 1234, &error) != VEXFS_MOUNT_OK)
        return Fail("chown keep uid", error);
    task_stat = {};
    if (vexfs_mount_stat(session, "/agent/task.txt", &task_stat, &error) != VEXFS_MOUNT_OK ||
        JsonInteger(task_stat, "gid") != 1234)
        return Fail("chown stat", error);
    vexfs_mount_free(task_stat.data);
    const char acl_json[] =
        "[{\"principal\":\"alice\",\"effect\":\"allow\","
        "\"permissions\":\"read,write\",\"inherit\":1}]";
    if (vexfs_mount_acl_set(session, task_inode, acl_json, sizeof(acl_json) - 1, &error) !=
        VEXFS_MOUNT_OK) return Fail("acl set", error);
    vexfs_mount_bytes acl{};
    if (vexfs_mount_acl_get(session, task_inode, &acl, &error) != VEXFS_MOUNT_OK ||
        !Contains(acl, "alice") || !Contains(acl, "read,write"))
        return Fail("acl get", error);
    vexfs_mount_free(acl.data);

    int64_t snapshot_commit = 0;
    if (vexfs_mount_snapshot_create(session, "before-change", 0, &snapshot_commit, &error) !=
            VEXFS_MOUNT_OK || snapshot_commit <= 0)
        return Fail("snapshot create", error);
    vexfs_mount_bytes snapshots{};
    if (vexfs_mount_snapshot_list(session, &snapshots, &error) != VEXFS_MOUNT_OK ||
        !Contains(snapshots, "before-change")) return Fail("snapshot list", error);
    vexfs_mount_free(snapshots.data);
    if (vexfs_mount_write_file(session, "/agent/task.txt", "changed", 7, &version, &error) !=
        VEXFS_MOUNT_OK) return Fail("snapshot mutation write", error);
    if (vexfs_mount_set_mode(session, task_inode, 0600, &error) != VEXFS_MOUNT_OK)
        return Fail("snapshot mutation mode", error);
    if (vexfs_mount_remove(session, "/agent/task-link", 0, &error) != VEXFS_MOUNT_OK)
        return Fail("snapshot mutation remove", error);
    if (vexfs_mount_xattr_set(session, task_inode, "com.vexfs.test", "gamma", 5,
                              VEXFS_MOUNT_XATTR_MUST_REPLACE, &error) != VEXFS_MOUNT_OK)
        return Fail("snapshot mutation xattr", error);
    vexfs_mount_bytes snapshot_diff{};
    if (vexfs_mount_snapshot_diff(session, "before-change", "HEAD", &snapshot_diff, &error) !=
            VEXFS_MOUNT_OK || !Contains(snapshot_diff, "\"changes\":[{") ||
            !Contains(snapshot_diff, "/agent/task-link"))
        return Fail("snapshot diff", error);
    vexfs_mount_free(snapshot_diff.data);
    vexfs_mount_bytes snapshot_show{};
    if (vexfs_mount_snapshot_show(session, "before-change", &snapshot_show, &error) !=
            VEXFS_MOUNT_OK || !Contains(snapshot_show, "/agent/task.txt"))
        return Fail("snapshot show", error);
    vexfs_mount_free(snapshot_show.data);
    int64_t head_commit = 0;
    if (vexfs_mount_workspace_head(session, &head_commit, &error) != VEXFS_MOUNT_OK ||
        head_commit <= snapshot_commit) return Fail("workspace head", error);
    int64_t restored_commit = 0;
    vexfs_mount_config mounted_config = config;
    mounted_config.flags = VEXFS_RUNTIME_EXCLUSIVE_GATEWAY;
    vexfs_mount_session *mounted_session = nullptr;
    if (vexfs_mount_session_open(&mounted_config, &mounted_session, &error) != VEXFS_MOUNT_OK)
        return Fail("mounted restore guard open", error);
    if (vexfs_mount_snapshot_restore(session, "before-change", head_commit,
                                     &restored_commit, &error) != VEXFS_MOUNT_BUSY ||
        std::strstr(error.message, "active mount session") == nullptr)
        return Fail("mounted snapshot restore must be busy", error);
    vexfs_mount_session_close(mounted_session);
    if (vexfs_mount_snapshot_restore_safe(
            session, "before-change", head_commit, "safety-before-change-restore",
            &restored_commit, &error) != VEXFS_MOUNT_OK ||
        restored_commit <= head_commit) return Fail("snapshot restore", error);
    snapshots = {};
    if (vexfs_mount_snapshot_list(session, &snapshots, &error) != VEXFS_MOUNT_OK ||
        !Contains(snapshots, "safety-before-change-restore"))
        return Fail("snapshot restore safety point", error);
    vexfs_mount_free(snapshots.data);
    vexfs_mount_bytes snapshot_content{};
    if (vexfs_mount_read_file(session, "/agent/task.txt", &snapshot_content, &error) !=
            VEXFS_MOUNT_OK || !Equals(snapshot_content, "draft"))
        return Fail("snapshot restored content", error);
    vexfs_mount_free(snapshot_content.data);
    grep = {};
    if (vexfs_mount_grep(session, "/agent", "draft", 0, 100, &grep, &error) !=
            VEXFS_MOUNT_OK || !Contains(grep, "\"index_used\":true") ||
            JsonInteger(grep, "match_count") != 2 || Contains(grep, "changed"))
        return Fail("snapshot restored grep index", error);
    vexfs_mount_free(grep.data);
    task_stat = {};
    if (vexfs_mount_stat(session, "/agent/task.txt", &task_stat, &error) != VEXFS_MOUNT_OK ||
        JsonInteger(task_stat, "mode") != 0750) return Fail("snapshot restored mode", error);
    vexfs_mount_free(task_stat.data);
    target = {};
    if (vexfs_mount_readlink(session, link_inode, &target, &error) != VEXFS_MOUNT_OK ||
        !Equals(target, "task.txt")) return Fail("snapshot restored symlink", error);
    vexfs_mount_free(target.data);
    hardlink_stat = {};
    if (vexfs_mount_stat(session, "/agent/task-copy.txt", &hardlink_stat, &error) !=
            VEXFS_MOUNT_OK || JsonInteger(hardlink_stat, "inode") != task_inode ||
            JsonInteger(hardlink_stat, "link_count") != 2)
        return Fail("snapshot restored hard link", error);
    vexfs_mount_free(hardlink_stat.data);
    task_stat = {};
    if (vexfs_mount_stat(session, "/agent/task.txt", &task_stat, &error) != VEXFS_MOUNT_OK ||
        JsonInteger(task_stat, "gid") != 1234)
        return Fail("snapshot restored owner", error);
    vexfs_mount_free(task_stat.data);
    acl = {};
    if (vexfs_mount_acl_get(session, task_inode, &acl, &error) != VEXFS_MOUNT_OK ||
        !Contains(acl, "alice")) return Fail("snapshot restored acl", error);
    vexfs_mount_free(acl.data);
    xattr = {};
    if (vexfs_mount_xattr_get(session, task_inode, "com.vexfs.test", &xattr, &error) !=
            VEXFS_MOUNT_OK || !Equals(xattr, "beta"))
        return Fail("snapshot restored xattr", error);
    vexfs_mount_free(xattr.data);
    if (vexfs_mount_snapshot_restore(session, "before-change", head_commit,
                                     &restored_commit, &error) != VEXFS_MOUNT_CONFLICT)
        return Fail("stale snapshot restore", error);

    if (vexfs_mount_write_file(session, "/agent/versioned.txt", "old", 3, &version, &error) !=
            VEXFS_MOUNT_OK || version != 1)
        return Fail("version seed", error);
    if (vexfs_mount_write_file(session, "/agent/versioned.txt", "new", 3, &version, &error) !=
            VEXFS_MOUNT_OK || version != 2)
        return Fail("version update", error);
    std::string range_source(70032, 'x');
    range_source.replace(65530, 16, "0123456789abcdef");
    if (vexfs_mount_write_file(session, "/agent/range.bin", range_source.data(),
                               range_source.size(), &version, &error) != VEXFS_MOUNT_OK)
        return Fail("range seed", error);
    vexfs_mount_bytes range_content{};
    if (vexfs_mount_read_file_range(session, "/agent/range.bin", 65530, 16,
                                    &range_content, &error) != VEXFS_MOUNT_OK ||
        !Equals(range_content, "0123456789abcdef"))
        return Fail("range read across chunks", error);
    vexfs_mount_free(range_content.data);
    if (vexfs_mount_write_file_range(session, "/agent/range.bin", 65534,
                                     "RANGE", 5, "range-write-1", "full",
                                     &version, &error) != VEXFS_MOUNT_OK)
        return Fail("range write", error);
    range_content = {};
    if (vexfs_mount_read_file_range(session, "/agent/range.bin", 65530, 16,
                                    &range_content, &error) != VEXFS_MOUNT_OK ||
        !Equals(range_content, "0123RANGE9abcdef"))
        return Fail("range read after write", error);
    vexfs_mount_free(range_content.data);
    vexfs_mount_bytes history{};
    if (vexfs_mount_history(session, "/agent/versioned.txt", &history, &error) != VEXFS_MOUNT_OK ||
        !Contains(history, "\"version\":2") || !Contains(history, "\"version\":1"))
        return Fail("version history", error);
    vexfs_mount_free(history.data);
    vexfs_mount_bytes historical{};
    if (vexfs_mount_read_version(session, "/agent/versioned.txt", 1, &historical, &error) !=
            VEXFS_MOUNT_OK || !Equals(historical, "old"))
        return Fail("read historical version", error);
    vexfs_mount_free(historical.data);
    int64_t restored = 0;
    if (vexfs_mount_restore_version(session, "/agent/versioned.txt", 1, 2, &restored, &error) !=
            VEXFS_MOUNT_OK || restored != 3)
        return Fail("restore version", error);
    vexfs_mount_bytes restored_content{};
    if (vexfs_mount_read_file(session, "/agent/versioned.txt", &restored_content, &error) !=
            VEXFS_MOUNT_OK || !Equals(restored_content, "old"))
        return Fail("restored content", error);
    vexfs_mount_free(restored_content.data);
    if (vexfs_mount_restore_version(session, "/agent/versioned.txt", 2, 2, &restored, &error) !=
        VEXFS_MOUNT_CONFLICT) return Fail("stale restore must conflict", error);

    vexfs_mount_bytes handle{};
    if (vexfs_mount_handle_open(session, "/agent/task.txt", "rw", "open-cabi-1",
                                &handle, &error) != VEXFS_MOUNT_OK)
        return Fail("handle open", error);
    std::string handle_id(static_cast<const char *>(handle.data), static_cast<size_t>(handle.size));
    vexfs_mount_free(handle.data);

    int64_t generation = 0;
    if (vexfs_mount_handle_stage_write(session, handle_id.c_str(), 5, " ready", 6,
                                       "write-cabi-1", &generation, &error) != VEXFS_MOUNT_OK)
        return Fail("stage write", error);
    if (vexfs_mount_handle_publish(session, handle_id.c_str(), generation, "data",
                                   "publish-cabi-1", &version, &error) != VEXFS_MOUNT_OK)
        return Fail("publish", error);
    vexfs_mount_bytes state{};
    if (vexfs_mount_handle_close(session, handle_id.c_str(), 1, "close-cabi-1", &state,
                                 &error) != VEXFS_MOUNT_OK)
        return Fail("handle close", error);
    vexfs_mount_free(state.data);

    // 多次 write 只进入 staging；synchronize 一次发布最终 generation。
    if (vexfs_mount_write_file(session, "/agent/sync.txt", "A", 1, &version, &error) !=
        VEXFS_MOUNT_OK) return Fail("sync seed", error);
    handle = {};
    if (vexfs_mount_handle_open(session, "/agent/sync.txt", "rw", "open-sync-1",
                                &handle, &error) != VEXFS_MOUNT_OK)
        return Fail("sync handle open", error);
    handle_id.assign(static_cast<const char *>(handle.data), static_cast<size_t>(handle.size));
    vexfs_mount_free(handle.data);
    if (vexfs_mount_handle_stage_write(session, handle_id.c_str(), 1, "B", 1,
                                       "write-sync-1", &generation, &error) != VEXFS_MOUNT_OK)
        return Fail("sync stage 1", error);
    if (vexfs_mount_handle_stage_write(session, handle_id.c_str(), 2, "C", 1,
                                       "write-sync-2", &generation, &error) != VEXFS_MOUNT_OK)
        return Fail("sync stage 2", error);
    vexfs_mount_bytes content{};
    if (vexfs_mount_read_file(session, "/agent/sync.txt", &content, &error) != VEXFS_MOUNT_OK)
        return Fail("read before synchronize", error);
    if (!Equals(content, "A")) return Fail("staging must be invisible before synchronize", error);
    vexfs_mount_free(content.data);
    diagnostics = {};
    if (vexfs_mount_diagnostics(session, &diagnostics, &error) != VEXFS_MOUNT_OK ||
        !Contains(diagnostics, "\"pending_handles\":1"))
        return Fail("pending diagnostics", error);
    vexfs_mount_free(diagnostics.data);
    int64_t published = 0;
    if (vexfs_mount_synchronize(session, "sync-cabi-1", &published, &error) != VEXFS_MOUNT_OK ||
        published != 1) return Fail("synchronize", error);
    content = {};
    if (vexfs_mount_read_file(session, "/agent/sync.txt", &content, &error) != VEXFS_MOUNT_OK)
        return Fail("read after synchronize", error);
    if (!Equals(content, "ABC")) return Fail("synchronized content", error);
    vexfs_mount_free(content.data);
    state = {};
    if (vexfs_mount_handle_close(session, handle_id.c_str(), 1, "close-sync-1", &state,
                                 &error) != VEXFS_MOUNT_OK)
        return Fail("sync handle close", error);
    vexfs_mount_free(state.data);
    vexfs_mount_session_close(session);

    // 重开真实数据库文件，确认不是进程内假状态。
    session = nullptr;
    config.flags = VEXFS_RUNTIME_OPEN_NO_CREATE;
    if (vexfs_mount_session_open(&config, &session, &error) != VEXFS_MOUNT_OK)
        return Fail("reopen", error);
    content = {};
    if (vexfs_mount_read_file(session, "/agent/task.txt", &content, &error) != VEXFS_MOUNT_OK)
        return Fail("persistent read", error);
    if (!Equals(content, "draft ready")) return Fail("persistent content", error);
    vexfs_mount_free(content.data);
    task_stat = {};
    if (vexfs_mount_stat(session, "/agent/task.txt", &task_stat, &error) != VEXFS_MOUNT_OK ||
        JsonInteger(task_stat, "mode") != 0750)
        return Fail("persistent mode", error);
    vexfs_mount_free(task_stat.data);
    target = {};
    if (vexfs_mount_readlink(session, link_inode, &target, &error) != VEXFS_MOUNT_OK ||
        !Equals(target, "task.txt")) return Fail("persistent symlink", error);
    vexfs_mount_free(target.data);
    xattr = {};
    if (vexfs_mount_xattr_get(session, task_inode, "com.vexfs.test", &xattr, &error) !=
            VEXFS_MOUNT_OK || !Equals(xattr, "beta"))
        return Fail("persistent xattr", error);
    vexfs_mount_free(xattr.data);
    if (vexfs_mount_xattr_set(session, task_inode, "com.vexfs.test", nullptr, 0,
                              VEXFS_MOUNT_XATTR_DELETE, &error) != VEXFS_MOUNT_OK)
        return Fail("xattr delete", error);
    xattr = {};
    if (vexfs_mount_xattr_get(session, task_inode, "com.vexfs.test", &xattr, &error) !=
        VEXFS_MOUNT_NOT_FOUND) return Fail("deleted xattr must be missing", error);
    content = {};
    if (vexfs_mount_read_file(session, "/agent/sync.txt", &content, &error) != VEXFS_MOUNT_OK ||
        !Equals(content, "ABC")) return Fail("persistent synchronized content", error);
    vexfs_mount_free(content.data);

    vexfs_mount_bytes quota{};
    if (vexfs_mount_quota_get(session, &quota, &error) != VEXFS_MOUNT_OK ||
        !Contains(quota, "\"max_bytes\":null") ||
        JsonInteger(quota, "live_files") <= 0)
        return Fail("quota get", error);
    vexfs_mount_free(quota.data);
    quota = {};
    if (vexfs_mount_quota_set(session, -1, -1, 3, &quota, &error) != VEXFS_MOUNT_OK ||
        !Contains(quota, "\"max_file_bytes\":3"))
        return Fail("quota set", error);
    vexfs_mount_free(quota.data);
    if (vexfs_mount_write_file(session, "/agent/versioned.txt", "four", 4,
                               &version, &error) != VEXFS_MOUNT_NO_SPACE)
        return Fail("quota must reject oversized publish", error);
    content = {};
    if (vexfs_mount_read_file(session, "/agent/versioned.txt", &content, &error) !=
            VEXFS_MOUNT_OK || !Equals(content, "old"))
        return Fail("quota rejection must be atomic", error);
    vexfs_mount_free(content.data);
    quota = {};
    if (vexfs_mount_quota_set(session, -1, -1, -1, &quota, &error) != VEXFS_MOUNT_OK)
        return Fail("quota reset", error);
    vexfs_mount_free(quota.data);

    if (vexfs_mount_write_file(session, "/agent/gc.txt", "one", 3,
                               &version, &error) != VEXFS_MOUNT_OK ||
        vexfs_mount_write_file(session, "/agent/gc.txt", "two", 3,
                               &version, &error) != VEXFS_MOUNT_OK ||
        vexfs_mount_write_file(session, "/agent/gc.txt", "three", 5,
                               &version, &error) != VEXFS_MOUNT_OK)
        return Fail("retention seed", error);
    vexfs_mount_bytes retention{};
    if (vexfs_mount_retention_set(session, 1, 0, &retention, &error) != VEXFS_MOUNT_OK ||
        JsonInteger(retention, "reclaimable_versions") < 2)
        return Fail("retention set", error);
    vexfs_mount_free(retention.data);
    retention = {};
    if (vexfs_mount_gc_pause(session, 1, &retention, &error) != VEXFS_MOUNT_OK ||
        !Contains(retention, "\"gc_paused\":true"))
        return Fail("GC pause", error);
    vexfs_mount_free(retention.data);
    vexfs_mount_bytes gc{};
    if (vexfs_mount_gc(session, 1, &gc, &error) != VEXFS_MOUNT_BUSY)
        return Fail("paused GC must be busy", error);
    retention = {};
    if (vexfs_mount_gc_pause(session, 0, &retention, &error) != VEXFS_MOUNT_OK ||
        !Contains(retention, "\"gc_paused\":false"))
        return Fail("GC resume", error);
    vexfs_mount_free(retention.data);
    gc = {};
    if (vexfs_mount_gc(session, 1, &gc, &error) != VEXFS_MOUNT_OK ||
        JsonInteger(gc, "deleted_versions") != 1 ||
        !Contains(gc, "\"has_more\":true"))
        return Fail("bounded GC", error);
    vexfs_mount_free(gc.data);
    gc = {};
    if (vexfs_mount_gc(session, 1000, &gc, &error) != VEXFS_MOUNT_OK ||
        JsonInteger(gc, "remaining_reclaimable_versions") != 0)
        return Fail("complete GC", error);
    vexfs_mount_free(gc.data);
    retention = {};
    if (vexfs_mount_retention_set(session, 32, 30, &retention, &error) != VEXFS_MOUNT_OK)
        return Fail("retention reset", error);
    vexfs_mount_free(retention.data);
    vexfs_mount_session_close(session);

    // 独占挂载进程崩溃后，新 session 自动认领并发布该 session 已提交的 staging。
    // 这是 mount helper 重启恢复合同；再次 synchronize 必须保持幂等。
    config.flags = VEXFS_RUNTIME_EXCLUSIVE_GATEWAY;
    session = nullptr;
    if (vexfs_mount_session_open(&config, &session, &error) != VEXFS_MOUNT_OK)
        return Fail("crash seed open", error);

    handle = {};
    if (vexfs_mount_handle_create(session, "/agent/pending-snapshot.txt", 0644,
                                  "create-pending-snapshot", &handle, &error) !=
            VEXFS_MOUNT_OK)
        return Fail("pending snapshot create", error);
    handle_id.assign(static_cast<const char *>(handle.data), static_cast<size_t>(handle.size));
    vexfs_mount_free(handle.data);
    if (vexfs_mount_handle_stage_write(session, handle_id.c_str(), 0, "snap", 4,
                                       "write-pending-snapshot", &generation, &error) !=
            VEXFS_MOUNT_OK)
        return Fail("pending snapshot stage", error);
    // A management session must not silently snapshot stale HEAD while the
    // exclusive gateway owns dirty staging. It may explicitly request the
    // committed-only view, which must not contain the pending file.
    vexfs_mount_config admin_config = config;
    admin_config.flags = 0;
    vexfs_mount_session *admin_session = nullptr;
    if (vexfs_mount_session_open(&admin_config, &admin_session, &error) != VEXFS_MOUNT_OK)
        return Fail("admin session open", error);
    if (vexfs_mount_snapshot_create(admin_session, "must-not-be-stale", 0,
                                    &snapshot_commit, &error) != VEXFS_MOUNT_BUSY)
        return Fail("cross-session snapshot must be busy", error);
    if (vexfs_mount_snapshot_create(admin_session, "committed-only",
                                    VEXFS_SNAPSHOT_COMMITTED_ONLY,
                                    &snapshot_commit, &error) != VEXFS_MOUNT_OK)
        return Fail("committed-only snapshot", error);
    vexfs_mount_bytes committed_tree{};
    if (vexfs_mount_snapshot_show(admin_session, "committed-only", &committed_tree,
                                  &error) != VEXFS_MOUNT_OK ||
        Contains(committed_tree, "/agent/pending-snapshot.txt"))
        return Fail("committed-only snapshot excludes staging", error);
    vexfs_mount_free(committed_tree.data);
    vexfs_mount_session_close(admin_session);

    if (vexfs_mount_snapshot_create(session, "with-pending-handle", 0,
                                    &snapshot_commit, &error) != VEXFS_MOUNT_OK)
        return Fail("pending snapshot boundary", error);
    content = {};
    if (vexfs_mount_read_file(session, "/agent/pending-snapshot.txt", &content, &error) !=
            VEXFS_MOUNT_OK || !Equals(content, "snap"))
        return Fail("pending snapshot published content", error);
    vexfs_mount_free(content.data);
    state = {};
    if (vexfs_mount_handle_close(session, handle_id.c_str(), 0,
                                 "close-pending-snapshot", &state, &error) !=
            VEXFS_MOUNT_OK)
        return Fail("pending snapshot close", error);
    vexfs_mount_free(state.data);

    // The mount-only create-and-open path must produce one history commit and
    // one content version, even when macOS metadata or an unrelated commit
    // arrives before content.  In particular, no immutable history row may
    // point at the private version-0 staging state.
    const int64_t commits_before = Scalar(path, "SELECT count(*) FROM _vexfs_commits");
    const int64_t versions_before = Scalar(path, "SELECT count(*) FROM _vexfs_file_versions");
    const int64_t requests_before = Scalar(path, "SELECT count(*) FROM _vexfs_requests");
    handle = {};
    if (vexfs_mount_handle_create(session, "/agent/coalesced.txt", 0640,
                                  "create-coalesced-1", &handle, &error) != VEXFS_MOUNT_OK)
        return Fail("coalesced create handle", error);
    handle_id.assign(static_cast<const char *>(handle.data), static_cast<size_t>(handle.size));
    vexfs_mount_free(handle.data);
    vexfs_mount_bytes coalesced_stat{};
    if (vexfs_mount_stat(session, "/agent/coalesced.txt", &coalesced_stat, &error) !=
            VEXFS_MOUNT_OK || JsonInteger(coalesced_stat, "version") != 0)
        return Fail("coalesced create has no empty version", error);
    const int64_t coalesced_inode = JsonInteger(coalesced_stat, "inode");
    vexfs_mount_free(coalesced_stat.data);
    if (vexfs_mount_mkdir(session, "/agent/unrelated-boundary", &error) != VEXFS_MOUNT_OK)
        return Fail("unrelated history boundary", error);
    if (Scalar(path,
            "SELECT count(*) FROM _vexfs_inode_states WHERE kind='file' "
            "AND current_version=0") != 0 ||
        Scalar(path,
            "SELECT count(*) FROM _vexfs_dirty_inodes dirty "
            "JOIN _vexfs_inodes inode ON inode.id=dirty.inode_id "
            "WHERE inode.id=(SELECT inode_id FROM _vexfs_dentries "
            "WHERE name='coalesced.txt') AND inode.current_version=0") != 1)
        return Fail("unpublished create history isolation", error);
    if (vexfs_mount_xattr_set(session, coalesced_inode, "com.apple.provenance", "p", 1,
                              VEXFS_MOUNT_XATTR_ALWAYS_SET, &error) != VEXFS_MOUNT_OK)
        return Fail("coalesced provenance", error);
    if (vexfs_mount_set_mode(session, coalesced_inode, 0600, &error) != VEXFS_MOUNT_OK)
        return Fail("coalesced mode", error);
    if (vexfs_mount_handle_stage_write(session, handle_id.c_str(), 0, "content", 7,
                                       "write-coalesced-1", &generation, &error) !=
            VEXFS_MOUNT_OK)
        return Fail("coalesced stage", error);
    if (vexfs_mount_handle_publish_close(session, handle_id.c_str(), generation, "data",
                                         &version, &error) !=
            VEXFS_MOUNT_OK || version != 1)
        return Fail("coalesced publish close", error);
    if (Scalar(path, "SELECT count(*) FROM _vexfs_commits") - commits_before != 2 ||
        Scalar(path, "SELECT count(*) FROM _vexfs_file_versions") - versions_before != 1 ||
        Scalar(path, "SELECT count(*) FROM _vexfs_requests") - requests_before != 0)
        return Fail("coalesced write amplification", error);
    content = {};
    if (vexfs_mount_read_file(session, "/agent/coalesced.txt", &content, &error) !=
            VEXFS_MOUNT_OK || !Equals(content, "content"))
        return Fail("coalesced content", error);
    vexfs_mount_free(content.data);
    diagnostics = {};
    if (vexfs_mount_diagnostics(session, &diagnostics, &error) != VEXFS_MOUNT_OK ||
        JsonInteger(diagnostics, "ordinary_mutation_calls") < 5 ||
        JsonInteger(diagnostics, "synchronous_mode_switches") < 1)
        return Fail("performance diagnostics", error);
    vexfs_mount_free(diagnostics.data);

    if (vexfs_mount_write_file(session, "/agent/crash.txt", "old", 3, &version, &error) !=
        VEXFS_MOUNT_OK) return Fail("crash seed write", error);
    vexfs_mount_session *contender = nullptr;
    if (vexfs_mount_session_open(&config, &contender, &error) != VEXFS_MOUNT_BUSY)
        return Fail("exclusive gateway must reject a live contender", error);
    vexfs_mount_session_close(session);

    const pid_t child = fork();
    if (child < 0) return 1;
    if (child == 0) {
        vexfs_mount_session *crashed = nullptr;
        vexfs_mount_error child_error{};
        if (vexfs_mount_session_open(&config, &crashed, &child_error) != VEXFS_MOUNT_OK)
            _exit(2);
        vexfs_mount_bytes child_handle{};
        if (vexfs_mount_handle_open(crashed, "/agent/crash.txt", "rw", "crash-open-1",
                                    &child_handle, &child_error) != VEXFS_MOUNT_OK)
            _exit(3);
        std::string child_id(static_cast<const char *>(child_handle.data),
                             static_cast<size_t>(child_handle.size));
        int64_t child_generation = 0;
        if (vexfs_mount_handle_stage_write(crashed, child_id.c_str(), 0, "NEW", 3,
                                           "crash-write-1", &child_generation,
                                           &child_error) != VEXFS_MOUNT_OK)
            _exit(4);
        _exit(0);
    }
    int child_status = 0;
    if (waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0) return 1;

    session = nullptr;
    if (vexfs_mount_session_open(&config, &session, &error) != VEXFS_MOUNT_OK)
        return Fail("crash recovery open", error);
    content = {};
    if (vexfs_mount_read_file(session, "/agent/crash.txt", &content, &error) != VEXFS_MOUNT_OK ||
        !Equals(content, "NEW")) return Fail("crash content auto recovered", error);
    vexfs_mount_free(content.data);
    published = -1;
    if (vexfs_mount_synchronize(session, "crash-sync-1", &published, &error) != VEXFS_MOUNT_OK ||
        published != 0) {
        std::fprintf(stderr, "crash synchronize published=%lld\n", (long long)published);
        return Fail("crash recovery synchronize idempotent", error);
    }
    diagnostics = {};
    if (vexfs_mount_diagnostics(session, &diagnostics, &error) != VEXFS_MOUNT_OK ||
        !Contains(diagnostics, "\"retained_handles\":0"))
        return Fail("crash staging claimed", error);
    vexfs_mount_free(diagnostics.data);

    vexfs_mount_bytes check{};
    if (vexfs_mount_check(session, 0, &check, &error) != VEXFS_MOUNT_OK ||
        !Contains(check, "\"ok\":true") || !Contains(check, "\"mode\":\"deep\""))
        return Fail("deep integrity check", error);
    vexfs_mount_free(check.data);
    if (!Execute(path,
            "UPDATE _vexfs_chunks SET content=X'424144' WHERE id=("
            "SELECT entry.chunk_id FROM _vexfs_file_versions version "
            "JOIN _vexfs_manifest_chunks entry ON entry.manifest_id=version.manifest_id "
            "JOIN _vexfs_dentries dentry ON dentry.inode_id=version.inode_id "
            "JOIN _vexfs_inodes inode ON inode.id=version.inode_id "
            "WHERE dentry.name='crash.txt' AND version.version_no=inode.current_version "
            "AND entry.chunk_no=0)"))
        return Fail("inject checksum corruption", error);
    check = {};
    if (vexfs_mount_check(session, VEXFS_MOUNT_CHECK_QUICK, &check, &error) !=
            VEXFS_MOUNT_OK || !Contains(check, "\"ok\":true") ||
        !Contains(check, "\"mode\":\"quick\""))
        return Fail("quick integrity check", error);
    vexfs_mount_free(check.data);
    check = {};
    if (vexfs_mount_check(session, 0, &check, &error) != VEXFS_MOUNT_OK ||
        !Contains(check, "\"ok\":false") ||
        !Contains(check, "VEXFS_CHUNK_INVALID"))
        return Fail("checksum corruption report", error);
    vexfs_mount_free(check.data);
    content = {};
    if (vexfs_mount_read_file(session, "/agent/crash.txt", &content, &error) !=
            VEXFS_MOUNT_CORRUPTION)
        return Fail("corrupt content must not be returned", error);
    vexfs_mount_session_close(session);

    unlink(path);
    std::string wal = std::string(path) + "-wal";
    std::string shm = std::string(path) + "-shm";
    unlink(wal.c_str());
    unlink(shm.c_str());
    std::puts("VEXFS RUNTIME SMOKE: PASS");
    return 0;
}
