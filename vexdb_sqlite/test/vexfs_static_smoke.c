#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite3.h"
#include "vexdb_sqlite.h"

static int fail(sqlite3 *db, const char *what) {
    fprintf(stderr, "VEXFS SMOKE FAIL: %s: %s\n", what, sqlite3_errmsg(db));
    return 1;
}

static int exec_ok(sqlite3 *db, const char *sql) {
    char *message = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &message);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL failed: %s\n%s\n", sql, message ? message : sqlite3_errmsg(db));
        sqlite3_free(message);
        return 0;
    }
    return 1;
}

static int exec_fails(sqlite3 *db, const char *sql) {
    char *message = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &message);
    sqlite3_free(message);
    return rc != SQLITE_OK;
}

static int exec_fails_with(sqlite3 *db, const char *sql, const char *expected) {
    char *message = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &message);
    int matches = rc != SQLITE_OK && message != NULL && strstr(message, expected) != NULL;
    sqlite3_free(message);
    return matches;
}

static sqlite3_int64 scalar_int(sqlite3 *db, const char *sql, int *ok) {
    sqlite3_stmt *statement = NULL;
    *ok = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) return 0;
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return 0;
    }
    sqlite3_int64 result = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    *ok = 1;
    return result;
}

static char *scalar_text(sqlite3 *db, const char *sql) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK) return NULL;
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return NULL;
    }
    const unsigned char *value = sqlite3_column_text(statement, 0);
    int bytes = sqlite3_column_bytes(statement, 0);
    char *result = (char *)malloc((size_t)bytes + 1);
    if (result != NULL) {
        memcpy(result, value, (size_t)bytes);
        result[bytes] = '\0';
    }
    sqlite3_finalize(statement);
    return result;
}

