#include "vexfs_runtime_admin.h"

#include "sqlite3.h"
#include "agent_files/vexfs_sqlite.h"
#include "vexdb_sqlite.h"

#if defined(_WIN32) && defined(VEXFS_HAVE_LIBPQ)
#include <winsock2.h>
#endif

#if defined(VEXFS_HAVE_LIBPQ)
#include <libpq-fe.h>
#endif

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
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <cerrno>
#include <fcntl.h>
#if defined(VEXFS_HAVE_LIBPQ)
#include <poll.h>
#endif
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

struct vexfs_mount_session {
    sqlite3 *db = nullptr;
#if defined(VEXFS_HAVE_LIBPQ)
    PGconn *pg = nullptr;
    PGconn *publisher_pg = nullptr;
#endif
    bool postgresql = false;
    std::string connection;
    std::string workspace;
    std::string principal;
    std::string session_id;
    bool exclusive_gateway = false;
    bool session_started = false;
    bool no_create = false;
    // SQLite PRAGMA synchronous: 0=OFF, 1=NORMAL, 2=FULL.
    int synchronous_mode = 2;
    uint32_t operation_timeout_ms = 0;
    std::chrono::steady_clock::time_point next_heartbeat{};
    std::chrono::steady_clock::time_point next_visibility_poll{};
    std::chrono::steady_clock::time_point next_pg_reconnect_attempt{};
    std::chrono::steady_clock::time_point next_publisher_reconnect_attempt{};
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
    bool pg_visibility_resync_required = false;
    std::recursive_mutex mutex;
    std::mutex publisher_mutex;
};

namespace {

constexpr int kUnsupportedBackend = -1001;
constexpr int kPgDatabaseError = -1100;
constexpr int kPgInvalidArgument = -1101;
constexpr int kPgNotFound = -1102;
constexpr int kPgConflict = -1103;
constexpr int kPgReadOnly = -1104;
constexpr int kPgBusy = -1105;
constexpr int kPgPermission = -1106;
constexpr int kPgNoSpace = -1107;
constexpr int kPgCorruption = -1108;
constexpr int kPgNotEmpty = -1109;

#if defined(VEXFS_HAVE_LIBPQ)
void EnsurePostgreSQLConnection(vexfs_mount_session *session);
void EnsurePostgreSQLPublisherConnection(vexfs_mount_session *session);
#endif

class CallError : public std::runtime_error {
  public:
    CallError(int code, const std::string &message,
              const std::string &backend = VEXFS_RUNTIME_BACKEND_SQLITE)
        : std::runtime_error(message), code(code), backend(backend) {}
    int code;
    std::string backend;
};

#if defined(VEXFS_HAVE_LIBPQ)
constexpr uint32_t kPostgreSQLTransportTimeoutMs = 5000;

uint32_t PostgreSQLTransportTimeout(uint32_t operation_timeout_ms) {
    return operation_timeout_ms == 0
        ? kPostgreSQLTransportTimeoutMs
        : std::min(operation_timeout_ms, kPostgreSQLTransportTimeoutMs);
}

enum class PostgreSQLSocketWaitResult {
    kReady,
    kTimeout,
    kFailed,
};

PostgreSQLSocketWaitResult WaitPostgreSQLSocket(
        PGconn *connection, bool readable, bool writable,
        std::chrono::steady_clock::time_point deadline) {
    const int descriptor = connection == nullptr ? -1 : PQsocket(connection);
    if (descriptor < 0) return PostgreSQLSocketWaitResult::kFailed;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return PostgreSQLSocketWaitResult::kTimeout;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        const long timeout_ms = std::max<long>(1, static_cast<long>(remaining.count()));
#if defined(_WIN32)
        fd_set read_set;
        fd_set write_set;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        if (readable) FD_SET(static_cast<SOCKET>(descriptor), &read_set);
        if (writable) FD_SET(static_cast<SOCKET>(descriptor), &write_set);
        timeval timeout{};
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        const int result = select(0, readable ? &read_set : nullptr,
                                  writable ? &write_set : nullptr, nullptr, &timeout);
        if (result > 0) return PostgreSQLSocketWaitResult::kReady;
        if (result == 0) return PostgreSQLSocketWaitResult::kTimeout;
        if (WSAGetLastError() == WSAEINTR) continue;
        return PostgreSQLSocketWaitResult::kFailed;
#else
        pollfd item{};
        item.fd = descriptor;
        item.events = static_cast<short>((readable ? POLLIN : 0) |
                                         (writable ? POLLOUT : 0));
        const int result = poll(&item, 1, static_cast<int>(timeout_ms));
        if (result > 0) return PostgreSQLSocketWaitResult::kReady;
        if (result == 0) return PostgreSQLSocketWaitResult::kTimeout;
        if (errno == EINTR) continue;
        return PostgreSQLSocketWaitResult::kFailed;
#endif
    }
}

void DropPostgreSQLConnection(PGconn **connection) {
    if (connection == nullptr || *connection == nullptr) return;
    PQfinish(*connection);
    *connection = nullptr;
}

[[noreturn]] void FailPostgreSQLTransport(PGconn **connection,
                                          const std::string &message) {
    DropPostgreSQLConnection(connection);
    throw CallError(kPgDatabaseError, message, VEXFS_RUNTIME_BACKEND_POSTGRESQL);
}

PGresult *ExecutePostgreSQLQuery(
        PGconn **connection, uint32_t operation_timeout_ms, const char *sql,
        int parameter_count = 0, const Oid *types = nullptr,
        const char *const *values = nullptr, const int *lengths = nullptr,
        const int *formats = nullptr) {
    if (connection == nullptr || *connection == nullptr ||
        PQstatus(*connection) != CONNECTION_OK) {
        throw CallError(kPgDatabaseError, "PostgreSQL connection is not available",
                        VEXFS_RUNTIME_BACKEND_POSTGRESQL);
    }
    PGconn *raw = *connection;
    if (PQsetnonblocking(raw, 1) != 0) {
        const std::string message = PQerrorMessage(raw);
        FailPostgreSQLTransport(connection,
            "cannot enable nonblocking PostgreSQL I/O: " + message);
    }
    if (PQsendQueryParams(raw, sql, parameter_count, types, values, lengths,
                          formats, 0) == 0) {
        const std::string message = PQerrorMessage(raw);
        FailPostgreSQLTransport(connection,
            "cannot send PostgreSQL query: " + message);
    }

    const auto timeout = std::chrono::milliseconds(
        PostgreSQLTransportTimeout(operation_timeout_ms));
    auto idle_deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const int pending = PQflush(raw);
        if (pending == 0) break;
        if (pending < 0) {
            const std::string message = PQerrorMessage(raw);
            FailPostgreSQLTransport(connection,
                "cannot flush PostgreSQL query: " + message);
        }
        const PostgreSQLSocketWaitResult wait =
            WaitPostgreSQLSocket(raw, false, true, idle_deadline);
        if (wait == PostgreSQLSocketWaitResult::kTimeout) {
            FailPostgreSQLTransport(connection,
                "PostgreSQL query send timed out after " +
                    std::to_string(PostgreSQLTransportTimeout(operation_timeout_ms)) +
                    " ms without network progress");
        }
        if (wait == PostgreSQLSocketWaitResult::kFailed) {
            FailPostgreSQLTransport(connection,
                "PostgreSQL socket failed while sending a query");
        }
        idle_deadline = std::chrono::steady_clock::now() + timeout;
    }

    PGresult *result = nullptr;
    for (;;) {
        while (PQisBusy(raw) != 0) {
            const PostgreSQLSocketWaitResult wait =
                WaitPostgreSQLSocket(raw, true, false, idle_deadline);
            if (wait == PostgreSQLSocketWaitResult::kTimeout) {
                FailPostgreSQLTransport(connection,
                    "PostgreSQL query response timed out after " +
                        std::to_string(PostgreSQLTransportTimeout(operation_timeout_ms)) +
                        " ms without network progress");
            }
            if (wait == PostgreSQLSocketWaitResult::kFailed || PQconsumeInput(raw) == 0) {
                const std::string message = PQerrorMessage(raw);
                FailPostgreSQLTransport(connection,
                    "cannot receive PostgreSQL query result: " + message);
            }
            idle_deadline = std::chrono::steady_clock::now() + timeout;
        }
        PGresult *additional = PQgetResult(raw);
        if (additional == nullptr) break;
        if (result != nullptr) PQclear(result);
        result = additional;
    }
    if (result == nullptr) {
        const std::string message = PQerrorMessage(raw);
        FailPostgreSQLTransport(connection,
            "PostgreSQL query returned no result: " + message);
    }
    return result;
}

PGconn *ConnectPostgreSQL(const std::string &connection_string,
                          uint32_t operation_timeout_ms,
                          const char *connection_name) {
    PGconn *connection = PQconnectStart(connection_string.c_str());
    if (connection == nullptr) {
        throw CallError(kPgDatabaseError,
                        std::string("cannot allocate ") + connection_name,
                        VEXFS_RUNTIME_BACKEND_POSTGRESQL);
    }
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(PostgreSQLTransportTimeout(operation_timeout_ms));
    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline) {
            PQfinish(connection);
            throw CallError(kPgDatabaseError,
                            std::string(connection_name) + " timed out after " +
                                std::to_string(PostgreSQLTransportTimeout(
                                    operation_timeout_ms)) + " ms",
                            VEXFS_RUNTIME_BACKEND_POSTGRESQL);
        }
        const PostgresPollingStatusType status = PQconnectPoll(connection);
        if (status == PGRES_POLLING_OK) {
            if (PQsetnonblocking(connection, 1) == 0) return connection;
            const std::string message = PQerrorMessage(connection);
            PQfinish(connection);
            throw CallError(kPgDatabaseError,
                            std::string("cannot enable nonblocking ") + connection_name +
                                ": " + message,
                            VEXFS_RUNTIME_BACKEND_POSTGRESQL);
        }
        if (status == PGRES_POLLING_FAILED) {
            const std::string message = PQerrorMessage(connection);
            PQfinish(connection);
            throw CallError(kPgDatabaseError,
                            std::string(connection_name) + " failed: " + message,
                            VEXFS_RUNTIME_BACKEND_POSTGRESQL);
        }
        if (status == PGRES_POLLING_ACTIVE) continue;
        const PostgreSQLSocketWaitResult wait = WaitPostgreSQLSocket(
            connection, status == PGRES_POLLING_READING,
            status == PGRES_POLLING_WRITING, deadline);
        if (wait != PostgreSQLSocketWaitResult::kReady) {
            PQfinish(connection);
            throw CallError(kPgDatabaseError,
                            std::string(connection_name) +
                                (wait == PostgreSQLSocketWaitResult::kTimeout
                                    ? " timed out after " : " socket failed within ") +
                                std::to_string(PostgreSQLTransportTimeout(
                                    operation_timeout_ms)) + " ms",
                            VEXFS_RUNTIME_BACKEND_POSTGRESQL);
        }
    }
}
#endif

