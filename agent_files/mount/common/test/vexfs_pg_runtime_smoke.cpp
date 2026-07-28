#include "vexfs_runtime_admin.h"

#include <libpq-fe.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>

namespace {

int checks = 0;

int Fail(const char *operation, const vexfs_mount_error *error = nullptr) {
    if (error == nullptr) {
        std::fprintf(stderr, "VEXFS PG RUNTIME FAIL: %s\n", operation);
    } else {
        std::fprintf(stderr,
            "VEXFS PG RUNTIME FAIL: %s: status=%d backend=%s native=%d %s\n",
            operation, error->status, error->backend, error->native_code, error->message);
    }
    return 1;
}

bool CheckStatus(const char *operation, vexfs_mount_status status,
                 vexfs_mount_error &error) {
    ++checks;
    if (status == VEXFS_MOUNT_OK) return true;
    Fail(operation, &error);
    return false;
}

std::string Take(vexfs_mount_bytes *bytes) {
    std::string value;
    if (bytes->data != nullptr) {
        value.assign(static_cast<const char *>(bytes->data),
                     static_cast<size_t>(bytes->size));
        vexfs_mount_free(bytes->data);
    }
    bytes->data = nullptr;
    bytes->size = 0;
    return value;
}

bool Expect(bool condition, const char *message) {
    ++checks;
    if (condition) return true;
    Fail(message);
    return false;
}

int64_t JsonInteger(const std::string &json, const char *key) {
    const std::string marker = std::string("\"") + key + "\":";
    const size_t start = json.find(marker);
    if (start == std::string::npos) return -1;
    return std::strtoll(json.c_str() + start + marker.size(), nullptr, 10);
}

bool PgCommand(PGconn *connection, const char *sql) {
    PGresult *result = PQexec(connection, sql);
    const bool ok = result != nullptr &&
        (PQresultStatus(result) == PGRES_COMMAND_OK ||
         PQresultStatus(result) == PGRES_TUPLES_OK);
    if (!ok) std::fprintf(stderr, "PostgreSQL control command failed: %s\n",
                          result == nullptr ? PQerrorMessage(connection) :
                                              PQresultErrorMessage(result));
    if (result != nullptr) PQclear(result);
    return ok;
}

std::string PgScalar(PGconn *connection, const char *sql) {
    PGresult *result = PQexec(connection, sql);
    const bool ok = result != nullptr && PQresultStatus(result) == PGRES_TUPLES_OK &&
        PQntuples(result) == 1 && PQnfields(result) == 1 && !PQgetisnull(result, 0, 0);
    std::string value;
    if (ok) {
        value.assign(PQgetvalue(result, 0, 0),
                     static_cast<size_t>(PQgetlength(result, 0, 0)));
    } else {
        std::fprintf(stderr, "PostgreSQL scalar query failed: %s\n",
                     result == nullptr ? PQerrorMessage(connection) :
                                         PQresultErrorMessage(result));
    }
    if (result != nullptr) PQclear(result);
    return value;
}

bool WaitForScalarValue(PGconn *control, const char *sql, const char *expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (PgScalar(control, sql) == expected) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool ReconnectControl(PGconn *connection) {
    bool probe_ok = false;
    if (PQstatus(connection) == CONNECTION_OK) {
        PGresult *probe = PQexec(connection, "SELECT 1");
        probe_ok = probe != nullptr && PQresultStatus(probe) == PGRES_TUPLES_OK;
        if (probe != nullptr) PQclear(probe);
    }
    if (probe_ok) return true;

    PQreset(connection);
    if (PQstatus(connection) != CONNECTION_OK) return false;
    PGresult *probe = PQexec(connection, "SELECT 1");
    probe_ok = probe != nullptr && PQresultStatus(probe) == PGRES_TUPLES_OK;
    if (probe != nullptr) PQclear(probe);
    return probe_ok;
}

bool DropWorkspace(PGconn *connection, const std::string &workspace) {
    const char *values[] = {workspace.c_str()};
    PGresult *result = PQexecParams(
        connection, "SELECT vexfs_workspace_drop($1,true)", 1,
        nullptr, values, nullptr, nullptr, 0);
    const bool ok = result != nullptr && PQresultStatus(result) == PGRES_TUPLES_OK;
    if (result != nullptr) PQclear(result);
    return ok;
}

bool TerminateRuntimeConnection(PGconn *connection, int64_t backend_pid) {
    const std::string pid = std::to_string(backend_pid);
    const char *values[] = {pid.c_str()};
    PGresult *result = PQexecParams(
        connection,
        "SELECT pg_terminate_backend($1::integer) WHERE $1::integer<>pg_backend_pid()",
        1, nullptr, values, nullptr, nullptr, 0);
    bool ok = result != nullptr && PQresultStatus(result) == PGRES_TUPLES_OK &&
        PQntuples(result) == 1 && std::strcmp(PQgetvalue(result, 0, 0), "t") == 0;
    if (!ok) std::fprintf(stderr, "could not terminate runtime connection: %s\n",
                          result == nullptr ? PQerrorMessage(connection) :
                                              PQresultErrorMessage(result));
    if (result != nullptr) PQclear(result);
    return ok;
}

}  // namespace

int main(int argc, char **argv) {
    const char *dsn = argc == 2 ? argv[1] : std::getenv("VEXDB_PG_DSN");
    if (dsn == nullptr || *dsn == '\0') {
        std::fprintf(stderr, "usage: vexfs_pg_runtime_smoke DSN\n");
        return 2;
    }
    const std::string workspace = "pg-runtime-contract";
    PGconn *control = PQconnectdb(dsn);
    if (control == nullptr || PQstatus(control) != CONNECTION_OK) {
        std::fprintf(stderr, "control connection failed: %s\n",
                     control == nullptr ? "allocation failed" : PQerrorMessage(control));
        if (control != nullptr) PQfinish(control);
        return 1;
    }
    const std::string database_principal = PQuser(control);
    const std::string principal_json =
        "\"principal\":\"" + database_principal + "\"";
    DropWorkspace(control, workspace);

    vexfs_mount_config config{};
    config.abi_version = VEXFS_RUNTIME_ABI_VERSION;
    config.backend = VEXFS_RUNTIME_BACKEND_POSTGRESQL;
    config.connection = dsn;
    config.workspace = workspace.c_str();
    config.principal = nullptr;
    config.operation_timeout_ms = 5000;
    config.flags = 0;
    vexfs_mount_error error{};
    vexfs_mount_session *session = nullptr;
    if (!CheckStatus("open", vexfs_mount_session_open(&config, &session, &error), error))
        return 1;

    vexfs_mount_bytes bytes{};
    if (!CheckStatus("diagnostics", vexfs_mount_diagnostics(session, &bytes, &error), error))
        return 1;
    const std::string diagnostics = Take(&bytes);
    const int64_t backend_pid = JsonInteger(diagnostics, "backend_pid");
    if (!Expect(diagnostics.find("\"backend\":\"postgresql\"") != std::string::npos,
                "diagnostics backend") ||
        !Expect(diagnostics.find("\"schema_version\":\"0.9.0\"") != std::string::npos,
                "diagnostics contract version") ||
        !Expect(diagnostics.find("\"adapter_version\":\"0.4.0-alpha.1\"") !=
                    std::string::npos,
                "diagnostics adapter version") ||
        !Expect(diagnostics.find("\"security_mode\":\"database-role\"") != std::string::npos,
                "diagnostics security") ||
        !Expect(diagnostics.find(principal_json) != std::string::npos,
                "diagnostics principal") ||
        !Expect(backend_pid > 0, "diagnostics backend pid")) return 1;

    if (!CheckStatus("mkdir", vexfs_mount_mkdir(session, "/project", &error), error)) return 1;
    int64_t version = 0;
    vexfs_mount_bytes owned_handle_bytes{};
    vexfs_mount_bytes owned_stat_bytes{};
    if (!CheckStatus("owned handle create with stat", vexfs_mount_handle_create_owned_stat_durable(
            session, "/project/owned.txt", 04755, 501, 20,
            "create-owned", "full", &owned_handle_bytes, &owned_stat_bytes, &error), error))
        return 1;
    const std::string owned_handle = Take(&owned_handle_bytes);
    const std::string owned_stat = Take(&owned_stat_bytes);
    if (!Expect(JsonInteger(owned_stat, "version") == 0,
                "owned create visible before first publish") ||
        !Expect(JsonInteger(owned_stat, "mode") == 04755,
                "owned create special mode") ||
        !Expect(JsonInteger(owned_stat, "uid") == 501, "owned create uid") ||
        !Expect(JsonInteger(owned_stat, "gid") == 20, "owned create gid")) return 1;
    int64_t owned_generation = 0;
    if (!CheckStatus("owned create stage", vexfs_mount_handle_stage_write(
            session, owned_handle.c_str(), 0, "owned", 5, "stage-owned",
            &owned_generation, &error), error) ||
        !Expect(owned_generation == 2, "owned create generation") ||
        !CheckStatus("owned create publish close", vexfs_mount_handle_publish_close(
            session, owned_handle.c_str(), owned_generation, "full", &version, &error),
            error) ||
        !Expect(version == 1, "owned create first version") ||
        !CheckStatus("owned create read", vexfs_mount_read_file(
            session, "/project/owned.txt", &bytes, &error), error) ||
        !Expect(Take(&bytes) == "owned", "owned create content")) return 1;
    vexfs_mount_bytes batch_a_bytes{};
    vexfs_mount_bytes batch_b_bytes{};
    if (!CheckStatus("batch create a", vexfs_mount_handle_create_owned_durable(
            session, "/project/batch-a.txt", 0644, 501, 20,
            "create-batch-a", "none", &batch_a_bytes, &error), error) ||
        !CheckStatus("batch create b", vexfs_mount_handle_create_owned_durable(
            session, "/project/batch-b.txt", 0644, 501, 20,
            "create-batch-b", "none", &batch_b_bytes, &error), error)) return 1;
    const std::string batch_a = Take(&batch_a_bytes);
    const std::string batch_b = Take(&batch_b_bytes);
    int64_t batch_a_generation = 0;
    int64_t batch_b_generation = 0;
    if (!CheckStatus("batch stage a", vexfs_mount_handle_stage_write_durable(
            session, batch_a.c_str(), 0, "batch-a", 7,
            "stage-batch-a", "none", &batch_a_generation, &error), error) ||
        !CheckStatus("batch stage b", vexfs_mount_handle_stage_write_durable(
            session, batch_b.c_str(), 0, "batch-b", 7,
            "stage-batch-b", "none", &batch_b_generation, &error), error) ||
        !Expect(batch_a_generation == 2 && batch_b_generation == 2,
                "batch staged generations")) return 1;
    int64_t batch_published = 0;
    if (!CheckStatus("bounded batch publish", vexfs_mount_publish_close_batch(
            session, "full", 1, &batch_published, &error), error) ||
        !Expect(batch_published == 1, "bounded batch publish count") ||
        !CheckStatus("batch publish remainder", vexfs_mount_publish_close_all(
            session, "full", &batch_published, &error), error) ||
        !Expect(batch_published == 1, "batch publish remainder count") ||
        !CheckStatus("batch publish replay", vexfs_mount_publish_close_all(
            session, "full", &batch_published, &error), error) ||
        !Expect(batch_published == 0, "batch publish is idempotent") ||
        !CheckStatus("batch read a", vexfs_mount_read_file(
            session, "/project/batch-a.txt", &bytes, &error), error) ||
        !Expect(Take(&bytes) == "batch-a", "batch content a") ||
        !CheckStatus("batch read b", vexfs_mount_read_file(
            session, "/project/batch-b.txt", &bytes, &error), error) ||
        !Expect(Take(&bytes) == "batch-b", "batch content b")) return 1;

    vexfs_mount_bytes claimed_handle_bytes{};
    if (!CheckStatus("claimed create", vexfs_mount_handle_create_owned_durable(
            session, "/project/claimed.txt", 0644, 501, 20,
            "create-claimed", "none", &claimed_handle_bytes, &error), error)) return 1;
    const std::string claimed_handle = Take(&claimed_handle_bytes);
    int64_t claimed_generation = 0;
    if (!CheckStatus("claimed stage", vexfs_mount_handle_stage_write_durable(
            session, claimed_handle.c_str(), 0, "claimed", 7,
            "stage-claimed", "none", &claimed_generation, &error), error)) return 1;
    const std::string claims = "[{\"handle\":\"" + claimed_handle +
        "\",\"generation\":" + std::to_string(claimed_generation) + "}]";
    if (!Expect(PgCommand(control,
            "BEGIN; SELECT 1 FROM _vexfs.workspaces "
            "WHERE name='pg-runtime-contract' FOR UPDATE"),
            "lock workspace for publisher concurrency")) return 1;
    std::atomic<bool> publisher_started{false};
    vexfs_mount_status publisher_status = VEXFS_MOUNT_DATABASE_ERROR;
    vexfs_mount_error publisher_error{};
    int64_t publisher_version = 0;
    std::thread publisher([&] {
        publisher_started.store(true);
        publisher_status = vexfs_mount_handle_publish_close_background(
            session, claimed_handle.c_str(), claimed_generation, "full",
            &publisher_version, &publisher_error);
    });
    while (!publisher_started.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto foreground_started = std::chrono::steady_clock::now();
    const vexfs_mount_status foreground_status = vexfs_mount_stat(
        session, "/project/owned.txt", &bytes, &error);
    const auto foreground_elapsed = std::chrono::steady_clock::now() - foreground_started;
    const bool unlocked = PgCommand(control, "ROLLBACK");
    publisher.join();
    if (!CheckStatus("foreground stat during blocked publisher", foreground_status, error) ||
        !Expect(Take(&bytes).find("\"inode\":") != std::string::npos,
                "foreground stat result during publisher") ||
        !Expect(foreground_elapsed < std::chrono::seconds(1),
                "background publisher does not hold foreground runtime mutex") ||
        !Expect(unlocked, "unlock workspace after concurrency probe") ||
        !CheckStatus("single-file background publisher", publisher_status,
                     publisher_error) ||
        !Expect(publisher_version == 1, "background publisher version")) return 1;
    if (!CheckStatus("background publisher replay",
            vexfs_mount_handle_publish_close_background(
                session, claimed_handle.c_str(), claimed_generation, "full",
                &publisher_version, &error), error) ||
        !Expect(publisher_version == 1, "background publisher replay version") ||
        !CheckStatus("claimed publisher replay", vexfs_mount_publish_close_claimed(
            session, "full", claims.c_str(), &bytes, &error), error) ||
        !Expect(Take(&bytes).find(claimed_handle) != std::string::npos,
                "claimed publisher replay result") ||
        !CheckStatus("claimed content", vexfs_mount_read_file(
            session, "/project/claimed.txt", &bytes, &error), error) ||
        !Expect(Take(&bytes) == "claimed", "claimed content value")) return 1;

    // Model the hard network case where PostgreSQL commits but the client never
    // consumes the result. A second control connection observes the committed
    // content before the first connection is discarded. Retrying the exact
    // handle generation must return version 1 without creating a duplicate.
    vexfs_mount_bytes unknown_handle_bytes{};
    if (!CheckStatus("unknown-result create", vexfs_mount_handle_create_owned_durable(
            session, "/project/unknown-result.txt", 0644, 501, 20,
            "create-unknown-result", "none", &unknown_handle_bytes, &error), error)) return 1;
    const std::string unknown_handle = Take(&unknown_handle_bytes);
    int64_t unknown_generation = 0;
    if (!CheckStatus("unknown-result stage", vexfs_mount_handle_stage_write_durable(
            session, unknown_handle.c_str(), 0, "unknown", 7,
            "stage-unknown-result", "none", &unknown_generation, &error), error)) return 1;
    PGconn *unknown = PQconnectdb(dsn);
    if (!Expect(unknown != nullptr && PQstatus(unknown) == CONNECTION_OK,
                "unknown-result connection")) {
        if (unknown != nullptr) PQfinish(unknown);
        return 1;
    }
    const std::string generation_text = std::to_string(unknown_generation);
    const char *unknown_values[] = {unknown_handle.c_str(), generation_text.c_str()};
    if (!Expect(PQsendQueryParams(
            unknown, "SELECT vexfs_handle_publish_close($1,$2::bigint,'full')", 2,
            nullptr, unknown_values, nullptr, nullptr, 0) == 1,
            "send publish without consuming result")) {
        PQfinish(unknown);
        return 1;
    }
    while (PQflush(unknown) == 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!Expect(PQstatus(unknown) == CONNECTION_OK,
                "unknown-result query sent") ||
        !Expect(WaitForScalarValue(control,
            "SELECT convert_from(vexfs_read('pg-runtime-contract',"
            "'/project/unknown-result.txt'),'UTF8')", "unknown"),
            "unknown-result commit visible before response read")) {
        PQfinish(unknown);
        return 1;
    }
    PQfinish(unknown);
    int64_t unknown_version = 0;
    if (!CheckStatus("unknown-result generation retry",
            vexfs_mount_handle_publish_close_background(
                session, unknown_handle.c_str(), unknown_generation, "full",
                &unknown_version, &error), error) ||
        !Expect(unknown_version == 1, "unknown-result retry version") ||
        !Expect(PgScalar(control,
            "SELECT count(*) FROM _vexfs.file_versions AS version "
            "WHERE version.workspace_id=(SELECT workspace_id FROM _vexfs.workspaces "
            "WHERE name='pg-runtime-contract') AND version.inode_id="
            "((vexfs_stat('pg-runtime-contract','/project/unknown-result.txt')->>"
            "'inode')::bigint)") == "1", "unknown-result has one version")) return 1;

    if (!CheckStatus("write v1", vexfs_mount_write_file(
            session, "/project/main.txt", "alpha\n", 6, &version, &error), error) ||
        !Expect(version == 1, "write version 1")) return 1;
    if (!CheckStatus("write v2", vexfs_mount_write_file(
            session, "/project/main.txt", "beta alpha\n", 11, &version, &error), error) ||
        !Expect(version == 2, "write version 2")) return 1;
    if (!CheckStatus("read", vexfs_mount_read_file(
            session, "/project/main.txt", &bytes, &error), error) ||
        !Expect(Take(&bytes) == "beta alpha\n", "read content")) return 1;

    if (!CheckStatus("stat", vexfs_mount_stat(
            session, "/project/main.txt", &bytes, &error), error)) return 1;
    const std::string stat = Take(&bytes);
    const int64_t inode = JsonInteger(stat, "inode");
    if (!Expect(inode > 0 && JsonInteger(stat, "version") == 2, "stat metadata")) return 1;
    if (!CheckStatus("chmod", vexfs_mount_set_mode(session, inode, 0750, &error), error) ||
        !CheckStatus("times", vexfs_mount_set_times(
            session, inode, 1700000000123LL, 1700000100456LL,
            VEXFS_MOUNT_TIME_ACCESS | VEXFS_MOUNT_TIME_MODIFY, &error), error) ||
        !CheckStatus("chown", vexfs_mount_chown(session, inode, 501, 20, &error), error)) return 1;

    if (!CheckStatus("xattr set", vexfs_mount_xattr_set(
            session, inode, "user.runtime", "value", 5,
            VEXFS_MOUNT_XATTR_MUST_CREATE, &error), error) ||
        !CheckStatus("xattr get", vexfs_mount_xattr_get(
            session, inode, "user.runtime", &bytes, &error), error) ||
        !Expect(Take(&bytes) == "value", "xattr value")) return 1;
    const std::string acl =
        "[{\"principal\":\"" + database_principal + "\",\"effect\":\"allow\","
        "\"permissions\":\"read,write,execute,metadata\",\"inherit\":1}]";
    if (!CheckStatus("acl set", vexfs_mount_acl_set(
            session, inode, acl.data(), acl.size(), &error), error) ||
        !CheckStatus("acl get", vexfs_mount_acl_get(session, inode, &bytes, &error), error) ||
        !Expect(Take(&bytes).find(principal_json) != std::string::npos,
                "acl content")) return 1;

    if (!CheckStatus("hardlink", vexfs_mount_link(
            session, "/project/main.txt", "/project/main-link.txt", &error), error) ||
        !CheckStatus("symlink", vexfs_mount_symlink(
            session, "/project/current", "main.txt", 8, &error), error) ||
        !CheckStatus("list", vexfs_mount_list(session, "/project", &bytes, &error), error))
        return 1;
    const std::string listing = Take(&bytes);
    if (!Expect(listing.find("\"name\":\"main.txt\"") != std::string::npos &&
                listing.find("\"name\":\"current\"") != std::string::npos,
                "directory listing")) return 1;
    if (!CheckStatus("find first page", vexfs_mount_find(
            session, "/project", "*.txt", "file", 11, 11, -1,
            1700000200000LL, "", 1, &bytes, &error), error)) return 1;
    const std::string find_first_page = Take(&bytes);
    if (!Expect(find_first_page.find("/project/main-link.txt") != std::string::npos &&
                find_first_page.find("\"next_cursor\":\"/project/main-link.txt\"") !=
                    std::string::npos &&
                find_first_page.find("/project/main.txt") == std::string::npos,
                "find first page content")) return 1;
    if (!CheckStatus("find second page", vexfs_mount_find(
            session, "/project", "*.txt", "file", 11, 11, -1,
            1700000200000LL, "/project/main-link.txt", 1, &bytes, &error), error))
        return 1;
    const std::string find_second_page = Take(&bytes);
    if (!Expect(find_second_page.find("/project/main.txt") != std::string::npos &&
                find_second_page.find("\"next_cursor\":null") != std::string::npos,
                "find second page content")) return 1;

    if (!CheckStatus("grep", vexfs_mount_grep(
            session, "/project", "alpha", 0, 100, &bytes, &error), error)) return 1;
    const std::string grep = Take(&bytes);
    if (!Expect(grep.find("\"match_count\":2") != std::string::npos &&
                grep.find("\"index_used\":false") != std::string::npos,
                "grep hardlink matches")) return 1;
    if (!CheckStatus("grep index", vexfs_mount_grep_index(
            session, "status", &bytes, &error), error)) return 1;
    const std::string index = Take(&bytes);
    if (!Expect(index.find("\"available\":true") != std::string::npos &&
                index.find("\"enabled\":false") != std::string::npos &&
                index.find("\"backend\":\"pg-trgm\"") != std::string::npos,
                "trigram capability status") ||
        !CheckStatus("grep index enable", vexfs_mount_grep_index(
            session, "enable", &bytes, &error), error)) return 1;
    const std::string enabled_index = Take(&bytes);
    if (!Expect(enabled_index.find("\"enabled\":true") != std::string::npos &&
                enabled_index.find("\"dirty\":false") != std::string::npos &&
                enabled_index.find("\"indexed_files\":6") != std::string::npos,
                "trigram index enabled") ||
        !CheckStatus("indexed grep", vexfs_mount_grep(
            session, "/project", "alpha", 0, 100, &bytes, &error), error)) return 1;
    const std::string indexed_grep = Take(&bytes);
    if (!Expect(indexed_grep.find("\"match_count\":2") != std::string::npos &&
                indexed_grep.find("\"index_used\":true") != std::string::npos,
                "trigram indexed grep")) return 1;

    std::string range_source(70032, 'x');
    range_source.replace(65530, 16, "0123456789abcdef");
    if (!CheckStatus("range seed", vexfs_mount_write_file(
            session, "/project/range.bin", range_source.data(), range_source.size(),
            &version, &error), error) ||
        !CheckStatus("range read", vexfs_mount_read_file_range(
            session, "/project/range.bin", 65530, 16, &bytes, &error), error) ||
        !Expect(Take(&bytes) == "0123456789abcdef", "range read across chunks")) return 1;
    if (!CheckStatus("range write", vexfs_mount_write_file_range(
            session, "/project/range.bin", 65534, "RANGE", 5,
            "pg-range-write-1", "full", &version, &error), error)) return 1;
    const int64_t range_version = version;
    if (!CheckStatus("range write replay", vexfs_mount_write_file_range(
            session, "/project/range.bin", 65534, "RANGE", 5,
            "pg-range-write-1", "full", &version, &error), error) ||
        !Expect(version == range_version, "range write replay version") ||
        !CheckStatus("range read after write", vexfs_mount_read_file_range(
            session, "/project/range.bin", 65530, 16, &bytes, &error), error) ||
        !Expect(Take(&bytes) == "0123RANGE9abcdef", "range read after write")) return 1;

    vexfs_mount_bytes handle_bytes{};
    if (!CheckStatus("handle create", vexfs_mount_handle_create(
            session, "/project/staged.txt", 0644, "create-staged",
            &handle_bytes, &error), error)) return 1;
    const std::string handle = Take(&handle_bytes);
    int64_t generation = 0;
    if (!CheckStatus("stage", vexfs_mount_handle_stage_write(
            session, handle.c_str(), 0, "staged", 6, "stage-staged",
            &generation, &error), error) ||
        !Expect(generation > 0, "stage generation")) return 1;
    int64_t replay_generation = 0;
    if (!CheckStatus("stage replay", vexfs_mount_handle_stage_write(
            session, handle.c_str(), 0, "staged", 6, "stage-staged",
            &replay_generation, &error), error) ||
        !Expect(replay_generation == generation, "stage replay generation")) return 1;
    ++checks;
    if (vexfs_mount_handle_stage_write(
            session, handle.c_str(), 0, "other", 5, "stage-staged",
            &replay_generation, &error) != VEXFS_MOUNT_CONFLICT)
        return Fail("stage replay fingerprint", &error);

    vexfs_mount_session *peer = nullptr;
    vexfs_mount_config peer_config = config;
    if (!CheckStatus("peer open", vexfs_mount_session_open(
            &peer_config, &peer, &error), error)) return 1;
    ++checks;
    if (vexfs_mount_read_file(peer, "/project/staged.txt", &bytes, &error) !=
            VEXFS_MOUNT_NOT_FOUND) return Fail("unpublished isolation", &error);
    if (!CheckStatus("handle read", vexfs_mount_handle_read(
            session, handle.c_str(), 0, 64, &bytes, &error), error) ||
        !Expect(Take(&bytes) == "staged", "staged read")) return 1;
    if (!CheckStatus("publish", vexfs_mount_handle_publish(
            session, handle.c_str(), generation, "data", "publish-staged",
            &version, &error), error) ||
        !CheckStatus("close handle", vexfs_mount_handle_close(
            session, handle.c_str(), 0, "close-staged", &bytes, &error), error)) return 1;
    Take(&bytes);
    if (!CheckStatus("peer published read", vexfs_mount_read_file(
            peer, "/project/staged.txt", &bytes, &error), error) ||
        !Expect(Take(&bytes) == "staged", "published visibility")) return 1;

    vexfs_mount_visibility visibility{};
    if (!CheckStatus("visibility initial", vexfs_mount_refresh_visibility(
            session, &visibility, &error), error)) return 1;
    if (!CheckStatus("peer write", vexfs_mount_write_file(
            peer, "/project/peer.txt", "peer", 4, &version, &error), error) ||
        !CheckStatus("visibility external", vexfs_mount_refresh_visibility(
            session, &visibility, &error), error) ||
        !Expect(visibility.external_commit == 1, "external change notification")) return 1;

    int64_t snapshot_commit = 0;
    if (!CheckStatus("snapshot", vexfs_mount_snapshot_create(
            session, "baseline", VEXFS_SNAPSHOT_COMMITTED_ONLY,
            &snapshot_commit, &error), error) ||
        !CheckStatus("snapshot list", vexfs_mount_snapshot_list(
            session, &bytes, &error), error) ||
        !Expect(Take(&bytes).find("\"name\":\"baseline\"") != std::string::npos,
                "snapshot list content")) return 1;
    int64_t unsupported_historical = 0;
    if (vexfs_mount_snapshot_create_at_commit(
            session, "unsupported-history", "manual", snapshot_commit,
            &unsupported_historical, &error) != VEXFS_MOUNT_UNSUPPORTED ||
        std::strstr(error.message, "supported only by SQLite") == nullptr)
        return Fail("PG historical snapshot boundary", &error);
    ++checks;
    vexfs_mount_bytes unsupported_tree{};
    if (vexfs_mount_workspace_show_commit_page(
            session, snapshot_commit, "", 10, &unsupported_tree, &error) !=
            VEXFS_MOUNT_UNSUPPORTED ||
        std::strstr(error.message, "supported only by SQLite") == nullptr)
        return Fail("PG historical tree boundary", &error);
    ++checks;
    int64_t unsupported_time_commit = 0;
    int64_t unsupported_time_created_at = 0;
    if (vexfs_mount_snapshot_create_at_time(
            session, "unsupported-time", "manual", "2099-01-01T00:00:00Z",
            &unsupported_time_commit, &unsupported_time_created_at, &error) !=
            VEXFS_MOUNT_UNSUPPORTED ||
        std::strstr(error.message, "supported only by SQLite") == nullptr)
        return Fail("PG historical time boundary", &error);
    ++checks;
    if (!CheckStatus("mutate after snapshot", vexfs_mount_write_file(
            session, "/project/main.txt", "changed\n", 8, &version, &error), error)) return 1;
    int64_t head = 0;
    if (!CheckStatus("workspace head", vexfs_mount_workspace_head(
            session, &head, &error), error)) return 1;
    int64_t restore_commit = 0;
    if (vexfs_mount_snapshot_restore_safe(
            session, "baseline", head, "must-not-exist-while-peer-mounted",
            &restore_commit, &error) != VEXFS_MOUNT_BUSY ||
        std::strstr(error.message, "active mount session") == nullptr)
        return Fail("peer mount must block snapshot restore", &error);
    ++checks;
    vexfs_mount_session_close(peer);
    peer = nullptr;
    if (!CheckStatus("snapshot restore", vexfs_mount_snapshot_restore_safe(
            session, "baseline", head, "safety-before-baseline-restore",
            &restore_commit, &error), error) ||
        !CheckStatus("restored read", vexfs_mount_read_file(
            session, "/project/main.txt", &bytes, &error), error) ||
        !Expect(Take(&bytes) == "beta alpha\n", "snapshot restored content")) return 1;
    if (!CheckStatus("head after baseline restore", vexfs_mount_workspace_head(
            session, &head, &error), error) ||
        !CheckStatus("restore safety snapshot", vexfs_mount_snapshot_restore_safe(
            session, "safety-before-baseline-restore", head,
            "safety-before-undo", &restore_commit, &error), error) ||
        !CheckStatus("safety restored read", vexfs_mount_read_file(
            session, "/project/main.txt", &bytes, &error), error) ||
        !Expect(Take(&bytes) == "changed\n", "safety snapshot restores pre-restore tree"))
        return 1;
    if (!CheckStatus("head after safety restore", vexfs_mount_workspace_head(
            session, &head, &error), error) ||
        !CheckStatus("restore baseline again", vexfs_mount_snapshot_restore_safe(
            session, "baseline", head, "safety-before-final-baseline",
            &restore_commit, &error), error) ||
        !CheckStatus("final baseline read", vexfs_mount_read_file(
            session, "/project/main.txt", &bytes, &error), error) ||
        !Expect(Take(&bytes) == "beta alpha\n", "final baseline content")) return 1;

    if (!CheckStatus("peer reopen", vexfs_mount_session_open(
            &peer_config, &peer, &error), error)) return 1;

    if (!CheckStatus("deep check", vexfs_mount_check(session, 0, &bytes, &error), error) ||
        !Expect(Take(&bytes).find("\"ok\":true") != std::string::npos,
                "deep check result")) return 1;

    if (!Expect(TerminateRuntimeConnection(control, backend_pid),
                "terminate runtime connection")) return 1;
    if (!CheckStatus("peer write while listener disconnected", vexfs_mount_write_file(
            peer, "/project/missed-notification.txt", "missed", 6,
            &version, &error), error)) return 1;
    vexfs_mount_visibility visibility_after_disconnect{};
    const vexfs_mount_status first_visibility_after_disconnect =
        vexfs_mount_refresh_visibility(session, &visibility_after_disconnect, &error);
    ++checks;
    if (first_visibility_after_disconnect != VEXFS_MOUNT_OK &&
        first_visibility_after_disconnect != VEXFS_MOUNT_DATABASE_ERROR)
        return Fail("visibility disconnect status", &error);
    bool resync_invalidated_cache = first_visibility_after_disconnect == VEXFS_MOUNT_OK &&
        visibility_after_disconnect.external_commit == 1;
    if (!resync_invalidated_cache) {
        if (first_visibility_after_disconnect == VEXFS_MOUNT_DATABASE_ERROR) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        }
        if (!CheckStatus("visibility reconnect next operation",
                vexfs_mount_refresh_visibility(
                    session, &visibility_after_disconnect, &error), error)) return 1;
        resync_invalidated_cache = visibility_after_disconnect.external_commit == 1;
    }
    if (!Expect(resync_invalidated_cache,
                "reconnect invalidates notifications missed while disconnected")) return 1;
    bytes = {};
    const vexfs_mount_status first_after_disconnect =
        vexfs_mount_stat(session, "/project/main.txt", &bytes, &error);
    ++checks;
    if (first_after_disconnect != VEXFS_MOUNT_OK &&
        first_after_disconnect != VEXFS_MOUNT_DATABASE_ERROR)
        return Fail("disconnect status", &error);
    if (first_after_disconnect == VEXFS_MOUNT_OK) Take(&bytes);
    if (!CheckStatus("reconnect next operation", vexfs_mount_stat(
            session, "/project/main.txt", &bytes, &error), error) ||
        !Expect(JsonInteger(Take(&bytes), "inode") == inode,
                "reconnect preserved workspace")) return 1;

    const char *restart_gate = std::getenv("VEXFS_PG_RESTART_GATE_DIR");
    if (restart_gate != nullptr && *restart_gate != '\0') {
        const std::string ready = std::string(restart_gate) + "/ready";
        const std::string resume = std::string(restart_gate) + "/continue";
        {
            std::ofstream marker(ready, std::ios::trunc);
            if (!marker) return Fail("create restart ready marker");
            marker << "ready\n";
        }
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(120);
        while (!std::ifstream(resume).good()) {
            if (std::chrono::steady_clock::now() >= deadline)
                return Fail("wait for PostgreSQL restart");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        bytes = {};
        const vexfs_mount_status first_after_restart =
            vexfs_mount_stat(session, "/project/main.txt", &bytes, &error);
        ++checks;
        if (first_after_restart != VEXFS_MOUNT_OK &&
            first_after_restart != VEXFS_MOUNT_DATABASE_ERROR)
            return Fail("restart first operation status", &error);
        if (first_after_restart == VEXFS_MOUNT_OK) Take(&bytes);
        if (!CheckStatus("restart reconnect next operation", vexfs_mount_stat(
                session, "/project/main.txt", &bytes, &error), error) ||
            !Expect(JsonInteger(Take(&bytes), "inode") == inode,
                    "restart reconnect preserved workspace")) return 1;
        if (!CheckStatus("restart diagnostics", vexfs_mount_diagnostics(
                session, &bytes, &error), error)) return 1;
        const int64_t restarted_backend_pid = JsonInteger(Take(&bytes), "backend_pid");
        if (!Expect(restarted_backend_pid > 0 && restarted_backend_pid != backend_pid,
                    "restart created a new backend connection")) return 1;

        if (!Expect(ReconnectControl(control),
                    "control reconnect after server restart")) return 1;
    }

    vexfs_mount_session_close(peer);
    vexfs_mount_session_close(session);
    if (!Expect(DropWorkspace(control, workspace), "cleanup workspace")) return 1;
    if (!Expect(PgCommand(control, "SELECT 1"), "control connection")) return 1;
    PQfinish(control);
    std::printf("VEXFS PG RUNTIME CONTRACT: PASS (%d checks)\n", checks);
    return 0;
}
