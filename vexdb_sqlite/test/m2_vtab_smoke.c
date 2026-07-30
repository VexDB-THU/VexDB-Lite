// M2 虚拟表冒烟测试（静态注册形态）。
//
// 验收覆盖（计划 M2）：建表参数解析、INSERT（BLOB/JSON/自动 rowid）、暴力 KNN
// 正确性（最近邻 + distance 值 + 升序）、DELETE/UPDATE、**事务回滚**（shadow
// 表随宿主事务）、**关库重开**（config 恢复 + 数据在）、错误路径。
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define unlink _unlink
#else
#include <unistd.h>
#endif
#include "sqlite3.h"
#include "vexdb_sqlite.h"

static int g_fail = 0;

static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "M2 FAIL: %s\n", what);
        g_fail = 1;
    }
}

static int exec_ok(sqlite3 *db, const char *sql, const char *what) {
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "M2 FAIL: %s: rc=%d %s\n", what, rc, errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        g_fail = 1;
        return 0;
    }
    return 1;
}

static void expect_error(sqlite3 *db, const char *sql, const char *what) {
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    sqlite3_free(errmsg);
    check(rc != SQLITE_OK, what);
}

// 跑 KNN 查询，校验返回的 (rowid, distance) 序列。
static void expect_knn(sqlite3 *db, const char *table, const char *query, int k,
                       const sqlite3_int64 *want_ids, const double *want_dists,
                       int want_n, const char *what) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT rowid, distance FROM %s WHERE embedding MATCH '%s' AND k = %d",
             table, query, k);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "M2 FAIL: %s: prepare: %s\n", what, sqlite3_errmsg(db));
        g_fail = 1;
        return;
    }
    int n = 0, ok = 1;
    double prev = -1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(st, 0);
        double d = sqlite3_column_double(st, 1);
        if (n < want_n) {
            if (want_ids && id != want_ids[n]) {
                fprintf(stderr, "  row %d: rowid=%lld want %lld\n", n, (long long)id,
                        (long long)want_ids[n]);
                ok = 0;
            }
            if (want_dists && fabs(d - want_dists[n]) > 1e-5) {
                fprintf(stderr, "  row %d: dist=%.8f want %.8f\n", n, d, want_dists[n]);
                ok = 0;
            }
        }
        if (d < prev) { fprintf(stderr, "  row %d: distance not ascending\n", n); ok = 0; }
        prev = d;
        n++;
    }
    sqlite3_finalize(st);
    if (n != want_n) { fprintf(stderr, "  rows=%d want %d\n", n, want_n); ok = 0; }
    check(ok, what);
}

static sqlite3_int64 count_rows(sqlite3 *db, const char *table) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s", table);
    sqlite3_stmt *st = NULL;
    sqlite3_int64 n = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

