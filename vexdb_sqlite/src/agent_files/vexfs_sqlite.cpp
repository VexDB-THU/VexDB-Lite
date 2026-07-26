#include "vexdb_sqlite_internal.h"

#include "agent_files/vexfs_sqlite.h"
#include "vexfs_checksum.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr const char *kContractVersion = "0.9.0";
constexpr sqlite3_int64 kMaxStagedBytes = 128LL * 1024LL * 1024LL;
constexpr sqlite3_int64 kContentChunkBytes = 64LL * 1024LL;
constexpr sqlite3_int64 kMaxXattrBytes = 64LL * 1024LL;
constexpr sqlite3_int64 kMaxSymlinkBytes = 4096;
constexpr sqlite3_int64 kRequestRetentionRows = 64LL * 1024LL;
constexpr sqlite3_int64 kRequestPruneInterval = 4LL * 1024LL;
constexpr sqlite3_int64 kMaxGrepResults = 10LL * 1024LL;
constexpr size_t kMaxGrepPatternBytes = 4u * 1024u;

std::mutex g_schema_ready_mutex;
std::unordered_set<sqlite3 *> g_schema_ready_connections;
enum class GrepIndexState { kDisabled, kAvailable, kUnavailable };
std::unordered_map<sqlite3 *, GrepIndexState> g_grep_index_connections;

bool IsSha256(const std::string &value);

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
        BindBlobView(index, value.empty() ? nullptr : value.data(), value.size());
    }

    void BindBlobView(int index, const void *data, size_t size) {
        const void *bound = size == 0 ? static_cast<const void *>("") : data;
        Check(sqlite3_bind_blob64(stmt_, index, bound, size, SQLITE_STATIC),
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

    void Reset() {
        Check(sqlite3_reset(stmt_), "reset");
        Check(sqlite3_clear_bindings(stmt_), "clear bindings");
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

    const unsigned char *BlobData(int column) const {
        return static_cast<const unsigned char *>(sqlite3_column_blob(stmt_, column));
    }

    int BlobBytes(int column) const { return sqlite3_column_bytes(stmt_, column); }

    bool BlobEquals(int column, const void *data, size_t size) const {
        const int bytes = BlobBytes(column);
        if (bytes < 0 || static_cast<size_t>(bytes) != size) return false;
        if (size == 0) return true;
        const unsigned char *stored = BlobData(column);
        return stored != nullptr && data != nullptr &&
               std::memcmp(stored, data, size) == 0;
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

bool HasTable(sqlite3 *db, const char *table) {
    Statement statement(db,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1");
    statement.BindText(1, table);
    return statement.Row();
}

void CreateHistorySchema(sqlite3 *db) {
    Exec(db, R"SQL(
CREATE TABLE IF NOT EXISTS _vexfs_inode_states(
    workspace_id INTEGER NOT NULL,
    inode_id INTEGER NOT NULL,
    commit_id INTEGER NOT NULL,
    kind TEXT NOT NULL CHECK(kind IN ('file','directory','symlink')),
    mode INTEGER NOT NULL,
    owner_principal TEXT NOT NULL DEFAULT 'local',
    uid INTEGER NOT NULL DEFAULT 0,
    gid INTEGER NOT NULL DEFAULT 0,
    size INTEGER NOT NULL,
    current_version INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    accessed_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    changed_at INTEGER NOT NULL,
    deleted_at INTEGER,
    PRIMARY KEY(workspace_id, inode_id, commit_id)
);
CREATE INDEX IF NOT EXISTS _vexfs_inode_states_commit_idx
    ON _vexfs_inode_states(workspace_id, commit_id, inode_id);
CREATE TABLE IF NOT EXISTS _vexfs_dentry_states(
    workspace_id INTEGER NOT NULL,
    parent_inode INTEGER NOT NULL,
    name TEXT NOT NULL COLLATE BINARY,
    commit_id INTEGER NOT NULL,
    inode_id INTEGER NOT NULL,
    deleted INTEGER NOT NULL CHECK(deleted IN (0,1)),
    PRIMARY KEY(workspace_id, parent_inode, name, commit_id)
);
CREATE INDEX IF NOT EXISTS _vexfs_dentry_states_commit_idx
    ON _vexfs_dentry_states(workspace_id, commit_id, parent_inode, name);
CREATE TABLE IF NOT EXISTS _vexfs_xattr_states(
    workspace_id INTEGER NOT NULL,
    inode_id INTEGER NOT NULL,
    name TEXT NOT NULL COLLATE BINARY,
    commit_id INTEGER NOT NULL,
    value BLOB NOT NULL,
    deleted INTEGER NOT NULL CHECK(deleted IN (0,1)),
    PRIMARY KEY(workspace_id, inode_id, name, commit_id)
);
CREATE INDEX IF NOT EXISTS _vexfs_xattr_states_commit_idx
    ON _vexfs_xattr_states(workspace_id, commit_id, inode_id, name);
CREATE TABLE IF NOT EXISTS _vexfs_acl_states(
    workspace_id INTEGER NOT NULL,
    inode_id INTEGER NOT NULL,
    principal_id TEXT NOT NULL COLLATE BINARY,
    effect TEXT NOT NULL,
    commit_id INTEGER NOT NULL,
    permissions TEXT NOT NULL,
    inherit_flags INTEGER NOT NULL DEFAULT 0,
    deleted INTEGER NOT NULL CHECK(deleted IN (0,1)),
    PRIMARY KEY(workspace_id,inode_id,principal_id,effect,commit_id)
);
CREATE INDEX IF NOT EXISTS _vexfs_acl_states_commit_idx
    ON _vexfs_acl_states(workspace_id,commit_id,inode_id,principal_id,effect);
CREATE TABLE IF NOT EXISTS _vexfs_snapshots(
    id INTEGER PRIMARY KEY,
    workspace_id INTEGER NOT NULL,
    name TEXT NOT NULL COLLATE BINARY,
    commit_id INTEGER NOT NULL,
    created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    UNIQUE(workspace_id, name)
);
CREATE INDEX IF NOT EXISTS _vexfs_snapshots_commit_idx
    ON _vexfs_snapshots(workspace_id, commit_id);

CREATE TABLE IF NOT EXISTS _vexfs_dirty_inodes(
    workspace_id INTEGER NOT NULL,
    inode_id INTEGER NOT NULL,
    PRIMARY KEY(workspace_id, inode_id)
);
CREATE TABLE IF NOT EXISTS _vexfs_dirty_dentries(
    workspace_id INTEGER NOT NULL,
    parent_inode INTEGER NOT NULL,
    name TEXT NOT NULL COLLATE BINARY,
    inode_id INTEGER NOT NULL,
    PRIMARY KEY(workspace_id, parent_inode, name)
);
CREATE TABLE IF NOT EXISTS _vexfs_dirty_xattrs(
    workspace_id INTEGER NOT NULL,
    inode_id INTEGER NOT NULL,
    name TEXT NOT NULL COLLATE BINARY,
    PRIMARY KEY(workspace_id, inode_id, name)
);
CREATE TABLE IF NOT EXISTS _vexfs_dirty_acl(
    workspace_id INTEGER NOT NULL,
    inode_id INTEGER NOT NULL,
    principal_id TEXT NOT NULL COLLATE BINARY,
    effect TEXT NOT NULL,
    PRIMARY KEY(workspace_id,inode_id,principal_id,effect)
);

CREATE TRIGGER IF NOT EXISTS _vexfs_history_inode_insert
AFTER INSERT ON _vexfs_inodes BEGIN
    INSERT INTO _vexfs_dirty_inodes(workspace_id,inode_id)
    VALUES(NEW.workspace_id,NEW.id)
    ON CONFLICT(workspace_id,inode_id) DO NOTHING;
END;
CREATE TRIGGER IF NOT EXISTS _vexfs_history_inode_update
AFTER UPDATE ON _vexfs_inodes BEGIN
    INSERT INTO _vexfs_dirty_inodes(workspace_id,inode_id)
    VALUES(NEW.workspace_id,NEW.id)
    ON CONFLICT(workspace_id,inode_id) DO NOTHING;
END;
CREATE TRIGGER IF NOT EXISTS _vexfs_history_dentry_insert
AFTER INSERT ON _vexfs_dentries BEGIN
    INSERT INTO _vexfs_dirty_dentries(workspace_id,parent_inode,name,inode_id)
    VALUES(NEW.workspace_id,NEW.parent_inode,NEW.name,NEW.inode_id)
    ON CONFLICT(workspace_id,parent_inode,name) DO UPDATE SET inode_id=excluded.inode_id;
END;
CREATE TRIGGER IF NOT EXISTS _vexfs_history_dentry_update_old
AFTER UPDATE ON _vexfs_dentries BEGIN
    INSERT INTO _vexfs_dirty_dentries(workspace_id,parent_inode,name,inode_id)
    VALUES(OLD.workspace_id,OLD.parent_inode,OLD.name,OLD.inode_id)
    ON CONFLICT(workspace_id,parent_inode,name) DO UPDATE SET inode_id=excluded.inode_id;
END;
CREATE TRIGGER IF NOT EXISTS _vexfs_history_dentry_update_new
AFTER UPDATE ON _vexfs_dentries BEGIN
    INSERT INTO _vexfs_dirty_dentries(workspace_id,parent_inode,name,inode_id)
    VALUES(NEW.workspace_id,NEW.parent_inode,NEW.name,NEW.inode_id)
    ON CONFLICT(workspace_id,parent_inode,name) DO UPDATE SET inode_id=excluded.inode_id;
END;
CREATE TRIGGER IF NOT EXISTS _vexfs_history_dentry_delete
AFTER DELETE ON _vexfs_dentries BEGIN
    INSERT INTO _vexfs_dirty_dentries(workspace_id,parent_inode,name,inode_id)
    VALUES(OLD.workspace_id,OLD.parent_inode,OLD.name,OLD.inode_id)
    ON CONFLICT(workspace_id,parent_inode,name) DO UPDATE SET inode_id=excluded.inode_id;
END;
CREATE TRIGGER IF NOT EXISTS _vexfs_history_xattr_insert
AFTER INSERT ON _vexfs_xattrs BEGIN
    INSERT INTO _vexfs_dirty_xattrs(workspace_id,inode_id,name)
    SELECT workspace_id,NEW.inode_id,NEW.name FROM _vexfs_inodes WHERE id=NEW.inode_id
    ON CONFLICT(workspace_id,inode_id,name) DO NOTHING;
END;
CREATE TRIGGER IF NOT EXISTS _vexfs_history_xattr_update
AFTER UPDATE ON _vexfs_xattrs BEGIN
    INSERT INTO _vexfs_dirty_xattrs(workspace_id,inode_id,name)
    SELECT workspace_id,NEW.inode_id,NEW.name FROM _vexfs_inodes WHERE id=NEW.inode_id
    ON CONFLICT(workspace_id,inode_id,name) DO NOTHING;
END;
CREATE TRIGGER IF NOT EXISTS _vexfs_history_xattr_delete
AFTER DELETE ON _vexfs_xattrs BEGIN
    INSERT INTO _vexfs_dirty_xattrs(workspace_id,inode_id,name)
    SELECT workspace_id,OLD.inode_id,OLD.name FROM _vexfs_inodes WHERE id=OLD.inode_id
    ON CONFLICT(workspace_id,inode_id,name) DO NOTHING;
END;
)SQL");
}

void EnsureAclHistoryTriggers(sqlite3 *db) {
    Exec(db, R"SQL(
CREATE TABLE IF NOT EXISTS _vexfs_dirty_acl(
    workspace_id INTEGER NOT NULL,
    inode_id INTEGER NOT NULL,
    principal_id TEXT NOT NULL COLLATE BINARY,
    effect TEXT NOT NULL,
    PRIMARY KEY(workspace_id,inode_id,principal_id,effect)
);
CREATE TRIGGER IF NOT EXISTS _vexfs_history_acl_insert
AFTER INSERT ON _vexfs_acl_entries BEGIN
    INSERT INTO _vexfs_dirty_acl(workspace_id,inode_id,principal_id,effect)
    VALUES(NEW.workspace_id,NEW.inode_id,NEW.principal_id,NEW.effect)
    ON CONFLICT(workspace_id,inode_id,principal_id,effect) DO NOTHING;
END;
CREATE TRIGGER IF NOT EXISTS _vexfs_history_acl_update
AFTER UPDATE ON _vexfs_acl_entries BEGIN
    INSERT INTO _vexfs_dirty_acl(workspace_id,inode_id,principal_id,effect)
    VALUES(NEW.workspace_id,NEW.inode_id,NEW.principal_id,NEW.effect)
    ON CONFLICT(workspace_id,inode_id,principal_id,effect) DO NOTHING;
END;
CREATE TRIGGER IF NOT EXISTS _vexfs_history_acl_delete
AFTER DELETE ON _vexfs_acl_entries BEGIN
    INSERT INTO _vexfs_dirty_acl(workspace_id,inode_id,principal_id,effect)
    VALUES(OLD.workspace_id,OLD.inode_id,OLD.principal_id,OLD.effect)
    ON CONFLICT(workspace_id,inode_id,principal_id,effect) DO NOTHING;
END;
)SQL");
}

bool GrepIndexMarker(sqlite3 *db) {
    Statement marker(db,
        "SELECT 1 FROM _vexfs_meta WHERE key='grep_index' AND value='trigram-v1' LIMIT 1");
    return marker.Row();
}

bool GrepIndexDirty(sqlite3 *db) {
    Statement marker(db,
        "SELECT 1 FROM _vexfs_meta WHERE key='grep_index_dirty' AND value='1' LIMIT 1");
    return marker.Row();
}

void SetGrepIndexState(sqlite3 *db, GrepIndexState state) {
    std::lock_guard<std::mutex> lock(g_schema_ready_mutex);
    g_grep_index_connections[db] = state;
}

GrepIndexState GetGrepIndexState(sqlite3 *db) {
    std::lock_guard<std::mutex> lock(g_schema_ready_mutex);
    const auto found = g_grep_index_connections.find(db);
    return found == g_grep_index_connections.end() ? GrepIndexState::kDisabled : found->second;
}

std::vector<unsigned char> ReadVersionContent(sqlite3 *db, sqlite3_int64 inode_id,
                                              sqlite3_int64 version);

void RebuildGrepIndex(sqlite3 *db) {
    Savepoint savepoint(db, "vexfs_grep_index_rebuild");
    Exec(db, "DELETE FROM _vexfs_content_fts");
    struct IndexedFile {
        sqlite3_int64 inode_id;
        sqlite3_int64 workspace_id;
        sqlite3_int64 version;
    };
    std::vector<IndexedFile> files;
    {
        Statement rows(db, R"SQL(
SELECT id,workspace_id,current_version FROM _vexfs_inodes
WHERE kind='file' AND deleted_at IS NULL
)SQL");
        while (rows.Row()) {
            files.push_back({rows.Int64(0), rows.Int64(1), rows.Int64(2)});
        }
    }
    Statement insert(db, R"SQL(
INSERT INTO _vexfs_content_fts(rowid,workspace_id,inode_id,version_no,content)
VALUES(?1,?2,?1,?3,?4)
)SQL");
    for (const IndexedFile &file : files) {
        const std::vector<unsigned char> content =
            ReadVersionContent(db, file.inode_id, file.version);
        insert.BindInt64(1, file.inode_id);
        insert.BindInt64(2, file.workspace_id);
        insert.BindInt64(3, file.version);
        insert.BindText(4, content.empty() ? std::string() :
            std::string(reinterpret_cast<const char *>(content.data()), content.size()));
        insert.Done();
        insert.Reset();
    }
    Exec(db, "INSERT OR REPLACE INTO _vexfs_meta(key,value) "
             "VALUES('grep_index_dirty','0')");
    savepoint.Release();
}

void DetectGrepIndex(sqlite3 *db) {
    if (!GrepIndexMarker(db)) {
        SetGrepIndexState(db, GrepIndexState::kDisabled);
        return;
    }
    if (sqlite3_compileoption_used("ENABLE_FTS5") == 0 ||
        !HasTable(db, "_vexfs_content_fts")) {
        SetGrepIndexState(db, GrepIndexState::kUnavailable);
        return;
    }
    if (GrepIndexDirty(db)) {
        if (sqlite3_db_readonly(db, "main") == 1) {
            SetGrepIndexState(db, GrepIndexState::kUnavailable);
            return;
        }
        RebuildGrepIndex(db);
    }
    SetGrepIndexState(db, GrepIndexState::kAvailable);
}

void EnableGrepIndex(sqlite3 *db) {
    if (sqlite3_compileoption_used("ENABLE_FTS5") == 0) {
        throw SqlError("this SQLite host does not provide FTS5", SQLITE_MISMATCH);
    }
    const GrepIndexState previous_state = GetGrepIndexState(db);
    Savepoint savepoint(db, "vexfs_grep_index_enable");
    try {
        Exec(db, R"SQL(
CREATE VIRTUAL TABLE IF NOT EXISTS _vexfs_content_fts USING fts5(
    workspace_id UNINDEXED,
    inode_id UNINDEXED,
    version_no UNINDEXED,
    content,
    tokenize='trigram'
);
INSERT OR REPLACE INTO _vexfs_meta(key,value) VALUES('grep_index','trigram-v1');
INSERT OR REPLACE INTO _vexfs_meta(key,value) VALUES('grep_index_dirty','1');
)SQL");
        SetGrepIndexState(db, GrepIndexState::kAvailable);
        RebuildGrepIndex(db);
        savepoint.Release();
    } catch (...) {
        SetGrepIndexState(db, previous_state);
        throw;
    }
}

void DisableGrepIndex(sqlite3 *db) {
    if (GrepIndexMarker(db) && sqlite3_compileoption_used("ENABLE_FTS5") == 0) {
        throw SqlError("cannot remove the FTS5 index from this SQLite host", SQLITE_MISMATCH);
    }
    Savepoint savepoint(db, "vexfs_grep_index_disable");
    Exec(db, R"SQL(
DROP TABLE IF EXISTS _vexfs_content_fts;
DELETE FROM _vexfs_meta WHERE key IN ('grep_index','grep_index_dirty');
)SQL");
    savepoint.Release();
    SetGrepIndexState(db, GrepIndexState::kDisabled);
}

void EnsureSchemaSlow(sqlite3 *db) {
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
        if (version != kContractVersion)
            throw SqlError("unsupported VexFS schema version: " + version, SQLITE_MISMATCH);
        Statement layout(db,
            "SELECT value FROM _vexfs_meta WHERE key='staging_layout' LIMIT 1");
        if (!layout.Row() || layout.Text(0) != "overlay-v1") {
            throw SqlError("unsupported VexFS staging layout; create a new database",
                           SQLITE_MISMATCH);
        }
        DetectGrepIndex(db);
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
    state TEXT NOT NULL DEFAULT 'active' CHECK(state IN ('active','importing')),
    root_inode INTEGER,
    head_commit INTEGER,
    history_floor_commit INTEGER,
    retention_keep_versions INTEGER NOT NULL DEFAULT 32
        CHECK(retention_keep_versions >= 0),
    retention_keep_days INTEGER NOT NULL DEFAULT 30
        CHECK(retention_keep_days >= 0),
    gc_paused INTEGER NOT NULL DEFAULT 0 CHECK(gc_paused IN (0,1)),
    quota_max_bytes INTEGER CHECK(quota_max_bytes IS NULL OR quota_max_bytes >= 0),
    quota_max_files INTEGER CHECK(quota_max_files IS NULL OR quota_max_files >= 0),
    quota_max_file_bytes INTEGER
        CHECK(quota_max_file_bytes IS NULL OR quota_max_file_bytes >= 0),
    live_files INTEGER NOT NULL DEFAULT 0 CHECK(live_files >= 0),
    live_bytes INTEGER NOT NULL DEFAULT 0 CHECK(live_bytes >= 0),
    created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000)
);
CREATE TABLE IF NOT EXISTS _vexfs_inodes(
    id INTEGER PRIMARY KEY,
    workspace_id INTEGER NOT NULL,
    kind TEXT NOT NULL CHECK(kind IN ('file','directory','symlink')),
    mode INTEGER NOT NULL,
    owner_principal TEXT NOT NULL DEFAULT 'local',
    uid INTEGER NOT NULL DEFAULT 0,
    gid INTEGER NOT NULL DEFAULT 0,
    size INTEGER NOT NULL DEFAULT 0,
    current_version INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    accessed_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    updated_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    changed_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    deleted_at INTEGER
);
CREATE INDEX IF NOT EXISTS _vexfs_inodes_workspace_idx
    ON _vexfs_inodes(workspace_id, id);
CREATE TRIGGER IF NOT EXISTS _vexfs_quota_inode_insert
AFTER INSERT ON _vexfs_inodes
WHEN NEW.kind<>'directory' AND NEW.deleted_at IS NULL BEGIN
    UPDATE _vexfs_workspaces
    SET live_files=live_files+1,live_bytes=live_bytes+NEW.size
    WHERE id=NEW.workspace_id;
END;
CREATE TRIGGER IF NOT EXISTS _vexfs_quota_inode_update
AFTER UPDATE ON _vexfs_inodes BEGIN
    UPDATE _vexfs_workspaces
    SET live_files=live_files-CASE WHEN OLD.kind<>'directory' AND OLD.deleted_at IS NULL
                                  THEN 1 ELSE 0 END,
        live_bytes=live_bytes-CASE WHEN OLD.kind<>'directory' AND OLD.deleted_at IS NULL
                                  THEN OLD.size ELSE 0 END
    WHERE id=OLD.workspace_id;
    UPDATE _vexfs_workspaces
    SET live_files=live_files+CASE WHEN NEW.kind<>'directory' AND NEW.deleted_at IS NULL
                                  THEN 1 ELSE 0 END,
        live_bytes=live_bytes+CASE WHEN NEW.kind<>'directory' AND NEW.deleted_at IS NULL
                                  THEN NEW.size ELSE 0 END
    WHERE id=NEW.workspace_id;
END;
CREATE TRIGGER IF NOT EXISTS _vexfs_quota_inode_delete
AFTER DELETE ON _vexfs_inodes
WHEN OLD.kind<>'directory' AND OLD.deleted_at IS NULL BEGIN
    UPDATE _vexfs_workspaces
    SET live_files=live_files-1,live_bytes=live_bytes-OLD.size
    WHERE id=OLD.workspace_id;
END;
CREATE TABLE IF NOT EXISTS _vexfs_dentries(
    workspace_id INTEGER NOT NULL,
    parent_inode INTEGER NOT NULL,
    name TEXT NOT NULL COLLATE BINARY,
    inode_id INTEGER NOT NULL,
    PRIMARY KEY(workspace_id, parent_inode, name)
);
CREATE INDEX IF NOT EXISTS _vexfs_dentries_inode_idx
    ON _vexfs_dentries(workspace_id, inode_id);
CREATE TABLE IF NOT EXISTS _vexfs_commits(
    id INTEGER PRIMARY KEY,
    workspace_id INTEGER NOT NULL,
    parent_commit INTEGER,
    message TEXT NOT NULL,
    created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000)
);
CREATE TABLE IF NOT EXISTS _vexfs_manifests(
    id INTEGER PRIMARY KEY,
    workspace_id INTEGER NOT NULL,
    file_size INTEGER NOT NULL CHECK(file_size >= 0),
    chunk_size INTEGER NOT NULL CHECK(chunk_size = 65536),
    chunk_count INTEGER NOT NULL CHECK(chunk_count >= 0),
    checksum TEXT NOT NULL CHECK(length(checksum)=64),
    created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000)
);
CREATE TABLE IF NOT EXISTS _vexfs_chunks(
    id INTEGER PRIMARY KEY,
    workspace_id INTEGER NOT NULL,
    inode_id INTEGER NOT NULL,
    content BLOB NOT NULL,
    size INTEGER NOT NULL CHECK(size >= 0 AND size <= 65536),
    checksum TEXT NOT NULL CHECK(length(checksum)=64),
    created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000)
);
CREATE TABLE IF NOT EXISTS _vexfs_manifest_chunks(
    manifest_id INTEGER NOT NULL,
    chunk_no INTEGER NOT NULL CHECK(chunk_no >= 0),
    chunk_id INTEGER NOT NULL,
    PRIMARY KEY(manifest_id, chunk_no)
);
CREATE INDEX IF NOT EXISTS _vexfs_chunks_manifest_idx
    ON _vexfs_manifest_chunks(manifest_id, chunk_no);
CREATE INDEX IF NOT EXISTS _vexfs_manifest_chunks_chunk_idx
    ON _vexfs_manifest_chunks(chunk_id);
CREATE TABLE IF NOT EXISTS _vexfs_file_versions(
    id INTEGER PRIMARY KEY,
    inode_id INTEGER NOT NULL,
    version_no INTEGER NOT NULL,
    commit_id INTEGER NOT NULL,
    manifest_id INTEGER,
    size INTEGER NOT NULL,
    checksum TEXT NOT NULL CHECK(length(checksum)=64),
    source_version_no INTEGER,
    created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    CHECK((manifest_id IS NOT NULL AND source_version_no IS NULL) OR
          (manifest_id IS NULL AND source_version_no IS NOT NULL)),
    UNIQUE(inode_id, version_no)
);
CREATE TABLE IF NOT EXISTS _vexfs_xattrs(
    inode_id INTEGER NOT NULL,
    name TEXT NOT NULL COLLATE BINARY,
    value BLOB NOT NULL,
    updated_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    PRIMARY KEY(inode_id, name)
);
CREATE TABLE IF NOT EXISTS _vexfs_acl_entries(
    workspace_id INTEGER NOT NULL,
    inode_id INTEGER NOT NULL,
    principal_id TEXT NOT NULL COLLATE BINARY,
    effect TEXT NOT NULL CHECK(effect IN ('allow','deny')),
    permissions TEXT NOT NULL,
    inherit_flags INTEGER NOT NULL DEFAULT 0 CHECK(inherit_flags >= 0),
    created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    updated_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    PRIMARY KEY(workspace_id,inode_id,principal_id,effect)
);
CREATE INDEX IF NOT EXISTS _vexfs_acl_entries_inode_idx
    ON _vexfs_acl_entries(workspace_id,inode_id,principal_id,effect);
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
    base_manifest_id INTEGER,
    base_size INTEGER NOT NULL CHECK(base_size >= 0),
    base_visible_size INTEGER NOT NULL CHECK(base_visible_size >= 0),
    logical_size INTEGER NOT NULL CHECK(logical_size >= 0),
    capacity INTEGER NOT NULL CHECK(capacity >= 0),
    created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    updated_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER) * 1000),
    CHECK(base_visible_size <= base_size)
);
CREATE TABLE IF NOT EXISTS _vexfs_staging_chunks(
    handle_id TEXT NOT NULL,
    chunk_no INTEGER NOT NULL CHECK(chunk_no >= 0),
    content BLOB NOT NULL CHECK(length(content) > 0 AND length(content) <= 65536),
    PRIMARY KEY(handle_id, chunk_no)
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
INSERT OR IGNORE INTO _vexfs_meta(key, value) VALUES('contract_version', '0.9.0');
INSERT OR IGNORE INTO _vexfs_meta(key, value) VALUES('content_model', 'chunked-v1');
INSERT OR IGNORE INTO _vexfs_meta(key, value) VALUES('staging_layout', 'overlay-v1');
)SQL");
    CreateHistorySchema(db);
    EnsureAclHistoryTriggers(db);
    savepoint.Release();
    DetectGrepIndex(db);
}

void MarkSchemaReady(sqlite3 *db) {
    // A mounted workspace owns one long-lived SQLite connection. Schema validation and
    // migration are connection setup work; repeating the sqlite_master/pragma checks for
    // every stat, write and xattr callback becomes dominant on large trees.
    std::lock_guard<std::mutex> lock(g_schema_ready_mutex);
    g_schema_ready_connections.insert(db);
}

bool IsSchemaReady(sqlite3 *db) {
    std::lock_guard<std::mutex> lock(g_schema_ready_mutex);
    return g_schema_ready_connections.find(db) != g_schema_ready_connections.end();
}

void ForgetSchemaReady(void *pointer) {
    auto *db = static_cast<sqlite3 *>(pointer);
    std::lock_guard<std::mutex> lock(g_schema_ready_mutex);
    g_schema_ready_connections.erase(db);
    g_grep_index_connections.erase(db);
}

void EnsureSchema(sqlite3 *db) {
    if (IsSchemaReady(db)) return;
    EnsureSchemaSlow(db);
    MarkSchemaReady(db);
}

void EnsureSchemaForced(sqlite3 *db) {
    // vexfs_init 是显式校验边界。项目尚未发版，不做旧 schema 原地迁移；版本
    // 不一致直接失败，避免把历史 SQLite 分支带入其他 backend。
    EnsureSchemaSlow(db);
    MarkSchemaReady(db);
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

sqlite3_int64 RequiredOwnerId(sqlite3_value *value, const char *name) {
    if (sqlite3_value_type(value) != SQLITE_INTEGER) {
        throw SqlError(std::string(name) + " must be an integer", SQLITE_MISMATCH);
    }
    const sqlite3_int64 result = sqlite3_value_int64(value);
    if (result < -1 || result > 0xffffffffLL) {
        throw SqlError(std::string(name) + " must be -1 or fit unsigned 32-bit values",
                       SQLITE_RANGE);
    }
    return result;
}

int RequiredMode(sqlite3_value *value) {
    if (sqlite3_value_type(value) != SQLITE_INTEGER) {
        throw SqlError("mode must be an integer", SQLITE_MISMATCH);
    }
    const sqlite3_int64 mode = sqlite3_value_int64(value);
    if (mode < 0 || mode > 0777) {
        throw SqlError("mode must contain only rwx permission bits (0..0777)", SQLITE_RANGE);
    }
    return static_cast<int>(mode);
}

sqlite3_int64 CurrentUid() {
#if defined(_WIN32)
    return 0;
#else
    return static_cast<sqlite3_int64>(getuid());
#endif
}

sqlite3_int64 CurrentGid() {
#if defined(_WIN32)
    return 0;
#else
    return static_cast<sqlite3_int64>(getgid());
#endif
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
        "FROM _vexfs_workspaces WHERE name = ?1 AND state='active'");
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
    std::string owner_principal = "local";
    sqlite3_int64 uid = 0;
    sqlite3_int64 gid = 0;
    sqlite3_int64 created_at = 0;
    sqlite3_int64 accessed_at = 0;
    sqlite3_int64 updated_at = 0;
    sqlite3_int64 changed_at = 0;
};

struct QuotaPolicy {
    sqlite3_int64 max_bytes = -1;
    sqlite3_int64 max_files = -1;
    sqlite3_int64 max_file_bytes = -1;
};

struct QuotaStats {
    sqlite3_int64 live_files = 0;
    sqlite3_int64 live_bytes = 0;
    sqlite3_int64 largest_file_bytes = 0;
};

QuotaPolicy ReadQuotaPolicy(sqlite3 *db, sqlite3_int64 workspace_id) {
    Statement statement(db,
        "SELECT quota_max_bytes,quota_max_files,quota_max_file_bytes "
        "FROM _vexfs_workspaces WHERE id=?1");
    statement.BindInt64(1, workspace_id);
    if (!statement.Row()) throw SqlError("workspace not found", SQLITE_NOTFOUND);
    return {
        statement.Type(0) == SQLITE_NULL ? -1 : statement.Int64(0),
        statement.Type(1) == SQLITE_NULL ? -1 : statement.Int64(1),
        statement.Type(2) == SQLITE_NULL ? -1 : statement.Int64(2),
    };
}

