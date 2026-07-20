#include "vexfs_mount_contract.h"

#include "sqlite3.h"
#include "vexdb_sqlite.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

struct vexfs_mount_session {
    sqlite3 *db = nullptr;
    std::string workspace;
    std::string session_id;
    bool exclusive_gateway = false;
    bool session_started = false;
    bool synchronous_full = true;
    std::recursive_mutex mutex;
};

namespace {

class CallError : public std::runtime_error {
  public:
    CallError(int code, const std::string &message) : std::runtime_error(message), code(code) {}
    int code;
};

class Call {
  public:
    Call(sqlite3 *db, const char *sql) : db_(db) {
        const int rc = sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr);
        if (rc != SQLITE_OK) Throw(rc);
    }
    ~Call() {
        if (statement_ != nullptr) sqlite3_finalize(statement_);
    }
    Call(const Call &) = delete;
    Call &operator=(const Call &) = delete;

    void Text(int index, const char *value) {
        if (value == nullptr) throw CallError(SQLITE_MISUSE, "required text is NULL");
        Check(sqlite3_bind_text(statement_, index, value, -1, SQLITE_TRANSIENT));
    }
    void Int64(int index, int64_t value) {
        Check(sqlite3_bind_int64(statement_, index, value));
    }
    void Int(int index, int value) { Check(sqlite3_bind_int(statement_, index, value)); }
    void Blob(int index, const void *data, uint64_t size) {
        if (size > static_cast<uint64_t>(std::numeric_limits<sqlite3_uint64>::max())) {
            throw CallError(SQLITE_TOOBIG, "buffer is too large");
        }
        if (data == nullptr && size != 0) throw CallError(SQLITE_MISUSE, "buffer is NULL");
        Check(sqlite3_bind_blob64(statement_, index, size == 0 ? "" : data, size, SQLITE_TRANSIENT));
    }
    void Row() {
        const int rc = sqlite3_step(statement_);
        if (rc != SQLITE_ROW) Throw(rc);
    }
    bool MaybeRow() {
        const int rc = sqlite3_step(statement_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        Throw(rc);
        return false;
    }
    int64_t ResultInt64() const { return sqlite3_column_int64(statement_, 0); }
    std::string ResultText() const {
        const unsigned char *value = sqlite3_column_text(statement_, 0);
        return value == nullptr ? std::string() :
            std::string(reinterpret_cast<const char *>(value));
    }
    const void *ResultBlob() const { return sqlite3_column_blob(statement_, 0); }
    int ResultBytes() const { return sqlite3_column_bytes(statement_, 0); }

  private:
    void Check(int rc) {
        if (rc != SQLITE_OK) Throw(rc);
    }
    [[noreturn]] void Throw(int rc) {
        throw CallError(rc, sqlite3_errmsg(db_));
    }

    sqlite3 *db_ = nullptr;
    sqlite3_stmt *statement_ = nullptr;
};

void ClearError(vexfs_mount_error *error) {
    if (error == nullptr) return;
    error->status = VEXFS_MOUNT_OK;
    error->sqlite_code = SQLITE_OK;
    error->message[0] = '\0';
}

vexfs_mount_status MapStatus(int sqlite_code, const std::string &) {
    const int primary = sqlite_code & 0xff;
    if (primary == SQLITE_BUSY || primary == SQLITE_LOCKED) return VEXFS_MOUNT_BUSY;
    if (primary == SQLITE_MISUSE || primary == SQLITE_RANGE || primary == SQLITE_TOOBIG ||
        primary == SQLITE_MISMATCH) return VEXFS_MOUNT_INVALID_ARGUMENT;
    if (primary == SQLITE_NOTFOUND) return VEXFS_MOUNT_NOT_FOUND;
    if (primary == SQLITE_CONSTRAINT) return VEXFS_MOUNT_CONFLICT;
    if (primary == SQLITE_READONLY) return VEXFS_MOUNT_READ_ONLY;
    if (primary == SQLITE_AUTH || primary == SQLITE_PERM) return VEXFS_MOUNT_PERMISSION_DENIED;
    if (primary == SQLITE_FULL) return VEXFS_MOUNT_NO_SPACE;
    if (primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB) return VEXFS_MOUNT_CORRUPTION;
    return VEXFS_MOUNT_DATABASE_ERROR;
}

vexfs_mount_status SetError(vexfs_mount_error *error, int sqlite_code,
                            const std::string &message) {
    const vexfs_mount_status status = MapStatus(sqlite_code, message);
    if (error != nullptr) {
        error->status = status;
        error->sqlite_code = sqlite_code;
        std::snprintf(error->message, sizeof(error->message), "%s", message.c_str());
    }
    return status;
}

template <typename Function>
vexfs_mount_status Guard(vexfs_mount_error *error, Function function) {
    ClearError(error);
    try {
        function();
        return VEXFS_MOUNT_OK;
    } catch (const CallError &exception) {
        return SetError(error, exception.code, exception.what());
    } catch (const std::bad_alloc &) {
        return SetError(error, SQLITE_NOMEM, "out of memory");
    } catch (const std::exception &exception) {
        return SetError(error, SQLITE_ERROR, exception.what());
    } catch (...) {
        return SetError(error, SQLITE_ERROR, "unknown mount contract error");
    }
}

template <typename Function>
vexfs_mount_status Guard(vexfs_mount_session *session, vexfs_mount_error *error,
                         Function function) {
    return Guard(error, [&] {
        if (session == nullptr) throw CallError(SQLITE_MISUSE, "mount session is NULL");
        std::lock_guard<std::recursive_mutex> lock(session->mutex);
        function();
    });
}

void RequireSession(vexfs_mount_session *session) {
    if (session == nullptr || session->db == nullptr) {
        throw CallError(SQLITE_MISUSE, "mount session is NULL");
    }
    if (session->exclusive_gateway && session->session_started) {
        Call heartbeat(session->db, "SELECT vexfs_mount_session_heartbeat(?1,?2)");
        heartbeat.Text(1, session->workspace.c_str());
        heartbeat.Text(2, session->session_id.c_str());
        heartbeat.Row();
    }
}

#if !defined(_WIN32)
void PreparePrivateDatabase(const char *path, bool no_create) {
    struct stat info {};
    if (stat(path, &info) == 0) {
        if (info.st_uid != geteuid()) {
            throw CallError(SQLITE_AUTH, "database is not owned by the current user");
        }
        if (!no_create && chmod(path, 0600) != 0) {
            throw CallError(SQLITE_AUTH, std::string("cannot protect database: ") + std::strerror(errno));
        }
        return;
    }
    if (errno != ENOENT) {
        throw CallError(SQLITE_CANTOPEN, std::string("cannot inspect database: ") + std::strerror(errno));
    }
    if (no_create) throw CallError(SQLITE_CANTOPEN, "database does not exist");
    const int descriptor = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (descriptor < 0) {
        throw CallError(SQLITE_CANTOPEN, std::string("cannot create database: ") + std::strerror(errno));
    }
    close(descriptor);
}

void ProtectSidecar(const std::string &path) {
    struct stat info {};
    if (stat(path.c_str(), &info) == 0 && info.st_uid == geteuid()) chmod(path.c_str(), 0600);
}
#else
void PreparePrivateDatabase(const char *, bool) {}
void ProtectSidecar(const std::string &) {}
#endif

bool LocalProcessIsAlive(const std::string &session_id) {
    const size_t separator = session_id.find('-');
    if (separator == std::string::npos) return true;
    const std::string process_text = session_id.substr(0, separator);
    char *end = nullptr;
    const long long process_id = std::strtoll(process_text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || process_id <= 0) return true;
#if defined(_WIN32)
    return true;
#else
    if (process_id == static_cast<long long>(getpid())) return true;
    if (kill(static_cast<pid_t>(process_id), 0) == 0) return true;
    return errno != ESRCH;
#endif
}

void CheckExistingLocalSession(sqlite3 *db, const char *workspace,
                               const std::string &new_session_id) {
    Call current(db,
        "SELECT s.session_id FROM _vexfs_mount_sessions s "
        "JOIN _vexfs_workspaces w ON w.id=s.workspace_id WHERE w.name=?1");
    current.Text(1, workspace);
    if (!current.MaybeRow()) return;
    const std::string session_id = current.ResultText();
    if (session_id == new_session_id) return;
    if (LocalProcessIsAlive(session_id)) {
        throw CallError(SQLITE_BUSY, "workspace already has an active mount process");
    }
    Call expire(db,
        "UPDATE _vexfs_mount_sessions SET lease_until=0 WHERE session_id=?1 RETURNING 1");
    expire.Text(1, session_id.c_str());
    expire.Row();
}

void CopyResult(Call &call, vexfs_mount_bytes *output) {
    if (output == nullptr) throw CallError(SQLITE_MISUSE, "output buffer is NULL");
    output->data = nullptr;
    output->size = 0;
    const int bytes = call.ResultBytes();
    if (bytes < 0) throw CallError(SQLITE_ERROR, "negative result size");
    if (bytes == 0) return;
    void *copy = std::malloc(static_cast<size_t>(bytes) + 1);
    if (copy == nullptr) throw std::bad_alloc();
    std::memcpy(copy, call.ResultBlob(), static_cast<size_t>(bytes));
    static_cast<unsigned char *>(copy)[bytes] = 0;
    output->data = copy;
    output->size = static_cast<uint64_t>(bytes);
}

void CopyString(const std::string &value, vexfs_mount_bytes *output) {
    if (output == nullptr) throw CallError(SQLITE_MISUSE, "output buffer is NULL");
    output->data = nullptr;
    output->size = 0;
    if (value.empty()) return;
    void *copy = std::malloc(value.size() + 1);
    if (copy == nullptr) throw std::bad_alloc();
    std::memcpy(copy, value.data(), value.size());
    static_cast<char *>(copy)[value.size()] = '\0';
    output->data = copy;
    output->size = static_cast<uint64_t>(value.size());
}

std::string JsonEscape(const std::string &value) {
    std::string output;
    output.reserve(value.size() + 8);
    for (const unsigned char c : value) {
        switch (c) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (c < 0x20) {
                    char escaped[7] = {};
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                    output += escaped;
                } else {
                    output.push_back(static_cast<char>(c));
                }
        }
    }
    return output;
}