int main(void) {
    sqlite3 *db = NULL;
    int ok = 0;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) return fail(db, "open");
    if (vexdb_sqlite_register(db) != SQLITE_OK) return fail(db, "register");

    if (!exec_ok(db, "SELECT vexfs_init(); SELECT vexfs_workspace_create('default');"))
        return fail(db, "initialize");
    if (!exec_ok(db, "SELECT vexfs_mkdir('default','/notes/2026');"))
        return fail(db, "mkdir");
    if (!exec_ok(db, "SELECT vexfs_write('default','/notes/2026/hello.txt',X'68656C6C6F');"))
        return fail(db, "write");

    char *text = scalar_text(db, "SELECT CAST(vexfs_read('default','/notes/2026/hello.txt') AS TEXT)");
    if (text == NULL || strcmp(text, "hello") != 0) return fail(db, "read after write");
    free(text);

    text = scalar_text(db,
        "SELECT vexfs_grep('default','/notes','ell',0,100)");
    if (text == NULL || strstr(text, "\"path\":\"/notes/2026/hello.txt\"") == NULL ||
        strstr(text, "\"line\":1") == NULL || strstr(text, "\"match_count\":1") == NULL)
        return fail(db, "database grep");
    free(text);
    text = scalar_text(db,
        "SELECT vexfs_grep('default','/notes','HEL',1,100)");
    if (text == NULL || strstr(text, "\"match_count\":1") == NULL)
        return fail(db, "database grep ignore case");
    free(text);
    text = scalar_text(db, "SELECT vexfs_grep_index('enable')");
    if (text == NULL || strstr(text, "\"enabled\":true") == NULL ||
        strstr(text, "\"available\":true") == NULL ||
        strstr(text, "\"backend\":\"fts5-trigram\"") == NULL)
        return fail(db, "enable database grep index");
    free(text);
    text = scalar_text(db,
        "SELECT vexfs_grep('default','/notes','ell',0,100)");
    if (text == NULL || strstr(text, "\"index_used\":true") == NULL ||
        strstr(text, "\"match_count\":1") == NULL)
        return fail(db, "indexed database grep");
    free(text);
    text = scalar_text(db,
        "SELECT vexfs_grep('default','/notes','el',0,100)");
    if (text == NULL || strstr(text, "\"index_used\":false") == NULL ||
        strstr(text, "\"match_count\":1") == NULL)
        return fail(db, "short database grep fallback");
    free(text);

    // SQL 事务回滚必须同时回滚文件内容。
    if (!exec_ok(db, "BEGIN; SELECT vexfs_write('default','/notes/2026/hello.txt','changed'); ROLLBACK;"))
        return fail(db, "transaction rollback");
    text = scalar_text(db, "SELECT CAST(vexfs_read('default','/notes/2026/hello.txt') AS TEXT)");
    if (text == NULL || strcmp(text, "hello") != 0) return fail(db, "rollback content");
    free(text);

    char *handle = scalar_text(db,
        "SELECT vexfs_handle_open('default','/notes/2026/hello.txt','rw','open-1')");
    if (handle == NULL || strlen(handle) != 32) return fail(db, "handle open");

    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT vexfs_handle_stage_write('%s',5,' world','write-1')", handle);
    sqlite3_int64 generation = scalar_int(db, sql, &ok);
    if (!ok || generation != 1) return fail(db, "stage write");
    // 同一个 request_id 重试，不得再生成一代。
    sqlite3_int64 repeated = scalar_int(db, sql, &ok);
    if (!ok || repeated != generation) return fail(db, "idempotent stage write");
    snprintf(sql, sizeof(sql),
        "SELECT vexfs_handle_stage_write('%s',4,'!', 'write-1')", handle);
    if (!exec_fails(db, sql)) return fail(db, "request id argument mismatch");

    snprintf(sql, sizeof(sql),
        "SELECT CAST(vexfs_handle_read('%s',0,64) AS TEXT)", handle);
    text = scalar_text(db, sql);
    if (text == NULL || strcmp(text, "hello world") != 0) return fail(db, "read staged data");
    free(text);

    snprintf(sql, sizeof(sql),
        "SELECT vexfs_handle_publish('%s',1,'data','publish-1')", handle);
    sqlite3_int64 version = scalar_int(db, sql, &ok);
    if (!ok || version != 2) return fail(db, "publish");
    snprintf(sql, sizeof(sql),
        "SELECT vexfs_handle_close('%s',1,'close-1')", handle);
    text = scalar_text(db, sql);
    if (text == NULL || strcmp(text, "closed") != 0) return fail(db, "close");
    free(text);
    free(handle);

    // 暂存元数据每个 handle 只有一行；稀疏写只保存碰到的 64 KiB 块，并补零空洞。
    if (!exec_ok(db, "SELECT vexfs_write('default','/notes/grow.bin',X'');"))
        return fail(db, "grow seed");
    handle = scalar_text(db,
        "SELECT vexfs_handle_open('default','/notes/grow.bin','rw','open-grow-1')");
    if (handle == NULL) return fail(db, "grow open");
    snprintf(sql, sizeof(sql),
        "SELECT vexfs_handle_stage_write('%s',70000,X'5A','write-grow-1')", handle);
    generation = scalar_int(db, sql, &ok);
    if (!ok || generation != 1) return fail(db, "grow stage");
    snprintf(sql, sizeof(sql),
        "SELECT count(*) FROM _vexfs_staging WHERE handle_id='%s'", handle);
    if (scalar_int(db, sql, &ok) != 1 || !ok) return fail(db, "bounded staging rows");
    snprintf(sql, sizeof(sql),
        "SELECT capacity FROM _vexfs_staging WHERE handle_id='%s'", handle);
    if (scalar_int(db, sql, &ok) != 4465 || !ok) return fail(db, "dirty chunk capacity");
    snprintf(sql, sizeof(sql),
        "SELECT hex(vexfs_handle_read('%s',69999,2))", handle);
    text = scalar_text(db, sql);
    if (text == NULL || strcmp(text, "005A") != 0) return fail(db, "sparse staging read");
    free(text);
    snprintf(sql, sizeof(sql),
        "SELECT vexfs_handle_truncate('%s',5,'truncate-grow-1')", handle);
    generation = scalar_int(db, sql, &ok);
    if (!ok || generation != 2) return fail(db, "handle truncate");
    snprintf(sql, sizeof(sql),
        "SELECT length(vexfs_handle_read('%s',0,100))", handle);
    if (scalar_int(db, sql, &ok) != 5 || !ok) return fail(db, "truncated staged size");
    snprintf(sql, sizeof(sql),
        "SELECT vexfs_handle_truncate('%s',70001,'regrow-grow-1')", handle);
    generation = scalar_int(db, sql, &ok);
    if (!ok || generation != 3) return fail(db, "handle regrow");
    snprintf(sql, sizeof(sql),
        "SELECT hex(vexfs_handle_read('%s',70000,1))", handle);
    text = scalar_text(db, sql);
    if (text == NULL || strcmp(text, "00") != 0) return fail(db, "truncate must not resurrect data");
    free(text);
    snprintf(sql, sizeof(sql),
        "SELECT vexfs_handle_truncate('%s',5,'truncate-grow-2')", handle);
    generation = scalar_int(db, sql, &ok);
    if (!ok || generation != 4) return fail(db, "handle final truncate");
    snprintf(sql, sizeof(sql),
        "SELECT vexfs_handle_publish('%s',4,'data','publish-grow-1');"
        "SELECT vexfs_handle_close('%s',1,'close-grow-1')", handle, handle);
    if (!exec_ok(db, sql)) return fail(db, "publish truncated file");
    free(handle);

    // replace rename 是单个数据库操作，inode 路径会随父目录重命名实时更新。
    if (!exec_ok(db,
        "SELECT vexfs_mkdir('default','/rename/source/child');"
        "SELECT vexfs_write('default','/rename/source/child/value.txt','source');"
        "SELECT vexfs_write('default','/rename/source/child/live.txt','live');"
        "SELECT vexfs_write('default','/rename/destination.txt','destination');"))
        return fail(db, "rename seed");
    sqlite3_int64 inode = scalar_int(db,
        "SELECT json_extract(vexfs_stat('default','/rename/source/child/live.txt'),'$.inode')", &ok);
    if (!ok || inode == 0) return fail(db, "rename inode");
    if (!exec_ok(db,
        "SELECT vexfs_rename('default','/rename/source/child/value.txt',"
        "'/rename/destination.txt',1);"
        "SELECT vexfs_rename('default','/rename/source','/rename/moved',0);"))
        return fail(db, "atomic rename");
    snprintf(sql, sizeof(sql), "SELECT vexfs_path('default',%lld)", (long long)inode);
    text = scalar_text(db, sql);
    if (text == NULL || strcmp(text, "/rename/moved/child/live.txt") != 0) {
        fprintf(stderr, "resolved inode path: %s\n", text == NULL ? "(null)" : text);
        return fail(db, "inode path after ancestor rename");
    }
    free(text);
    text = scalar_text(db,
        "SELECT CAST(vexfs_read('default','/rename/destination.txt') AS TEXT)");
    if (text == NULL || strcmp(text, "source") != 0) return fail(db, "rename replacement content");
    free(text);

    text = scalar_text(db, "SELECT CAST(vexfs_read('default','/notes/2026/hello.txt') AS TEXT)");
    if (text == NULL || strcmp(text, "hello world") != 0) return fail(db, "read published data");
    free(text);

    text = scalar_text(db, "SELECT vexfs_list('default','/notes/2026')");
    if (text == NULL || strstr(text, "hello.txt") == NULL) return fail(db, "list");
    free(text);

    text = scalar_text(db, "SELECT vexfs_history('default','/notes/2026/hello.txt')");
    if (text == NULL || strstr(text, "\"version\":2") == NULL ||
        strstr(text, "\"version\":1") == NULL) return fail(db, "history");
    free(text);
    text = scalar_text(db,
        "SELECT vexfs_history('default','/notes/2026/hello.txt',1,0)");
    if (text == NULL || strstr(text, "\"entries\":[{\"version\":2") == NULL ||
        strstr(text, "\"next_before\":2") == NULL) return fail(db, "paged history");
    free(text);
    text = scalar_text(db,
        "SELECT CAST(vexfs_read_version('default','/notes/2026/hello.txt',1) AS TEXT)");
    if (text == NULL || strcmp(text, "hello") != 0) return fail(db, "read version");
    free(text);
    version = scalar_int(db,
        "SELECT vexfs_restore_version('default','/notes/2026/hello.txt',1,2)", &ok);
    if (!ok || version != 3) return fail(db, "restore version");
    text = scalar_text(db,
        "SELECT CAST(vexfs_read('default','/notes/2026/hello.txt') AS TEXT)");
    if (text == NULL || strcmp(text, "hello") != 0) return fail(db, "restored content");
    free(text);
    version = scalar_int(db,
        "SELECT source_version_no FROM _vexfs_file_versions "
        "WHERE version_no=3 ORDER BY id DESC LIMIT 1", &ok);
    if (!ok || version != 1) return fail(db, "restored content reference");
    if (!exec_fails(db,
        "SELECT vexfs_restore_version('default','/notes/2026/hello.txt',2,2)"))
        return fail(db, "stale restore conflict");

    if (!exec_ok(db,
        "SELECT vexfs_move('default','/notes/2026/hello.txt','/notes/hello.txt');"
        "SELECT vexfs_remove('default','/notes/2026',0);"))
        return fail(db, "move and remove");

    // POSIX mode 与符号链接必须通过静态注册入口工作。
    inode = scalar_int(db,
        "SELECT vexfs_create('default','/notes/run.sh','file',488)", &ok);
    if (!ok || inode == 0) return fail(db, "create file with mode");
    snprintf(sql, sizeof(sql), "SELECT vexfs_set_mode('default',%lld,493)",
             (long long)inode);
    if (scalar_int(db, sql, &ok) != 493 || !ok) return fail(db, "set executable mode");
    if (scalar_int(db,
        "SELECT json_extract(vexfs_stat('default','/notes/run.sh'),'$.mode')", &ok) != 493 ||
        !ok) return fail(db, "persist executable mode");
    sqlite3_int64 link_inode = scalar_int(db,
        "SELECT vexfs_symlink('default','/notes/run-link','run.sh')", &ok);
    if (!ok || link_inode == 0) return fail(db, "create symlink");
    snprintf(sql, sizeof(sql), "SELECT CAST(vexfs_readlink('default',%lld) AS TEXT)",
             (long long)link_inode);
    text = scalar_text(db, sql);
    if (text == NULL || strcmp(text, "run.sh") != 0) return fail(db, "read symlink");
    free(text);

    if (!exec_ok(db,
        "SELECT vexfs_xattr_set('default',"
        "json_extract(vexfs_stat('default','/notes/run.sh'),'$.inode'),"
        "'user.snapshot','old',0);"
        "SELECT vexfs_snapshot_create('default','before-tree');"
        "SELECT vexfs_write('default','/notes/run.sh','changed');"
        "SELECT vexfs_set_mode('default',"
        "json_extract(vexfs_stat('default','/notes/run.sh'),'$.inode'),420);"
        "SELECT vexfs_rename('default','/notes/run.sh','/notes/renamed.sh',0);"
        "SELECT vexfs_remove('default','/notes/run-link',0);"
        "SELECT vexfs_xattr_set('default',"
        "json_extract(vexfs_stat('default','/notes/renamed.sh'),'$.inode'),"
        "'user.snapshot','new',0);")) return fail(db, "snapshot mutations");
    text = scalar_text(db,
        "SELECT vexfs_snapshot_diff('default','before-tree','HEAD')");
    if (text == NULL || strstr(text, "renamed.sh") == NULL ||
        strstr(text, "run-link") == NULL) return fail(db, "snapshot diff");
    free(text);
    version = scalar_int(db,
        "SELECT vexfs_snapshot_restore('default','before-tree',"
        "(SELECT head_commit FROM _vexfs_workspaces WHERE name='default'))", &ok);
    if (!ok || version <= 0) return fail(db, "snapshot restore");
    if (scalar_int(db,
        "SELECT json_extract(vexfs_stat('default','/notes/run.sh'),'$.mode')", &ok) != 493 ||
        !ok) return fail(db, "snapshot restored mode");
    if (!exec_fails(db, "SELECT vexfs_stat('default','/notes/renamed.sh')"))
        return fail(db, "snapshot removed later rename");
    snprintf(sql, sizeof(sql), "SELECT CAST(vexfs_readlink('default',%lld) AS TEXT)",
             (long long)link_inode);
    text = scalar_text(db, sql);
    if (text == NULL || strcmp(text, "run.sh") != 0)
        return fail(db, "snapshot restored symlink");
    free(text);
    text = scalar_text(db,
        "SELECT CAST(vexfs_xattr_get('default',"
        "json_extract(vexfs_stat('default','/notes/run.sh'),'$.inode'),"
        "'user.snapshot') AS TEXT)");
    if (text == NULL || strcmp(text, "old") != 0) return fail(db, "snapshot restored xattr");
    free(text);
    text = scalar_text(db, "SELECT vexfs_snapshot_list('default')");
    if (text == NULL || strstr(text, "before-tree") == NULL) return fail(db, "snapshot list");
    free(text);

    scalar_int(db, "SELECT vexfs_item_reclaim('default','reclaim-1')", &ok);
    if (!ok) return fail(db, "reclaim");

    sqlite3_close(db);

    // 尚未正式发版，不保留旧 schema 迁移。版本不匹配必须明确拒绝且不能改库。
    db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) return fail(db, "version mismatch open");
    if (vexdb_sqlite_register(db) != SQLITE_OK) return fail(db, "version mismatch register");
    if (!exec_ok(db,
        "CREATE TABLE _vexfs_meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "INSERT INTO _vexfs_meta VALUES('contract_version','0.1.0');"
        "CREATE TABLE legacy_marker(value TEXT NOT NULL);"
        "INSERT INTO legacy_marker VALUES('unchanged');"))
        return fail(db, "version mismatch fixture");
    if (!exec_fails_with(db, "SELECT vexfs_init();", "unsupported VexFS schema version: 0.1.0"))
        return fail(db, "old schema must be rejected");
    text = scalar_text(db, "SELECT value FROM _vexfs_meta WHERE key='contract_version'");
    if (text == NULL || strcmp(text, "0.1.0") != 0) return fail(db, "old schema changed");
    free(text);
    text = scalar_text(db, "SELECT value FROM legacy_marker");
    if (text == NULL || strcmp(text, "unchanged") != 0) return fail(db, "old data changed");
    free(text);
    if (scalar_int(db, "SELECT count(*) FROM sqlite_master WHERE name LIKE '_vexfs_%'", &ok) != 1 ||
        !ok) return fail(db, "old schema was partially initialized");
    sqlite3_close(db);
    printf("VEXFS STATIC SMOKE: PASS\n");
    return 0;
}