int main(void) {
    const char *tmpdir = getenv("VEXDB_SQLITE_TEST_TMPDIR");
    if (!tmpdir || !tmpdir[0]) tmpdir = getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0]) tmpdir = "/tmp";
    char dbpath[512];
    snprintf(dbpath, sizeof(dbpath), "%s/vexdb_m2_smoke.db", tmpdir);
    unlink(dbpath);
    sqlite3 *db = NULL;
    if (sqlite3_open(dbpath, &db) != SQLITE_OK) return 1;
    if (vexdb_sqlite_register(db) != SQLITE_OK) return 1;

    // ── 建表 + 写入 ──
    exec_ok(db,
            "CREATE VIRTUAL TABLE idx USING GRAPH_INDEX(embedding FLOAT[3], metric=l2, m=8)",
            "create vtab");
    exec_ok(db, "INSERT INTO idx(rowid, embedding) VALUES (1, '[0,0,0]')", "insert json 1");
    exec_ok(db, "INSERT INTO idx(rowid, embedding) VALUES (2, '[3,4,0]')", "insert json 2");
    exec_ok(db, "INSERT INTO idx(rowid, embedding) VALUES (3, vexdb_f32('[1,1,1]'))",
            "insert blob 3");
    exec_ok(db, "INSERT INTO idx(embedding) VALUES ('[10,10,10]')", "insert auto rowid");
    check(count_rows(db, "idx") == 4, "4 rows after insert");

    // ── KNN 正确性：query=[0,0,0] → 1(0), 3(sqrt3), 2(5) ──
    {
        sqlite3_int64 ids[] = {1, 3, 2};
        double dists[] = {0.0, sqrt(3.0), 5.0};
        expect_knn(db, "idx", "[0,0,0]", 3, ids, dists, 3, "knn l2 top3");
    }
    // k 大于行数 → 返回全部 4 行
    expect_knn(db, "idx", "[0,0,0]", 100, NULL, NULL, 4, "knn k>rows");

    // ── DELETE / UPDATE ──
    exec_ok(db, "DELETE FROM idx WHERE rowid = 3", "delete rowid 3");
    {
        sqlite3_int64 ids[] = {1, 2};
        expect_knn(db, "idx", "[0,0,0]", 2, ids, NULL, 2, "knn after delete");
    }
    exec_ok(db, "UPDATE idx SET embedding = '[0,0,1]' WHERE rowid = 2", "update vector");
    {
        sqlite3_int64 ids[] = {1, 2};
        double dists[] = {0.0, 1.0};
        expect_knn(db, "idx", "[0,0,0]", 2, ids, dists, 2, "knn after update");
    }

    // ── 事务回滚：shadow 表随宿主事务 ──
    exec_ok(db, "BEGIN", "begin");
    exec_ok(db, "INSERT INTO idx(rowid, embedding) VALUES (50, '[9,9,9]')", "insert in txn");
    check(count_rows(db, "idx") == 4, "row visible in txn");
    exec_ok(db, "ROLLBACK", "rollback");
    check(count_rows(db, "idx") == 3, "rollback removes row");

    // ── cosine metric 第二张表 ──
    exec_ok(db,
            "CREATE VIRTUAL TABLE idxc USING GRAPH_INDEX(embedding FLOAT[2], metric=cosine)",
            "create cosine vtab");
    exec_ok(db, "INSERT INTO idxc(rowid, embedding) VALUES (1, '[1,0]')", "cos insert 1");
    exec_ok(db, "INSERT INTO idxc(rowid, embedding) VALUES (2, '[0,1]')", "cos insert 2");
    {
        sqlite3_int64 ids[] = {1, 2};
        double dists[] = {0.0, 1.0};  // 同向=0，正交=1
        expect_knn(db, "idxc", "[2,0]", 2, ids, dists, 2, "cosine knn");
    }

    // ── 错误路径 ──
    expect_error(db, "INSERT INTO idx(rowid, embedding) VALUES (60, '[1,2]')",
                 "dim mismatch insert errors");
    expect_error(db, "INSERT INTO idx(rowid, embedding) VALUES (1, '[0,0,0]')",
                 "duplicate rowid errors");
    expect_error(db, "SELECT * FROM idx WHERE embedding MATCH '[0,0,0]'",
                 "match without k errors");
    expect_error(db, "SELECT * FROM idx WHERE embedding MATCH '[0,0,0]' AND k = 0",
                 "k=0 errors");
    expect_error(db, "INSERT INTO idx(rowid, embedding, distance) VALUES (70, '[0,0,0]', 1.0)",
                 "write distance column errors");
    expect_error(db,
                 "CREATE VIRTUAL TABLE bad USING GRAPH_INDEX(v FLOAT[3], metric=hamming)",
                 "unknown metric errors");

    // ── 关库重开：config 恢复 + 数据在 + KNN 仍对 ──
    sqlite3_close(db);
    db = NULL;
    if (sqlite3_open(dbpath, &db) != SQLITE_OK) return 1;
    if (vexdb_sqlite_register(db) != SQLITE_OK) return 1;
    check(count_rows(db, "idx") == 3, "reopen: rows survive");
    {
        sqlite3_int64 ids[] = {1, 2};
        double dists[] = {0.0, 1.0};
        expect_knn(db, "idx", "[0,0,0]", 2, ids, dists, 2, "reopen: knn correct");
    }
    // 重开后维度校验仍生效（config 真的被读回来了）
    expect_error(db, "INSERT INTO idx(rowid, embedding) VALUES (80, '[1,2]')",
                 "reopen: dim check survives");

    // ── DROP 清理 shadow ──
    exec_ok(db, "DROP TABLE idx", "drop vtab");
    {
        sqlite3_stmt *st = NULL;
        int rc = sqlite3_prepare_v2(db, "SELECT count(*) FROM idx_vectors", -1, &st, NULL);
        sqlite3_finalize(st);
        check(rc != SQLITE_OK, "drop removes shadow tables");
    }

    sqlite3_close(db);
    unlink(dbpath);
    if (g_fail) {
        printf("M2 VTAB SMOKE: FAIL\n");
        return 1;
    }
    printf("M2 VTAB SMOKE: PASS\n");
    return 0;
}