void Exec(sqlite3 *db, const char *sql) {
    char *message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        std::string text = message == nullptr ? sqlite3_errmsg(db) : message;
        sqlite3_free(message);
        throw CallError(rc, text);
    }
}

void UseStagingDurability(vexfs_mount_session *session) {
    if (!session->synchronous_full) return;
    Exec(session->db, "PRAGMA synchronous=NORMAL");
    session->synchronous_full = false;
}

void UseFullDurability(vexfs_mount_session *session) {
    if (session->synchronous_full) return;
    // publish/synchronize 前切回 FULL。这个权威事务会同步同一 WAL 中此前已提交的
    // staging frame；成功返回后，最终文件和它依赖的 staging 都达到 FULL 级别。
    Exec(session->db, "PRAGMA synchronous=FULL");
    session->synchronous_full = true;
}

std::string RandomSessionId() {
    unsigned char bytes[16];
    sqlite3_randomness(sizeof(bytes), bytes);
    char output[33];
    for (size_t index = 0; index < sizeof(bytes); ++index) {
        std::snprintf(output + index * 2, 3, "%02x", bytes[index]);
    }
    output[32] = '\0';
    const long long process_id = static_cast<long long>(
#if defined(_WIN32)
        _getpid()
#else
        getpid()
#endif
    );
    return std::to_string(process_id) + "-" + output;
}

}  // namespace

