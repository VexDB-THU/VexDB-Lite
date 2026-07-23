#include "vexfs_runtime_admin.h"

#include "sqlite3.h"
#include "agent_files/vexfs_sqlite.h"
#include "vexdb_sqlite.h"

#include <algorithm>
#include <chrono>
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
    std::string principal;
    std::string session_id;
    bool exclusive_gateway = false;
    bool session_started = false;
    bool no_create = false;
    bool synchronous_full = true;
    std::chrono::steady_clock::time_point next_heartbeat{};
    uint64_t contract_calls = 0;
    uint64_t ordinary_mutation_calls = 0;
    uint64_t full_boundary_calls = 0;
    uint64_t synchronous_mode_switches = 0;
    uint64_t durability_barriers = 0;
    int64_t observed_data_version = -1;
    int64_t observed_workspace_head = 0;
    uint64_t cache_generation = 1;
    uint64_t mutation_epoch = 0;
    uint64_t observed_mutation_epoch = 0;
    bool visibility_initialized = false;
    std::recursive_mutex mutex;
};

namespace {

constexpr int kUnsupportedBackend = -1001;

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
    void Null(int index) { Check(sqlite3_bind_null(statement_, index)); }
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
    error->native_code = SQLITE_OK;
    std::snprintf(error->backend, sizeof(error->backend), "%s",
                  VEXFS_RUNTIME_BACKEND_SQLITE);
    error->message[0] = '\0';
}

vexfs_mount_status MapStatus(int sqlite_code, const std::string &) {
    if (sqlite_code == kUnsupportedBackend) return VEXFS_MOUNT_UNSUPPORTED;
    if (sqlite_code == VEXFS_SQLITE_CONSTRAINT_NOT_EMPTY) return VEXFS_MOUNT_NOT_EMPTY;
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
        error->native_code = sqlite_code;
        std::snprintf(error->backend, sizeof(error->backend), "%s",
                      VEXFS_RUNTIME_BACKEND_SQLITE);
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
        return SetError(error, SQLITE_ERROR, "unknown VexFS runtime error");
    }
}

template <typename Function>
vexfs_mount_status Guard(vexfs_mount_session *session, vexfs_mount_error *error,
                         Function function) {
    return Guard(error, [&] {
        if (session == nullptr) throw CallError(SQLITE_MISUSE, "mount session is NULL");
        std::lock_guard<std::recursive_mutex> lock(session->mutex);
        ++session->contract_calls;
        function();
    });
}

void RequireSession(vexfs_mount_session *session) {
    if (session == nullptr || session->db == nullptr) {
        throw CallError(SQLITE_MISUSE, "mount session is NULL");
    }
    if (session->exclusive_gateway && session->session_started) {
        const auto now = std::chrono::steady_clock::now();
        if (now < session->next_heartbeat) return;
        Call heartbeat(session->db, "SELECT vexfs_mount_session_heartbeat(?1,?2)");
        heartbeat.Text(1, session->workspace.c_str());
        heartbeat.Text(2, session->session_id.c_str());
        heartbeat.Row();
        // The database lease is 30 seconds.  Refreshing every 10 seconds keeps
        // the safety margin without turning every filesystem callback into an
        // extra SQLite write transaction.
        session->next_heartbeat = now + std::chrono::seconds(10);
    }
}

#if !defined(_WIN32)
int OpenPrivateFile(const char *path, bool writable) {
    int flags = (writable ? O_RDWR : O_RDONLY) | O_NONBLOCK;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    return open(path, flags);
}

void ValidatePrivateFileDescriptor(int descriptor, const std::string &path,
                                   bool protect_mode) {
    struct stat info {};
    if (fstat(descriptor, &info) != 0) {
        throw CallError(SQLITE_CANTOPEN,
                        "cannot inspect opened database file: " + path);
    }
    if (!S_ISREG(info.st_mode)) {
        throw CallError(SQLITE_AUTH, "database path must be a regular file");
    }
    if (info.st_uid != geteuid()) {
        throw CallError(SQLITE_AUTH, "database is not owned by the current user");
    }
    if (protect_mode && fchmod(descriptor, 0600) != 0) {
        throw CallError(SQLITE_AUTH,
                        "cannot protect database file: " + path + ": " +
                        std::strerror(errno));
    }
}

void PreparePrivateDatabase(const char *path, bool no_create) {
    int descriptor = OpenPrivateFile(path, !no_create);
    if (descriptor >= 0) {
        try {
            ValidatePrivateFileDescriptor(descriptor, path, !no_create);
        } catch (...) {
            close(descriptor);
            throw;
        }
        close(descriptor);
        return;
    }
    if (errno == ELOOP) {
        throw CallError(SQLITE_AUTH, "database path must not be a symbolic link");
    }
    if (errno != ENOENT) {
        throw CallError(SQLITE_CANTOPEN, std::string("cannot inspect database: ") + std::strerror(errno));
    }
    if (no_create) throw CallError(SQLITE_CANTOPEN, "database does not exist");
    int flags = O_CREAT | O_EXCL | O_RDWR;
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    const int created_descriptor = open(path, flags, 0600);
    if (created_descriptor < 0) {
        throw CallError(SQLITE_CANTOPEN, std::string("cannot create database: ") + std::strerror(errno));
    }
    close(created_descriptor);
}

