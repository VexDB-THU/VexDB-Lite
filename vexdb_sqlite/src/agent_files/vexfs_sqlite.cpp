#include "vexdb_sqlite_internal.h"

#include "agent_files/vexfs_sqlite.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr const char *kContractVersion = "0.3.0";
constexpr sqlite3_int64 kMaxStagedBytes = 128LL * 1024LL * 1024LL;
constexpr sqlite3_int64 kInitialStagingCapacity = 64LL * 1024LL;

class SqlError : public std::runtime_error {
  public:
    explicit SqlError(const std::string &message, int code = SQLITE_ERROR)
        : std::runtime_error(message), code_(code) {}

    int code() const { return code_; }

  private:
    int code_;
};

class Statement {
  public:
    Statement(sqlite3 *db, const char *sql) : db_(db) {
        const int rc = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr);
        if (rc != SQLITE_OK) Throw("prepare", rc);
    }

    ~Statement() {
        if (stmt_ != nullptr) sqlite3_finalize(stmt_);
    }

    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    void BindInt64(int index, sqlite3_int64 value) {
        Check(sqlite3_bind_int64(stmt_, index, value), "bind integer");
    }

    void BindInt(int index, int value) {
        Check(sqlite3_bind_int(stmt_, index, value), "bind integer");
    }

    void BindText(int index, const std::string &value) {
        Check(sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                                SQLITE_TRANSIENT),
              "bind text");
    }

    void BindBlob(int index, const std::vector<unsigned char> &value) {
        const void *data = value.empty() ? static_cast<const void *>("") : value.data();
        Check(sqlite3_bind_blob64(stmt_, index, data, value.size(), SQLITE_STATIC),
              "bind blob");
    }

    void BindNull(int index) { Check(sqlite3_bind_null(stmt_, index), "bind NULL"); }

    bool Row() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        Throw("step", rc);
        return false;
    }

    void Done() {
        const int rc = sqlite3_step(stmt_);
        if (rc != SQLITE_DONE) Throw("step", rc);
    }

    sqlite3_int64 Int64(int column) const { return sqlite3_column_int64(stmt_, column); }
    int Int(int column) const { return sqlite3_column_int(stmt_, column); }
    int Type(int column) const { return sqlite3_column_type(stmt_, column); }

    std::string Text(int column) const {
        const unsigned char *text = sqlite3_column_text(stmt_, column);
        const int bytes = sqlite3_column_bytes(stmt_, column);
        return text == nullptr ? std::string() : std::string(reinterpret_cast<const char *>(text), bytes);
    }

    std::vector<unsigned char> Blob(int column) const {
        const auto *data = static_cast<const unsigned char *>(sqlite3_column_blob(stmt_, column));
        const int bytes = sqlite3_column_bytes(stmt_, column);
        if (data == nullptr || bytes == 0) return {};
        return std::vector<unsigned char>(data, data + bytes);
    }

  private:
    void Check(int rc, const char *action) {
        if (rc != SQLITE_OK) Throw(action, rc);
    }

    [[noreturn]] void Throw(const char *action, int rc) {
        throw SqlError(std::string(action) + ": " + sqlite3_errmsg(db_) +
                       " (rc=" + std::to_string(rc) + ")", rc);
    }

    sqlite3 *db_ = nullptr;
    sqlite3_stmt *stmt_ = nullptr;
};

class ReadOnlyBlob {
  public:
    ReadOnlyBlob(sqlite3 *db, sqlite3_int64 rowid) {
        const int rc = sqlite3_blob_open(db, "main", "_vexfs_file_versions", "content",
                                         rowid, 0, &blob_);
        if (rc != SQLITE_OK) throw SqlError("cannot open version BLOB", rc);
    }

    ~ReadOnlyBlob() {
        if (blob_ != nullptr) sqlite3_blob_close(blob_);
    }

    int size() const { return sqlite3_blob_bytes(blob_); }

    void Read(void *output, int bytes, int offset) {
        const int rc = sqlite3_blob_read(blob_, output, bytes, offset);
        if (rc != SQLITE_OK) throw SqlError("cannot read version BLOB", rc);
    }

    ReadOnlyBlob(const ReadOnlyBlob &) = delete;
    ReadOnlyBlob &operator=(const ReadOnlyBlob &) = delete;

  private:
    sqlite3_blob *blob_ = nullptr;
};

void Exec(sqlite3 *db, const char *sql) {
    char *message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        const std::string error = message == nullptr ? sqlite3_errmsg(db) : message;
        sqlite3_free(message);
        throw SqlError(error, rc);
    }
}

class Savepoint {
  public:
    Savepoint(sqlite3 *db, const char *name) : db_(db), name_(name) {
        Exec(db_, ("SAVEPOINT " + name_).c_str());
    }

    ~Savepoint() {
        if (!active_) return;
        try {
            Exec(db_, ("ROLLBACK TO SAVEPOINT " + name_).c_str());
            Exec(db_, ("RELEASE SAVEPOINT " + name_).c_str());
        } catch (...) {
        }
    }

    void Release() {
        Exec(db_, ("RELEASE SAVEPOINT " + name_).c_str());
        active_ = false;
    }

    Savepoint(const Savepoint &) = delete;
    Savepoint &operator=(const Savepoint &) = delete;

  private:
    sqlite3 *db_;
    std::string name_;
    bool active_ = true;
};

void AcquireWriteLock(sqlite3 *db) {
    // SQLite 从读事务升级为写事务时可能不执行 busy handler。先做一次无副作用
    // UPDATE，让 busy_timeout 在读取业务状态前生效，并在后续 savepoint 内保持写锁。
    Exec(db, "UPDATE _vexfs_meta SET value=value WHERE key='contract_version'");
}

bool HasVersionReferences(sqlite3 *db) {
    Statement statement(db,
        "SELECT 1 FROM pragma_table_info('_vexfs_file_versions') "
        "WHERE name='source_version_no' LIMIT 1");
    return statement.Row();
}

bool HasVersionTable(sqlite3 *db) {
    Statement statement(db,
        "SELECT 1 FROM sqlite_master WHERE type='table' "
        "AND name='_vexfs_file_versions' LIMIT 1");
    return statement.Row();
}

void EnsureVersionReferences(sqlite3 *db) {
    // 极早期 0.1 测试库允许只包含 handle/staging 表；没有版本表时不凭空补半张 schema。
    if (HasVersionTable(db) && !HasVersionReferences(db)) {
        Exec(db, "ALTER TABLE _vexfs_file_versions ADD COLUMN source_version_no INTEGER");
    }
}

void MigrateSchema010To020(sqlite3 *db) {
    Exec(db, "SAVEPOINT vexfs_migrate_010_020");
    try {
        Exec(db, R"SQL(
ALTER TABLE _vexfs_handles ADD COLUMN owner_session TEXT;
ALTER TABLE _vexfs_staging RENAME TO _vexfs_staging_010;
CREATE TABLE _vexfs_staging(
    handle_id TEXT PRIMARY KEY,
    generation INTEGER NOT NULL,
    content BLOB NOT NULL,
    logical_size INTEGER NOT NULL,
    capacity INTEGER NOT NULL,
    created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    updated_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000)
);
INSERT INTO _vexfs_staging(handle_id, generation, content, logical_size, capacity, created_at)
SELECT s.handle_id, s.generation, s.content, length(s.content), length(s.content), s.created_at
FROM _vexfs_staging_010 s
JOIN _vexfs_handles h ON h.id=s.handle_id AND h.dirty_generation=s.generation;
DROP TABLE _vexfs_staging_010;
ALTER TABLE _vexfs_requests ADD COLUMN request_fingerprint BLOB NOT NULL DEFAULT X'';
DELETE FROM _vexfs_requests;
UPDATE _vexfs_inodes SET current_version=1
WHERE kind='directory' AND deleted_at IS NULL AND current_version<1;
UPDATE _vexfs_meta SET value='0.2.0' WHERE key='contract_version';
)SQL");
        Exec(db, "RELEASE SAVEPOINT vexfs_migrate_010_020");
    } catch (...) {
        try {
            Exec(db, "ROLLBACK TO SAVEPOINT vexfs_migrate_010_020");
            Exec(db, "RELEASE SAVEPOINT vexfs_migrate_010_020");
        } catch (...) {
        }
        throw;
    }
}

void MigrateSchema020To030(sqlite3 *db) {
    Savepoint savepoint(db, "vexfs_migrate_020_030");
    Exec(db, R"SQL(
CREATE TABLE IF NOT EXISTS _vexfs_staging_data(
    handle_id TEXT PRIMARY KEY,
    content BLOB NOT NULL
);
INSERT OR REPLACE INTO _vexfs_staging_data(handle_id, content)
SELECT handle_id, content FROM _vexfs_staging;
UPDATE _vexfs_staging SET content=X'' WHERE length(content)>0;
INSERT OR REPLACE INTO _vexfs_meta(key, value) VALUES('staging_layout', 'split-v1');
CREATE TABLE IF NOT EXISTS _vexfs_mount_sessions(
    workspace_id INTEGER PRIMARY KEY,
    session_id TEXT NOT NULL UNIQUE,
    lease_until INTEGER NOT NULL,
    created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    updated_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000)
);
)SQL");
    EnsureVersionReferences(db);
    // 合同标记必须最后写入；前面的布局升级有任何一步失败，整个 savepoint 回滚。
    Exec(db, "UPDATE _vexfs_meta SET value='0.3.0' WHERE key='contract_version'");
    savepoint.Release();
}

void EnsureSplitStagingLayout(sqlite3 *db) {
    Statement marker(db,
        "SELECT 1 FROM _vexfs_meta WHERE key='staging_layout' AND value='split-v1' LIMIT 1");
    if (marker.Row()) return;

    Exec(db, "SAVEPOINT vexfs_split_staging");
    try {
        Exec(db, R"SQL(
CREATE TABLE IF NOT EXISTS _vexfs_staging_data(
    handle_id TEXT PRIMARY KEY,
    content BLOB NOT NULL
);
INSERT OR REPLACE INTO _vexfs_staging_data(handle_id, content)
SELECT handle_id, content FROM _vexfs_staging;
UPDATE _vexfs_staging SET content=X'' WHERE length(content)>0;
INSERT OR REPLACE INTO _vexfs_meta(key, value) VALUES('staging_layout', 'split-v1');
)SQL");
        Exec(db, "RELEASE SAVEPOINT vexfs_split_staging");
    } catch (...) {
        try {
            Exec(db, "ROLLBACK TO SAVEPOINT vexfs_split_staging");
            Exec(db, "RELEASE SAVEPOINT vexfs_split_staging");
        } catch (...) {
        }
        throw;
    }
}

void EnsureRuntimeTables(sqlite3 *db) {
    {
        Statement exists(db,
            "SELECT 1 FROM sqlite_master WHERE type='table' "
            "AND name='_vexfs_mount_sessions' LIMIT 1");
        if (!exists.Row()) {
            Exec(db, R"SQL(
CREATE TABLE IF NOT EXISTS _vexfs_mount_sessions(
    workspace_id INTEGER PRIMARY KEY,
    session_id TEXT NOT NULL UNIQUE,
    lease_until INTEGER NOT NULL,
    created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    updated_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000)
);
)SQL");
        }
    }
    EnsureVersionReferences(db);
}