extern "C" vexfs_mount_status vexfs_mount_session_open(const vexfs_mount_config *config,
                                                         vexfs_mount_session **output,
                                                         vexfs_mount_error *error) {
    if (output != nullptr) *output = nullptr;
    return Guard(error, [&] {
        if (config == nullptr || output == nullptr || config->database_path == nullptr ||
            config->workspace == nullptr) {
            throw CallError(SQLITE_MISUSE, "config, output, database_path and workspace are required");
        }
        if (config->abi_version != VEXFS_MOUNT_ABI_VERSION) {
            throw CallError(SQLITE_MISUSE, "unsupported mount ABI version");
        }
        vexfs_mount_session *session = new vexfs_mount_session();
        session->workspace = config->workspace;
        session->session_id = RandomSessionId();
        session->exclusive_gateway = (config->flags & VEXFS_MOUNT_EXCLUSIVE_GATEWAY) != 0;
        const bool no_create = (config->flags & VEXFS_MOUNT_OPEN_NO_CREATE) != 0;
        PreparePrivateDatabase(config->database_path, no_create);
        int rc = sqlite3_open_v2(config->database_path, &session->db,
            (no_create ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) |
                SQLITE_OPEN_FULLMUTEX,
            nullptr);
        if (rc != SQLITE_OK) {
            const std::string message = session->db == nullptr ? "cannot open database" :
                                        sqlite3_errmsg(session->db);
            if (session->db != nullptr) sqlite3_close(session->db);
            delete session;
            throw CallError(rc, message);
        }
        try {
            sqlite3_busy_timeout(session->db,
                static_cast<int>(config->busy_timeout_ms == 0 ? 5000 : config->busy_timeout_ms));
            rc = vexdb_sqlite_register(session->db);
            if (rc != SQLITE_OK) throw CallError(rc, sqlite3_errmsg(session->db));
            if (no_create) {
                // doctor 必须保持只读，不初始化、不迁移，也不在关闭时执行 session_end。
                Exec(session->db, "PRAGMA foreign_keys=ON;");
            } else {
                Exec(session->db,
                    "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;");
                ProtectSidecar(std::string(config->database_path) + "-wal");
                ProtectSidecar(std::string(config->database_path) + "-shm");
                Call init(session->db, "SELECT vexfs_init()");
                init.Row();
                Call workspace(session->db, "SELECT vexfs_workspace_create(?1)");
                workspace.Text(1, config->workspace);
                workspace.Row();
                ProtectSidecar(std::string(config->database_path) + "-wal");
                ProtectSidecar(std::string(config->database_path) + "-shm");
                if (session->exclusive_gateway) {
                    CheckExistingLocalSession(session->db, config->workspace,
                                              session->session_id);
                    Call start(session->db, "SELECT vexfs_mount_session_start(?1,?2)");
                    start.Text(1, config->workspace);
                    start.Text(2, session->session_id.c_str());
                    start.Row();
                    session->session_started = true;
                }
            }
        } catch (...) {
            sqlite3_close(session->db);
            delete session;
            throw;
        }
        *output = session;
    });
}