void ProtectSidecar(const std::string &path) {
    const int descriptor = OpenPrivateFile(path.c_str(), true);
    if (descriptor < 0) {
        if (errno == ENOENT) return;
        throw CallError(SQLITE_AUTH, "SQLite sidecar is not a private regular file: " + path);
    }
    try {
        ValidatePrivateFileDescriptor(descriptor, path, true);
    } catch (...) {
        close(descriptor);
        throw;
    }
    close(descriptor);
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

void UseOrdinaryDurability(vexfs_mount_session *session) {
    ++session->ordinary_mutation_calls;
    ++session->mutation_epoch;
    if (!session->synchronous_full) return;
    Exec(session->db, "PRAGMA synchronous=NORMAL");
    session->synchronous_full = false;
    ++session->synchronous_mode_switches;
}

void UseFullDurability(vexfs_mount_session *session) {
    ++session->full_boundary_calls;
    if (session->synchronous_full) return;
    // fsync/synchronize/snapshot 前切回 FULL。随后必须执行真实写事务；仅修改
    // PRAGMA 不会形成持久化屏障。
    Exec(session->db, "PRAGMA synchronous=FULL");
    session->synchronous_full = true;
    ++session->synchronous_mode_switches;
}

void DurabilityBarrier(vexfs_mount_session *session) {
    Call barrier(session->db,
        "INSERT INTO _vexfs_meta(key,value) VALUES('durability_epoch','1') "
        "ON CONFLICT(key) DO UPDATE SET value=CAST(CAST(value AS INTEGER)+1 AS TEXT) "
        "RETURNING value");
    barrier.Row();
    ++session->durability_barriers;
}

const char *EffectiveRequestId(vexfs_mount_session *session, const char *request_id,
                               bool safe_without_persistent_replay) {
    if (request_id == nullptr) throw CallError(SQLITE_MISUSE, "request_id is NULL");
    // FSKit/libfuse generate a fresh UUID for every callback, so persisting
    // stage/publish/sync UUIDs cannot deduplicate an OS retry.  These operations
    // are generation-idempotent.  Keep open/close/append requests persisted.
    if (session->exclusive_gateway && safe_without_persistent_replay) return "";
    return request_id;
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
        if (config == nullptr || output == nullptr || config->backend == nullptr ||
            config->connection == nullptr || config->workspace == nullptr ||
            config->principal == nullptr) {
            throw CallError(SQLITE_MISUSE,
                "config, output, backend, connection, workspace and principal are required");
        }
        if (config->abi_version != VEXFS_RUNTIME_ABI_VERSION) {
            throw CallError(SQLITE_MISUSE, "unsupported runtime ABI version");
        }
        if (std::strcmp(config->backend, VEXFS_RUNTIME_BACKEND_SQLITE) != 0) {
            throw CallError(kUnsupportedBackend,
                            "this adapter only supports the sqlite backend");
        }
        if (std::strcmp(config->principal, "local") != 0) {
            throw CallError(SQLITE_AUTH,
                "sqlite backend is single-user and only accepts the local principal");
        }
        vexfs_mount_session *session = new vexfs_mount_session();
        session->workspace = config->workspace;
        session->principal = config->principal;
        session->session_id = RandomSessionId();
        session->exclusive_gateway = (config->flags & VEXFS_RUNTIME_EXCLUSIVE_GATEWAY) != 0;
        const bool no_create = (config->flags & VEXFS_RUNTIME_OPEN_NO_CREATE) != 0;
        session->no_create = no_create;
        PreparePrivateDatabase(config->connection, no_create);
        int sqlite_open_flags =
            (no_create ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) |
            SQLITE_OPEN_FULLMUTEX;
        int rc = sqlite3_open_v2(config->connection, &session->db, sqlite_open_flags, nullptr);
        if (rc != SQLITE_OK) {
            const std::string message = session->db == nullptr ? "cannot open database" :
                                        sqlite3_errmsg(session->db);
            if (session->db != nullptr) sqlite3_close(session->db);
            delete session;
            throw CallError(rc, message);
        }
        try {
            rc = sqlite3_extended_result_codes(session->db, 1);
            if (rc != SQLITE_OK) throw CallError(rc, sqlite3_errmsg(session->db));
            sqlite3_busy_timeout(session->db,
                static_cast<int>(config->operation_timeout_ms == 0
                    ? 5000 : config->operation_timeout_ms));
            rc = vexdb_sqlite_register(session->db);
            if (rc != SQLITE_OK) throw CallError(rc, sqlite3_errmsg(session->db));
            if (no_create) {
                // doctor/check 必须保持只读，不初始化、不迁移，也不在关闭时执行 session_end。
                Exec(session->db, "PRAGMA foreign_keys=ON;");
            } else {
                Exec(session->db,
                    "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;");
                ProtectSidecar(std::string(config->connection) + "-wal");
                ProtectSidecar(std::string(config->connection) + "-shm");
                Call init(session->db, "SELECT vexfs_init()");
                init.Row();
                Call workspace(session->db, "SELECT vexfs_workspace_create(?1)");
                workspace.Text(1, config->workspace);
                workspace.Row();
                ProtectSidecar(std::string(config->connection) + "-wal");
                ProtectSidecar(std::string(config->connection) + "-shm");
                if (session->exclusive_gateway) {
                    CheckExistingLocalSession(session->db, config->workspace,
                                              session->session_id);
                    Call start(session->db, "SELECT vexfs_mount_session_start(?1,?2)");
                    start.Text(1, config->workspace);
                    start.Text(2, session->session_id.c_str());
                    start.Row();
                    session->session_started = true;
                    // A previous gateway may have died after staging writes but before
                    // close. SessionStart claims those retained handles for this new
                    // session; publish them before exposing the remounted workspace.
                    Call recover(session->db,
                        "SELECT vexfs_mount_synchronize(?1,?2,?3)");
                    recover.Text(1, config->workspace);
                    recover.Text(2, "");
                    recover.Text(3, session->session_id.c_str());
                    recover.Row();
                    session->next_heartbeat =
                        std::chrono::steady_clock::now() + std::chrono::seconds(10);
                }
                // Ordinary filesystem callbacks use WAL/NORMAL.  Explicit
                // synchronize, fsync, snapshot and safe close switch to FULL.
                Exec(session->db, "PRAGMA synchronous=NORMAL");
                session->synchronous_full = false;
                ++session->synchronous_mode_switches;
            }
        } catch (...) {
            if (session->session_started) {
                try {
                    Call end(session->db, "SELECT vexfs_mount_session_end(?1,?2)");
                    end.Text(1, session->workspace.c_str());
                    end.Text(2, session->session_id.c_str());
                    end.Row();
                } catch (...) {
                }
            }
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
            if (!session->no_create) {
                UseFullDurability(session);
                Call synchronize(session->db, session->session_started
                    ? "SELECT vexfs_mount_synchronize(?1,'',?2)"
                    : "SELECT vexfs_mount_synchronize(?1,'')");
                synchronize.Text(1, session->workspace.c_str());
                if (session->session_started) {
                    synchronize.Text(2, session->session_id.c_str());
                }
                synchronize.Row();
                DurabilityBarrier(session);
                if (session->session_started) {
                    Call end(session->db, "SELECT vexfs_mount_session_end(?1,?2)");
                    end.Text(1, session->workspace.c_str());
                    end.Text(2, session->session_id.c_str());
                    end.Row();
                    session->session_started = false;
                }
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
        // doctor 以只读方式打开数据库，不初始化也不改写 schema。
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
        const bool schema_ready = contract_version == "0.9.0";
        const std::string value =
            "{\"schema_version\":\"" + JsonEscape(contract_version) +
            "\",\"schema_ready\":" + (schema_ready ? "true" : "false") +
            ",\"backend\":\"sqlite\",\"connection\":\"" +
            JsonEscape(filename == nullptr ? "" : filename) +
            "\",\"workspace\":\"" + JsonEscape(session->workspace) +
            "\",\"security_mode\":\"single-user\",\"principal\":\"" +
            JsonEscape(session->principal) +
            "\",\"workspace_exists\":" + std::to_string(workspace_exists.ResultInt64()) +
            ",\"journal_mode\":\"" + JsonEscape(journal.ResultText()) +
            "\",\"synchronous\":" + std::to_string(synchronous.ResultInt64()) +
            ",\"foreign_keys\":" + std::to_string(foreign_keys.ResultInt64()) +
            ",\"pending_handles\":" + std::to_string(pending.ResultInt64()) +
            ",\"retained_handles\":" + std::to_string(retained.ResultInt64()) +
            ",\"staging_bytes\":" + std::to_string(staging.ResultInt64()) +
            ",\"contract_calls\":" + std::to_string(session->contract_calls) +
            ",\"ordinary_mutation_calls\":" +
                std::to_string(session->ordinary_mutation_calls) +
            ",\"full_boundary_calls\":" +
                std::to_string(session->full_boundary_calls) +
            ",\"synchronous_mode_switches\":" +
                std::to_string(session->synchronous_mode_switches) +
            ",\"durability_barriers\":" +
                std::to_string(session->durability_barriers) +
            ",\"sqlite_total_changes\":" +
                std::to_string(sqlite3_total_changes64(session->db)) + "}";
        CopyString(value, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_mkdir(vexfs_mount_session *session,
                                                  const char *path,
                                                  vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseOrdinaryDurability(session);
        Call call(session->db, "SELECT vexfs_mkdir(?1,?2)");
        call.Text(1, session->workspace.c_str()); call.Text(2, path); call.Row();
    });
}

extern "C" vexfs_mount_status vexfs_mount_create(vexfs_mount_session *session,
                                                     const char *path, const char *kind,
                                                     uint32_t mode,
                                                     vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseOrdinaryDurability(session);
        if (path == nullptr || kind == nullptr || mode > 0777) {
            throw CallError(SQLITE_MISUSE, "path, kind and mode 0..0777 are required");
        }
        Call call(session->db, "SELECT vexfs_create(?1,?2,?3,?4)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, path);
        call.Text(3, kind);
        call.Int(4, static_cast<int>(mode));
        call.Row();
    });
}

extern "C" vexfs_mount_status vexfs_mount_set_mode(vexfs_mount_session *session,
                                                       int64_t inode, uint32_t mode,
                                                       vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseOrdinaryDurability(session);
        if (inode <= 0 || mode > 0777) {
            throw CallError(SQLITE_MISUSE, "positive inode and mode 0..0777 are required");
        }
        Call call(session->db, "SELECT vexfs_set_mode(?1,?2,?3)");
        call.Text(1, session->workspace.c_str());
        call.Int64(2, inode);
        call.Int(3, static_cast<int>(mode));
        call.Row();
    });
}