class Call {
  public:
    Call(sqlite3 *db, const char *sql) : db_(db) { PrepareSQLite(sql); }
    Call(vexfs_mount_session *session, const char *sql, bool publisher = false) {
        if (session == nullptr) throw CallError(SQLITE_MISUSE, "mount session is NULL");
#if defined(VEXFS_HAVE_LIBPQ)
        if (session->postgresql) {
            if (publisher) EnsurePostgreSQLPublisherConnection(session);
            else EnsurePostgreSQLConnection(session);
            session_ = session;
            publisher_ = publisher;
            pg_slot_ = publisher ? &session->publisher_pg : &session->pg;
            pg_ = *pg_slot_;
            pg_timeout_ms_ = session->operation_timeout_ms;
            if (pg_ == nullptr || PQstatus(pg_) != CONNECTION_OK) {
                throw CallError(kPgDatabaseError, "PostgreSQL connection is not available",
                                VEXFS_RUNTIME_BACKEND_POSTGRESQL);
            }
            PreparePostgreSQL(sql);
            return;
        }
#endif
        db_ = session->db;
        PrepareSQLite(sql);
    }
    ~Call() {
        if (statement_ != nullptr) sqlite3_finalize(statement_);
#if defined(VEXFS_HAVE_LIBPQ)
        if (pg_result_ != nullptr) PQclear(pg_result_);
#endif
    }
    Call(const Call &) = delete;
    Call &operator=(const Call &) = delete;

    void Text(int index, const char *value) {
        if (value == nullptr) throw CallError(SQLITE_MISUSE, "required text is NULL");
#if defined(VEXFS_HAVE_LIBPQ)
        if (pg_ != nullptr) {
            PgBind(index, std::string(value), 0, 0, false);
            return;
        }
#endif
        Check(sqlite3_bind_text(statement_, index, value, -1, SQLITE_TRANSIENT));
    }
    void Int64(int index, int64_t value) {
#if defined(VEXFS_HAVE_LIBPQ)
        if (pg_ != nullptr) {
            PgBind(index, std::to_string(value), 0, 0, false);
            return;
        }
#endif
        Check(sqlite3_bind_int64(statement_, index, value));
    }
    void Int(int index, int value) { Int64(index, value); }
    void Null(int index) {
#if defined(VEXFS_HAVE_LIBPQ)
        if (pg_ != nullptr) {
            PgBind(index, std::string(), 0, 0, true);
            return;
        }
#endif
        Check(sqlite3_bind_null(statement_, index));
    }
    void Blob(int index, const void *data, uint64_t size) {
        if (size > static_cast<uint64_t>(std::numeric_limits<sqlite3_uint64>::max())) {
            throw CallError(SQLITE_TOOBIG, "buffer is too large");
        }
        if (data == nullptr && size != 0) throw CallError(SQLITE_MISUSE, "buffer is NULL");
#if defined(VEXFS_HAVE_LIBPQ)
        if (pg_ != nullptr) {
            if (size > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
                throw CallError(kPgInvalidArgument,
                                "PostgreSQL parameter is too large",
                                VEXFS_RUNTIME_BACKEND_POSTGRESQL);
            }
            PgBind(index,
                   std::string(static_cast<const char *>(size == 0 ? "" : data),
                               static_cast<size_t>(size)),
                   17, 1, false);
            return;
        }
#endif
        Check(sqlite3_bind_blob64(statement_, index, size == 0 ? "" : data, size, SQLITE_TRANSIENT));
    }
    void Row() {
#if defined(VEXFS_HAVE_LIBPQ)
        if (pg_ != nullptr) {
            ExecutePostgreSQL();
            if (PQntuples(pg_result_) < 1) {
                throw CallError(kPgNotFound, "PostgreSQL query returned no row",
                                VEXFS_RUNTIME_BACKEND_POSTGRESQL);
            }
            return;
        }
#endif
        const int rc = sqlite3_step(statement_);
        if (rc != SQLITE_ROW) Throw(rc);
    }
    bool MaybeRow() {
#if defined(VEXFS_HAVE_LIBPQ)
        if (pg_ != nullptr) {
            ExecutePostgreSQL();
            return PQntuples(pg_result_) > 0;
        }
#endif
        const int rc = sqlite3_step(statement_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        Throw(rc);
        return false;
    }
    int64_t ResultInt64(int column = 0) const {
#if defined(VEXFS_HAVE_LIBPQ)
        if (pg_ != nullptr) {
            const std::string value = ResultText(column);
            char *end = nullptr;
            const long long result = std::strtoll(value.c_str(), &end, 10);
            if (end == nullptr || *end != '\0') {
                throw CallError(kPgDatabaseError,
                                "PostgreSQL result is not an integer",
                                VEXFS_RUNTIME_BACKEND_POSTGRESQL);
            }
            return static_cast<int64_t>(result);
        }
#endif
        return sqlite3_column_int64(statement_, column);
    }
    std::string ResultText(int column = 0) const {
#if defined(VEXFS_HAVE_LIBPQ)
        if (pg_ != nullptr) {
            if (pg_result_ == nullptr || PQntuples(pg_result_) < 1 ||
                column < 0 || column >= PQnfields(pg_result_) ||
                PQgetisnull(pg_result_, 0, column)) {
                return std::string();
            }
            return std::string(PQgetvalue(pg_result_, 0, column),
                               static_cast<size_t>(PQgetlength(pg_result_, 0, column)));
        }
#endif
        if (column < 0 || column >= sqlite3_column_count(statement_)) return std::string();
        const unsigned char *value = sqlite3_column_text(statement_, column);
        return value == nullptr ? std::string() :
            std::string(reinterpret_cast<const char *>(value));
    }
    const void *ResultBlob() const {
#if defined(VEXFS_HAVE_LIBPQ)
        if (pg_ != nullptr) {
            PreparePostgreSQLResultBytes();
            return pg_result_bytes_.data();
        }
#endif
        return sqlite3_column_blob(statement_, 0);
    }
    int ResultBytes() const {
#if defined(VEXFS_HAVE_LIBPQ)
        if (pg_ != nullptr) {
            PreparePostgreSQLResultBytes();
            if (pg_result_bytes_.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
                throw CallError(kPgInvalidArgument, "PostgreSQL result is too large",
                                VEXFS_RUNTIME_BACKEND_POSTGRESQL);
            }
            return static_cast<int>(pg_result_bytes_.size());
        }
#endif
        return sqlite3_column_bytes(statement_, 0);
    }

  private:
    void PrepareSQLite(const char *sql) {
        if (db_ == nullptr) throw CallError(SQLITE_MISUSE, "SQLite connection is NULL");
        const int rc = sqlite3_prepare_v2(db_, sql, -1, &statement_, nullptr);
        if (rc != SQLITE_OK) Throw(rc);
    }
#if defined(VEXFS_HAVE_LIBPQ)
    struct PgParameter {
        std::string value;
        Oid type = 0;
        int format = 0;
        bool is_null = true;
    };

    void PreparePostgreSQL(const char *sql) {
        pg_sql_.clear();
        const std::string source(sql == nullptr ? "" : sql);
        for (size_t index = 0; index < source.size();) {
            if (source[index] == '?' && index + 1 < source.size() &&
                source[index + 1] >= '0' && source[index + 1] <= '9') {
                pg_sql_.push_back('$');
                ++index;
                while (index < source.size() && source[index] >= '0' && source[index] <= '9') {
                    pg_sql_.push_back(source[index++]);
                }
            } else {
                pg_sql_.push_back(source[index++]);
            }
        }
    }

    void PgBind(int index, std::string value, Oid type, int format, bool is_null) {
        if (index <= 0 || index > 1024) {
            throw CallError(kPgInvalidArgument, "PostgreSQL parameter index is invalid",
                            VEXFS_RUNTIME_BACKEND_POSTGRESQL);
        }
        if (pg_parameters_.size() < static_cast<size_t>(index)) {
            pg_parameters_.resize(static_cast<size_t>(index));
        }
        pg_parameters_[static_cast<size_t>(index - 1)] = {
            std::move(value), type, format, is_null};
    }

    static int PostgreSQLCode(const char *state) {
        if (state == nullptr) return kPgDatabaseError;
        const std::string value(state);
        if (value == "P0002") return kPgNotFound;
        if (value == "40001" || value == "23505") return kPgConflict;
        if (value == "42501") return kPgPermission;
        if (value == "25006") return kPgReadOnly;
        if (value == "53100") return kPgNoSpace;
        if (value == "55006" || value == "55P03") return kPgBusy;
        if (value == "XX001") return kPgCorruption;
        if (value == "2BP01") return kPgNotEmpty;
        if (value.rfind("22", 0) == 0 || value == "42809") return kPgInvalidArgument;
        return kPgDatabaseError;
    }

    void ExecutePostgreSQL() {
        if (pg_result_ != nullptr) return;
        std::vector<Oid> types(pg_parameters_.size());
        std::vector<const char *> values(pg_parameters_.size());
        std::vector<int> lengths(pg_parameters_.size());
        std::vector<int> formats(pg_parameters_.size());
        for (size_t index = 0; index < pg_parameters_.size(); ++index) {
            const PgParameter &parameter = pg_parameters_[index];
            types[index] = parameter.type;
            values[index] = parameter.is_null ? nullptr : parameter.value.data();
            lengths[index] = static_cast<int>(parameter.value.size());
            formats[index] = parameter.format;
        }
        try {
            pg_result_ = ExecutePostgreSQLQuery(
                pg_slot_, pg_timeout_ms_, pg_sql_.c_str(),
                static_cast<int>(pg_parameters_.size()),
                types.empty() ? nullptr : types.data(),
                values.empty() ? nullptr : values.data(),
                lengths.empty() ? nullptr : lengths.data(),
                formats.empty() ? nullptr : formats.data());
        } catch (...) {
            if (session_ != nullptr) {
                const auto retry_at =
                    std::chrono::steady_clock::now() + std::chrono::seconds(1);
                if (publisher_) session_->next_publisher_reconnect_attempt = retry_at;
                else {
                    session_->next_pg_reconnect_attempt = retry_at;
                    session_->pg_visibility_resync_required = true;
                }
            }
            throw;
        }
        const ExecStatusType status = PQresultStatus(pg_result_);
        if (status != PGRES_TUPLES_OK) {
            const char *state = PQresultErrorField(pg_result_, PG_DIAG_SQLSTATE);
            std::string message = PQresultErrorMessage(pg_result_);
            if (state != nullptr && *state != '\0') {
                message = std::string("SQLSTATE ") + state + ": " + message;
            }
            throw CallError(PostgreSQLCode(state), message,
                            VEXFS_RUNTIME_BACKEND_POSTGRESQL);
        }
    }

    void PreparePostgreSQLResultBytes() const {
        if (pg_result_bytes_ready_) return;
        pg_result_bytes_ready_ = true;
        pg_result_bytes_.clear();
        if (pg_result_ == nullptr || PQntuples(pg_result_) < 1 ||
            PQnfields(pg_result_) < 1 || PQgetisnull(pg_result_, 0, 0)) return;
        if (PQftype(pg_result_, 0) == 17) {
            size_t size = 0;
            unsigned char *value = PQunescapeBytea(
                reinterpret_cast<const unsigned char *>(PQgetvalue(pg_result_, 0, 0)),
                &size);
            if (value == nullptr) {
                throw CallError(kPgDatabaseError, "cannot decode PostgreSQL bytea result",
                                VEXFS_RUNTIME_BACKEND_POSTGRESQL);
            }
            pg_result_bytes_.assign(reinterpret_cast<const char *>(value), size);
            PQfreemem(value);
            return;
        }
        pg_result_bytes_.assign(PQgetvalue(pg_result_, 0, 0),
                                static_cast<size_t>(PQgetlength(pg_result_, 0, 0)));
        if (PQftype(pg_result_, 0) == 114 || PQftype(pg_result_, 0) == 3802) {
            std::string compact;
            compact.reserve(pg_result_bytes_.size());
            bool quoted = false;
            bool escaped = false;
            for (const char value : pg_result_bytes_) {
                if (quoted) {
                    compact.push_back(value);
                    if (escaped) escaped = false;
                    else if (value == '\\') escaped = true;
                    else if (value == '"') quoted = false;
                } else if (value == '"') {
                    quoted = true;
                    compact.push_back(value);
                } else if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
                    compact.push_back(value);
                }
            }
            pg_result_bytes_ = std::move(compact);
        }
    }
#endif

