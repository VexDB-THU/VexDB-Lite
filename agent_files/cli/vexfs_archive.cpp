#include "vexfs_archive.h"

#include "vexdb_sqlite.h"
#include "vexfs_checksum.h"

#include "sqlite3.h"

#if defined(VEXFS_HAVE_LIBPQ)
#include <libpq-fe.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vexfs_cli {
namespace {

constexpr int kArchiveFormatVersion = 2;
constexpr int kBlobVerifyChunk = 1024 * 1024;

class Error : public std::runtime_error {
  public:
    explicit Error(const std::string &message) : std::runtime_error(message) {}
};

struct DatabaseCloser {
    void operator()(sqlite3 *db) const {
        if (db != nullptr) sqlite3_close_v2(db);
    }
};
using Database = std::unique_ptr<sqlite3, DatabaseCloser>;

class NewDatabaseCleanup {
  public:
    NewDatabaseCleanup(std::string path, bool active)
        : path_(std::move(path)), active_(active) {}
    ~NewDatabaseCleanup() {
        if (!active_) return;
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_ + "-wal", ignored);
        std::filesystem::remove(path_ + "-shm", ignored);
    }
    void Release() { active_ = false; }

  private:
    std::string path_;
    bool active_ = false;
};

class Statement {
  public:
    Statement(sqlite3 *db, const char *sql) : db_(db) {
        const int rc = sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr);
        if (rc != SQLITE_OK) Throw("prepare", rc);
    }
    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    void Text(int index, const std::string &value) {
        Check("bind text", sqlite3_bind_text(statement_, index, value.data(),
                                              static_cast<int>(value.size()), SQLITE_TRANSIENT));
    }
    void Int64(int index, sqlite3_int64 value) {
        Check("bind integer", sqlite3_bind_int64(statement_, index, value));
    }
    void Blob(int index, const void *value, size_t size) {
        if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
            throw Error("archive record BLOB is too large");
        }
        Check("bind blob", sqlite3_bind_blob(
            statement_, index, size == 0 ? "" : value, static_cast<int>(size),
            SQLITE_TRANSIENT));
    }
    void Null(int index) { Check("bind NULL", sqlite3_bind_null(statement_, index)); }
    bool Row() {
        const int rc = sqlite3_step(statement_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        Throw("step", rc);
        return false;
    }
    void Done() {
        const int rc = sqlite3_step(statement_);
        if (rc != SQLITE_DONE) Throw("step", rc);
    }
    sqlite3_int64 Int64(int column) const { return sqlite3_column_int64(statement_, column); }
    int Int(int column) const { return sqlite3_column_int(statement_, column); }
    int Type(int column) const { return sqlite3_column_type(statement_, column); }
    std::string Text(int column) const {
        const auto *value = sqlite3_column_text(statement_, column);
        const int size = sqlite3_column_bytes(statement_, column);
        return value == nullptr ? std::string() :
            std::string(reinterpret_cast<const char *>(value), size);
    }
    const void *Blob(int column) const { return sqlite3_column_blob(statement_, column); }
    int Bytes(int column) const { return sqlite3_column_bytes(statement_, column); }
    void Reset() {
        Check("reset", sqlite3_reset(statement_));
        Check("clear bindings", sqlite3_clear_bindings(statement_));
    }

  private:
    void Check(const char *action, int rc) {
        if (rc != SQLITE_OK) Throw(action, rc);
    }
    [[noreturn]] void Throw(const char *action, int rc) const {
        throw Error(std::string(action) + ": " + sqlite3_errmsg(db_) +
                    " (rc=" + std::to_string(rc) + ")");
    }
    sqlite3 *db_ = nullptr;
    sqlite3_stmt *statement_ = nullptr;
};

void Exec(sqlite3 *db, const char *sql) {
    char *message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc == SQLITE_OK) return;
    const std::string detail = message == nullptr ? sqlite3_errmsg(db) : message;
    sqlite3_free(message);
    throw Error(detail + " (rc=" + std::to_string(rc) + ")");
}

Database Open(const std::string &path, int flags) {
    sqlite3 *raw = nullptr;
    const int rc = sqlite3_open_v2(path.c_str(), &raw, flags, nullptr);
    Database db(raw);
    if (rc != SQLITE_OK) {
        throw Error("cannot open " + path + ": " +
                    (raw == nullptr ? std::string(sqlite3_errstr(rc)) : sqlite3_errmsg(raw)));
    }
    sqlite3_extended_result_codes(raw, 1);
    sqlite3_busy_timeout(raw, 5000);
    return db;
}

void RegisterVexDB(sqlite3 *db) {
    const int rc = vexdb_sqlite_register(db);
    if (rc != SQLITE_OK) {
        throw Error("cannot register VexDB: " + std::string(sqlite3_errmsg(db)));
    }
}

void HashFunction(sqlite3_context *context, int, sqlite3_value **values) {
    const void *data = sqlite3_value_blob(values[0]);
    const int size = sqlite3_value_bytes(values[0]);
    const std::string result = vexfs::Sha256Hex(size == 0 ? "" : data,
                                                static_cast<size_t>(size));
    sqlite3_result_text(context, result.c_str(), static_cast<int>(result.size()),
                        SQLITE_TRANSIENT);
}

void RegisterHash(sqlite3 *db) {
    const int rc = sqlite3_create_function_v2(db, "vexfs_archive_sha256", 1,
        SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS, nullptr,
        HashFunction, nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) throw Error("cannot register archive checksum function");
}

void Attach(sqlite3 *db, const std::string &path, const char *name) {
    const std::string sql = std::string("ATTACH DATABASE ?1 AS ") + name;
    Statement attach(db, sql.c_str());
    attach.Text(1, path);
    attach.Done();
}