void EnsureSchema(sqlite3 *db) {
    bool schema_exists = false;
    std::string version;
    {
        Statement exists(db,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='_vexfs_meta' LIMIT 1");
        if (exists.Row()) {
            schema_exists = true;
            Statement check(db,
                "SELECT value FROM _vexfs_meta WHERE key='contract_version' LIMIT 1");
            if (!check.Row()) throw SqlError("VexFS schema has no contract version");
            version = check.Text(0);
        }
    }
    if (schema_exists) {
        if (version == "0.2.0" && sqlite3_db_readonly(db, "main") == 1) {
            // 0.3 的版本读取 API 只依赖 0.2 已有的 file_versions/commits。
            // 只读连接允许兼容读取，但不会创建 session 表或改写合同标记。
            return;
        }
        if (version == "0.1.0") {
            MigrateSchema010To020(db);
            version = "0.2.0";
        }
        if (version == "0.2.0") {
            MigrateSchema020To030(db);
            version = "0.3.0";
        }
        if (version != kContractVersion)
            throw SqlError("unsupported VexFS schema version: " + version, SQLITE_MISMATCH);
        EnsureSplitStagingLayout(db);
        EnsureRuntimeTables(db);
        return;
    }
    Savepoint savepoint(db, "vexfs_schema_init");
    Exec(db, R"SQL(
CREATE TABLE IF NOT EXISTS _vexfs_meta(
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS _vexfs_workspaces(
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,
    root_inode INTEGER,
    head_commit INTEGER,
    created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000)
);
CREATE TABLE IF NOT EXISTS _vexfs_inodes(
    id INTEGER PRIMARY KEY,
    workspace_id INTEGER NOT NULL,
    kind TEXT NOT NULL CHECK(kind IN ('file','directory')),
    mode INTEGER NOT NULL,
    size INTEGER NOT NULL DEFAULT 0,
    current_version INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    updated_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    deleted_at INTEGER
);
CREATE INDEX IF NOT EXISTS _vexfs_inodes_workspace_idx
    ON _vexfs_inodes(workspace_id, id);
CREATE TABLE IF NOT EXISTS _vexfs_dentries(
    workspace_id INTEGER NOT NULL,
    parent_inode INTEGER NOT NULL,
    name TEXT NOT NULL COLLATE BINARY,
    inode_id INTEGER NOT NULL,
    PRIMARY KEY(workspace_id, parent_inode, name)
);
CREATE UNIQUE INDEX IF NOT EXISTS _vexfs_dentries_inode_idx
    ON _vexfs_dentries(workspace_id, inode_id);
CREATE TABLE IF NOT EXISTS _vexfs_commits(
    id INTEGER PRIMARY KEY,
    workspace_id INTEGER NOT NULL,
    parent_commit INTEGER,
    message TEXT NOT NULL,
    created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000)
);
CREATE TABLE IF NOT EXISTS _vexfs_file_versions(
    id INTEGER PRIMARY KEY,
    inode_id INTEGER NOT NULL,
    version_no INTEGER NOT NULL,
    commit_id INTEGER NOT NULL,
    content BLOB NOT NULL,
    size INTEGER NOT NULL,
    source_version_no INTEGER,
    created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    UNIQUE(inode_id, version_no)
);
CREATE TABLE IF NOT EXISTS _vexfs_handles(
    id TEXT PRIMARY KEY,
    workspace_id INTEGER NOT NULL,
    inode_id INTEGER NOT NULL,
    open_flags TEXT NOT NULL,
    writable INTEGER NOT NULL,
    expected_version INTEGER NOT NULL,
    dirty_generation INTEGER NOT NULL DEFAULT 0,
    published_generation INTEGER NOT NULL DEFAULT 0,
    state TEXT NOT NULL CHECK(state IN ('open','retained','closed')),
    lease_until INTEGER,
    owner_session TEXT,
    opened_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    updated_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000)
);
CREATE INDEX IF NOT EXISTS _vexfs_handles_workspace_idx
    ON _vexfs_handles(workspace_id, state);
CREATE TABLE IF NOT EXISTS _vexfs_staging(
    handle_id TEXT PRIMARY KEY,
    generation INTEGER NOT NULL,
    content BLOB NOT NULL,
    logical_size INTEGER NOT NULL,
    capacity INTEGER NOT NULL,
    created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    updated_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000)
);
CREATE TABLE IF NOT EXISTS _vexfs_staging_data(
    handle_id TEXT PRIMARY KEY,
    content BLOB NOT NULL
);
CREATE TABLE IF NOT EXISTS _vexfs_requests(
    request_id TEXT PRIMARY KEY,
    operation TEXT NOT NULL,
    request_fingerprint BLOB NOT NULL,
    result_integer INTEGER,
    result_text TEXT,
    created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000)
);
CREATE TABLE IF NOT EXISTS _vexfs_mount_sessions(
    workspace_id INTEGER PRIMARY KEY,
    session_id TEXT NOT NULL UNIQUE,
    lease_until INTEGER NOT NULL,
    created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    updated_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000)
);
INSERT OR IGNORE INTO _vexfs_meta(key, value) VALUES('contract_version', '0.3.0');
INSERT OR IGNORE INTO _vexfs_meta(key, value) VALUES('staging_layout', 'split-v1');
)SQL");
    savepoint.Release();
}

std::string RequiredText(sqlite3_value *value, const char *name) {
    if (sqlite3_value_type(value) == SQLITE_NULL) {
        throw SqlError(std::string(name) + " must not be NULL", SQLITE_MISMATCH);
    }
    const unsigned char *text = sqlite3_value_text(value);
    const int bytes = sqlite3_value_bytes(value);
    if (text == nullptr) return {};
    return std::string(reinterpret_cast<const char *>(text), bytes);
}

std::vector<unsigned char> RequiredBlob(sqlite3_value *value, const char *name) {
    if (sqlite3_value_type(value) == SQLITE_NULL) {
        throw SqlError(std::string(name) + " must not be NULL", SQLITE_MISMATCH);
    }
    const auto *data = static_cast<const unsigned char *>(sqlite3_value_blob(value));
    const int bytes = sqlite3_value_bytes(value);
    if (bytes <= 0) return {};
    if (data == nullptr) throw SqlError(std::string(name) + " is not readable", SQLITE_MISMATCH);
    return std::vector<unsigned char>(data, data + bytes);
}

sqlite3_int64 RequiredPositiveInteger(sqlite3_value *value, const char *name) {
    if (sqlite3_value_type(value) != SQLITE_INTEGER) {
        throw SqlError(std::string(name) + " must be a positive integer", SQLITE_MISMATCH);
    }
    const sqlite3_int64 result = sqlite3_value_int64(value);
    if (result <= 0) {
        throw SqlError(std::string(name) + " must be a positive integer", SQLITE_RANGE);
    }
    return result;
}

sqlite3_int64 RequiredNonnegativeInteger(sqlite3_value *value, const char *name) {
    if (sqlite3_value_type(value) != SQLITE_INTEGER) {
        throw SqlError(std::string(name) + " must be a non-negative integer", SQLITE_MISMATCH);
    }
    const sqlite3_int64 result = sqlite3_value_int64(value);
    if (result < 0) {
        throw SqlError(std::string(name) + " must be a non-negative integer", SQLITE_RANGE);
    }
    return result;
}