    void Check(int rc) {
        if (rc != SQLITE_OK) Throw(rc);
    }
    [[noreturn]] void Throw(int rc) {
        throw CallError(rc, sqlite3_errmsg(db_));
    }

    sqlite3 *db_ = nullptr;
    sqlite3_stmt *statement_ = nullptr;
#if defined(VEXFS_HAVE_LIBPQ)
    vexfs_mount_session *session_ = nullptr;
    PGconn **pg_slot_ = nullptr;
    PGconn *pg_ = nullptr;
    uint32_t pg_timeout_ms_ = 0;
    bool publisher_ = false;
    PGresult *pg_result_ = nullptr;
    std::string pg_sql_;
    std::vector<PgParameter> pg_parameters_;
    mutable bool pg_result_bytes_ready_ = false;
    mutable std::string pg_result_bytes_;
#endif
};

#if defined(VEXFS_HAVE_LIBPQ)
void RequirePostgreSQLCommand(PGconn **connection, uint32_t operation_timeout_ms,
                              const char *sql, const char *operation) {
    PGresult *result = ExecutePostgreSQLQuery(
        connection, operation_timeout_ms, sql);
    if (result != nullptr && PQresultStatus(result) == PGRES_COMMAND_OK) {
        PQclear(result);
        return;
    }
    const std::string message = result == nullptr
        ? "PostgreSQL command returned no result" : PQresultErrorMessage(result);
    if (result != nullptr) PQclear(result);
    throw CallError(kPgDatabaseError,
                    std::string(operation) + ": " + message,
                    VEXFS_RUNTIME_BACKEND_POSTGRESQL);
}

void ReconnectPostgreSQLConnection(PGconn **connection,
                                   const std::string &connection_string,
                                   uint32_t operation_timeout_ms,
                                   const char *connection_name) {
    DropPostgreSQLConnection(connection);
    *connection = ConnectPostgreSQL(connection_string, operation_timeout_ms,
                                    connection_name);
}

void EnsurePostgreSQLConnection(vexfs_mount_session *session) {
    if (session == nullptr || !session->postgresql || session->connection.empty()) {
        throw CallError(kPgDatabaseError, "PostgreSQL connection is not available",
                        VEXFS_RUNTIME_BACKEND_POSTGRESQL);
    }
    if (session->pg != nullptr && PQstatus(session->pg) == CONNECTION_OK) {
        session->next_pg_reconnect_attempt = {};
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < session->next_pg_reconnect_attempt) {
        throw CallError(kPgDatabaseError,
                        "PostgreSQL reconnect is cooling down after a failed attempt",
                        VEXFS_RUNTIME_BACKEND_POSTGRESQL);
    }

    // Only reconnect before a new operation.  A command interrupted in flight
    // has an unknown commit state, so this layer never replays it implicitly.
    try {
        ReconnectPostgreSQLConnection(&session->pg, session->connection,
                                      session->operation_timeout_ms,
                                      "PostgreSQL connection reconnect");
        if (session->operation_timeout_ms != 0) {
            const std::string command = "SET statement_timeout TO " +
                std::to_string(session->operation_timeout_ms);
            RequirePostgreSQLCommand(&session->pg, session->operation_timeout_ms,
                                     command.c_str(), "restore statement timeout");
        }
        RequirePostgreSQLCommand(&session->pg, session->operation_timeout_ms,
                                 "LISTEN vexfs_change",
                                 "restore change notifications");
        if (session->session_started) {
            Call start(session, "SELECT vexfs_mount_session_start(?1,?2)");
            start.Text(1, session->workspace.c_str());
            start.Text(2, session->session_id.c_str());
            start.Row();
            session->next_heartbeat =
                std::chrono::steady_clock::now() + std::chrono::seconds(10);
        }
        // LISTEN notifications emitted while the socket was gone cannot be
        // replayed. Keep this flag until visibility has been read successfully.
        session->pg_visibility_resync_required = true;
        session->next_pg_reconnect_attempt = {};
    } catch (...) {
        DropPostgreSQLConnection(&session->pg);
        session->pg_visibility_resync_required = true;
        // NFS can retransmit one metadata RPC while this reconnect is still in
        // progress. Make queued duplicates fail fast instead of starting a
        // second full reconnect and extending one filesystem call indefinitely.
        session->next_pg_reconnect_attempt =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        throw;
    }
}

void EnsurePostgreSQLPublisherConnection(vexfs_mount_session *session) {
    if (session == nullptr || !session->postgresql || session->connection.empty()) {
        throw CallError(kPgDatabaseError,
                        "PostgreSQL publisher connection is not available",
                        VEXFS_RUNTIME_BACKEND_POSTGRESQL);
    }
    if (session->publisher_pg != nullptr &&
        PQstatus(session->publisher_pg) == CONNECTION_OK) {
        session->next_publisher_reconnect_attempt = {};
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < session->next_publisher_reconnect_attempt) {
        throw CallError(kPgDatabaseError,
                        "PostgreSQL publisher reconnect is cooling down after a failed attempt",
                        VEXFS_RUNTIME_BACKEND_POSTGRESQL);
    }

    // As with the foreground connection, never replay a command whose commit
    // result is unknown. The gateway keeps the claimed generations and retries
    // them only after the database reports their current state.
    try {
        ReconnectPostgreSQLConnection(&session->publisher_pg, session->connection,
                                      session->operation_timeout_ms,
                                      "PostgreSQL publisher reconnect");
        if (session->operation_timeout_ms != 0) {
            const std::string command = "SET statement_timeout TO " +
                std::to_string(session->operation_timeout_ms);
            RequirePostgreSQLCommand(
                &session->publisher_pg, session->operation_timeout_ms,
                command.c_str(), "restore publisher statement timeout");
        }
        session->next_publisher_reconnect_attempt = {};
    } catch (...) {
        DropPostgreSQLConnection(&session->publisher_pg);
        session->next_publisher_reconnect_attempt =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        throw;
    }
}
#endif

void ClearError(vexfs_mount_error *error,
                const char *backend = VEXFS_RUNTIME_BACKEND_SQLITE) {
    if (error == nullptr) return;
    error->status = VEXFS_MOUNT_OK;
    error->native_code = SQLITE_OK;
    std::snprintf(error->backend, sizeof(error->backend), "%s",
                  backend);
    error->message[0] = '\0';
}