extern "C" vexfs_mount_status vexfs_mount_set_times(vexfs_mount_session *session,
                                                        int64_t inode,
                                                        int64_t accessed_at_ms,
                                                        int64_t modified_at_ms,
                                                        uint32_t mask,
                                                        vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseOrdinaryDurability(session);
        if (inode <= 0 || accessed_at_ms < 0 || modified_at_ms < 0 ||
            mask == 0 || (mask & ~(VEXFS_MOUNT_TIME_ACCESS | VEXFS_MOUNT_TIME_MODIFY)) != 0) {
            throw CallError(SQLITE_MISUSE,
                            "positive inode, non-negative times and a valid mask are required");
        }
        Call call(session->db, "SELECT vexfs_set_times(?1,?2,?3,?4,?5)");
        call.Text(1, session->workspace.c_str());
        call.Int64(2, inode);
        call.Int64(3, accessed_at_ms);
        call.Int64(4, modified_at_ms);
        call.Int(5, static_cast<int>(mask));
        call.Row();
    });
}

extern "C" vexfs_mount_status vexfs_mount_symlink(vexfs_mount_session *session,
                                                      const char *path, const void *target,
                                                      uint64_t target_size,
                                                      vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseOrdinaryDurability(session);
        if (path == nullptr || target == nullptr || target_size == 0 ||
            target_size > VEXFS_MOUNT_SYMLINK_MAX) {
            throw CallError(SQLITE_MISUSE,
                            "path and symlink target of 1..4096 bytes are required");
        }
        Call call(session->db, "SELECT vexfs_symlink(?1,?2,?3)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, path);
        call.Blob(3, target, target_size);
        call.Row();
    });
}