std::string ReadOnlyUri(const std::string &path) {
    const std::string absolute = std::filesystem::absolute(path).generic_string();
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string uri = "file:";
    for (unsigned char value : absolute) {
        const bool safe = (value >= 'a' && value <= 'z') ||
            (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
            value == '/' || value == ':' || value == '-' || value == '_' ||
            value == '.' || value == '~';
        if (safe) uri.push_back(static_cast<char>(value));
        else {
            uri.push_back('%');
            uri.push_back(hex[value >> 4]);
            uri.push_back(hex[value & 15]);
        }
    }
    return uri + "?mode=ro";
}

void AttachReadOnly(sqlite3 *db, const std::string &path, const char *name) {
    Attach(db, ReadOnlyUri(path), name);
}

std::string JsonEscape(const std::string &input) {
    std::string output;
    output.reserve(input.size() + 8);
    for (unsigned char value : input) {
        switch (value) {
            case '\\': output += "\\\\"; break;
            case '"': output += "\\\""; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (value < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    output += "\\u00";
                    output.push_back(hex[value >> 4]);
                    output.push_back(hex[value & 15]);
                } else {
                    output.push_back(static_cast<char>(value));
                }
        }
    }
    return output;
}

sqlite3_int64 ScalarInt64(sqlite3 *db, const char *sql) {
    Statement statement(db, sql);
    if (!statement.Row()) throw Error("query returned no row");
    return statement.Int64(0);
}

std::string ComputePackageChecksum(sqlite3 *db) {
    vexfs::Sha256 hash;
    {
        Statement manifest(db, R"SQL(
SELECT json_array(format_version,source_engine,source_workspace,source_commit,
 source_snapshot,root_source_inode,history_floor_source_commit,retention_keep_versions,
 retention_keep_days,quota_max_bytes,quota_max_files,quota_max_file_bytes,created_at,complete)
FROM package.manifest
)SQL");
        if (!manifest.Row()) throw Error("archive manifest is missing");
        const std::string metadata = manifest.Text(0);
        hash.Update("manifest\0", 9);
        hash.Update(metadata.data(), metadata.size());
        hash.Update("\n", 1);
        if (manifest.Row()) throw Error("archive has more than one manifest row");
    }
    constexpr std::array<const char *, 14> tables = {
        "commits", "inodes", "dentries", "file_versions", "manifests", "chunks", "inode_states",
        "dentry_states", "xattr_states", "acl_states", "snapshots", "xattrs",
        "acl_entries", "principals"
    };
    for (const char *table : tables) {
        const std::string sql = "SELECT record_key,record_checksum FROM package." +
            std::string(table) + " ORDER BY record_key";
        Statement rows(db, sql.c_str());
        while (rows.Row()) {
            const std::string key = rows.Text(0);
            const std::string checksum = rows.Text(1);
            hash.Update(table, std::strlen(table));
            hash.Update("\0", 1);
            hash.Update(key.data(), key.size());
            hash.Update("\0", 1);
            hash.Update(checksum.data(), checksum.size());
            hash.Update("\n", 1);
        }
    }
    return vexfs::Hex(hash.Finish());
}

void CreateArchiveSchema(sqlite3 *db) {
    Exec(db, R"SQL(
CREATE TABLE package.manifest(
 format_version INTEGER NOT NULL,source_engine TEXT NOT NULL,source_workspace TEXT NOT NULL,
 source_commit INTEGER NOT NULL,source_snapshot TEXT,root_source_inode INTEGER NOT NULL,
 history_floor_source_commit INTEGER NOT NULL,retention_keep_versions INTEGER NOT NULL,
 retention_keep_days INTEGER NOT NULL,quota_max_bytes INTEGER,quota_max_files INTEGER,
 quota_max_file_bytes INTEGER,created_at INTEGER NOT NULL,package_checksum TEXT,
 complete INTEGER NOT NULL CHECK(complete IN (0,1)));
CREATE TABLE package.commits(
 record_key TEXT PRIMARY KEY,source_id INTEGER UNIQUE NOT NULL,parent_source_id INTEGER,
 message TEXT NOT NULL,created_at INTEGER NOT NULL,record_checksum TEXT NOT NULL);
CREATE TABLE package.inodes(
 record_key TEXT PRIMARY KEY,source_id INTEGER UNIQUE NOT NULL,kind TEXT NOT NULL,mode INTEGER NOT NULL,
 owner_principal TEXT NOT NULL,uid INTEGER NOT NULL,gid INTEGER NOT NULL,size INTEGER NOT NULL,
 current_version INTEGER NOT NULL,created_at INTEGER NOT NULL,accessed_at INTEGER NOT NULL,
 updated_at INTEGER NOT NULL,changed_at INTEGER NOT NULL,deleted_at INTEGER,
 record_checksum TEXT NOT NULL);
CREATE TABLE package.dentries(
 record_key TEXT PRIMARY KEY,parent_source_inode INTEGER NOT NULL,name TEXT NOT NULL,
 inode_source_id INTEGER NOT NULL,record_checksum TEXT NOT NULL);
CREATE TABLE package.file_versions(
 id INTEGER PRIMARY KEY,record_key TEXT UNIQUE NOT NULL,source_inode INTEGER NOT NULL,
 version_no INTEGER NOT NULL,source_commit INTEGER NOT NULL,source_manifest INTEGER,
 size INTEGER NOT NULL,checksum TEXT NOT NULL,source_version_no INTEGER,created_at INTEGER NOT NULL,
 record_checksum TEXT NOT NULL,UNIQUE(source_inode,version_no));
CREATE TABLE package.manifests(
 id INTEGER PRIMARY KEY,record_key TEXT UNIQUE NOT NULL,source_id INTEGER UNIQUE NOT NULL,
 file_size INTEGER NOT NULL,chunk_size INTEGER NOT NULL,chunk_count INTEGER NOT NULL,
 checksum TEXT NOT NULL,created_at INTEGER NOT NULL,record_checksum TEXT NOT NULL);
CREATE TABLE package.chunks(
 id INTEGER PRIMARY KEY,record_key TEXT UNIQUE NOT NULL,source_manifest INTEGER NOT NULL,
 chunk_no INTEGER NOT NULL,content BLOB NOT NULL,size INTEGER NOT NULL,checksum TEXT NOT NULL,
 record_checksum TEXT NOT NULL,UNIQUE(source_manifest,chunk_no));
CREATE TABLE package.inode_states(
 record_key TEXT PRIMARY KEY,source_inode INTEGER NOT NULL,source_commit INTEGER NOT NULL,
 kind TEXT NOT NULL,mode INTEGER NOT NULL,owner_principal TEXT NOT NULL,uid INTEGER NOT NULL,
 gid INTEGER NOT NULL,size INTEGER NOT NULL,current_version INTEGER NOT NULL,
 created_at INTEGER NOT NULL,accessed_at INTEGER NOT NULL,updated_at INTEGER NOT NULL,
 changed_at INTEGER NOT NULL,deleted_at INTEGER,record_checksum TEXT NOT NULL);
CREATE TABLE package.dentry_states(
 record_key TEXT PRIMARY KEY,parent_source_inode INTEGER NOT NULL,name TEXT NOT NULL,
 source_commit INTEGER NOT NULL,inode_source_id INTEGER NOT NULL,deleted INTEGER NOT NULL,
 record_checksum TEXT NOT NULL);
CREATE TABLE package.xattr_states(
 record_key TEXT PRIMARY KEY,source_inode INTEGER NOT NULL,name TEXT NOT NULL,
 source_commit INTEGER NOT NULL,value BLOB NOT NULL,deleted INTEGER NOT NULL,
 record_checksum TEXT NOT NULL);
CREATE TABLE package.acl_states(
 record_key TEXT PRIMARY KEY,source_inode INTEGER NOT NULL,principal_id TEXT NOT NULL,
 effect TEXT NOT NULL,source_commit INTEGER NOT NULL,permissions TEXT NOT NULL,
 inherit_flags INTEGER NOT NULL,deleted INTEGER NOT NULL,record_checksum TEXT NOT NULL);
CREATE TABLE package.snapshots(
 record_key TEXT PRIMARY KEY,name TEXT UNIQUE NOT NULL,source_commit INTEGER NOT NULL,
 created_at INTEGER NOT NULL,record_checksum TEXT NOT NULL);
CREATE TABLE package.xattrs(
 record_key TEXT PRIMARY KEY,source_inode INTEGER NOT NULL,name TEXT NOT NULL,value BLOB NOT NULL,
 updated_at INTEGER NOT NULL,record_checksum TEXT NOT NULL);
CREATE TABLE package.acl_entries(
 record_key TEXT PRIMARY KEY,source_inode INTEGER NOT NULL,principal_id TEXT NOT NULL,
 effect TEXT NOT NULL,permissions TEXT NOT NULL,inherit_flags INTEGER NOT NULL,
 created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL,record_checksum TEXT NOT NULL);
CREATE TABLE package.principals(
 record_key TEXT PRIMARY KEY,source_principal TEXT UNIQUE NOT NULL,
 record_checksum TEXT NOT NULL);
)SQL");
}

void CopyArchiveChunks(sqlite3 *db) {
    struct BlobCopy {
        sqlite3_int64 package_rowid;
        sqlite3_int64 source_rowid;
        sqlite3_int64 size;
        std::string checksum;
    };
    std::vector<BlobCopy> copies;
    {
        Statement rows(db, R"SQL(
SELECT package_chunk.id,source_chunk.rowid,package_chunk.size,package_chunk.checksum
FROM package.chunks package_chunk
JOIN _vexfs_manifest_chunks source_entry
  ON source_entry.manifest_id=package_chunk.source_manifest
 AND source_entry.chunk_no=package_chunk.chunk_no
JOIN _vexfs_chunks source_chunk ON source_chunk.id=source_entry.chunk_id
ORDER BY package_chunk.id
)SQL");
        while (rows.Row()) {
            copies.push_back({rows.Int64(0), rows.Int64(1), rows.Int64(2), rows.Text(3)});
        }
    }
    std::vector<unsigned char> buffer(kBlobVerifyChunk);
    for (const BlobCopy &copy : copies) {
        const sqlite3_int64 package_rowid = copy.package_rowid;
        const sqlite3_int64 source_rowid = copy.source_rowid;
        const sqlite3_int64 expected_size = copy.size;
        const std::string &expected_checksum = copy.checksum;
        if (expected_size < 0 || expected_size > std::numeric_limits<int>::max()) {
            throw Error("source chunk size is outside archive format limits");
        }
        sqlite3_blob *raw_source = nullptr;
        sqlite3_blob *raw_target = nullptr;
        if (sqlite3_blob_open(db, "main", "_vexfs_chunks", "content",
                              source_rowid, 0, &raw_source) != SQLITE_OK) {
            throw Error("cannot open source chunk BLOB");
        }
        std::unique_ptr<sqlite3_blob, decltype(&sqlite3_blob_close)> source(
            raw_source, sqlite3_blob_close);
        if (sqlite3_blob_open(db, "package", "chunks", "content",
                              package_rowid, 1, &raw_target) != SQLITE_OK) {
            throw Error("cannot open archive chunk BLOB");
        }
        std::unique_ptr<sqlite3_blob, decltype(&sqlite3_blob_close)> target(
            raw_target, sqlite3_blob_close);
        const int bytes = sqlite3_blob_bytes(source.get());
        if (bytes != expected_size || sqlite3_blob_bytes(target.get()) != bytes) {
            throw Error("source chunk size does not match metadata");
        }
        vexfs::Sha256 hash;
        for (int offset = 0; offset < bytes;) {
            const int amount = std::min<int>(static_cast<int>(buffer.size()), bytes - offset);
            if (sqlite3_blob_read(source.get(), buffer.data(), amount, offset) != SQLITE_OK ||
                sqlite3_blob_write(target.get(), buffer.data(), amount, offset) != SQLITE_OK) {
                throw Error("cannot stream archive chunk BLOB");
            }
            hash.Update(buffer.data(), static_cast<size_t>(amount));
            offset += amount;
        }
        if (vexfs::Hex(hash.Finish()) != expected_checksum) {
            throw Error("source chunk checksum mismatch during export");
        }
    }
}

void CopyImportedChunks(sqlite3 *db) {
    struct BlobCopy {
        sqlite3_int64 target_rowid;
        sqlite3_int64 package_rowid;
        sqlite3_int64 size;
        std::string checksum;
    };
    std::vector<BlobCopy> copies;
    {
        Statement rows(db, R"SQL(
SELECT target.rowid,package_chunk.id,package_chunk.size,package_chunk.checksum
FROM package.chunks package_chunk
JOIN import_chunk_map mapped
  ON mapped.source_manifest=package_chunk.source_manifest
 AND mapped.chunk_no=package_chunk.chunk_no
JOIN _vexfs_chunks target ON target.id=mapped.local_id
ORDER BY package_chunk.id
)SQL");
        while (rows.Row()) {
            copies.push_back({rows.Int64(0), rows.Int64(1), rows.Int64(2), rows.Text(3)});
        }
    }
    std::vector<unsigned char> buffer(kBlobVerifyChunk);
    for (const BlobCopy &copy : copies) {
        sqlite3_blob *raw_source = nullptr;
        sqlite3_blob *raw_target = nullptr;
        if (sqlite3_blob_open(db, "package", "chunks", "content",
                              copy.package_rowid, 0, &raw_source) != SQLITE_OK) {
            throw Error("cannot open package chunk BLOB during import");
        }
        std::unique_ptr<sqlite3_blob, decltype(&sqlite3_blob_close)> source(
            raw_source, sqlite3_blob_close);
        if (sqlite3_blob_open(db, "main", "_vexfs_chunks", "content",
                              copy.target_rowid, 1, &raw_target) != SQLITE_OK) {
            throw Error("cannot open target chunk BLOB during import");
        }
        std::unique_ptr<sqlite3_blob, decltype(&sqlite3_blob_close)> target(
            raw_target, sqlite3_blob_close);
        const int bytes = sqlite3_blob_bytes(source.get());
        if (copy.size < 0 || copy.size > std::numeric_limits<int>::max() ||
            bytes != copy.size || sqlite3_blob_bytes(target.get()) != bytes) {
            throw Error("package chunk size changed during import");
        }
        vexfs::Sha256 hash;
        for (int offset = 0; offset < bytes;) {
            const int amount = std::min<int>(static_cast<int>(buffer.size()), bytes - offset);
            if (sqlite3_blob_read(source.get(), buffer.data(), amount, offset) != SQLITE_OK ||
                sqlite3_blob_write(target.get(), buffer.data(), amount, offset) != SQLITE_OK) {
                throw Error("cannot stream package chunk BLOB during import");
            }
            hash.Update(buffer.data(), static_cast<size_t>(amount));
            offset += amount;
        }
        if (vexfs::Hex(hash.Finish()) != copy.checksum) {
            throw Error("package chunk checksum changed during import");
        }
    }
}

#if defined(VEXFS_HAVE_LIBPQ)

struct PgConnectionCloser {
    void operator()(PGconn *connection) const {
        if (connection != nullptr) PQfinish(connection);
    }
};
using PgConnection = std::unique_ptr<PGconn, PgConnectionCloser>;

struct PgResultCloser {
    void operator()(PGresult *result) const {
        if (result != nullptr) PQclear(result);
    }
};
using PgResult = std::unique_ptr<PGresult, PgResultCloser>;

PgConnection OpenPostgres(const std::string &dsn) {
    PGconn *raw = PQconnectdb(dsn.c_str());
    PgConnection connection(raw);
    if (raw == nullptr || PQstatus(raw) != CONNECTION_OK) {
        throw Error("cannot connect to PostgreSQL: " +
                    std::string(raw == nullptr ? "out of memory" : PQerrorMessage(raw)));
    }
    return connection;
}

void PgCommand(PGconn *connection, const char *sql) {
    PgResult result(PQexec(connection, sql));
    if (!result || PQresultStatus(result.get()) != PGRES_COMMAND_OK) {
        throw Error("PostgreSQL command failed: " + std::string(PQerrorMessage(connection)));
    }
}

std::string PgScalar(PGconn *connection, const char *sql, int parameter_count,
                     const char *const *values, const int *lengths = nullptr,
                     const int *formats = nullptr) {
    PgResult result(PQexecParams(
        connection, sql, parameter_count, nullptr, values, lengths, formats, 0));
    if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK ||
        PQntuples(result.get()) != 1 || PQnfields(result.get()) != 1 ||
        PQgetisnull(result.get(), 0, 0)) {
        throw Error("PostgreSQL query failed: " + std::string(PQerrorMessage(connection)));
    }
    return std::string(PQgetvalue(result.get(), 0, 0),
                       static_cast<size_t>(PQgetlength(result.get(), 0, 0)));
}

struct PgArchiveExportResult {
    sqlite3_int64 source_commit = 0;
    std::string checksum;
};