std::vector<std::string> PathParts(const std::string &path) {
    if (path.empty() || path.front() != '/') throw SqlError("path must be absolute", SQLITE_MISMATCH);
    if (path.find('\0') != std::string::npos) throw SqlError("path contains NUL", SQLITE_MISMATCH);

    std::vector<std::string> parts;
    size_t start = 1;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const size_t end = slash == std::string::npos ? path.size() : slash;
        if (end > start) {
            std::string part = path.substr(start, end - start);
            if (part == "." || part == "..")
                throw SqlError("path must not contain . or ..", SQLITE_MISMATCH);
            if (part.size() > 255)
                throw SqlError("path component is longer than 255 bytes", SQLITE_MISMATCH);
            parts.push_back(std::move(part));
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return parts;
}

struct Workspace {
    sqlite3_int64 id = 0;
    sqlite3_int64 root_inode = 0;
    sqlite3_int64 head_commit = 0;
};

Workspace FindWorkspace(sqlite3 *db, const std::string &name) {
    Statement statement(db,
        "SELECT id, root_inode, COALESCE(head_commit, 0) "
        "FROM _vexfs_workspaces WHERE name = ?1");
    statement.BindText(1, name);
    if (!statement.Row()) throw SqlError("workspace not found: " + name, SQLITE_NOTFOUND);
    return {statement.Int64(0), statement.Int64(1), statement.Int64(2)};
}

struct Node {
    sqlite3_int64 id = 0;
    std::string kind;
    sqlite3_int64 size = 0;
    sqlite3_int64 version = 0;
    int mode = 0;
    sqlite3_int64 created_at = 0;
    sqlite3_int64 updated_at = 0;
};

struct VersionStorage {
    sqlite3_int64 rowid = 0;
    sqlite3_int64 size = 0;
    sqlite3_int64 canonical_version = 0;
};

VersionStorage ResolveVersionStorage(sqlite3 *db, sqlite3_int64 inode_id,
                                     sqlite3_int64 version) {
    if (!HasVersionReferences(db)) {
        Statement statement(db,
            "SELECT rowid,size,version_no FROM _vexfs_file_versions "
            "WHERE inode_id=?1 AND version_no=?2");
        statement.BindInt64(1, inode_id);
        statement.BindInt64(2, version);
        if (!statement.Row()) {
            throw SqlError("file version not found: " + std::to_string(version),
                           SQLITE_NOTFOUND);
        }
        return {statement.Int64(0), statement.Int64(1), statement.Int64(2)};
    }

    Statement statement(db, R"SQL(
SELECT CASE WHEN v.source_version_no IS NULL THEN v.rowid ELSE source.rowid END,
       v.size,
       COALESCE(v.source_version_no,v.version_no),
       CASE WHEN v.source_version_no IS NULL THEN v.size ELSE source.size END
FROM _vexfs_file_versions v
LEFT JOIN _vexfs_file_versions source
  ON source.inode_id=v.inode_id AND source.version_no=v.source_version_no
WHERE v.inode_id=?1 AND v.version_no=?2
)SQL");
    statement.BindInt64(1, inode_id);
    statement.BindInt64(2, version);
    if (!statement.Row()) {
        throw SqlError("file version not found: " + std::to_string(version), SQLITE_NOTFOUND);
    }
    if (statement.Type(0) == SQLITE_NULL || statement.Int64(1) < 0 ||
        statement.Int64(1) != statement.Int64(3)) {
        throw SqlError("file version content reference is corrupt", SQLITE_CORRUPT);
    }
    return {statement.Int64(0), statement.Int64(1), statement.Int64(2)};
}

std::vector<unsigned char> ReadVersionContent(sqlite3 *db, sqlite3_int64 inode_id,
                                              sqlite3_int64 version) {
    const VersionStorage storage = ResolveVersionStorage(db, inode_id, version);
    if (storage.size > kMaxStagedBytes) {
        throw SqlError("file version is larger than 128 MiB", SQLITE_CORRUPT);
    }
    ReadOnlyBlob blob(db, storage.rowid);
    if (blob.size() != storage.size) {
        throw SqlError("file version size does not match content", SQLITE_CORRUPT);
    }
    std::vector<unsigned char> content(static_cast<size_t>(storage.size));
    constexpr int kChunkSize = 64 * 1024;
    for (sqlite3_int64 offset = 0; offset < storage.size; offset += kChunkSize) {
        const int bytes = static_cast<int>(
            std::min<sqlite3_int64>(kChunkSize, storage.size - offset));
        blob.Read(content.data() + offset, bytes, static_cast<int>(offset));
    }
    return content;
}

bool FindChild(sqlite3 *db, sqlite3_int64 workspace_id, sqlite3_int64 parent,
               const std::string &name, Node *node) {
    Statement statement(db,
        "SELECT i.id, i.kind, i.size, i.current_version, i.mode, i.created_at, i.updated_at "
        "FROM _vexfs_dentries d JOIN _vexfs_inodes i ON i.id = d.inode_id "
        "WHERE d.workspace_id = ?1 AND d.parent_inode = ?2 AND d.name = ?3 "
        "AND i.deleted_at IS NULL");
    statement.BindInt64(1, workspace_id);
    statement.BindInt64(2, parent);
    statement.BindText(3, name);
    if (!statement.Row()) return false;
    *node = {statement.Int64(0), statement.Text(1), statement.Int64(2),
             statement.Int64(3), statement.Int(4), statement.Int64(5), statement.Int64(6)};
    return true;
}

Node RootNode(sqlite3 *db, const Workspace &workspace) {
    Statement statement(db,
        "SELECT id, kind, size, current_version, mode, created_at, updated_at FROM _vexfs_inodes "
        "WHERE id = ?1 AND workspace_id = ?2 AND deleted_at IS NULL");
    statement.BindInt64(1, workspace.root_inode);
    statement.BindInt64(2, workspace.id);
    if (!statement.Row()) throw SqlError("workspace root is missing", SQLITE_CORRUPT);
    return {statement.Int64(0), statement.Text(1), statement.Int64(2),
             statement.Int64(3), statement.Int(4), statement.Int64(5), statement.Int64(6)};
}

bool TryResolve(sqlite3 *db, const Workspace &workspace, const std::vector<std::string> &parts,
                Node *node) {
    Node current = RootNode(db, workspace);
    for (const std::string &part : parts) {
        if (current.kind != "directory") return false;
        if (!FindChild(db, workspace.id, current.id, part, &current)) return false;
    }
    *node = std::move(current);
    return true;
}

Node Resolve(sqlite3 *db, const Workspace &workspace, const std::vector<std::string> &parts) {
    Node node;
    if (!TryResolve(db, workspace, parts, &node)) throw SqlError("path not found", SQLITE_NOTFOUND);
    return node;
}

std::pair<Node, std::string> ResolveParent(sqlite3 *db, const Workspace &workspace,
                                           const std::vector<std::string> &parts) {
    if (parts.empty()) throw SqlError("root path has no parent", SQLITE_MISMATCH);
    std::vector<std::string> parent_parts(parts.begin(), parts.end() - 1);
    Node parent = Resolve(db, workspace, parent_parts);
    if (parent.kind != "directory") throw SqlError("parent is not a directory", SQLITE_MISMATCH);
    return {std::move(parent), parts.back()};
}

std::string PathForInode(sqlite3 *db, const Workspace &workspace, sqlite3_int64 inode) {
    if (inode == workspace.root_inode) return "/";
    Statement statement(db, R"SQL(
WITH RECURSIVE ancestors(inode_id, parent_inode, path, depth) AS (
    SELECT d.inode_id, d.parent_inode, d.name, 1
    FROM _vexfs_dentries d
    JOIN _vexfs_inodes i ON i.id=d.inode_id AND i.deleted_at IS NULL
    WHERE d.workspace_id=?1 AND d.inode_id=?2
    UNION ALL
    SELECT d.inode_id, d.parent_inode, d.name || '/' || a.path, a.depth+1
    FROM _vexfs_dentries d
    JOIN ancestors a ON a.parent_inode=d.inode_id
    JOIN _vexfs_inodes i ON i.id=d.inode_id AND i.deleted_at IS NULL
    WHERE d.workspace_id=?1 AND a.depth < 1024
)
SELECT '/' || path FROM ancestors WHERE parent_inode=?3 ORDER BY depth DESC LIMIT 1
)SQL");
    statement.BindInt64(1, workspace.id);
    statement.BindInt64(2, inode);
    statement.BindInt64(3, workspace.root_inode);
    if (!statement.Row()) throw SqlError("inode path not found", SQLITE_NOTFOUND);
    return statement.Text(0);
}

sqlite3_int64 CreateCommit(sqlite3 *db, const Workspace &workspace, const std::string &message) {
    Savepoint savepoint(db, "vexfs_create_commit");
    Statement insert(db,
        "INSERT INTO _vexfs_commits(workspace_id, parent_commit, message) "
        "SELECT id, NULLIF(head_commit, 0), ?2 FROM _vexfs_workspaces WHERE id=?1");
    insert.BindInt64(1, workspace.id);
    insert.BindText(2, message);
    insert.Done();
    if (sqlite3_changes64(db) != 1) {
        throw SqlError("workspace not found", SQLITE_NOTFOUND);
    }
    const sqlite3_int64 commit = sqlite3_last_insert_rowid(db);

    Statement update(db, "UPDATE _vexfs_workspaces SET head_commit = ?1 WHERE id = ?2");
    update.BindInt64(1, commit);
    update.BindInt64(2, workspace.id);
    update.Done();
    savepoint.Release();
    return commit;
}

sqlite3_int64 CreateInode(sqlite3 *db, sqlite3_int64 workspace_id, const char *kind, int mode) {
    Statement insert(db,
        "INSERT INTO _vexfs_inodes(workspace_id, kind, mode, current_version) "
        "VALUES(?1, ?2, ?3, ?4)");
    insert.BindInt64(1, workspace_id);
    insert.BindText(2, kind);
    insert.BindInt(3, mode);
    insert.BindInt64(4, std::strcmp(kind, "directory") == 0 ? 1 : 0);
    insert.Done();
    return sqlite3_last_insert_rowid(db);
}

void TouchDirectory(sqlite3 *db, sqlite3_int64 workspace_id, sqlite3_int64 inode) {
    Statement update(db,
        "UPDATE _vexfs_inodes SET current_version=current_version+1, "
        "updated_at=CAST(strftime('%s','now') AS INTEGER)*1000 "
        "WHERE id=?1 AND workspace_id=?2 AND kind='directory' AND deleted_at IS NULL");
    update.BindInt64(1, inode);
    update.BindInt64(2, workspace_id);
    update.Done();
}

void AddDentry(sqlite3 *db, sqlite3_int64 workspace_id, sqlite3_int64 parent,
               const std::string &name, sqlite3_int64 inode) {
    Statement insert(db,
        "INSERT INTO _vexfs_dentries(workspace_id, parent_inode, name, inode_id) "
        "VALUES(?1, ?2, ?3, ?4)");
    insert.BindInt64(1, workspace_id);
    insert.BindInt64(2, parent);
    insert.BindText(3, name);
    insert.BindInt64(4, inode);
    insert.Done();
    TouchDirectory(db, workspace_id, parent);
}

sqlite3_int64 StoreVersion(sqlite3 *db, const Workspace &workspace, const Node &node,
                           const std::vector<unsigned char> &content,
                           const std::string &message) {
    Savepoint savepoint(db, "vexfs_store_version");
    const sqlite3_int64 version = node.version + 1;
    Statement advance(db,
        "UPDATE _vexfs_inodes SET size=?1, current_version=?2, "
        "updated_at=CAST(strftime('%s','now') AS INTEGER)*1000 "
        "WHERE id=?3 AND current_version=?4");
    advance.BindInt64(1, static_cast<sqlite3_int64>(content.size()));
    advance.BindInt64(2, version);
    advance.BindInt64(3, node.id);
    advance.BindInt64(4, node.version);
    advance.Done();
    if (sqlite3_changes64(db) != 1) {
        throw SqlError("version conflict: file changed before publish", SQLITE_CONSTRAINT);
    }

    const sqlite3_int64 commit = CreateCommit(db, workspace, message);
    Statement insert(db,
        "INSERT INTO _vexfs_file_versions(inode_id, version_no, commit_id, content, size) "
        "VALUES(?1, ?2, ?3, ?4, ?5)");
    insert.BindInt64(1, node.id);
    insert.BindInt64(2, version);
    insert.BindInt64(3, commit);
    insert.BindBlob(4, content);
    insert.BindInt64(5, static_cast<sqlite3_int64>(content.size()));
    insert.Done();
    savepoint.Release();
    return version;
}

sqlite3_int64 StoreVersionFromVersion(sqlite3 *db, const Workspace &workspace, const Node &node,
                                      sqlite3_int64 source_version,
                                      const std::string &message) {
    Savepoint savepoint(db, "vexfs_restore_version");
    AcquireWriteLock(db);
    const VersionStorage source = ResolveVersionStorage(db, node.id, source_version);
    const sqlite3_int64 version = node.version + 1;
    Statement advance(db,
        "UPDATE _vexfs_inodes SET size=?1, current_version=?2, "
        "updated_at=CAST(strftime('%s','now') AS INTEGER)*1000 "
        "WHERE id=?3 AND current_version=?4");
    advance.BindInt64(1, source.size);
    advance.BindInt64(2, version);
    advance.BindInt64(3, node.id);
    advance.BindInt64(4, node.version);
    advance.Done();
    if (sqlite3_changes64(db) != 1) {
        throw SqlError("version conflict: file changed before restore", SQLITE_CONSTRAINT);
    }
    const sqlite3_int64 commit = CreateCommit(db, workspace, message);
    Statement insert(db,
        "INSERT INTO _vexfs_file_versions"
        "(inode_id,version_no,commit_id,content,size,source_version_no) "
        "VALUES(?1,?2,?3,X'',?4,?5)");
    insert.BindInt64(1, node.id);
    insert.BindInt64(2, version);
    insert.BindInt64(3, commit);
    insert.BindInt64(4, source.size);
    insert.BindInt64(5, source.canonical_version);
    insert.Done();
    if (sqlite3_changes64(db) != 1) {
        throw SqlError("file version not found: " + std::to_string(source_version), SQLITE_NOTFOUND);
    }
    savepoint.Release();
    return version;
}

std::vector<unsigned char> ReadNode(sqlite3 *db, const Node &node) {
    if (node.kind != "file") throw SqlError("path is not a file", SQLITE_MISMATCH);
    if (node.version == 0) return {};
    return ReadVersionContent(db, node.id, node.version);
}

std::vector<unsigned char> ReadNodeVersion(sqlite3 *db, const Node &node,
                                           sqlite3_int64 version) {
    if (node.kind != "file") throw SqlError("path is not a file", SQLITE_MISMATCH);
    if (version <= 0) throw SqlError("version must be a positive integer", SQLITE_RANGE);
    return ReadVersionContent(db, node.id, version);
}

sqlite3_int64 WritePath(sqlite3 *db, const Workspace &workspace,
                        const std::vector<std::string> &parts,
                        const std::vector<unsigned char> &content,
                        const std::string &message) {
    Savepoint savepoint(db, "vexfs_write_path");
    AcquireWriteLock(db);
    auto [parent, name] = ResolveParent(db, workspace, parts);
    Node node;
    if (!FindChild(db, workspace.id, parent.id, name, &node)) {
        node.id = CreateInode(db, workspace.id, "file", 0644);
        node.kind = "file";
        node.mode = 0644;
        AddDentry(db, workspace.id, parent.id, name, node.id);
    }
    if (node.kind != "file") throw SqlError("path is not a file", SQLITE_MISMATCH);
    const sqlite3_int64 version = StoreVersion(db, workspace, node, content, message);
    savepoint.Release();
    return version;
}

std::string RandomHandleId() {
    unsigned char bytes[16];
    sqlite3_randomness(sizeof(bytes), bytes);
    const uint64_t process_id = static_cast<uint64_t>(
#if defined(_WIN32)
        _getpid()
#else
        getpid()
#endif
    );
    for (size_t index = 0; index < sizeof(process_id); ++index) {
        bytes[sizeof(bytes) - sizeof(process_id) + index] ^=
            static_cast<unsigned char>(process_id >> (index * 8));
    }
    char output[33];
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        std::snprintf(output + i * 2, 3, "%02x", bytes[i]);
    }
    output[32] = '\0';
    return output;
}