vexfs_mount_status MapStatus(int sqlite_code, const std::string &) {
    if (sqlite_code == kUnsupportedBackend) return VEXFS_MOUNT_UNSUPPORTED;
    if (sqlite_code == kPgInvalidArgument) return VEXFS_MOUNT_INVALID_ARGUMENT;
    if (sqlite_code == kPgNotFound) return VEXFS_MOUNT_NOT_FOUND;
    if (sqlite_code == kPgConflict) return VEXFS_MOUNT_CONFLICT;
    if (sqlite_code == kPgReadOnly) return VEXFS_MOUNT_READ_ONLY;
    if (sqlite_code == kPgBusy) return VEXFS_MOUNT_BUSY;
    if (sqlite_code == kPgPermission) return VEXFS_MOUNT_PERMISSION_DENIED;
    if (sqlite_code == kPgNoSpace) return VEXFS_MOUNT_NO_SPACE;
    if (sqlite_code == kPgCorruption) return VEXFS_MOUNT_CORRUPTION;
    if (sqlite_code == kPgNotEmpty) return VEXFS_MOUNT_NOT_EMPTY;
    if (sqlite_code == kPgDatabaseError) return VEXFS_MOUNT_DATABASE_ERROR;
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
                            const std::string &message,
                            const std::string &backend = VEXFS_RUNTIME_BACKEND_SQLITE) {
    const vexfs_mount_status status = MapStatus(sqlite_code, message);
    if (error != nullptr) {
        error->status = status;
        error->native_code = sqlite_code;
        std::snprintf(error->backend, sizeof(error->backend), "%s",
                      backend.c_str());
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
        return SetError(error, exception.code, exception.what(), exception.backend);
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
    if (error != nullptr && session != nullptr && session->postgresql) {
        ClearError(error, VEXFS_RUNTIME_BACKEND_POSTGRESQL);
    }
    const vexfs_mount_status status = Guard(error, [&] {
        if (session == nullptr) throw CallError(SQLITE_MISUSE, "mount session is NULL");
        std::lock_guard<std::recursive_mutex> lock(session->mutex);
        ++session->contract_calls;
        function();
    });
    if (status == VEXFS_MOUNT_OK && error != nullptr && session != nullptr &&
        session->postgresql) {
        ClearError(error, VEXFS_RUNTIME_BACKEND_POSTGRESQL);
    }
    return status;
}

#if defined(VEXFS_HAVE_LIBPQ)
template <typename Function>
vexfs_mount_status GuardPublisher(vexfs_mount_session *session,
                                  vexfs_mount_error *error,
                                  Function function) {
    if (error != nullptr) {
        ClearError(error, VEXFS_RUNTIME_BACKEND_POSTGRESQL);
    }
    const vexfs_mount_status status = Guard(error, [&] {
        if (session == nullptr || !session->postgresql) {
            throw CallError(kUnsupportedBackend,
                            "background publisher requires PostgreSQL",
                            VEXFS_RUNTIME_BACKEND_POSTGRESQL);
        }
        std::lock_guard<std::mutex> lock(session->publisher_mutex);
        EnsurePostgreSQLPublisherConnection(session);
        function();
    });
    if (status == VEXFS_MOUNT_OK && error != nullptr) {
        ClearError(error, VEXFS_RUNTIME_BACKEND_POSTGRESQL);
    }
    return status;
}
#endif

void RequireSession(vexfs_mount_session *session) {
    if (session == nullptr ||
        (!session->postgresql && session->db == nullptr)
#if defined(VEXFS_HAVE_LIBPQ)
        || (session->postgresql && session->connection.empty())
#endif
        ) {
        throw CallError(SQLITE_MISUSE, "mount session is NULL");
    }
    if (session->session_started) {
        const auto now = std::chrono::steady_clock::now();
        if (now < session->next_heartbeat) return;
        Call heartbeat(session, "SELECT vexfs_mount_session_heartbeat(?1,?2)");
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
    // Never open and close a WAL/SHM sidecar while SQLite owns the database.
    // POSIX drops every fcntl lock that this process holds on a file when any
    // descriptor for that file is closed. Opening -shm here used to release
    // SQLite's WAL locks, allowing a peer process to truncate the still-mapped
    // wal-index and crash the gateway with SIGBUS.
    struct stat info {};
    if (lstat(path.c_str(), &info) != 0) {
        if (errno == ENOENT) return;
        throw CallError(SQLITE_AUTH,
                        "cannot inspect SQLite sidecar: " + path + ": " +
                        std::strerror(errno));
    }
    if (S_ISLNK(info.st_mode) || !S_ISREG(info.st_mode)) {
        throw CallError(SQLITE_AUTH,
                        "SQLite sidecar is not a private regular file: " + path);
    }
    if (info.st_uid != geteuid()) {
        throw CallError(SQLITE_AUTH,
                        "SQLite sidecar is not owned by the current user: " + path);
    }
    // The database lives in a user-private directory, so a path-based chmod is
    // safe here and, unlike open/fchmod/close, cannot disturb SQLite WAL locks.
    if ((info.st_mode & 0777) != 0600 && chmod(path.c_str(), 0600) != 0) {
        throw CallError(SQLITE_AUTH,
                        "cannot protect SQLite sidecar: " + path + ": " +
                        std::strerror(errno));
    }
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
    if (session->postgresql || session->synchronous_mode == 1) return;
    Exec(session->db, "PRAGMA synchronous=NORMAL");
    session->synchronous_mode = 1;
    ++session->synchronous_mode_switches;
}

void UseRelaxedDurability(vexfs_mount_session *session) {
    ++session->ordinary_mutation_calls;
    ++session->mutation_epoch;
    if (session->postgresql || session->synchronous_mode == 0) return;
    Exec(session->db, "PRAGMA synchronous=OFF");
    session->synchronous_mode = 0;
    ++session->synchronous_mode_switches;
}

void UseFullDurability(vexfs_mount_session *session) {
    ++session->full_boundary_calls;
    if (session->postgresql || session->synchronous_mode == 2) return;
    // fsync/synchronize/snapshot 前切回 FULL。随后必须执行真实写事务；仅修改
    // PRAGMA 不会形成持久化屏障。
    Exec(session->db, "PRAGMA synchronous=FULL");
    session->synchronous_mode = 2;
    ++session->synchronous_mode_switches;
}

void UseRequestedDurability(vexfs_mount_session *session, const char *durability) {
    if (std::strcmp(durability, "full") == 0) UseFullDurability(session);
    else if (std::strcmp(durability, "none") == 0) UseRelaxedDurability(session);
    else UseOrdinaryDurability(session);
}

void DurabilityBarrier(vexfs_mount_session *session) {
    if (session->postgresql) {
        ++session->durability_barriers;
        return;
    }
    Call barrier(session,
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
    if (!session->postgresql && session->exclusive_gateway &&
        safe_without_persistent_replay) return "";
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
            config->connection == nullptr || config->workspace == nullptr) {
            throw CallError(SQLITE_MISUSE,
                "config, output, backend, connection and workspace are required");
        }
        if (config->abi_version != VEXFS_RUNTIME_ABI_VERSION) {
            throw CallError(SQLITE_MISUSE, "unsupported runtime ABI version");
        }
        const bool sqlite_backend =
            std::strcmp(config->backend, VEXFS_RUNTIME_BACKEND_SQLITE) == 0;
        const bool postgresql_backend =
            std::strcmp(config->backend, VEXFS_RUNTIME_BACKEND_POSTGRESQL) == 0;
        if (!sqlite_backend && !postgresql_backend) {
            throw CallError(kUnsupportedBackend,
                            "runtime backend must be sqlite or postgresql");
        }
        if (sqlite_backend &&
            (config->principal == nullptr || std::strcmp(config->principal, "local") != 0)) {
            throw CallError(SQLITE_AUTH,
                "sqlite backend is single-user and only accepts the local principal");
        }
        vexfs_mount_session *session = new vexfs_mount_session();
        session->connection = config->connection;
        session->workspace = config->workspace;
        session->principal = config->principal == nullptr ? "" : config->principal;
        session->session_id = RandomSessionId();
        session->exclusive_gateway = (config->flags & VEXFS_RUNTIME_EXCLUSIVE_GATEWAY) != 0;
        const bool no_create = (config->flags & VEXFS_RUNTIME_OPEN_NO_CREATE) != 0;
        session->no_create = no_create;
        session->operation_timeout_ms = config->operation_timeout_ms;
#if defined(VEXFS_HAVE_LIBPQ)
        if (postgresql_backend) {
            session->postgresql = true;
            try {
                session->pg = ConnectPostgreSQL(
                    session->connection, session->operation_timeout_ms,
                    "PostgreSQL connection");
                session->publisher_pg = ConnectPostgreSQL(
                    session->connection, session->operation_timeout_ms,
                    "PostgreSQL publisher connection");
                if (config->operation_timeout_ms != 0) {
                    const std::string command = "SET statement_timeout TO " +
                        std::to_string(config->operation_timeout_ms);
                    RequirePostgreSQLCommand(
                        &session->pg, session->operation_timeout_ms,
                        command.c_str(), "set statement timeout");
                    RequirePostgreSQLCommand(
                        &session->publisher_pg, session->operation_timeout_ms,
                        command.c_str(), "set publisher statement timeout");
                }
                Call version(session, "SELECT vexfs_pg_adapter_version()");
                version.Row();
                Call actor(session, "SELECT session_user");
                actor.Row();
                session->principal = actor.ResultText();
                bool workspace_exists = true;
                try {
                    Call workspace(session, "SELECT vexfs_workspace_stat(?1)");
                    workspace.Text(1, config->workspace);
                    workspace.Row();
                } catch (const CallError &exception) {
                    if (exception.code != kPgNotFound || no_create) throw;
                    workspace_exists = false;
                }
                if (!workspace_exists) {
                    Call create(session, "SELECT vexfs_workspace_create(?1)");
                    create.Text(1, config->workspace);
                    create.Row();
                }
                RequirePostgreSQLCommand(
                    &session->pg, session->operation_timeout_ms,
                    "LISTEN vexfs_change", "listen for change notifications");
                if (!no_create) {
                    Call start(session, "SELECT vexfs_mount_session_start(?1,?2)");
                    start.Text(1, config->workspace);
                    start.Text(2, session->session_id.c_str());
                    start.Row();
                    session->session_started = true;
                    Call recover(session,
                        "SELECT vexfs_mount_synchronize(?1,?2,?3)");
                    recover.Text(1, config->workspace);
                    recover.Text(2, ("mount-recover:" + session->session_id).c_str());
                    recover.Text(3, session->session_id.c_str());
                    recover.Row();
                    session->next_heartbeat =
                        std::chrono::steady_clock::now() + std::chrono::seconds(10);
                }
            } catch (...) {
                if (session->session_started) {
                    try {
                        Call end(session, "SELECT vexfs_mount_session_end(?1,?2)");
                        end.Text(1, session->workspace.c_str());
                        end.Text(2, session->session_id.c_str());
                        end.Row();
                    } catch (...) {
                    }
                }
                DropPostgreSQLConnection(&session->publisher_pg);
                DropPostgreSQLConnection(&session->pg);
                delete session;
                throw;
            }
            *output = session;
            return;
        }
#else
        if (postgresql_backend) {
            delete session;
            throw CallError(kUnsupportedBackend,
                            "postgresql backend was built without libpq");
        }
#endif
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
                    "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;"
                    // Move automatic checkpoints out of the hottest small-file loop.
                    // This is a target threshold, not a hard WAL-size limit when readers
                    // prevent a checkpoint.
                    "PRAGMA wal_autocheckpoint=8192;"
                    // One gateway owns one long-lived connection. Keep its suggested page
                    // cache near 32 MiB so history lookups do not fall back to a tiny
                    // default cache as the workspace grows.
                    "PRAGMA cache_size=-32768;");
                ProtectSidecar(std::string(config->connection) + "-wal");
                ProtectSidecar(std::string(config->connection) + "-shm");
                Call init(session, "SELECT vexfs_init()");
                init.Row();
                Call workspace(session, "SELECT vexfs_workspace_create(?1)");
                workspace.Text(1, config->workspace);
                workspace.Row();
                ProtectSidecar(std::string(config->connection) + "-wal");
                ProtectSidecar(std::string(config->connection) + "-shm");
                if (session->exclusive_gateway) {
                    CheckExistingLocalSession(session->db, config->workspace,
                                              session->session_id);
                    Call start(session, "SELECT vexfs_mount_session_start(?1,?2)");
                    start.Text(1, config->workspace);
                    start.Text(2, session->session_id.c_str());
                    start.Row();
                    session->session_started = true;
                    // A previous gateway may have died after staging writes but before
                    // close. SessionStart claims those retained handles for this new
                    // session; publish them before exposing the remounted workspace.
                    Call recover(session,
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
                session->synchronous_mode = 1;
                ++session->synchronous_mode_switches;
            }
        } catch (...) {
            if (session->session_started) {
                try {
                    Call end(session, "SELECT vexfs_mount_session_end(?1,?2)");
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
    if (session->db != nullptr || session->postgresql
#if defined(VEXFS_HAVE_LIBPQ)
        || session->pg != nullptr || session->publisher_pg != nullptr
#endif
    ) {
        std::lock_guard<std::recursive_mutex> lock(session->mutex);
        try {
            if (!session->no_create) {
                UseFullDurability(session);
                if (session->postgresql && session->session_started) {
                    // A gateway can own thousands of small staged files. One
                    // all-in-one synchronize transaction may exceed the normal
                    // statement timeout and roll every publish back during
                    // unmount. Commit bounded shutdown batches instead; no NFS
                    // request can arrive after the volume has been detached.
                    for (;;) {
                        Call publish(session,
                            "SELECT vexfs_mount_publish_close_all(?1,?2,'full',64)");
                        publish.Text(1, session->workspace.c_str());
                        publish.Text(2, session->session_id.c_str());
                        publish.Row();
                        if (publish.ResultInt64() == 0) break;
                    }
                } else {
                    Call synchronize(session, session->session_started
                        ? "SELECT vexfs_mount_synchronize(?1,'',?2)"
                        : "SELECT vexfs_mount_synchronize(?1,'')");
                    synchronize.Text(1, session->workspace.c_str());
                    if (session->session_started) {
                        synchronize.Text(2, session->session_id.c_str());
                    }
                    synchronize.Row();
                }
                DurabilityBarrier(session);
                if (session->session_started) {
                    Call end(session, "SELECT vexfs_mount_session_end(?1,?2)");
                    end.Text(1, session->workspace.c_str());
                    end.Text(2, session->session_id.c_str());
                    end.Row();
                    session->session_started = false;
                }
            }
        } catch (...) {
            // close 不能抛异常；未发布内容仍留在数据库中，后续可诊断和回收。
        }
        if (session->db != nullptr) sqlite3_close(session->db);
#if defined(VEXFS_HAVE_LIBPQ)
        if (session->publisher_pg != nullptr) {
            std::lock_guard<std::mutex> publisher_lock(session->publisher_mutex);
            PQfinish(session->publisher_pg);
            session->publisher_pg = nullptr;
        }
        if (session->pg != nullptr) PQfinish(session->pg);
#endif
    }
    delete session;
}

extern "C" vexfs_mount_status vexfs_mount_session_keepalive(
    vexfs_mount_session *session, vexfs_mount_error *error) {
    return Guard(session, error, [&] { RequireSession(session); });
}

extern "C" vexfs_mount_status vexfs_mount_diagnostics(vexfs_mount_session *session,
                                                         vexfs_mount_bytes *json,
                                                         vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (session->postgresql) {
            Call diagnostics(session, "SELECT vexfs_diagnostics(?1)");
            diagnostics.Text(1, session->workspace.c_str());
            diagnostics.Row();
            CopyResult(diagnostics, json);
            return;
        }
        // doctor 以只读方式打开数据库，不初始化也不改写 schema。
        Call contract(session,
            "SELECT value FROM _vexfs_meta WHERE key='contract_version'");
        contract.Row();
        Call layout(session,
            "SELECT value FROM _vexfs_meta WHERE key='staging_layout'");
        layout.Row();
        Call journal(session, "PRAGMA journal_mode");
        journal.Row();
        Call synchronous(session, "PRAGMA synchronous");
        synchronous.Row();
        Call foreign_keys(session, "PRAGMA foreign_keys");
        foreign_keys.Row();
        Call workspace_exists(session,
            "SELECT count(*) FROM _vexfs_workspaces WHERE name=?1");
        workspace_exists.Text(1, session->workspace.c_str());
        workspace_exists.Row();
        Call pending(session,
            "SELECT count(*) FROM _vexfs_handles WHERE workspace_id="
            "(SELECT id FROM _vexfs_workspaces WHERE name=?1) "
            "AND state IN ('open','retained') AND dirty_generation>published_generation");
        pending.Text(1, session->workspace.c_str());
        pending.Row();
        Call retained(session,
            "SELECT count(*) FROM _vexfs_handles WHERE workspace_id="
            "(SELECT id FROM _vexfs_workspaces WHERE name=?1) AND state='retained'");
        retained.Text(1, session->workspace.c_str());
        retained.Row();
        const bool split_staging = sqlite3_table_column_metadata(
            session->db, "main", "_vexfs_staging", "logical_size",
            nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
        Call staging(session, split_staging
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
        const std::string staging_layout = layout.ResultText();
        const bool schema_ready = contract_version == "0.9.0" &&
                                  staging_layout == "overlay-v1";
        std::string recovery = "null";
        const bool snapshot_policy_ready =
            sqlite3_table_column_metadata(
                session->db, "main", "_vexfs_snapshots", "snapshot_type",
                nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK &&
            sqlite3_table_column_metadata(
                session->db, "main", "_vexfs_workspaces", "snapshot_agent_keep",
                nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK &&
            sqlite3_table_column_metadata(
                session->db, "main", "_vexfs_workspaces", "snapshot_safety_keep",
                nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK &&
            sqlite3_table_column_metadata(
                session->db, "main", "_vexfs_workspaces", "snapshot_keep_days",
                nullptr, nullptr, nullptr, nullptr, nullptr) == SQLITE_OK;
        if (schema_ready && snapshot_policy_ready && workspace_exists.ResultInt64() == 1) {
            // 两个状态函数只创建连接级临时查询表，不改写数据库文件。doctor 因此仍然
            // 不初始化、不迁移，同时可以给出版本恢复占用和可回收空间。
            Call recovery_status(session, R"SQL(
WITH status AS (
  SELECT vexfs_snapshot_policy_get(?1) AS snapshot_policy,
         vexfs_retention_get(?1) AS retention
)
SELECT json_object(
  'snapshot_count',json_extract(snapshot_policy,'$.snapshots.total'),
  'protected_history_bytes',max(
    json_extract(retention,'$.retained_history_bytes')-
    json_extract(retention,'$.reclaimable_bytes'),0),
  'reclaimable_bytes',json_extract(retention,'$.reclaimable_bytes'),
  'oldest_recovery_commit',json_extract(snapshot_policy,'$.oldest_recovery_commit'),
  'oldest_recovery_created_at',
    json_extract(snapshot_policy,'$.oldest_recovery_created_at'))
FROM status
)SQL");
            recovery_status.Text(1, session->workspace.c_str());
            recovery_status.Row();
            recovery = recovery_status.ResultText();
        }
        const std::string value =
            "{\"schema_version\":\"" + JsonEscape(contract_version) +
            "\",\"staging_layout\":\"" + JsonEscape(staging_layout) +
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
            ",\"recovery\":" + recovery +
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
        Call call(session, "SELECT vexfs_mkdir(?1,?2)");
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
        Call call(session, "SELECT vexfs_create(?1,?2,?3,?4)");
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
        Call call(session, "SELECT vexfs_set_mode(?1,?2,?3)");
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
        Call call(session, "SELECT vexfs_set_times(?1,?2,?3,?4,?5)");
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
        Call call(session, "SELECT vexfs_symlink(?1,?2,?3)");
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
        Call call(session, "SELECT vexfs_readlink(?1,?2)");
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
        Call call(session, "SELECT vexfs_link(?1,?2,?3)");
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
        Call call(session, "SELECT vexfs_chown(?1,?2,?3,?4)");
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
        Call call(session, "SELECT vexfs_write(?1,?2,?3)");
        call.Text(1, session->workspace.c_str()); call.Text(2, path); call.Blob(3, data, size);
        call.Row(); *version = call.ResultInt64();
    });
}

extern "C" vexfs_mount_status vexfs_mount_write_file_range(
        vexfs_mount_session *session, const char *path, uint64_t offset,
        const void *data, uint64_t size, const char *request_id,
        const char *durability, int64_t *version, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (path == nullptr || request_id == nullptr || durability == nullptr ||
            version == nullptr || (size > 0 && data == nullptr) ||
            offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            throw CallError(SQLITE_MISUSE,
                            "path, content, request, durability and version are required");
        }
        UseRequestedDurability(session, durability);
#if defined(VEXFS_HAVE_LIBPQ)
        if (session->postgresql) {
            Call call(session, "SELECT vexfs_write_range(?1,?2,?3,?4,?5,?6,?7)");
            call.Text(1, session->workspace.c_str());
            call.Text(2, path);
            call.Int64(3, static_cast<int64_t>(offset));
            call.Blob(4, data, size);
            call.Text(5, request_id);
            call.Text(6, session->session_id.c_str());
            call.Text(7, durability);
            call.Row();
            *version = call.ResultInt64();
            return;
        }
#endif
        const std::string open_request = std::string(request_id) + ":open";
        Call open(session, "SELECT vexfs_handle_open(?1,?2,'rw',?3,?4)");
        open.Text(1, session->workspace.c_str());
        open.Text(2, path);
        open.Text(3, EffectiveRequestId(session, open_request.c_str(), true));
        open.Text(4, session->session_id.c_str());
        open.Row();
        const std::string handle = open.ResultText();
        try {
            const std::string stage_request = std::string(request_id) + ":stage";
            Call stage(session, "SELECT vexfs_handle_stage_write(?1,?2,?3,?4)");
            stage.Text(1, handle.c_str());
            stage.Int64(2, static_cast<int64_t>(offset));
            stage.Blob(3, data, size);
            stage.Text(4, EffectiveRequestId(session, stage_request.c_str(), true));
            stage.Row();
            const int64_t generation = stage.ResultInt64();
            Call publish(session, "SELECT vexfs_handle_publish_close(?1,?2,?3)");
            publish.Text(1, handle.c_str());
            publish.Int64(2, generation);
            publish.Text(3, durability);
            publish.Row();
            *version = publish.ResultInt64();
        } catch (...) {
            try {
                const std::string close_request = std::string(request_id) + ":close-failed";
                Call close(session, "SELECT vexfs_handle_close(?1,0,?2)");
                close.Text(1, handle.c_str());
                close.Text(2, close_request.c_str());
                close.Row();
            } catch (...) {
            }
            throw;
        }
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
        Call call(session, "SELECT vexfs_append(?1,?2,?3)");
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
        Call call(session, "SELECT vexfs_read(?1,?2)");
        call.Text(1, session->workspace.c_str()); call.Text(2, path); call.Row();
        CopyResult(call, content);
    });
}

extern "C" vexfs_mount_status vexfs_mount_read_file_range(
        vexfs_mount_session *session, const char *path, uint64_t offset,
        uint64_t length, vexfs_mount_bytes *content, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (path == nullptr || content == nullptr ||
            offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
            length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            throw CallError(SQLITE_MISUSE,
                            "path, content and a valid read range are required");
        }
        Call call(session, "SELECT vexfs_read_range(?1,?2,?3,?4)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, path);
        call.Int64(3, static_cast<int64_t>(offset));
        call.Int64(4, static_cast<int64_t>(length));
        call.Row();
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
        Call call(session, "SELECT vexfs_grep(?1,?2,?3,?4,?5)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, path);
        call.Text(3, pattern);
        call.Int64(4, static_cast<int64_t>(flags));
        call.Int64(5, static_cast<int64_t>(limit));
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_find(
        vexfs_mount_session *session, const char *path, const char *name_pattern,
        const char *kind, int64_t min_size, int64_t max_size,
        int64_t modified_after_ms, int64_t modified_before_ms,
        const char *after_path, uint32_t limit, vexfs_mount_bytes *json,
        vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (path == nullptr || name_pattern == nullptr || kind == nullptr ||
            after_path == nullptr || json == nullptr) {
            throw CallError(SQLITE_MISUSE, "find inputs and output are required");
        }
        if (limit == 0 || limit > 1000 || min_size < -1 || max_size < -1 ||
            modified_after_ms < -1 || modified_before_ms < -1) {
            throw CallError(SQLITE_RANGE, "find limits are outside the supported range");
        }
        Call call(session, "SELECT vexfs_find(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, path);
        if (*name_pattern == '\0') call.Null(3); else call.Text(3, name_pattern);
        if (*kind == '\0') call.Null(4); else call.Text(4, kind);
        if (min_size < 0) call.Null(5); else call.Int64(5, min_size);
        if (max_size < 0) call.Null(6); else call.Int64(6, max_size);
        if (modified_after_ms < 0) call.Null(7); else call.Int64(7, modified_after_ms);
        if (modified_before_ms < 0) call.Null(8); else call.Int64(8, modified_before_ms);
        if (*after_path == '\0') call.Null(9); else call.Text(9, after_path);
        call.Int64(10, limit);
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
        Call call(session, "SELECT vexfs_check(?1,?2)");
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
        Call call(session, session->postgresql
            ? "SELECT vexfs_grep_index(?1,?2)"
            : "SELECT vexfs_grep_index(?1)");
        if (session->postgresql) {
            call.Text(1, session->workspace.c_str());
            call.Text(2, action);
        } else {
            call.Text(1, action);
        }
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
        Call call(session, session->postgresql
            ? "SELECT vexfs_history_json(?1,?2,100,0)"
            : "SELECT vexfs_history(?1,?2)");
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
        Call call(session, session->postgresql
            ? "SELECT vexfs_history_json(?1,?2,?3,?4)"
            : "SELECT vexfs_history(?1,?2,?3,?4)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, path);
        call.Int64(3, static_cast<int64_t>(limit));
        call.Int64(4, before_version);
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_workspace_log_page(
    vexfs_mount_session *session, uint32_t limit, int64_t before_commit,
    vexfs_mount_bytes *json, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (json == nullptr || limit == 0 || limit > 1000 || before_commit < 0) {
            throw CallError(
                SQLITE_RANGE,
                "workspace log output is required, limit must be 1..1000, and before must be non-negative");
        }
        Call call(session, "SELECT vexfs_workspace_log(?1,?2,?3)");
        call.Text(1, session->workspace.c_str());
        call.Int64(2, static_cast<int64_t>(limit));
        call.Int64(3, before_commit);
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_workspace_show_commit_page(
    vexfs_mount_session *session, int64_t commit, const char *after_path,
    uint32_t limit, vexfs_mount_bytes *json, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (commit <= 0 || after_path == nullptr || limit == 0 || json == nullptr) {
            throw CallError(
                SQLITE_MISUSE,
                "positive commit, cursor, limit and output are required");
        }
        if (session->postgresql) {
            throw CallError(
                kUnsupportedBackend,
                "historical commit inspection is currently supported only by SQLite",
                VEXFS_RUNTIME_BACKEND_POSTGRESQL);
        }
        Call call(session,
            "SELECT vexfs_workspace_show_commit(?1,?2,?3,?4)");
        call.Text(1, session->workspace.c_str());
        call.Int64(2, commit);
        call.Text(3, after_path);
        call.Int64(4, limit);
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_workspace_diff_commits_page(
    vexfs_mount_session *session, int64_t from_commit, int64_t to_commit,
    const char *after_path, uint32_t limit, vexfs_mount_bytes *json,
    vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (from_commit <= 0 || to_commit < 0 || after_path == nullptr ||
            limit == 0 || json == nullptr) {
            throw CallError(
                SQLITE_MISUSE,
                "positive from commit, optional to commit, cursor, limit and output are required");
        }
        if (session->postgresql) {
            throw CallError(
                kUnsupportedBackend,
                "historical commit inspection is currently supported only by SQLite",
                VEXFS_RUNTIME_BACKEND_POSTGRESQL);
        }
        Call call(session,
            "SELECT vexfs_workspace_diff_commits(?1,?2,?3,?4,?5)");
        call.Text(1, session->workspace.c_str());
        call.Int64(2, from_commit);
        if (to_commit == 0) call.Null(3); else call.Int64(3, to_commit);
        call.Text(4, after_path);
        call.Int64(5, limit);
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
        Call call(session, "SELECT vexfs_read_version(?1,?2,?3)");
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
        Call call(session, "SELECT vexfs_compare_versions(?1,?2,?3,?4)");
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
        Call call(session, "SELECT vexfs_restore_version(?1,?2,?3,?4)");
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
        Call call(session, "SELECT vexfs_stat(?1,?2)");
        call.Text(1, session->workspace.c_str()); call.Text(2, path); call.Row(); CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_path_for_inode(vexfs_mount_session *session,
                                                            int64_t inode,
                                                            vexfs_mount_bytes *path,
                                                            vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        Call call(session, "SELECT vexfs_path(?1,?2)");
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
        Call call(session, session->postgresql
            ? "SELECT vexfs_list_json(?1,?2)"
            : "SELECT vexfs_list(?1,?2)");
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
        Call call(session, session->postgresql
            ? "SELECT vexfs_list_versioned_json(?1,?2)"
            : "SELECT json_object('version',"
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
        Call call(session, "SELECT vexfs_move(?1,?2,?3)");
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
        Call call(session, "SELECT vexfs_rename(?1,?2,?3,?4)");
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
        Call call(session, "SELECT vexfs_remove(?1,?2,?3)");
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
        Call call(session, "SELECT vexfs_xattr_get(?1,?2,?3)");
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
        Call call(session, "SELECT vexfs_xattr_list(?1,?2)");
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
        Call call(session, "SELECT vexfs_xattr_set(?1,?2,?3,?4,?5)");
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
        Call call(session, "SELECT vexfs_acl_get(?1,?2)");
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
        Call call(session, "SELECT vexfs_acl_set(?1,?2,?3)");
        call.Text(1, session->workspace.c_str());
        call.Int64(2, inode);
        if (session->postgresql) {
            const std::string acl(static_cast<const char *>(json),
                                  static_cast<size_t>(size));
            call.Text(3, acl.c_str());
        } else {
            call.Blob(3, json, size);
        }
        call.Row();
    });
}

extern "C" vexfs_mount_status vexfs_mount_workspace_head(vexfs_mount_session *session,
                                                               int64_t *head_commit,
                                                               vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (head_commit == nullptr) throw CallError(SQLITE_MISUSE, "head output is NULL");
        Call call(session, session->postgresql
            ? "SELECT vexfs_workspace_head(?1)"
            : "SELECT COALESCE(head_commit,0) FROM _vexfs_workspaces WHERE name=?1");
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
        if (session->postgresql) {
#if defined(VEXFS_HAVE_LIBPQ)
            // PQconsumeInput does not reconnect a broken connection. Every
            // other PG operation reaches EnsurePostgreSQLConnection through
            // the session-backed Call constructor; visibility polling must use the same boundary
            // or an NFS metadata request can stay permanently stuck after one
            // real TCP disconnect.
            EnsurePostgreSQLConnection(session);
            if (PQconsumeInput(session->pg) == 0) {
                const std::string message = PQerrorMessage(session->pg);
                DropPostgreSQLConnection(&session->pg);
                session->pg_visibility_resync_required = true;
                session->next_pg_reconnect_attempt = {};
                throw CallError(kPgDatabaseError, message,
                                VEXFS_RUNTIME_BACKEND_POSTGRESQL);
            }
            bool notification = false;
            while (PGnotify *notice = PQnotifies(session->pg)) {
                notification = true;
                PQfreemem(notice);
            }
            const auto now = std::chrono::steady_clock::now();
            const bool local_mutation = session->visibility_initialized &&
                session->mutation_epoch != session->observed_mutation_epoch;
            const bool periodic_poll = now >= session->next_visibility_poll;
            const bool visibility_resync = session->pg_visibility_resync_required;
            int external_workspace_commit = 0;
            if (!session->visibility_initialized || notification || local_mutation ||
                periodic_poll || visibility_resync) {
                Call head(session, "SELECT vexfs_workspace_head(?1)");
                head.Text(1, session->workspace.c_str());
                head.Row();
                Call generation(session, "SELECT vexfs_cache_generation(?1)");
                generation.Text(1, session->workspace.c_str());
                generation.Row();
                const int64_t current_head = head.ResultInt64();
                const uint64_t current_generation = static_cast<uint64_t>(
                    std::max<int64_t>(0, generation.ResultInt64()));
                const bool visibility_changed =
                    current_head != session->observed_workspace_head ||
                    current_generation != session->cache_generation;
                external_workspace_commit = session->visibility_initialized &&
                    (visibility_resync ||
                     (visibility_changed && (notification || !local_mutation))) ? 1 : 0;
                session->observed_workspace_head = current_head;
                session->cache_generation = current_generation;
                session->observed_mutation_epoch = session->mutation_epoch;
                session->visibility_initialized = true;
                session->pg_visibility_resync_required = false;
                session->next_visibility_poll = now + std::chrono::seconds(1);
            }
            visibility->workspace_head = session->observed_workspace_head;
            visibility->cache_generation = session->cache_generation;
            visibility->external_commit = external_workspace_commit;
            return;
#endif
        }
        Call data_version(session, "PRAGMA data_version");
        data_version.Row();
        const int64_t current_data_version = data_version.ResultInt64();
        const bool external_database_commit = session->visibility_initialized &&
            current_data_version != session->observed_data_version;
        const bool local_mutation = session->visibility_initialized &&
            session->mutation_epoch != session->observed_mutation_epoch;
        int external_workspace_commit = 0;
        if (!session->visibility_initialized || external_database_commit || local_mutation) {
            Call head(session,
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

vexfs_mount_status SnapshotCreate(vexfs_mount_session *session, const char *name,
                                  const char *snapshot_type, uint32_t flags,
                                  int64_t *commit, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        if (name == nullptr || snapshot_type == nullptr || commit == nullptr) {
            throw CallError(
                SQLITE_MISUSE,
                "snapshot name, type and commit output are required");
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
            Call synchronize(session,
                "SELECT vexfs_mount_synchronize(?1,'',?2)");
            synchronize.Text(1, session->workspace.c_str());
            synchronize.Text(2, session->session_id.c_str());
            synchronize.Row();
        }
        Call call(session,
            "SELECT vexfs_snapshot_create(?1,?2,NULL,?3,?4)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, name);
        call.Text(3, committed_only ? "committed-only" : "consistent");
        call.Text(4, snapshot_type);
        call.Row();
        *commit = call.ResultInt64();
    });
}

extern "C" vexfs_mount_status vexfs_mount_snapshot_create(
        vexfs_mount_session *session, const char *name, uint32_t flags,
        int64_t *commit, vexfs_mount_error *error) {
    return SnapshotCreate(session, name, "manual", flags, commit, error);
}

extern "C" vexfs_mount_status vexfs_mount_snapshot_create_typed(
        vexfs_mount_session *session, const char *name, const char *snapshot_type,
        uint32_t flags, int64_t *commit, vexfs_mount_error *error) {
    return SnapshotCreate(session, name, snapshot_type, flags, commit, error);
}

extern "C" vexfs_mount_status vexfs_mount_snapshot_create_at_commit(
        vexfs_mount_session *session, const char *name, const char *snapshot_type,
        int64_t source_commit, int64_t *commit, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        if (name == nullptr || snapshot_type == nullptr || commit == nullptr ||
            source_commit <= 0) {
            throw CallError(
                SQLITE_MISUSE,
                "snapshot name, type, positive source commit and output are required");
        }
        if (session->postgresql) {
            throw CallError(
                kUnsupportedBackend,
                "historical commit snapshots are currently supported only by SQLite",
                VEXFS_RUNTIME_BACKEND_POSTGRESQL);
        }
        Call call(session,
            "SELECT vexfs_snapshot_create_at_commit(?1,?2,?3,?4)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, name);
        call.Int64(3, source_commit);
        call.Text(4, snapshot_type);
        call.Row();
        *commit = call.ResultInt64();
    });
}

extern "C" vexfs_mount_status vexfs_mount_snapshot_create_at_time(
        vexfs_mount_session *session, const char *name, const char *snapshot_type,
        const char *requested_at, int64_t *commit, int64_t *commit_created_at,
        vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        if (name == nullptr || snapshot_type == nullptr || requested_at == nullptr ||
            commit == nullptr || commit_created_at == nullptr) {
            throw CallError(
                SQLITE_MISUSE,
                "snapshot name, type, time and commit outputs are required");
        }
        if (session->postgresql) {
            throw CallError(
                kUnsupportedBackend,
                "historical time snapshots are currently supported only by SQLite",
                VEXFS_RUNTIME_BACKEND_POSTGRESQL);
        }
        Call call(session, R"SQL(
WITH created(result) AS MATERIALIZED (
  SELECT vexfs_snapshot_create_at_time(?1,?2,?3,?4)
)
SELECT json_extract(result,'$.commit'),
       json_extract(result,'$.commit_created_at')
FROM created
)SQL");
        call.Text(1, session->workspace.c_str());
        call.Text(2, name);
        call.Text(3, requested_at);
        call.Text(4, snapshot_type);
        call.Row();
        *commit = call.ResultInt64(0);
        *commit_created_at = call.ResultInt64(1);
    });
}

extern "C" vexfs_mount_status vexfs_mount_snapshot_list(vexfs_mount_session *session,
                                                              vexfs_mount_bytes *json,
                                                              vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (json == nullptr) throw CallError(SQLITE_MISUSE, "output is NULL");
        Call call(session, session->postgresql
            ? "SELECT vexfs_snapshot_list_json(?1)"
            : "SELECT vexfs_snapshot_list(?1)");
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
        Call call(session, "SELECT vexfs_snapshot_show(?1,?2)");
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
        Call call(session, "SELECT vexfs_snapshot_diff(?1,?2,?3)");
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
        Call call(session, "SELECT vexfs_snapshot_drop(?1,?2)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, name);
        call.Row();
    });
}

extern "C" vexfs_mount_status vexfs_mount_snapshot_policy_get(
        vexfs_mount_session *session, vexfs_mount_bytes *json,
        vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (json == nullptr) throw CallError(SQLITE_MISUSE, "output is NULL");
        Call call(session, "SELECT vexfs_snapshot_policy_get(?1)");
        call.Text(1, session->workspace.c_str());
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_snapshot_policy_set(
        vexfs_mount_session *session, uint32_t agent_keep,
        uint32_t safety_keep, uint32_t keep_days,
        vexfs_mount_bytes *json, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        if (agent_keep > 1000000 || safety_keep > 1000000 ||
            keep_days > 36500 || json == nullptr) {
            throw CallError(
                SQLITE_RANGE,
                "snapshot keep counts must be at most 1000000 and days at most 36500");
        }
        Call call(session, "SELECT vexfs_snapshot_policy_set(?1,?2,?3,?4)");
        call.Text(1, session->workspace.c_str());
        call.Int64(2, agent_keep);
        call.Int64(3, safety_keep);
        call.Int64(4, keep_days);
        call.Row();
        CopyResult(call, json);
    });
}

extern "C" vexfs_mount_status vexfs_mount_snapshot_prune(
        vexfs_mount_session *session, int dry_run, vexfs_mount_bytes *json,
        vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if ((dry_run != 0 && dry_run != 1) || json == nullptr) {
            throw CallError(SQLITE_RANGE, "dry_run must be 0 or 1 and output is required");
        }
        if (!dry_run) UseFullDurability(session);
        Call call(session, "SELECT vexfs_snapshot_prune(?1,?2)");
        call.Text(1, session->workspace.c_str());
        call.Int64(2, dry_run);
        call.Row();
        CopyResult(call, json);
    });
}

vexfs_mount_status SnapshotRestore(vexfs_mount_session *session, const char *name,
                                   int64_t expected_head, const char *safety_name,
                                   int64_t *new_commit, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseFullDurability(session);
        if (name == nullptr || expected_head <= 0 || new_commit == nullptr) {
            throw CallError(SQLITE_MISUSE,
                            "snapshot name, positive expected head and commit output are required");
        }
        Call call(session, session->postgresql
            ? "SELECT vexfs_snapshot_restore(?1,?2,?3,?4,?5)"
            : "SELECT vexfs_snapshot_restore(?1,?2,?3,?4)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, name);
        call.Int64(3, expected_head);
        if (safety_name == nullptr) call.Null(4); else call.Text(4, safety_name);
        if (session->postgresql) {
            if (session->session_started) call.Text(5, session->session_id.c_str());
            else call.Null(5);
        }
        call.Row();
        *new_commit = call.ResultInt64();
    });
}

extern "C" vexfs_mount_status vexfs_mount_snapshot_restore(vexfs_mount_session *session,
                                                                 const char *name,
                                                                 int64_t expected_head,
                                                                 int64_t *new_commit,
                                                                 vexfs_mount_error *error) {
    return SnapshotRestore(session, name, expected_head, nullptr, new_commit, error);
}

extern "C" vexfs_mount_status vexfs_mount_snapshot_restore_safe(
        vexfs_mount_session *session, const char *name, int64_t expected_head,
        const char *safety_name, int64_t *new_commit, vexfs_mount_error *error) {
    if (safety_name == nullptr || *safety_name == '\0') {
        return SetError(error, SQLITE_MISUSE, "safety snapshot name is required",
                        session != nullptr && session->postgresql
                            ? VEXFS_RUNTIME_BACKEND_POSTGRESQL
                            : VEXFS_RUNTIME_BACKEND_SQLITE);
    }
    return SnapshotRestore(session, name, expected_head, safety_name, new_commit, error);
}

extern "C" vexfs_mount_status vexfs_mount_quota_get(
        vexfs_mount_session *session, vexfs_mount_bytes *json,
        vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (json == nullptr) throw CallError(SQLITE_MISUSE, "output is NULL");
        Call call(session, "SELECT vexfs_quota_get(?1)");
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
        Call call(session, "SELECT vexfs_quota_set(?1,?2,?3,?4)");
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
        Call call(session, "SELECT vexfs_retention_get(?1)");
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
        Call call(session, "SELECT vexfs_retention_set(?1,?2,?3)");
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
        Call call(session, "SELECT vexfs_gc(?1,?2)");
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
        Call call(session, "SELECT vexfs_gc_pause(?1,?2)");
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
        Call call(session, "SELECT vexfs_handle_open(?1,?2,?3,?4,?5)");
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
        Call call(session, "SELECT vexfs_handle_create(?1,?2,?3,?4,?5)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, path);
        call.Int(3, static_cast<int>(mode));
        call.Text(4, EffectiveRequestId(session, request_id, true));
        call.Text(5, session->session_id.c_str());
        call.Row();
        CopyResult(call, handle);
    });
}

extern "C" vexfs_mount_status vexfs_mount_handle_create_owned_durable(
    vexfs_mount_session *session, const char *path, uint32_t mode,
    int64_t uid, int64_t gid, const char *request_id,
    const char *durability, vexfs_mount_bytes *handle,
    vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (path == nullptr || request_id == nullptr || durability == nullptr ||
            handle == nullptr || mode > 07777 || uid < 0 || gid < 0) {
            throw CallError(SQLITE_MISUSE,
                            "path, mode 0..07777, owner, request, durability and handle are required");
        }
        UseRequestedDurability(session, durability);
        Call call(session, "SELECT vexfs_handle_create(?1,?2,?3,?4,?5,?6,?7)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, path);
        call.Int(3, static_cast<int>(mode));
        call.Int64(4, uid);
        call.Int64(5, gid);
        call.Text(6, EffectiveRequestId(session, request_id, true));
        call.Text(7, session->session_id.c_str());
        call.Row();
        CopyResult(call, handle);
    });
}

extern "C" vexfs_mount_status vexfs_mount_handle_create_owned_stat_durable(
    vexfs_mount_session *session, const char *path, uint32_t mode,
    int64_t uid, int64_t gid, const char *request_id,
    const char *durability, vexfs_mount_bytes *handle,
    vexfs_mount_bytes *stat_json, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (path == nullptr || request_id == nullptr || durability == nullptr ||
            handle == nullptr || stat_json == nullptr || mode > 07777 || uid < 0 || gid < 0) {
            throw CallError(
                SQLITE_MISUSE,
                "path, mode 0..07777, owner, request, durability, handle and stat are required");
        }
        UseRequestedDurability(session, durability);
#if defined(VEXFS_HAVE_LIBPQ)
        if (session->postgresql) {
            // The server wrapper runs create and stat as separate PL/pgSQL
            // statements so the STABLE stat function sees the new inode. Both
            // results still travel in one libpq request, removing one WAN RTT.
            Call call(session,
                "SELECT created.handle, created.stat::text "
                "FROM vexfs_handle_create_stat(?1,?2,?3,?4,?5,?6,?7) AS created");
            call.Text(1, session->workspace.c_str());
            call.Text(2, path);
            call.Int(3, static_cast<int>(mode));
            call.Int64(4, uid);
            call.Int64(5, gid);
            call.Text(6, EffectiveRequestId(session, request_id, true));
            call.Text(7, session->session_id.c_str());
            call.Row();
            CopyString(call.ResultText(0), handle);
            CopyString(call.ResultText(1), stat_json);
            return;
        }
#endif
        // SQLite is local, so keep the existing functions as the source of truth.
        // The adapter still exposes the same combined contract to platform code.
        std::string handle_value;
        {
            Call create(session, "SELECT vexfs_handle_create(?1,?2,?3,?4,?5,?6,?7)");
            create.Text(1, session->workspace.c_str());
            create.Text(2, path);
            create.Int(3, static_cast<int>(mode));
            create.Int64(4, uid);
            create.Int64(5, gid);
            create.Text(6, EffectiveRequestId(session, request_id, true));
            create.Text(7, session->session_id.c_str());
            create.Row();
            handle_value = create.ResultText();
        }
        std::string stat_value;
        {
            Call stat(session, "SELECT vexfs_stat(?1,?2)");
            stat.Text(1, session->workspace.c_str());
            stat.Text(2, path);
            stat.Row();
            stat_value = stat.ResultText();
        }
        CopyString(handle_value, handle);
        CopyString(stat_value, stat_json);
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
        Call call(session, "SELECT vexfs_handle_truncate(?1,?2,?3)");
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
        Call call(session, "SELECT vexfs_handle_stage_write(?1,?2,?3,?4)");
        call.Text(1, handle);
        call.Int64(2, static_cast<int64_t>(offset));
        call.Blob(3, data, size);
        call.Text(4, EffectiveRequestId(session, request_id, true));
        call.Row();
        *generation = call.ResultInt64();
    });
}

extern "C" vexfs_mount_status vexfs_mount_handle_stage_write_durable(
    vexfs_mount_session *session, const char *handle, uint64_t offset, const void *data,
    uint64_t size, const char *request_id, const char *durability,
    int64_t *generation, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (handle == nullptr || request_id == nullptr || durability == nullptr ||
            generation == nullptr || (size > 0 && data == nullptr) ||
            offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            throw CallError(SQLITE_MISUSE,
                            "handle, content, request, durability and generation are required");
        }
        UseRequestedDurability(session, durability);
        Call call(session, "SELECT vexfs_handle_stage_write(?1,?2,?3,?4)");
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
        Call call(session, "SELECT vexfs_handle_append(?1,?2,?3)");
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
        Call call(session, "SELECT vexfs_handle_read(?1,?2,?3)");
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
        UseRequestedDurability(session, durability);
        if (version == nullptr) throw CallError(SQLITE_MISUSE, "version output is NULL");
        Call call(session, "SELECT vexfs_handle_publish(?1,?2,?3,?4)");
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
        UseRequestedDurability(session, durability);
        Call call(session, "SELECT vexfs_handle_publish_close(?1,?2,?3)");
        call.Text(1, handle);
        call.Int64(2, generation);
        call.Text(3, durability);
        call.Row();
        *version = call.ResultInt64();
    });
}

extern "C" vexfs_mount_status vexfs_mount_handle_publish_close_background(
    vexfs_mount_session *session, const char *handle, int64_t generation,
    const char *durability, int64_t *version, vexfs_mount_error *error) {
#if defined(VEXFS_HAVE_LIBPQ)
    return GuardPublisher(session, error, [&] {
        if (handle == nullptr || durability == nullptr || version == nullptr) {
            throw CallError(kPgInvalidArgument,
                            "handle, durability and version output are required",
                            VEXFS_RUNTIME_BACKEND_POSTGRESQL);
        }
        Call call(session, "SELECT vexfs_handle_publish_close(?1,?2,?3)", true);
        call.Text(1, handle);
        call.Int64(2, generation);
        call.Text(3, durability);
        call.Row();
        *version = call.ResultInt64();
    });
#else
    (void)session;
    (void)handle;
    (void)generation;
    (void)durability;
    (void)version;
    return SetError(error, kUnsupportedBackend,
                    "background publisher requires PostgreSQL",
                    VEXFS_RUNTIME_BACKEND_POSTGRESQL);
#endif
}

extern "C" vexfs_mount_status vexfs_mount_handle_close(
    vexfs_mount_session *session, const char *handle, int retain_unpublished,
    const char *request_id, vexfs_mount_bytes *state, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseOrdinaryDurability(session);
        Call call(session, "SELECT vexfs_handle_close(?1,?2,?3)");
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
        Call call(session, "SELECT vexfs_mount_synchronize(?1,?2,?3)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, EffectiveRequestId(session, request_id, true));
        call.Text(3, session->session_id.c_str()); call.Row();
        *published = call.ResultInt64();
        if (*published > 0) ++session->mutation_epoch;
        DurabilityBarrier(session);
    });
}