PgArchiveExportResult ExportPostgresRecords(
    const std::string &dsn, const std::string &workspace,
    const std::string &snapshot, const std::string &output) {
    if (dsn.empty()) throw Error("PostgreSQL export needs a connection DSN");
    if (workspace.empty()) throw Error("PostgreSQL export needs a workspace name");
    if (output.empty()) throw Error("export output path is empty");
    if (std::filesystem::exists(output)) {
        throw Error("export output already exists: " + output);
    }

    bool owns_output = false;
    try {
        {
            Database created = Open(output, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                            SQLITE_OPEN_EXCLUSIVE);
            Exec(created.get(), "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;");
        }
        owns_output = true;
        std::filesystem::permissions(
            output, std::filesystem::perms::owner_read |
                    std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);

        Database package = Open(":memory:", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                             SQLITE_OPEN_URI);
        RegisterHash(package.get());
        Attach(package.get(), output, "package");
        Exec(package.get(), "BEGIN IMMEDIATE");
        CreateArchiveSchema(package.get());
        Exec(package.get(), R"SQL(
CREATE TEMP TABLE incoming(
 record_type TEXT NOT NULL,record_key TEXT NOT NULL,record_json TEXT NOT NULL,
 PRIMARY KEY(record_type,record_key));
)SQL");
        Statement incoming(package.get(),
            "INSERT INTO incoming(record_type,record_key,record_json) VALUES(?1,?2,?3)");
        Statement chunk(package.get(), R"SQL(
INSERT INTO package.chunks(
 record_key,source_manifest,chunk_no,content,size,checksum,record_checksum)
VALUES(?1,json_extract(?2,'$.source_manifest'),json_extract(?2,'$.chunk_no'),?3,
       json_extract(?2,'$.size'),json_extract(?2,'$.checksum'),'')
)SQL");

        PgConnection connection = OpenPostgres(dsn);
        PgCommand(connection.get(), "BEGIN ISOLATION LEVEL REPEATABLE READ");
        PgCommand(connection.get(), "SET LOCAL client_min_messages=warning");
        const char *values[2] = {workspace.c_str(), snapshot.empty() ? nullptr : snapshot.c_str()};
        if (PQsendQueryParams(connection.get(),
                "SELECT record_type,record_key,record_json::text,content "
                "FROM public.vexfs_archive_export_records($1,$2)",
                2, nullptr, values, nullptr, nullptr, 0) != 1) {
            throw Error("cannot start PostgreSQL archive stream: " +
                        std::string(PQerrorMessage(connection.get())));
        }
        if (PQsetSingleRowMode(connection.get()) != 1) {
            throw Error("cannot enable PostgreSQL single-row archive streaming");
        }
        bool completed = false;
        while (PGresult *raw = PQgetResult(connection.get())) {
            PgResult row(raw);
            const ExecStatusType status = PQresultStatus(raw);
            if (status == PGRES_TUPLES_OK) {
                completed = true;
                continue;
            }
            if (status != PGRES_SINGLE_TUPLE || PQntuples(raw) != 1 || PQnfields(raw) != 4) {
                throw Error("PostgreSQL archive stream failed: " +
                            std::string(PQresultErrorMessage(raw)));
            }
            const std::string type(PQgetvalue(raw, 0, 0),
                                   static_cast<size_t>(PQgetlength(raw, 0, 0)));
            const std::string key(PQgetvalue(raw, 0, 1),
                                  static_cast<size_t>(PQgetlength(raw, 0, 1)));
            const std::string json(PQgetvalue(raw, 0, 2),
                                   static_cast<size_t>(PQgetlength(raw, 0, 2)));
            if (type == "chunks") {
                if (PQgetisnull(raw, 0, 3)) throw Error("PostgreSQL chunk content is NULL");
                size_t decoded_size = 0;
                unsigned char *decoded = PQunescapeBytea(
                    reinterpret_cast<const unsigned char *>(PQgetvalue(raw, 0, 3)),
                    &decoded_size);
                if (decoded == nullptr) throw Error("cannot decode PostgreSQL chunk content");
                try {
                    chunk.Text(1, key);
                    chunk.Text(2, json);
                    chunk.Blob(3, decoded, decoded_size);
                    chunk.Done();
                    chunk.Reset();
                } catch (...) {
                    PQfreemem(decoded);
                    throw;
                }
                PQfreemem(decoded);
            } else {
                incoming.Text(1, type);
                incoming.Text(2, key);
                incoming.Text(3, json);
                incoming.Done();
                incoming.Reset();
            }
        }
        if (!completed) {
            throw Error("PostgreSQL archive stream ended without completion");
        }
        PgCommand(connection.get(), "COMMIT");

        Exec(package.get(), R"SQL(
INSERT INTO package.manifest(
 format_version,source_engine,source_workspace,source_commit,source_snapshot,
 root_source_inode,history_floor_source_commit,retention_keep_versions,
 retention_keep_days,quota_max_bytes,quota_max_files,quota_max_file_bytes,
 created_at,package_checksum,complete)
SELECT json_extract(record_json,'$.format_version'),
       json_extract(record_json,'$.source_engine'),
       json_extract(record_json,'$.source_workspace'),
       json_extract(record_json,'$.source_commit'),
       json_extract(record_json,'$.source_snapshot'),
       json_extract(record_json,'$.root_source_inode'),
       json_extract(record_json,'$.history_floor_source_commit'),
       json_extract(record_json,'$.retention_keep_versions'),
       json_extract(record_json,'$.retention_keep_days'),
       json_extract(record_json,'$.quota_max_bytes'),
       json_extract(record_json,'$.quota_max_files'),
       json_extract(record_json,'$.quota_max_file_bytes'),
       json_extract(record_json,'$.created_at'),NULL,0
FROM incoming WHERE record_type='manifest';

INSERT INTO package.commits(
 record_key,source_id,parent_source_id,message,created_at,record_checksum)
SELECT record_key,json_extract(record_json,'$.source_id'),
       json_extract(record_json,'$.parent_source_id'),json_extract(record_json,'$.message'),
       json_extract(record_json,'$.created_at'),''
FROM incoming WHERE record_type='commits';

INSERT INTO package.inodes(
 record_key,source_id,kind,mode,owner_principal,uid,gid,size,current_version,
 created_at,accessed_at,updated_at,changed_at,deleted_at,record_checksum)
SELECT record_key,json_extract(record_json,'$.source_id'),json_extract(record_json,'$.kind'),
       json_extract(record_json,'$.mode'),json_extract(record_json,'$.owner_principal'),
       json_extract(record_json,'$.uid'),json_extract(record_json,'$.gid'),
       json_extract(record_json,'$.size'),json_extract(record_json,'$.current_version'),
       json_extract(record_json,'$.created_at'),json_extract(record_json,'$.accessed_at'),
       json_extract(record_json,'$.updated_at'),json_extract(record_json,'$.changed_at'),
       json_extract(record_json,'$.deleted_at'),''
FROM incoming WHERE record_type='inodes';

INSERT INTO package.dentries(
 record_key,parent_source_inode,name,inode_source_id,record_checksum)
SELECT record_key,json_extract(record_json,'$.parent_source_inode'),
       json_extract(record_json,'$.name'),json_extract(record_json,'$.inode_source_id'),''
FROM incoming WHERE record_type='dentries';

INSERT INTO package.file_versions(
 record_key,source_inode,version_no,source_commit,source_manifest,size,checksum,
 source_version_no,created_at,record_checksum)
SELECT record_key,json_extract(record_json,'$.source_inode'),
       json_extract(record_json,'$.version_no'),json_extract(record_json,'$.source_commit'),
       json_extract(record_json,'$.source_manifest'),json_extract(record_json,'$.size'),
       json_extract(record_json,'$.checksum'),json_extract(record_json,'$.source_version_no'),
       json_extract(record_json,'$.created_at'),''
FROM incoming WHERE record_type='file_versions';

INSERT INTO package.manifests(
 record_key,source_id,file_size,chunk_size,chunk_count,checksum,created_at,record_checksum)
SELECT record_key,json_extract(record_json,'$.source_id'),
       json_extract(record_json,'$.file_size'),json_extract(record_json,'$.chunk_size'),
       json_extract(record_json,'$.chunk_count'),json_extract(record_json,'$.checksum'),
       json_extract(record_json,'$.created_at'),''
FROM incoming WHERE record_type='manifests';

INSERT INTO package.inode_states(
 record_key,source_inode,source_commit,kind,mode,owner_principal,uid,gid,size,
 current_version,created_at,accessed_at,updated_at,changed_at,deleted_at,record_checksum)
SELECT record_key,json_extract(record_json,'$.source_inode'),
       json_extract(record_json,'$.source_commit'),json_extract(record_json,'$.kind'),
       json_extract(record_json,'$.mode'),json_extract(record_json,'$.owner_principal'),
       json_extract(record_json,'$.uid'),json_extract(record_json,'$.gid'),
       json_extract(record_json,'$.size'),json_extract(record_json,'$.current_version'),
       json_extract(record_json,'$.created_at'),json_extract(record_json,'$.accessed_at'),
       json_extract(record_json,'$.updated_at'),json_extract(record_json,'$.changed_at'),
       json_extract(record_json,'$.deleted_at'),''
FROM incoming WHERE record_type='inode_states';

INSERT INTO package.dentry_states(
 record_key,parent_source_inode,name,source_commit,inode_source_id,deleted,record_checksum)
SELECT record_key,json_extract(record_json,'$.parent_source_inode'),
       json_extract(record_json,'$.name'),json_extract(record_json,'$.source_commit'),
       json_extract(record_json,'$.inode_source_id'),json_extract(record_json,'$.deleted'),''
FROM incoming WHERE record_type='dentry_states';

INSERT INTO package.xattr_states(
 record_key,source_inode,name,source_commit,value,deleted,record_checksum)
SELECT record_key,json_extract(record_json,'$.source_inode'),json_extract(record_json,'$.name'),
       json_extract(record_json,'$.source_commit'),
       unhex(json_extract(record_json,'$.value_hex')),
       json_extract(record_json,'$.deleted'),''
FROM incoming WHERE record_type='xattr_states';

INSERT INTO package.acl_states(
 record_key,source_inode,principal_id,effect,source_commit,permissions,
 inherit_flags,deleted,record_checksum)
SELECT record_key,json_extract(record_json,'$.source_inode'),
       json_extract(record_json,'$.principal_id'),json_extract(record_json,'$.effect'),
       json_extract(record_json,'$.source_commit'),json_extract(record_json,'$.permissions'),
       json_extract(record_json,'$.inherit_flags'),json_extract(record_json,'$.deleted'),''
FROM incoming WHERE record_type='acl_states';

INSERT INTO package.snapshots(record_key,name,source_commit,created_at,record_checksum)
SELECT record_key,json_extract(record_json,'$.name'),
       json_extract(record_json,'$.source_commit'),json_extract(record_json,'$.created_at'),''
FROM incoming WHERE record_type='snapshots';

INSERT INTO package.xattrs(record_key,source_inode,name,value,updated_at,record_checksum)
SELECT record_key,json_extract(record_json,'$.source_inode'),json_extract(record_json,'$.name'),
       unhex(json_extract(record_json,'$.value_hex')),
       json_extract(record_json,'$.updated_at'),''
FROM incoming WHERE record_type='xattrs';

INSERT INTO package.acl_entries(
 record_key,source_inode,principal_id,effect,permissions,inherit_flags,
 created_at,updated_at,record_checksum)
SELECT record_key,json_extract(record_json,'$.source_inode'),
       json_extract(record_json,'$.principal_id'),json_extract(record_json,'$.effect'),
       json_extract(record_json,'$.permissions'),json_extract(record_json,'$.inherit_flags'),
       json_extract(record_json,'$.created_at'),json_extract(record_json,'$.updated_at'),''
FROM incoming WHERE record_type='acl_entries';

INSERT INTO package.principals(record_key,source_principal,record_checksum)
SELECT record_key,json_extract(record_json,'$.source_principal'),''
FROM incoming WHERE record_type='principals';

UPDATE package.commits SET record_checksum=
 vexfs_archive_sha256(CAST(json_array(source_id,parent_source_id,message,created_at) AS BLOB));
UPDATE package.inodes SET record_checksum=
 vexfs_archive_sha256(CAST(json_array(source_id,kind,mode,owner_principal,uid,gid,size,
  current_version,created_at,accessed_at,updated_at,changed_at,deleted_at) AS BLOB));
UPDATE package.dentries SET record_checksum=
 vexfs_archive_sha256(CAST(json_array(parent_source_inode,name,inode_source_id) AS BLOB));
UPDATE package.file_versions SET record_checksum=
 vexfs_archive_sha256(CAST(json_array(source_inode,version_no,source_commit,source_manifest,
  size,checksum,source_version_no,created_at) AS BLOB));
UPDATE package.manifests SET record_checksum=
 vexfs_archive_sha256(CAST(json_array(source_id,file_size,chunk_size,chunk_count,checksum,
  created_at) AS BLOB));
UPDATE package.chunks SET record_checksum=
 vexfs_archive_sha256(CAST(json_array(source_manifest,chunk_no,size,checksum) AS BLOB));
UPDATE package.inode_states SET record_checksum=
 vexfs_archive_sha256(CAST(json_array(source_inode,source_commit,kind,mode,owner_principal,
  uid,gid,size,current_version,created_at,accessed_at,updated_at,changed_at,deleted_at) AS BLOB));
UPDATE package.dentry_states SET record_checksum=
 vexfs_archive_sha256(CAST(json_array(parent_source_inode,name,source_commit,inode_source_id,
  deleted) AS BLOB));
UPDATE package.xattr_states SET record_checksum=
 vexfs_archive_sha256(CAST(json_array(source_inode,name,source_commit,hex(value),deleted) AS BLOB));
UPDATE package.acl_states SET record_checksum=
 vexfs_archive_sha256(CAST(json_array(source_inode,principal_id,effect,source_commit,
  permissions,inherit_flags,deleted) AS BLOB));
UPDATE package.snapshots SET record_checksum=
 vexfs_archive_sha256(CAST(json_array(name,source_commit,created_at) AS BLOB));
UPDATE package.xattrs SET record_checksum=
 vexfs_archive_sha256(CAST(json_array(source_inode,name,hex(value),updated_at) AS BLOB));
UPDATE package.acl_entries SET record_checksum=
 vexfs_archive_sha256(CAST(json_array(source_inode,principal_id,effect,permissions,
  inherit_flags,created_at,updated_at) AS BLOB));
UPDATE package.principals SET record_checksum=
 vexfs_archive_sha256(CAST(json_array(source_principal) AS BLOB));
)SQL");
        Exec(package.get(), "UPDATE package.manifest SET complete=1");
        const std::string checksum = ComputePackageChecksum(package.get());
        {
            Statement finish(package.get(),
                "UPDATE package.manifest SET package_checksum=?1");
            finish.Text(1, checksum);
            finish.Done();
        }
        const sqlite3_int64 source_commit = ScalarInt64(
            package.get(), "SELECT source_commit FROM package.manifest");
        Exec(package.get(), "COMMIT");
        return {source_commit, checksum};
    } catch (...) {
        if (owns_output) {
            std::error_code ignored;
            std::filesystem::remove(output, ignored);
        }
        throw;
    }
}