std::string JsonEscape(const std::string &value) {
    std::string output;
    output.reserve(value.size() + 8);
    for (unsigned char c : value) {
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
                    char escaped[7];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                    output += escaped;
                } else {
                    output.push_back(static_cast<char>(c));
                }
        }
    }
    return output;
}

enum class CachedKind { kMissing, kInteger, kText };

struct CachedRequest {
    CachedKind kind = CachedKind::kMissing;
    sqlite3_int64 integer = 0;
    std::string text;
};

class RequestFingerprint {
  public:
    explicit RequestFingerprint(const char *operation) {
        states_ = {1469598103934665603ULL, 1099511628211ULL, 7809847782465536322ULL,
                   9650029242287828579ULL};
        AddText(operation);
    }

    void AddText(const std::string &value) {
        AddLength(value.size());
        AddBytes(value.data(), value.size());
    }

    void AddInt64(sqlite3_int64 value) {
        const uint64_t bits = static_cast<uint64_t>(value);
        std::array<unsigned char, 8> bytes{};
        for (size_t index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<unsigned char>(bits >> ((bytes.size() - index - 1) * 8));
        }
        AddLength(bytes.size());
        AddBytes(bytes.data(), bytes.size());
    }

    void AddBlob(const std::vector<unsigned char> &value) {
        AddLength(value.size());
        AddBytes(value.data(), value.size());
    }

    std::vector<unsigned char> Finish() const {
        std::vector<unsigned char> output(sizeof(states_));
        std::memcpy(output.data(), states_.data(), output.size());
        return output;
    }

  private:
    void AddLength(size_t value) {
        const uint64_t length = static_cast<uint64_t>(value);
        std::array<unsigned char, 8> bytes{};
        for (size_t index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<unsigned char>(length >> ((bytes.size() - index - 1) * 8));
        }
        AddBytes(bytes.data(), bytes.size());
    }

    void AddBytes(const void *data, size_t size) {
        const auto *bytes = static_cast<const unsigned char *>(data);
        for (size_t index = 0; index < size; ++index) {
            for (size_t state = 0; state < states_.size(); ++state) {
                states_[state] ^= static_cast<uint64_t>(bytes[index] + state * 37U);
                states_[state] *= 1099511628211ULL + state * 2ULL;
            }
        }
    }

    std::array<uint64_t, 4> states_{};
};

CachedRequest FindRequest(sqlite3 *db, const std::string &request_id, const char *operation,
                          const std::vector<unsigned char> &fingerprint) {
    if (request_id.empty()) throw SqlError("request_id must not be empty", SQLITE_MISMATCH);
    Statement statement(db,
        "SELECT operation, request_fingerprint, result_integer, result_text FROM _vexfs_requests "
        "WHERE request_id = ?1");
    statement.BindText(1, request_id);
    if (!statement.Row()) return {};
    if (statement.Text(0) != operation) {
        throw SqlError("request_id was already used by another operation", SQLITE_CONSTRAINT);
    }
    if (statement.Blob(1) != fingerprint) {
        throw SqlError("request_id was reused with different arguments", SQLITE_CONSTRAINT);
    }
    if (statement.Type(3) != SQLITE_NULL) {
        return {CachedKind::kText, 0, statement.Text(3)};
    }
    return {CachedKind::kInteger, statement.Int64(2), {}};
}

void StoreRequestInteger(sqlite3 *db, const std::string &request_id, const char *operation,
                         const std::vector<unsigned char> &fingerprint, sqlite3_int64 result) {
    Statement statement(db,
        "INSERT INTO _vexfs_requests(request_id, operation, request_fingerprint, result_integer) "
        "VALUES(?1, ?2, ?3, ?4)");
    statement.BindText(1, request_id);
    statement.BindText(2, operation);
    statement.BindBlob(3, fingerprint);
    statement.BindInt64(4, result);
    statement.Done();
}

void StoreRequestText(sqlite3 *db, const std::string &request_id, const char *operation,
                      const std::vector<unsigned char> &fingerprint, const std::string &result) {
    Statement statement(db,
        "INSERT INTO _vexfs_requests(request_id, operation, request_fingerprint, result_text) "
        "VALUES(?1, ?2, ?3, ?4)");
    statement.BindText(1, request_id);
    statement.BindText(2, operation);
    statement.BindBlob(3, fingerprint);
    statement.BindText(4, result);
    statement.Done();
}

struct Handle {
    std::string id;
    sqlite3_int64 workspace_id = 0;
    sqlite3_int64 inode_id = 0;
    bool writable = false;
    sqlite3_int64 expected_version = 0;
    sqlite3_int64 dirty_generation = 0;
    sqlite3_int64 published_generation = 0;
    std::string state;
    std::string owner_session;
};

Handle FindHandle(sqlite3 *db, const std::string &id) {
    Statement statement(db,
        "SELECT id, workspace_id, inode_id, writable, expected_version, dirty_generation, "
        "published_generation, state, COALESCE(owner_session, '') "
        "FROM _vexfs_handles WHERE id = ?1");
    statement.BindText(1, id);
    if (!statement.Row()) throw SqlError("handle not found", SQLITE_NOTFOUND);
    return {statement.Text(0), statement.Int64(1), statement.Int64(2), statement.Int(3) != 0,
            statement.Int64(4), statement.Int64(5), statement.Int64(6), statement.Text(7),
            statement.Text(8)};
}

struct StagingInfo {
    sqlite3_int64 rowid = 0;
    sqlite3_int64 generation = 0;
    sqlite3_int64 logical_size = 0;
    sqlite3_int64 capacity = 0;
};

StagingInfo FindStaging(sqlite3 *db, const Handle &handle) {
    Statement statement(db,
        "SELECT d.rowid, s.generation, s.logical_size, s.capacity "
        "FROM _vexfs_staging s JOIN _vexfs_staging_data d ON d.handle_id=s.handle_id "
        "WHERE s.handle_id = ?1");
    statement.BindText(1, handle.id);
    if (!statement.Row()) throw SqlError("staging area not found", SQLITE_NOTFOUND);
    return {statement.Int64(0), statement.Int64(1), statement.Int64(2), statement.Int64(3)};
}

void WriteBlob(sqlite3 *db, sqlite3_int64 rowid, sqlite3_int64 offset,
               const void *data, sqlite3_int64 size) {
    if (size == 0) return;
    if (offset < 0 || size < 0 || offset > std::numeric_limits<int>::max() ||
        size > std::numeric_limits<int>::max() || offset + size > std::numeric_limits<int>::max()) {
        throw SqlError("staging BLOB range is out of range", SQLITE_RANGE);
    }
    sqlite3_blob *blob = nullptr;
    int rc = sqlite3_blob_open(db, "main", "_vexfs_staging_data", "content", rowid, 1, &blob);
    if (rc != SQLITE_OK) {
        throw SqlError("cannot open staging BLOB: " + std::string(sqlite3_errmsg(db)));
    }
    rc = sqlite3_blob_write(blob, data, static_cast<int>(size), static_cast<int>(offset));
    const int close_rc = sqlite3_blob_close(blob);
    if (rc != SQLITE_OK) {
        throw SqlError("cannot write staging BLOB: " + std::string(sqlite3_errmsg(db)));
    }
    if (close_rc != SQLITE_OK) {
        throw SqlError("cannot close staging BLOB: " + std::string(sqlite3_errmsg(db)));
    }
}

void ZeroBlobRange(sqlite3 *db, sqlite3_int64 rowid, sqlite3_int64 offset,
                   sqlite3_int64 size) {
    static const std::array<unsigned char, 64 * 1024> zeros{};
    if (size == 0) return;
    if (offset < 0 || size < 0 || offset > std::numeric_limits<int>::max() ||
        size > std::numeric_limits<int>::max() || offset + size > std::numeric_limits<int>::max()) {
        throw SqlError("staging zero range is out of range", SQLITE_RANGE);
    }
    sqlite3_blob *blob = nullptr;
    int rc = sqlite3_blob_open(db, "main", "_vexfs_staging_data", "content", rowid, 1, &blob);
    if (rc != SQLITE_OK) {
        throw SqlError("cannot open staging BLOB: " + std::string(sqlite3_errmsg(db)));
    }
    while (size > 0) {
        const sqlite3_int64 chunk = std::min<sqlite3_int64>(size, zeros.size());
        rc = sqlite3_blob_write(blob, zeros.data(), static_cast<int>(chunk),
                                static_cast<int>(offset));
        if (rc != SQLITE_OK) break;
        offset += chunk;
        size -= chunk;
    }
    const int close_rc = sqlite3_blob_close(blob);
    if (rc != SQLITE_OK) {
        throw SqlError("cannot zero staging BLOB: " + std::string(sqlite3_errmsg(db)));
    }
    if (close_rc != SQLITE_OK) {
        throw SqlError("cannot close staging BLOB: " + std::string(sqlite3_errmsg(db)));
    }
}

StagingInfo EnsureStagingCapacity(sqlite3 *db, const Handle &handle, StagingInfo staging,
                                  sqlite3_int64 required) {
    if (required <= staging.capacity) return staging;
    if (required > kMaxStagedBytes)
        throw SqlError("staged file is larger than 128 MiB", SQLITE_TOOBIG);
    sqlite3_int64 capacity = std::max(staging.capacity, kInitialStagingCapacity);
    while (capacity < required) capacity = std::min(kMaxStagedBytes, capacity * 2);
    Statement grow(db,
        "UPDATE _vexfs_staging_data SET content=CAST(content||zeroblob(?1) AS BLOB) "
        "WHERE handle_id=?2");
    grow.BindInt64(1, capacity - staging.capacity);
    grow.BindText(2, handle.id);
    grow.Done();
    Statement update(db,
        "UPDATE _vexfs_staging SET capacity=?1, "
        "updated_at=CAST(strftime('%s','now') AS INTEGER)*1000 WHERE handle_id=?2");
    update.BindInt64(1, capacity);
    update.BindText(2, handle.id);
    update.Done();
    staging.capacity = capacity;
    return staging;
}