void CheckQuotaForNewInode(sqlite3 *db, sqlite3_int64 workspace_id,
                           const char *kind) {
    if (std::strcmp(kind, "directory") == 0) return;
    Statement usage(db,
        "SELECT quota_max_files,live_files FROM _vexfs_workspaces WHERE id=?1");
    usage.BindInt64(1, workspace_id);
    if (!usage.Row()) throw SqlError("workspace not found", SQLITE_NOTFOUND);
    if (usage.Type(0) != SQLITE_NULL && usage.Int64(1) >= usage.Int64(0)) {
        throw SqlError("workspace file quota exceeded", SQLITE_FULL);
    }
}

void CheckQuotaForContentSize(sqlite3 *db, sqlite3_int64 workspace_id,
                              sqlite3_int64 inode_id, sqlite3_int64 new_size) {
    if (new_size < 0) throw SqlError("file size must be non-negative", SQLITE_RANGE);
    const QuotaPolicy policy = ReadQuotaPolicy(db, workspace_id);
    Statement usage(db, R"SQL(
SELECT live_bytes,
       COALESCE((SELECT size FROM _vexfs_inodes
                 WHERE id=?2 AND workspace_id=?1 AND deleted_at IS NULL
                   AND kind<>'directory'),0),
       EXISTS(SELECT 1 FROM _vexfs_inodes
              WHERE id=?2 AND workspace_id=?1 AND deleted_at IS NULL
                AND kind<>'directory')
FROM _vexfs_workspaces
WHERE id=?1
)SQL");
    usage.BindInt64(1, workspace_id);
    usage.BindInt64(2, inode_id);
    if (!usage.Row()) throw SqlError("workspace not found", SQLITE_NOTFOUND);
    const bool is_live = usage.Int(2) != 0;
    const sqlite3_int64 old_size = is_live ? usage.Int64(1) : 0;

    // Lowering a quota below current usage must not strand the workspace. Writes that
    // shrink a file remain legal until usage is back under the configured limit.
    if (policy.max_file_bytes >= 0 && new_size > policy.max_file_bytes &&
        (!is_live || new_size >= old_size)) {
        throw SqlError("maximum file size quota exceeded", SQLITE_FULL);
    }
    if (policy.max_bytes >= 0 && is_live && new_size >= old_size) {
        const sqlite3_int64 unchanged_bytes = usage.Int64(0) - old_size;
        if (new_size > policy.max_bytes ||
            unchanged_bytes > policy.max_bytes - new_size) {
            throw SqlError("workspace byte quota exceeded", SQLITE_FULL);
        }
    }
}

struct VersionStorage {
    sqlite3_int64 manifest_id = 0;
    sqlite3_int64 size = 0;
    sqlite3_int64 canonical_version = 0;
    sqlite3_int64 chunk_count = 0;
    std::string checksum;
};

VersionStorage ResolveVersionStorage(sqlite3 *db, sqlite3_int64 inode_id,
                                     sqlite3_int64 version) {
    Statement statement(db, R"SQL(
SELECT CASE WHEN v.source_version_no IS NULL THEN v.manifest_id ELSE source.manifest_id END,
       v.size,
       COALESCE(v.source_version_no,v.version_no),
       manifest.file_size,
       v.checksum,
       CASE WHEN v.source_version_no IS NULL THEN v.checksum ELSE source.checksum END,
       manifest.checksum,manifest.chunk_size,manifest.chunk_count
FROM _vexfs_file_versions v
LEFT JOIN _vexfs_file_versions source
  ON source.inode_id=v.inode_id AND source.version_no=v.source_version_no
LEFT JOIN _vexfs_manifests manifest
  ON manifest.id=CASE WHEN v.source_version_no IS NULL THEN v.manifest_id ELSE source.manifest_id END
WHERE v.inode_id=?1 AND v.version_no=?2
)SQL");
    statement.BindInt64(1, inode_id);
    statement.BindInt64(2, version);
    if (!statement.Row()) {
        throw SqlError("file version not found: " + std::to_string(version), SQLITE_NOTFOUND);
    }
    if (statement.Type(0) == SQLITE_NULL || statement.Int64(0) <= 0 ||
        statement.Int64(1) < 0 || statement.Int64(1) != statement.Int64(3) ||
        statement.Text(4).size() != 64 || statement.Text(4) != statement.Text(5) ||
        statement.Text(4) != statement.Text(6) ||
        statement.Int64(7) != kContentChunkBytes || statement.Int64(8) < 0) {
        throw SqlError("file version content reference is corrupt", SQLITE_CORRUPT);
    }
    return {statement.Int64(0), statement.Int64(1), statement.Int64(2),
            statement.Int64(8), statement.Text(4)};
}

std::vector<unsigned char> ReadVersionContent(sqlite3 *db, sqlite3_int64 inode_id,
                                              sqlite3_int64 version) {
    const VersionStorage storage = ResolveVersionStorage(db, inode_id, version);
    if (storage.size > kMaxStagedBytes) {
        throw SqlError("file version is larger than 128 MiB", SQLITE_CORRUPT);
    }
    std::vector<unsigned char> content(static_cast<size_t>(storage.size));
    Statement chunks(db, R"SQL(
SELECT chunk_no,content,size,checksum
FROM _vexfs_manifest_chunks entry
JOIN _vexfs_chunks chunk ON chunk.id=entry.chunk_id
WHERE entry.manifest_id=?1 ORDER BY entry.chunk_no
)SQL");
    chunks.BindInt64(1, storage.manifest_id);
    vexfs::Sha256 file_hash;
    sqlite3_int64 offset = 0;
    sqlite3_int64 expected_chunk = 0;
    while (chunks.Row()) {
        const sqlite3_int64 expected_size = std::min<sqlite3_int64>(
            kContentChunkBytes, storage.size - offset);
        const int chunk_bytes = chunks.BlobBytes(1);
        const unsigned char *chunk = chunks.BlobData(1);
        if (chunks.Int64(0) != expected_chunk || expected_size <= 0 ||
            chunks.Int64(2) != expected_size ||
            chunk_bytes != expected_size || chunk == nullptr ||
            chunks.Text(3).size() != 64 ||
            vexfs::Sha256Hex(chunk, static_cast<size_t>(chunk_bytes)) != chunks.Text(3)) {
            throw SqlError("file manifest chunk is corrupt", SQLITE_CORRUPT);
        }
        std::memcpy(content.data() + offset, chunk, static_cast<size_t>(chunk_bytes));
        file_hash.Update(chunk, static_cast<size_t>(chunk_bytes));
        offset += expected_size;
        ++expected_chunk;
    }
    if (offset != storage.size || expected_chunk != storage.chunk_count ||
        expected_chunk != (storage.size + kContentChunkBytes - 1) / kContentChunkBytes ||
        vexfs::Hex(file_hash.Finish()) != storage.checksum) {
        throw SqlError("file version checksum does not match content", SQLITE_CORRUPT);
    }
    return content;
}

std::vector<unsigned char> ReadVersionRange(sqlite3 *db, sqlite3_int64 inode_id,
                                            sqlite3_int64 version,
                                            sqlite3_int64 offset,
                                            sqlite3_int64 length) {
    const VersionStorage storage = ResolveVersionStorage(db, inode_id, version);
    if (storage.size > kMaxStagedBytes) {
        throw SqlError("file version is larger than 128 MiB", SQLITE_CORRUPT);
    }
    if (offset < 0 || length < 0 || offset > kMaxStagedBytes ||
        length > kMaxStagedBytes) {
        throw SqlError("read range must be within 0..128 MiB", SQLITE_RANGE);
    }
    if (length == 0 || offset >= storage.size) return {};

    const sqlite3_int64 end = std::min<sqlite3_int64>(storage.size, offset + length);
    const sqlite3_int64 first_chunk = offset / kContentChunkBytes;
    const sqlite3_int64 last_chunk = (end - 1) / kContentChunkBytes;
    std::vector<unsigned char> content;
    content.reserve(static_cast<size_t>(end - offset));

    Statement chunks(db, R"SQL(
SELECT entry.chunk_no,chunk.content,chunk.size,chunk.checksum
FROM _vexfs_manifest_chunks entry
JOIN _vexfs_chunks chunk ON chunk.id=entry.chunk_id
WHERE entry.manifest_id=?1 AND entry.chunk_no BETWEEN ?2 AND ?3
ORDER BY entry.chunk_no
)SQL");
    chunks.BindInt64(1, storage.manifest_id);
    chunks.BindInt64(2, first_chunk);
    chunks.BindInt64(3, last_chunk);
    sqlite3_int64 expected_chunk = first_chunk;
    while (chunks.Row()) {
        const sqlite3_int64 chunk_no = chunks.Int64(0);
        const sqlite3_int64 chunk_offset = chunk_no * kContentChunkBytes;
        const sqlite3_int64 expected_size = std::min<sqlite3_int64>(
            kContentChunkBytes, storage.size - chunk_offset);
        const int chunk_bytes = chunks.BlobBytes(1);
        const unsigned char *chunk = chunks.BlobData(1);
        if (chunk_no != expected_chunk || expected_size <= 0 ||
            chunks.Int64(2) != expected_size || chunk_bytes != expected_size ||
            chunk == nullptr || chunks.Text(3).size() != 64 ||
            vexfs::Sha256Hex(chunk, static_cast<size_t>(chunk_bytes)) != chunks.Text(3)) {
            throw SqlError("file manifest range chunk is corrupt", SQLITE_CORRUPT);
        }
        const sqlite3_int64 copy_start = std::max<sqlite3_int64>(offset, chunk_offset) -
            chunk_offset;
        const sqlite3_int64 copy_end = std::min<sqlite3_int64>(
            end, chunk_offset + expected_size) - chunk_offset;
        content.insert(content.end(), chunk + copy_start, chunk + copy_end);
        ++expected_chunk;
    }
    if (expected_chunk != last_chunk + 1 ||
        content.size() != static_cast<size_t>(end - offset)) {
        throw SqlError("file manifest range is incomplete", SQLITE_CORRUPT);
    }
    return content;
}

bool FindChild(sqlite3 *db, sqlite3_int64 workspace_id, sqlite3_int64 parent,
               const std::string &name, Node *node) {
    Statement statement(db,
        "SELECT i.id, i.kind, i.size, i.current_version, i.mode, i.owner_principal, i.uid, i.gid, "
        "i.created_at, i.accessed_at, i.updated_at, i.changed_at "
        "FROM _vexfs_dentries d JOIN _vexfs_inodes i ON i.id = d.inode_id "
        "WHERE d.workspace_id = ?1 AND d.parent_inode = ?2 AND d.name = ?3 "
        "AND i.deleted_at IS NULL");
    statement.BindInt64(1, workspace_id);
    statement.BindInt64(2, parent);
    statement.BindText(3, name);
    if (!statement.Row()) return false;
    *node = {statement.Int64(0), statement.Text(1), statement.Int64(2),
             statement.Int64(3), statement.Int(4), statement.Text(5), statement.Int64(6),
             statement.Int64(7), statement.Int64(8), statement.Int64(9),
             statement.Int64(10), statement.Int64(11)};
    return true;
}