#endif  // VEXFS_HAVE_LIBPQ

}  // namespace

std::string ExportArchive(const std::string &database, const std::string &workspace,
                          const std::string &snapshot, const std::string &output) {
    if (database.empty() || database == ":memory:") {
        throw Error("export needs a persistent SQLite database path");
    }
    if (!std::filesystem::is_regular_file(database)) {
        throw Error("source database file not found: " + database);
    }
    if (output.empty()) throw Error("export output path is empty");
    if (std::filesystem::exists(output)) {
        throw Error("export output already exists: " + output);
    }

    // SQLite applies the connection open mode to newly attached databases. Open the
    // source read-write so the new package can be created; the source stays in a read
    // transaction and is never modified by the export statements below.
    Database db = Open(database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI);
    RegisterVexDB(db.get());
    RegisterHash(db.get());
    bool owns_output = false;
    try {
        {
            Database package = Open(output, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                            SQLITE_OPEN_EXCLUSIVE);
            Exec(package.get(), "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;");
        }
        owns_output = true;
        std::filesystem::permissions(
            output, std::filesystem::perms::owner_read |
                    std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);
        Exec(db.get(), "BEGIN");
        Statement source(db.get(), R"SQL(
SELECT id,root_inode,head_commit,history_floor_commit,retention_keep_versions,
       retention_keep_days,quota_max_bytes,quota_max_files,quota_max_file_bytes
FROM _vexfs_workspaces WHERE name=?1 AND state='active'
)SQL");
        source.Text(1, workspace);
        if (!source.Row()) throw Error("workspace not found: " + workspace);
        const sqlite3_int64 workspace_id = source.Int64(0);
        const sqlite3_int64 root_inode = source.Int64(1);
        sqlite3_int64 source_commit = source.Int64(2);
        const sqlite3_int64 history_floor = source.Int64(3);
        const sqlite3_int64 keep_versions = source.Int64(4);
        const sqlite3_int64 keep_days = source.Int64(5);
        const bool max_bytes_null = source.Type(6) == SQLITE_NULL;
        const bool max_files_null = source.Type(7) == SQLITE_NULL;
        const bool max_file_bytes_null = source.Type(8) == SQLITE_NULL;
        const sqlite3_int64 max_bytes = source.Int64(6);
        const sqlite3_int64 max_files = source.Int64(7);
        const sqlite3_int64 max_file_bytes = source.Int64(8);

        if (!snapshot.empty() && snapshot != "HEAD") {
            Statement selected(db.get(),
                "SELECT commit_id FROM _vexfs_snapshots WHERE workspace_id=?1 AND name=?2");
            selected.Int64(1, workspace_id);
            selected.Text(2, snapshot);
            if (!selected.Row()) throw Error("snapshot not found: " + snapshot);
            source_commit = selected.Int64(0);
        } else {
            Statement dirty(db.get(),
                "SELECT 1 FROM _vexfs_handles WHERE workspace_id=?1 "
                "AND state IN ('open','retained') "
                "AND dirty_generation>published_generation LIMIT 1");
            dirty.Int64(1, workspace_id);
            if (dirty.Row()) {
                throw Error("workspace has unpublished file handles; synchronize or export "
                            "an existing snapshot");
            }
        }
        if (source_commit <= 0 || history_floor <= 0 || source_commit < history_floor) {
            throw Error("selected commit is outside restorable workspace history");
        }

        Attach(db.get(), output, "package");
        CreateArchiveSchema(db.get());
        {
            Statement manifest(db.get(), R"SQL(
INSERT INTO package.manifest(
 format_version,source_engine,source_workspace,source_commit,source_snapshot,
 root_source_inode,history_floor_source_commit,retention_keep_versions,
 retention_keep_days,quota_max_bytes,quota_max_files,quota_max_file_bytes,
 created_at,package_checksum,complete)
VALUES(?1,'sqlite',?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,
       CAST(unixepoch('subsec')*1000 AS INTEGER),NULL,0)
)SQL");
            manifest.Int64(1, kArchiveFormatVersion);
            manifest.Text(2, workspace);
            manifest.Int64(3, source_commit);
            if (snapshot.empty() || snapshot == "HEAD") manifest.Null(4);
            else manifest.Text(4, snapshot);
            manifest.Int64(5, root_inode);
            manifest.Int64(6, history_floor);
            manifest.Int64(7, keep_versions);
            manifest.Int64(8, keep_days);
            if (max_bytes_null) manifest.Null(9); else manifest.Int64(9, max_bytes);
            if (max_files_null) manifest.Null(10); else manifest.Int64(10, max_files);
            if (max_file_bytes_null) manifest.Null(11); else manifest.Int64(11, max_file_bytes);
            manifest.Done();
        }

        Exec(db.get(), R"SQL(
CREATE TEMP TABLE selected_commits(id INTEGER PRIMARY KEY,parent_commit INTEGER);
)SQL");
        {
            Statement commits(db.get(), R"SQL(
INSERT INTO selected_commits(id,parent_commit)
WITH RECURSIVE chain(id,parent_commit,depth) AS (
 SELECT id,parent_commit,0 FROM _vexfs_commits WHERE id=?1 AND workspace_id=?2
 UNION ALL
 SELECT parent.id,parent.parent_commit,chain.depth+1
 FROM chain JOIN _vexfs_commits parent ON parent.id=chain.parent_commit
 WHERE parent.workspace_id=?2 AND chain.depth<1000000)
SELECT id,parent_commit FROM chain
)SQL");
            commits.Int64(1, source_commit);
            commits.Int64(2, workspace_id);
            commits.Done();
        }
        if (ScalarInt64(db.get(), "SELECT count(*) FROM selected_commits") == 0) {
            throw Error("selected source commit does not exist");
        }

        Exec(db.get(), R"SQL(
INSERT INTO package.commits
SELECT printf('%020lld',c.id),c.id,c.parent_commit,c.message,c.created_at,
       vexfs_archive_sha256(CAST(json_array(c.id,c.parent_commit,c.message,c.created_at) AS BLOB))
FROM _vexfs_commits c JOIN selected_commits selected ON selected.id=c.id;

INSERT INTO package.inodes
WITH ranked AS (
 SELECT state.*,ROW_NUMBER() OVER(PARTITION BY state.inode_id ORDER BY state.commit_id DESC) rank
 FROM _vexfs_inode_states state JOIN selected_commits selected ON selected.id=state.commit_id
 WHERE state.workspace_id=(SELECT id FROM _vexfs_workspaces
                           WHERE name=(SELECT source_workspace FROM package.manifest)))
SELECT printf('%020lld',inode_id),inode_id,kind,mode,owner_principal,uid,gid,size,
       current_version,created_at,accessed_at,updated_at,changed_at,deleted_at,
       vexfs_archive_sha256(CAST(json_array(inode_id,kind,mode,owner_principal,uid,gid,size,
          current_version,created_at,accessed_at,updated_at,changed_at,deleted_at) AS BLOB))
FROM ranked WHERE rank=1;

INSERT INTO package.dentries
WITH ranked AS (
 SELECT state.*,ROW_NUMBER() OVER(
   PARTITION BY state.parent_inode,state.name ORDER BY state.commit_id DESC) rank
 FROM _vexfs_dentry_states state JOIN selected_commits selected ON selected.id=state.commit_id
 WHERE state.workspace_id=(SELECT id FROM _vexfs_workspaces
                           WHERE name=(SELECT source_workspace FROM package.manifest)))
SELECT printf('%020lld:%s',parent_inode,hex(name)),parent_inode,name,inode_id,
       vexfs_archive_sha256(CAST(json_array(parent_inode,name,inode_id) AS BLOB))
FROM ranked WHERE rank=1 AND deleted=0;

INSERT INTO package.file_versions(
 record_key,source_inode,version_no,source_commit,source_manifest,size,checksum,
 source_version_no,created_at,record_checksum)
WITH RECURSIVE needed(source_inode,version_no) AS (
 SELECT state.inode_id,state.current_version
 FROM _vexfs_inode_states state JOIN selected_commits selected ON selected.id=state.commit_id
 WHERE state.workspace_id=(SELECT id FROM _vexfs_workspaces
                           WHERE name=(SELECT source_workspace FROM package.manifest))
   AND state.kind<>'directory' AND state.current_version>0
 UNION
 SELECT version.inode_id,version.source_version_no
 FROM needed JOIN _vexfs_file_versions version
   ON version.inode_id=needed.source_inode AND version.version_no=needed.version_no
 WHERE version.source_version_no IS NOT NULL)
SELECT printf('%020lld:%020lld',version.inode_id,version.version_no),version.inode_id,
       version.version_no,version.commit_id,version.manifest_id,version.size,version.checksum,
       version.source_version_no,version.created_at,
       vexfs_archive_sha256(CAST(json_array(version.inode_id,version.version_no,
          version.commit_id,version.manifest_id,version.size,version.checksum,version.source_version_no,
          version.created_at) AS BLOB))
FROM _vexfs_file_versions version JOIN needed
 ON needed.source_inode=version.inode_id AND needed.version_no=version.version_no;

INSERT INTO package.manifests(
 record_key,source_id,file_size,chunk_size,chunk_count,checksum,created_at,record_checksum)
SELECT printf('%020lld',manifest.id),manifest.id,manifest.file_size,manifest.chunk_size,
       manifest.chunk_count,manifest.checksum,manifest.created_at,
       vexfs_archive_sha256(CAST(json_array(manifest.id,manifest.file_size,
          manifest.chunk_size,manifest.chunk_count,manifest.checksum,manifest.created_at) AS BLOB))
FROM _vexfs_manifests manifest
JOIN (SELECT DISTINCT source_manifest FROM package.file_versions
      WHERE source_manifest IS NOT NULL) needed
  ON needed.source_manifest=manifest.id;

INSERT INTO package.chunks(
 record_key,source_manifest,chunk_no,content,size,checksum,record_checksum)
SELECT printf('%020lld:%020lld',entry.manifest_id,entry.chunk_no),entry.manifest_id,
       entry.chunk_no,zeroblob(chunk.size),chunk.size,chunk.checksum,
       vexfs_archive_sha256(CAST(json_array(entry.manifest_id,entry.chunk_no,
          chunk.size,chunk.checksum) AS BLOB))
FROM _vexfs_manifest_chunks entry
JOIN _vexfs_chunks chunk ON chunk.id=entry.chunk_id
JOIN package.manifests manifest ON manifest.source_id=entry.manifest_id;

INSERT INTO package.inode_states
SELECT printf('%020lld:%020lld',state.inode_id,state.commit_id),state.inode_id,state.commit_id,
       state.kind,state.mode,state.owner_principal,state.uid,state.gid,state.size,
       state.current_version,state.created_at,state.accessed_at,state.updated_at,state.changed_at,
       state.deleted_at,
       vexfs_archive_sha256(CAST(json_array(state.inode_id,state.commit_id,state.kind,state.mode,
          state.owner_principal,state.uid,state.gid,state.size,state.current_version,
          state.created_at,state.accessed_at,state.updated_at,state.changed_at,state.deleted_at)
          AS BLOB))
FROM _vexfs_inode_states state JOIN selected_commits selected ON selected.id=state.commit_id
WHERE state.workspace_id=(SELECT id FROM _vexfs_workspaces
                          WHERE name=(SELECT source_workspace FROM package.manifest));

INSERT INTO package.dentry_states
SELECT printf('%020lld:%s:%020lld',state.parent_inode,hex(state.name),state.commit_id),
       state.parent_inode,state.name,state.commit_id,state.inode_id,state.deleted,
       vexfs_archive_sha256(CAST(json_array(state.parent_inode,state.name,state.commit_id,
          state.inode_id,state.deleted) AS BLOB))
FROM _vexfs_dentry_states state JOIN selected_commits selected ON selected.id=state.commit_id
WHERE state.workspace_id=(SELECT id FROM _vexfs_workspaces
                          WHERE name=(SELECT source_workspace FROM package.manifest));

INSERT INTO package.xattr_states
SELECT printf('%020lld:%s:%020lld',state.inode_id,hex(state.name),state.commit_id),
       state.inode_id,state.name,state.commit_id,state.value,state.deleted,
       vexfs_archive_sha256(CAST(json_array(state.inode_id,state.name,state.commit_id,
          hex(state.value),state.deleted) AS BLOB))
FROM _vexfs_xattr_states state JOIN selected_commits selected ON selected.id=state.commit_id
WHERE state.workspace_id=(SELECT id FROM _vexfs_workspaces
                          WHERE name=(SELECT source_workspace FROM package.manifest));

INSERT INTO package.acl_states
SELECT printf('%020lld:%s:%s:%020lld',state.inode_id,hex(state.principal_id),state.effect,
              state.commit_id),state.inode_id,state.principal_id,state.effect,state.commit_id,
       state.permissions,state.inherit_flags,state.deleted,
       vexfs_archive_sha256(CAST(json_array(state.inode_id,state.principal_id,state.effect,
          state.commit_id,state.permissions,state.inherit_flags,state.deleted) AS BLOB))
FROM _vexfs_acl_states state JOIN selected_commits selected ON selected.id=state.commit_id
WHERE state.workspace_id=(SELECT id FROM _vexfs_workspaces
                          WHERE name=(SELECT source_workspace FROM package.manifest));

INSERT INTO package.snapshots
SELECT hex(snapshot.name),snapshot.name,snapshot.commit_id,snapshot.created_at,
       vexfs_archive_sha256(CAST(json_array(snapshot.name,snapshot.commit_id,
          snapshot.created_at) AS BLOB))
FROM _vexfs_snapshots snapshot JOIN selected_commits selected ON selected.id=snapshot.commit_id
WHERE snapshot.workspace_id=(SELECT id FROM _vexfs_workspaces
                             WHERE name=(SELECT source_workspace FROM package.manifest));

INSERT INTO package.xattrs
WITH ranked AS (
 SELECT state.*,ROW_NUMBER() OVER(PARTITION BY state.inode_id,state.name
                                 ORDER BY state.commit_id DESC) rank
 FROM _vexfs_xattr_states state JOIN selected_commits selected ON selected.id=state.commit_id
 WHERE state.workspace_id=(SELECT id FROM _vexfs_workspaces
                           WHERE name=(SELECT source_workspace FROM package.manifest)))
SELECT printf('%020lld:%s',inode_id,hex(name)),inode_id,name,value,
       (SELECT created_at FROM package.manifest),
       vexfs_archive_sha256(CAST(json_array(inode_id,name,hex(value),
          (SELECT created_at FROM package.manifest)) AS BLOB))
FROM ranked
JOIN package.inodes live ON live.source_id=ranked.inode_id AND live.deleted_at IS NULL
WHERE rank=1 AND deleted=0;

INSERT INTO package.acl_entries
WITH ranked AS (
 SELECT state.*,ROW_NUMBER() OVER(PARTITION BY state.inode_id,state.principal_id,state.effect
                                 ORDER BY state.commit_id DESC) rank
 FROM _vexfs_acl_states state JOIN selected_commits selected ON selected.id=state.commit_id
 WHERE state.workspace_id=(SELECT id FROM _vexfs_workspaces
                           WHERE name=(SELECT source_workspace FROM package.manifest)))
SELECT printf('%020lld:%s:%s',inode_id,hex(principal_id),effect),inode_id,principal_id,effect,
       permissions,inherit_flags,(SELECT created_at FROM package.manifest),
       (SELECT created_at FROM package.manifest),
       vexfs_archive_sha256(CAST(json_array(inode_id,principal_id,effect,permissions,
          inherit_flags,(SELECT created_at FROM package.manifest),
          (SELECT created_at FROM package.manifest)) AS BLOB))
FROM ranked
JOIN package.inodes live ON live.source_id=ranked.inode_id AND live.deleted_at IS NULL
WHERE rank=1 AND deleted=0;

INSERT INTO package.principals
SELECT hex(principal),principal,
       vexfs_archive_sha256(CAST(json_array(principal) AS BLOB))
FROM (
 SELECT owner_principal principal FROM package.inodes
 UNION
 SELECT principal_id FROM package.acl_states
 UNION
 SELECT principal_id FROM package.acl_entries)
WHERE principal<>'';
)SQL");

        CopyArchiveChunks(db.get());
        Exec(db.get(), "UPDATE package.manifest SET complete=1");
        const std::string checksum = ComputePackageChecksum(db.get());
        {
            Statement finish(db.get(),
                "UPDATE package.manifest SET package_checksum=?1");
            finish.Text(1, checksum);
            finish.Done();
        }
        Exec(db.get(), "COMMIT");

        const auto bytes = std::filesystem::file_size(output);
        return "{\"format_version\":" + std::to_string(kArchiveFormatVersion) +
            ",\"workspace\":\"" + JsonEscape(workspace) +
            "\",\"source_commit\":" + std::to_string(source_commit) +
            ",\"package_checksum\":\"" + checksum +
            "\",\"bytes\":" + std::to_string(bytes) + "}";
    } catch (...) {
        try { Exec(db.get(), "ROLLBACK"); } catch (...) {}
        db.reset();
        if (owns_output) {
            std::error_code ignored;
            std::filesystem::remove(output, ignored);
        }
        throw;
    }
}