extern "C" vexfs_mount_status vexfs_mount_readlink(vexfs_mount_session *session,
                                                       int64_t inode,
                                                       vexfs_mount_bytes *target,
                                                       vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (inode <= 0 || target == nullptr) {
            throw CallError(SQLITE_MISUSE, "positive inode and target output are required");
        }
        Call call(session->db, "SELECT vexfs_readlink(?1,?2)");
        call.Text(1, session->workspace.c_str());
        call.Int64(2, inode);
        call.Row();
        CopyResult(call, target);
    });
}

extern "C" vexfs_mount_status vexfs_mount_link(vexfs_mount_session *session,
                                                  const char *source,
                                                  const char *destination,
                                                  vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseOrdinaryDurability(session);
        if (source == nullptr || destination == nullptr) {
            throw CallError(SQLITE_MISUSE, "source and destination are required");
        }
        Call call(session->db, "SELECT vexfs_link(?1,?2,?3)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, source);
        call.Text(3, destination);
        call.Row();
    });
}

extern "C" vexfs_mount_status vexfs_mount_chown(vexfs_mount_session *session,
                                                   int64_t inode, int64_t uid, int64_t gid,
                                                   vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseOrdinaryDurability(session);
        if (inode <= 0 || uid < -1 || gid < -1 ||
            uid > static_cast<int64_t>(UINT32_MAX) ||
            gid > static_cast<int64_t>(UINT32_MAX)) {
            throw CallError(SQLITE_MISUSE,
                            "positive inode and uid/gid in -1..4294967295 are required");
        }
        Call call(session->db, "SELECT vexfs_chown(?1,?2,?3,?4)");
        call.Text(1, session->workspace.c_str());
        call.Int64(2, inode);
        call.Int64(3, uid);
        call.Int64(4, gid);
        call.Row();
    });
}

extern "C" vexfs_mount_status vexfs_mount_write_file(vexfs_mount_session *session,
                                                       const char *path, const void *data,
                                                       uint64_t size, int64_t *version,
                                                       vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseOrdinaryDurability(session);
        if (version == nullptr) throw CallError(SQLITE_MISUSE, "version output is NULL");
        Call call(session->db, "SELECT vexfs_write(?1,?2,?3)");
        call.Text(1, session->workspace.c_str()); call.Text(2, path); call.Blob(3, data, size);
        call.Row(); *version = call.ResultInt64();
    });
}