extern "C" void vexfs_mount_session_close(vexfs_mount_session *session) {
    if (session == nullptr) return;
    if (session->db != nullptr) {
        std::lock_guard<std::recursive_mutex> lock(session->mutex);
        try {
            if (session->session_started) {
                UseFullDurability(session);
                Call end(session->db, "SELECT vexfs_mount_session_end(?1,?2)");
                end.Text(1, session->workspace.c_str());
                end.Text(2, session->session_id.c_str());
                end.Row();
                session->session_started = false;
            }
        } catch (...) {
            // close 不能抛异常；未发布内容仍留在数据库中，后续可诊断和回收。
        }
        sqlite3_close(session->db);
    }
    delete session;
}

extern "C" vexfs_mount_status vexfs_mount_diagnostics(vexfs_mount_session *session,
                                                         vexfs_mount_bytes *json,
                                                         vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        // doctor 以只读方式打开数据库，不能为了诊断偷偷迁移。这里读取数据库自身
        // 的合同版本，并兼容 0.1 staging 列布局。
        Call contract(session->db,
            "SELECT value FROM _vexfs_meta WHERE key='contract_version'");
        contract.Row();
        Call journal(session->db, "PRAGMA journal_mode");
        journal.Row();
        Call synchronous(session->db, "PRAGMA synchronous");
        synchronous.Row();
        Call foreign_keys(session->db, "PRAGMA foreign_keys");
        foreign_keys.Row();
        Call workspace_exists(session->db,
            "SELECT count(*) FROM _vexfs_workspaces WHERE name=?1");
        workspace_exists.Text(1, session->workspace.c_str());
        workspace_exists.Row();
        Call pending(session->db,
            "SELECT count(*) FROM _vexfs_handles WHERE workspace_id="
            "(SELECT id FROM _vexfs_workspaces WHERE name=?1) "
            "AND state IN ('open','retained') AND dirty_generation>published_generation");
        pending.Text(1, session->workspace.c_str());
        pending.Row();
        Call retained(session->db,
            "SELECT count(*) FROM _vexfs_handles WHERE workspace_id="
            "(SELECT id FROM _vexfs_workspaces WHERE name=?1) AND state='retained'");
        retained.Text(1, session->workspace.c_str());
        retained.Row();
        const bool split_staging = sqlite3_table_column_metadata(
            session->db, "main", "_vexfs_staging", "logical_size",
            nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
        Call staging(session->db, split_staging
            ? "SELECT COALESCE(sum(s.logical_size),0) FROM _vexfs_staging s "
              "JOIN _vexfs_handles h ON h.id=s.handle_id WHERE h.workspace_id="
              "(SELECT id FROM _vexfs_workspaces WHERE name=?1)"
            : "SELECT COALESCE(sum(length(s.content)),0) FROM _vexfs_staging s "
              "JOIN _vexfs_handles h ON h.id=s.handle_id WHERE h.workspace_id="
              "(SELECT id FROM _vexfs_workspaces WHERE name=?1)");
        staging.Text(1, session->workspace.c_str());
        staging.Row();
        const char *filename = sqlite3_db_filename(session->db, "main");
        const std::string contract_version = contract.ResultText();
        const bool compatible = contract_version == "0.3.0";
        const bool upgrade_required = contract_version == "0.1.0" || contract_version == "0.2.0";
        const std::string value =
            "{\"contract_version\":\"" + JsonEscape(contract_version) +
            "\",\"compatible\":" + (compatible ? "true" : "false") +
            ",\"upgrade_required\":" + (upgrade_required ? "true" : "false") +
            ",\"database_path\":\"" + JsonEscape(filename == nullptr ? "" : filename) +
            "\",\"workspace\":\"" + JsonEscape(session->workspace) +
            "\",\"workspace_exists\":" + std::to_string(workspace_exists.ResultInt64()) +
            ",\"journal_mode\":\"" + JsonEscape(journal.ResultText()) +
            "\",\"synchronous\":" + std::to_string(synchronous.ResultInt64()) +
            ",\"foreign_keys\":" + std::to_string(foreign_keys.ResultInt64()) +
            ",\"pending_handles\":" + std::to_string(pending.ResultInt64()) +
            ",\"retained_handles\":" + std::to_string(retained.ResultInt64()) +
            ",\"staging_bytes\":" + std::to_string(staging.ResultInt64()) + "}";
        CopyString(value, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_mkdir(vexfs_mount_session *session,
                                                  const char *path,
                                                  vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        Call call(session->db, "SELECT vexfs_mkdir(?1,?2)");
        call.Text(1, session->workspace.c_str()); call.Text(2, path); call.Row();
    });
}

extern "C" vexfs_mount_status vexfs_mount_write_file(vexfs_mount_session *session,
                                                       const char *path, const void *data,
                                                       uint64_t size, int64_t *version,
                                                       vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        if (version == nullptr) throw CallError(SQLITE_MISUSE, "version output is NULL");
        Call call(session->db, "SELECT vexfs_write(?1,?2,?3)");
        call.Text(1, session->workspace.c_str()); call.Text(2, path); call.Blob(3, data, size);
        call.Row(); *version = call.ResultInt64();
    });
}