std::string ExportPostgresArchive(const std::string &dsn, const std::string &workspace,
                                  const std::string &snapshot, const std::string &output) {
#if defined(VEXFS_HAVE_LIBPQ)
    const PgArchiveExportResult result =
        ExportPostgresRecords(dsn, workspace, snapshot, output);
    const auto bytes = std::filesystem::file_size(output);
    return "{\"format_version\":" + std::to_string(kArchiveFormatVersion) +
        ",\"workspace\":\"" + JsonEscape(workspace) +
        "\",\"source_commit\":" + std::to_string(result.source_commit) +
        ",\"package_checksum\":\"" + result.checksum +
        "\",\"bytes\":" + std::to_string(bytes) + "}";
#else
    (void)dsn;
    (void)workspace;
    (void)snapshot;
    (void)output;
    throw Error("this vexdb build does not include PostgreSQL client support");
#endif
}

namespace {

struct ArchiveInfo {
    std::string workspace;
    sqlite3_int64 source_commit = 0;
    sqlite3_int64 versions = 0;
    sqlite3_int64 content_bytes = 0;
    std::string checksum;
};

void RequireZero(sqlite3 *db, const char *sql, const char *message) {
    if (ScalarInt64(db, sql) != 0) throw Error(message);
}

ArchiveInfo VerifyAttachedArchive(sqlite3 *db) {
    Exec(db, "PRAGMA trusted_schema=OFF");
    {
        Statement quick(db, "PRAGMA package.quick_check");
        if (!quick.Row() || quick.Text(0) != "ok") {
            throw Error("archive SQLite container is corrupt");
        }
    }
    RequireZero(db, R"SQL(
SELECT count(*) FROM package.sqlite_master
WHERE type IN ('view','trigger') OR (type='table' AND name NOT IN (
 'manifest','commits','inodes','dentries','file_versions','manifests','chunks','inode_states',
 'dentry_states','xattr_states','acl_states','snapshots','xattrs','acl_entries','principals'))
)SQL", "archive has an unexpected schema object");
    if (ScalarInt64(db, R"SQL(
SELECT count(*) FROM package.sqlite_master WHERE type='table' AND name IN (
 'manifest','commits','inodes','dentries','file_versions','manifests','chunks','inode_states',
 'dentry_states','xattr_states','acl_states','snapshots','xattrs','acl_entries','principals')
)SQL") != 15) {
        throw Error("archive schema is incomplete");
    }

    ArchiveInfo info;
    {
        Statement manifest(db, R"SQL(
SELECT format_version,source_workspace,source_commit,package_checksum,complete
FROM package.manifest
)SQL");
        if (!manifest.Row()) throw Error("archive manifest is missing");
        if (manifest.Int(0) != kArchiveFormatVersion) {
            throw Error("unsupported archive format version: " +
                        std::to_string(manifest.Int(0)));
        }
        info.workspace = manifest.Text(1);
        info.source_commit = manifest.Int64(2);
        info.checksum = manifest.Text(3);
        if (manifest.Int(4) != 1 || info.checksum.size() != 64) {
            throw Error("archive was not completed");
        }
        if (manifest.Row()) throw Error("archive has more than one manifest row");
    }

    RequireZero(db, R"SQL(
SELECT sum(bad) FROM (
 SELECT count(*) bad FROM package.commits WHERE record_checksum<>
  vexfs_archive_sha256(CAST(json_array(source_id,parent_source_id,message,created_at) AS BLOB))
 UNION ALL SELECT count(*) FROM package.inodes WHERE record_checksum<>
  vexfs_archive_sha256(CAST(json_array(source_id,kind,mode,owner_principal,uid,gid,size,
   current_version,created_at,accessed_at,updated_at,changed_at,deleted_at) AS BLOB)))
)SQL", "archive commit or inode record checksum mismatch");
    RequireZero(db, R"SQL(
SELECT sum(bad) FROM (
 SELECT count(*) bad FROM package.dentries WHERE record_checksum<>
 vexfs_archive_sha256(CAST(json_array(parent_source_inode,name,inode_source_id) AS BLOB))
 UNION ALL SELECT count(*) FROM package.file_versions WHERE record_checksum<>
  vexfs_archive_sha256(CAST(json_array(source_inode,version_no,source_commit,source_manifest,
   size,checksum,source_version_no,created_at) AS BLOB))
 UNION ALL SELECT count(*) FROM package.manifests WHERE record_checksum<>
  vexfs_archive_sha256(CAST(json_array(source_id,file_size,chunk_size,chunk_count,checksum,
   created_at) AS BLOB))
 UNION ALL SELECT count(*) FROM package.chunks WHERE record_checksum<>
  vexfs_archive_sha256(CAST(json_array(source_manifest,chunk_no,size,checksum) AS BLOB))
 UNION ALL SELECT count(*) FROM package.inode_states WHERE record_checksum<>
  vexfs_archive_sha256(CAST(json_array(source_inode,source_commit,kind,mode,owner_principal,
   uid,gid,size,current_version,created_at,accessed_at,updated_at,changed_at,deleted_at) AS BLOB))
 UNION ALL SELECT count(*) FROM package.dentry_states WHERE record_checksum<>
  vexfs_archive_sha256(CAST(json_array(parent_source_inode,name,source_commit,inode_source_id,
   deleted) AS BLOB))
 UNION ALL SELECT count(*) FROM package.xattr_states WHERE record_checksum<>
  vexfs_archive_sha256(CAST(json_array(source_inode,name,source_commit,hex(value),deleted) AS BLOB))
 UNION ALL SELECT count(*) FROM package.acl_states WHERE record_checksum<>
  vexfs_archive_sha256(CAST(json_array(source_inode,principal_id,effect,source_commit,
   permissions,inherit_flags,deleted) AS BLOB))
 UNION ALL SELECT count(*) FROM package.snapshots WHERE record_checksum<>
  vexfs_archive_sha256(CAST(json_array(name,source_commit,created_at) AS BLOB))
 UNION ALL SELECT count(*) FROM package.xattrs WHERE record_checksum<>
  vexfs_archive_sha256(CAST(json_array(source_inode,name,hex(value),updated_at) AS BLOB))
 UNION ALL SELECT count(*) FROM package.acl_entries WHERE record_checksum<>
  vexfs_archive_sha256(CAST(json_array(source_inode,principal_id,effect,permissions,
   inherit_flags,created_at,updated_at) AS BLOB))
 UNION ALL SELECT count(*) FROM package.principals WHERE record_checksum<>
  vexfs_archive_sha256(CAST(json_array(source_principal) AS BLOB)))
)SQL", "archive record checksum mismatch");

    RequireZero(db, R"SQL(
SELECT count(*) FROM package.manifest manifest
WHERE NOT EXISTS(SELECT 1 FROM package.commits WHERE source_id=manifest.source_commit)
   OR NOT EXISTS(SELECT 1 FROM package.commits
                 WHERE source_id=manifest.history_floor_source_commit)
   OR NOT EXISTS(SELECT 1 FROM package.inodes
                 WHERE source_id=manifest.root_source_inode AND kind='directory'
                   AND deleted_at IS NULL)
)SQL", "archive manifest references a missing object");
    RequireZero(db, R"SQL(
SELECT sum(bad) FROM (
 SELECT count(*) bad FROM package.commits child
 WHERE child.parent_source_id IS NOT NULL AND NOT EXISTS(
  SELECT 1 FROM package.commits parent WHERE parent.source_id=child.parent_source_id)
 UNION ALL SELECT count(*) FROM package.dentries entry
 WHERE NOT EXISTS(SELECT 1 FROM package.inodes inode
                  WHERE inode.source_id=entry.parent_source_inode AND inode.kind='directory'
                    AND inode.deleted_at IS NULL)
    OR NOT EXISTS(SELECT 1 FROM package.inodes inode
                  WHERE inode.source_id=entry.inode_source_id AND inode.deleted_at IS NULL)
 UNION ALL SELECT count(*) FROM package.file_versions version
 WHERE NOT EXISTS(SELECT 1 FROM package.inodes inode
                  WHERE inode.source_id=version.source_inode)
    OR NOT EXISTS(SELECT 1 FROM package.commits commit_row
                  WHERE commit_row.source_id=version.source_commit)
    OR (version.source_version_no IS NULL AND
        (version.source_manifest IS NULL OR NOT EXISTS(
          SELECT 1 FROM package.manifests manifest
          WHERE manifest.source_id=version.source_manifest
            AND manifest.file_size=version.size AND manifest.checksum=version.checksum)))
    OR (version.source_version_no IS NOT NULL AND version.source_manifest IS NOT NULL)
 UNION ALL SELECT count(*) FROM package.manifests manifest
 WHERE manifest.chunk_size<>65536 OR manifest.file_size<0 OR manifest.chunk_count<0
    OR manifest.chunk_count<>(manifest.file_size+65535)/65536
    OR (SELECT count(*) FROM package.file_versions version
        WHERE version.source_manifest=manifest.source_id
          AND version.source_version_no IS NULL)<>1
 UNION ALL SELECT count(*) FROM package.chunks chunk
 WHERE NOT EXISTS(SELECT 1 FROM package.manifests manifest
                  WHERE manifest.source_id=chunk.source_manifest)
    OR chunk.chunk_no<0 OR chunk.size<=0 OR chunk.size>65536
 UNION ALL SELECT count(*) FROM package.inodes inode
 WHERE inode.kind<>'directory' AND inode.current_version>0 AND NOT EXISTS(
  SELECT 1 FROM package.file_versions version
  WHERE version.source_inode=inode.source_id AND version.version_no=inode.current_version)
 UNION ALL SELECT count(*) FROM package.snapshots snapshot
 WHERE NOT EXISTS(SELECT 1 FROM package.commits commit_row
                  WHERE commit_row.source_id=snapshot.source_commit))
)SQL", "archive contains a broken object reference");
    RequireZero(db, R"SQL(
SELECT count(*) FROM package.file_versions alias
WHERE alias.source_version_no IS NOT NULL AND (
 alias.source_manifest IS NOT NULL OR NOT EXISTS(
  SELECT 1 FROM package.file_versions source
  WHERE source.source_inode=alias.source_inode
    AND source.version_no=alias.source_version_no
    AND source.source_version_no IS NULL
    AND source.size=alias.size AND source.checksum=alias.checksum))
)SQL", "archive contains a broken content alias");
    RequireZero(db, R"SQL(
SELECT sum(bad) FROM (
 SELECT count(*) bad FROM package.inode_states state
 WHERE NOT EXISTS(SELECT 1 FROM package.inodes inode
                  WHERE inode.source_id=state.source_inode)
    OR NOT EXISTS(SELECT 1 FROM package.commits commit_row
                  WHERE commit_row.source_id=state.source_commit)
    OR (state.kind<>'directory' AND state.current_version>0 AND NOT EXISTS(
        SELECT 1 FROM package.file_versions version
        WHERE version.source_inode=state.source_inode
          AND version.version_no=state.current_version))
 UNION ALL SELECT count(*) FROM package.dentry_states state
 WHERE NOT EXISTS(SELECT 1 FROM package.inodes inode
                  WHERE inode.source_id=state.parent_source_inode AND inode.kind='directory')
    OR NOT EXISTS(SELECT 1 FROM package.inodes inode
                  WHERE inode.source_id=state.inode_source_id)
    OR NOT EXISTS(SELECT 1 FROM package.commits commit_row
                  WHERE commit_row.source_id=state.source_commit)
 UNION ALL SELECT count(*) FROM package.xattr_states state
 WHERE NOT EXISTS(SELECT 1 FROM package.inodes inode
                  WHERE inode.source_id=state.source_inode)
    OR NOT EXISTS(SELECT 1 FROM package.commits commit_row
                  WHERE commit_row.source_id=state.source_commit)
 UNION ALL SELECT count(*) FROM package.acl_states state
 WHERE NOT EXISTS(SELECT 1 FROM package.inodes inode
                  WHERE inode.source_id=state.source_inode)
    OR NOT EXISTS(SELECT 1 FROM package.commits commit_row
                  WHERE commit_row.source_id=state.source_commit)
 UNION ALL SELECT count(*) FROM package.xattrs attribute
 WHERE NOT EXISTS(SELECT 1 FROM package.inodes inode
                  WHERE inode.source_id=attribute.source_inode AND inode.deleted_at IS NULL)
 UNION ALL SELECT count(*) FROM package.acl_entries entry
 WHERE NOT EXISTS(SELECT 1 FROM package.inodes inode
                  WHERE inode.source_id=entry.source_inode AND inode.deleted_at IS NULL)
 UNION ALL SELECT count(*) FROM package.inodes inode
 WHERE NOT EXISTS(SELECT 1 FROM package.principals principal
                  WHERE principal.source_principal=inode.owner_principal)
 UNION ALL SELECT count(*) FROM package.acl_states entry
 WHERE NOT EXISTS(SELECT 1 FROM package.principals principal
                  WHERE principal.source_principal=entry.principal_id))
)SQL", "archive history or metadata references a missing object");
    RequireZero(db, R"SQL(
WITH RECURSIVE reachable(source_id) AS (
 SELECT root_source_inode FROM package.manifest
 UNION
 SELECT entry.inode_source_id FROM package.dentries entry
 JOIN reachable parent ON parent.source_id=entry.parent_source_inode
), counts AS (
 SELECT (SELECT count(*) FROM package.inodes WHERE deleted_at IS NULL) live_count,
        (SELECT count(*) FROM reachable) reachable_count)
SELECT (live_count<>reachable_count)
    OR EXISTS(SELECT 1 FROM package.dentries entry
              JOIN package.manifest manifest
              ON entry.inode_source_id=manifest.root_source_inode)
    OR EXISTS(
       SELECT 1 FROM package.inodes directory
       WHERE directory.kind='directory' AND directory.deleted_at IS NULL
         AND directory.source_id<>(SELECT root_source_inode FROM package.manifest)
         AND (SELECT count(*) FROM package.dentries entry
              WHERE entry.inode_source_id=directory.source_id)<>1)
FROM counts
)SQL", "archive directory tree is cyclic or unreachable");

    Statement manifests(db, R"SQL(
SELECT source_id,file_size,chunk_count,checksum FROM package.manifests ORDER BY source_id
)SQL");
    std::vector<unsigned char> buffer(kBlobVerifyChunk);
    while (manifests.Row()) {
        const sqlite3_int64 source_manifest = manifests.Int64(0);
        const sqlite3_int64 expected_size = manifests.Int64(1);
        const sqlite3_int64 expected_chunks = manifests.Int64(2);
        const std::string expected_checksum = manifests.Text(3);
        if (expected_size < 0 || expected_chunks < 0 || expected_checksum.size() != 64) {
            throw Error("archive manifest content metadata is invalid");
        }
        Statement chunks(db, R"SQL(
SELECT id,chunk_no,size,checksum,length(content) FROM package.chunks
WHERE source_manifest=?1 ORDER BY chunk_no
)SQL");
        chunks.Int64(1, source_manifest);
        std::vector<vexfs::ManifestChunkChecksum> manifest_chunks;
        manifest_chunks.reserve(static_cast<size_t>(expected_chunks));
        sqlite3_int64 total = 0;
        sqlite3_int64 chunk_no = 0;
        while (chunks.Row()) {
            const sqlite3_int64 rowid = chunks.Int64(0);
            const sqlite3_int64 stored_no = chunks.Int64(1);
            const sqlite3_int64 chunk_size = chunks.Int64(2);
            const std::string chunk_checksum = chunks.Text(3);
            const sqlite3_int64 expected_chunk_size = std::min<sqlite3_int64>(
                65536, expected_size - total);
            if (stored_no != chunk_no || expected_chunk_size <= 0 ||
                chunk_size != expected_chunk_size || chunks.Int64(4) != chunk_size ||
                chunk_checksum.size() != 64 || chunk_size > std::numeric_limits<int>::max()) {
                throw Error("archive chunk size or order is invalid");
            }
            sqlite3_blob *raw_blob = nullptr;
            if (sqlite3_blob_open(db, "package", "chunks", "content", rowid, 0,
                                  &raw_blob) != SQLITE_OK) {
                throw Error("cannot open archive chunk BLOB");
            }
            std::unique_ptr<sqlite3_blob, decltype(&sqlite3_blob_close)> blob(
                raw_blob, sqlite3_blob_close);
            vexfs::Sha256 chunk_hash;
            int offset = 0;
            const int bytes = sqlite3_blob_bytes(blob.get());
            while (offset < bytes) {
                const int amount = std::min<int>(static_cast<int>(buffer.size()), bytes - offset);
                if (sqlite3_blob_read(blob.get(), buffer.data(), amount, offset) != SQLITE_OK) {
                    throw Error("cannot read archive chunk BLOB");
                }
                chunk_hash.Update(buffer.data(), static_cast<size_t>(amount));
                offset += amount;
            }
            if (vexfs::Hex(chunk_hash.Finish()) != chunk_checksum) {
                throw Error("archive chunk checksum mismatch");
            }
            manifest_chunks.push_back({
                static_cast<uint64_t>(chunk_size), chunk_checksum});
            total += chunk_size;
            ++chunk_no;
        }
        if (total != expected_size || chunk_no != expected_chunks ||
            vexfs::ManifestChecksum(
                static_cast<uint64_t>(expected_size), 65536, manifest_chunks) !=
                expected_checksum) {
            throw Error("archive content checksum mismatch");
        }
        info.content_bytes += expected_size;
    }
    info.versions = ScalarInt64(db, "SELECT count(*) FROM package.file_versions");

    const std::string actual_checksum = ComputePackageChecksum(db);
    if (actual_checksum != info.checksum) throw Error("archive package checksum mismatch");
    return info;
}

#if defined(VEXFS_HAVE_LIBPQ)

std::string ImportPostgresRecords(sqlite3 *db, const std::string &dsn,
                                  const std::string &workspace) {
    Statement manifest(db, R"SQL(
SELECT json_object(
 'format_version',format_version,'source_engine',source_engine,
 'source_workspace',source_workspace,'source_commit',source_commit,
 'source_snapshot',source_snapshot,'root_source_inode',root_source_inode,
 'history_floor_source_commit',history_floor_source_commit,
 'retention_keep_versions',retention_keep_versions,
 'retention_keep_days',retention_keep_days,'quota_max_bytes',quota_max_bytes,
 'quota_max_files',quota_max_files,'quota_max_file_bytes',quota_max_file_bytes,
 'created_at',created_at)
FROM package.manifest
)SQL");
    if (!manifest.Row()) throw Error("archive manifest is missing");
    const std::string manifest_json = manifest.Text(0);

    PgConnection connection = OpenPostgres(dsn);
    PgCommand(connection.get(), "BEGIN");
    try {
        PgCommand(connection.get(), "SET LOCAL client_min_messages=warning");
        const char *begin_values[2] = {workspace.c_str(), manifest_json.c_str()};
        const std::string job = PgScalar(
            connection.get(),
            "SELECT public.vexfs_archive_import_begin($1,$2::jsonb)::text",
            2, begin_values);

        struct RecordQuery {
            const char *type;
            const char *sql;
            bool has_content;
        };
        static constexpr RecordQuery queries[] = {
            {"commits", R"SQL(
SELECT record_key,json_object(
 'source_id',source_id,'parent_source_id',parent_source_id,
 'message',message,'created_at',created_at),NULL
FROM package.commits ORDER BY record_key)SQL", false},
            {"inodes", R"SQL(
SELECT record_key,json_object(
 'source_id',source_id,'kind',kind,'mode',mode,'owner_principal',owner_principal,
 'uid',uid,'gid',gid,'size',size,'current_version',current_version,
 'created_at',created_at,'accessed_at',accessed_at,'updated_at',updated_at,
 'changed_at',changed_at,'deleted_at',deleted_at),NULL
FROM package.inodes ORDER BY record_key)SQL", false},
            {"dentries", R"SQL(
SELECT record_key,json_object(
 'parent_source_inode',parent_source_inode,'name',name,
 'inode_source_id',inode_source_id),NULL
FROM package.dentries ORDER BY record_key)SQL", false},
            {"file_versions", R"SQL(
SELECT record_key,json_object(
 'source_inode',source_inode,'version_no',version_no,'source_commit',source_commit,
 'source_manifest',source_manifest,'size',size,'checksum',checksum,
 'source_version_no',source_version_no,'created_at',created_at),NULL
FROM package.file_versions ORDER BY record_key)SQL", false},
            {"manifests", R"SQL(
SELECT record_key,json_object(
 'source_id',source_id,'file_size',file_size,'chunk_size',chunk_size,
 'chunk_count',chunk_count,'checksum',checksum,'created_at',created_at),NULL
FROM package.manifests ORDER BY record_key)SQL", false},
            {"chunks", R"SQL(
SELECT record_key,json_object(
 'source_manifest',source_manifest,'chunk_no',chunk_no,'size',size,
 'checksum',checksum),content
FROM package.chunks ORDER BY record_key)SQL", true},
            {"inode_states", R"SQL(
SELECT record_key,json_object(
 'source_inode',source_inode,'source_commit',source_commit,'kind',kind,'mode',mode,
 'owner_principal',owner_principal,'uid',uid,'gid',gid,'size',size,
 'current_version',current_version,'created_at',created_at,'accessed_at',accessed_at,
 'updated_at',updated_at,'changed_at',changed_at,'deleted_at',deleted_at),NULL
FROM package.inode_states ORDER BY record_key)SQL", false},
            {"dentry_states", R"SQL(
SELECT record_key,json_object(
 'parent_source_inode',parent_source_inode,'name',name,'source_commit',source_commit,
 'inode_source_id',inode_source_id,'deleted',deleted),NULL
FROM package.dentry_states ORDER BY record_key)SQL", false},
            {"xattr_states", R"SQL(
SELECT record_key,json_object(
 'source_inode',source_inode,'name',name,'source_commit',source_commit,
 'value_hex',lower(hex(value)),'deleted',deleted),NULL
FROM package.xattr_states ORDER BY record_key)SQL", false},
            {"acl_states", R"SQL(
SELECT record_key,json_object(
 'source_inode',source_inode,'principal_id',principal_id,'effect',effect,
 'source_commit',source_commit,'permissions',permissions,
 'inherit_flags',inherit_flags,'deleted',deleted),NULL
FROM package.acl_states ORDER BY record_key)SQL", false},
            {"snapshots", R"SQL(
SELECT record_key,json_object(
 'name',name,'source_commit',source_commit,'created_at',created_at),NULL
FROM package.snapshots ORDER BY record_key)SQL", false},
            {"xattrs", R"SQL(
SELECT record_key,json_object(
 'source_inode',source_inode,'name',name,'value_hex',lower(hex(value)),
 'updated_at',updated_at),NULL
FROM package.xattrs ORDER BY record_key)SQL", false},
            {"acl_entries", R"SQL(
SELECT record_key,json_object(
 'source_inode',source_inode,'principal_id',principal_id,'effect',effect,
 'permissions',permissions,'inherit_flags',inherit_flags,
 'created_at',created_at,'updated_at',updated_at),NULL
FROM package.acl_entries ORDER BY record_key)SQL", false},
            {"principals", R"SQL(
SELECT record_key,json_object('source_principal',source_principal),NULL
FROM package.principals ORDER BY record_key)SQL", false}
        };

        for (const RecordQuery &query : queries) {
            Statement records(db, query.sql);
            while (records.Row()) {
                const std::string key = records.Text(0);
                const std::string json = records.Text(1);
                const int content_bytes = query.has_content ? records.Bytes(2) : 0;
                const void *content = query.has_content ? records.Blob(2) : nullptr;
                const char *values[5] = {
                    job.c_str(), query.type, key.c_str(), json.c_str(),
                    query.has_content ? static_cast<const char *>(content) : nullptr};
                const int lengths[5] = {0, 0, 0, 0, content_bytes};
                const int formats[5] = {0, 0, 0, 0, query.has_content ? 1 : 0};
                (void)PgScalar(
                    connection.get(),
                    "SELECT public.vexfs_archive_import_record("
                    "$1::bigint,$2,$3,$4::jsonb,$5)::text",
                    5, values, lengths, formats);
            }
        }
        const char *finish_values[1] = {job.c_str()};
        const std::string result = PgScalar(
            connection.get(),
            "SELECT public.vexfs_archive_import_finish($1::bigint)::text",
            1, finish_values);
        PgCommand(connection.get(), "COMMIT");
        return result;
    } catch (...) {
        try { PgCommand(connection.get(), "ROLLBACK"); } catch (...) {}
        throw;
    }
}

#endif  // VEXFS_HAVE_LIBPQ

}  // namespace