extern "C" vexfs_mount_status vexfs_mount_append_file(vexfs_mount_session *session,
                                                          int64_t inode, const void *data,
                                                          uint64_t size, int64_t *version,
                                                          vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseOrdinaryDurability(session);
        if (inode <= 0 || version == nullptr || (size > 0 && data == nullptr)) {
            throw CallError(SQLITE_MISUSE,
                            "positive inode, content and version output are required");
        }
        Call call(session->db, "SELECT vexfs_append(?1,?2,?3)");
        call.Text(1, session->workspace.c_str());
        call.Int64(2, inode);
        call.Blob(3, data, size);
        call.Row();
        *version = call.ResultInt64();
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

extern "C" vexfs_mount_status vexfs_mount_grep(vexfs_mount_session *session,
                                                 const char *path, const char *pattern,
                                                 uint32_t flags, uint32_t limit,
                                                 vexfs_mount_bytes *json,
                                                 vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (path == nullptr || pattern == nullptr || pattern[0] == '\0' || limit == 0 ||
            limit > 10240 || (flags & ~(VEXFS_MOUNT_GREP_IGNORE_CASE |
                                       VEXFS_MOUNT_GREP_FILES_ONLY)) != 0) {
            throw CallError(SQLITE_RANGE,
                            "grep needs path, a non-empty pattern, valid flags and limit 1..10240");
        }
        Call call(session->db, "SELECT vexfs_grep(?1,?2,?3,?4,?5)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, path);
        call.Text(3, pattern);
        call.Int64(4, static_cast<int64_t>(flags));
        call.Int64(5, static_cast<int64_t>(limit));
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_check(vexfs_mount_session *session,
                                                   uint32_t flags,
                                                   vexfs_mount_bytes *json,
                                                   vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if ((flags & ~VEXFS_MOUNT_CHECK_QUICK) != 0) {
            throw CallError(SQLITE_RANGE, "check flags are invalid");
        }
        Call call(session->db, "SELECT vexfs_check(?1,?2)");
        call.Text(1, session->workspace.c_str());
        call.Int(2, (flags & VEXFS_MOUNT_CHECK_QUICK) == 0 ? 1 : 0);
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_grep_index(vexfs_mount_session *session,
                                                       const char *action,
                                                       vexfs_mount_bytes *json,
                                                       vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (action == nullptr || action[0] == '\0') {
            throw CallError(SQLITE_MISUSE, "index action is required");
        }
        Call call(session->db, "SELECT vexfs_grep_index(?1)");
        call.Text(1, action);
        call.Row();
        CopyResult(call, json);
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

extern "C" vexfs_mount_status vexfs_mount_list_versioned(
    vexfs_mount_session *session, const char *path, vexfs_mount_bytes *json,
    vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (path == nullptr || json == nullptr) {
            throw CallError(SQLITE_MISUSE, "path and output are required");
        }
        Call call(session->db,
            "SELECT json_object('version',"
            "json_extract(vexfs_stat(?1,?2),'$.version'),"
            "'entries',json(vexfs_list(?1,?2)))");
        call.Text(1, session->workspace.c_str());
        call.Text(2, path);
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_move(vexfs_mount_session *session,
                                                 const char *source, const char *destination,
                                                 vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseOrdinaryDurability(session);
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
        UseOrdinaryDurability(session);
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
        UseOrdinaryDurability(session);
        Call call(session->db, "SELECT vexfs_remove(?1,?2,?3)");
        call.Text(1, session->workspace.c_str()); call.Text(2, path); call.Int(3, recursive); call.Row();
    });
}

extern "C" vexfs_mount_status vexfs_mount_xattr_get(vexfs_mount_session *session,
                                                       int64_t inode, const char *name,
                                                       vexfs_mount_bytes *value,
                                                       vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (inode <= 0 || name == nullptr || value == nullptr) {
            throw CallError(SQLITE_MISUSE, "inode, xattr name and output are required");
        }
        Call call(session->db, "SELECT vexfs_xattr_get(?1,?2,?3)");
        call.Text(1, session->workspace.c_str());
        call.Int64(2, inode);
        call.Text(3, name);
        call.Row();
        CopyResult(call, value);
    });
}

extern "C" vexfs_mount_status vexfs_mount_xattr_list(vexfs_mount_session *session,
                                                        int64_t inode,
                                                        vexfs_mount_bytes *json,
                                                        vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (inode <= 0 || json == nullptr) {
            throw CallError(SQLITE_MISUSE, "inode and output are required");
        }
        Call call(session->db, "SELECT vexfs_xattr_list(?1,?2)");
        call.Text(1, session->workspace.c_str());
        call.Int64(2, inode);
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_xattr_set(vexfs_mount_session *session,
                                                       int64_t inode, const char *name,
                                                       const void *value, uint64_t size,
                                                       int policy,
                                                       vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseOrdinaryDurability(session);
        if (inode <= 0 || name == nullptr || policy < 0 || policy > 3 ||
            (policy != 3 && value == nullptr && size != 0)) {
            throw CallError(SQLITE_MISUSE, "invalid inode, xattr name, value or policy");
        }
        Call call(session->db, "SELECT vexfs_xattr_set(?1,?2,?3,?4,?5)");
        call.Text(1, session->workspace.c_str());
        call.Int64(2, inode);
        call.Text(3, name);
        if (policy == 3) call.Null(4); else call.Blob(4, value, size);
        call.Int(5, policy);
        call.Row();
    });
}

extern "C" vexfs_mount_status vexfs_mount_acl_get(vexfs_mount_session *session,
                                                     int64_t inode, vexfs_mount_bytes *json,
                                                     vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (inode <= 0 || json == nullptr) {
            throw CallError(SQLITE_MISUSE, "inode and output are required");
        }
        Call call(session->db, "SELECT vexfs_acl_get(?1,?2)");
        call.Text(1, session->workspace.c_str());
        call.Int64(2, inode);
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_acl_set(vexfs_mount_session *session,
                                                     int64_t inode, const void *json,
                                                     uint64_t size, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseOrdinaryDurability(session);
        if (inode <= 0 || json == nullptr || size == 0) {
            throw CallError(SQLITE_MISUSE, "inode and non-empty ACL JSON are required");
        }
        Call call(session->db, "SELECT vexfs_acl_set(?1,?2,?3)");
        call.Text(1, session->workspace.c_str());
        call.Int64(2, inode);
        call.Blob(3, json, size);
        call.Row();
    });
}