Node RootNode(sqlite3 *db, const Workspace &workspace) {
    Statement statement(db,
        "SELECT id, kind, size, current_version, mode, owner_principal, uid, gid, "
        "created_at, accessed_at, updated_at, changed_at FROM _vexfs_inodes "
        "WHERE id = ?1 AND workspace_id = ?2 AND deleted_at IS NULL");
    statement.BindInt64(1, workspace.root_inode);
    statement.BindInt64(2, workspace.id);
    if (!statement.Row()) throw SqlError("workspace root is missing", SQLITE_CORRUPT);
    return {statement.Int64(0), statement.Text(1), statement.Int64(2),
             statement.Int64(3), statement.Int(4), statement.Text(5), statement.Int64(6),
             statement.Int64(7), statement.Int64(8), statement.Int64(9),
             statement.Int64(10), statement.Int64(11)};
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

sqlite3_int64 LinkCount(sqlite3 *db, const Workspace &workspace, sqlite3_int64 inode) {
    Statement statement(db, R"SQL(
SELECT i.kind,
       CASE WHEN i.kind='directory' THEN
         2 + (SELECT count(*) FROM _vexfs_dentries d
              JOIN _vexfs_inodes child ON child.id=d.inode_id
              WHERE d.workspace_id=i.workspace_id AND d.parent_inode=i.id
                AND child.kind='directory' AND child.deleted_at IS NULL)
       ELSE
         (SELECT count(*) FROM _vexfs_dentries d
          WHERE d.workspace_id=i.workspace_id AND d.inode_id=i.id)
       END
FROM _vexfs_inodes i WHERE i.workspace_id=?1 AND i.id=?2
)SQL");
    statement.BindInt64(1, workspace.id);
    statement.BindInt64(2, inode);
    if (!statement.Row()) throw SqlError("inode link count unavailable", SQLITE_CORRUPT);
    return statement.Int64(1);
}

void FlushHistoryChanges(sqlite3 *db, const Workspace &workspace, sqlite3_int64 commit) {
    Statement inodes(db, R"SQL(
INSERT OR REPLACE INTO _vexfs_inode_states(
 workspace_id,inode_id,commit_id,kind,mode,owner_principal,uid,gid,size,current_version,
 created_at,accessed_at,updated_at,changed_at,deleted_at)
SELECT i.workspace_id,i.id,?2,i.kind,i.mode,i.owner_principal,i.uid,i.gid,i.size,i.current_version,
       i.created_at,i.accessed_at,i.updated_at,i.changed_at,i.deleted_at
FROM _vexfs_dirty_inodes dirty
JOIN _vexfs_inodes i ON i.workspace_id=dirty.workspace_id AND i.id=dirty.inode_id
WHERE dirty.workspace_id=?1
  -- handle_create deliberately keeps a new file at version 0 until its first
  -- publish, so several create/write/metadata calls can share one commit.  A
  -- commit for another inode must not make that private staging state part of
  -- immutable history: version 0 has no manifest and cannot be restored.
  AND NOT (i.kind='file' AND i.current_version=0)
)SQL");
    inodes.BindInt64(1, workspace.id);
    inodes.BindInt64(2, commit);
    inodes.Done();

    Statement dentries(db, R"SQL(
INSERT OR REPLACE INTO _vexfs_dentry_states(
 workspace_id,parent_inode,name,commit_id,inode_id,deleted)
SELECT dirty.workspace_id,dirty.parent_inode,dirty.name,?2,
       COALESCE(d.inode_id,dirty.inode_id),CASE WHEN d.inode_id IS NULL THEN 1 ELSE 0 END
FROM _vexfs_dirty_dentries dirty
LEFT JOIN _vexfs_dentries d
  ON d.workspace_id=dirty.workspace_id AND d.parent_inode=dirty.parent_inode
 AND d.name=dirty.name
WHERE dirty.workspace_id=?1
  AND NOT EXISTS (
    SELECT 1 FROM _vexfs_inodes pending
    WHERE pending.workspace_id=dirty.workspace_id
      AND pending.id=COALESCE(d.inode_id,dirty.inode_id)
      AND pending.kind='file' AND pending.current_version=0
  )
)SQL");
    dentries.BindInt64(1, workspace.id);
    dentries.BindInt64(2, commit);
    dentries.Done();

    Statement xattrs(db, R"SQL(
INSERT OR REPLACE INTO _vexfs_xattr_states(
 workspace_id,inode_id,name,commit_id,value,deleted)
SELECT dirty.workspace_id,dirty.inode_id,dirty.name,?2,
       COALESCE(x.value,X''),CASE WHEN x.inode_id IS NULL THEN 1 ELSE 0 END
FROM _vexfs_dirty_xattrs dirty
LEFT JOIN _vexfs_xattrs x ON x.inode_id=dirty.inode_id AND x.name=dirty.name
WHERE dirty.workspace_id=?1
  AND NOT EXISTS (
    SELECT 1 FROM _vexfs_inodes pending
    WHERE pending.workspace_id=dirty.workspace_id AND pending.id=dirty.inode_id
      AND pending.kind='file' AND pending.current_version=0
  )
)SQL");
    xattrs.BindInt64(1, workspace.id);
    xattrs.BindInt64(2, commit);
    xattrs.Done();

    Statement acl(db, R"SQL(
INSERT OR REPLACE INTO _vexfs_acl_states(
 workspace_id,inode_id,principal_id,effect,commit_id,permissions,inherit_flags,deleted)
SELECT dirty.workspace_id,dirty.inode_id,dirty.principal_id,dirty.effect,?2,
       COALESCE(a.permissions,''),COALESCE(a.inherit_flags,0),
       CASE WHEN a.inode_id IS NULL THEN 1 ELSE 0 END
FROM _vexfs_dirty_acl dirty
LEFT JOIN _vexfs_acl_entries a
  ON a.workspace_id=dirty.workspace_id AND a.inode_id=dirty.inode_id
 AND a.principal_id=dirty.principal_id AND a.effect=dirty.effect
WHERE dirty.workspace_id=?1
  AND NOT EXISTS (
    SELECT 1 FROM _vexfs_inodes pending
    WHERE pending.workspace_id=dirty.workspace_id AND pending.id=dirty.inode_id
      AND pending.kind='file' AND pending.current_version=0
  )
)SQL");
    acl.BindInt64(1, workspace.id);
    acl.BindInt64(2, commit);
    acl.Done();

    // Keep unpublished files dirty across unrelated commits.  Once publish
    // advances current_version, the next CreateCommit flushes and clears all
    // of their inode/dentry/xattr/ACL changes atomically.  A version-0 inode
    // deleted before publish never entered history, so its dirty rows can be
    // discarded.
    Statement clear_inodes(db, R"SQL(
DELETE FROM _vexfs_dirty_inodes
WHERE workspace_id=?1 AND (
  NOT EXISTS (
    SELECT 1 FROM _vexfs_inodes pending
    WHERE pending.workspace_id=_vexfs_dirty_inodes.workspace_id
      AND pending.id=_vexfs_dirty_inodes.inode_id
  ) OR EXISTS (
    SELECT 1 FROM _vexfs_inodes publishable
    WHERE publishable.workspace_id=_vexfs_dirty_inodes.workspace_id
      AND publishable.id=_vexfs_dirty_inodes.inode_id
      AND (publishable.kind<>'file' OR publishable.current_version>0
           OR publishable.deleted_at IS NOT NULL)
  )
)
)SQL");
    clear_inodes.BindInt64(1, workspace.id);
    clear_inodes.Done();
    Statement clear_dentries(db, R"SQL(
DELETE FROM _vexfs_dirty_dentries
WHERE workspace_id=?1 AND NOT EXISTS (
  SELECT 1 FROM _vexfs_inodes pending
  WHERE pending.workspace_id=_vexfs_dirty_dentries.workspace_id
    AND pending.id=COALESCE((
      SELECT live.inode_id FROM _vexfs_dentries live
      WHERE live.workspace_id=_vexfs_dirty_dentries.workspace_id
        AND live.parent_inode=_vexfs_dirty_dentries.parent_inode
        AND live.name=_vexfs_dirty_dentries.name
    ),_vexfs_dirty_dentries.inode_id)
    AND pending.kind='file' AND pending.current_version=0
    AND pending.deleted_at IS NULL
)
)SQL");
    clear_dentries.BindInt64(1, workspace.id);
    clear_dentries.Done();
    Statement clear_xattrs(db, R"SQL(
DELETE FROM _vexfs_dirty_xattrs
WHERE workspace_id=?1 AND NOT EXISTS (
  SELECT 1 FROM _vexfs_inodes pending
  WHERE pending.workspace_id=_vexfs_dirty_xattrs.workspace_id
    AND pending.id=_vexfs_dirty_xattrs.inode_id
    AND pending.kind='file' AND pending.current_version=0
    AND pending.deleted_at IS NULL
)
)SQL");
    clear_xattrs.BindInt64(1, workspace.id);
    clear_xattrs.Done();
    Statement clear_acl(db, R"SQL(
DELETE FROM _vexfs_dirty_acl
WHERE workspace_id=?1 AND NOT EXISTS (
  SELECT 1 FROM _vexfs_inodes pending
  WHERE pending.workspace_id=_vexfs_dirty_acl.workspace_id
    AND pending.id=_vexfs_dirty_acl.inode_id
    AND pending.kind='file' AND pending.current_version=0
    AND pending.deleted_at IS NULL
)
)SQL");
    clear_acl.BindInt64(1, workspace.id);
    clear_acl.Done();
}

sqlite3_int64 CreateCommitInTransaction(sqlite3 *db, const Workspace &workspace,
                                       const std::string &message) {
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

    FlushHistoryChanges(db, workspace, commit);

    Statement update(db,
        "UPDATE _vexfs_workspaces SET head_commit=?1, "
        "history_floor_commit=COALESCE(history_floor_commit,?1) WHERE id=?2");
    update.BindInt64(1, commit);
    update.BindInt64(2, workspace.id);
    update.Done();
    return commit;
}

sqlite3_int64 CreateCommit(sqlite3 *db, const Workspace &workspace,
                           const std::string &message) {
    Savepoint savepoint(db, "vexfs_create_commit");
    const sqlite3_int64 commit = CreateCommitInTransaction(db, workspace, message);
    savepoint.Release();
    return commit;
}

sqlite3_int64 CreateInode(sqlite3 *db, sqlite3_int64 workspace_id, const char *kind, int mode) {
    CheckQuotaForNewInode(db, workspace_id, kind);
    Statement insert(db,
        "INSERT INTO _vexfs_inodes(workspace_id, kind, mode, owner_principal, uid, gid, current_version) "
        "VALUES(?1, ?2, ?3, 'local', ?4, ?5, ?6)");
    insert.BindInt64(1, workspace_id);
    insert.BindText(2, kind);
    insert.BindInt(3, mode);
    insert.BindInt64(4, CurrentUid());
    insert.BindInt64(5, CurrentGid());
    insert.BindInt64(6, std::strcmp(kind, "directory") == 0 ? 1 : 0);
    insert.Done();
    return sqlite3_last_insert_rowid(db);
}

void TouchDirectory(sqlite3 *db, sqlite3_int64 workspace_id, sqlite3_int64 inode) {
    Statement update(db,
        "UPDATE _vexfs_inodes SET current_version=current_version+1, "
        "updated_at=CAST(unixepoch('subsec')*1000 AS INTEGER), "
        "changed_at=CAST(unixepoch('subsec')*1000 AS INTEGER) "
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
    // ACLs marked inheritable are copied at creation time.  Existing inode ACLs
    // win on hard-link creation, so this is an INSERT OR IGNORE rather than a
    // replacement of the inode's access policy.
    Statement inherit(db, R"SQL(
INSERT OR IGNORE INTO _vexfs_acl_entries(
 workspace_id,inode_id,principal_id,effect,permissions,inherit_flags)
SELECT workspace_id,?3,principal_id,effect,permissions,inherit_flags
FROM _vexfs_acl_entries
WHERE workspace_id=?1 AND inode_id=?2 AND inherit_flags<>0
)SQL");
    inherit.BindInt64(1, workspace_id);
    inherit.BindInt64(2, parent);
    inherit.BindInt64(3, inode);
    inherit.Done();
    TouchDirectory(db, workspace_id, parent);
}

void MarkGrepIndexDirty(sqlite3 *db) {
    if (GetGrepIndexState(db) == GrepIndexState::kDisabled) return;
    Exec(db,
        "INSERT OR REPLACE INTO _vexfs_meta(key,value) VALUES('grep_index_dirty','1')");
}

void UpdateGrepIndexFromVersion(sqlite3 *db, sqlite3_int64 workspace_id,
                                sqlite3_int64 inode_id, sqlite3_int64 version) {
    const GrepIndexState state = GetGrepIndexState(db);
    if (state == GrepIndexState::kDisabled) return;
    if (state == GrepIndexState::kUnavailable) {
        MarkGrepIndexDirty(db);
        return;
    }
    Statement remove(db, "DELETE FROM _vexfs_content_fts WHERE rowid=?1");
    remove.BindInt64(1, inode_id);
    remove.Done();
    Statement live(db, "SELECT 1 FROM _vexfs_inodes WHERE id=?1 AND workspace_id=?2 "
                       "AND kind='file' AND deleted_at IS NULL");
    live.BindInt64(1, inode_id);
    live.BindInt64(2, workspace_id);
    if (!live.Row()) return;
    const std::vector<unsigned char> content = ReadVersionContent(db, inode_id, version);
    Statement insert(db, R"SQL(
INSERT INTO _vexfs_content_fts(rowid,workspace_id,inode_id,version_no,content)
VALUES(?1,?2,?1,?3,?4)
)SQL");
    insert.BindInt64(1, inode_id);
    insert.BindInt64(2, workspace_id);
    insert.BindInt64(3, version);
    insert.BindText(4, content.empty() ? std::string() :
        std::string(reinterpret_cast<const char *>(content.data()), content.size()));
    insert.Done();
}

void RefreshGrepIndexAfterTreeChange(sqlite3 *db) {
    const GrepIndexState state = GetGrepIndexState(db);
    if (state == GrepIndexState::kAvailable) RebuildGrepIndex(db);
    else if (state == GrepIndexState::kUnavailable) MarkGrepIndexDirty(db);
}

void RemoveDeletedFilesFromGrepIndex(sqlite3 *db, sqlite3_int64 workspace_id) {
    const GrepIndexState state = GetGrepIndexState(db);
    if (state == GrepIndexState::kDisabled) return;
    if (state == GrepIndexState::kUnavailable) {
        MarkGrepIndexDirty(db);
        return;
    }
    Statement remove(db, R"SQL(
DELETE FROM _vexfs_content_fts
WHERE rowid IN (
    SELECT id FROM _vexfs_inodes
    WHERE workspace_id=?1 AND kind='file' AND deleted_at IS NOT NULL
)
)SQL");
    remove.BindInt64(1, workspace_id);
    remove.Done();
}

sqlite3_int64 StoreManifest(sqlite3 *db, sqlite3_int64 workspace_id,
                            sqlite3_int64 inode_id,
                            sqlite3_int64 previous_manifest,
                            const std::vector<unsigned char> &content,
                            std::string *checksum_out) {
    const sqlite3_int64 size = static_cast<sqlite3_int64>(content.size());
    const sqlite3_int64 chunk_count =
        (size + kContentChunkBytes - 1) / kContentChunkBytes;
    const std::string pending_checksum(64, '0');
    Statement manifest(db, R"SQL(
INSERT INTO _vexfs_manifests(workspace_id,file_size,chunk_size,chunk_count,checksum)
VALUES(?1,?2,?3,?4,?5)
)SQL");
    manifest.BindInt64(1, workspace_id);
    manifest.BindInt64(2, size);
    manifest.BindInt64(3, kContentChunkBytes);
    manifest.BindInt64(4, chunk_count);
    manifest.BindText(5, pending_checksum);
    manifest.Done();
    const sqlite3_int64 manifest_id = sqlite3_last_insert_rowid(db);

    Statement previous(db, R"SQL(
SELECT chunk.id,chunk.content,chunk.size,chunk.checksum
FROM _vexfs_manifest_chunks entry
JOIN _vexfs_chunks chunk ON chunk.id=entry.chunk_id
WHERE entry.manifest_id=?1 AND entry.chunk_no=?2 AND chunk.inode_id=?3
)SQL");
    Statement chunk(db, R"SQL(
INSERT INTO _vexfs_chunks(workspace_id,inode_id,content,size,checksum)
VALUES(?1,?2,?3,?4,?5)
)SQL");
    Statement entry(db, R"SQL(
INSERT INTO _vexfs_manifest_chunks(manifest_id,chunk_no,chunk_id)
VALUES(?1,?2,?3)
)SQL");
    std::unordered_map<std::string, sqlite3_int64> new_chunks;
    vexfs::Sha256 file_hash;
    for (sqlite3_int64 chunk_no = 0, offset = 0; offset < size;
         ++chunk_no, offset += kContentChunkBytes) {
        const size_t bytes = static_cast<size_t>(std::min<sqlite3_int64>(
            kContentChunkBytes, size - offset));
        const unsigned char *data = content.data() + offset;
        file_hash.Update(data, bytes);
        const std::string chunk_checksum = vexfs::Sha256Hex(data, bytes);
        sqlite3_int64 chunk_id = 0;
        if (previous_manifest > 0) {
            previous.BindInt64(1, previous_manifest);
            previous.BindInt64(2, chunk_no);
            previous.BindInt64(3, inode_id);
            if (previous.Row() && previous.Int64(2) == static_cast<sqlite3_int64>(bytes) &&
                previous.Text(3) == chunk_checksum && previous.BlobEquals(1, data, bytes)) {
                chunk_id = previous.Int64(0);
            }
            previous.Reset();
        }
        const std::string reuse_key = std::to_string(bytes) + ":" + chunk_checksum;
        if (chunk_id == 0) {
            const auto reused = new_chunks.find(reuse_key);
            if (reused != new_chunks.end()) chunk_id = reused->second;
        }
        if (chunk_id == 0) {
            chunk.BindInt64(1, workspace_id);
            chunk.BindInt64(2, inode_id);
            chunk.BindBlobView(3, data, bytes);
            chunk.BindInt64(4, static_cast<sqlite3_int64>(bytes));
            chunk.BindText(5, chunk_checksum);
            chunk.Done();
            chunk_id = sqlite3_last_insert_rowid(db);
            new_chunks.emplace(reuse_key, chunk_id);
            chunk.Reset();
        }
        entry.BindInt64(1, manifest_id);
        entry.BindInt64(2, chunk_no);
        entry.BindInt64(3, chunk_id);
        entry.Done();
        entry.Reset();
    }
    const std::string checksum = vexfs::Hex(file_hash.Finish());
    Statement finalize(db,
        "UPDATE _vexfs_manifests SET checksum=?1 WHERE id=?2");
    finalize.BindText(1, checksum);
    finalize.BindInt64(2, manifest_id);
    finalize.Done();
    *checksum_out = checksum;
    return manifest_id;
}

sqlite3_int64 StoreVersion(sqlite3 *db, const Workspace &workspace, const Node &node,
                           const std::vector<unsigned char> &content,
                           const std::string &message) {
    Savepoint savepoint(db, "vexfs_store_version");
    AcquireWriteLock(db);
    const sqlite3_int64 previous_manifest = node.version > 0
        ? ResolveVersionStorage(db, node.id, node.version).manifest_id : 0;
    CheckQuotaForContentSize(db, workspace.id, node.id,
                             static_cast<sqlite3_int64>(content.size()));
    const sqlite3_int64 version = node.version + 1;
    Statement advance(db,
        "UPDATE _vexfs_inodes SET size=?1, current_version=?2, "
        "updated_at=CAST(unixepoch('subsec')*1000 AS INTEGER), "
        "changed_at=CAST(unixepoch('subsec')*1000 AS INTEGER) "
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
    std::string checksum;
    const sqlite3_int64 manifest_id = StoreManifest(
        db, workspace.id, node.id, previous_manifest, content, &checksum);
    Statement insert(db,
        "INSERT INTO _vexfs_file_versions"
        "(inode_id,version_no,commit_id,manifest_id,size,checksum) "
        "VALUES(?1,?2,?3,?4,?5,?6)");
    insert.BindInt64(1, node.id);
    insert.BindInt64(2, version);
    insert.BindInt64(3, commit);
    insert.BindInt64(4, manifest_id);
    insert.BindInt64(5, static_cast<sqlite3_int64>(content.size()));
    insert.BindText(6, checksum);
    insert.Done();
    UpdateGrepIndexFromVersion(db, workspace.id, node.id, version);
    savepoint.Release();
    return version;
}

sqlite3_int64 StoreVersionFromVersion(sqlite3 *db, const Workspace &workspace, const Node &node,
                                      sqlite3_int64 source_version,
                                      const std::string &message) {
    Savepoint savepoint(db, "vexfs_restore_version");
    AcquireWriteLock(db);
    const VersionStorage source = ResolveVersionStorage(db, node.id, source_version);
    CheckQuotaForContentSize(db, workspace.id, node.id, source.size);
    const sqlite3_int64 version = node.version + 1;
    Statement advance(db,
        "UPDATE _vexfs_inodes SET size=?1, current_version=?2, "
        "updated_at=CAST(unixepoch('subsec')*1000 AS INTEGER), "
        "changed_at=CAST(unixepoch('subsec')*1000 AS INTEGER) "
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
        "(inode_id,version_no,commit_id,manifest_id,size,checksum,source_version_no) "
        "VALUES(?1,?2,?3,NULL,?4,?5,?6)");
    insert.BindInt64(1, node.id);
    insert.BindInt64(2, version);
    insert.BindInt64(3, commit);
    insert.BindInt64(4, source.size);
    insert.BindText(5, source.checksum);
    insert.BindInt64(6, source.canonical_version);
    insert.Done();
    if (sqlite3_changes64(db) != 1) {
        throw SqlError("file version not found: " + std::to_string(source_version), SQLITE_NOTFOUND);
    }
    UpdateGrepIndexFromVersion(db, workspace.id, node.id, version);
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

std::string CanonicalPath(const std::vector<std::string> &parts) {
    if (parts.empty()) return "/";
    std::string path;
    for (const std::string &part : parts) {
        path.push_back('/');
        path += part;
    }
    return path;
}

bool IsUtf8Text(const std::vector<unsigned char> &content) {
    for (size_t index = 0; index < content.size();) {
        const unsigned char first = content[index];
        if (first == 0) return false;
        if (first < 0x80) {
            ++index;
            continue;
        }
        size_t trailing = 0;
        uint32_t codepoint = 0;
        uint32_t minimum = 0;
        if ((first & 0xe0) == 0xc0) {
            trailing = 1; codepoint = first & 0x1f; minimum = 0x80;
        } else if ((first & 0xf0) == 0xe0) {
            trailing = 2; codepoint = first & 0x0f; minimum = 0x800;
        } else if ((first & 0xf8) == 0xf0) {
            trailing = 3; codepoint = first & 0x07; minimum = 0x10000;
        } else {
            return false;
        }
        if (index + trailing >= content.size()) return false;
        for (size_t offset = 1; offset <= trailing; ++offset) {
            const unsigned char next = content[index + offset];
            if ((next & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return false;
        }
        index += trailing + 1;
    }
    return true;
}

unsigned char FoldAscii(unsigned char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
}

bool LineContains(const unsigned char *begin, const unsigned char *end,
                  const std::string &pattern, bool ignore_case) {
    const auto compare = [ignore_case](unsigned char left, unsigned char right) {
        return ignore_case ? FoldAscii(left) == FoldAscii(right) : left == right;
    };
    return std::search(begin, end, pattern.begin(), pattern.end(), compare) != end;
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
    // An empty request ID is the in-process mount fast path.  Generation checks
    // make stage/truncate/publish/synchronize replay-safe without growing the
    // persistent request table.  Public callers still pass IDs and retain the
    // cross-process retry contract.
    if (request_id.empty()) return {};
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

void MaybePruneRequests(sqlite3 *db) {
    const sqlite3_int64 newest_rowid = sqlite3_last_insert_rowid(db);
    if (newest_rowid <= kRequestRetentionRows ||
        newest_rowid % kRequestPruneInterval != 0) {
        return;
    }
    // request_id protects retries, but keeping every completed request forever makes a
    // long-lived mount grow without a bound.  Delete in batches so ordinary writes do
    // not pay for a count/sort query; the newest 65,536 results remain replayable.
    Statement prune(db, "DELETE FROM _vexfs_requests WHERE rowid <= ?1");
    prune.BindInt64(1, newest_rowid - kRequestRetentionRows);
    prune.Done();
}

void StoreRequestInteger(sqlite3 *db, const std::string &request_id, const char *operation,
                         const std::vector<unsigned char> &fingerprint, sqlite3_int64 result) {
    if (request_id.empty()) return;
    Statement statement(db,
        "INSERT INTO _vexfs_requests(request_id, operation, request_fingerprint, result_integer) "
        "VALUES(?1, ?2, ?3, ?4)");
    statement.BindText(1, request_id);
    statement.BindText(2, operation);
    statement.BindBlob(3, fingerprint);
    statement.BindInt64(4, result);
    statement.Done();
    MaybePruneRequests(db);
}

void StoreRequestText(sqlite3 *db, const std::string &request_id, const char *operation,
                      const std::vector<unsigned char> &fingerprint, const std::string &result) {
    if (request_id.empty()) return;
    Statement statement(db,
        "INSERT INTO _vexfs_requests(request_id, operation, request_fingerprint, result_text) "
        "VALUES(?1, ?2, ?3, ?4)");
    statement.BindText(1, request_id);
    statement.BindText(2, operation);
    statement.BindBlob(3, fingerprint);
    statement.BindText(4, result);
    statement.Done();
    MaybePruneRequests(db);
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
    sqlite3_int64 generation = 0;
    sqlite3_int64 base_manifest_id = 0;
    sqlite3_int64 base_size = 0;
    sqlite3_int64 base_visible_size = 0;
    sqlite3_int64 logical_size = 0;
    sqlite3_int64 capacity = 0;
};

StagingInfo FindStaging(sqlite3 *db, const Handle &handle) {
    Statement statement(db,
        "SELECT generation,COALESCE(base_manifest_id,0),base_size,base_visible_size,"
        "logical_size,capacity FROM _vexfs_staging WHERE handle_id=?1");
    statement.BindText(1, handle.id);
    if (!statement.Row()) throw SqlError("staging area not found", SQLITE_NOTFOUND);
    return {statement.Int64(0), statement.Int64(1), statement.Int64(2),
            statement.Int64(3), statement.Int64(4), statement.Int64(5)};
}

sqlite3_int64 DirtyChunkSize(sqlite3 *db, const std::string &handle_id,
                             sqlite3_int64 chunk_no) {
    Statement statement(db,
        "SELECT length(content) FROM _vexfs_staging_chunks "
        "WHERE handle_id=?1 AND chunk_no=?2");
    statement.BindText(1, handle_id);
    statement.BindInt64(2, chunk_no);
    return statement.Row() ? statement.Int64(0) : 0;
}

void StoreDirtyChunk(sqlite3 *db, const std::string &handle_id,
                     sqlite3_int64 chunk_no,
                     const std::vector<unsigned char> &content) {
    if (content.empty() || content.size() > static_cast<size_t>(kContentChunkBytes)) {
        throw SqlError("staging chunk must be within 1..65536 bytes", SQLITE_CORRUPT);
    }
    Statement statement(db, R"SQL(
INSERT INTO _vexfs_staging_chunks(handle_id,chunk_no,content)
VALUES(?1,?2,?3)
ON CONFLICT(handle_id,chunk_no) DO UPDATE SET content=excluded.content
)SQL");
    statement.BindText(1, handle_id);
    statement.BindInt64(2, chunk_no);
    statement.BindBlob(3, content);
    statement.Done();
}

std::vector<unsigned char> ReadEffectiveChunk(sqlite3 *db, const Handle &handle,
                                              const StagingInfo &staging,
                                              sqlite3_int64 chunk_no,
                                              sqlite3_int64 *dirty_bytes_out = nullptr) {
    if (chunk_no < 0) throw SqlError("staging chunk number is out of range", SQLITE_RANGE);
    std::vector<unsigned char> content(static_cast<size_t>(kContentChunkBytes), 0);
    Statement dirty(db,
        "SELECT content FROM _vexfs_staging_chunks WHERE handle_id=?1 AND chunk_no=?2");
    dirty.BindText(1, handle.id);
    dirty.BindInt64(2, chunk_no);
    if (dirty.Row()) {
        const int dirty_bytes = dirty.BlobBytes(0);
        if (dirty_bytes <= 0 || dirty_bytes > kContentChunkBytes ||
            dirty.BlobData(0) == nullptr) {
            throw SqlError("staging chunk is corrupt", SQLITE_CORRUPT);
        }
        std::memcpy(content.data(), dirty.BlobData(0), static_cast<size_t>(dirty_bytes));
        if (dirty_bytes_out != nullptr) *dirty_bytes_out = dirty_bytes;
        return content;
    }
    if (dirty_bytes_out != nullptr) *dirty_bytes_out = 0;

    const sqlite3_int64 chunk_offset = chunk_no * kContentChunkBytes;
    if (chunk_offset >= staging.base_visible_size) return content;
    if (staging.base_manifest_id <= 0 || staging.base_size <= chunk_offset ||
        staging.base_visible_size > staging.base_size) {
        throw SqlError("staging base reference is corrupt", SQLITE_CORRUPT);
    }
    Statement base(db, R"SQL(
SELECT chunk.content,chunk.size,chunk.checksum
FROM _vexfs_manifest_chunks entry
JOIN _vexfs_chunks chunk ON chunk.id=entry.chunk_id
WHERE entry.manifest_id=?1 AND entry.chunk_no=?2
)SQL");
    base.BindInt64(1, staging.base_manifest_id);
    base.BindInt64(2, chunk_no);
    if (!base.Row()) throw SqlError("staging base chunk is missing", SQLITE_CORRUPT);
    const sqlite3_int64 expected_size = std::min<sqlite3_int64>(
        kContentChunkBytes, staging.base_size - chunk_offset);
    if (expected_size <= 0 || base.Int64(1) != expected_size ||
        base.BlobBytes(0) != expected_size || base.BlobData(0) == nullptr ||
        base.Text(2).size() != 64 ||
        vexfs::Sha256Hex(base.BlobData(0), static_cast<size_t>(expected_size)) != base.Text(2)) {
        throw SqlError("staging base chunk is corrupt", SQLITE_CORRUPT);
    }
    const sqlite3_int64 visible = std::min<sqlite3_int64>(
        expected_size, staging.base_visible_size - chunk_offset);
    std::memcpy(content.data(), base.BlobData(0), static_cast<size_t>(visible));
    return content;
}

void CreateStaging(sqlite3 *db, const Handle &handle,
                   const std::vector<unsigned char> &content) {
    if (content.size() > static_cast<size_t>(kMaxStagedBytes)) {
        throw SqlError("staged file is larger than 128 MiB", SQLITE_TOOBIG);
    }
    const sqlite3_int64 chunk_count =
        (static_cast<sqlite3_int64>(content.size()) + kContentChunkBytes - 1) /
        kContentChunkBytes;
    Statement insert(db,
        "INSERT INTO _vexfs_staging(handle_id,generation,base_manifest_id,base_size,"
        "base_visible_size,logical_size,capacity) VALUES(?1,?2,NULL,0,0,?3,?4)");
    insert.BindText(1, handle.id);
    insert.BindInt64(2, handle.dirty_generation);
    insert.BindInt64(3, static_cast<sqlite3_int64>(content.size()));
    insert.BindInt64(4, static_cast<sqlite3_int64>(content.size()));
    insert.Done();
    for (sqlite3_int64 chunk_no = 0; chunk_no < chunk_count; ++chunk_no) {
        const sqlite3_int64 offset = chunk_no * kContentChunkBytes;
        const size_t bytes = static_cast<size_t>(std::min<sqlite3_int64>(
            kContentChunkBytes, static_cast<sqlite3_int64>(content.size()) - offset));
        std::vector<unsigned char> chunk(content.begin() + offset,
                                         content.begin() + offset + bytes);
        StoreDirtyChunk(db, handle.id, chunk_no, chunk);
    }
}

void CreateStagingFromVersion(sqlite3 *db, const Handle &handle, const Node &node) {
    if (node.version <= 0 || node.size == 0) {
        CreateStaging(db, handle, {});
        return;
    }
    const VersionStorage storage = ResolveVersionStorage(db, node.id, node.version);
    if (storage.size != node.size) {
        throw SqlError("current file size does not match manifest", SQLITE_CORRUPT);
    }
    Statement insert(db,
        "INSERT INTO _vexfs_staging(handle_id,generation,base_manifest_id,base_size,"
        "base_visible_size,logical_size,capacity) VALUES(?1,?2,?3,?4,?4,?4,0)");
    insert.BindText(1, handle.id);
    insert.BindInt64(2, handle.dirty_generation);
    insert.BindInt64(3, storage.manifest_id);
    insert.BindInt64(4, storage.size);
    insert.Done();
}

sqlite3_int64 StoreManifestFromStaging(sqlite3 *db, sqlite3_int64 workspace_id,
                                       sqlite3_int64 inode_id,
                                       sqlite3_int64 previous_manifest,
                                       const Handle &handle,
                                       const StagingInfo &staging,
                                       std::string *checksum_out) {
    if (staging.logical_size < 0 || staging.logical_size > kMaxStagedBytes ||
        staging.base_size < 0 || staging.base_size > kMaxStagedBytes ||
        staging.base_visible_size < 0 ||
        staging.base_visible_size > staging.base_size || staging.capacity < 0 ||
        staging.capacity > kMaxStagedBytes) {
        throw SqlError("staging size is corrupt", SQLITE_CORRUPT);
    }
    Statement dirty_stats(db,
        "SELECT count(*),COALESCE(sum(length(content)),0),COALESCE(max(chunk_no),-1) "
        "FROM _vexfs_staging_chunks WHERE handle_id=?1");
    dirty_stats.BindText(1, handle.id);
    dirty_stats.Row();
    const sqlite3_int64 chunk_count =
        (staging.logical_size + kContentChunkBytes - 1) / kContentChunkBytes;
    if (dirty_stats.Int64(1) != staging.capacity ||
        (dirty_stats.Int64(0) > 0 && dirty_stats.Int64(2) >= chunk_count)) {
        throw SqlError("staging chunk set is corrupt", SQLITE_CORRUPT);
    }
    std::vector<unsigned char> buffer(static_cast<size_t>(kContentChunkBytes));
    const std::string pending_checksum(64, '0');
    Statement manifest(db, R"SQL(
INSERT INTO _vexfs_manifests(workspace_id,file_size,chunk_size,chunk_count,checksum)
VALUES(?1,?2,?3,?4,?5)
)SQL");
    manifest.BindInt64(1, workspace_id);
    manifest.BindInt64(2, staging.logical_size);
    manifest.BindInt64(3, kContentChunkBytes);
    manifest.BindInt64(4, chunk_count);
    manifest.BindText(5, pending_checksum);
    manifest.Done();
    const sqlite3_int64 manifest_id = sqlite3_last_insert_rowid(db);
    Statement previous(db, R"SQL(
SELECT chunk.id,chunk.content,chunk.size,chunk.checksum
FROM _vexfs_manifest_chunks entry
JOIN _vexfs_chunks chunk ON chunk.id=entry.chunk_id
WHERE entry.manifest_id=?1 AND entry.chunk_no=?2 AND chunk.inode_id=?3
)SQL");
    Statement chunk(db, R"SQL(
INSERT INTO _vexfs_chunks(workspace_id,inode_id,content,size,checksum)
VALUES(?1,?2,?3,?4,?5)
)SQL");
    Statement entry(db, R"SQL(
INSERT INTO _vexfs_manifest_chunks(manifest_id,chunk_no,chunk_id)
VALUES(?1,?2,?3)
)SQL");
    Statement staged_chunk(db, R"SQL(
SELECT dirty.content,base.id,base.content,base.size,base.checksum,base.inode_id
FROM (SELECT 1) seed
LEFT JOIN _vexfs_staging_chunks dirty
  ON dirty.handle_id=?1 AND dirty.chunk_no=?2
LEFT JOIN _vexfs_manifest_chunks base_entry
  ON base_entry.manifest_id=?3 AND base_entry.chunk_no=?2
LEFT JOIN _vexfs_chunks base ON base.id=base_entry.chunk_id
)SQL");
    std::unordered_map<std::string, sqlite3_int64> new_chunks;
    vexfs::Sha256 file_hash;
    for (sqlite3_int64 chunk_no = 0, offset = 0; chunk_no < chunk_count;
         ++chunk_no, offset += kContentChunkBytes) {
        const int bytes = static_cast<int>(std::min<sqlite3_int64>(
            kContentChunkBytes, staging.logical_size - offset));
        std::fill(buffer.begin(), buffer.end(), 0);
        sqlite3_int64 chunk_id = 0;
        std::string base_checksum;
        staged_chunk.BindText(1, handle.id);
        staged_chunk.BindInt64(2, chunk_no);
        staged_chunk.BindInt64(3, staging.base_manifest_id);
        if (!staged_chunk.Row()) {
            throw SqlError("cannot resolve staging chunk", SQLITE_CORRUPT);
        }
        if (staged_chunk.Type(0) != SQLITE_NULL) {
            const int dirty_bytes = staged_chunk.BlobBytes(0);
            if (dirty_bytes <= 0 || dirty_bytes > kContentChunkBytes ||
                staged_chunk.BlobData(0) == nullptr) {
                throw SqlError("staging chunk is corrupt", SQLITE_CORRUPT);
            }
            std::memcpy(buffer.data(), staged_chunk.BlobData(0),
                        static_cast<size_t>(dirty_bytes));
        } else if (offset < staging.base_visible_size) {
            const sqlite3_int64 expected_base_size = std::min<sqlite3_int64>(
                kContentChunkBytes, staging.base_size - offset);
            if (staging.base_manifest_id <= 0 || expected_base_size <= 0 ||
                staged_chunk.Type(1) == SQLITE_NULL || staged_chunk.Int64(3) != expected_base_size ||
                staged_chunk.BlobBytes(2) != expected_base_size ||
                staged_chunk.BlobData(2) == nullptr || staged_chunk.Text(4).size() != 64 ||
                staged_chunk.Int64(5) != inode_id) {
                throw SqlError("staging base chunk is corrupt", SQLITE_CORRUPT);
            }
            const sqlite3_int64 visible = std::min<sqlite3_int64>(
                expected_base_size, staging.base_visible_size - offset);
            std::memcpy(buffer.data(), staged_chunk.BlobData(2),
                        static_cast<size_t>(visible));
            base_checksum = staged_chunk.Text(4);
            if (visible == expected_base_size && expected_base_size == bytes) {
                chunk_id = staged_chunk.Int64(1);
            } else if (vexfs::Sha256Hex(
                           staged_chunk.BlobData(2),
                           static_cast<size_t>(expected_base_size)) != base_checksum) {
                throw SqlError("staging base chunk checksum mismatch", SQLITE_CORRUPT);
            }
        }
        staged_chunk.Reset();
        file_hash.Update(buffer.data(), static_cast<size_t>(bytes));
        const std::string chunk_checksum =
            vexfs::Sha256Hex(buffer.data(), static_cast<size_t>(bytes));
        if (chunk_id > 0 && chunk_checksum != base_checksum) {
            throw SqlError("staging base chunk checksum mismatch", SQLITE_CORRUPT);
        }
        if (chunk_id == 0 && previous_manifest > 0) {
            previous.BindInt64(1, previous_manifest);
            previous.BindInt64(2, chunk_no);
            previous.BindInt64(3, inode_id);
            if (previous.Row() && previous.Int64(2) == bytes &&
                previous.Text(3) == chunk_checksum &&
                previous.BlobEquals(1, buffer.data(), static_cast<size_t>(bytes))) {
                chunk_id = previous.Int64(0);
            }
            previous.Reset();
        }
        const std::string reuse_key = std::to_string(bytes) + ":" + chunk_checksum;
        if (chunk_id == 0) {
            const auto reused = new_chunks.find(reuse_key);
            if (reused != new_chunks.end()) chunk_id = reused->second;
        }
        if (chunk_id == 0) {
            chunk.BindInt64(1, workspace_id);
            chunk.BindInt64(2, inode_id);
            chunk.BindBlobView(3, buffer.data(), static_cast<size_t>(bytes));
            chunk.BindInt64(4, bytes);
            chunk.BindText(5, chunk_checksum);
            chunk.Done();
            chunk_id = sqlite3_last_insert_rowid(db);
            new_chunks.emplace(reuse_key, chunk_id);
            chunk.Reset();
        }
        entry.BindInt64(1, manifest_id);
        entry.BindInt64(2, chunk_no);
        entry.BindInt64(3, chunk_id);
        entry.Done();
        entry.Reset();
    }
    const std::string checksum = vexfs::Hex(file_hash.Finish());
    Statement finalize(db,
        "UPDATE _vexfs_manifests SET checksum=?1 WHERE id=?2");
    finalize.BindText(1, checksum);
    finalize.BindInt64(2, manifest_id);
    finalize.Done();
    *checksum_out = checksum;
    return manifest_id;
}

sqlite3_int64 StageWrite(sqlite3 *db, const Handle &handle, sqlite3_int64 offset,
                         const std::vector<unsigned char> &patch) {
    if (patch.empty()) return handle.dirty_generation;
    if (offset < 0 || offset > kMaxStagedBytes ||
        patch.size() > static_cast<size_t>(kMaxStagedBytes - offset)) {
        throw SqlError("staged file is larger than 128 MiB", SQLITE_TOOBIG);
    }
    const sqlite3_int64 required = offset + static_cast<sqlite3_int64>(patch.size());
    const StagingInfo staging = FindStaging(db, handle);
    sqlite3_int64 capacity_delta = 0;
    const sqlite3_int64 logical_size = std::max(staging.logical_size, required);
    size_t patch_offset = 0;
    while (patch_offset < patch.size()) {
        const sqlite3_int64 absolute = offset + static_cast<sqlite3_int64>(patch_offset);
        const sqlite3_int64 chunk_no = absolute / kContentChunkBytes;
        const sqlite3_int64 within = absolute % kContentChunkBytes;
        const size_t bytes = std::min<size_t>(
            patch.size() - patch_offset,
            static_cast<size_t>(kContentChunkBytes - within));
        const bool full_chunk = within == 0 && bytes == static_cast<size_t>(kContentChunkBytes);
        sqlite3_int64 previous_dirty_bytes = 0;
        std::vector<unsigned char> chunk;
        if (full_chunk) {
            previous_dirty_bytes = DirtyChunkSize(db, handle.id, chunk_no);
            chunk.assign(patch.begin() + static_cast<std::ptrdiff_t>(patch_offset),
                         patch.begin() + static_cast<std::ptrdiff_t>(patch_offset + bytes));
        } else {
            chunk = ReadEffectiveChunk(
                db, handle, staging, chunk_no, &previous_dirty_bytes);
            std::memcpy(chunk.data() + within, patch.data() + patch_offset, bytes);
            const sqlite3_int64 chunk_offset = chunk_no * kContentChunkBytes;
            const sqlite3_int64 stored_bytes = std::min<sqlite3_int64>(
                kContentChunkBytes, logical_size - chunk_offset);
            chunk.resize(static_cast<size_t>(stored_bytes));
        }
        StoreDirtyChunk(db, handle.id, chunk_no, chunk);
        capacity_delta += static_cast<sqlite3_int64>(chunk.size()) - previous_dirty_bytes;
        patch_offset += bytes;
    }
    const sqlite3_int64 generation = handle.dirty_generation + 1;
    Statement stage(db,
        "UPDATE _vexfs_staging SET generation=?1,logical_size=?2,capacity=capacity+?3,"
        "updated_at=CAST(strftime('%s','now') AS INTEGER)*1000 WHERE handle_id=?4");
    stage.BindInt64(1, generation);
    stage.BindInt64(2, logical_size);
    stage.BindInt64(3, capacity_delta);
    stage.BindText(4, handle.id);
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
    sqlite3_int64 base_visible_size = staging.base_visible_size;
    if (size < staging.logical_size) {
        const sqlite3_int64 remainder = size % kContentChunkBytes;
        std::vector<unsigned char> boundary;
        if (remainder != 0) {
            boundary = ReadEffectiveChunk(db, handle, staging, size / kContentChunkBytes);
            boundary.resize(static_cast<size_t>(remainder));
        }
        const sqlite3_int64 first_removed =
            (size + kContentChunkBytes - 1) / kContentChunkBytes;
        Statement remove(db,
            "DELETE FROM _vexfs_staging_chunks WHERE handle_id=?1 AND chunk_no>=?2");
        remove.BindText(1, handle.id);
        remove.BindInt64(2, first_removed);
        remove.Done();
        if (remainder != 0) {
            StoreDirtyChunk(db, handle.id, size / kContentChunkBytes, boundary);
        }
        base_visible_size = std::min(base_visible_size, size);
    }
    Statement dirty_bytes(db,
        "SELECT COALESCE(sum(length(content)),0) FROM _vexfs_staging_chunks "
        "WHERE handle_id=?1");
    dirty_bytes.BindText(1, handle.id);
    dirty_bytes.Row();
    const sqlite3_int64 generation = handle.dirty_generation + 1;
    Statement stage(db,
        "UPDATE _vexfs_staging SET generation=?1,logical_size=?2,base_visible_size=?3,"
        "capacity=?4,updated_at=CAST(strftime('%s','now') AS INTEGER)*1000 "
        "WHERE handle_id=?5");
    stage.BindInt64(1, generation);
    stage.BindInt64(2, size);
    stage.BindInt64(3, base_visible_size);
    stage.BindInt64(4, dirty_bytes.Int64(0));
    stage.BindText(5, handle.id);
    stage.Done();
    Statement update(db,
        "UPDATE _vexfs_handles SET dirty_generation=?1, "
        "updated_at=CAST(strftime('%s','now') AS INTEGER)*1000 WHERE id=?2");
    update.BindInt64(1, generation);
    update.BindText(2, handle.id);
    update.Done();
    return generation;
}

std::vector<unsigned char> ReadStagingRange(sqlite3 *db, const Handle &handle,
                                            sqlite3_int64 offset, sqlite3_int64 length) {
    if (offset < 0 || length < 0 || offset > kMaxStagedBytes ||
        length > kMaxStagedBytes) {
        throw SqlError("read range must be within 0..128 MiB", SQLITE_RANGE);
    }
    const StagingInfo staging = FindStaging(db, handle);
    if (length == 0 || offset >= staging.logical_size) return {};
    const sqlite3_int64 bytes = std::min<sqlite3_int64>(
        length, staging.logical_size - offset);
    std::vector<unsigned char> result(static_cast<size_t>(bytes));
    sqlite3_int64 copied = 0;
    while (copied < bytes) {
        const sqlite3_int64 absolute = offset + copied;
        const sqlite3_int64 chunk_no = absolute / kContentChunkBytes;
        const sqlite3_int64 within = absolute % kContentChunkBytes;
        const sqlite3_int64 take = std::min<sqlite3_int64>(
            bytes - copied, kContentChunkBytes - within);
        const std::vector<unsigned char> chunk =
            ReadEffectiveChunk(db, handle, staging, chunk_no);
        std::memcpy(result.data() + copied, chunk.data() + within,
                    static_cast<size_t>(take));
        copied += take;
    }
    return result;
}

bool FindActiveMountStaging(sqlite3 *db, sqlite3_int64 workspace_id,
                            sqlite3_int64 inode_id, Handle *result) {
    Statement statement(db, R"SQL(
SELECT h.id,h.workspace_id,h.inode_id,h.writable,h.expected_version,
       h.dirty_generation,h.published_generation,h.state,
       COALESCE(h.owner_session,'')
FROM _vexfs_handles h
JOIN _vexfs_mount_sessions session
  ON session.workspace_id=h.workspace_id AND session.session_id=h.owner_session
WHERE h.workspace_id=?1 AND h.inode_id=?2 AND h.writable=1
  AND h.state='open' AND h.dirty_generation>h.published_generation
  AND session.lease_until>CAST(unixepoch('subsec')*1000 AS INTEGER)
ORDER BY h.updated_at DESC,h.id DESC
LIMIT 1
)SQL");
    statement.BindInt64(1, workspace_id);
    statement.BindInt64(2, inode_id);
    if (!statement.Row()) return false;
    *result = {statement.Text(0), statement.Int64(1), statement.Int64(2),
               statement.Int(3) != 0, statement.Int64(4), statement.Int64(5),
               statement.Int64(6), statement.Text(7), statement.Text(8)};
    return true;
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
        "SELECT id, kind, size, current_version, mode, owner_principal, uid, gid, "
        "created_at, accessed_at, updated_at, changed_at FROM _vexfs_inodes "
        "WHERE id = ?1 AND (?2 OR deleted_at IS NULL)");
    statement.BindInt64(1, id);
    statement.BindInt(2, include_deleted ? 1 : 0);
    if (!statement.Row()) throw SqlError("inode not found", SQLITE_NOTFOUND);
    return {statement.Int64(0), statement.Text(1), statement.Int64(2),
            statement.Int64(3), statement.Int(4), statement.Text(5), statement.Int64(6),
            statement.Int64(7), statement.Int64(8), statement.Int64(9),
             statement.Int64(10), statement.Int64(11)};
}

sqlite3_int64 StoreVersionFromStaging(sqlite3 *db, const Workspace &workspace,
                                      const Node &node, const Handle &handle,
                                      sqlite3_int64 generation,
                                      const std::string &message,
                                      bool transaction_owned = false) {
    // PublishHandleInTransaction always owns the surrounding savepoint. Keeping
    // another savepoint here added two SQL statements to every small-file close
    // without adding another rollback boundary.
    if (!transaction_owned) AcquireWriteLock(db);
    const StagingInfo staging = FindStaging(db, handle);
    if (staging.generation != generation) {
        throw SqlError("staged generation is stale or missing", SQLITE_NOTFOUND);
    }
    const sqlite3_int64 previous_manifest = node.version > 0
        ? ResolveVersionStorage(db, node.id, node.version).manifest_id : 0;
    CheckQuotaForContentSize(db, workspace.id, node.id, staging.logical_size);
    const sqlite3_int64 version = node.version + 1;
    Statement advance(db,
        "UPDATE _vexfs_inodes SET size=?1,current_version=?2,"
        "updated_at=CAST(unixepoch('subsec')*1000 AS INTEGER),"
        "changed_at=CAST(unixepoch('subsec')*1000 AS INTEGER) "
        "WHERE id=?3 AND current_version=?4");
    advance.BindInt64(1, staging.logical_size);
    advance.BindInt64(2, version);
    advance.BindInt64(3, node.id);
    advance.BindInt64(4, node.version);
    advance.Done();
    if (sqlite3_changes64(db) != 1) {
        throw SqlError("version conflict: file changed before publish", SQLITE_CONSTRAINT);
    }
    const sqlite3_int64 commit = transaction_owned
        ? CreateCommitInTransaction(db, workspace, message)
        : CreateCommit(db, workspace, message);
    std::string checksum;
    const sqlite3_int64 manifest_id = StoreManifestFromStaging(
        db, workspace.id, node.id, previous_manifest, handle, staging, &checksum);
    Statement insert(db, R"SQL(
INSERT INTO _vexfs_file_versions(
 inode_id,version_no,commit_id,manifest_id,size,checksum)
VALUES(?1,?2,?3,?4,?5,?6)
)SQL");
    insert.BindInt64(1, node.id);
    insert.BindInt64(2, version);
    insert.BindInt64(3, commit);
    insert.BindInt64(4, manifest_id);
    insert.BindInt64(5, staging.logical_size);
    insert.BindText(6, checksum);
    insert.Done();
    UpdateGrepIndexFromVersion(db, workspace.id, node.id, version);
    Statement rebase(db,
        "UPDATE _vexfs_staging SET base_manifest_id=?1,base_size=logical_size,"
        "base_visible_size=logical_size,capacity=0,"
        "updated_at=CAST(strftime('%s','now') AS INTEGER)*1000 WHERE handle_id=?2");
    rebase.BindInt64(1, manifest_id);
    rebase.BindText(2, handle.id);
    rebase.Done();
    Statement clear_overlay(db,
        "DELETE FROM _vexfs_staging_chunks WHERE handle_id=?1");
    clear_overlay.BindText(1, handle.id);
    clear_overlay.Done();
    return version;
}

sqlite3_int64 PublishHandleInTransaction(sqlite3 *db, Handle handle,
                                         sqlite3_int64 generation,
                                         bool transaction_owned = false) {
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
    Workspace workspace = WorkspaceById(db, handle.workspace_id);
    const sqlite3_int64 version = StoreVersionFromStaging(
        db, workspace, node, handle, generation, "handle publish", transaction_owned);

    Statement update(db,
        "UPDATE _vexfs_handles SET expected_version = ?1, published_generation = ?2, "
        "updated_at = CAST(strftime('%s','now') AS INTEGER) * 1000 WHERE id = ?3");
    update.BindInt64(1, version);
    update.BindInt64(2, generation);
    update.BindText(3, handle.id);
    update.Done();
    return version;
}

sqlite3_int64 PublishHandle(sqlite3 *db, Handle handle, sqlite3_int64 generation) {
    Savepoint savepoint(db, "vexfs_publish_handle");
    const sqlite3_int64 version = PublishHandleInTransaction(
        db, std::move(handle), generation);
    savepoint.Release();
    return version;
}

bool HasPendingContentPublish(sqlite3 *db, sqlite3_int64 workspace_id,
                              sqlite3_int64 inode_id) {
    Statement statement(db,
        "SELECT 1 FROM _vexfs_handles WHERE workspace_id=?1 AND inode_id=?2 "
        "AND writable=1 AND state IN ('open','retained') "
        "AND dirty_generation>published_generation LIMIT 1");
    statement.BindInt64(1, workspace_id);
    statement.BindInt64(2, inode_id);
    return statement.Row();
}

void CommitMetadataChange(sqlite3 *db, const Workspace &workspace,
                          sqlite3_int64 inode_id, const char *message) {
    // FSKit commonly sets mode/times/com.apple.provenance between create and
    // the first content publish.  Leave those trigger rows dirty so the content
    // publish flushes one coherent history commit instead of three.
    if (!HasPendingContentPublish(db, workspace.id, inode_id)) {
        CreateCommit(db, workspace, message);
    }
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
        EnsureSchemaForced(sqlite3_context_db_handle(context));
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
        Statement acl(db,
            "INSERT INTO _vexfs_acl_entries(workspace_id,inode_id,principal_id,effect,permissions,inherit_flags) "
            "VALUES(?1,?2,'local','allow','traverse,list,read,write,create,delete,rename,history,snapshot,share,admin',1)");
        acl.BindInt64(1, workspace_id);
        acl.BindInt64(2, root);
        acl.Done();
        CreateCommit(db, Workspace{workspace_id, root, 0}, "create workspace");
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

void CreateFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const auto parts = PathParts(RequiredText(values[1], "path"));
        const std::string kind = RequiredText(values[2], "kind");
        const int mode = RequiredMode(values[3]);
        if (parts.empty()) throw SqlError("workspace root already exists", SQLITE_CONSTRAINT);
        if (kind != "file" && kind != "directory") {
            throw SqlError("special files (fifo, device and socket) are not supported",
                           SQLITE_MISMATCH);
        }

        Savepoint savepoint(db, "vexfs_create_item");
        AcquireWriteLock(db);
        auto [parent, name] = ResolveParent(db, workspace, parts);
        Node existing;
        if (FindChild(db, workspace.id, parent.id, name, &existing)) {
            throw SqlError("destination already exists", SQLITE_CONSTRAINT);
        }
        Node node;
        node.id = CreateInode(db, workspace.id, kind.c_str(), mode);
        node.kind = kind;
        node.mode = mode;
        AddDentry(db, workspace.id, parent.id, name, node.id);
        if (kind == "file") {
            StoreVersion(db, workspace, node, {}, "create file");
        } else {
            CreateCommit(db, workspace, "create directory");
        }
        savepoint.Release();
        sqlite3_result_int64(context, node.id);
    });
}

void SymlinkFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const auto parts = PathParts(RequiredText(values[1], "path"));
        const auto target = RequiredBlob(values[2], "symlink target");
        if (parts.empty()) throw SqlError("workspace root already exists", SQLITE_CONSTRAINT);
        if (target.empty()) throw SqlError("symlink target must not be empty", SQLITE_MISMATCH);
        if (static_cast<sqlite3_int64>(target.size()) > kMaxSymlinkBytes) {
            throw SqlError("symlink target is longer than 4096 bytes", SQLITE_TOOBIG);
        }
        if (std::find(target.begin(), target.end(), 0) != target.end()) {
            throw SqlError("symlink target contains NUL", SQLITE_MISMATCH);
        }

        Savepoint savepoint(db, "vexfs_create_symlink");
        AcquireWriteLock(db);
        auto [parent, name] = ResolveParent(db, workspace, parts);
        Node existing;
        if (FindChild(db, workspace.id, parent.id, name, &existing)) {
            throw SqlError("destination already exists", SQLITE_CONSTRAINT);
        }
        Node node;
        node.id = CreateInode(db, workspace.id, "symlink", 0777);
        node.kind = "symlink";
        node.mode = 0777;
        AddDentry(db, workspace.id, parent.id, name, node.id);
        StoreVersion(db, workspace, node, target, "create symlink");
        savepoint.Release();
        sqlite3_result_int64(context, node.id);
    });
}

void HardlinkFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const auto source_parts = PathParts(RequiredText(values[1], "source"));
        const auto destination_parts = PathParts(RequiredText(values[2], "destination"));
        if (source_parts.empty() || destination_parts.empty()) {
            throw SqlError("hard links cannot target the workspace root", SQLITE_MISMATCH);
        }
        Savepoint savepoint(db, "vexfs_hardlink");
        AcquireWriteLock(db);
        const Node source = Resolve(db, workspace, source_parts);
        if (source.kind != "file") {
            throw SqlError("hard links are supported only for regular files", SQLITE_MISMATCH);
        }
        auto [parent, name] = ResolveParent(db, workspace, destination_parts);
        Node existing;
        if (FindChild(db, workspace.id, parent.id, name, &existing)) {
            throw SqlError("destination already exists", SQLITE_CONSTRAINT);
        }
        AddDentry(db, workspace.id, parent.id, name, source.id);
        CreateCommit(db, workspace, "create hard link");
        savepoint.Release();
        sqlite3_result_int64(context, source.id);
    });
}