std::string VerifyArchive(const std::string &input) {
    if (!std::filesystem::is_regular_file(input)) {
        throw Error("archive file not found: " + input);
    }
    Database db = Open(":memory:", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI);
    RegisterHash(db.get());
    AttachReadOnly(db.get(), input, "package");
    Exec(db.get(), "PRAGMA query_only=ON");
    const ArchiveInfo info = VerifyAttachedArchive(db.get());
    return "{\"ok\":true,\"format_version\":" +
        std::to_string(kArchiveFormatVersion) + ",\"workspace\":\"" +
        JsonEscape(info.workspace) + "\",\"source_commit\":" +
        std::to_string(info.source_commit) + ",\"versions\":" +
        std::to_string(info.versions) + ",\"content_bytes\":" +
        std::to_string(info.content_bytes) + ",\"package_checksum\":\"" +
        info.checksum + "\"}";
}

std::string ImportPostgresArchive(const std::string &dsn, const std::string &workspace,
                                  const std::string &input) {
#if defined(VEXFS_HAVE_LIBPQ)
    if (dsn.empty()) throw Error("PostgreSQL import needs a connection DSN");
    if (workspace.empty() || workspace.size() > 128) {
        throw Error("workspace name must be 1..128 bytes");
    }
    if (!std::filesystem::is_regular_file(input)) {
        throw Error("archive file not found: " + input);
    }
    Database db = Open(":memory:", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI);
    RegisterHash(db.get());
    AttachReadOnly(db.get(), input, "package");
    Exec(db.get(), "PRAGMA query_only=ON");
    const ArchiveInfo info = VerifyAttachedArchive(db.get());
    std::string result = ImportPostgresRecords(db.get(), dsn, workspace);
    if (!result.empty() && result.back() == '}') {
        result.pop_back();
        result += ",\"package_checksum\":\"" + info.checksum + "\"}";
    }
    return result;
#else
    (void)dsn;
    (void)workspace;
    (void)input;
    throw Error("this vexdb build does not include PostgreSQL client support");
#endif
}