extern "C" vexfs_mount_status vexfs_mount_read_file(vexfs_mount_session *session,
                                                      const char *path,
                                                      vexfs_mount_bytes *content,
                                                      vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        Call call(session->db, "SELECT vexfs_read(?1,?2)");
        call.Text(1, session->workspace.c_str()); call.Text(2, path); call.Row();
        CopyResult(call, content);
    });
}

extern "C" vexfs_mount_status vexfs_mount_history(vexfs_mount_session *session,
                                                     const char *path,
                                                     vexfs_mount_bytes *json,
                                                     vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        Call call(session->db, "SELECT vexfs_history(?1,?2)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, path);
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_history_page(
    vexfs_mount_session *session, const char *path, uint32_t limit,
    int64_t before_version, vexfs_mount_bytes *json, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (limit == 0 || limit > 1000 || before_version < 0) {
            throw CallError(SQLITE_RANGE, "history limit must be 1..1000 and before must be non-negative");
        }
        Call call(session->db, "SELECT vexfs_history(?1,?2,?3,?4)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, path);
        call.Int64(3, static_cast<int64_t>(limit));
        call.Int64(4, before_version);
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_read_version(vexfs_mount_session *session,
                                                          const char *path, int64_t version,
                                                          vexfs_mount_bytes *content,
                                                          vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (version <= 0) throw CallError(SQLITE_MISUSE, "version must be a positive integer");
        Call call(session->db, "SELECT vexfs_read_version(?1,?2,?3)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, path);
        call.Int64(3, version);
        call.Row();
        CopyResult(call, content);
    });
}

extern "C" vexfs_mount_status vexfs_mount_compare_versions(
    vexfs_mount_session *session, const char *path, int64_t from_version,
    int64_t to_version, vexfs_mount_bytes *json, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (from_version <= 0 || to_version <= 0) {
            throw CallError(SQLITE_RANGE, "versions must be positive integers");
        }
        Call call(session->db, "SELECT vexfs_compare_versions(?1,?2,?3,?4)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, path);
        call.Int64(3, from_version);
        call.Int64(4, to_version);
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_restore_version(
    vexfs_mount_session *session, const char *path, int64_t target_version,
    int64_t expected_version, int64_t *new_version, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        if (target_version <= 0 || expected_version <= 0 || new_version == nullptr) {
            throw CallError(SQLITE_MISUSE,
                            "target version, expected version and new version output are required");
        }
        Call call(session->db, "SELECT vexfs_restore_version(?1,?2,?3,?4)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, path);
        call.Int64(3, target_version);
        call.Int64(4, expected_version);
        call.Row();
        *new_version = call.ResultInt64();
    });
}

extern "C" vexfs_mount_status vexfs_mount_stat(vexfs_mount_session *session,
                                                 const char *path, vexfs_mount_bytes *json,
                                                 vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        Call call(session->db, "SELECT vexfs_stat(?1,?2)");
        call.Text(1, session->workspace.c_str()); call.Text(2, path); call.Row(); CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_path_for_inode(vexfs_mount_session *session,
                                                            int64_t inode,
                                                            vexfs_mount_bytes *path,
                                                            vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        Call call(session->db, "SELECT vexfs_path(?1,?2)");
        call.Text(1, session->workspace.c_str());
        call.Int64(2, inode);
        call.Row();
        CopyResult(call, path);
    });
}

extern "C" vexfs_mount_status vexfs_mount_list(vexfs_mount_session *session,
                                                 const char *path, vexfs_mount_bytes *json,
                                                 vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        Call call(session->db, "SELECT vexfs_list(?1,?2)");
        call.Text(1, session->workspace.c_str()); call.Text(2, path); call.Row(); CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_move(vexfs_mount_session *session,
                                                 const char *source, const char *destination,
                                                 vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        Call call(session->db, "SELECT vexfs_move(?1,?2,?3)");
        call.Text(1, session->workspace.c_str()); call.Text(2, source); call.Text(3, destination); call.Row();
    });
}

extern "C" vexfs_mount_status vexfs_mount_rename(vexfs_mount_session *session,
                                                    const char *source,
                                                    const char *destination, int replace,
                                                    vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        Call call(session->db, "SELECT vexfs_rename(?1,?2,?3,?4)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, source);
        call.Text(3, destination);
        call.Int(4, replace);
        call.Row();
    });
}

extern "C" vexfs_mount_status vexfs_mount_remove(vexfs_mount_session *session,
                                                   const char *path, int recursive,
                                                   vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        Call call(session->db, "SELECT vexfs_remove(?1,?2,?3)");
        call.Text(1, session->workspace.c_str()); call.Text(2, path); call.Int(3, recursive); call.Row();
    });
}