void CreateStaging(sqlite3 *db, const Handle &handle,
                   const std::vector<unsigned char> &content) {
    sqlite3_int64 capacity = static_cast<sqlite3_int64>(content.size());
    if (handle.writable && capacity < kInitialStagingCapacity) capacity = kInitialStagingCapacity;
    Statement insert(db,
        "INSERT INTO _vexfs_staging(handle_id,generation,content,logical_size,capacity) "
        "VALUES(?1,?2,X'',?4,?3)");
    insert.BindText(1, handle.id);
    insert.BindInt64(2, handle.dirty_generation);
    insert.BindInt64(3, capacity);
    insert.BindInt64(4, static_cast<sqlite3_int64>(content.size()));
    insert.Done();
    Statement data(db,
        "INSERT INTO _vexfs_staging_data(handle_id,content) VALUES(?1,zeroblob(?2))");
    data.BindText(1, handle.id);
    data.BindInt64(2, capacity);
    data.Done();
    if (!content.empty()) {
        const StagingInfo staging = FindStaging(db, handle);
        WriteBlob(db, staging.rowid, 0, content.data(), static_cast<sqlite3_int64>(content.size()));
    }
}

sqlite3_int64 StageWrite(sqlite3 *db, const Handle &handle, sqlite3_int64 offset,
                         const std::vector<unsigned char> &patch) {
    if (patch.empty()) return handle.dirty_generation;
    const sqlite3_int64 required = offset + static_cast<sqlite3_int64>(patch.size());
    StagingInfo staging = EnsureStagingCapacity(db, handle, FindStaging(db, handle), required);
    if (offset > staging.logical_size) {
        ZeroBlobRange(db, staging.rowid, staging.logical_size, offset - staging.logical_size);
    }
    WriteBlob(db, staging.rowid, offset, patch.data(), static_cast<sqlite3_int64>(patch.size()));
    const sqlite3_int64 logical_size = std::max(staging.logical_size, required);
    const sqlite3_int64 generation = handle.dirty_generation + 1;
    Statement stage(db,
        "UPDATE _vexfs_staging SET generation=?1, logical_size=?2, "
        "updated_at=CAST(strftime('%s','now') AS INTEGER)*1000 WHERE handle_id=?3");
    stage.BindInt64(1, generation);
    stage.BindInt64(2, logical_size);
    stage.BindText(3, handle.id);
    stage.Done();
    Statement update(db,
        "UPDATE _vexfs_handles SET dirty_generation=?1, "
        "updated_at=CAST(strftime('%s','now') AS INTEGER)*1000 WHERE id=?2");
    update.BindInt64(1, generation);
    update.BindText(2, handle.id);
    update.Done();
    return generation;
}

sqlite3_int64 TruncateStaging(sqlite3 *db, const Handle &handle, sqlite3_int64 size) {
    if (size < 0 || size > kMaxStagedBytes)
        throw SqlError("file size is out of range", SQLITE_RANGE);
    StagingInfo staging = FindStaging(db, handle);
    if (size == staging.logical_size) return handle.dirty_generation;
    staging = EnsureStagingCapacity(db, handle, staging, size);
    if (size > staging.logical_size) {
        ZeroBlobRange(db, staging.rowid, staging.logical_size, size - staging.logical_size);
    }
    const sqlite3_int64 generation = handle.dirty_generation + 1;
    Statement stage(db,
        "UPDATE _vexfs_staging SET generation=?1, logical_size=?2, "
        "updated_at=CAST(strftime('%s','now') AS INTEGER)*1000 WHERE handle_id=?3");
    stage.BindInt64(1, generation);
    stage.BindInt64(2, size);
    stage.BindText(3, handle.id);
    stage.Done();
    Statement update(db,
        "UPDATE _vexfs_handles SET dirty_generation=?1, "
        "updated_at=CAST(strftime('%s','now') AS INTEGER)*1000 WHERE id=?2");
    update.BindInt64(1, generation);
    update.BindText(2, handle.id);
    update.Done();
    return generation;
}

std::vector<unsigned char> ReadStaging(sqlite3 *db, const Handle &handle,
                                       sqlite3_int64 generation) {
    Statement statement(db,
        "SELECT substr(d.content,1,s.logical_size) FROM _vexfs_staging s "
        "JOIN _vexfs_staging_data d ON d.handle_id=s.handle_id "
        "WHERE s.handle_id=?1 AND s.generation=?2");
    statement.BindText(1, handle.id);
    statement.BindInt64(2, generation);
    if (!statement.Row()) throw SqlError("staged generation is stale or missing", SQLITE_NOTFOUND);
    return statement.Blob(0);
}

std::vector<unsigned char> ReadStagingRange(sqlite3 *db, const Handle &handle,
                                            sqlite3_int64 offset, sqlite3_int64 length) {
    Statement statement(db,
        "SELECT CASE WHEN ?2>=logical_size THEN X'' "
        "ELSE substr(d.content,?2+1,min(?3,logical_size-?2)) END "
        "FROM _vexfs_staging s JOIN _vexfs_staging_data d ON d.handle_id=s.handle_id "
        "WHERE s.handle_id=?1");
    statement.BindText(1, handle.id);
    statement.BindInt64(2, offset);
    statement.BindInt64(3, length);
    if (!statement.Row()) throw SqlError("staging area not found", SQLITE_NOTFOUND);
    return statement.Blob(0);
}

Workspace WorkspaceById(sqlite3 *db, sqlite3_int64 id) {
    Statement statement(db,
        "SELECT id, root_inode, COALESCE(head_commit, 0) FROM _vexfs_workspaces WHERE id = ?1");
    statement.BindInt64(1, id);
    if (!statement.Row()) throw SqlError("workspace not found", SQLITE_NOTFOUND);
    return {statement.Int64(0), statement.Int64(1), statement.Int64(2)};
}

Node NodeById(sqlite3 *db, sqlite3_int64 id, bool include_deleted = false) {
    Statement statement(db,
        "SELECT id, kind, size, current_version, mode, created_at, updated_at FROM _vexfs_inodes "
        "WHERE id = ?1 AND (?2 OR deleted_at IS NULL)");
    statement.BindInt64(1, id);
    statement.BindInt(2, include_deleted ? 1 : 0);
    if (!statement.Row()) throw SqlError("inode not found", SQLITE_NOTFOUND);
    return {statement.Int64(0), statement.Text(1), statement.Int64(2),
            statement.Int64(3), statement.Int(4), statement.Int64(5), statement.Int64(6)};
}

sqlite3_int64 PublishHandle(sqlite3 *db, Handle handle, sqlite3_int64 generation) {
    if (handle.state == "closed") throw SqlError("handle is closed", SQLITE_NOTFOUND);
    if (generation <= handle.published_generation) {
        return NodeById(db, handle.inode_id, true).version;
    }
    if (generation != handle.dirty_generation) {
        throw SqlError("generation is stale or has not been staged", SQLITE_CONSTRAINT);
    }

    Node node = NodeById(db, handle.inode_id, true);
    if (node.version != handle.expected_version) {
        throw SqlError("write conflict: file changed after handle_open", SQLITE_CONSTRAINT);
    }
    const std::vector<unsigned char> content = ReadStaging(db, handle, generation);
    Workspace workspace = WorkspaceById(db, handle.workspace_id);
    Savepoint savepoint(db, "vexfs_publish_handle");
    const sqlite3_int64 version = StoreVersion(db, workspace, node, content, "handle publish");

    Statement update(db,
        "UPDATE _vexfs_handles SET expected_version = ?1, published_generation = ?2, "
        "updated_at = CAST(strftime('%s','now') AS INTEGER) * 1000 WHERE id = ?3");
    update.BindInt64(1, version);
    update.BindInt64(2, generation);
    update.BindText(3, handle.id);
    update.Done();
    savepoint.Release();
    return version;
}

void ResultBlob(sqlite3_context *context, const std::vector<unsigned char> &value) {
    const void *data = value.empty() ? static_cast<const void *>("") : value.data();
    sqlite3_result_blob64(context, data, value.size(), SQLITE_TRANSIENT);
}

template <typename Function>
void Guard(sqlite3_context *context, Function function) {
    try {
        function();
    } catch (const SqlError &error) {
        sqlite3_result_error(context, error.what(), -1);
        sqlite3_result_error_code(context, error.code());
    } catch (const std::exception &error) {
        sqlite3_result_error(context, error.what(), -1);
    } catch (...) {
        sqlite3_result_error(context, "unknown VexFS error", -1);
    }
}

void InitFunction(sqlite3_context *context, int, sqlite3_value **) {
    Guard(context, [&] {
        EnsureSchema(sqlite3_context_db_handle(context));
        sqlite3_result_int(context, 1);
    });
}

void ContractVersionFunction(sqlite3_context *context, int, sqlite3_value **) {
    sqlite3_result_text(context, kContractVersion, -1, SQLITE_STATIC);
}

void WorkspaceCreateFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string name = RequiredText(values[0], "workspace");
        if (name.empty() || name.size() > 128)
            throw SqlError("workspace name must be 1..128 bytes", SQLITE_MISMATCH);

        try {
            const Workspace existing = FindWorkspace(db, name);
            sqlite3_result_int64(context, existing.id);
            return;
        } catch (const SqlError &) {
            // Missing is expected; INSERT below still enforces the unique constraint.
        }

        Statement insert(db, "INSERT INTO _vexfs_workspaces(name) VALUES(?1)");
        insert.BindText(1, name);
        insert.Done();
        const sqlite3_int64 workspace_id = sqlite3_last_insert_rowid(db);
        const sqlite3_int64 root = CreateInode(db, workspace_id, "directory", 0755);
        Statement update(db, "UPDATE _vexfs_workspaces SET root_inode = ?1 WHERE id = ?2");
        update.BindInt64(1, root);
        update.BindInt64(2, workspace_id);
        update.Done();
        sqlite3_result_int64(context, workspace_id);
    });
}

void MkdirFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const auto parts = PathParts(RequiredText(values[1], "path"));
        Node current = RootNode(db, workspace);
        bool changed = false;
        for (const std::string &part : parts) {
            Node child;
            if (!FindChild(db, workspace.id, current.id, part, &child)) {
                child.id = CreateInode(db, workspace.id, "directory", 0755);
                child.kind = "directory";
                child.mode = 0755;
                AddDentry(db, workspace.id, current.id, part, child.id);
                changed = true;
            } else if (child.kind != "directory") {
                throw SqlError("path component is not a directory", SQLITE_MISMATCH);
            }
            current = std::move(child);
        }
        if (changed) CreateCommit(db, workspace, "mkdir");
        sqlite3_result_int64(context, current.id);
    });
}

void WriteFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const auto parts = PathParts(RequiredText(values[1], "path"));
        const auto content = RequiredBlob(values[2], "content");
        if (content.size() > static_cast<size_t>(kMaxStagedBytes)) {
            throw SqlError("file is larger than the Phase 0 limit (128 MiB)", SQLITE_TOOBIG);
        }
        sqlite3_result_int64(context, WritePath(db, workspace, parts, content, "write file"));
    });
}

void ReadFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const Node node = Resolve(db, workspace, PathParts(RequiredText(values[1], "path")));
        ResultBlob(context, ReadNode(db, node));
    });
}