std::string ImportArchive(const std::string &database, const std::string &workspace,
                          const std::string &input) {
    if (database.empty() || database == ":memory:") {
        throw Error("import needs a persistent SQLite database path");
    }
    if (workspace.empty() || workspace.size() > 128) {
        throw Error("workspace name must be 1..128 bytes");
    }
    if (!std::filesystem::is_regular_file(input)) {
        throw Error("archive file not found: " + input);
    }

    const bool database_exists = std::filesystem::exists(database);
    if (!database_exists) {
        Database created = Open(database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                          SQLITE_OPEN_EXCLUSIVE);
        created.reset();
        std::filesystem::permissions(
            database, std::filesystem::perms::owner_read |
                      std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);
    }
    NewDatabaseCleanup new_database(database, !database_exists);
    Database db = Open(database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI);
    RegisterVexDB(db.get());
    RegisterHash(db.get());
    AttachReadOnly(db.get(), input, "package");
    Exec(db.get(), "SELECT vexfs_init()");

    Exec(db.get(), "BEGIN IMMEDIATE");
    try {
        // Verify and import from the same attached-database snapshot. This closes
        // the window where another process could change the archive after
        // verification but before rows are copied.
        const ArchiveInfo info = VerifyAttachedArchive(db.get());
        {
            Statement existing(db.get(),
                "SELECT 1 FROM _vexfs_workspaces WHERE name=?1 LIMIT 1");
            existing.Text(1, workspace);
            if (existing.Row()) throw Error("workspace already exists: " + workspace);
        }
        Statement source_manifest(db.get(), R"SQL(
SELECT root_source_inode,source_commit,history_floor_source_commit,
 retention_keep_versions,retention_keep_days,quota_max_bytes,quota_max_files,
 quota_max_file_bytes,created_at
FROM package.manifest
)SQL");
        if (!source_manifest.Row()) throw Error("archive manifest is missing");
        const sqlite3_int64 source_root = source_manifest.Int64(0);
        const sqlite3_int64 source_head = source_manifest.Int64(1);
        const sqlite3_int64 source_floor = source_manifest.Int64(2);

        Statement workspace_insert(db.get(), R"SQL(
INSERT INTO _vexfs_workspaces(
 name,state,root_inode,head_commit,history_floor_commit,retention_keep_versions,
 retention_keep_days,quota_max_bytes,quota_max_files,quota_max_file_bytes,created_at)
VALUES(?1,'importing',NULL,NULL,NULL,?2,?3,?4,?5,?6,?7)
)SQL");
        workspace_insert.Text(1, workspace);
        workspace_insert.Int64(2, source_manifest.Int64(3));
        workspace_insert.Int64(3, source_manifest.Int64(4));
        if (source_manifest.Type(5) == SQLITE_NULL) workspace_insert.Null(4);
        else workspace_insert.Int64(4, source_manifest.Int64(5));
        if (source_manifest.Type(6) == SQLITE_NULL) workspace_insert.Null(5);
        else workspace_insert.Int64(5, source_manifest.Int64(6));
        if (source_manifest.Type(7) == SQLITE_NULL) workspace_insert.Null(6);
        else workspace_insert.Int64(6, source_manifest.Int64(7));
        workspace_insert.Int64(7, source_manifest.Int64(8));
        workspace_insert.Done();
        const sqlite3_int64 workspace_id = sqlite3_last_insert_rowid(db.get());

        Exec(db.get(), R"SQL(
CREATE TEMP TABLE import_commit_map(source_id INTEGER PRIMARY KEY,local_id INTEGER UNIQUE NOT NULL);
CREATE TEMP TABLE import_inode_map(source_id INTEGER PRIMARY KEY,local_id INTEGER UNIQUE NOT NULL);
CREATE TEMP TABLE import_manifest_map(source_id INTEGER PRIMARY KEY,local_id INTEGER UNIQUE NOT NULL);
CREATE TEMP TABLE import_chunk_map(
 source_manifest INTEGER NOT NULL,chunk_no INTEGER NOT NULL,local_id INTEGER NOT NULL,
 PRIMARY KEY(source_manifest,chunk_no));
)SQL");
        std::unordered_map<sqlite3_int64, sqlite3_int64> commit_map;
        {
            Statement rows(db.get(),
                "SELECT source_id,parent_source_id,message,created_at "
                "FROM package.commits ORDER BY source_id");
            while (rows.Row()) {
                const sqlite3_int64 source_id = rows.Int64(0);
                const bool has_parent = rows.Type(1) != SQLITE_NULL;
                sqlite3_int64 local_parent = 0;
                if (has_parent) {
                    const auto parent = commit_map.find(rows.Int64(1));
                    if (parent == commit_map.end()) {
                        throw Error("archive commit order is not a valid parent chain");
                    }
                    local_parent = parent->second;
                }
                Statement insert(db.get(), R"SQL(
INSERT INTO _vexfs_commits(workspace_id,parent_commit,message,created_at)
VALUES(?1,?2,?3,?4)
)SQL");
                insert.Int64(1, workspace_id);
                if (has_parent) insert.Int64(2, local_parent); else insert.Null(2);
                insert.Text(3, rows.Text(2));
                insert.Int64(4, rows.Int64(3));
                insert.Done();
                const sqlite3_int64 local_id = sqlite3_last_insert_rowid(db.get());
                commit_map[source_id] = local_id;
                Statement map_row(db.get(),
                    "INSERT INTO import_commit_map(source_id,local_id) VALUES(?1,?2)");
                map_row.Int64(1, source_id);
                map_row.Int64(2, local_id);
                map_row.Done();
            }
        }

        std::unordered_map<sqlite3_int64, sqlite3_int64> inode_map;
        {
            Statement rows(db.get(), R"SQL(
SELECT source_id,kind,mode,owner_principal,uid,gid,size,current_version,
 created_at,accessed_at,updated_at,changed_at,deleted_at
FROM package.inodes ORDER BY source_id
)SQL");
            while (rows.Row()) {
                Statement insert(db.get(), R"SQL(
INSERT INTO _vexfs_inodes(
 workspace_id,kind,mode,owner_principal,uid,gid,size,current_version,
 created_at,accessed_at,updated_at,changed_at,deleted_at)
VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)
)SQL");
                insert.Int64(1, workspace_id);
                insert.Text(2, rows.Text(1));
                for (int source_column = 2, target_parameter = 3;
                     source_column <= 11; ++source_column, ++target_parameter) {
                    if (source_column == 3) insert.Text(target_parameter, rows.Text(source_column));
                    else insert.Int64(target_parameter, rows.Int64(source_column));
                }
                if (rows.Type(12) == SQLITE_NULL) insert.Null(13);
                else insert.Int64(13, rows.Int64(12));
                insert.Done();
                const sqlite3_int64 local_id = sqlite3_last_insert_rowid(db.get());
                const sqlite3_int64 source_id = rows.Int64(0);
                inode_map[source_id] = local_id;
                Statement map_row(db.get(),
                    "INSERT INTO import_inode_map(source_id,local_id) VALUES(?1,?2)");
                map_row.Int64(1, source_id);
                map_row.Int64(2, local_id);
                map_row.Done();
            }
        }
        const auto root = inode_map.find(source_root);
        const auto head = commit_map.find(source_head);
        const auto floor = commit_map.find(source_floor);
        if (root == inode_map.end() || head == commit_map.end() || floor == commit_map.end()) {
            throw Error("archive manifest mapping is incomplete");
        }

        {
            Statement rows(db.get(), R"SQL(
SELECT source_id,file_size,chunk_size,chunk_count,checksum,created_at
FROM package.manifests ORDER BY source_id
)SQL");
            while (rows.Row()) {
                Statement insert(db.get(), R"SQL(
INSERT INTO _vexfs_manifests(
 workspace_id,file_size,chunk_size,chunk_count,checksum,created_at)
VALUES(?1,?2,?3,?4,?5,?6)
)SQL");
                insert.Int64(1, workspace_id);
                insert.Int64(2, rows.Int64(1));
                insert.Int64(3, rows.Int64(2));
                insert.Int64(4, rows.Int64(3));
                insert.Text(5, rows.Text(4));
                insert.Int64(6, rows.Int64(5));
                insert.Done();
                Statement map_row(db.get(),
                    "INSERT INTO import_manifest_map(source_id,local_id) VALUES(?1,?2)");
                map_row.Int64(1, rows.Int64(0));
                map_row.Int64(2, sqlite3_last_insert_rowid(db.get()));
                map_row.Done();
            }
        }
        {
            std::unordered_map<std::string, sqlite3_int64> reusable_chunks;
            Statement rows(db.get(), R"SQL(
SELECT chunk.source_manifest,chunk.chunk_no,chunk.size,chunk.checksum,inode.local_id
FROM package.chunks chunk
JOIN package.file_versions version
  ON version.source_manifest=chunk.source_manifest AND version.source_version_no IS NULL
JOIN import_inode_map inode ON inode.source_id=version.source_inode
ORDER BY chunk.source_manifest,chunk.chunk_no
)SQL");
            while (rows.Row()) {
                const std::string reuse_key = std::to_string(rows.Int64(4)) + ":" +
                    std::to_string(rows.Int64(2)) + ":" + rows.Text(3);
                sqlite3_int64 local_chunk = 0;
                const auto reusable = reusable_chunks.find(reuse_key);
                if (reusable != reusable_chunks.end()) {
                    local_chunk = reusable->second;
                } else {
                    Statement insert(db.get(), R"SQL(
INSERT INTO _vexfs_chunks(workspace_id,inode_id,content,size,checksum)
VALUES(?1,?2,zeroblob(?3),?3,?4)
)SQL");
                    insert.Int64(1, workspace_id);
                    insert.Int64(2, rows.Int64(4));
                    insert.Int64(3, rows.Int64(2));
                    insert.Text(4, rows.Text(3));
                    insert.Done();
                    local_chunk = sqlite3_last_insert_rowid(db.get());
                    reusable_chunks.emplace(reuse_key, local_chunk);
                }
                Statement map_row(db.get(), R"SQL(
INSERT INTO import_chunk_map(source_manifest,chunk_no,local_id)
VALUES(?1,?2,?3)
)SQL");
                map_row.Int64(1, rows.Int64(0));
                map_row.Int64(2, rows.Int64(1));
                map_row.Int64(3, local_chunk);
                map_row.Done();
            }
        }
        Exec(db.get(), R"SQL(
INSERT INTO _vexfs_manifest_chunks(manifest_id,chunk_no,chunk_id)
SELECT manifest.local_id,chunk.chunk_no,chunk.local_id
FROM import_chunk_map chunk
JOIN import_manifest_map manifest ON manifest.source_id=chunk.source_manifest;
)SQL");
        CopyImportedChunks(db.get());

        Exec(db.get(), R"SQL(
INSERT INTO _vexfs_file_versions(
 inode_id,version_no,commit_id,manifest_id,size,checksum,source_version_no,created_at)
SELECT inode.local_id,version.version_no,commit_row.local_id,manifest.local_id,
       version.size,version.checksum,version.source_version_no,version.created_at
FROM package.file_versions version
JOIN import_inode_map inode ON inode.source_id=version.source_inode
JOIN import_commit_map commit_row ON commit_row.source_id=version.source_commit
LEFT JOIN import_manifest_map manifest ON manifest.source_id=version.source_manifest;

INSERT INTO _vexfs_inode_states(
 workspace_id,inode_id,commit_id,kind,mode,owner_principal,uid,gid,size,current_version,
 created_at,accessed_at,updated_at,changed_at,deleted_at)
SELECT (SELECT id FROM _vexfs_workspaces WHERE state='importing'),inode.local_id,
       commit_row.local_id,state.kind,state.mode,state.owner_principal,state.uid,state.gid,
       state.size,state.current_version,state.created_at,state.accessed_at,state.updated_at,
       state.changed_at,state.deleted_at
FROM package.inode_states state
JOIN import_inode_map inode ON inode.source_id=state.source_inode
JOIN import_commit_map commit_row ON commit_row.source_id=state.source_commit;

INSERT INTO _vexfs_dentries(workspace_id,parent_inode,name,inode_id)
SELECT (SELECT id FROM _vexfs_workspaces WHERE state='importing'),parent.local_id,
       entry.name,child.local_id
FROM package.dentries entry
JOIN import_inode_map parent ON parent.source_id=entry.parent_source_inode
JOIN import_inode_map child ON child.source_id=entry.inode_source_id;

INSERT INTO _vexfs_dentry_states(
 workspace_id,parent_inode,name,commit_id,inode_id,deleted)
SELECT (SELECT id FROM _vexfs_workspaces WHERE state='importing'),parent.local_id,
       state.name,commit_row.local_id,child.local_id,state.deleted
FROM package.dentry_states state
JOIN import_inode_map parent ON parent.source_id=state.parent_source_inode
JOIN import_inode_map child ON child.source_id=state.inode_source_id
JOIN import_commit_map commit_row ON commit_row.source_id=state.source_commit;

INSERT INTO _vexfs_xattrs(inode_id,name,value,updated_at)
SELECT inode.local_id,attribute.name,attribute.value,attribute.updated_at
FROM package.xattrs attribute
JOIN import_inode_map inode ON inode.source_id=attribute.source_inode;

INSERT INTO _vexfs_xattr_states(workspace_id,inode_id,name,commit_id,value,deleted)
SELECT (SELECT id FROM _vexfs_workspaces WHERE state='importing'),inode.local_id,
       state.name,commit_row.local_id,state.value,state.deleted
FROM package.xattr_states state
JOIN import_inode_map inode ON inode.source_id=state.source_inode
JOIN import_commit_map commit_row ON commit_row.source_id=state.source_commit;

INSERT INTO _vexfs_acl_entries(
 workspace_id,inode_id,principal_id,effect,permissions,inherit_flags,created_at,updated_at)
SELECT (SELECT id FROM _vexfs_workspaces WHERE state='importing'),inode.local_id,
       entry.principal_id,entry.effect,entry.permissions,entry.inherit_flags,
       entry.created_at,entry.updated_at
FROM package.acl_entries entry
JOIN import_inode_map inode ON inode.source_id=entry.source_inode;

INSERT INTO _vexfs_acl_states(
 workspace_id,inode_id,principal_id,effect,commit_id,permissions,inherit_flags,deleted)
SELECT (SELECT id FROM _vexfs_workspaces WHERE state='importing'),inode.local_id,
       state.principal_id,state.effect,commit_row.local_id,state.permissions,
       state.inherit_flags,state.deleted
FROM package.acl_states state
JOIN import_inode_map inode ON inode.source_id=state.source_inode
JOIN import_commit_map commit_row ON commit_row.source_id=state.source_commit;

INSERT INTO _vexfs_snapshots(workspace_id,name,commit_id,created_at)
SELECT (SELECT id FROM _vexfs_workspaces WHERE state='importing'),snapshot.name,
       commit_row.local_id,snapshot.created_at
FROM package.snapshots snapshot
JOIN import_commit_map commit_row ON commit_row.source_id=snapshot.source_commit;
)SQL");

        Statement publish(db.get(), R"SQL(
UPDATE _vexfs_workspaces
SET root_inode=?1,head_commit=?2,history_floor_commit=?3,state='active'
WHERE id=?4 AND state='importing'
)SQL");
        publish.Int64(1, root->second);
        publish.Int64(2, head->second);
        publish.Int64(3, floor->second);
        publish.Int64(4, workspace_id);
        publish.Done();

        {
            Statement clean(db.get(),
                "SELECT json_extract(vexfs_check(?1,0),'$.ok')");
            clean.Text(1, workspace);
            if (!clean.Row() || clean.Int(0) != 1) {
                throw Error("imported workspace failed deep integrity check");
            }
        }
        {
            Statement clear(db.get(),
                "DELETE FROM _vexfs_dirty_inodes WHERE workspace_id=?1");
            clear.Int64(1, workspace_id);
            clear.Done();
        }
        {
            Statement clear(db.get(),
                "DELETE FROM _vexfs_dirty_dentries WHERE workspace_id=?1");
            clear.Int64(1, workspace_id);
            clear.Done();
        }
        {
            Statement clear(db.get(),
                "DELETE FROM _vexfs_dirty_xattrs WHERE workspace_id=?1");
            clear.Int64(1, workspace_id);
            clear.Done();
        }
        {
            Statement clear(db.get(),
                "DELETE FROM _vexfs_dirty_acl WHERE workspace_id=?1");
            clear.Int64(1, workspace_id);
            clear.Done();
        }
        Exec(db.get(), R"SQL(
UPDATE _vexfs_meta SET value='1' WHERE key='grep_index_dirty';
COMMIT;
)SQL");

        const std::string result = "{\"workspace\":\"" + JsonEscape(workspace) +
            "\",\"source_workspace\":\"" + JsonEscape(info.workspace) +
            "\",\"source_commit\":" + std::to_string(info.source_commit) +
            ",\"versions\":" + std::to_string(info.versions) +
            ",\"content_bytes\":" + std::to_string(info.content_bytes) +
            ",\"package_checksum\":\"" + info.checksum + "\"}";
        new_database.Release();
        return result;
    } catch (...) {
        try { Exec(db.get(), "ROLLBACK"); } catch (...) {}
        throw;
    }
}

}  // namespace vexfs_cli