extern "C" vexfs_mount_status vexfs_mount_handle_open(vexfs_mount_session *session,
                                                        const char *path, const char *flags,
                                                        const char *request_id,
                                                        vexfs_mount_bytes *handle,
                                                        vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        Call call(session->db, "SELECT vexfs_handle_open(?1,?2,?3,?4,?5)");
        call.Text(1, session->workspace.c_str()); call.Text(2, path); call.Text(3, flags);
        call.Text(4, request_id); call.Text(5, session->session_id.c_str());
        call.Row(); CopyResult(call, handle);
    });
}

extern "C" vexfs_mount_status vexfs_mount_handle_truncate(
    vexfs_mount_session *session, const char *handle, uint64_t size,
    const char *request_id, int64_t *generation, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
            generation == nullptr) {
            throw CallError(SQLITE_MISUSE, "size is too large or generation output is NULL");
        }
        UseStagingDurability(session);
        Call call(session->db, "SELECT vexfs_handle_truncate(?1,?2,?3)");
        call.Text(1, handle);
        call.Int64(2, static_cast<int64_t>(size));
        call.Text(3, request_id);
        call.Row();
        *generation = call.ResultInt64();
    });
}

extern "C" vexfs_mount_status vexfs_mount_handle_stage_write(
    vexfs_mount_session *session, const char *handle, uint64_t offset, const void *data,
    uint64_t size, const char *request_id, int64_t *generation, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) || generation == nullptr) {
            throw CallError(SQLITE_MISUSE, "offset is too large or generation output is NULL");
        }
        UseStagingDurability(session);
        Call call(session->db, "SELECT vexfs_handle_stage_write(?1,?2,?3,?4)");
        call.Text(1, handle);
        call.Int64(2, static_cast<int64_t>(offset));
        call.Blob(3, data, size);
        call.Text(4, request_id);
        call.Row();
        *generation = call.ResultInt64();
    });
}