extern "C" vexfs_mount_status vexfs_mount_publish_close_all(
    vexfs_mount_session *session, const char *durability,
    int64_t *published, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (durability == nullptr || published == nullptr) {
            throw CallError(SQLITE_MISUSE,
                            "durability and published output are required");
        }
        UseRequestedDurability(session, durability);
        Call call(session, session->postgresql
            ? "SELECT vexfs_mount_publish_close_all(?1,?2,?3)"
            : "SELECT vexfs_mount_publish_close_all(?1,?2)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, session->session_id.c_str());
        if (session->postgresql) call.Text(3, durability);
        call.Row();
        *published = call.ResultInt64();
        if (*published > 0) ++session->mutation_epoch;
    });
}

extern "C" vexfs_mount_status vexfs_mount_publish_close_batch(
    vexfs_mount_session *session, const char *durability, int64_t max_count,
    int64_t *published, vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        if (durability == nullptr || published == nullptr || max_count <= 0) {
            throw CallError(SQLITE_MISUSE,
                            "durability, a positive max_count and published output are required");
        }
        UseRequestedDurability(session, durability);
        Call call(session, session->postgresql
            ? "SELECT vexfs_mount_publish_close_all(?1,?2,?3,?4)"
            : "SELECT vexfs_mount_publish_close_all(?1,?2,?3)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, session->session_id.c_str());
        if (session->postgresql) {
            call.Text(3, durability);
            call.Int64(4, max_count);
        } else {
            call.Int64(3, max_count);
        }
        call.Row();
        *published = call.ResultInt64();
        if (*published > 0) ++session->mutation_epoch;
    });
}