void RequireWorkspaceInode(sqlite3 *db, const Workspace &workspace, sqlite3_int64 inode);

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

void AppendFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const sqlite3_int64 inode = RequiredPositiveInteger(values[1], "inode");
        const auto suffix = RequiredBlob(values[2], "content");
        Savepoint savepoint(db, "vexfs_append");
        AcquireWriteLock(db);
        RequireWorkspaceInode(db, workspace, inode);
        const Node node = NodeById(db, inode, true);
        if (node.kind != "file") throw SqlError("inode is not a file", SQLITE_MISMATCH);
        if (node.size < 0 || node.size > kMaxStagedBytes ||
            suffix.size() > static_cast<size_t>(kMaxStagedBytes - node.size)) {
            throw SqlError("file is larger than the Phase 0 limit (128 MiB)", SQLITE_TOOBIG);
        }
        std::vector<unsigned char> content = ReadNode(db, node);
        content.insert(content.end(), suffix.begin(), suffix.end());
        const sqlite3_int64 version = StoreVersion(
            db, workspace, node, content, "append file");
        savepoint.Release();
        sqlite3_result_int64(context, version);
    });
}

void ReadFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const Node node = Resolve(db, workspace, PathParts(RequiredText(values[1], "path")));
        // A mounted workspace stores its mutable head in staging and seals version
        // history in batches.  Other readers must still observe NFS FILE_SYNC
        // writes immediately.  Ordinary SDK handles remain private because they
        // do not own the active mount session.
        Handle mounted;
        if (node.kind == "file" &&
            FindActiveMountStaging(db, workspace.id, node.id, &mounted)) {
            ResultBlob(context, ReadStagingRange(db, mounted, 0, kMaxStagedBytes));
            return;
        }
        ResultBlob(context, ReadNode(db, node));
    });
}

void ReadRangeFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const Node node = Resolve(db, workspace, PathParts(RequiredText(values[1], "path")));
        const sqlite3_int64 offset = RequiredNonnegativeInteger(values[2], "offset");
        const sqlite3_int64 length = RequiredNonnegativeInteger(values[3], "length");
        if (node.kind != "file") throw SqlError("path is not a file", SQLITE_MISMATCH);
        Handle mounted;
        if (length != 0 &&
            FindActiveMountStaging(db, workspace.id, node.id, &mounted)) {
            ResultBlob(context, ReadStagingRange(db, mounted, offset, length));
            return;
        }
        if (node.version == 0 || length == 0 || offset >= node.size) {
            ResultBlob(context, std::vector<unsigned char>{});
            return;
        }
        ResultBlob(context, ReadVersionRange(db, node.id, node.version, offset, length));
    });
}

void GrepFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const auto parts = PathParts(RequiredText(values[1], "path"));
        const Node root = Resolve(db, workspace, parts);
        const std::string pattern = RequiredText(values[2], "pattern");
        const sqlite3_int64 flags = RequiredNonnegativeInteger(values[3], "flags");
        const sqlite3_int64 limit = RequiredPositiveInteger(values[4], "limit");
        if (pattern.empty() || pattern.size() > kMaxGrepPatternBytes) {
            throw SqlError("grep pattern must be 1..4096 bytes", SQLITE_RANGE);
        }
        if ((flags & ~3LL) != 0) throw SqlError("unsupported grep flags", SQLITE_RANGE);
        if (limit > kMaxGrepResults) {
            throw SqlError("grep limit must be at most 10240", SQLITE_RANGE);
        }
        const bool ignore_case = (flags & 1) != 0;
        const bool files_only = (flags & 2) != 0;
        const std::vector<unsigned char> pattern_bytes(pattern.begin(), pattern.end());
        size_t pattern_characters = 0;
        if (IsUtf8Text(pattern_bytes)) {
            pattern_characters = static_cast<size_t>(std::count_if(
                pattern_bytes.begin(), pattern_bytes.end(),
                [](unsigned char value) { return (value & 0xc0) != 0x80; }));
        }
        const bool index_used = GetGrepIndexState(db) == GrepIndexState::kAvailable &&
            pattern_characters >= 3;
        constexpr const char *scan_query = R"SQL(
WITH RECURSIVE tree(inode_id,path,kind,current_version,size) AS (
  SELECT i.id,?2,i.kind,i.current_version,i.size
  FROM _vexfs_inodes i WHERE i.id=?3 AND i.workspace_id=?1 AND i.deleted_at IS NULL
  UNION ALL
  SELECT child.id,
         CASE WHEN tree.path='/' THEN '/'||d.name ELSE tree.path||'/'||d.name END,
         child.kind,child.current_version,child.size
  FROM tree
  JOIN _vexfs_dentries d ON d.workspace_id=?1 AND d.parent_inode=tree.inode_id
  JOIN _vexfs_inodes child ON child.id=d.inode_id AND child.deleted_at IS NULL
  WHERE tree.kind='directory'
)
SELECT tree.path,tree.inode_id,tree.current_version,tree.size,version.checksum
FROM tree
JOIN _vexfs_file_versions version
  ON version.inode_id=tree.inode_id AND version.version_no=tree.current_version
WHERE tree.kind='file'
)SQL";
        constexpr const char *indexed_query = R"SQL(
WITH RECURSIVE
candidates(inode_id) AS MATERIALIZED (
  SELECT rowid FROM _vexfs_content_fts WHERE _vexfs_content_fts MATCH ?4
),
ancestors(inode_id,parent_inode,path,depth) AS (
  SELECT d.inode_id,d.parent_inode,d.name,1
  FROM candidates candidate
  JOIN _vexfs_dentries d ON d.inode_id=candidate.inode_id AND d.workspace_id=?1
  UNION ALL
  SELECT ancestor.inode_id,d.parent_inode,d.name||'/'||ancestor.path,ancestor.depth+1
  FROM ancestors ancestor
  JOIN _vexfs_dentries d ON d.inode_id=ancestor.parent_inode AND d.workspace_id=?1
  WHERE ancestor.parent_inode<>?3 AND ancestor.depth<1024
),
paths(inode_id,path) AS MATERIALIZED (
  SELECT inode_id,'/'||path FROM ancestors WHERE parent_inode=?3
)
SELECT paths.path,inode.id,inode.current_version,inode.size,version.checksum
FROM paths
JOIN _vexfs_inodes inode ON inode.id=paths.inode_id AND inode.deleted_at IS NULL
JOIN _vexfs_file_versions version
  ON version.inode_id=inode.id AND version.version_no=inode.current_version
WHERE inode.kind='file' AND
      (?2='/' OR paths.path=?2 OR substr(paths.path,1,length(?2)+1)=?2||'/')
)SQL";
        Statement files(db, index_used ? indexed_query : scan_query);
        files.BindInt64(1, workspace.id);
        files.BindText(2, CanonicalPath(parts));
        files.BindInt64(3, index_used ? workspace.root_inode : root.id);
        if (index_used) {
            std::string quoted_pattern = "\"";
            for (const char value : pattern) {
                if (value == '"') quoted_pattern += "\"\"";
                else quoted_pattern.push_back(value);
            }
            quoted_pattern.push_back('"');
            files.BindText(4, quoted_pattern);
        }

        std::string matches = "[";
        bool first_match = true;
        bool truncated = false;
        sqlite3_int64 match_count = 0;
        sqlite3_int64 files_scanned = 0;
        sqlite3_int64 bytes_scanned = 0;
        sqlite3_int64 binary_files_skipped = 0;
        while (files.Row()) {
            ++files_scanned;
            const sqlite3_int64 expected_size = files.Int64(3);
            if (expected_size < 0 || expected_size > kMaxStagedBytes) {
                throw SqlError("current file content reference is corrupt", SQLITE_CORRUPT);
            }
            const std::vector<unsigned char> content =
                ReadVersionContent(db, files.Int64(1), files.Int64(2));
            if (static_cast<sqlite3_int64>(content.size()) != expected_size) {
                throw SqlError("current file size does not match content", SQLITE_CORRUPT);
            }
            if (!IsSha256(files.Text(4)) ||
                vexfs::Sha256Hex(content.data(), content.size()) != files.Text(4)) {
                throw SqlError("current file checksum does not match content", SQLITE_CORRUPT);
            }
            bytes_scanned += expected_size;
            if (!IsUtf8Text(content)) {
                ++binary_files_skipped;
                continue;
            }
            const std::string path = files.Text(0);
            size_t line_start = 0;
            sqlite3_int64 line_number = 1;
            while (line_start < content.size()) {
                const auto begin = content.begin() + static_cast<std::ptrdiff_t>(line_start);
                const auto newline = std::find(begin, content.end(), static_cast<unsigned char>('\n'));
                const size_t line_end = static_cast<size_t>(newline - content.begin());
                if (LineContains(content.data() + line_start, content.data() + line_end,
                                 pattern, ignore_case)) {
                    if (!first_match) matches.push_back(',');
                    first_match = false;
                    const std::string line(reinterpret_cast<const char *>(content.data() + line_start),
                                           line_end - line_start);
                    matches += "{\"path\":\"" + JsonEscape(path) +
                        "\",\"line\":" + std::to_string(line_number) +
                        ",\"text\":\"" + JsonEscape(line) + "\"}";
                    ++match_count;
                    if (match_count == limit) {
                        truncated = true;
                        break;
                    }
                    if (files_only) break;
                }
                if (newline == content.end()) break;
                line_start = line_end + 1;
                ++line_number;
            }
            if (truncated) break;
        }
        matches.push_back(']');
        const std::string json = "{\"matches\":" + matches +
            ",\"match_count\":" + std::to_string(match_count) +
            ",\"files_scanned\":" + std::to_string(files_scanned) +
            ",\"bytes_scanned\":" + std::to_string(bytes_scanned) +
            ",\"binary_files_skipped\":" + std::to_string(binary_files_skipped) +
            ",\"index_used\":" + (index_used ? "true" : "false") +
            ",\"truncated\":" + (truncated ? "true" : "false") + "}";
        sqlite3_result_text(context, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    });
}

std::string GrepIndexStatus(sqlite3 *db) {
    const GrepIndexState state = GetGrepIndexState(db);
    sqlite3_int64 indexed_files = 0;
    if (state == GrepIndexState::kAvailable) {
        Statement count(db, "SELECT count(*) FROM _vexfs_content_fts");
        if (count.Row()) indexed_files = count.Int64(0);
    }
    return "{\"enabled\":" + std::string(state == GrepIndexState::kDisabled ? "false" : "true") +
        ",\"available\":" + (state == GrepIndexState::kAvailable ? "true" : "false") +
        ",\"dirty\":" + (GrepIndexDirty(db) ? "true" : "false") +
        ",\"backend\":\"" +
        (state == GrepIndexState::kAvailable ? "fts5-trigram" : "scan") +
        "\",\"indexed_files\":" + std::to_string(indexed_files) + "}";
}

void GrepIndexFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string action = RequiredText(values[0], "action");
        if (action == "enable") {
            EnableGrepIndex(db);
        } else if (action == "rebuild") {
            if (GetGrepIndexState(db) != GrepIndexState::kAvailable) {
                throw SqlError("grep index is not enabled or available", SQLITE_MISMATCH);
            }
            RebuildGrepIndex(db);
        } else if (action == "disable") {
            DisableGrepIndex(db);
        } else if (action != "status") {
            throw SqlError("index action must be status, enable, rebuild or disable",
                           SQLITE_MISMATCH);
        }
        const std::string json = GrepIndexStatus(db);
        sqlite3_result_text(context, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
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
            "SELECT v.version_no, v.commit_id, c.parent_commit, v.size, v.created_at, c.message, "
            "v.checksum "
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
                "\",\"checksum\":\"" + JsonEscape(statement.Text(6)) +
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
        Statement from_rows(db, R"SQL(
SELECT entry.chunk_no,chunk.content,chunk.size,chunk.checksum
FROM _vexfs_manifest_chunks entry
JOIN _vexfs_chunks chunk ON chunk.id=entry.chunk_id
WHERE entry.manifest_id=?1 ORDER BY entry.chunk_no
)SQL");
        Statement to_rows(db, R"SQL(
SELECT entry.chunk_no,chunk.content,chunk.size,chunk.checksum
FROM _vexfs_manifest_chunks entry
JOIN _vexfs_chunks chunk ON chunk.id=entry.chunk_id
WHERE entry.manifest_id=?1 ORDER BY entry.chunk_no
)SQL");
        from_rows.BindInt64(1, from.manifest_id);
        to_rows.BindInt64(1, to.manifest_id);
        vexfs::Sha256 from_hash;
        vexfs::Sha256 to_hash;
        bool changed = from.size != to.size;
        bool binary = false;
        sqlite3_int64 chunk_no = 0;
        sqlite3_int64 from_chunks = 0;
        sqlite3_int64 to_chunks = 0;
        sqlite3_int64 from_bytes_total = 0;
        sqlite3_int64 to_bytes_total = 0;
        while (true) {
            const bool has_from = from_rows.Row();
            const bool has_to = to_rows.Row();
            if (!has_from && !has_to) break;
            std::vector<unsigned char> from_chunk;
            std::vector<unsigned char> to_chunk;
            if (has_from) {
                from_chunk = from_rows.Blob(1);
                if (from_rows.Int64(0) != chunk_no ||
                    from_rows.Int64(2) != static_cast<sqlite3_int64>(from_chunk.size()) ||
                    from_chunk.empty() || from_chunk.size() > kContentChunkBytes ||
                    vexfs::Sha256Hex(from_chunk.data(), from_chunk.size()) != from_rows.Text(3)) {
                    throw SqlError("file manifest chunk is corrupt", SQLITE_CORRUPT);
                }
                from_hash.Update(from_chunk.data(), from_chunk.size());
                from_bytes_total += static_cast<sqlite3_int64>(from_chunk.size());
                ++from_chunks;
            }
            if (has_to) {
                to_chunk = to_rows.Blob(1);
                if (to_rows.Int64(0) != chunk_no ||
                    to_rows.Int64(2) != static_cast<sqlite3_int64>(to_chunk.size()) ||
                    to_chunk.empty() || to_chunk.size() > kContentChunkBytes ||
                    vexfs::Sha256Hex(to_chunk.data(), to_chunk.size()) != to_rows.Text(3)) {
                    throw SqlError("file manifest chunk is corrupt", SQLITE_CORRUPT);
                }
                to_hash.Update(to_chunk.data(), to_chunk.size());
                to_bytes_total += static_cast<sqlite3_int64>(to_chunk.size());
                ++to_chunks;
            }
            binary = binary ||
                std::find(from_chunk.begin(), from_chunk.end(), 0) != from_chunk.end() ||
                std::find(to_chunk.begin(), to_chunk.end(), 0) != to_chunk.end();
            if (!changed && from_chunk != to_chunk) changed = true;
            ++chunk_no;
        }
        if (from_bytes_total != from.size || to_bytes_total != to.size ||
            from_chunks != from.chunk_count || to_chunks != to.chunk_count ||
            vexfs::Hex(from_hash.Finish()) != from.checksum ||
            vexfs::Hex(to_hash.Finish()) != to.checksum) {
            throw SqlError("file version checksum does not match content", SQLITE_CORRUPT);
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
        std::string checksum;
        if (node.kind != "directory" && node.version > 0) {
            Statement version(db,
                "SELECT checksum FROM _vexfs_file_versions WHERE inode_id=?1 AND version_no=?2");
            version.BindInt64(1, node.id);
            version.BindInt64(2, node.version);
            if (!version.Row()) throw SqlError("current file version is missing", SQLITE_CORRUPT);
            checksum = version.Text(0);
        }
        const std::string json = "{\"path\":\"" + JsonEscape(path) + "\",\"inode\":" +
            std::to_string(node.id) + ",\"kind\":\"" + node.kind + "\",\"mode\":" +
            std::to_string(node.mode) + ",\"owner_principal\":\"" +
            JsonEscape(node.owner_principal) + "\",\"uid\":" + std::to_string(node.uid) +
            ",\"gid\":" + std::to_string(node.gid) + ",\"size\":" + std::to_string(node.size) +
            ",\"link_count\":" + std::to_string(LinkCount(db, workspace, node.id)) +
            ",\"version\":" + std::to_string(node.version) + ",\"checksum\":" +
            (checksum.empty() ? "null" : "\"" + JsonEscape(checksum) + "\"") +
            ",\"created_at\":" +
            std::to_string(node.created_at) + ",\"accessed_at\":" +
            std::to_string(node.accessed_at) + ",\"updated_at\":" +
            std::to_string(node.updated_at) + ",\"changed_at\":" +
            std::to_string(node.changed_at) + "}";
        sqlite3_result_text(context, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    });
}

struct CheckIssue {
    std::string code;
    std::string object;
    std::string type;
    std::string message;
    std::string suggestion;
};

struct CheckReport {
    static constexpr size_t kMaxReportedIssues = 1000;

    std::vector<CheckIssue> issues;
    sqlite3_int64 total_issues = 0;
    sqlite3_int64 inodes = 0;
    sqlite3_int64 dentries = 0;
    sqlite3_int64 versions = 0;
    sqlite3_int64 manifests = 0;
    sqlite3_int64 chunks = 0;
    sqlite3_int64 content_bytes = 0;
    sqlite3_int64 commits = 0;
    sqlite3_int64 snapshots = 0;
    sqlite3_int64 handles = 0;
    sqlite3_int64 staging_objects = 0;
    sqlite3_int64 history_rows = 0;

    void Add(std::string code, std::string object, std::string type,
             std::string message, std::string suggestion) {
        ++total_issues;
        if (issues.size() < kMaxReportedIssues) {
            issues.push_back({std::move(code), std::move(object), std::move(type),
                              std::move(message), std::move(suggestion)});
        }
    }
};