extern "C" vexfs_mount_status vexfs_mount_handle_read(vexfs_mount_session *session,
                                                        const char *handle, uint64_t offset,
                                                        uint64_t length,
                                                        vexfs_mount_bytes *content,
                                                        vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
            length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            throw CallError(SQLITE_MISUSE, "read range is too large");
        }
        Call call(session->db, "SELECT vexfs_handle_read(?1,?2,?3)");
        call.Text(1, handle); call.Int64(2, static_cast<int64_t>(offset));
        call.Int64(3, static_cast<int64_t>(length)); call.Row(); CopyResult(call, content);
    });
}

extern "C" vexfs_mount_status vexfs_mount_handle_publish(
    vexfs_mount_session *session, const char *handle, int64_t generation,
    const char *durability, const char *request_id, int64_t *version,
    vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        if (version == nullptr) throw CallError(SQLITE_MISUSE, "version output is NULL");
        Call call(session->db, "SELECT vexfs_handle_publish(?1,?2,?3,?4)");
        call.Text(1, handle); call.Int64(2, generation); call.Text(3, durability);
        call.Text(4, request_id); call.Row(); *version = call.ResultInt64();
    });
}

extern "C" vexfs_mount_status vexfs_mount_handle_close(
    vexfs_mount_session *session, const char *handle, int retain_unpublished,
    const char *request_id, vexfs_mount_bytes *state, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        Call call(session->db, "SELECT vexfs_handle_close(?1,?2,?3)");
        call.Text(1, handle); call.Int(2, retain_unpublished); call.Text(3, request_id);
        call.Row(); CopyResult(call, state);
    });
}

extern "C" vexfs_mount_status vexfs_mount_synchronize(vexfs_mount_session *session,
                                                        const char *request_id,
                                                        int64_t *published,
                                                        vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        if (published == nullptr) throw CallError(SQLITE_MISUSE, "published output is NULL");
        Call call(session->db, "SELECT vexfs_mount_synchronize(?1,?2,?3)");
        call.Text(1, session->workspace.c_str()); call.Text(2, request_id);
        call.Text(3, session->session_id.c_str()); call.Row();
        *published = call.ResultInt64();
    });
}

extern "C" vexfs_mount_status vexfs_mount_reclaim(vexfs_mount_session *session,
                                                    const char *request_id,
                                                    int64_t *reclaimed,
                                                    vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        if (reclaimed == nullptr) throw CallError(SQLITE_MISUSE, "reclaimed output is NULL");
        Call call(session->db, "SELECT vexfs_item_reclaim(?1,?2)");
        call.Text(1, session->workspace.c_str()); call.Text(2, request_id); call.Row();
        *reclaimed = call.ResultInt64();
    });
}

extern "C" void vexfs_mount_free(void *memory) {
    std::free(memory);
}
