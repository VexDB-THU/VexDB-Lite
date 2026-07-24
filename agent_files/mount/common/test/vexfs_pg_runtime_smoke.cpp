#include "vexfs_runtime_admin.h"

#include <libpq-fe.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
        !Expect(diagnostics.find("\"principal\":\"postgres\"") != std::string::npos,
                "diagnostics principal") ||
        !Expect(backend_pid > 0, "diagnostics backend pid")) return 1;

    if (!CheckStatus("mkdir", vexfs_mount_mkdir(session, "/project", &error), error)) return 1;
    int64_t version = 0;
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
    const char acl[] =
        "[{\"principal\":\"postgres\",\"effect\":\"allow\","
        "\"permissions\":\"read,write,execute,metadata\",\"inherit\":1}]";
    if (!CheckStatus("acl set", vexfs_mount_acl_set(
            session, inode, acl, sizeof(acl) - 1, &error), error) ||
        !CheckStatus("acl get", vexfs_mount_acl_get(session, inode, &bytes, &error), error) ||
        !Expect(Take(&bytes).find("\"principal\":\"postgres\"") != std::string::npos,
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
                enabled_index.find("\"indexed_files\":1") != std::string::npos,
                "trigram index enabled") ||
        !CheckStatus("indexed grep", vexfs_mount_grep(
            session, "/project", "alpha", 0, 100, &bytes, &error), error)) return 1;
    const std::string indexed_grep = Take(&bytes);
    if (!Expect(indexed_grep.find("\"match_count\":2") != std::string::npos &&
                indexed_grep.find("\"index_used\":true") != std::string::npos,
                "trigram indexed grep")) return 1;

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
    if (!CheckStatus("mutate after snapshot", vexfs_mount_write_file(
            session, "/project/main.txt", "changed\n", 8, &version, &error), error)) return 1;
    int64_t head = 0;
    if (!CheckStatus("workspace head", vexfs_mount_workspace_head(
            session, &head, &error), error)) return 1;
    int64_t restore_commit = 0;
    if (!CheckStatus("snapshot restore", vexfs_mount_snapshot_restore(
            session, "baseline", head, &restore_commit, &error), error) ||
        !CheckStatus("restored read", vexfs_mount_read_file(
            session, "/project/main.txt", &bytes, &error), error) ||
        !Expect(Take(&bytes) == "beta alpha\n", "snapshot restored content")) return 1;

    if (!CheckStatus("deep check", vexfs_mount_check(session, 0, &bytes, &error), error) ||
        !Expect(Take(&bytes).find("\"ok\":true") != std::string::npos,
                "deep check result")) return 1;

    if (!Expect(TerminateRuntimeConnection(control, backend_pid),
                "terminate runtime connection")) return 1;
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