extern "C" vexfs_mount_status vexfs_mount_publish_close_claimed(
    vexfs_mount_session *session, const char *durability,
    const char *claims_json, vexfs_mount_bytes *result_json,
    vexfs_mount_error *error) {
#if defined(VEXFS_HAVE_LIBPQ)
    return GuardPublisher(session, error, [&] {
        if (durability == nullptr || claims_json == nullptr || result_json == nullptr) {
            throw CallError(kPgInvalidArgument,
                            "durability, claims and result output are required",
                            VEXFS_RUNTIME_BACKEND_POSTGRESQL);
        }
        Call call(session,
            "SELECT vexfs_mount_publish_close_claimed(?1,?2,?3,?4::jsonb)", true);
        call.Text(1, session->workspace.c_str());
        call.Text(2, session->session_id.c_str());
        call.Text(3, durability);
        call.Text(4, claims_json);
        call.Row();
        CopyResult(call, result_json);
    });
#else
    (void)session;
    (void)durability;
    (void)claims_json;
    (void)result_json;
    return SetError(error, kUnsupportedBackend,
                    "background publisher requires PostgreSQL",
                    VEXFS_RUNTIME_BACKEND_POSTGRESQL);
#endif
}

extern "C" vexfs_mount_status vexfs_mount_reclaim(vexfs_mount_session *session,
                                                    const char *request_id,
                                                    int64_t *reclaimed,
                                                    vexfs_mount_error *error) {
    return Guard(session, error, [&] {
        RequireSession(session);
        UseOrdinaryDurability(session);
        if (reclaimed == nullptr) throw CallError(SQLITE_MISUSE, "reclaimed output is NULL");
        Call call(session, "SELECT vexfs_item_reclaim(?1,?2)");
        call.Text(1, session->workspace.c_str());
        call.Text(2, EffectiveRequestId(session, request_id, true));
        call.Row();
        *reclaimed = call.ResultInt64();
    });
}

extern "C" void vexfs_mount_free(void *memory) {
    std::free(memory);
}