extern "C" vexfs_mount_status vexfs_mount_workspace_head(vexfs_mount_session *session,
                                                               int64_t *head_commit,
                                                               vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (head_commit == nullptr) throw CallError(SQLITE_MISUSE, "head output is NULL");
        Call call(session->db,
            "SELECT COALESCE(head_commit,0) FROM _vexfs_workspaces WHERE name=?1");
        call.Text(1, session->workspace.c_str());
        call.Row();
        *head_commit = call.ResultInt64();
    });
}

extern "C" vexfs_mount_status vexfs_mount_refresh_visibility(
        vexfs_mount_session *session, vexfs_mount_visibility *visibility,
        vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (visibility == nullptr) {
            throw CallError(SQLITE_MISUSE, "visibility output is NULL");
        }
        Call data_version(session->db, "PRAGMA data_version");
        data_version.Row();
        const int64_t current_data_version = data_version.ResultInt64();
        const bool external_database_commit = session->visibility_initialized &&
            current_data_version != session->observed_data_version;
        const bool local_mutation = session->visibility_initialized &&
            session->mutation_epoch != session->observed_mutation_epoch;
        int external_workspace_commit = 0;
        if (!session->visibility_initialized || external_database_commit || local_mutation) {
            Call head(session->db,
                "SELECT COALESCE(head_commit,0) FROM _vexfs_workspaces WHERE name=?1");
            head.Text(1, session->workspace.c_str());
            head.Row();
            const int64_t current_head = head.ResultInt64();
            if (session->visibility_initialized &&
                current_head != session->observed_workspace_head) {
                ++session->cache_generation;
                external_workspace_commit = external_database_commit ? 1 : 0;
            }
            session->observed_workspace_head = current_head;
            session->observed_data_version = current_data_version;
            session->observed_mutation_epoch = session->mutation_epoch;
            session->visibility_initialized = true;
        }
        visibility->workspace_head = session->observed_workspace_head;
        visibility->cache_generation = session->cache_generation;
        visibility->external_commit = external_workspace_commit;
    });
}

extern "C" vexfs_mount_status vexfs_mount_snapshot_create(vexfs_mount_session *session,
                                                               const char *name,
                                                               uint32_t flags,
                                                               int64_t *commit,
                                                               vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        if (name == nullptr || commit == nullptr) {
            throw CallError(SQLITE_MISUSE, "snapshot name and commit output are required");
        }
        if ((flags & ~VEXFS_SNAPSHOT_COMMITTED_ONLY) != 0) {
            throw CallError(SQLITE_MISUSE, "unsupported snapshot flags");
        }
        const bool committed_only = (flags & VEXFS_SNAPSHOT_COMMITTED_ONLY) != 0;
        // Only the owning gateway may publish its live handles. A management
        // session first publishes its own handles, then the database-level
        // snapshot barrier rejects any dirty handle owned by another session.
        // This prevents a successful snapshot from silently missing writes.
        if (!committed_only) {
            Call synchronize(session->db,
                "SELECT vexfs_mount_synchronize(?1,'',?2)");
            synchronize.Text(1, session->workspace.c_str());
            synchronize.Text(2, session->session_id.c_str());
            synchronize.Row();
        }
        Call call(session->db, committed_only
            ? "SELECT vexfs_snapshot_create(?1,?2,NULL,'committed-only')"
            : "SELECT vexfs_snapshot_create(?1,?2)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, name);
        call.Row();
        *commit = call.ResultInt64();
    });
}