bool IsSha256(const std::string &value) {
    if (value.size() != 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

std::string HashManifest(sqlite3 *db, sqlite3_int64 manifest_id,
                         sqlite3_int64 expected_size,
                         sqlite3_int64 expected_chunk_count) {
    Statement rows(db, R"SQL(
SELECT entry.chunk_no,chunk.content,chunk.size,chunk.checksum
FROM _vexfs_manifest_chunks entry
JOIN _vexfs_chunks chunk ON chunk.id=entry.chunk_id
WHERE entry.manifest_id=?1 ORDER BY entry.chunk_no
)SQL");
    rows.BindInt64(1, manifest_id);
    vexfs::Sha256 hash;
    sqlite3_int64 chunk_no = 0;
    sqlite3_int64 total = 0;
    while (rows.Row()) {
        const std::vector<unsigned char> content = rows.Blob(1);
        const sqlite3_int64 expected_chunk_size = std::min<sqlite3_int64>(
            kContentChunkBytes, expected_size - total);
        if (rows.Int64(0) != chunk_no || expected_chunk_size <= 0 ||
            rows.Int64(2) != expected_chunk_size ||
            static_cast<sqlite3_int64>(content.size()) != expected_chunk_size ||
            !IsSha256(rows.Text(3)) ||
            vexfs::Sha256Hex(content.data(), content.size()) != rows.Text(3)) {
            throw SqlError("file manifest chunk is corrupt", SQLITE_CORRUPT);
        }
        hash.Update(content.data(), content.size());
        total += expected_chunk_size;
        ++chunk_no;
    }
    if (total != expected_size || chunk_no != expected_chunk_count ||
        chunk_no != (expected_size + kContentChunkBytes - 1) / kContentChunkBytes) {
        throw SqlError("file manifest chunk count or size is corrupt", SQLITE_CORRUPT);
    }
    return vexfs::Hex(hash.Finish());
}

std::string CheckReportJson(const CheckReport &report, const std::string &workspace,
                            bool deep, sqlite3_int64 elapsed_ms) {
    std::string issues = "[";
    bool first = true;
    for (const CheckIssue &issue : report.issues) {
        if (!first) issues += ',';
        first = false;
        issues += "{\"code\":\"" + JsonEscape(issue.code) +
            "\",\"object\":\"" + JsonEscape(issue.object) +
            "\",\"type\":\"" + JsonEscape(issue.type) +
            "\",\"message\":\"" + JsonEscape(issue.message) +
            "\",\"suggestion\":\"" + JsonEscape(issue.suggestion) + "\"}";
    }
    issues += ']';
    return "{\"ok\":" + std::string(report.total_issues == 0 ? "true" : "false") +
        ",\"workspace\":\"" + JsonEscape(workspace) +
        "\",\"mode\":\"" + (deep ? "deep" : "quick") +
        "\",\"content_model\":\"chunked-v1\",\"checked\":{" +
        "\"inodes\":" + std::to_string(report.inodes) +
        ",\"dentries\":" + std::to_string(report.dentries) +
        ",\"versions\":" + std::to_string(report.versions) +
        ",\"manifests\":" + std::to_string(report.manifests) +
        ",\"chunks\":" + std::to_string(report.chunks) +
        ",\"content_bytes\":" + std::to_string(report.content_bytes) +
        ",\"commits\":" + std::to_string(report.commits) +
        ",\"snapshots\":" + std::to_string(report.snapshots) +
        ",\"handles\":" + std::to_string(report.handles) +
        ",\"staging_objects\":" + std::to_string(report.staging_objects) +
        ",\"history_rows\":" + std::to_string(report.history_rows) +
        "},\"issue_count\":" + std::to_string(report.total_issues) +
        ",\"reported_issue_count\":" + std::to_string(report.issues.size()) +
        ",\"truncated\":" +
        (report.total_issues > static_cast<sqlite3_int64>(report.issues.size()) ? "true" : "false") +
        ",\"elapsed_ms\":" + std::to_string(elapsed_ms) +
        ",\"issues\":" + issues + "}";
}

void CheckFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        const auto started = std::chrono::steady_clock::now();
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string workspace_name = RequiredText(values[0], "workspace");
        if (sqlite3_value_type(values[1]) != SQLITE_INTEGER) {
            throw SqlError("deep must be 0 or 1", SQLITE_MISMATCH);
        }
        const int deep_value = sqlite3_value_int(values[1]);
        if (deep_value != 0 && deep_value != 1) {
            throw SqlError("deep must be 0 or 1", SQLITE_RANGE);
        }
        const bool deep = deep_value != 0;
        const Workspace workspace = FindWorkspace(db, workspace_name);
        CheckReport report;

        try {
            Statement quick_check(db, "PRAGMA quick_check(100)");
            while (quick_check.Row()) {
                const std::string result = quick_check.Text(0);
                if (result != "ok") {
                    report.Add("VEXFS_SQLITE_CORRUPTION", "database", "sqlite",
                               result, "restore the SQLite database from a verified backup");
                }
            }
        } catch (const SqlError &error) {
            report.Add("VEXFS_SQLITE_CHECK_FAILED", "database", "sqlite", error.what(),
                       "run SQLite integrity_check offline and restore a verified backup");
        }

        std::unordered_map<sqlite3_int64, sqlite3_int64> commits;
        {
            Statement rows(db,
                "SELECT id,COALESCE(parent_commit,0) FROM _vexfs_commits "
                "WHERE workspace_id=?1 ORDER BY id");
            rows.BindInt64(1, workspace.id);
            while (rows.Row()) {
                ++report.commits;
                commits[rows.Int64(0)] = rows.Int64(1);
            }
        }
        if (workspace.head_commit != 0 && commits.count(workspace.head_commit) == 0) {
            report.Add("VEXFS_HEAD_COMMIT_MISSING", "workspace:" + workspace_name,
                       "commit", "workspace head_commit does not exist",
                       "restore a verified backup; do not create a replacement commit by hand");
        }
        for (const auto &entry : commits) {
            const sqlite3_int64 id = entry.first;
            const sqlite3_int64 parent = entry.second;
            if (parent != 0 && commits.count(parent) == 0) {
                report.Add("VEXFS_COMMIT_PARENT_MISSING", "commit:" + std::to_string(id),
                           "commit", "parent commit does not exist in this workspace",
                           "restore commit history from a verified backup");
            } else if (parent >= id && parent != 0) {
                report.Add("VEXFS_COMMIT_CHAIN_INVALID", "commit:" + std::to_string(id),
                           "commit", "parent commit is not older than its child",
                           "restore commit history from a verified backup");
            }
        }
        std::unordered_set<sqlite3_int64> commit_chain;
        sqlite3_int64 chain_commit = workspace.head_commit;
        while (chain_commit != 0 && commits.count(chain_commit) != 0 &&
               commit_chain.insert(chain_commit).second) {
            chain_commit = commits.at(chain_commit);
        }
        if (chain_commit != 0 && commit_chain.count(chain_commit) != 0) {
            report.Add("VEXFS_COMMIT_CYCLE", "commit:" + std::to_string(chain_commit),
                       "commit", "workspace commit chain contains a cycle",
                       "restore commit history from a verified backup");
        }
        for (const auto &entry : commits) {
            if (commit_chain.count(entry.first) == 0) {
                report.Add("VEXFS_COMMIT_UNREACHABLE", "commit:" +
                           std::to_string(entry.first), "commit",
                           "commit is not reachable from workspace head",
                           "restore commit history from a verified backup");
            }
        }

        struct InodeCheck {
            std::string kind;
            sqlite3_int64 size = 0;
            sqlite3_int64 version = 0;
            bool deleted = false;
        };
        std::unordered_map<sqlite3_int64, InodeCheck> inodes;
        {
            Statement rows(db,
                "SELECT id,kind,size,current_version,deleted_at IS NOT NULL "
                "FROM _vexfs_inodes WHERE workspace_id=?1");
            rows.BindInt64(1, workspace.id);
            while (rows.Row()) {
                ++report.inodes;
                inodes[rows.Int64(0)] = {rows.Text(1), rows.Int64(2), rows.Int64(3),
                                         rows.Int(4) != 0};
            }
        }
        {
            sqlite3_int64 actual_files = 0;
            sqlite3_int64 actual_bytes = 0;
            for (const auto &entry : inodes) {
                if (!entry.second.deleted && entry.second.kind != "directory") {
                    ++actual_files;
                    actual_bytes += entry.second.size;
                }
            }
            Statement stored(db,
                "SELECT live_files,live_bytes FROM _vexfs_workspaces WHERE id=?1");
            stored.BindInt64(1, workspace.id);
            if (!stored.Row() || stored.Int64(0) != actual_files ||
                stored.Int64(1) != actual_bytes) {
                report.Add("VEXFS_QUOTA_USAGE_MISMATCH", "workspace:" + workspace_name,
                           "quota", "stored live usage counters do not match active inodes",
                           "restore a verified backup or rebuild quota counters offline");
            }
        }
        const auto root = inodes.find(workspace.root_inode);
        if (root == inodes.end() || root->second.deleted || root->second.kind != "directory") {
            report.Add("VEXFS_ROOT_INVALID", "inode:" + std::to_string(workspace.root_inode),
                       "inode", "workspace root is missing, deleted, or not a directory",
                       "restore the workspace from a verified snapshot or backup");
        }

        std::unordered_map<sqlite3_int64, std::vector<sqlite3_int64>> children;
        std::unordered_map<sqlite3_int64, sqlite3_int64> directory_links;
        {
            Statement rows(db,
                "SELECT parent_inode,name,inode_id FROM _vexfs_dentries WHERE workspace_id=?1");
            rows.BindInt64(1, workspace.id);
            while (rows.Row()) {
                ++report.dentries;
                const sqlite3_int64 parent = rows.Int64(0);
                const std::string name = rows.Text(1);
                const sqlite3_int64 child = rows.Int64(2);
                const auto parent_inode = inodes.find(parent);
                const auto child_inode = inodes.find(child);
                const std::string object = "dentry:" + std::to_string(parent) + "/" + name;
                const bool valid_parent = parent_inode != inodes.end() &&
                    !parent_inode->second.deleted && parent_inode->second.kind == "directory";
                if (name.empty() || name == "." || name == ".." || name.size() > 255 ||
                    name.find('/') != std::string::npos || name.find('\0') != std::string::npos) {
                    report.Add("VEXFS_DENTRY_NAME_INVALID", object, "dentry",
                               "directory entry name is invalid",
                               "restore the directory entry from a verified snapshot");
                }
                if (!valid_parent) {
                    report.Add("VEXFS_DENTRY_PARENT_INVALID", object, "dentry",
                               "parent inode is missing, deleted, or not a directory",
                               "restore the directory tree from a verified snapshot");
                }
                if (child_inode == inodes.end() || child_inode->second.deleted) {
                    report.Add("VEXFS_DENTRY_CHILD_INVALID", object, "dentry",
                               "child inode is missing or deleted",
                               "restore the directory tree from a verified snapshot");
                } else {
                    if (child_inode->second.kind == "directory") ++directory_links[child];
                    if (valid_parent) children[parent].push_back(child);
                }
                if (child == workspace.root_inode) {
                    report.Add("VEXFS_ROOT_HAS_PARENT", object, "dentry",
                               "workspace root appears as a child",
                               "restore the directory tree from a verified snapshot");
                }
            }
        }
        for (const auto &entry : directory_links) {
            if (entry.second > 1) {
                report.Add("VEXFS_DIRECTORY_HARDLINK", "inode:" + std::to_string(entry.first),
                           "dentry", "directory has more than one parent",
                           "restore the directory tree from a verified snapshot");
            }
        }
        std::unordered_set<sqlite3_int64> reachable;
        std::vector<sqlite3_int64> pending;
        if (root != inodes.end() && !root->second.deleted) {
            reachable.insert(workspace.root_inode);
            pending.push_back(workspace.root_inode);
        }
        while (!pending.empty()) {
            const sqlite3_int64 parent = pending.back();
            pending.pop_back();
            for (const sqlite3_int64 child : children[parent]) {
                if (reachable.insert(child).second) pending.push_back(child);
            }
        }
        for (const auto &entry : inodes) {
            if (!entry.second.deleted && reachable.count(entry.first) == 0) {
                report.Add("VEXFS_INODE_UNREACHABLE", "inode:" + std::to_string(entry.first),
                           "inode", "active inode is not reachable from the workspace root",
                           "restore the directory tree from a verified snapshot");
            }
        }

        struct VersionCheck {
            sqlite3_int64 id = 0;
            sqlite3_int64 manifest = 0;
            sqlite3_int64 commit = 0;
            sqlite3_int64 size = 0;
            sqlite3_int64 source = 0;
            sqlite3_int64 manifest_size = -1;
            sqlite3_int64 chunk_size = -1;
            sqlite3_int64 chunk_count = -1;
            sqlite3_int64 actual_chunk_count = -1;
            sqlite3_int64 actual_chunk_bytes = -1;
            sqlite3_int64 minimum_chunk = -1;
            sqlite3_int64 maximum_chunk = -1;
            std::string checksum;
            std::string manifest_checksum;
        };
        std::map<std::pair<sqlite3_int64, sqlite3_int64>, VersionCheck> versions;
        {
            Statement rows(db, R"SQL(
SELECT v.id,v.inode_id,v.version_no,v.commit_id,v.size,
       COALESCE(v.source_version_no,0),v.checksum,COALESCE(v.manifest_id,0),
       COALESCE(manifest.file_size,-1),COALESCE(manifest.chunk_size,-1),
       COALESCE(manifest.chunk_count,-1),COALESCE(chunk_stats.actual_count,-1),
       COALESCE(chunk_stats.actual_bytes,-1),COALESCE(chunk_stats.minimum_chunk,-1),
       COALESCE(chunk_stats.maximum_chunk,-1),COALESCE(manifest.checksum,'')
FROM _vexfs_file_versions v
JOIN _vexfs_inodes i ON i.id=v.inode_id
LEFT JOIN _vexfs_manifests manifest ON manifest.id=v.manifest_id
LEFT JOIN (
 SELECT entry.manifest_id,count(*) actual_count,COALESCE(sum(chunk.size),0) actual_bytes,
        min(entry.chunk_no) minimum_chunk,max(entry.chunk_no) maximum_chunk
 FROM _vexfs_manifest_chunks entry
 JOIN _vexfs_chunks chunk ON chunk.id=entry.chunk_id
 GROUP BY entry.manifest_id
) chunk_stats ON chunk_stats.manifest_id=manifest.id
WHERE i.workspace_id=?1
ORDER BY v.inode_id,v.version_no
)SQL");
            rows.BindInt64(1, workspace.id);
            while (rows.Row()) {
                ++report.versions;
                VersionCheck version{rows.Int64(0), rows.Int64(7), rows.Int64(3),
                                     rows.Int64(4), rows.Int64(5), rows.Int64(8),
                                     rows.Int64(9), rows.Int64(10), rows.Int64(11),
                                     rows.Int64(12), rows.Int64(13), rows.Int64(14),
                                     rows.Text(6), rows.Text(15)};
                versions[{rows.Int64(1), rows.Int64(2)}] = std::move(version);
            }
        }
        {
            Statement orphan(db,
                "SELECT v.id,v.inode_id FROM _vexfs_file_versions v "
                "LEFT JOIN _vexfs_inodes i ON i.id=v.inode_id WHERE i.id IS NULL");
            while (orphan.Row()) {
                report.Add("VEXFS_VERSION_INODE_MISSING",
                           "version-row:" + std::to_string(orphan.Int64(0)), "content",
                           "file version references an unknown inode",
                           "restore version history from a verified backup");
            }
        }
        for (const auto &entry : versions) {
            const sqlite3_int64 inode = entry.first.first;
            const sqlite3_int64 number = entry.first.second;
            const VersionCheck &version = entry.second;
            const std::string object = "version:" + std::to_string(inode) + "/" +
                                       std::to_string(number);
            if (version.size < 0 || version.size > kMaxStagedBytes) {
                report.Add("VEXFS_VERSION_SIZE_INVALID", object, "content",
                           "file version size is outside the supported range",
                           "restore this file version from a verified snapshot or backup");
            }
            if (!IsSha256(version.checksum)) {
                report.Add("VEXFS_CHECKSUM_INVALID", object, "content",
                           "stored SHA-256 is not a lowercase 64-character digest",
                           "restore this file version from a verified snapshot or backup");
            }
            if (commits.count(version.commit) == 0) {
                report.Add("VEXFS_VERSION_COMMIT_MISSING", object, "commit",
                           "file version commit does not exist in this workspace",
                           "restore version history from a verified backup");
            }
            if (version.source == 0) {
                report.content_bytes += std::max<sqlite3_int64>(version.size, 0);
                const sqlite3_int64 expected_chunks = version.size < 0 ? -1 :
                    (version.size + kContentChunkBytes - 1) / kContentChunkBytes;
                if (version.manifest <= 0 || version.manifest_size != version.size ||
                    version.chunk_size != kContentChunkBytes ||
                    version.chunk_count != expected_chunks ||
                    (expected_chunks == 0 && version.actual_chunk_count != -1) ||
                    (expected_chunks > 0 &&
                     (version.actual_chunk_count != expected_chunks ||
                      version.actual_chunk_bytes != version.size ||
                      version.minimum_chunk != 0 ||
                      version.maximum_chunk != expected_chunks - 1)) ||
                    version.manifest_checksum != version.checksum) {
                    report.Add("VEXFS_MANIFEST_MISMATCH", object, "manifest",
                               "manifest metadata does not match its canonical version",
                               "restore this file version from a verified snapshot or backup");
                } else if (deep && IsSha256(version.checksum)) {
                    try {
                        const std::string actual = HashManifest(
                            db, version.manifest, version.size, version.chunk_count);
                        if (actual != version.checksum) {
                            report.Add("VEXFS_CHECKSUM_MISMATCH", object, "content",
                                       "manifest chunks do not match the file SHA-256",
                                       "restore this file version from a verified snapshot or backup");
                        }
                    } catch (const SqlError &error) {
                        report.Add("VEXFS_CHUNK_INVALID", object, "chunk", error.what(),
                                   "restore this file version from a verified snapshot or backup");
                    }
                }
            } else {
                const auto source = versions.find({inode, version.source});
                if (version.manifest != 0) {
                    report.Add("VEXFS_ALIAS_HAS_MANIFEST", object, "manifest",
                               "version alias unexpectedly stores its own manifest",
                               "restore version history from a verified backup");
                }
                if (source == versions.end() || source->second.source != 0) {
                    report.Add("VEXFS_VERSION_SOURCE_INVALID", object, "content",
                               "version alias does not point directly to a canonical version",
                               "restore version history from a verified backup");
                } else if (source->second.size != version.size ||
                           source->second.checksum != version.checksum) {
                    report.Add("VEXFS_VERSION_SOURCE_MISMATCH", object, "content",
                               "version alias metadata differs from its source",
                               "restore version history from a verified backup");
                }
            }
        }
        {
            Statement rows(db,
                "SELECT id FROM _vexfs_manifests WHERE workspace_id=?1");
            rows.BindInt64(1, workspace.id);
            while (rows.Row()) ++report.manifests;
        }
        {
            Statement rows(db, R"SQL(
SELECT id FROM _vexfs_chunks WHERE workspace_id=?1
)SQL");
            rows.BindInt64(1, workspace.id);
            while (rows.Row()) ++report.chunks;
        }
        {
            Statement rows(db, R"SQL(
SELECT manifest.id FROM _vexfs_manifests manifest
WHERE manifest.workspace_id=?1 AND NOT EXISTS(
  SELECT 1 FROM _vexfs_file_versions version
  WHERE version.manifest_id=manifest.id AND version.source_version_no IS NULL)
)SQL");
            rows.BindInt64(1, workspace.id);
            while (rows.Row()) {
                report.Add("VEXFS_MANIFEST_UNREFERENCED",
                           "manifest:" + std::to_string(rows.Int64(0)), "manifest",
                           "manifest is not referenced by a canonical file version",
                           "restore from backup or remove the unreachable manifest offline");
            }
        }
        {
            Statement rows(db, R"SQL(
SELECT version.id,manifest.id FROM _vexfs_file_versions version
JOIN _vexfs_inodes inode ON inode.id=version.inode_id
JOIN _vexfs_manifests manifest ON manifest.id=version.manifest_id
WHERE inode.workspace_id=?1 AND manifest.workspace_id<>?1
)SQL");
            rows.BindInt64(1, workspace.id);
            while (rows.Row()) {
                report.Add("VEXFS_MANIFEST_WORKSPACE_MISMATCH",
                           "version-row:" + std::to_string(rows.Int64(0)), "manifest",
                           "file version references another workspace manifest",
                           "restore this file version from a verified backup");
            }
        }
        {
            Statement rows(db, R"SQL(
SELECT chunk.id FROM _vexfs_chunks chunk
WHERE NOT EXISTS(SELECT 1 FROM _vexfs_manifest_chunks entry
                 WHERE entry.chunk_id=chunk.id)
)SQL");
            while (rows.Row()) {
                report.Add("VEXFS_CHUNK_UNREFERENCED",
                           "chunk:" + std::to_string(rows.Int64(0)), "chunk",
                           "chunk object is not referenced by any manifest",
                           "restore content tables from a verified backup");
            }
        }
        {
            Statement rows(db, R"SQL(
SELECT entry.rowid FROM _vexfs_manifest_chunks entry
LEFT JOIN _vexfs_manifests manifest ON manifest.id=entry.manifest_id
LEFT JOIN _vexfs_chunks chunk ON chunk.id=entry.chunk_id
WHERE manifest.id IS NULL OR chunk.id IS NULL
)SQL");
            while (rows.Row()) {
                report.Add("VEXFS_MANIFEST_CHUNK_REFERENCE_INVALID",
                           "manifest-chunk:" + std::to_string(rows.Int64(0)), "chunk",
                           "manifest chunk entry references a missing object",
                           "restore content tables from a verified backup");
            }
        }
        for (const auto &entry : inodes) {
            if (entry.second.deleted || entry.second.kind == "directory") continue;
            const auto version = versions.find({entry.first, entry.second.version});
            bool pending_first_publish = false;
            if (entry.second.version == 0 && entry.second.size == 0) {
                Statement handle(db,
                    "SELECT 1 FROM _vexfs_handles WHERE workspace_id=?1 AND inode_id=?2 "
                    "AND state IN ('open','retained') LIMIT 1");
                handle.BindInt64(1, workspace.id);
                handle.BindInt64(2, entry.first);
                pending_first_publish = handle.Row();
            }
            if (!pending_first_publish &&
                (entry.second.version <= 0 || version == versions.end())) {
                report.Add("VEXFS_CURRENT_VERSION_MISSING", "inode:" + std::to_string(entry.first),
                           "inode", "active file or symlink has no current version",
                           "restore this inode from a verified snapshot or backup");
            } else if (!pending_first_publish && version->second.size != entry.second.size) {
                report.Add("VEXFS_CURRENT_SIZE_MISMATCH", "inode:" + std::to_string(entry.first),
                           "inode", "inode size differs from its current version",
                           "restore this inode from a verified snapshot or backup");
            }
        }

        {
            Statement rows(db,
                "SELECT name,commit_id FROM _vexfs_snapshots WHERE workspace_id=?1");
            rows.BindInt64(1, workspace.id);
            while (rows.Row()) {
                ++report.snapshots;
                if (commits.count(rows.Int64(1)) == 0) {
                    report.Add("VEXFS_SNAPSHOT_COMMIT_MISSING", "snapshot:" + rows.Text(0),
                               "snapshot", "snapshot commit does not exist in this workspace",
                               "drop the broken snapshot name or restore history from backup");
                }
            }
        }
        {
            Statement rows(db,
                "SELECT inode_id,commit_id,kind,size,current_version,deleted_at IS NOT NULL "
                "FROM _vexfs_inode_states WHERE workspace_id=?1");
            rows.BindInt64(1, workspace.id);
            while (rows.Row()) {
                ++report.history_rows;
                const sqlite3_int64 inode = rows.Int64(0);
                const sqlite3_int64 commit = rows.Int64(1);
                const std::string object = "inode-state:" + std::to_string(inode) + "@" +
                                           std::to_string(commit);
                if (commits.count(commit) == 0) {
                    report.Add("VEXFS_HISTORY_COMMIT_MISSING", object,
                               "history", "history row references a missing commit",
                               "restore workspace history from a verified backup");
                }
                if (inodes.count(inode) == 0) {
                    report.Add("VEXFS_HISTORY_INODE_MISSING", object, "history",
                               "inode state references an unknown inode",
                               "restore workspace history from a verified backup");
                }
                if (rows.Int(5) == 0 && rows.Text(2) != "directory") {
                    const auto version = versions.find({inode, rows.Int64(4)});
                    if (version == versions.end()) {
                        report.Add("VEXFS_HISTORY_VERSION_MISSING", object, "history",
                                   "inode state references a missing file version",
                                   "restore workspace history from a verified backup");
                    } else if (version->second.size != rows.Int64(3)) {
                        report.Add("VEXFS_HISTORY_SIZE_MISMATCH", object, "history",
                                   "inode state size differs from its file version",
                                   "restore workspace history from a verified backup");
                    }
                }
            }
        }
        {
            Statement rows(db,
                "SELECT parent_inode,name,commit_id,inode_id FROM _vexfs_dentry_states "
                "WHERE workspace_id=?1");
            rows.BindInt64(1, workspace.id);
            while (rows.Row()) {
                ++report.history_rows;
                const std::string object = "dentry-state:" + std::to_string(rows.Int64(0)) +
                                           "/" + rows.Text(1) + "@" +
                                           std::to_string(rows.Int64(2));
                if (commits.count(rows.Int64(2)) == 0) {
                    report.Add("VEXFS_HISTORY_COMMIT_MISSING", object, "history",
                               "history row references a missing commit",
                               "restore workspace history from a verified backup");
                }
                if (inodes.count(rows.Int64(0)) == 0 || inodes.count(rows.Int64(3)) == 0) {
                    report.Add("VEXFS_HISTORY_DENTRY_INVALID", object, "history",
                               "historical directory entry references an unknown inode",
                               "restore workspace history from a verified backup");
                }
            }
        }
        for (const char *table : {"_vexfs_xattr_states", "_vexfs_acl_states"}) {
            const std::string sql = "SELECT inode_id,commit_id FROM " + std::string(table) +
                                    " WHERE workspace_id=?1";
            Statement rows(db, sql.c_str());
            rows.BindInt64(1, workspace.id);
            while (rows.Row()) {
                ++report.history_rows;
                const std::string object = std::string("table:") + table + "@" +
                                           std::to_string(rows.Int64(1));
                if (commits.count(rows.Int64(1)) == 0) {
                    report.Add("VEXFS_HISTORY_COMMIT_MISSING", object, "history",
                               "history row references a missing commit",
                               "restore workspace history from a verified backup");
                }
                if (inodes.count(rows.Int64(0)) == 0) {
                    report.Add("VEXFS_HISTORY_INODE_MISSING", object, "history",
                               "history row references an unknown inode",
                               "restore workspace history from a verified backup");
                }
            }
        }

        {
            Statement rows(db, R"SQL(
SELECT h.id,h.inode_id,h.state,h.dirty_generation,h.published_generation,
       s.generation,s.base_manifest_id,s.base_size,s.base_visible_size,
       s.logical_size,s.capacity,dirty.chunk_count,dirty.chunk_bytes,dirty.max_chunk
FROM _vexfs_handles h
LEFT JOIN _vexfs_staging s ON s.handle_id=h.id
LEFT JOIN (
  SELECT handle_id,count(*) chunk_count,COALESCE(sum(length(content)),0) chunk_bytes,
         max(chunk_no) max_chunk
  FROM _vexfs_staging_chunks GROUP BY handle_id
) dirty ON dirty.handle_id=h.id
WHERE h.workspace_id=?1
)SQL");
            rows.BindInt64(1, workspace.id);
            while (rows.Row()) {
                ++report.handles;
                const std::string id = rows.Text(0);
                const std::string object = "handle:" + id;
                const bool active = rows.Text(2) == "open" || rows.Text(2) == "retained";
                if (inodes.count(rows.Int64(1)) == 0) {
                    report.Add("VEXFS_HANDLE_INODE_MISSING", object, "staging",
                               "handle inode does not exist in this workspace",
                               "close the handle and restore the inode from backup if needed");
                }
                if (rows.Int64(3) < rows.Int64(4)) {
                    report.Add("VEXFS_HANDLE_GENERATION_INVALID", object, "staging",
                               "published generation is newer than dirty generation",
                               "discard the handle and reopen the file");
                }
                if (active) {
                    ++report.staging_objects;
                    const sqlite3_int64 dirty_chunks =
                        rows.Type(11) == SQLITE_NULL ? 0 : rows.Int64(11);
                    const sqlite3_int64 dirty_bytes =
                        rows.Type(12) == SQLITE_NULL ? 0 : rows.Int64(12);
                    const sqlite3_int64 expected_chunks = rows.Type(9) == SQLITE_NULL ? 0 :
                        (rows.Int64(9) + kContentChunkBytes - 1) / kContentChunkBytes;
                    if (rows.Type(5) == SQLITE_NULL ||
                        (rows.Int64(10) > 0 && rows.Type(11) == SQLITE_NULL)) {
                        report.Add("VEXFS_STAGING_MISSING", object, "staging",
                                   "open or retained handle has no complete staging object",
                                   "discard the handle; recover unpublished work from backup if available");
                    } else if (rows.Int64(5) != rows.Int64(3) || rows.Int64(7) < 0 ||
                               rows.Int64(8) < 0 || rows.Int64(8) > rows.Int64(7) ||
                               rows.Int64(8) > rows.Int64(9) || rows.Int64(9) < 0 ||
                               rows.Int64(9) > kMaxStagedBytes || rows.Int64(10) < 0 ||
                               rows.Int64(10) > kMaxStagedBytes ||
                               (rows.Int64(7) > 0 &&
                                (rows.Type(6) == SQLITE_NULL || rows.Int64(6) <= 0)) ||
                               dirty_bytes != rows.Int64(10) ||
                               (dirty_chunks > 0 && rows.Int64(13) >= expected_chunks)) {
                        report.Add("VEXFS_STAGING_INVALID", object, "staging",
                                   "staging generation, size, or capacity is inconsistent",
                                   "discard the handle; recover unpublished work from backup if available");
                    }
                } else if (rows.Type(5) != SQLITE_NULL || rows.Type(11) != SQLITE_NULL) {
                    report.Add("VEXFS_CLOSED_HANDLE_STAGING", object, "staging",
                               "closed handle still owns staging data",
                               "run a future repair or garbage-collection command after backing up");
                }
            }
        }
        {
            Statement orphan(db,
                "SELECT s.handle_id FROM _vexfs_staging s LEFT JOIN _vexfs_handles h "
                "ON h.id=s.handle_id WHERE h.id IS NULL");
            while (orphan.Row()) {
                report.Add("VEXFS_ORPHAN_STAGING", "handle:" + orphan.Text(0), "staging",
                           "staging metadata has no owning handle",
                           "run a future repair or garbage-collection command after backing up");
            }
            Statement orphan_data(db,
                "SELECT d.handle_id FROM _vexfs_staging_chunks d LEFT JOIN _vexfs_staging s "
                "ON s.handle_id=d.handle_id WHERE s.handle_id IS NULL");
            while (orphan_data.Row()) {
                report.Add("VEXFS_ORPHAN_STAGING_DATA", "handle:" + orphan_data.Text(0),
                           "staging", "staging BLOB has no metadata row",
                           "run a future repair or garbage-collection command after backing up");
            }
        }

        const sqlite3_int64 elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        const std::string json = CheckReportJson(report, workspace_name, deep, elapsed_ms);
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
            "i.owner_principal, i.uid, i.gid, i.created_at, i.accessed_at, "
            "i.updated_at, i.changed_at FROM _vexfs_dentries d "
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
                std::to_string(statement.Int(5)) + ",\"owner_principal\":\"" +
                JsonEscape(statement.Text(6)) + "\",\"uid\":" +
                std::to_string(statement.Int64(7)) + ",\"gid\":" +
                std::to_string(statement.Int64(8)) + ",\"created_at\":" +
                std::to_string(statement.Int64(9)) + ",\"accessed_at\":" +
                std::to_string(statement.Int64(10)) + ",\"updated_at\":" +
                std::to_string(statement.Int64(11)) + ",\"changed_at\":" +
                std::to_string(statement.Int64(12)) + ",\"link_count\":" +
                std::to_string(LinkCount(db, workspace, statement.Int64(1))) + "}";
        }
        json += ']';
        sqlite3_result_text(context, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    });
}

void RequireWorkspaceInode(sqlite3 *db, const Workspace &workspace, sqlite3_int64 inode) {
    Statement statement(db,
        "SELECT 1 FROM _vexfs_inodes WHERE id=?1 AND workspace_id=?2 LIMIT 1");
    statement.BindInt64(1, inode);
    statement.BindInt64(2, workspace.id);
    if (!statement.Row()) throw SqlError("inode not found", SQLITE_NOTFOUND);
}

void ReadlinkFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const sqlite3_int64 inode = RequiredPositiveInteger(values[1], "inode");
        RequireWorkspaceInode(db, workspace, inode);
        const Node node = NodeById(db, inode, true);
        if (node.kind != "symlink") {
            throw SqlError("inode is not a symbolic link", SQLITE_MISMATCH);
        }
        if (node.version <= 0) throw SqlError("symlink target is missing", SQLITE_CORRUPT);
        ResultBlob(context, ReadVersionContent(db, node.id, node.version));
    });
}

void SetModeFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const sqlite3_int64 inode = RequiredPositiveInteger(values[1], "inode");
        const int mode = RequiredMode(values[2]);
        Savepoint savepoint(db, "vexfs_set_mode");
        AcquireWriteLock(db);
        RequireWorkspaceInode(db, workspace, inode);
        const Node node = NodeById(db, inode, true);
        if (node.kind == "symlink") {
            throw SqlError("symbolic link mode cannot be changed", SQLITE_MISMATCH);
        }
        if (node.mode != mode) {
            Statement update(db,
                "UPDATE _vexfs_inodes SET mode=?1, "
                "changed_at=CAST(unixepoch('subsec')*1000 AS INTEGER) "
                "WHERE id=?2 AND workspace_id=?3");
            update.BindInt(1, mode);
            update.BindInt64(2, inode);
            update.BindInt64(3, workspace.id);
            update.Done();
            if (sqlite3_changes64(db) != 1) {
                throw SqlError("inode not found", SQLITE_NOTFOUND);
            }
            CommitMetadataChange(db, workspace, inode, "chmod");
        }
        savepoint.Release();
        sqlite3_result_int(context, mode);
    });
}

void SetTimesFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const sqlite3_int64 inode = RequiredPositiveInteger(values[1], "inode");
        const sqlite3_int64 accessed_at = RequiredNonnegativeInteger(values[2], "access time");
        const sqlite3_int64 updated_at = RequiredNonnegativeInteger(values[3], "modify time");
        const int mask = sqlite3_value_int(values[4]);
        if ((mask & ~3) != 0 || mask == 0) {
            throw SqlError("time mask must select access time, modify time, or both",
                           SQLITE_MISMATCH);
        }
        Savepoint savepoint(db, "vexfs_set_times");
        AcquireWriteLock(db);
        RequireWorkspaceInode(db, workspace, inode);
        Statement update(db,
            "UPDATE _vexfs_inodes SET "
            "accessed_at=CASE WHEN (?1&1)<>0 THEN ?2 ELSE accessed_at END,"
            "updated_at=CASE WHEN (?1&2)<>0 THEN ?3 ELSE updated_at END,"
            "changed_at=CAST(unixepoch('subsec')*1000 AS INTEGER) "
            "WHERE id=?4 AND workspace_id=?5 AND deleted_at IS NULL");
        update.BindInt(1, mask);
        update.BindInt64(2, accessed_at);
        update.BindInt64(3, updated_at);
        update.BindInt64(4, inode);
        update.BindInt64(5, workspace.id);
        update.Done();
        if (sqlite3_changes64(db) != 1) throw SqlError("inode not found", SQLITE_NOTFOUND);
        CommitMetadataChange(db, workspace, inode, "set times");
        savepoint.Release();
        sqlite3_result_int(context, mask);
    });
}

struct AclEntry {
    std::string principal;
    std::string effect;
    std::string permissions;
    sqlite3_int64 inherit_flags = 0;

    bool operator==(const AclEntry &other) const {
        return principal == other.principal && effect == other.effect &&
            permissions == other.permissions && inherit_flags == other.inherit_flags;
    }
};

std::vector<AclEntry> ReadAclEntries(sqlite3 *db, const Workspace &workspace,
                                     sqlite3_int64 inode) {
    Statement statement(db,
        "SELECT principal_id,effect,permissions,inherit_flags FROM _vexfs_acl_entries "
        "WHERE workspace_id=?1 AND inode_id=?2 ORDER BY principal_id COLLATE BINARY,effect");
    statement.BindInt64(1, workspace.id);
    statement.BindInt64(2, inode);
    std::vector<AclEntry> entries;
    while (statement.Row()) {
        entries.push_back({statement.Text(0), statement.Text(1), statement.Text(2),
                           statement.Int64(3)});
    }
    return entries;
}

std::string AclJson(const std::vector<AclEntry> &entries) {
    std::string json = "[";
    bool first = true;
    for (const AclEntry &entry : entries) {
        if (!first) json += ',';
        first = false;
        json += "{\"principal\":\"" + JsonEscape(entry.principal) +
            "\",\"effect\":\"" + JsonEscape(entry.effect) +
            "\",\"permissions\":\"" + JsonEscape(entry.permissions) +
            "\",\"inherit\":" + std::to_string(entry.inherit_flags) + "}";
    }
    json += ']';
    return json;
}