void HistoryFunction(sqlite3_context *context, int argument_count, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const Node node = Resolve(db, workspace, PathParts(RequiredText(values[1], "path")));
        if (node.kind != "file") throw SqlError("path is not a file", SQLITE_MISMATCH);
        const sqlite3_int64 limit = argument_count == 2
            ? 100 : RequiredPositiveInteger(values[2], "limit");
        const sqlite3_int64 before_version = argument_count == 2
            ? 0 : RequiredNonnegativeInteger(values[3], "before version");
        if (limit > 1000) throw SqlError("history limit must be at most 1000", SQLITE_RANGE);

        Statement statement(db,
            "SELECT v.version_no, v.commit_id, c.parent_commit, v.size, v.created_at, c.message "
            "FROM _vexfs_file_versions v JOIN _vexfs_commits c ON c.id=v.commit_id "
            "WHERE v.inode_id=?1 AND (?2=0 OR v.version_no<?2) "
            "ORDER BY v.version_no DESC LIMIT ?3");
        statement.BindInt64(1, node.id);
        statement.BindInt64(2, before_version);
        statement.BindInt64(3, limit + 1);
        std::string entries = "[";
        bool first = true;
        sqlite3_int64 count = 0;
        sqlite3_int64 last_version = 0;
        bool has_more = false;
        while (statement.Row()) {
            if (count == limit) {
                has_more = true;
                break;
            }
            if (!first) entries += ',';
            first = false;
            last_version = statement.Int64(0);
            entries += "{\"version\":" + std::to_string(last_version) +
                ",\"commit\":" + std::to_string(statement.Int64(1)) +
                ",\"parent_commit\":";
            if (statement.Type(2) == SQLITE_NULL) entries += "null";
            else entries += std::to_string(statement.Int64(2));
            entries += ",\"size\":" + std::to_string(statement.Int64(3)) +
                ",\"created_at\":" + std::to_string(statement.Int64(4)) +
                ",\"message\":\"" + JsonEscape(statement.Text(5)) +
                "\",\"current\":" +
                (statement.Int64(0) == node.version ? "true" : "false") + "}";
            ++count;
        }
        entries += ']';
        const std::string json = argument_count == 2 ? entries :
            "{\"entries\":" + entries + ",\"next_before\":" +
            (has_more ? std::to_string(last_version) : "null") + "}";
        sqlite3_result_text(context, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    });
}

void ReadVersionFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const Node node = Resolve(db, workspace, PathParts(RequiredText(values[1], "path")));
        ResultBlob(context, ReadNodeVersion(db, node, RequiredPositiveInteger(values[2], "version")));
    });
}

void CompareVersionsFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string path = RequiredText(values[1], "path");
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const Node node = Resolve(db, workspace, PathParts(path));
        if (node.kind != "file") throw SqlError("path is not a file", SQLITE_MISMATCH);
        const sqlite3_int64 from_version = RequiredPositiveInteger(values[2], "from version");
        const sqlite3_int64 to_version = RequiredPositiveInteger(values[3], "to version");
        const VersionStorage from = ResolveVersionStorage(db, node.id, from_version);
        const VersionStorage to = ResolveVersionStorage(db, node.id, to_version);
        ReadOnlyBlob from_blob(db, from.rowid);
        ReadOnlyBlob to_blob(db, to.rowid);
        if (from_blob.size() != from.size || to_blob.size() != to.size) {
            throw SqlError("file version size does not match content", SQLITE_CORRUPT);
        }
        std::array<unsigned char, 64 * 1024> from_chunk{};
        std::array<unsigned char, 64 * 1024> to_chunk{};
        bool changed = from.size != to.size;
        bool binary = false;
        const sqlite3_int64 maximum = std::max(from.size, to.size);
        for (sqlite3_int64 offset = 0; offset < maximum; offset += from_chunk.size()) {
            const int from_bytes = static_cast<int>(std::min<sqlite3_int64>(
                from_chunk.size(), std::max<sqlite3_int64>(0, from.size - offset)));
            const int to_bytes = static_cast<int>(std::min<sqlite3_int64>(
                to_chunk.size(), std::max<sqlite3_int64>(0, to.size - offset)));
            if (from_bytes > 0) from_blob.Read(from_chunk.data(), from_bytes, static_cast<int>(offset));
            if (to_bytes > 0) to_blob.Read(to_chunk.data(), to_bytes, static_cast<int>(offset));
            binary = binary ||
                std::find(from_chunk.begin(), from_chunk.begin() + from_bytes, 0) !=
                    from_chunk.begin() + from_bytes ||
                std::find(to_chunk.begin(), to_chunk.begin() + to_bytes, 0) !=
                    to_chunk.begin() + to_bytes;
            if (!changed && (from_bytes != to_bytes ||
                !std::equal(from_chunk.begin(), from_chunk.begin() + from_bytes,
                            to_chunk.begin()))) changed = true;
        }
        const std::string json =
            "{\"path\":\"" + JsonEscape(path) + "\",\"from\":" +
            std::to_string(from_version) + ",\"to\":" + std::to_string(to_version) +
            ",\"changed\":" + (changed ? "true" : "false") +
            ",\"binary\":" + (binary ? "true" : "false") +
            ",\"from_size\":" + std::to_string(from.size) +
            ",\"to_size\":" + std::to_string(to.size) + "}";
        sqlite3_result_text(context, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    });
}

void RestoreVersionFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const Node node = Resolve(db, workspace, PathParts(RequiredText(values[1], "path")));
        const sqlite3_int64 target_version = RequiredPositiveInteger(values[2], "version");
        const sqlite3_int64 expected_version = RequiredPositiveInteger(values[3], "expected version");
        if (node.version != expected_version) {
            throw SqlError("version conflict: expected " + std::to_string(expected_version) +
                           ", current " + std::to_string(node.version), SQLITE_CONSTRAINT);
        }
        if (target_version == node.version) {
            throw SqlError("target version is already current", SQLITE_MISMATCH);
        }
        const sqlite3_int64 version = StoreVersionFromVersion(
            db, workspace, node, target_version,
            "restore version " + std::to_string(target_version));
        sqlite3_result_int64(context, version);
    });
}

void StatFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string path = RequiredText(values[1], "path");
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const Node node = Resolve(db, workspace, PathParts(path));
        const std::string json = "{\"path\":\"" + JsonEscape(path) + "\",\"inode\":" +
            std::to_string(node.id) + ",\"kind\":\"" + node.kind + "\",\"mode\":" +
            std::to_string(node.mode) + ",\"size\":" + std::to_string(node.size) +
            ",\"version\":" + std::to_string(node.version) + ",\"created_at\":" +
            std::to_string(node.created_at) + ",\"updated_at\":" +
            std::to_string(node.updated_at) + "}";
        sqlite3_result_text(context, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    });
}

void PathFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const std::string path = PathForInode(db, workspace, sqlite3_value_int64(values[1]));
        sqlite3_result_text(context, path.data(), static_cast<int>(path.size()), SQLITE_TRANSIENT);
    });
}

void ListFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const Node directory = Resolve(db, workspace, PathParts(RequiredText(values[1], "path")));
        if (directory.kind != "directory")
            throw SqlError("path is not a directory", SQLITE_MISMATCH);

        Statement statement(db,
            "SELECT d.name, i.id, i.kind, i.size, i.current_version, i.mode, "
            "i.created_at, i.updated_at FROM _vexfs_dentries d "
            "JOIN _vexfs_inodes i ON i.id = d.inode_id "
            "WHERE d.workspace_id = ?1 AND d.parent_inode = ?2 AND i.deleted_at IS NULL "
            "ORDER BY d.name COLLATE BINARY");
        statement.BindInt64(1, workspace.id);
        statement.BindInt64(2, directory.id);
        std::string json = "[";
        bool first = true;
        while (statement.Row()) {
            if (!first) json += ',';
            first = false;
            json += "{\"name\":\"" + JsonEscape(statement.Text(0)) + "\",\"inode\":" +
                std::to_string(statement.Int64(1)) + ",\"kind\":\"" + statement.Text(2) +
                "\",\"size\":" + std::to_string(statement.Int64(3)) + ",\"version\":" +
                std::to_string(statement.Int64(4)) + ",\"mode\":" +
                std::to_string(statement.Int(5)) + ",\"created_at\":" +
                std::to_string(statement.Int64(6)) + ",\"updated_at\":" +
                std::to_string(statement.Int64(7)) + "}";
        }
        json += ']';
        sqlite3_result_text(context, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    });
}

bool DirectoryIsEmpty(sqlite3 *db, sqlite3_int64 workspace_id, sqlite3_int64 inode) {
    Statement children(db,
        "SELECT 1 FROM _vexfs_dentries d JOIN _vexfs_inodes i ON i.id=d.inode_id "
        "WHERE d.workspace_id=?1 AND d.parent_inode=?2 AND i.deleted_at IS NULL LIMIT 1");
    children.BindInt64(1, workspace_id);
    children.BindInt64(2, inode);
    return !children.Row();
}

void TombstoneTree(sqlite3 *db, sqlite3_int64 workspace_id, sqlite3_int64 inode) {
    Statement tombstone(db, R"SQL(
WITH RECURSIVE tree(id) AS (
    SELECT ?1
    UNION ALL
    SELECT d.inode_id FROM _vexfs_dentries d JOIN tree ON d.parent_inode = tree.id
    WHERE d.workspace_id = ?2
)
UPDATE _vexfs_inodes SET deleted_at = CAST(strftime('%s','now') AS INTEGER) * 1000
WHERE workspace_id = ?2 AND id IN (SELECT id FROM tree)
)SQL");
    tombstone.BindInt64(1, inode);
    tombstone.BindInt64(2, workspace_id);
    tombstone.Done();
}

void RenameFunction(sqlite3_context *context, int argument_count, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const auto source_parts = PathParts(RequiredText(values[1], "source"));
        const auto destination_parts = PathParts(RequiredText(values[2], "destination"));
        const bool replace = argument_count == 4 && sqlite3_value_int(values[3]) != 0;
        if (source_parts.empty() || destination_parts.empty())
            throw SqlError("root cannot be moved", SQLITE_MISMATCH);
        if (source_parts == destination_parts) {
            sqlite3_result_int(context, 1);
            return;
        }
        if (destination_parts.size() > source_parts.size() &&
            std::equal(source_parts.begin(), source_parts.end(), destination_parts.begin())) {
            throw SqlError("directory cannot be moved into itself", SQLITE_MISMATCH);
        }
        auto [source_parent, source_name] = ResolveParent(db, workspace, source_parts);
        Node source;
        if (!FindChild(db, workspace.id, source_parent.id, source_name, &source)) {
            throw SqlError("source path not found", SQLITE_NOTFOUND);
        }
        auto [destination_parent, destination_name] = ResolveParent(db, workspace, destination_parts);
        Node existing;
        if (FindChild(db, workspace.id, destination_parent.id, destination_name, &existing)) {
            if (!replace) throw SqlError("destination already exists", SQLITE_CONSTRAINT);
            if (source.kind != existing.kind)
                throw SqlError("source and destination types differ", SQLITE_MISMATCH);
            if (existing.kind == "directory" && !DirectoryIsEmpty(db, workspace.id, existing.id)) {
                throw SqlError("destination directory is not empty", SQLITE_CONSTRAINT);
            }
            Statement detach(db,
                "DELETE FROM _vexfs_dentries WHERE workspace_id=?1 AND parent_inode=?2 AND name=?3");
            detach.BindInt64(1, workspace.id);
            detach.BindInt64(2, destination_parent.id);
            detach.BindText(3, destination_name);
            detach.Done();
            TombstoneTree(db, workspace.id, existing.id);
        }
        Statement update(db,
            "UPDATE _vexfs_dentries SET parent_inode = ?1, name = ?2 "
            "WHERE workspace_id = ?3 AND parent_inode = ?4 AND name = ?5");
        update.BindInt64(1, destination_parent.id);
        update.BindText(2, destination_name);
        update.BindInt64(3, workspace.id);
        update.BindInt64(4, source_parent.id);
        update.BindText(5, source_name);
        update.Done();
        TouchDirectory(db, workspace.id, source_parent.id);
        if (destination_parent.id != source_parent.id) {
            TouchDirectory(db, workspace.id, destination_parent.id);
        }
        CreateCommit(db, workspace, replace ? "rename replace" : "move");
        sqlite3_result_int(context, 1);
    });
}

void RemoveFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const auto parts = PathParts(RequiredText(values[1], "path"));
        const bool recursive = sqlite3_value_int(values[2]) != 0;
        if (parts.empty()) throw SqlError("workspace root cannot be removed", SQLITE_MISMATCH);
        auto [parent, name] = ResolveParent(db, workspace, parts);
        Node node;
        if (!FindChild(db, workspace.id, parent.id, name, &node))
            throw SqlError("path not found", SQLITE_NOTFOUND);
        if (node.kind == "directory" && !recursive &&
            !DirectoryIsEmpty(db, workspace.id, node.id)) {
            throw SqlError("directory is not empty", SQLITE_CONSTRAINT);
        }
        Statement detach(db,
            "DELETE FROM _vexfs_dentries WHERE workspace_id=?1 AND parent_inode=?2 AND name=?3");
        detach.BindInt64(1, workspace.id);
        detach.BindInt64(2, parent.id);
        detach.BindText(3, name);
        detach.Done();
        TombstoneTree(db, workspace.id, node.id);
        TouchDirectory(db, workspace.id, parent.id);
        CreateCommit(db, workspace, "remove");
        sqlite3_result_int(context, 1);
    });
}

void HandleOpenFunction(sqlite3_context *context, int argument_count, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string workspace_name = RequiredText(values[0], "workspace");
        const std::string path = RequiredText(values[1], "path");
        const std::string flags = RequiredText(values[2], "flags");
        const std::string request_id = RequiredText(values[3], "request_id");
        const std::string owner_session = argument_count == 5
            ? RequiredText(values[4], "owner_session") : std::string();
        RequestFingerprint fingerprint_builder("handle_open");
        fingerprint_builder.AddText(workspace_name);
        fingerprint_builder.AddText(path);
        fingerprint_builder.AddText(flags);
        fingerprint_builder.AddText(owner_session);
        const auto fingerprint = fingerprint_builder.Finish();
        const CachedRequest cached = FindRequest(db, request_id, "handle_open", fingerprint);
        if (cached.kind == CachedKind::kText) {
            sqlite3_result_text(context, cached.text.data(), static_cast<int>(cached.text.size()), SQLITE_TRANSIENT);
            return;
        }

        const bool writable = flags.find('w') != std::string::npos;
        const bool create = flags.find('c') != std::string::npos;
        const bool truncate = flags.find('t') != std::string::npos;
        if (flags.find('r') == std::string::npos && !writable) {
            throw SqlError("flags must contain r or w", SQLITE_MISMATCH);
        }
        if ((create || truncate) && !writable)
            throw SqlError("create/truncate requires write access", SQLITE_MISMATCH);

        Workspace workspace = FindWorkspace(db, workspace_name);
        const auto parts = PathParts(path);
        Node node;
        if (!TryResolve(db, workspace, parts, &node)) {
            if (!create) throw SqlError("path not found", SQLITE_NOTFOUND);
            WritePath(db, workspace, parts, {}, "create file");
            workspace = WorkspaceById(db, workspace.id);
            node = Resolve(db, workspace, parts);
        }
        if (node.kind != "file") throw SqlError("path is not a file", SQLITE_MISMATCH);

        const std::string handle_id = RandomHandleId();
        const sqlite3_int64 dirty_generation = truncate ? 1 : 0;
        Statement insert(db,
            "INSERT INTO _vexfs_handles(id, workspace_id, inode_id, open_flags, writable, "
            "expected_version, dirty_generation, state, owner_session) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,'open',?8)");
        insert.BindText(1, handle_id);
        insert.BindInt64(2, workspace.id);
        insert.BindInt64(3, node.id);
        insert.BindText(4, flags);
        insert.BindInt(5, writable ? 1 : 0);
        insert.BindInt64(6, node.version);
        insert.BindInt64(7, dirty_generation);
        if (argument_count == 5) insert.BindText(8, owner_session);
        else insert.BindNull(8);
        insert.Done();

        Handle handle{handle_id, workspace.id, node.id, writable, node.version,
                      dirty_generation, 0, "open", owner_session};
        CreateStaging(db, handle, truncate ? std::vector<unsigned char>() : ReadNode(db, node));
        StoreRequestText(db, request_id, "handle_open", fingerprint, handle_id);
        sqlite3_result_text(context, handle_id.data(), static_cast<int>(handle_id.size()), SQLITE_TRANSIENT);
    });
}

void HandleStageWriteFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string handle_id = RequiredText(values[0], "handle");
        const sqlite3_int64 offset = sqlite3_value_int64(values[1]);
        const auto patch = RequiredBlob(values[2], "content");
        const std::string request_id = RequiredText(values[3], "request_id");
        RequestFingerprint fingerprint_builder("handle_stage_write");
        fingerprint_builder.AddText(handle_id);
        fingerprint_builder.AddInt64(offset);
        fingerprint_builder.AddBlob(patch);
        const auto fingerprint = fingerprint_builder.Finish();
        const CachedRequest cached = FindRequest(db, request_id, "handle_stage_write", fingerprint);
        if (cached.kind == CachedKind::kInteger) {
            sqlite3_result_int64(context, cached.integer);
            return;
        }
        Handle handle = FindHandle(db, handle_id);
        if (handle.state != "open") throw SqlError("handle is not open", SQLITE_NOTFOUND);
        if (!handle.writable) throw SqlError("handle is read-only", SQLITE_READONLY);
        if (offset < 0 || offset > kMaxStagedBytes)
            throw SqlError("write offset is out of range", SQLITE_RANGE);
        if (patch.size() > static_cast<size_t>(kMaxStagedBytes - offset)) {
            throw SqlError("staged file is larger than 128 MiB", SQLITE_TOOBIG);
        }
        const sqlite3_int64 generation = StageWrite(db, handle, offset, patch);
        StoreRequestInteger(db, request_id, "handle_stage_write", fingerprint, generation);
        sqlite3_result_int64(context, generation);
    });
}

void HandleTruncateFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string handle_id = RequiredText(values[0], "handle");
        const sqlite3_int64 size = sqlite3_value_int64(values[1]);
        const std::string request_id = RequiredText(values[2], "request_id");
        RequestFingerprint fingerprint_builder("handle_truncate");
        fingerprint_builder.AddText(handle_id);
        fingerprint_builder.AddInt64(size);
        const auto fingerprint = fingerprint_builder.Finish();
        const CachedRequest cached = FindRequest(db, request_id, "handle_truncate", fingerprint);
        if (cached.kind == CachedKind::kInteger) {
            sqlite3_result_int64(context, cached.integer);
            return;
        }
        const Handle handle = FindHandle(db, handle_id);
        if (handle.state != "open") throw SqlError("handle is not open", SQLITE_NOTFOUND);
        if (!handle.writable) throw SqlError("handle is read-only", SQLITE_READONLY);
        const sqlite3_int64 generation = TruncateStaging(db, handle, size);
        StoreRequestInteger(db, request_id, "handle_truncate", fingerprint, generation);
        sqlite3_result_int64(context, generation);
    });
}

void HandleReadFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Handle handle = FindHandle(db, RequiredText(values[0], "handle"));
        if (handle.state == "closed") throw SqlError("handle is closed", SQLITE_NOTFOUND);
        const sqlite3_int64 offset = sqlite3_value_int64(values[1]);
        const sqlite3_int64 length = sqlite3_value_int64(values[2]);
        if (offset < 0 || length < 0)
            throw SqlError("offset and length must be non-negative", SQLITE_RANGE);
        ResultBlob(context, ReadStagingRange(db, handle, offset, length));
    });
}

void HandlePublishFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string handle_id = RequiredText(values[0], "handle");
        const sqlite3_int64 generation = sqlite3_value_int64(values[1]);
        const std::string durability = RequiredText(values[2], "durability");
        const std::string request_id = RequiredText(values[3], "request_id");
        RequestFingerprint fingerprint_builder("handle_publish");
        fingerprint_builder.AddText(handle_id);
        fingerprint_builder.AddInt64(generation);
        fingerprint_builder.AddText(durability);
        const auto fingerprint = fingerprint_builder.Finish();
        const CachedRequest cached = FindRequest(db, request_id, "handle_publish", fingerprint);
        if (cached.kind == CachedKind::kInteger) {
            sqlite3_result_int64(context, cached.integer);
            return;
        }
        if (durability != "none" && durability != "data" && durability != "full") {
            throw SqlError("durability must be none, data, or full", SQLITE_MISMATCH);
        }
        const Handle handle = FindHandle(db, handle_id);
        const sqlite3_int64 version = PublishHandle(db, handle, generation);
        StoreRequestInteger(db, request_id, "handle_publish", fingerprint, version);
        sqlite3_result_int64(context, version);
    });
}

void HandleCloseFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string handle_id = RequiredText(values[0], "handle");
        const bool retain_requested = sqlite3_value_int(values[1]) != 0;
        const std::string request_id = RequiredText(values[2], "request_id");
        RequestFingerprint fingerprint_builder("handle_close");
        fingerprint_builder.AddText(handle_id);
        fingerprint_builder.AddInt64(retain_requested ? 1 : 0);
        const auto fingerprint = fingerprint_builder.Finish();
        const CachedRequest cached = FindRequest(db, request_id, "handle_close", fingerprint);
        if (cached.kind == CachedKind::kText) {
            sqlite3_result_text(context, cached.text.data(), static_cast<int>(cached.text.size()), SQLITE_TRANSIENT);
            return;
        }
        const Handle handle = FindHandle(db, handle_id);
        const bool retain = retain_requested &&
                            handle.dirty_generation > handle.published_generation;
        const std::string state = retain ? "retained" : "closed";
        Statement update(db,
            "UPDATE _vexfs_handles SET state=?1, "
            "lease_until=CASE WHEN ?2 THEN (CAST(strftime('%s','now') AS INTEGER)+86400)*1000 "
            "ELSE NULL END, updated_at=CAST(strftime('%s','now') AS INTEGER)*1000 WHERE id=?3");
        update.BindText(1, state);
        update.BindInt(2, retain ? 1 : 0);
        update.BindText(3, handle.id);
        update.Done();
        if (!retain) {
            Statement clear_data(db, "DELETE FROM _vexfs_staging_data WHERE handle_id=?1");
            clear_data.BindText(1, handle.id);
            clear_data.Done();
            Statement clear(db, "DELETE FROM _vexfs_staging WHERE handle_id=?1");
            clear.BindText(1, handle.id);
            clear.Done();
        }
        StoreRequestText(db, request_id, "handle_close", fingerprint, state);
        sqlite3_result_text(context, state.data(), static_cast<int>(state.size()), SQLITE_TRANSIENT);
    });
}