extern "C" vexfs_mount_status vexfs_mount_snapshot_list(vexfs_mount_session *session,
                                                              vexfs_mount_bytes *json,
                                                              vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (json == nullptr) throw CallError(SQLITE_MISUSE, "output is NULL");
        Call call(session->db, "SELECT vexfs_snapshot_list(?1)");
        call.Text(1, session->workspace.c_str());
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_snapshot_show(vexfs_mount_session *session,
                                                              const char *name,
                                                              vexfs_mount_bytes *json,
                                                              vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (name == nullptr || json == nullptr) {
            throw CallError(SQLITE_MISUSE, "snapshot name and output are required");
        }
        Call call(session->db, "SELECT vexfs_snapshot_show(?1,?2)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, name);
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_snapshot_diff(vexfs_mount_session *session,
                                                              const char *from, const char *to,
                                                              vexfs_mount_bytes *json,
                                                              vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (from == nullptr || to == nullptr || json == nullptr) {
            throw CallError(SQLITE_MISUSE, "from, to and output are required");
        }
        Call call(session->db, "SELECT vexfs_snapshot_diff(?1,?2,?3)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, from);
        call.Text(3, to);
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_snapshot_drop(vexfs_mount_session *session,
                                                              const char *name,
                                                              vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        if (name == nullptr) throw CallError(SQLITE_MISUSE, "snapshot name is required");
        Call call(session->db, "SELECT vexfs_snapshot_drop(?1,?2)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, name);
        call.Row();
    });
}

extern "C" vexfs_mount_status vexfs_mount_snapshot_restore(vexfs_mount_session *session,
                                                                 const char *name,
                                                                 int64_t expected_head,
                                                                 int64_t *new_commit,
                                                                 vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        if (name == nullptr || expected_head <= 0 || new_commit == nullptr) {
            throw CallError(SQLITE_MISUSE,
                            "snapshot name, positive expected head and commit output are required");
        }
        Call call(session->db, "SELECT vexfs_snapshot_restore(?1,?2,?3)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, name);
        call.Int64(3, expected_head);
        call.Row();
        *new_commit = call.ResultInt64();
    });
}

extern "C" vexfs_mount_status vexfs_mount_quota_get(
        vexfs_mount_session *session, vexfs_mount_bytes *json,
        vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (json == nullptr) throw CallError(SQLITE_MISUSE, "output is NULL");
        Call call(session->db, "SELECT vexfs_quota_get(?1)");
        call.Text(1, session->workspace.c_str());
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_quota_set(
        vexfs_mount_session *session, int64_t max_bytes,
        int64_t max_files, int64_t max_file_bytes,
        vexfs_mount_bytes *json, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        if (max_bytes < -1 || max_files < -1 || max_file_bytes < -1 || json == nullptr) {
            throw CallError(SQLITE_RANGE,
                            "quota values must be -1 (unlimited) or non-negative");
        }
        Call call(session->db, "SELECT vexfs_quota_set(?1,?2,?3,?4)");
        call.Text(1, session->workspace.c_str());
        if (max_bytes < 0) call.Null(2); else call.Int64(2, max_bytes);
        if (max_files < 0) call.Null(3); else call.Int64(3, max_files);
        if (max_file_bytes < 0) call.Null(4); else call.Int64(4, max_file_bytes);
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_retention_get(
        vexfs_mount_session *session, vexfs_mount_bytes *json,
        vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (json == nullptr) throw CallError(SQLITE_MISUSE, "output is NULL");
        Call call(session->db, "SELECT vexfs_retention_get(?1)");
        call.Text(1, session->workspace.c_str());
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_retention_set(
        vexfs_mount_session *session, uint32_t keep_versions,
        uint32_t keep_days, vexfs_mount_bytes *json,
        vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        if (json == nullptr) throw CallError(SQLITE_MISUSE, "output is NULL");
        Call call(session->db, "SELECT vexfs_retention_set(?1,?2,?3)");
        call.Text(1, session->workspace.c_str());
        call.Int64(2, keep_versions);
        call.Int64(3, keep_days);
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_gc(
        vexfs_mount_session *session, uint32_t batch,
        vexfs_mount_bytes *json, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        if (batch == 0 || batch > 10000 || json == nullptr) {
            throw CallError(SQLITE_RANGE, "batch must be between 1 and 10000");
        }
        Call call(session->db, "SELECT vexfs_gc(?1,?2)");
        call.Text(1, session->workspace.c_str());
        call.Int64(2, batch);
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_gc_pause(
        vexfs_mount_session *session, int paused,
        vexfs_mount_bytes *json, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        if ((paused != 0 && paused != 1) || json == nullptr) {
            throw CallError(SQLITE_RANGE, "paused must be 0 or 1");
        }
        Call call(session->db, "SELECT vexfs_gc_pause(?1,?2)");
        call.Text(1, session->workspace.c_str());
        call.Int(2, paused);
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_handle_open(vexfs_mount_session *session,
                                                        const char *path, const char *flags,
                                                        const char *request_id,
                                                        vexfs_mount_bytes *handle,
                                                        vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseOrdinaryDurability(session);
        Call call(session->db, "SELECT vexfs_handle_open(?1,?2,?3,?4,?5)");
        call.Text(1, session->workspace.c_str()); call.Text(2, path); call.Text(3, flags);
        call.Text(4, EffectiveRequestId(session, request_id, true));
        call.Text(5, session->session_id.c_str());
        call.Row(); CopyResult(call, handle);
    });
}

extern "C" vexfs_mount_status vexfs_mount_handle_create(
    vexfs_mount_session *session, const char *path, uint32_t mode,
    const char *request_id, vexfs_mount_bytes *handle, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseOrdinaryDurability(session);
        if (path == nullptr || request_id == nullptr || handle == nullptr || mode > 0777) {
            throw CallError(SQLITE_MISUSE,
                            "path, mode 0..0777, request_id and handle output are required");
        }
        Call call(session->db, "SELECT vexfs_handle_create(?1,?2,?3,?4,?5)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, path);
        call.Int(3, static_cast<int>(mode));
        call.Text(4, EffectiveRequestId(session, request_id, true));
        call.Text(5, session->session_id.c_str());
        call.Row();
        CopyResult(call, handle);
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
        UseOrdinaryDurability(session);
        Call call(session->db, "SELECT vexfs_handle_truncate(?1,?2,?3)");
        call.Text(1, handle);
        call.Int64(2, static_cast<int64_t>(size));
        call.Text(3, EffectiveRequestId(session, request_id, true));
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
        UseOrdinaryDurability(session);
        Call call(session->db, "SELECT vexfs_handle_stage_write(?1,?2,?3,?4)");
        call.Text(1, handle);
        call.Int64(2, static_cast<int64_t>(offset));
        call.Blob(3, data, size);
        call.Text(4, EffectiveRequestId(session, request_id, true));
        call.Row();
        *generation = call.ResultInt64();
    });
}

extern "C" vexfs_mount_status vexfs_mount_handle_append(
    vexfs_mount_session *session, const char *handle, const void *data, uint64_t size,
    const char *request_id, int64_t *version, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (handle == nullptr || request_id == nullptr || version == nullptr ||
            (size > 0 && data == nullptr)) {
            throw CallError(SQLITE_MISUSE,
                            "handle, content, request_id and version output are required");
        }
        UseOrdinaryDurability(session);
        Call call(session->db, "SELECT vexfs_handle_append(?1,?2,?3)");
        call.Text(1, handle);
        call.Blob(2, data, size);
        call.Text(3, request_id);
        call.Row();
        *version = call.ResultInt64();
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
        if (durability == nullptr) {
            throw CallError(SQLITE_MISUSE, "durability is NULL");
        }
        if (std::strcmp(durability, "full") == 0) UseFullDurability(session);
        else UseOrdinaryDurability(session);
        if (version == nullptr) throw CallError(SQLITE_MISUSE, "version output is NULL");
        Call call(session->db, "SELECT vexfs_handle_publish(?1,?2,?3,?4)");
        call.Text(1, handle); call.Int64(2, generation); call.Text(3, durability);
        call.Text(4, EffectiveRequestId(session, request_id, true));
        call.Row(); *version = call.ResultInt64();
    });
}

extern "C" vexfs_mount_status vexfs_mount_handle_publish_close(
    vexfs_mount_session *session, const char *handle, int64_t generation,
    const char *durability, int64_t *version, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (handle == nullptr || durability == nullptr || version == nullptr) {
            throw CallError(SQLITE_MISUSE,
                            "handle, durability and version output are required");
        }
        if (std::strcmp(durability, "full") == 0) UseFullDurability(session);
        else UseOrdinaryDurability(session);
        Call call(session->db, "SELECT vexfs_handle_publish_close(?1,?2,?3)");
        call.Text(1, handle);
        call.Int64(2, generation);
        call.Text(3, durability);
        call.Row();
        *version = call.ResultInt64();
    });
}

extern "C" vexfs_mount_status vexfs_mount_handle_close(
    vexfs_mount_session *session, const char *handle, int retain_unpublished,
    const char *request_id, vexfs_mount_bytes *state, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseOrdinaryDurability(session);
        Call call(session->db, "SELECT vexfs_handle_close(?1,?2,?3)");
        call.Text(1, handle); call.Int(2, retain_unpublished);
        call.Text(3, EffectiveRequestId(session, request_id, true));
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
        call.Text(1, session->workspace.c_str());
        call.Text(2, EffectiveRequestId(session, request_id, true));
        call.Text(3, session->session_id.c_str()); call.Row();
        *published = call.ResultInt64();
        if (*published > 0) ++session->mutation_epoch;
        DurabilityBarrier(session);
    });
}

extern "C" vexfs_mount_status vexfs_mount_reclaim(vexfs_mount_session *session,
                                                    const char *request_id,
                                                    int64_t *reclaimed,
                                                    vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseOrdinaryDurability(session);
        if (reclaimed == nullptr) throw CallError(SQLITE_MISUSE, "reclaimed output is NULL");
        Call call(session->db, "SELECT vexfs_item_reclaim(?1,?2)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, EffectiveRequestId(session, request_id, true));
        call.Row();
        *reclaimed = call.ResultInt64();
    });
}

extern "C" void vexfs_mount_free(void *memory) {
    std::free(memory);
}