std::vector<AclEntry> ParseAclJson(sqlite3 *db, sqlite3_value *value) {
    const std::string json = RequiredText(value, "acl");
    Statement root(db, "SELECT json_type(?1)");
    root.BindText(1, json);
    if (!root.Row() || root.Type(0) == SQLITE_NULL || root.Text(0) != "array") {
        throw SqlError("acl must be a JSON array", SQLITE_MISMATCH);
    }
    Statement statement(db, R"SQL(
SELECT COALESCE(json_extract(value,'$.principal'),json_extract(value,'$.principal_id')),
       COALESCE(json_extract(value,'$.effect'),'allow'),
       json_extract(value,'$.permissions'),
       COALESCE(json_extract(value,'$.inherit'),json_extract(value,'$.inherit_flags'),0)
FROM json_each(?1)
)SQL");
    statement.BindText(1, json);
    std::vector<AclEntry> entries;
    while (statement.Row()) {
        const std::string principal = statement.Text(0);
        const std::string effect = statement.Text(1);
        const std::string permissions = statement.Text(2);
        const sqlite3_int64 inherit = statement.Int64(3);
        if (principal.empty() || principal.size() > 255)
            throw SqlError("acl principal must be 1..255 bytes", SQLITE_RANGE);
        if (effect != "allow" && effect != "deny")
            throw SqlError("acl effect must be allow or deny", SQLITE_MISMATCH);
        if (permissions.empty() || permissions.size() > 1024)
            throw SqlError("acl permissions must be 1..1024 bytes", SQLITE_RANGE);
        if (inherit < 0 || inherit > 255)
            throw SqlError("acl inherit flags must be 0..255", SQLITE_RANGE);
        entries.push_back({principal, effect, permissions, inherit});
        if (entries.size() > 1024) throw SqlError("acl has too many entries", SQLITE_TOOBIG);
    }
    return entries;
}

void AclGetFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const sqlite3_int64 inode = RequiredPositiveInteger(values[1], "inode");
        RequireWorkspaceInode(db, workspace, inode);
        const std::string json = AclJson(ReadAclEntries(db, workspace, inode));
        sqlite3_result_text(context, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    });
}

void AclSetFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const sqlite3_int64 inode = RequiredPositiveInteger(values[1], "inode");
        RequireWorkspaceInode(db, workspace, inode);
        const std::vector<AclEntry> entries = ParseAclJson(db, values[2]);
        Savepoint savepoint(db, "vexfs_acl_set");
        AcquireWriteLock(db);
        Statement clear(db, "DELETE FROM _vexfs_acl_entries WHERE workspace_id=?1 AND inode_id=?2");
        clear.BindInt64(1, workspace.id);
        clear.BindInt64(2, inode);
        clear.Done();
        for (const AclEntry &entry : entries) {
            Statement insert(db,
                "INSERT INTO _vexfs_acl_entries(workspace_id,inode_id,principal_id,effect,permissions,inherit_flags) "
                "VALUES(?1,?2,?3,?4,?5,?6)");
            insert.BindInt64(1, workspace.id);
            insert.BindInt64(2, inode);
            insert.BindText(3, entry.principal);
            insert.BindText(4, entry.effect);
            insert.BindText(5, entry.permissions);
            insert.BindInt64(6, entry.inherit_flags);
            insert.Done();
        }
        Statement touch(db,
            "UPDATE _vexfs_inodes SET changed_at=CAST(unixepoch('subsec')*1000 AS INTEGER) "
            "WHERE id=?1 AND workspace_id=?2");
        touch.BindInt64(1, inode);
        touch.BindInt64(2, workspace.id);
        touch.Done();
        CommitMetadataChange(db, workspace, inode, "set acl");
        savepoint.Release();
        sqlite3_result_int(context, static_cast<int>(entries.size()));
    });
}

void AclDeleteFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const sqlite3_int64 inode = RequiredPositiveInteger(values[1], "inode");
        RequireWorkspaceInode(db, workspace, inode);
        Savepoint savepoint(db, "vexfs_acl_delete");
        AcquireWriteLock(db);
        Statement remove(db, "DELETE FROM _vexfs_acl_entries WHERE workspace_id=?1 AND inode_id=?2");
        remove.BindInt64(1, workspace.id);
        remove.BindInt64(2, inode);
        remove.Done();
        const sqlite3_int64 removed = sqlite3_changes64(db);
        if (removed > 0) CommitMetadataChange(db, workspace, inode, "delete acl");
        savepoint.Release();
        sqlite3_result_int64(context, removed);
    });
}

void AclGrantFunction(sqlite3_context *context, int argument_count, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const sqlite3_int64 inode = RequiredPositiveInteger(values[1], "inode");
        const std::string principal = RequiredText(values[2], "principal");
        const std::string permissions = RequiredText(values[3], "permissions");
        if (principal.empty() || principal.size() > 255 || permissions.empty() || permissions.size() > 1024)
            throw SqlError("invalid ACL principal or permissions", SQLITE_RANGE);
        RequireWorkspaceInode(db, workspace, inode);
        const std::string effect = (argument_count < 5 || sqlite3_value_type(values[4]) == SQLITE_NULL)
            ? "allow" : RequiredText(values[4], "effect");
        if (effect != "allow" && effect != "deny")
            throw SqlError("acl effect must be allow or deny", SQLITE_MISMATCH);
        Savepoint savepoint(db, "vexfs_acl_grant");
        AcquireWriteLock(db);
        Statement upsert(db,
            "INSERT INTO _vexfs_acl_entries(workspace_id,inode_id,principal_id,effect,permissions) "
            "VALUES(?1,?2,?3,?4,?5) ON CONFLICT(workspace_id,inode_id,principal_id,effect) "
            "DO UPDATE SET permissions=excluded.permissions,updated_at=CAST(strftime('%s','now') AS INTEGER)*1000");
        upsert.BindInt64(1, workspace.id);
        upsert.BindInt64(2, inode);
        upsert.BindText(3, principal);
        upsert.BindText(4, effect);
        upsert.BindText(5, permissions);
        upsert.Done();
        CommitMetadataChange(db, workspace, inode, "grant acl");
        savepoint.Release();
        sqlite3_result_int(context, 1);
    });
}

void AclRevokeFunction(sqlite3_context *context, int argument_count, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const sqlite3_int64 inode = RequiredPositiveInteger(values[1], "inode");
        const std::string principal = RequiredText(values[2], "principal");
        RequireWorkspaceInode(db, workspace, inode);
        const std::string effect = (argument_count < 4 || sqlite3_value_type(values[3]) == SQLITE_NULL)
            ? std::string() : RequiredText(values[3], "effect");
        if (!effect.empty() && effect != "allow" && effect != "deny")
            throw SqlError("acl effect must be allow or deny", SQLITE_MISMATCH);
        Savepoint savepoint(db, "vexfs_acl_revoke");
        AcquireWriteLock(db);
        Statement remove(db,
            "DELETE FROM _vexfs_acl_entries WHERE workspace_id=?1 AND inode_id=?2 "
            "AND principal_id=?3 AND (?4='' OR effect=?4)");
        remove.BindInt64(1, workspace.id);
        remove.BindInt64(2, inode);
        remove.BindText(3, principal);
        remove.BindText(4, effect);
        remove.Done();
        const sqlite3_int64 removed = sqlite3_changes64(db);
        if (removed > 0) CommitMetadataChange(db, workspace, inode, "revoke acl");
        savepoint.Release();
        sqlite3_result_int64(context, removed);
    });
}

void ChownFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const sqlite3_int64 inode = RequiredPositiveInteger(values[1], "inode");
        const sqlite3_int64 requested_uid = RequiredOwnerId(values[2], "uid");
        const sqlite3_int64 requested_gid = RequiredOwnerId(values[3], "gid");
        RequireWorkspaceInode(db, workspace, inode);
        const Node current = NodeById(db, inode, true);
        const sqlite3_int64 uid = requested_uid < 0 ? current.uid : requested_uid;
        const sqlite3_int64 gid = requested_gid < 0 ? current.gid : requested_gid;
        Savepoint savepoint(db, "vexfs_chown");
        AcquireWriteLock(db);
        Statement update(db,
            "UPDATE _vexfs_inodes SET uid=?1,gid=?2,changed_at=CAST(unixepoch('subsec')*1000 AS INTEGER) "
            "WHERE workspace_id=?3 AND id=?4");
        update.BindInt64(1, uid);
        update.BindInt64(2, gid);
        update.BindInt64(3, workspace.id);
        update.BindInt64(4, inode);
        update.Done();
        if (sqlite3_changes64(db) != 1) throw SqlError("inode not found", SQLITE_NOTFOUND);
        CommitMetadataChange(db, workspace, inode, "chown");
        savepoint.Release();
        sqlite3_result_int64(context, uid);
    });
}

void OwnerSetFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const std::string path = RequiredText(values[1], "path");
        const std::string owner = RequiredText(values[2], "owner principal");
        if (owner.empty() || owner.size() > 255) throw SqlError("owner principal must be 1..255 bytes", SQLITE_RANGE);
        const Node node = Resolve(db, workspace, PathParts(path));
        Savepoint savepoint(db, "vexfs_owner_set");
        AcquireWriteLock(db);
        Statement update(db,
            "UPDATE _vexfs_inodes SET owner_principal=?1,changed_at=CAST(unixepoch('subsec')*1000 AS INTEGER) "
            "WHERE workspace_id=?2 AND id=?3");
        update.BindText(1, owner);
        update.BindInt64(2, workspace.id);
        update.BindInt64(3, node.id);
        update.Done();
        if (sqlite3_changes64(db) != 1) throw SqlError("inode not found", SQLITE_NOTFOUND);
        CommitMetadataChange(db, workspace, node.id, "set owner");
        savepoint.Release();
        sqlite3_result_text(context, owner.data(), static_cast<int>(owner.size()), SQLITE_TRANSIENT);
    });
}

void GetXattrFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const sqlite3_int64 inode = RequiredPositiveInteger(values[1], "inode");
        const std::string name = RequiredText(values[2], "xattr name");
        if (name.empty() || name.size() > 255)
            throw SqlError("xattr name must be 1..255 bytes", SQLITE_RANGE);
        RequireWorkspaceInode(db, workspace, inode);
        Statement statement(db,
            "SELECT value FROM _vexfs_xattrs WHERE inode_id=?1 AND name=?2");
        statement.BindInt64(1, inode);
        statement.BindText(2, name);
        if (!statement.Row()) throw SqlError("xattr not found", SQLITE_NOTFOUND);
        const std::vector<unsigned char> value = statement.Blob(0);
        sqlite3_result_blob64(context, value.empty() ? static_cast<const void *>("") : value.data(),
                              value.size(), SQLITE_TRANSIENT);
    });
}

void ListXattrsFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const sqlite3_int64 inode = RequiredPositiveInteger(values[1], "inode");
        RequireWorkspaceInode(db, workspace, inode);
        Statement statement(db,
            "SELECT name FROM _vexfs_xattrs WHERE inode_id=?1 ORDER BY name COLLATE BINARY");
        statement.BindInt64(1, inode);
        std::string json = "[";
        bool first = true;
        while (statement.Row()) {
            if (!first) json += ',';
            first = false;
            json += "\"" + JsonEscape(statement.Text(0)) + "\"";
        }
        json += ']';
        sqlite3_result_text(context, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    });
}

void SetXattrFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const sqlite3_int64 inode = RequiredPositiveInteger(values[1], "inode");
        const std::string name = RequiredText(values[2], "xattr name");
        const int policy = sqlite3_value_int(values[4]);
        if (name.empty() || name.size() > 255)
            throw SqlError("xattr name must be 1..255 bytes", SQLITE_RANGE);
        if (policy < 0 || policy > 3)
            throw SqlError("xattr policy must be 0..3", SQLITE_RANGE);
        RequireWorkspaceInode(db, workspace, inode);

        Statement exists(db,
            "SELECT 1 FROM _vexfs_xattrs WHERE inode_id=?1 AND name=?2");
        exists.BindInt64(1, inode);
        exists.BindText(2, name);
        const bool found = exists.Row();
        if (policy == 1 && found) throw SqlError("xattr already exists", SQLITE_CONSTRAINT);
        if ((policy == 2 || policy == 3) && !found)
            throw SqlError("xattr not found", SQLITE_NOTFOUND);

        Savepoint savepoint(db, "vexfs_set_xattr");
        if (policy == 3) {
            Statement remove(db,
                "DELETE FROM _vexfs_xattrs WHERE inode_id=?1 AND name=?2");
            remove.BindInt64(1, inode);
            remove.BindText(2, name);
            remove.Done();
        } else {
            const std::vector<unsigned char> value = RequiredBlob(values[3], "xattr value");
            if (static_cast<sqlite3_int64>(value.size()) > kMaxXattrBytes)
                throw SqlError("xattr is larger than 64 KiB", SQLITE_TOOBIG);
            Statement store(db,
                "INSERT INTO _vexfs_xattrs(inode_id,name,value) VALUES(?1,?2,?3) "
                "ON CONFLICT(inode_id,name) DO UPDATE SET value=excluded.value, "
                "updated_at=CAST(strftime('%s','now') AS INTEGER)*1000");
            store.BindInt64(1, inode);
            store.BindText(2, name);
            store.BindBlob(3, value);
            store.Done();
        }
        Statement touch(db,
            "UPDATE _vexfs_inodes SET changed_at=CAST(unixepoch('subsec')*1000 AS INTEGER) "
            "WHERE id=?1 AND workspace_id=?2");
        touch.BindInt64(1, inode);
        touch.BindInt64(2, workspace.id);
        touch.Done();
        CommitMetadataChange(db, workspace, inode,
                             policy == 3 ? "remove xattr" : "set xattr");
        savepoint.Release();
        sqlite3_result_int(context, 1);
    });
}

struct SnapshotInfo {
    sqlite3_int64 id = 0;
    std::string name;
    sqlite3_int64 commit = 0;
    sqlite3_int64 created_at = 0;
};

struct SnapshotTreeNode {
    sqlite3_int64 inode = 0;
    std::string kind;
    int mode = 0;
    std::string owner_principal = "local";
    sqlite3_int64 uid = 0;
    sqlite3_int64 gid = 0;
    sqlite3_int64 size = 0;
    sqlite3_int64 version = 0;
    sqlite3_int64 canonical_version = 0;
    sqlite3_int64 created_at = 0;
    sqlite3_int64 accessed_at = 0;
    sqlite3_int64 updated_at = 0;
    sqlite3_int64 changed_at = 0;
    std::vector<AclEntry> acl;
    std::map<std::string, std::vector<unsigned char>> xattrs;
};

using SnapshotTree = std::map<std::string, SnapshotTreeNode>;

QuotaStats SnapshotQuotaStats(const SnapshotTree &tree) {
    QuotaStats result;
    std::unordered_set<sqlite3_int64> counted;
    for (const auto &[path, node] : tree) {
        (void)path;
        if (node.kind == "directory" || !counted.insert(node.inode).second) continue;
        ++result.live_files;
        if (node.size > std::numeric_limits<sqlite3_int64>::max() - result.live_bytes) {
            throw SqlError("snapshot byte usage is too large", SQLITE_TOOBIG);
        }
        result.live_bytes += node.size;
        result.largest_file_bytes = std::max(result.largest_file_bytes, node.size);
    }
    return result;
}

void CheckQuotaForSnapshotRestore(sqlite3 *db, const Workspace &workspace,
                                  const SnapshotTree &current,
                                  const SnapshotTree &target) {
    const QuotaPolicy policy = ReadQuotaPolicy(db, workspace.id);
    const QuotaStats before = SnapshotQuotaStats(current);
    const QuotaStats after = SnapshotQuotaStats(target);
    if (policy.max_files >= 0 && after.live_files > policy.max_files &&
        after.live_files >= before.live_files) {
        throw SqlError("snapshot exceeds workspace file quota", SQLITE_FULL);
    }
    if (policy.max_bytes >= 0 && after.live_bytes > policy.max_bytes &&
        after.live_bytes >= before.live_bytes) {
        throw SqlError("snapshot exceeds workspace byte quota", SQLITE_FULL);
    }
    if (policy.max_file_bytes >= 0 &&
        after.largest_file_bytes > policy.max_file_bytes &&
        after.largest_file_bytes >= before.largest_file_bytes) {
        throw SqlError("snapshot exceeds maximum file size quota", SQLITE_FULL);
    }
}

void ResolveSnapshotCanonicalVersions(sqlite3 *db, SnapshotTree &tree) {
    std::string requested = "[";
    bool first = true;
    size_t expected = 0;
    std::map<std::pair<sqlite3_int64, sqlite3_int64>, bool> unique;
    for (const auto &[path, node] : tree) {
        (void)path;
        if ((node.kind != "file" && node.kind != "symlink") || node.version <= 0) continue;
        if (!unique.emplace(std::make_pair(node.inode, node.version), true).second) continue;
        if (!first) requested += ',';
        first = false;
        ++expected;
        requested += "{\"inode\":" + std::to_string(node.inode) +
            ",\"version\":" + std::to_string(node.version) + "}";
    }
    requested += ']';
    if (expected == 0) return;

    Statement statement(db, R"SQL(
WITH RECURSIVE
requested(inode_id,requested_version) AS (
  SELECT CAST(json_extract(value,'$.inode') AS INTEGER),
         CAST(json_extract(value,'$.version') AS INTEGER)
  FROM json_each(?1)
),
chain(inode_id,requested_version,version_no,source_version_no,depth) AS (
  SELECT r.inode_id,r.requested_version,v.version_no,v.source_version_no,0
  FROM requested r JOIN _vexfs_file_versions v
    ON v.inode_id=r.inode_id AND v.version_no=r.requested_version
  UNION ALL
  SELECT c.inode_id,c.requested_version,v.version_no,v.source_version_no,c.depth+1
  FROM chain c JOIN _vexfs_file_versions v
    ON v.inode_id=c.inode_id AND v.version_no=c.source_version_no
  WHERE c.source_version_no IS NOT NULL AND c.depth<1024
)
SELECT inode_id,requested_version,version_no FROM chain
WHERE source_version_no IS NULL
)SQL");
    statement.BindText(1, requested);
    std::map<std::pair<sqlite3_int64, sqlite3_int64>, sqlite3_int64> resolved;
    while (statement.Row()) {
        resolved[{statement.Int64(0), statement.Int64(1)}] = statement.Int64(2);
    }
    if (resolved.size() != expected) {
        throw SqlError("workspace history references a missing or cyclic file version",
                       SQLITE_CORRUPT);
    }
    for (auto &[path, node] : tree) {
        (void)path;
        if ((node.kind == "file" || node.kind == "symlink") && node.version > 0) {
            node.canonical_version = resolved.at({node.inode, node.version});
        } else {
            node.canonical_version = node.version;
        }
    }
}

sqlite3_int64 CurrentHead(sqlite3 *db, const Workspace &workspace) {
    Statement statement(db,
        "SELECT COALESCE(head_commit,0) FROM _vexfs_workspaces WHERE id=?1");
    statement.BindInt64(1, workspace.id);
    if (!statement.Row()) throw SqlError("workspace not found", SQLITE_NOTFOUND);
    return statement.Int64(0);
}

void ValidateHistoryCommit(sqlite3 *db, const Workspace &workspace, sqlite3_int64 commit) {
    Statement statement(db,
        "SELECT COALESCE(history_floor_commit,0),"
        "EXISTS(SELECT 1 FROM _vexfs_commits WHERE id=?2 AND workspace_id=?1) "
        "FROM _vexfs_workspaces WHERE id=?1");
    statement.BindInt64(1, workspace.id);
    statement.BindInt64(2, commit);
    if (!statement.Row() || statement.Int(1) != 1) {
        throw SqlError("workspace commit not found: " + std::to_string(commit), SQLITE_NOTFOUND);
    }
    const sqlite3_int64 floor = statement.Int64(0);
    if (floor == 0 || commit < floor) {
        throw SqlError("workspace history before commit " + std::to_string(floor) +
                       " is not restorable", SQLITE_NOTFOUND);
    }
}

SnapshotInfo FindSnapshot(sqlite3 *db, const Workspace &workspace, const std::string &name) {
    Statement statement(db,
        "SELECT id,name,commit_id,created_at FROM _vexfs_snapshots "
        "WHERE workspace_id=?1 AND name=?2");
    statement.BindInt64(1, workspace.id);
    statement.BindText(2, name);
    if (!statement.Row()) throw SqlError("snapshot not found: " + name, SQLITE_NOTFOUND);
    SnapshotInfo result{statement.Int64(0), statement.Text(1), statement.Int64(2),
                        statement.Int64(3)};
    ValidateHistoryCommit(db, workspace, result.commit);
    return result;
}

sqlite3_int64 ResolveSnapshotReference(sqlite3 *db, const Workspace &workspace,
                                       const std::string &reference) {
    if (reference == "HEAD") {
        const sqlite3_int64 head = CurrentHead(db, workspace);
        ValidateHistoryCommit(db, workspace, head);
        return head;
    }
    return FindSnapshot(db, workspace, reference).commit;
}

SnapshotTree CurrentTree(sqlite3 *db, const Workspace &workspace) {
    Statement statement(db, R"SQL(
WITH RECURSIVE tree(path,inode_id,depth) AS (
  SELECT '/',?2,0
  UNION ALL
  SELECT CASE WHEN tree.path='/' THEN '/'||d.name ELSE tree.path||'/'||d.name END,
         d.inode_id,tree.depth+1
  FROM tree JOIN _vexfs_dentries d
    ON d.workspace_id=?1 AND d.parent_inode=tree.inode_id
  JOIN _vexfs_inodes child ON child.id=d.inode_id AND child.deleted_at IS NULL
  WHERE tree.depth<1024
)
SELECT tree.path,i.id,i.kind,i.mode,i.owner_principal,i.uid,i.gid,
       i.size,i.current_version,i.created_at,i.accessed_at,i.updated_at,i.changed_at
FROM tree JOIN _vexfs_inodes i ON i.id=tree.inode_id AND i.deleted_at IS NULL
ORDER BY tree.path COLLATE BINARY
)SQL");
    statement.BindInt64(1, workspace.id);
    statement.BindInt64(2, workspace.root_inode);
    SnapshotTree tree;
    std::map<sqlite3_int64, std::vector<std::string>> paths_by_inode;
    while (statement.Row()) {
        const std::string path = statement.Text(0);
        SnapshotTreeNode node{statement.Int64(1), statement.Text(2), statement.Int(3),
                              statement.Text(4), statement.Int64(5), statement.Int64(6),
                              statement.Int64(7), statement.Int64(8), 0,
                              statement.Int64(9), statement.Int64(10),
                              statement.Int64(11), statement.Int64(12), {}, {}};
        tree.emplace(path, std::move(node));
        paths_by_inode[statement.Int64(1)].push_back(path);
    }
    if (tree.find("/") == tree.end()) {
        throw SqlError("workspace root is missing", SQLITE_CORRUPT);
    }
    // A regular file (or symlink) may have more than one dentry when the
    // workspace contains a hard link.  Directories may not: a repeated
    // directory inode would make the path graph cyclic/ambiguous and would
    // make a snapshot impossible to restore safely.  Keep this check after
    // materialising the CTE so it also catches cycles in a manually-corrupted
    // current tree (the depth guard in the CTE merely truncates those cycles).
    for (const auto &[inode, paths] : paths_by_inode) {
        const auto first = tree.find(paths.front());
        if (first != tree.end() && first->second.kind == "directory" && paths.size() > 1) {
            throw SqlError("workspace contains a directory cycle or duplicate inode",
                           SQLITE_CORRUPT);
        }
        (void)inode;
    }
    ResolveSnapshotCanonicalVersions(db, tree);
    Statement xattrs(db, R"SQL(
SELECT x.inode_id,x.name,x.value FROM _vexfs_xattrs x
JOIN _vexfs_inodes i ON i.id=x.inode_id
WHERE i.workspace_id=?1 AND i.deleted_at IS NULL
ORDER BY x.inode_id,x.name COLLATE BINARY
)SQL");
    xattrs.BindInt64(1, workspace.id);
    while (xattrs.Row()) {
        const auto paths = paths_by_inode.find(xattrs.Int64(0));
        if (paths == paths_by_inode.end()) continue;
        for (const std::string &path : paths->second) {
            tree[path].xattrs.emplace(xattrs.Text(1), xattrs.Blob(2));
        }
    }
    Statement acl(db,
        "SELECT inode_id,principal_id,effect,permissions,inherit_flags FROM _vexfs_acl_entries "
        "WHERE workspace_id=?1 ORDER BY inode_id,principal_id COLLATE BINARY,effect");
    acl.BindInt64(1, workspace.id);
    while (acl.Row()) {
        const auto paths = paths_by_inode.find(acl.Int64(0));
        if (paths == paths_by_inode.end()) continue;
        const AclEntry entry{acl.Text(1), acl.Text(2), acl.Text(3), acl.Int64(4)};
        for (const std::string &path : paths->second) tree[path].acl.push_back(entry);
    }
    return tree;
}

SnapshotTree TreeAtCommit(sqlite3 *db, const Workspace &workspace, sqlite3_int64 commit) {
    ValidateHistoryCommit(db, workspace, commit);
    Statement inode_statement(db, R"SQL(
WITH
ranked_inode AS MATERIALIZED (
  SELECT s.*,ROW_NUMBER() OVER(
    PARTITION BY s.inode_id ORDER BY s.commit_id DESC) AS vexfs_rank
  FROM _vexfs_inode_states s WHERE s.workspace_id=?1 AND s.commit_id<=?2
)
SELECT inode_id,kind,mode,owner_principal,uid,gid,size,current_version,
       created_at,accessed_at,updated_at,changed_at
FROM ranked_inode WHERE vexfs_rank=1 AND deleted_at IS NULL ORDER BY inode_id
)SQL");
    inode_statement.BindInt64(1, workspace.id);
    inode_statement.BindInt64(2, commit);
    std::map<sqlite3_int64, SnapshotTreeNode> nodes;
    while (inode_statement.Row()) {
        SnapshotTreeNode node{inode_statement.Int64(0), inode_statement.Text(1),
                              inode_statement.Int(2), inode_statement.Text(3),
                              inode_statement.Int64(4), inode_statement.Int64(5),
                              inode_statement.Int64(6), inode_statement.Int64(7), 0,
                              inode_statement.Int64(8), inode_statement.Int64(9),
                              inode_statement.Int64(10), inode_statement.Int64(11), {}, {}};
        nodes.emplace(node.inode, std::move(node));
    }
    if (nodes.find(workspace.root_inode) == nodes.end()) {
        throw SqlError("workspace root is missing from history", SQLITE_CORRUPT);
    }

    Statement dentry_statement(db, R"SQL(
WITH
ranked_dentry AS MATERIALIZED (
  SELECT s.*,ROW_NUMBER() OVER(
    PARTITION BY s.parent_inode,s.name ORDER BY s.commit_id DESC) AS vexfs_rank
  FROM _vexfs_dentry_states s WHERE s.workspace_id=?1 AND s.commit_id<=?2
)
SELECT parent_inode,name,inode_id FROM ranked_dentry
WHERE vexfs_rank=1 AND deleted=0 ORDER BY parent_inode,name COLLATE BINARY
)SQL");
    dentry_statement.BindInt64(1, workspace.id);
    dentry_statement.BindInt64(2, commit);
    std::map<sqlite3_int64, std::vector<std::pair<std::string, sqlite3_int64>>> children;
    while (dentry_statement.Row()) {
        children[dentry_statement.Int64(0)].emplace_back(
            dentry_statement.Text(1), dentry_statement.Int64(2));
    }

    struct PendingNode {
        sqlite3_int64 inode;
        std::string path;
        int depth;
    };
    std::vector<PendingNode> pending{{workspace.root_inode, "/", 0}};
    // Files and symlinks can legitimately be reached through several
    // dentries (hard links).  Only directory inodes participate in traversal,
    // so only they need a visited set.  Treating every inode as globally
    // visited incorrectly rejected a valid snapshot containing two hard links
    // to the same file.
    std::map<sqlite3_int64, bool> visited_directories;
    SnapshotTree tree;
    std::map<sqlite3_int64, std::vector<std::string>> paths_by_inode;
    for (size_t index = 0; index < pending.size(); ++index) {
        const PendingNode current = pending[index];
        const auto node = nodes.find(current.inode);
        if (node == nodes.end()) continue;
        if (node->second.kind == "directory" &&
            !visited_directories.emplace(current.inode, true).second) {
            throw SqlError("workspace history contains a directory cycle or duplicate inode",
                           SQLITE_CORRUPT);
        }
        tree.emplace(current.path, node->second);
        paths_by_inode[current.inode].push_back(current.path);
        if (node->second.kind != "directory") continue;
        const auto child_entries = children.find(current.inode);
        if (child_entries == children.end()) continue;
        if (current.depth >= 1024) {
            throw SqlError("workspace history exceeds 1024 path components", SQLITE_TOOBIG);
        }
        for (const auto &[name, child_inode] : child_entries->second) {
            if (nodes.find(child_inode) == nodes.end()) continue;
            const std::string child_path = current.path == "/" ? "/" + name :
                current.path + "/" + name;
            pending.push_back({child_inode, child_path, current.depth + 1});
        }
    }
    ResolveSnapshotCanonicalVersions(db, tree);

    Statement xattrs(db, R"SQL(
WITH ranked AS MATERIALIZED (
  SELECT s.*,ROW_NUMBER() OVER(
    PARTITION BY s.inode_id,s.name ORDER BY s.commit_id DESC) AS vexfs_rank
  FROM _vexfs_xattr_states s WHERE s.workspace_id=?1 AND s.commit_id<=?2)
SELECT inode_id,name,value FROM ranked
WHERE vexfs_rank=1 AND deleted=0 ORDER BY inode_id,name COLLATE BINARY
)SQL");
    xattrs.BindInt64(1, workspace.id);
    xattrs.BindInt64(2, commit);
    while (xattrs.Row()) {
        const auto paths = paths_by_inode.find(xattrs.Int64(0));
        if (paths == paths_by_inode.end()) continue;
        for (const std::string &path : paths->second) {
            tree[path].xattrs.emplace(xattrs.Text(1), xattrs.Blob(2));
        }
    }
    Statement acl(db, R"SQL(
WITH ranked AS MATERIALIZED (
  SELECT s.*,ROW_NUMBER() OVER(
    PARTITION BY s.inode_id,s.principal_id,s.effect ORDER BY s.commit_id DESC) AS vexfs_rank
  FROM _vexfs_acl_states s WHERE s.workspace_id=?1 AND s.commit_id<=?2)
SELECT inode_id,principal_id,effect,permissions,inherit_flags FROM ranked
WHERE vexfs_rank=1 AND deleted=0
ORDER BY inode_id,principal_id COLLATE BINARY,effect
)SQL");
    acl.BindInt64(1, workspace.id);
    acl.BindInt64(2, commit);
    while (acl.Row()) {
        const auto paths = paths_by_inode.find(acl.Int64(0));
        if (paths == paths_by_inode.end()) continue;
        const AclEntry entry{acl.Text(1), acl.Text(2), acl.Text(3), acl.Int64(4)};
        for (const std::string &path : paths->second) tree[path].acl.push_back(entry);
    }
    return tree;
}

bool SnapshotNodesEqual(const SnapshotTreeNode &left, const SnapshotTreeNode &right) {
    return left.inode == right.inode && left.kind == right.kind && left.mode == right.mode &&
        left.size == right.size && left.canonical_version == right.canonical_version &&
        left.owner_principal == right.owner_principal && left.uid == right.uid &&
        left.gid == right.gid && left.created_at == right.created_at &&
        left.accessed_at == right.accessed_at && left.updated_at == right.updated_at &&
        left.changed_at == right.changed_at && left.acl == right.acl &&
        left.xattrs == right.xattrs;
}