void RetainOpenHandles(sqlite3 *db, const Workspace &workspace,
                       const std::string &session_id, bool matching_session) {
    const char *predicate = matching_session
        ? "owner_session=?2"
        : "owner_session IS NOT NULL AND owner_session<>?2";
    const std::string sql =
        "UPDATE _vexfs_handles SET "
        "state=CASE WHEN dirty_generation>published_generation THEN 'retained' ELSE 'closed' END, "
        "lease_until=CASE WHEN dirty_generation>published_generation "
        "THEN (CAST(strftime('%s','now') AS INTEGER)+86400)*1000 ELSE NULL END, "
        "updated_at=CAST(strftime('%s','now') AS INTEGER)*1000 "
        "WHERE workspace_id=?1 AND state='open' AND " + std::string(predicate);
    Statement update(db, sql.c_str());
    update.BindInt64(1, workspace.id);
    update.BindText(2, session_id);
    update.Done();
    Statement clear_data(db,
        "DELETE FROM _vexfs_staging_data WHERE handle_id IN "
        "(SELECT id FROM _vexfs_handles WHERE workspace_id=?1 AND state='closed')");
    clear_data.BindInt64(1, workspace.id);
    clear_data.Done();
    Statement clear(db,
        "DELETE FROM _vexfs_staging WHERE handle_id IN "
        "(SELECT id FROM _vexfs_handles WHERE workspace_id=?1 AND state='closed')");
    clear.BindInt64(1, workspace.id);
    clear.Done();
}

void ClaimRetainedHandles(sqlite3 *db, const Workspace &workspace,
                          const std::string &session_id) {
    Statement claim(db,
        "UPDATE _vexfs_handles SET state='open',owner_session=?1,lease_until=NULL,"
        "updated_at=CAST(strftime('%s','now') AS INTEGER)*1000 "
        "WHERE workspace_id=?2 AND state='retained' "
        "AND dirty_generation>published_generation");
    claim.BindText(1, session_id);
    claim.BindInt64(2, workspace.id);
    claim.Done();
}

void SessionStartFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const std::string session_id = RequiredText(values[1], "session_id");
        if (session_id.empty()) throw SqlError("session_id must not be empty", SQLITE_MISMATCH);
        Savepoint savepoint(db, "vexfs_session_start");
        Statement current(db,
            "SELECT session_id,lease_until>CAST(strftime('%s','now') AS INTEGER)*1000 "
            "FROM _vexfs_mount_sessions WHERE workspace_id=?1");
        current.BindInt64(1, workspace.id);
        std::string previous_session;
        bool active = false;
        if (current.Row()) {
            previous_session = current.Text(0);
            active = current.Int(1) != 0;
        }
        if (active && previous_session != session_id) {
            throw SqlError("workspace already has an active mount session", SQLITE_BUSY);
        }
        if (!previous_session.empty() && previous_session != session_id) {
            RetainOpenHandles(db, workspace, previous_session, true);
        }
        ClaimRetainedHandles(db, workspace, session_id);
        Statement upsert(db,
            "INSERT INTO _vexfs_mount_sessions(workspace_id,session_id,lease_until) "
            "VALUES(?1,?2,(CAST(strftime('%s','now') AS INTEGER)+30)*1000) "
            "ON CONFLICT(workspace_id) DO UPDATE SET session_id=excluded.session_id,"
            "lease_until=excluded.lease_until,"
            "updated_at=CAST(strftime('%s','now') AS INTEGER)*1000");
        upsert.BindInt64(1, workspace.id);
        upsert.BindText(2, session_id);
        upsert.Done();
        savepoint.Release();
        sqlite3_result_int(context, 1);
    });
}

void SessionHeartbeatFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const std::string session_id = RequiredText(values[1], "session_id");
        if (session_id.empty()) throw SqlError("session_id must not be empty", SQLITE_MISMATCH);
        Statement update(db,
            "UPDATE _vexfs_mount_sessions SET "
            "lease_until=(CAST(strftime('%s','now') AS INTEGER)+30)*1000,"
            "updated_at=CAST(strftime('%s','now') AS INTEGER)*1000 "
            "WHERE workspace_id=?1 AND session_id=?2");
        update.BindInt64(1, workspace.id);
        update.BindText(2, session_id);
        update.Done();
        if (sqlite3_changes64(db) != 1) {
            throw SqlError("mount session is stale", SQLITE_BUSY);
        }
        sqlite3_result_int(context, 1);
    });
}

void SessionEndFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const std::string session_id = RequiredText(values[1], "session_id");
        if (session_id.empty()) throw SqlError("session_id must not be empty", SQLITE_MISMATCH);
        Savepoint savepoint(db, "vexfs_session_end");
        RetainOpenHandles(db, workspace, session_id, true);
        Statement remove(db,
            "DELETE FROM _vexfs_mount_sessions WHERE workspace_id=?1 AND session_id=?2");
        remove.BindInt64(1, workspace.id);
        remove.BindText(2, session_id);
        remove.Done();
        savepoint.Release();
        sqlite3_result_int(context, 1);
    });
}

void SynchronizeFunction(sqlite3_context *context, int argument_count, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string workspace_name = RequiredText(values[0], "workspace");
        const std::string request_id = RequiredText(values[1], "request_id");
        const std::string owner_session = argument_count == 3
            ? RequiredText(values[2], "owner_session") : std::string();
        RequestFingerprint fingerprint_builder("mount_synchronize");
        fingerprint_builder.AddText(workspace_name);
        fingerprint_builder.AddText(owner_session);
        const auto fingerprint = fingerprint_builder.Finish();
        const CachedRequest cached = FindRequest(db, request_id, "mount_synchronize", fingerprint);
        if (cached.kind == CachedKind::kInteger) {
            sqlite3_result_int64(context, cached.integer);
            return;
        }
        const Workspace workspace = FindWorkspace(db, workspace_name);
        const char *sql = argument_count == 3
            ? "SELECT id, dirty_generation FROM _vexfs_handles WHERE workspace_id=?1 "
              "AND state='open' AND owner_session=?2 "
              "AND dirty_generation>published_generation ORDER BY id"
            : "SELECT id, dirty_generation FROM _vexfs_handles WHERE workspace_id=?1 "
              "AND state IN ('open','retained') AND owner_session IS NULL "
              "AND dirty_generation>published_generation ORDER BY id";
        Statement query(db, sql);
        query.BindInt64(1, workspace.id);
        if (argument_count == 3) query.BindText(2, owner_session);
        std::vector<std::pair<std::string, sqlite3_int64>> pending;
        while (query.Row()) pending.emplace_back(query.Text(0), query.Int64(1));
        for (const auto &item : pending) PublishHandle(db, FindHandle(db, item.first), item.second);
        StoreRequestInteger(db, request_id, "mount_synchronize", fingerprint,
                            static_cast<sqlite3_int64>(pending.size()));
        sqlite3_result_int64(context, static_cast<sqlite3_int64>(pending.size()));
    });
}

void ReclaimFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string workspace_name = RequiredText(values[0], "workspace");
        const std::string request_id = RequiredText(values[1], "request_id");
        RequestFingerprint fingerprint_builder("item_reclaim");
        fingerprint_builder.AddText(workspace_name);
        const auto fingerprint = fingerprint_builder.Finish();
        const CachedRequest cached = FindRequest(db, request_id, "item_reclaim", fingerprint);
        if (cached.kind == CachedKind::kInteger) {
            sqlite3_result_int64(context, cached.integer);
            return;
        }
        const Workspace workspace = FindWorkspace(db, workspace_name);
        Statement query(db,
            "SELECT id FROM _vexfs_handles WHERE workspace_id=?1 AND state='closed'");
        query.BindInt64(1, workspace.id);
        std::vector<std::string> handles;
        while (query.Row()) handles.push_back(query.Text(0));
        for (const std::string &handle : handles) {
            Statement clear_data(db, "DELETE FROM _vexfs_staging_data WHERE handle_id=?1");
            clear_data.BindText(1, handle);
            clear_data.Done();
            Statement clear(db, "DELETE FROM _vexfs_staging WHERE handle_id=?1");
            clear.BindText(1, handle);
            clear.Done();
            Statement remove(db, "DELETE FROM _vexfs_handles WHERE id=?1");
            remove.BindText(1, handle);
            remove.Done();
        }
        StoreRequestInteger(db, request_id, "item_reclaim", fingerprint,
                            static_cast<sqlite3_int64>(handles.size()));
        sqlite3_result_int64(context, static_cast<sqlite3_int64>(handles.size()));
    });
}

struct FunctionDefinition {
    const char *name;
    int arguments;
    void (*function)(sqlite3_context *, int, sqlite3_value **);
    int flags;
};

}  // namespace

extern "C" int vexfs_sqlite_register(sqlite3 *db) {
    static const FunctionDefinition functions[] = {
        {"vexfs_init", 0, InitFunction, SQLITE_UTF8},
        {"vexfs_contract_version", 0, ContractVersionFunction,
         SQLITE_UTF8 | SQLITE_DETERMINISTIC},
        {"vexfs_workspace_create", 1, WorkspaceCreateFunction, SQLITE_UTF8},
        {"vexfs_mkdir", 2, MkdirFunction, SQLITE_UTF8},
        {"vexfs_write", 3, WriteFunction, SQLITE_UTF8},
        {"vexfs_read", 2, ReadFunction, SQLITE_UTF8},
        {"vexfs_history", 2, HistoryFunction, SQLITE_UTF8},
        {"vexfs_history", 4, HistoryFunction, SQLITE_UTF8},
        {"vexfs_read_version", 3, ReadVersionFunction, SQLITE_UTF8},
        {"vexfs_compare_versions", 4, CompareVersionsFunction, SQLITE_UTF8},
        {"vexfs_restore_version", 4, RestoreVersionFunction, SQLITE_UTF8},
        {"vexfs_stat", 2, StatFunction, SQLITE_UTF8},
        {"vexfs_path", 2, PathFunction, SQLITE_UTF8},
        {"vexfs_list", 2, ListFunction, SQLITE_UTF8},
        {"vexfs_move", 3, RenameFunction, SQLITE_UTF8},
        {"vexfs_rename", 4, RenameFunction, SQLITE_UTF8},
        {"vexfs_remove", 3, RemoveFunction, SQLITE_UTF8},
        {"vexfs_handle_open", 4, HandleOpenFunction, SQLITE_UTF8},
        {"vexfs_handle_open", 5, HandleOpenFunction, SQLITE_UTF8},
        {"vexfs_handle_stage_write", 4, HandleStageWriteFunction, SQLITE_UTF8},
        {"vexfs_handle_truncate", 3, HandleTruncateFunction, SQLITE_UTF8},
        {"vexfs_handle_read", 3, HandleReadFunction, SQLITE_UTF8},
        {"vexfs_handle_publish", 4, HandlePublishFunction, SQLITE_UTF8},
        {"vexfs_handle_close", 3, HandleCloseFunction, SQLITE_UTF8},
        {"vexfs_mount_session_start", 2, SessionStartFunction, SQLITE_UTF8},
        {"vexfs_mount_session_heartbeat", 2, SessionHeartbeatFunction, SQLITE_UTF8},
        {"vexfs_mount_session_end", 2, SessionEndFunction, SQLITE_UTF8},
        {"vexfs_mount_synchronize", 2, SynchronizeFunction, SQLITE_UTF8},
        {"vexfs_mount_synchronize", 3, SynchronizeFunction, SQLITE_UTF8},
        {"vexfs_item_reclaim", 2, ReclaimFunction, SQLITE_UTF8},
    };
    for (const auto &definition : functions) {
        const int rc = sqlite3_create_function(db, definition.name, definition.arguments,
                                               definition.flags, nullptr, definition.function,
                                               nullptr, nullptr);
        if (rc != SQLITE_OK) return rc;
    }
    return SQLITE_OK;
}