std::string SnapshotNodeJson(const SnapshotTreeNode &node) {
    std::string xattrs = "[";
    bool first = true;
    for (const auto &entry : node.xattrs) {
        if (!first) xattrs += ',';
        first = false;
        xattrs += "\"" + JsonEscape(entry.first) + "\"";
    }
    xattrs += ']';
    std::string acl = "[";
    first = true;
    for (const AclEntry &entry : node.acl) {
        if (!first) acl += ',';
        first = false;
        acl += "{\"principal\":\"" + JsonEscape(entry.principal) +
            "\",\"effect\":\"" + JsonEscape(entry.effect) +
            "\",\"permissions\":\"" + JsonEscape(entry.permissions) +
            "\",\"inherit\":" + std::to_string(entry.inherit_flags) + "}";
    }
    acl += ']';
    return "{\"inode\":" + std::to_string(node.inode) +
        ",\"kind\":\"" + JsonEscape(node.kind) + "\",\"mode\":" +
        std::to_string(node.mode) + ",\"owner_principal\":\"" +
        JsonEscape(node.owner_principal) + "\",\"uid\":" + std::to_string(node.uid) +
        ",\"gid\":" + std::to_string(node.gid) + ",\"size\":" + std::to_string(node.size) +
        ",\"version\":" + std::to_string(node.version) +
        ",\"content_version\":" + std::to_string(node.canonical_version) +
        ",\"created_at\":" + std::to_string(node.created_at) +
        ",\"accessed_at\":" + std::to_string(node.accessed_at) +
        ",\"updated_at\":" + std::to_string(node.updated_at) +
        ",\"changed_at\":" + std::to_string(node.changed_at) +
        ",\"acl\":" + acl + ",\"xattrs\":" + xattrs + "}";
}

std::string SnapshotTreeJson(const SnapshotTree &tree) {
    std::string json = "[";
    bool first = true;
    for (const auto &[path, node] : tree) {
        if (!first) json += ',';
        first = false;
        json += "{\"path\":\"" + JsonEscape(path) + "\",\"state\":" +
            SnapshotNodeJson(node) + "}";
    }
    json += ']';
    return json;
}

std::string SnapshotDiffJson(const SnapshotTree &before, const SnapshotTree &after) {
    std::string json = "[";
    bool first = true;
    auto append = [&](const std::string &path, const char *change,
                      const SnapshotTreeNode *old_node, const SnapshotTreeNode *new_node) {
        if (!first) json += ',';
        first = false;
        json += "{\"path\":\"" + JsonEscape(path) + "\",\"change\":\"" + change +
            "\",\"before\":" + (old_node == nullptr ? "null" : SnapshotNodeJson(*old_node)) +
            ",\"after\":" + (new_node == nullptr ? "null" : SnapshotNodeJson(*new_node)) + "}";
    };
    auto left = before.begin();
    auto right = after.begin();
    while (left != before.end() || right != after.end()) {
        if (right == after.end() || (left != before.end() && left->first < right->first)) {
            append(left->first, "delete", &left->second, nullptr);
            ++left;
        } else if (left == before.end() || right->first < left->first) {
            append(right->first, "add", nullptr, &right->second);
            ++right;
        } else {
            if (!SnapshotNodesEqual(left->second, right->second)) {
                append(left->first, "modify", &left->second, &right->second);
            }
            ++left;
            ++right;
        }
    }
    json += ']';
    return json;
}

void SnapshotCreateFunction(sqlite3_context *context, int argument_count,
                            sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const std::string name = RequiredText(values[1], "snapshot name");
        if (name.empty() || name.size() > 128 || name == "HEAD") {
            throw SqlError("snapshot name must be 1..128 bytes and not HEAD", SQLITE_MISMATCH);
        }
        const std::string mode = argument_count == 4
            ? RequiredText(values[3], "snapshot mode") : "consistent";
        if (mode != "consistent" && mode != "committed-only") {
            throw SqlError("snapshot mode must be consistent or committed-only", SQLITE_MISMATCH);
        }
        Savepoint savepoint(db, "vexfs_snapshot_create");
        AcquireWriteLock(db);
        const sqlite3_int64 head = CurrentHead(db, workspace);
        if (argument_count >= 3 && sqlite3_value_type(values[2]) != SQLITE_NULL) {
            const sqlite3_int64 expected = RequiredPositiveInteger(values[2], "expected head");
            if (expected != head) throw SqlError("workspace head conflict", SQLITE_CONSTRAINT);
        }
        if (mode == "consistent") {
            Statement dirty(db,
                "SELECT count(*) FROM _vexfs_handles WHERE workspace_id=?1 "
                "AND state IN ('open','retained') "
                "AND dirty_generation>published_generation");
            dirty.BindInt64(1, workspace.id);
            dirty.Row();
            if (dirty.Int64(0) != 0) {
                throw SqlError(
                    "workspace has unpublished file handles; synchronize the owning mount "
                    "or use committed-only", SQLITE_BUSY);
            }
        }
        ValidateHistoryCommit(db, workspace, head);
        Statement insert(db,
            "INSERT INTO _vexfs_snapshots(workspace_id,name,commit_id) VALUES(?1,?2,?3)");
        insert.BindInt64(1, workspace.id);
        insert.BindText(2, name);
        insert.BindInt64(3, head);
        insert.Done();
        savepoint.Release();
        sqlite3_result_int64(context, head);
    });
}

void SnapshotListFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        Statement statement(db,
            "SELECT name,commit_id,created_at FROM _vexfs_snapshots "
            "WHERE workspace_id=?1 ORDER BY created_at DESC,id DESC");
        statement.BindInt64(1, workspace.id);
        std::string json = "[";
        bool first = true;
        while (statement.Row()) {
            if (!first) json += ',';
            first = false;
            json += "{\"name\":\"" + JsonEscape(statement.Text(0)) +
                "\",\"commit\":" + std::to_string(statement.Int64(1)) +
                ",\"created_at\":" + std::to_string(statement.Int64(2)) + "}";
        }
        json += ']';
        sqlite3_result_text(context, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    });
}

void SnapshotShowFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const SnapshotInfo snapshot = FindSnapshot(
            db, workspace, RequiredText(values[1], "snapshot name"));
        const std::string json = "{\"name\":\"" + JsonEscape(snapshot.name) +
            "\",\"commit\":" + std::to_string(snapshot.commit) +
            ",\"created_at\":" + std::to_string(snapshot.created_at) +
            ",\"entries\":" + SnapshotTreeJson(TreeAtCommit(db, workspace, snapshot.commit)) + "}";
        sqlite3_result_text(context, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    });
}

void SnapshotDiffFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const std::string from = RequiredText(values[1], "from snapshot");
        const std::string to = RequiredText(values[2], "to snapshot");
        const sqlite3_int64 from_commit = ResolveSnapshotReference(db, workspace, from);
        const sqlite3_int64 to_commit = ResolveSnapshotReference(db, workspace, to);
        const SnapshotTree from_tree = from == "HEAD" ? CurrentTree(db, workspace) :
            TreeAtCommit(db, workspace, from_commit);
        const SnapshotTree to_tree = to == "HEAD" ? CurrentTree(db, workspace) :
            TreeAtCommit(db, workspace, to_commit);
        const std::string changes = SnapshotDiffJson(from_tree, to_tree);
        const std::string json = "{\"from\":\"" + JsonEscape(from) +
            "\",\"from_commit\":" + std::to_string(from_commit) +
            ",\"to\":\"" + JsonEscape(to) + "\",\"to_commit\":" +
            std::to_string(to_commit) + ",\"changes\":" + changes + "}";
        sqlite3_result_text(context, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    });
}

void SnapshotDropFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const std::string name = RequiredText(values[1], "snapshot name");
        Statement remove(db,
            "DELETE FROM _vexfs_snapshots WHERE workspace_id=?1 AND name=?2");
        remove.BindInt64(1, workspace.id);
        remove.BindText(2, name);
        remove.Done();
        if (sqlite3_changes64(db) != 1) {
            throw SqlError("snapshot not found: " + name, SQLITE_NOTFOUND);
        }
        sqlite3_result_int(context, 1);
    });
}

QuotaStats ReadQuotaStats(sqlite3 *db, sqlite3_int64 workspace_id) {
    Statement statement(db, R"SQL(
SELECT workspace.live_files,workspace.live_bytes,
       COALESCE((SELECT max(size) FROM _vexfs_inodes
                 WHERE workspace_id=?1 AND deleted_at IS NULL AND kind<>'directory'),0)
FROM _vexfs_workspaces workspace WHERE workspace.id=?1
)SQL");
    statement.BindInt64(1, workspace_id);
    statement.Row();
    return {statement.Int64(0), statement.Int64(1), statement.Int64(2)};
}

std::string NullableQuotaJson(sqlite3_int64 value) {
    return value < 0 ? "null" : std::to_string(value);
}

std::string QuotaJson(const std::string &workspace_name,
                      const QuotaPolicy &policy, const QuotaStats &stats) {
    const bool over_bytes = policy.max_bytes >= 0 && stats.live_bytes > policy.max_bytes;
    const bool over_files = policy.max_files >= 0 && stats.live_files > policy.max_files;
    const bool over_file_bytes = policy.max_file_bytes >= 0 &&
        stats.largest_file_bytes > policy.max_file_bytes;
    return "{\"workspace\":\"" + JsonEscape(workspace_name) +
        "\",\"max_bytes\":" + NullableQuotaJson(policy.max_bytes) +
        ",\"max_files\":" + NullableQuotaJson(policy.max_files) +
        ",\"max_file_bytes\":" + NullableQuotaJson(policy.max_file_bytes) +
        ",\"live_bytes\":" + std::to_string(stats.live_bytes) +
        ",\"live_files\":" + std::to_string(stats.live_files) +
        ",\"largest_file_bytes\":" + std::to_string(stats.largest_file_bytes) +
        ",\"over_quota\":" +
            ((over_bytes || over_files || over_file_bytes) ? "true" : "false") + "}";
}

sqlite3_int64 OptionalQuotaValue(sqlite3_value *value, const char *name) {
    if (sqlite3_value_type(value) == SQLITE_NULL) return -1;
    return RequiredNonnegativeInteger(value, name);
}

void QuotaGetFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string workspace_name = RequiredText(values[0], "workspace");
        const Workspace workspace = FindWorkspace(db, workspace_name);
        const std::string json = QuotaJson(
            workspace_name, ReadQuotaPolicy(db, workspace.id),
            ReadQuotaStats(db, workspace.id));
        sqlite3_result_text(context, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    });
}

void QuotaSetFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string workspace_name = RequiredText(values[0], "workspace");
        const sqlite3_int64 max_bytes = OptionalQuotaValue(values[1], "max_bytes");
        const sqlite3_int64 max_files = OptionalQuotaValue(values[2], "max_files");
        const sqlite3_int64 max_file_bytes =
            OptionalQuotaValue(values[3], "max_file_bytes");
        const Workspace workspace = FindWorkspace(db, workspace_name);

        Savepoint savepoint(db, "vexfs_quota_set");
        AcquireWriteLock(db);
        Statement update(db, R"SQL(
UPDATE _vexfs_workspaces
SET quota_max_bytes=?1,quota_max_files=?2,quota_max_file_bytes=?3
WHERE id=?4
)SQL");
        if (max_bytes < 0) update.BindNull(1); else update.BindInt64(1, max_bytes);
        if (max_files < 0) update.BindNull(2); else update.BindInt64(2, max_files);
        if (max_file_bytes < 0) update.BindNull(3);
        else update.BindInt64(3, max_file_bytes);
        update.BindInt64(4, workspace.id);
        update.Done();
        savepoint.Release();

        const QuotaPolicy policy{max_bytes, max_files, max_file_bytes};
        const std::string json = QuotaJson(
            workspace_name, policy, ReadQuotaStats(db, workspace.id));
        sqlite3_result_text(context, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    });
}

struct RetentionPolicy {
    sqlite3_int64 keep_versions = 0;
    sqlite3_int64 keep_days = 0;
    bool gc_paused = false;
};

struct RetentionStats {
    sqlite3_int64 live_files = 0;
    sqlite3_int64 live_bytes = 0;
    sqlite3_int64 versions = 0;
    sqlite3_int64 stored_version_bytes = 0;
    sqlite3_int64 live_storage_bytes = 0;
    sqlite3_int64 reclaimable_versions = 0;
    sqlite3_int64 reclaimable_bytes = 0;
};

RetentionPolicy ReadRetentionPolicy(sqlite3 *db, const Workspace &workspace) {
    Statement statement(db,
        "SELECT retention_keep_versions,retention_keep_days,gc_paused "
        "FROM _vexfs_workspaces WHERE id=?1");
    statement.BindInt64(1, workspace.id);
    if (!statement.Row()) throw SqlError("workspace not found", SQLITE_NOTFOUND);
    return {statement.Int64(0), statement.Int64(1), statement.Int(2) != 0};
}

void PrepareRetentionKeepSet(sqlite3 *db, const Workspace &workspace,
                             const RetentionPolicy &policy) {
    Exec(db, R"SQL(
CREATE TEMP TABLE IF NOT EXISTS _vexfs_gc_keep_versions(
    inode_id INTEGER NOT NULL,
    version_no INTEGER NOT NULL,
    reason TEXT NOT NULL,
    PRIMARY KEY(inode_id,version_no)
);
DELETE FROM _vexfs_gc_keep_versions;
)SQL");

    Statement current(db, R"SQL(
INSERT OR IGNORE INTO _vexfs_gc_keep_versions(inode_id,version_no,reason)
SELECT id,current_version,'current'
FROM _vexfs_inodes
WHERE workspace_id=?1 AND deleted_at IS NULL AND kind<>'directory' AND current_version>0
)SQL");
    current.BindInt64(1, workspace.id);
    current.Done();

    Statement handles(db, R"SQL(
INSERT OR IGNORE INTO _vexfs_gc_keep_versions(inode_id,version_no,reason)
SELECT inode_id,expected_version,'handle'
FROM _vexfs_handles
WHERE workspace_id=?1 AND state IN ('open','retained') AND expected_version>0
)SQL");
    handles.BindInt64(1, workspace.id);
    handles.Done();

    if (policy.keep_versions > 0) {
        Statement recent(db, R"SQL(
INSERT OR IGNORE INTO _vexfs_gc_keep_versions(inode_id,version_no,reason)
SELECT inode_id,version_no,'recent'
FROM (
  SELECT v.inode_id,v.version_no,
         ROW_NUMBER() OVER(PARTITION BY v.inode_id ORDER BY v.version_no DESC) AS rank
  FROM _vexfs_file_versions v
  JOIN _vexfs_inodes i ON i.id=v.inode_id
  WHERE i.workspace_id=?1
)
WHERE rank<=?2
)SQL");
        recent.BindInt64(1, workspace.id);
        recent.BindInt64(2, policy.keep_versions);
        recent.Done();
    }

    if (policy.keep_days > 0) {
        Statement recent_days(db, R"SQL(
INSERT OR IGNORE INTO _vexfs_gc_keep_versions(inode_id,version_no,reason)
SELECT v.inode_id,v.version_no,'age'
FROM _vexfs_file_versions v
JOIN _vexfs_inodes i ON i.id=v.inode_id
WHERE i.workspace_id=?1
  AND v.created_at>=CAST(unixepoch('subsec')*1000 AS INTEGER)-(?2*86400000)
)SQL");
        recent_days.BindInt64(1, workspace.id);
        recent_days.BindInt64(2, policy.keep_days);
        recent_days.Done();
    }

    // 快照恢复按每个 inode 在目标 commit 之前的最后一条完整状态重建。
    // 先排名再过滤 deleted，避免把快照中已经删除的 inode 的更早版本误保留。
    Statement snapshots(db, R"SQL(
INSERT OR IGNORE INTO _vexfs_gc_keep_versions(inode_id,version_no,reason)
WITH ranked AS (
  SELECT snapshot.id AS snapshot_id,state.inode_id,state.current_version,state.deleted_at,
         ROW_NUMBER() OVER(
           PARTITION BY snapshot.id,state.inode_id ORDER BY state.commit_id DESC) AS rank
  FROM _vexfs_snapshots snapshot
  JOIN _vexfs_inode_states state
    ON state.workspace_id=snapshot.workspace_id AND state.commit_id<=snapshot.commit_id
  WHERE snapshot.workspace_id=?1 AND state.kind<>'directory' AND state.current_version>0
)
SELECT inode_id,current_version,'snapshot'
FROM ranked WHERE rank=1 AND deleted_at IS NULL
)SQL");
    snapshots.BindInt64(1, workspace.id);
    snapshots.Done();

    // restore 生成的版本是不可变内容的别名。任何被保留的别名都必须继续保留
    // 它最终指向的 canonical manifest；循环也兼容未来出现多级别名的情况。
    while (true) {
        Statement sources(db, R"SQL(
INSERT OR IGNORE INTO _vexfs_gc_keep_versions(inode_id,version_no,reason)
SELECT version.inode_id,version.source_version_no,'source'
FROM _vexfs_file_versions version
JOIN _vexfs_gc_keep_versions kept
  ON kept.inode_id=version.inode_id AND kept.version_no=version.version_no
WHERE version.source_version_no IS NOT NULL
)SQL");
        sources.Done();
        if (sqlite3_changes64(db) == 0) break;
    }
}

RetentionStats ReadRetentionStats(sqlite3 *db, const Workspace &workspace) {
    RetentionStats result;
    {
        Statement current(db,
            "SELECT live_files,live_bytes FROM _vexfs_workspaces WHERE id=?1");
        current.BindInt64(1, workspace.id);
        current.Row();
        result.live_files = current.Int64(0);
        result.live_bytes = current.Int64(1);
    }
    {
        Statement versions(db, R"SQL(
SELECT (SELECT count(*) FROM _vexfs_file_versions v
        JOIN _vexfs_inodes i ON i.id=v.inode_id WHERE i.workspace_id=?1),
       COALESCE((SELECT sum(size) FROM _vexfs_chunks WHERE workspace_id=?1),0)
)SQL");
        versions.BindInt64(1, workspace.id);
        versions.Row();
        result.versions = versions.Int64(0);
        result.stored_version_bytes = versions.Int64(1);
    }
    {
        Statement live_storage(db, R"SQL(
WITH live_manifests AS (
  SELECT DISTINCT source.manifest_id
  FROM _vexfs_inodes inode
  JOIN _vexfs_file_versions current
    ON current.inode_id=inode.id AND current.version_no=inode.current_version
  JOIN _vexfs_file_versions source
    ON source.inode_id=current.inode_id
   AND source.version_no=COALESCE(current.source_version_no,current.version_no)
  WHERE inode.workspace_id=?1 AND inode.deleted_at IS NULL AND inode.kind<>'directory'
), live_chunks AS (
  SELECT DISTINCT entry.chunk_id FROM live_manifests live
  JOIN _vexfs_manifest_chunks entry ON entry.manifest_id=live.manifest_id
)
SELECT COALESCE(sum(chunk.size),0)
FROM live_chunks live JOIN _vexfs_chunks chunk ON chunk.id=live.chunk_id
)SQL");
        live_storage.BindInt64(1, workspace.id);
        live_storage.Row();
        result.live_storage_bytes = live_storage.Int64(0);
    }
    {
        Statement reclaimable(db, R"SQL(
WITH reclaimable_versions AS MATERIALIZED (
 SELECT v.id,v.manifest_id
 FROM _vexfs_file_versions v
 JOIN _vexfs_inodes i ON i.id=v.inode_id
 LEFT JOIN _vexfs_gc_keep_versions kept
   ON kept.inode_id=v.inode_id AND kept.version_no=v.version_no
 WHERE i.workspace_id=?1 AND kept.inode_id IS NULL
), reclaimable_manifests AS MATERIALIZED (
 SELECT manifest_id FROM reclaimable_versions WHERE manifest_id IS NOT NULL
), reclaimable_chunks AS (
 SELECT DISTINCT entry.chunk_id FROM _vexfs_manifest_chunks entry
 JOIN reclaimable_manifests manifest ON manifest.manifest_id=entry.manifest_id
 WHERE NOT EXISTS(
  SELECT 1 FROM _vexfs_manifest_chunks retained
  WHERE retained.chunk_id=entry.chunk_id
    AND retained.manifest_id NOT IN (SELECT manifest_id FROM reclaimable_manifests))
)
SELECT (SELECT count(*) FROM reclaimable_versions),
       COALESCE((SELECT sum(chunk.size) FROM reclaimable_chunks item
                 JOIN _vexfs_chunks chunk ON chunk.id=item.chunk_id),0)
)SQL");
        reclaimable.BindInt64(1, workspace.id);
        reclaimable.Row();
        result.reclaimable_versions = reclaimable.Int64(0);
        result.reclaimable_bytes = reclaimable.Int64(1);
    }
    return result;
}

std::string RetentionJson(const std::string &workspace_name,
                          const RetentionPolicy &policy,
                          const RetentionStats &stats) {
    const sqlite3_int64 retained_history = std::max<sqlite3_int64>(
        0, stats.stored_version_bytes - stats.live_storage_bytes);
    return "{\"workspace\":\"" + JsonEscape(workspace_name) +
        "\",\"keep_versions\":" + std::to_string(policy.keep_versions) +
        ",\"keep_days\":" + std::to_string(policy.keep_days) +
        ",\"gc_paused\":" + (policy.gc_paused ? "true" : "false") +
        ",\"live_files\":" + std::to_string(stats.live_files) +
        ",\"live_bytes\":" + std::to_string(stats.live_bytes) +
        ",\"stored_versions\":" + std::to_string(stats.versions) +
        ",\"stored_version_bytes\":" + std::to_string(stats.stored_version_bytes) +
        ",\"retained_history_bytes\":" + std::to_string(retained_history) +
        ",\"reclaimable_versions\":" + std::to_string(stats.reclaimable_versions) +
        ",\"reclaimable_bytes\":" + std::to_string(stats.reclaimable_bytes) + "}";
}

void RetentionGetFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string workspace_name = RequiredText(values[0], "workspace");
        const Workspace workspace = FindWorkspace(db, workspace_name);
        const RetentionPolicy policy = ReadRetentionPolicy(db, workspace);
        PrepareRetentionKeepSet(db, workspace, policy);
        const std::string json = RetentionJson(
            workspace_name, policy, ReadRetentionStats(db, workspace));
        sqlite3_result_text(context, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    });
}

void RetentionSetFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string workspace_name = RequiredText(values[0], "workspace");
        const sqlite3_int64 keep_versions =
            RequiredNonnegativeInteger(values[1], "keep_versions");
        const sqlite3_int64 keep_days = RequiredNonnegativeInteger(values[2], "keep_days");
        if (keep_versions > 1000000) {
            throw SqlError("keep_versions must be at most 1000000", SQLITE_RANGE);
        }
        if (keep_days > 36500) {
            throw SqlError("keep_days must be at most 36500", SQLITE_RANGE);
        }
        const Workspace workspace = FindWorkspace(db, workspace_name);
        Statement update(db,
            "UPDATE _vexfs_workspaces SET retention_keep_versions=?1,retention_keep_days=?2 "
            "WHERE id=?3");
        update.BindInt64(1, keep_versions);
        update.BindInt64(2, keep_days);
        update.BindInt64(3, workspace.id);
        update.Done();
        const RetentionPolicy policy = ReadRetentionPolicy(db, workspace);
        PrepareRetentionKeepSet(db, workspace, policy);
        const std::string json = RetentionJson(
            workspace_name, policy, ReadRetentionStats(db, workspace));
        sqlite3_result_text(context, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    });
}

void GcPauseFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string workspace_name = RequiredText(values[0], "workspace");
        if (sqlite3_value_type(values[1]) != SQLITE_INTEGER ||
            (sqlite3_value_int(values[1]) != 0 && sqlite3_value_int(values[1]) != 1)) {
            throw SqlError("paused must be 0 or 1", SQLITE_MISMATCH);
        }
        const Workspace workspace = FindWorkspace(db, workspace_name);
        Savepoint savepoint(db, "vexfs_gc_pause");
        AcquireWriteLock(db);
        Statement update(db,
            "UPDATE _vexfs_workspaces SET gc_paused=?1 WHERE id=?2");
        update.BindInt(1, sqlite3_value_int(values[1]));
        update.BindInt64(2, workspace.id);
        update.Done();
        savepoint.Release();
        const RetentionPolicy policy = ReadRetentionPolicy(db, workspace);
        PrepareRetentionKeepSet(db, workspace, policy);
        const std::string json = RetentionJson(
            workspace_name, policy, ReadRetentionStats(db, workspace));
        sqlite3_result_text(context, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    });
}

void GcFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string workspace_name = RequiredText(values[0], "workspace");
        const sqlite3_int64 batch = RequiredPositiveInteger(values[1], "batch");
        if (batch > 10000) throw SqlError("batch must be at most 10000", SQLITE_RANGE);
        const Workspace workspace = FindWorkspace(db, workspace_name);

        Savepoint savepoint(db, "vexfs_gc");
        AcquireWriteLock(db);
        {
            Statement mounted(db,
                "SELECT 1 FROM _vexfs_mount_sessions WHERE workspace_id=?1 "
                "AND lease_until>CAST(unixepoch('subsec')*1000 AS INTEGER) LIMIT 1");
            mounted.BindInt64(1, workspace.id);
            if (mounted.Row()) {
                throw SqlError("workspace has an active mount session; unmount before GC",
                               SQLITE_BUSY);
            }
        }
        const RetentionPolicy policy = ReadRetentionPolicy(db, workspace);
        if (policy.gc_paused) throw SqlError("workspace GC is paused", SQLITE_BUSY);
        PrepareRetentionKeepSet(db, workspace, policy);

        Exec(db, R"SQL(
CREATE TEMP TABLE IF NOT EXISTS _vexfs_gc_delete_versions(
    id INTEGER PRIMARY KEY,
    inode_id INTEGER NOT NULL,
    version_no INTEGER NOT NULL,
    manifest_id INTEGER NOT NULL,
    stored_bytes INTEGER NOT NULL
);
DELETE FROM _vexfs_gc_delete_versions;
)SQL");
        Statement candidates(db, R"SQL(
INSERT INTO _vexfs_gc_delete_versions(id,inode_id,version_no,manifest_id,stored_bytes)
SELECT version.id,version.inode_id,version.version_no,
       COALESCE(version.manifest_id,0),COALESCE(manifest.file_size,0)
FROM _vexfs_file_versions version
JOIN _vexfs_inodes inode ON inode.id=version.inode_id
LEFT JOIN _vexfs_manifests manifest ON manifest.id=version.manifest_id
LEFT JOIN _vexfs_gc_keep_versions kept
  ON kept.inode_id=version.inode_id AND kept.version_no=version.version_no
WHERE inode.workspace_id=?1 AND kept.inode_id IS NULL
  AND (version.source_version_no IS NOT NULL OR NOT EXISTS (
    SELECT 1 FROM _vexfs_file_versions alias
    WHERE alias.inode_id=version.inode_id
      AND alias.source_version_no=version.version_no
  ))
ORDER BY version.source_version_no IS NULL,version.created_at,version.id
LIMIT ?2
)SQL");
        candidates.BindInt64(1, workspace.id);
        candidates.BindInt64(2, batch);
        candidates.Done();

        sqlite3_int64 deleted_versions = 0;
        sqlite3_int64 reclaimed_bytes = 0;
        {
            Statement count(db,
                "SELECT count(*) FROM _vexfs_gc_delete_versions");
            count.Row();
            deleted_versions = count.Int64(0);
        }
        {
            Statement bytes(db, R"SQL(
SELECT COALESCE(sum(chunk.size),0)
FROM _vexfs_chunks chunk
WHERE EXISTS(
  SELECT 1 FROM _vexfs_manifest_chunks removed
  JOIN _vexfs_gc_delete_versions candidate
    ON candidate.manifest_id=removed.manifest_id
  WHERE removed.chunk_id=chunk.id AND candidate.manifest_id<>0)
AND NOT EXISTS(
  SELECT 1 FROM _vexfs_manifest_chunks retained
  WHERE retained.chunk_id=chunk.id AND retained.manifest_id NOT IN(
    SELECT manifest_id FROM _vexfs_gc_delete_versions WHERE manifest_id<>0))
)SQL");
            bytes.Row();
            reclaimed_bytes = bytes.Int64(0);
        }
        if (deleted_versions > 0) {
            // 每条 inode state 都是完整状态。若它引用本批删除的版本，它既不是当前
            // 状态，也不是任何快照的边界状态；对应版本已在 keep set 中排除证明。
            Statement states(db, R"SQL(
DELETE FROM _vexfs_inode_states
WHERE workspace_id=?1 AND EXISTS (
  SELECT 1 FROM _vexfs_gc_delete_versions removed
  WHERE removed.inode_id=_vexfs_inode_states.inode_id
    AND removed.version_no=_vexfs_inode_states.current_version
)
)SQL");
            states.BindInt64(1, workspace.id);
            states.Done();
            Exec(db, R"SQL(
DELETE FROM _vexfs_file_versions
WHERE id IN (SELECT id FROM _vexfs_gc_delete_versions);
DELETE FROM _vexfs_manifest_chunks
WHERE manifest_id IN (
  SELECT manifest_id FROM _vexfs_gc_delete_versions WHERE manifest_id<>0
);
DELETE FROM _vexfs_manifests
WHERE id IN (
  SELECT manifest_id FROM _vexfs_gc_delete_versions WHERE manifest_id<>0
);
DELETE FROM _vexfs_chunks
WHERE workspace_id=(SELECT workspace_id FROM _vexfs_inodes
                    WHERE id=(SELECT inode_id FROM _vexfs_gc_delete_versions LIMIT 1))
  AND NOT EXISTS(SELECT 1 FROM _vexfs_manifest_chunks entry
                 WHERE entry.chunk_id=_vexfs_chunks.id);
)SQL");
        }

        PrepareRetentionKeepSet(db, workspace, policy);
        const RetentionStats remaining = ReadRetentionStats(db, workspace);
        const std::string json = "{\"workspace\":\"" + JsonEscape(workspace_name) +
            "\",\"batch\":" + std::to_string(batch) +
            ",\"deleted_versions\":" + std::to_string(deleted_versions) +
            ",\"reclaimed_bytes\":" + std::to_string(reclaimed_bytes) +
            ",\"remaining_versions\":" + std::to_string(remaining.versions) +
            ",\"remaining_reclaimable_versions\":" +
                std::to_string(remaining.reclaimable_versions) +
            ",\"remaining_reclaimable_bytes\":" +
                std::to_string(remaining.reclaimable_bytes) +
            ",\"has_more\":" +
                (remaining.reclaimable_versions == 0 ? "false" : "true") + "}";
        savepoint.Release();
        sqlite3_result_text(context, json.data(), static_cast<int>(json.size()), SQLITE_TRANSIENT);
    });
}

void RestoreWorkspaceTree(sqlite3 *db, const Workspace &workspace, sqlite3_int64 commit) {
    Statement clear_acl(db,
        "DELETE FROM _vexfs_acl_entries WHERE workspace_id=?1");
    clear_acl.BindInt64(1, workspace.id);
    clear_acl.Done();
    Statement clear_xattrs(db, R"SQL(
DELETE FROM _vexfs_xattrs WHERE inode_id IN (
  SELECT id FROM _vexfs_inodes WHERE workspace_id=?1)
)SQL");
    clear_xattrs.BindInt64(1, workspace.id);
    clear_xattrs.Done();
    Statement clear_dentries(db, "DELETE FROM _vexfs_dentries WHERE workspace_id=?1");
    clear_dentries.BindInt64(1, workspace.id);
    clear_dentries.Done();
    Statement tombstone(db,
        "UPDATE _vexfs_inodes SET deleted_at=CAST(strftime('%s','now') AS INTEGER)*1000 "
        "WHERE workspace_id=?1");
    tombstone.BindInt64(1, workspace.id);
    tombstone.Done();

    Statement restore_inodes(db, R"SQL(
WITH ranked AS MATERIALIZED (
  SELECT s.*,ROW_NUMBER() OVER(
    PARTITION BY s.inode_id ORDER BY s.commit_id DESC) AS vexfs_rank
  FROM _vexfs_inode_states s WHERE s.workspace_id=?1 AND s.commit_id<=?2),
latest AS MATERIALIZED (SELECT * FROM ranked WHERE vexfs_rank=1)
UPDATE _vexfs_inodes AS inode SET
  kind=(SELECT kind FROM latest WHERE inode_id=inode.id),
  mode=(SELECT mode FROM latest WHERE inode_id=inode.id),
  owner_principal=(SELECT owner_principal FROM latest WHERE inode_id=inode.id),
  uid=(SELECT uid FROM latest WHERE inode_id=inode.id),
  gid=(SELECT gid FROM latest WHERE inode_id=inode.id),
  size=(SELECT size FROM latest WHERE inode_id=inode.id),
  current_version=(SELECT current_version FROM latest WHERE inode_id=inode.id),
  created_at=(SELECT created_at FROM latest WHERE inode_id=inode.id),
  accessed_at=(SELECT accessed_at FROM latest WHERE inode_id=inode.id),
  updated_at=(SELECT updated_at FROM latest WHERE inode_id=inode.id),
  changed_at=(SELECT changed_at FROM latest WHERE inode_id=inode.id),
  deleted_at=NULL
WHERE inode.workspace_id=?1 AND EXISTS(
  SELECT 1 FROM latest WHERE inode_id=inode.id AND deleted_at IS NULL)
)SQL");
    restore_inodes.BindInt64(1, workspace.id);
    restore_inodes.BindInt64(2, commit);
    restore_inodes.Done();

    Statement restore_dentries(db, R"SQL(
WITH
ranked_inode AS MATERIALIZED (
  SELECT s.*,ROW_NUMBER() OVER(
    PARTITION BY s.inode_id ORDER BY s.commit_id DESC) AS vexfs_rank
  FROM _vexfs_inode_states s WHERE s.workspace_id=?1 AND s.commit_id<=?2),
latest_inode AS MATERIALIZED (SELECT * FROM ranked_inode WHERE vexfs_rank=1),
active_inode AS MATERIALIZED (SELECT * FROM latest_inode WHERE deleted_at IS NULL),
ranked_dentry AS MATERIALIZED (
  SELECT s.*,ROW_NUMBER() OVER(
    PARTITION BY s.parent_inode,s.name ORDER BY s.commit_id DESC) AS vexfs_rank
  FROM _vexfs_dentry_states s WHERE s.workspace_id=?1 AND s.commit_id<=?2),
latest_dentry AS MATERIALIZED (SELECT * FROM ranked_dentry WHERE vexfs_rank=1)
INSERT INTO _vexfs_dentries(workspace_id,parent_inode,name,inode_id)
SELECT d.workspace_id,d.parent_inode,d.name,d.inode_id FROM latest_dentry d
JOIN active_inode child ON child.inode_id=d.inode_id
JOIN active_inode parent ON parent.inode_id=d.parent_inode
WHERE d.deleted=0
)SQL");
    restore_dentries.BindInt64(1, workspace.id);
    restore_dentries.BindInt64(2, commit);
    restore_dentries.Done();

    Statement restore_xattrs(db, R"SQL(
WITH
ranked_inode AS MATERIALIZED (
  SELECT s.*,ROW_NUMBER() OVER(
    PARTITION BY s.inode_id ORDER BY s.commit_id DESC) AS vexfs_rank
  FROM _vexfs_inode_states s WHERE s.workspace_id=?1 AND s.commit_id<=?2),
latest_inode AS MATERIALIZED (SELECT * FROM ranked_inode WHERE vexfs_rank=1),
active_inode AS MATERIALIZED (SELECT * FROM latest_inode WHERE deleted_at IS NULL),
ranked_xattr AS MATERIALIZED (
  SELECT s.*,ROW_NUMBER() OVER(
    PARTITION BY s.inode_id,s.name ORDER BY s.commit_id DESC) AS vexfs_rank
  FROM _vexfs_xattr_states s WHERE s.workspace_id=?1 AND s.commit_id<=?2),
latest_xattr AS MATERIALIZED (SELECT * FROM ranked_xattr WHERE vexfs_rank=1)
INSERT INTO _vexfs_xattrs(inode_id,name,value)
SELECT x.inode_id,x.name,x.value FROM latest_xattr x
JOIN active_inode inode ON inode.inode_id=x.inode_id
WHERE x.deleted=0
)SQL");
    restore_xattrs.BindInt64(1, workspace.id);
    restore_xattrs.BindInt64(2, commit);
    restore_xattrs.Done();

    Statement restore_acl(db, R"SQL(
WITH
ranked_inode AS MATERIALIZED (
  SELECT s.*,ROW_NUMBER() OVER(
    PARTITION BY s.inode_id ORDER BY s.commit_id DESC) AS vexfs_rank
  FROM _vexfs_inode_states s WHERE s.workspace_id=?1 AND s.commit_id<=?2),
active_inode AS MATERIALIZED (SELECT * FROM ranked_inode WHERE vexfs_rank=1 AND deleted_at IS NULL),
ranked_acl AS MATERIALIZED (
  SELECT s.*,ROW_NUMBER() OVER(
    PARTITION BY s.inode_id,s.principal_id,s.effect ORDER BY s.commit_id DESC) AS vexfs_rank
  FROM _vexfs_acl_states s WHERE s.workspace_id=?1 AND s.commit_id<=?2),
latest_acl AS MATERIALIZED (SELECT * FROM ranked_acl WHERE vexfs_rank=1)
INSERT INTO _vexfs_acl_entries(workspace_id,inode_id,principal_id,effect,permissions,inherit_flags)
SELECT ?1,a.inode_id,a.principal_id,a.effect,a.permissions,a.inherit_flags
FROM latest_acl a JOIN active_inode inode ON inode.inode_id=a.inode_id
WHERE a.deleted=0
)SQL");
    restore_acl.BindInt64(1, workspace.id);
    restore_acl.BindInt64(2, commit);
    restore_acl.Done();
}

void PrepareRestoredVersionAliases(sqlite3 *db, const Workspace &workspace,
                                   sqlite3_int64 target_commit) {
    Statement update(db, R"SQL(
WITH
ranked AS MATERIALIZED (
  SELECT s.*,ROW_NUMBER() OVER(
    PARTITION BY s.inode_id ORDER BY s.commit_id DESC) AS vexfs_rank
  FROM _vexfs_inode_states s WHERE s.workspace_id=?1 AND s.commit_id<=?2),
latest AS MATERIALIZED (SELECT * FROM ranked WHERE vexfs_rank=1),
maximum AS (
  SELECT inode_id,MAX(version_no) AS version_no FROM _vexfs_file_versions GROUP BY inode_id)
UPDATE _vexfs_inodes AS inode SET
  current_version=(SELECT maximum.version_no+1 FROM maximum WHERE maximum.inode_id=inode.id)
WHERE inode.workspace_id=?1 AND inode.deleted_at IS NULL
  AND inode.kind IN ('file','symlink')
  AND EXISTS(
    SELECT 1 FROM latest JOIN maximum ON maximum.inode_id=latest.inode_id
    WHERE latest.inode_id=inode.id AND latest.deleted_at IS NULL
      AND maximum.version_no>latest.current_version)
)SQL");
    update.BindInt64(1, workspace.id);
    update.BindInt64(2, target_commit);
    update.Done();
}

void StoreRestoredVersionAliases(sqlite3 *db, const Workspace &workspace,
                                 sqlite3_int64 target_commit, sqlite3_int64 restore_commit) {
    Statement insert(db, R"SQL(
WITH ranked AS MATERIALIZED (
  SELECT s.*,ROW_NUMBER() OVER(
    PARTITION BY s.inode_id ORDER BY s.commit_id DESC) AS vexfs_rank
  FROM _vexfs_inode_states s WHERE s.workspace_id=?1 AND s.commit_id<=?2),
latest AS MATERIALIZED (SELECT * FROM ranked WHERE vexfs_rank=1)
INSERT INTO _vexfs_file_versions(
 inode_id,version_no,commit_id,manifest_id,size,checksum,source_version_no)
SELECT inode.id,inode.current_version,?3,NULL,target.size,source.checksum,
       COALESCE(source.source_version_no,source.version_no)
FROM _vexfs_inodes inode
JOIN latest target ON target.inode_id=inode.id AND target.deleted_at IS NULL
JOIN _vexfs_file_versions source
  ON source.inode_id=target.inode_id AND source.version_no=target.current_version
WHERE inode.workspace_id=?1 AND inode.deleted_at IS NULL
  AND inode.kind IN ('file','symlink')
  AND inode.current_version<>target.current_version
)SQL");
    insert.BindInt64(1, workspace.id);
    insert.BindInt64(2, target_commit);
    insert.BindInt64(3, restore_commit);
    insert.Done();
}

void SnapshotRestoreFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        Workspace workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        const std::string name = RequiredText(values[1], "snapshot name");
        const sqlite3_int64 expected_head = RequiredPositiveInteger(values[2], "expected head");
        Savepoint savepoint(db, "vexfs_snapshot_restore");
        AcquireWriteLock(db);
        workspace = FindWorkspace(db, RequiredText(values[0], "workspace"));
        if (workspace.head_commit != expected_head) {
            throw SqlError("workspace head conflict: expected " + std::to_string(expected_head) +
                           ", current " + std::to_string(workspace.head_commit),
                           SQLITE_CONSTRAINT);
        }
        Statement active_mount(db,
            "SELECT 1 FROM _vexfs_mount_sessions WHERE workspace_id=?1 "
            "AND lease_until>CAST(strftime('%s','now') AS INTEGER)*1000 LIMIT 1");
        active_mount.BindInt64(1, workspace.id);
        if (active_mount.Row()) {
            throw SqlError(
                "workspace has an active mount session; unmount before snapshot restore",
                SQLITE_BUSY);
        }
        const SnapshotInfo snapshot = FindSnapshot(db, workspace, name);
        Statement handles(db,
            "SELECT 1 FROM _vexfs_handles WHERE workspace_id=?1 "
            "AND state IN ('open','retained') LIMIT 1");
        handles.BindInt64(1, workspace.id);
        if (handles.Row()) {
            throw SqlError("workspace has open or retained file handles", SQLITE_BUSY);
        }
        const SnapshotTree current = CurrentTree(db, workspace);
        const SnapshotTree target = TreeAtCommit(db, workspace, snapshot.commit);
        if (SnapshotDiffJson(current, target) == "[]") {
            throw SqlError("snapshot already matches current workspace", SQLITE_MISMATCH);
        }
        CheckQuotaForSnapshotRestore(db, workspace, current, target);
        RestoreWorkspaceTree(db, workspace, snapshot.commit);
        PrepareRestoredVersionAliases(db, workspace, snapshot.commit);
        const sqlite3_int64 commit = CreateCommit(db, workspace, "restore snapshot " + name);
        StoreRestoredVersionAliases(db, workspace, snapshot.commit, commit);
        RefreshGrepIndexAfterTreeChange(db);
        savepoint.Release();
        sqlite3_result_int64(context, commit);
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
    RemoveDeletedFilesFromGrepIndex(db, workspace_id);
}

void TombstoneIfUnreferenced(sqlite3 *db, sqlite3_int64 workspace_id,
                             sqlite3_int64 inode) {
    Statement tombstone(db, R"SQL(
UPDATE _vexfs_inodes SET deleted_at=CAST(strftime('%s','now') AS INTEGER)*1000
WHERE workspace_id=?1 AND id=?2 AND kind<>'directory'
  AND NOT EXISTS(
    SELECT 1 FROM _vexfs_dentries
    WHERE workspace_id=?1 AND inode_id=?2)
)SQL");
    tombstone.BindInt64(1, workspace_id);
    tombstone.BindInt64(2, inode);
    tombstone.Done();
    RemoveDeletedFilesFromGrepIndex(db, workspace_id);
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
                throw SqlError("destination directory is not empty",
                               VEXFS_SQLITE_CONSTRAINT_NOT_EMPTY);
            }
            Statement detach(db,
                "DELETE FROM _vexfs_dentries WHERE workspace_id=?1 AND parent_inode=?2 AND name=?3");
            detach.BindInt64(1, workspace.id);
            detach.BindInt64(2, destination_parent.id);
            detach.BindText(3, destination_name);
            detach.Done();
            if (existing.kind == "directory") {
                TombstoneTree(db, workspace.id, existing.id);
            } else {
                TombstoneIfUnreferenced(db, workspace.id, existing.id);
            }
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
            throw SqlError("directory is not empty", VEXFS_SQLITE_CONSTRAINT_NOT_EMPTY);
        }
        Statement detach(db,
            "DELETE FROM _vexfs_dentries WHERE workspace_id=?1 AND parent_inode=?2 AND name=?3");
        detach.BindInt64(1, workspace.id);
        detach.BindInt64(2, parent.id);
        detach.BindText(3, name);
        detach.Done();
        if (node.kind == "directory") {
            TombstoneTree(db, workspace.id, node.id);
        } else {
            TombstoneIfUnreferenced(db, workspace.id, node.id);
        }
        TouchDirectory(db, workspace.id, parent.id);
        CreateCommit(db, workspace, "remove");
        sqlite3_result_int(context, 1);
    });
}

void HandleCreateFunction(sqlite3_context *context, int argument_count,
                          sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string workspace_name = RequiredText(values[0], "workspace");
        const std::string path = RequiredText(values[1], "path");
        const int mode = RequiredMode(values[2]);
        const bool owned_create = argument_count == 7;
        const sqlite3_int64 uid = owned_create
            ? RequiredOwnerId(values[3], "uid") : 0;
        const sqlite3_int64 gid = owned_create
            ? RequiredOwnerId(values[4], "gid") : 0;
        const int request_index = owned_create ? 5 : 3;
        const int owner_session_index = owned_create ? 6 : 4;
        const std::string request_id = RequiredText(values[request_index], "request_id");
        const std::string owner_session = argument_count == 5 || owned_create
            ? RequiredText(values[owner_session_index], "owner_session") : std::string();
        RequestFingerprint fingerprint_builder("handle_create");
        fingerprint_builder.AddText(workspace_name);
        fingerprint_builder.AddText(path);
        fingerprint_builder.AddInt64(mode);
        fingerprint_builder.AddInt64(uid);
        fingerprint_builder.AddInt64(gid);
        fingerprint_builder.AddText(owner_session);
        const auto fingerprint = fingerprint_builder.Finish();
        const CachedRequest cached = FindRequest(db, request_id, "handle_create", fingerprint);
        if (cached.kind == CachedKind::kText) {
            sqlite3_result_text(context, cached.text.data(),
                                static_cast<int>(cached.text.size()), SQLITE_TRANSIENT);
            return;
        }

        const Workspace workspace = FindWorkspace(db, workspace_name);
        const auto parts = PathParts(path);
        if (parts.empty()) throw SqlError("workspace root already exists", SQLITE_CONSTRAINT);
        Savepoint savepoint(db, "vexfs_handle_create");
        AcquireWriteLock(db);
        auto [parent, name] = ResolveParent(db, workspace, parts);
        Node existing;
        if (FindChild(db, workspace.id, parent.id, name, &existing)) {
            throw SqlError("destination already exists", SQLITE_CONSTRAINT);
        }
        const sqlite3_int64 inode = CreateInode(db, workspace.id, "file", mode);
        if (owned_create) {
            Statement owner(db,
                "UPDATE _vexfs_inodes SET uid=?1,gid=?2 WHERE workspace_id=?3 AND id=?4");
            owner.BindInt64(1, uid);
            owner.BindInt64(2, gid);
            owner.BindInt64(3, workspace.id);
            owner.BindInt64(4, inode);
            owner.Done();
        }
        AddDentry(db, workspace.id, parent.id, name, inode);

        const std::string handle_id = RandomHandleId();
        Statement insert(db,
            "INSERT INTO _vexfs_handles(id, workspace_id, inode_id, open_flags, writable, "
            "expected_version, dirty_generation, state, owner_session) "
            "VALUES(?1,?2,?3,'rw',1,0,1,'open',?4)");
        insert.BindText(1, handle_id);
        insert.BindInt64(2, workspace.id);
        insert.BindInt64(3, inode);
        if (argument_count == 5 || owned_create) insert.BindText(4, owner_session);
        else insert.BindNull(4);
        insert.Done();

        Handle handle{handle_id, workspace.id, inode, true, 0, 1, 0,
                      "open", owner_session};
        CreateStaging(db, handle, {});
        StoreRequestText(db, request_id, "handle_create", fingerprint, handle_id);
        savepoint.Release();
        sqlite3_result_text(context, handle_id.data(),
                            static_cast<int>(handle_id.size()), SQLITE_TRANSIENT);
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
        if (truncate) CreateStaging(db, handle, {});
        else CreateStagingFromVersion(db, handle, node);
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
        const StagingInfo staging = FindStaging(db, handle);
        const sqlite3_int64 staged_size = std::max<sqlite3_int64>(
            staging.logical_size, offset + static_cast<sqlite3_int64>(patch.size()));
        CheckQuotaForContentSize(db, handle.workspace_id, handle.inode_id, staged_size);
        const sqlite3_int64 generation = StageWrite(db, handle, offset, patch);
        StoreRequestInteger(db, request_id, "handle_stage_write", fingerprint, generation);
        sqlite3_result_int64(context, generation);
    });
}

void HandleAppendFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string handle_id = RequiredText(values[0], "handle");
        const auto suffix = RequiredBlob(values[1], "content");
        const std::string request_id = RequiredText(values[2], "request_id");
        RequestFingerprint fingerprint_builder("handle_append");
        fingerprint_builder.AddText(handle_id);
        fingerprint_builder.AddBlob(suffix);
        const auto fingerprint = fingerprint_builder.Finish();
        const CachedRequest cached = FindRequest(db, request_id, "handle_append", fingerprint);
        if (cached.kind == CachedKind::kInteger) {
            sqlite3_result_int64(context, cached.integer);
            return;
        }
        Handle handle = FindHandle(db, handle_id);
        if (handle.state != "open") throw SqlError("handle is not open", SQLITE_NOTFOUND);
        if (!handle.writable) throw SqlError("handle is read-only", SQLITE_READONLY);
        if (handle.dirty_generation != handle.published_generation) {
            throw SqlError("append requires a clean handle", SQLITE_CONSTRAINT);
        }
        Workspace workspace = WorkspaceById(db, handle.workspace_id);
        Savepoint savepoint(db, "vexfs_handle_append");
        AcquireWriteLock(db);
        const Node node = NodeById(db, handle.inode_id, true);
        if (node.kind != "file") throw SqlError("inode is not a file", SQLITE_MISMATCH);
        if (node.size < 0 || node.size > kMaxStagedBytes ||
            suffix.size() > static_cast<size_t>(kMaxStagedBytes - node.size)) {
            throw SqlError("file is larger than the Phase 0 limit (128 MiB)", SQLITE_TOOBIG);
        }
        CheckQuotaForContentSize(
            db, workspace.id, node.id,
            node.size + static_cast<sqlite3_int64>(suffix.size()));
        // O_APPEND is serialized against the current inode version, not the version
        // that happened to be current when this handle was opened. Refresh this
        // handle's clean staging view so concurrent appenders merge instead of
        // reporting a normal write conflict.
        {
            Statement remove_data(db,
                "DELETE FROM _vexfs_staging_chunks WHERE handle_id=?1");
            remove_data.BindText(1, handle.id);
            remove_data.Done();
            Statement remove_meta(db,
                "DELETE FROM _vexfs_staging WHERE handle_id=?1");
            remove_meta.BindText(1, handle.id);
            remove_meta.Done();
        }
        handle.expected_version = node.version;
        CreateStagingFromVersion(db, handle, node);
        const sqlite3_int64 generation = StageWrite(db, handle, node.size, suffix);
        handle.dirty_generation = generation;
        const sqlite3_int64 version = PublishHandleInTransaction(
            db, std::move(handle), generation);
        StoreRequestInteger(db, request_id, "handle_append", fingerprint, version);
        savepoint.Release();
        sqlite3_result_int64(context, version);
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
        if (size < 0 || size > kMaxStagedBytes) {
            throw SqlError("truncate size is out of range", SQLITE_RANGE);
        }
        CheckQuotaForContentSize(db, handle.workspace_id, handle.inode_id, size);
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

void HandlePublishCloseFunction(sqlite3_context *context, int, sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const std::string handle_id = RequiredText(values[0], "handle");
        const sqlite3_int64 generation = sqlite3_value_int64(values[1]);
        const std::string durability = RequiredText(values[2], "durability");
        if (durability != "none" && durability != "data" && durability != "full") {
            throw SqlError("durability must be none, data, or full", SQLITE_MISMATCH);
        }
        Savepoint savepoint(db, "vexfs_handle_publish_close");
        const sqlite3_int64 version = PublishHandleInTransaction(
            db, FindHandle(db, handle_id), generation);
        Statement update(db,
            "UPDATE _vexfs_handles SET state='closed',lease_until=NULL,"
            "updated_at=CAST(strftime('%s','now') AS INTEGER)*1000 WHERE id=?1");
        update.BindText(1, handle_id);
        update.Done();
        Statement clear_data(db, "DELETE FROM _vexfs_staging_chunks WHERE handle_id=?1");
        clear_data.BindText(1, handle_id);
        clear_data.Done();
        Statement clear(db, "DELETE FROM _vexfs_staging WHERE handle_id=?1");
        clear.BindText(1, handle_id);
        clear.Done();
        savepoint.Release();
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
            Statement clear_data(db, "DELETE FROM _vexfs_staging_chunks WHERE handle_id=?1");
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
        "DELETE FROM _vexfs_staging_chunks WHERE handle_id IN "
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

void PublishCloseAllFunction(sqlite3_context *context, int argument_count,
                             sqlite3_value **values) {
    Guard(context, [&] {
        sqlite3 *db = sqlite3_context_db_handle(context);
        EnsureSchema(db);
        const Workspace workspace = FindWorkspace(
            db, RequiredText(values[0], "workspace"));
        const std::string owner_session = RequiredText(values[1], "owner_session");
        if (owner_session.empty()) {
            throw SqlError("owner_session must not be empty", SQLITE_MISMATCH);
        }
        const sqlite3_int64 limit = argument_count == 3
            ? sqlite3_value_int64(values[2]) : 0;
        if (limit < 0) {
            throw SqlError("limit must be zero or positive", SQLITE_RANGE);
        }
        Savepoint savepoint(db, "vexfs_publish_close_all");
        AcquireWriteLock(db);
        Statement query(db,
            "SELECT id,dirty_generation FROM _vexfs_handles "
            "WHERE workspace_id=?1 AND owner_session=?2 AND state='open' "
            "AND writable=1 AND dirty_generation>published_generation ORDER BY id "
            "LIMIT ?3");
        query.BindInt64(1, workspace.id);
        query.BindText(2, owner_session);
        query.BindInt64(3, limit == 0 ? -1 : limit);
        std::vector<std::pair<std::string, sqlite3_int64>> pending;
        while (query.Row()) pending.emplace_back(query.Text(0), query.Int64(1));
        for (const auto &item : pending) {
            // The batch already owns one rollback boundary. Avoid two extra
            // SAVEPOINT statements per file while keeping the whole batch
            // atomic if any publish fails.
            PublishHandleInTransaction(
                db, FindHandle(db, item.first), item.second, true);
            Statement close(db,
                "UPDATE _vexfs_handles SET state='closed',lease_until=NULL,"
                "updated_at=CAST(strftime('%s','now') AS INTEGER)*1000 WHERE id=?1");
            close.BindText(1, item.first);
            close.Done();
            Statement clear_data(db,
                "DELETE FROM _vexfs_staging_chunks WHERE handle_id=?1");
            clear_data.BindText(1, item.first);
            clear_data.Done();
            Statement clear(db, "DELETE FROM _vexfs_staging WHERE handle_id=?1");
            clear.BindText(1, item.first);
            clear.Done();
        }
        savepoint.Release();
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
            Statement clear_data(db, "DELETE FROM _vexfs_staging_chunks WHERE handle_id=?1");
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

void ConnectionGuardFunction(sqlite3_context *context, int, sqlite3_value **) {
    sqlite3_result_int(context, 1);
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
        {"vexfs_create", 4, CreateFunction, SQLITE_UTF8},
        {"vexfs_symlink", 3, SymlinkFunction, SQLITE_UTF8},
        {"vexfs_link", 3, HardlinkFunction, SQLITE_UTF8},
        {"vexfs_write", 3, WriteFunction, SQLITE_UTF8},
        {"vexfs_append", 3, AppendFunction, SQLITE_UTF8},
        {"vexfs_read", 2, ReadFunction, SQLITE_UTF8},
        {"vexfs_read_range", 4, ReadRangeFunction, SQLITE_UTF8},
        {"vexfs_grep", 5, GrepFunction, SQLITE_UTF8},
        {"vexfs_grep_index", 1, GrepIndexFunction, SQLITE_UTF8},
        {"vexfs_history", 2, HistoryFunction, SQLITE_UTF8},
        {"vexfs_history", 4, HistoryFunction, SQLITE_UTF8},
        {"vexfs_read_version", 3, ReadVersionFunction, SQLITE_UTF8},
        {"vexfs_compare_versions", 4, CompareVersionsFunction, SQLITE_UTF8},
        {"vexfs_restore_version", 4, RestoreVersionFunction, SQLITE_UTF8},
        {"vexfs_stat", 2, StatFunction, SQLITE_UTF8},
        {"vexfs_check", 2, CheckFunction, SQLITE_UTF8},
        {"vexfs_path", 2, PathFunction, SQLITE_UTF8},
        {"vexfs_list", 2, ListFunction, SQLITE_UTF8},
        {"vexfs_readlink", 2, ReadlinkFunction, SQLITE_UTF8},
        {"vexfs_set_mode", 3, SetModeFunction, SQLITE_UTF8},
        {"vexfs_set_times", 5, SetTimesFunction, SQLITE_UTF8},
        {"vexfs_chown", 4, ChownFunction, SQLITE_UTF8},
        {"vexfs_owner_set", 3, OwnerSetFunction, SQLITE_UTF8},
        {"vexfs_acl_get", 2, AclGetFunction, SQLITE_UTF8},
        {"vexfs_acl_list", 2, AclGetFunction, SQLITE_UTF8},
        {"vexfs_acl_set", 3, AclSetFunction, SQLITE_UTF8},
        {"vexfs_acl_delete", 2, AclDeleteFunction, SQLITE_UTF8},
        {"vexfs_acl_grant", 4, AclGrantFunction, SQLITE_UTF8},
        {"vexfs_acl_grant", 5, AclGrantFunction, SQLITE_UTF8},
        {"vexfs_acl_revoke", 3, AclRevokeFunction, SQLITE_UTF8},
        {"vexfs_acl_revoke", 4, AclRevokeFunction, SQLITE_UTF8},
        {"vexfs_xattr_get", 3, GetXattrFunction, SQLITE_UTF8},
        {"vexfs_xattr_list", 2, ListXattrsFunction, SQLITE_UTF8},
        {"vexfs_xattr_set", 5, SetXattrFunction, SQLITE_UTF8},
        {"vexfs_snapshot_create", 2, SnapshotCreateFunction, SQLITE_UTF8},
        {"vexfs_snapshot_create", 3, SnapshotCreateFunction, SQLITE_UTF8},
        {"vexfs_snapshot_create", 4, SnapshotCreateFunction, SQLITE_UTF8},
        {"vexfs_snapshot_list", 1, SnapshotListFunction, SQLITE_UTF8},
        {"vexfs_snapshot_show", 2, SnapshotShowFunction, SQLITE_UTF8},
        {"vexfs_snapshot_diff", 3, SnapshotDiffFunction, SQLITE_UTF8},
        {"vexfs_snapshot_drop", 2, SnapshotDropFunction, SQLITE_UTF8},
        {"vexfs_snapshot_restore", 3, SnapshotRestoreFunction, SQLITE_UTF8},
        {"vexfs_quota_get", 1, QuotaGetFunction, SQLITE_UTF8},
        {"vexfs_quota_set", 4, QuotaSetFunction, SQLITE_UTF8},
        {"vexfs_retention_get", 1, RetentionGetFunction, SQLITE_UTF8},
        {"vexfs_retention_set", 3, RetentionSetFunction, SQLITE_UTF8},
        {"vexfs_gc_pause", 2, GcPauseFunction, SQLITE_UTF8},
        {"vexfs_gc", 2, GcFunction, SQLITE_UTF8},
        {"vexfs_move", 3, RenameFunction, SQLITE_UTF8},
        {"vexfs_rename", 4, RenameFunction, SQLITE_UTF8},
        {"vexfs_remove", 3, RemoveFunction, SQLITE_UTF8},
        {"vexfs_handle_create", 4, HandleCreateFunction, SQLITE_UTF8},
        {"vexfs_handle_create", 5, HandleCreateFunction, SQLITE_UTF8},
        {"vexfs_handle_create", 7, HandleCreateFunction, SQLITE_UTF8},
        {"vexfs_handle_open", 4, HandleOpenFunction, SQLITE_UTF8},
        {"vexfs_handle_open", 5, HandleOpenFunction, SQLITE_UTF8},
        {"vexfs_handle_stage_write", 4, HandleStageWriteFunction, SQLITE_UTF8},
        {"vexfs_handle_append", 3, HandleAppendFunction, SQLITE_UTF8},
        {"vexfs_handle_truncate", 3, HandleTruncateFunction, SQLITE_UTF8},
        {"vexfs_handle_read", 3, HandleReadFunction, SQLITE_UTF8},
        {"vexfs_handle_publish", 4, HandlePublishFunction, SQLITE_UTF8},
        {"vexfs_handle_publish_close", 3, HandlePublishCloseFunction, SQLITE_UTF8},
        {"vexfs_handle_close", 3, HandleCloseFunction, SQLITE_UTF8},
        {"vexfs_mount_session_start", 2, SessionStartFunction, SQLITE_UTF8},
        {"vexfs_mount_session_heartbeat", 2, SessionHeartbeatFunction, SQLITE_UTF8},
        {"vexfs_mount_session_end", 2, SessionEndFunction, SQLITE_UTF8},
        {"vexfs_mount_synchronize", 2, SynchronizeFunction, SQLITE_UTF8},
        {"vexfs_mount_synchronize", 3, SynchronizeFunction, SQLITE_UTF8},
        {"vexfs_mount_publish_close_all", 2, PublishCloseAllFunction, SQLITE_UTF8},
        {"vexfs_mount_publish_close_all", 3, PublishCloseAllFunction, SQLITE_UTF8},
        {"vexfs_item_reclaim", 2, ReclaimFunction, SQLITE_UTF8},
    };
    // sqlite3_create_function_v2 exists throughout the supported SQLite range.  Its
    // destructor removes the pointer-keyed schema cache before a connection address can
    // be reused; this avoids requiring sqlite3 client-data APIs added only in 3.44.
    int rc = sqlite3_create_function_v2(
        db, "_vexfs_connection_guard", 0, SQLITE_UTF8 | SQLITE_DETERMINISTIC, db,
        ConnectionGuardFunction, nullptr, nullptr, ForgetSchemaReady);
    if (rc != SQLITE_OK) return rc;
    for (const auto &definition : functions) {
        rc = sqlite3_create_function(db, definition.name, definition.arguments,
                                     definition.flags, nullptr, definition.function,
                                     nullptr, nullptr);
        if (rc != SQLITE_OK) return rc;
    }
    return SQLITE_OK;
}
