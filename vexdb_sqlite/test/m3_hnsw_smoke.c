// M3 HNSW 冒烟测试（静态注册形态）。
//
// 验收覆盖（计划 M3）：超过暴力阈值(64)的表走 HNSW 图——recall@10 对照暴力
// ground truth、distance 值与标量函数一致、增量 INSERT、**关库重开走 %_graph
// blob 还原**、DELETE 后正确（invalidate+rebuild）、ROLLBACK 后图一致、
// cosine，以及 RaBitQ 的 L2/cosine/IP。
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

#define DIM 16
#define N 400
#define K 10

static int g_fail = 0;

static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "M3 FAIL: %s\n", what);
        g_fail = 1;
    }
}

static int exec_ok(sqlite3 *db, const char *sql, const char *what) {
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "M3 FAIL: %s: rc=%d %s\n", what, rc, errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        g_fail = 1;
        return 0;
    }
    return 1;
}

// 固定种子 LCG，保证可复现。
static unsigned int g_seed = 20260610;
static float frand(void) {
    g_seed = g_seed * 1103515245u + 12345u;
    return ((g_seed >> 8) & 0xFFFF) / 65536.0f * 2.0f - 1.0f;
}

static void vec_json(char *buf, size_t cap, const float *v) {
    size_t off = 0;
    buf[off++] = '[';
    for (int d = 0; d < DIM; d++) {
        off += snprintf(buf + off, cap - off, "%s%.6f", d ? "," : "", v[d]);
    }
    snprintf(buf + off, cap - off, "]");
}

// KNN 查询（走 vtab MATCH+k），输出 rowid/dist 数组，返回行数。
static int knn(sqlite3 *db, const char *table, const char *query_json, int k,
               sqlite3_int64 *ids, double *dists) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "SELECT rowid, distance FROM %s WHERE embedding MATCH '%s' AND k = %d",
             table, query_json, k);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW && n < k) {
        ids[n] = sqlite3_column_int64(st, 0);
        dists[n] = sqlite3_column_double(st, 1);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

// 暴力 ground truth：vtab 全扫 + 标量距离函数排序。
static int brute(sqlite3 *db, const char *table, const char *fn, const char *query_json,
                 int k, sqlite3_int64 *ids, double *dists) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "SELECT rowid, %s(embedding, '%s') AS d FROM %s ORDER BY d LIMIT %d",
             fn, query_json, table, k);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW && n < k) {
        ids[n] = sqlite3_column_int64(st, 0);
        dists[n] = sqlite3_column_double(st, 1);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

// recall@k（按 id 集合交集），同时校验 KNN 的 dist 与 ground truth 首位一致。
static double recall_check(sqlite3 *db, const char *table, const char *fn,
                           const float *query, const char *what) {
    char qjson[2048];
    vec_json(qjson, sizeof(qjson), query);
    sqlite3_int64 gids[K], aids[K];
    double gd[K], ad[K];
    int gn = brute(db, table, fn, qjson, K, gids, gd);
    int an = knn(db, table, qjson, K, aids, ad);
    if (gn != K || an != K) {
        fprintf(stderr, "M3 FAIL: %s: rows g=%d a=%d\n", what, gn, an);
        g_fail = 1;
        return 0;
    }
    int hit = 0;
    for (int i = 0; i < K; i++) {
        for (int j = 0; j < K; j++) {
            if (aids[i] == gids[j]) { hit++; break; }
        }
    }
    // distance 数值与标量函数一致（top1 必须同 id 同值）
    if (aids[0] != gids[0] || fabs(ad[0] - gd[0]) > 1e-5) {
        fprintf(stderr, "M3 FAIL: %s: top1 id/dist mismatch a=(%lld,%.8f) g=(%lld,%.8f)\n",
                what, (long long)aids[0], ad[0], (long long)gids[0], gd[0]);
        g_fail = 1;
    }
    return (double)hit / K;
}

int main(int argc, char **argv) {
    const int skip_rabitq = argc > 1 && strcmp(argv[1], "--no-rabitq") == 0;
    /* Android 无 /tmp（用 TMPDIR=/data/local/tmp 覆盖）；桌面默认 /tmp */
    const char *tmpdir = getenv("TMPDIR");
    char dbpath[512];
    snprintf(dbpath, sizeof(dbpath), "%s/vexdb_m3_smoke.db", tmpdir ? tmpdir : "/tmp");
    unlink(dbpath);
    sqlite3 *db = NULL;
    if (sqlite3_open(dbpath, &db) != SQLITE_OK) return 1;
    if (vexdb_sqlite_register(db) != SQLITE_OK) return 1;

    static float data[N][DIM];
    for (int i = 0; i < N; i++) {
        for (int d = 0; d < DIM; d++) data[i][d] = frand();
    }

    // ── L2 表：N=400 > 阈值 64，KNN 走图 ──
    exec_ok(db, "CREATE VIRTUAL TABLE idx USING GRAPH_INDEX("
                "embedding FLOAT[16], metric=l2, m=16, ef_construction=200, ef_search=120)",
            "create l2 vtab");
    exec_ok(db, "BEGIN", "begin bulk");
    for (int i = 0; i < N; i++) {
        char buf[2048], sql[2304];
        vec_json(buf, sizeof(buf), data[i]);
        snprintf(sql, sizeof(sql), "INSERT INTO idx(rowid, embedding) VALUES (%d, '%s')",
                 i + 1, buf);
        if (!exec_ok(db, sql, "bulk insert")) break;
    }
    exec_ok(db, "COMMIT", "commit bulk");

    // recall（5 个查询取均值；小数据 + ef_search=120 应接近 1.0）
    double rsum = 0;
    for (int qi = 0; qi < 5; qi++) {
        rsum += recall_check(db, "idx", "vexdb_l2_distance", data[qi * 37 % N], "l2 recall");
    }
    printf("l2 recall@%d = %.3f (5 queries avg)\n", K, rsum / 5);
    check(rsum / 5 >= 0.9, "l2 recall >= 0.9");

    // ── 增量 INSERT：新行立即可查（内存图真增量） ──
    {
        float v[DIM];
        for (int d = 0; d < DIM; d++) v[d] = 9.0f;
        char buf[2048], sql[2304];
        vec_json(buf, sizeof(buf), v);
        snprintf(sql, sizeof(sql), "INSERT INTO idx(rowid, embedding) VALUES (9001, '%s')", buf);
        exec_ok(db, sql, "incremental insert");
        sqlite3_int64 ids[K]; double ds[K];
        int n = knn(db, "idx", buf, 1, ids, ds);
        check(n == 1 && ids[0] == 9001 && ds[0] < 1e-5, "incremental insert searchable");
    }

    // ── 关库重开：xSync 应已把图落盘到 %_graph；重开 load 路径结果一致 ──
    {
        sqlite3_stmt *st = NULL;
        sqlite3_prepare_v2(db, "SELECT count(*) FROM idx_graph", -1, &st, NULL);
        sqlite3_int64 blobs = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int64(st, 0) : -1;
        sqlite3_finalize(st);
        /* v2 段式（M9'）：meta/elems/upper/base/vec 至少 5 行 */
        check(blobs >= 5, "graph v2 segments persisted by xSync");
    }
    sqlite3_close(db);
    db = NULL;
    if (sqlite3_open(dbpath, &db) != SQLITE_OK) return 1;
    if (vexdb_sqlite_register(db) != SQLITE_OK) return 1;
    {
        double r = recall_check(db, "idx", "vexdb_l2_distance", data[3], "reopen recall");
        check(r >= 0.9, "reopen: recall via loaded blob >= 0.9");
    }

    // ── DELETE：被删行不再出现（invalidate + rebuild 路径） ──
    {
        char buf[2048];
        vec_json(buf, sizeof(buf), data[0]);
        exec_ok(db, "DELETE FROM idx WHERE rowid = 1", "delete row 1");
        sqlite3_int64 ids[K]; double ds[K];
        int n = knn(db, "idx", buf, K, ids, ds);
        check(n == K, "knn after delete returns k");
        for (int i = 0; i < n; i++) {
            check(ids[i] != 1, "deleted row absent from knn");
        }
    }

    // ── ROLLBACK：回滚的插入不可见 ──
    {
        float v[DIM];
        for (int d = 0; d < DIM; d++) v[d] = -9.0f;
        char buf[2048], sql[2304];
        vec_json(buf, sizeof(buf), v);
        exec_ok(db, "BEGIN", "begin rollback test");
        snprintf(sql, sizeof(sql), "INSERT INTO idx(rowid, embedding) VALUES (9002, '%s')", buf);
        exec_ok(db, sql, "insert in txn");
        exec_ok(db, "ROLLBACK", "rollback");
        sqlite3_int64 ids[K]; double ds[K];
        int n = knn(db, "idx", buf, 1, ids, ds);
        check(n == 1 && ids[0] != 9002, "rolled-back row absent from knn");
    }

    if (skip_rabitq) {
        char *errmsg = NULL;
        int rc = sqlite3_exec(db,
            "CREATE VIRTUAL TABLE unsupported_rabitq USING GRAPH_INDEX("
            "embedding FLOAT[16], metric=l2, quantizer=rabitq)",
            NULL, NULL, &errmsg);
        check(rc != SQLITE_OK && errmsg != NULL && strstr(errmsg, "not supported") != NULL,
              "unsupported RaBitQ is rejected explicitly");
        sqlite3_free(errmsg);
    } else {
    // ── RaBitQ cosine：归一化语义 + distance 数值对照 ──
    exec_ok(db, "CREATE VIRTUAL TABLE idxc USING GRAPH_INDEX("
                "embedding FLOAT[16], metric=cosine, quantizer=rabitq, "
                "m=16, ef_construction=200, ef_search=160)",
            "create rabitq cosine vtab");
    exec_ok(db, "BEGIN", "begin cos bulk");
    for (int i = 0; i < N; i++) {
        char buf[2048], sql[2304];
        vec_json(buf, sizeof(buf), data[i]);
        snprintf(sql, sizeof(sql), "INSERT INTO idxc(rowid, embedding) VALUES (%d, '%s')",
                 i + 1, buf);
        if (!exec_ok(db, sql, "cos bulk insert")) break;
    }
    exec_ok(db, "COMMIT", "commit cos bulk");
    {
        double rc_sum = 0;
        for (int qi = 0; qi < 5; qi++) {
            rc_sum += recall_check(db, "idxc", "vexdb_cosine_distance", data[qi * 53 % N],
                                   "rabitq cosine recall");
        }
        printf("rabitq cosine recall@%d = %.3f (5 queries avg)\n", K, rc_sum / 5);
        check(rc_sum / 5 >= 0.8, "rabitq cosine recall >= 0.8");
    }

    // ── RaBitQ IP：lower=closer 的负内积语义 + recall ──
    exec_ok(db, "CREATE VIRTUAL TABLE idxri USING GRAPH_INDEX("
                "embedding FLOAT[16], metric=ip, quantizer=rabitq, "
                "m=16, ef_construction=200, ef_search=160)",
            "create rabitq ip vtab");
    exec_ok(db, "BEGIN", "begin rabitq ip bulk");
    for (int i = 0; i < N; i++) {
        char buf[2048], sql[2304];
        vec_json(buf, sizeof(buf), data[i]);
        snprintf(sql, sizeof(sql), "INSERT INTO idxri(rowid, embedding) VALUES (%d, '%s')",
                 i + 1, buf);
        if (!exec_ok(db, sql, "rabitq ip bulk insert")) break;
    }
    exec_ok(db, "COMMIT", "commit rabitq ip bulk");
    {
        double ri_sum = 0;
        for (int qi = 0; qi < 5; qi++) {
            ri_sum += recall_check(db, "idxri", "vexdb_negative_inner_product",
                                   data[qi * 67 % N], "rabitq ip recall");
        }
        printf("rabitq ip recall@%d = %.3f (5 queries avg)\n", K, ri_sum / 5);
        check(ri_sum / 5 >= 0.8, "rabitq ip recall >= 0.8");
    }

    // ── RaBitQ：共享量化算法、增量编码、持久化重开 ──
    exec_ok(db, "CREATE VIRTUAL TABLE idxr USING GRAPH_INDEX("
                "embedding FLOAT[16], metric=l2, quantizer=rabitq, "
                "m=16, ef_construction=200, ef_search=160)",
            "create rabitq vtab");
    exec_ok(db, "BEGIN", "begin rabitq bulk");
    for (int i = 0; i < N; i++) {
        char buf[2048], sql[2304];
        vec_json(buf, sizeof(buf), data[i]);
        snprintf(sql, sizeof(sql), "INSERT INTO idxr(rowid, embedding) VALUES (%d, '%s')",
                 i + 1, buf);
        if (!exec_ok(db, sql, "rabitq bulk insert")) break;
    }
    exec_ok(db, "COMMIT", "commit rabitq bulk");
    {
        double rr_sum = 0;
        for (int qi = 0; qi < 5; qi++) {
            rr_sum += recall_check(db, "idxr", "vexdb_l2_distance", data[qi * 41 % N],
                                   "rabitq recall");
        }
        printf("rabitq recall@%d = %.3f (5 queries avg)\n", K, rr_sum / 5);
        check(rr_sum / 5 >= 0.8, "rabitq recall >= 0.8");
    }
    {
        float v[DIM];
        for (int d = 0; d < DIM; d++) v[d] = 7.0f + d * 0.01f;
        char buf[2048], sql[2304];
        vec_json(buf, sizeof(buf), v);
        snprintf(sql, sizeof(sql), "INSERT INTO idxr(rowid, embedding) VALUES (9100, '%s')", buf);
        exec_ok(db, sql, "rabitq incremental insert");
        sqlite3_int64 ids[K]; double ds[K];
        int n = knn(db, "idxr", buf, 1, ids, ds);
        check(n == 1 && ids[0] == 9100, "rabitq incremental insert searchable");
    }
    {
        char old_buf[2048];
        vec_json(old_buf, sizeof(old_buf), data[0]);
        exec_ok(db, "DELETE FROM idxr WHERE rowid = 1", "rabitq delete");
        sqlite3_int64 ids[K]; double ds[K];
        int n = knn(db, "idxr", old_buf, K, ids, ds);
        int found = 0;
        for (int i = 0; i < n; i++) found |= ids[i] == 1;
        check(!found, "rabitq deleted row stays filtered");

        float updated[DIM];
        for (int d = 0; d < DIM; d++) updated[d] = -8.0f - d * 0.01f;
        char updated_buf[2048], sql[2304];
        vec_json(updated_buf, sizeof(updated_buf), updated);
        snprintf(sql, sizeof(sql), "UPDATE idxr SET embedding = '%s' WHERE rowid = 2",
                 updated_buf);
        exec_ok(db, sql, "rabitq update");
        n = knn(db, "idxr", updated_buf, 1, ids, ds);
        check(n == 1 && ids[0] == 2, "rabitq updated row searchable");

        float rolled_back[DIM];
        for (int d = 0; d < DIM; d++) rolled_back[d] = 11.0f + d * 0.01f;
        char rollback_buf[2048];
        vec_json(rollback_buf, sizeof(rollback_buf), rolled_back);
        exec_ok(db, "BEGIN", "begin rabitq rollback");
        snprintf(sql, sizeof(sql), "INSERT INTO idxr(rowid, embedding) VALUES (9200, '%s')",
                 rollback_buf);
        exec_ok(db, sql, "rabitq rollback insert");
        exec_ok(db, "ROLLBACK", "rabitq rollback");
        n = knn(db, "idxr", rollback_buf, K, ids, ds);
        found = 0;
        for (int i = 0; i < n; i++) found |= ids[i] == 9200;
        check(!found, "rabitq rolled back row stays absent");
    }
    {
        sqlite3_stmt *st = NULL;
        sqlite3_prepare_v2(db,
                           "SELECT sum(kind=5), sum(kind=6) FROM idxr_graph WHERE kind IN (5,6)",
                           -1, &st, NULL);
        int nfixed = -1, ncodes = -1;
        if (sqlite3_step(st) == SQLITE_ROW) {
            nfixed = sqlite3_column_int(st, 0);
            ncodes = sqlite3_column_int(st, 1);
        }
        sqlite3_finalize(st);
        check(nfixed == 1 && ncodes > 1, "rabitq fixed and segmented codes persisted");
    }
    sqlite3_close(db);
    db = NULL;
    if (sqlite3_open(dbpath, &db) != SQLITE_OK) return 1;
    if (vexdb_sqlite_register(db) != SQLITE_OK) return 1;
    {
        double rr = recall_check(db, "idxr", "vexdb_l2_distance", data[17],
                                 "rabitq reopen recall");
        check(rr >= 0.8, "rabitq reopen recall >= 0.8");
    }
    }

    sqlite3_close(db);
    unlink(dbpath);
    if (g_fail) {
        printf("M3 HNSW SMOKE: FAIL\n");
        return 1;
    }
    printf("M3 HNSW SMOKE: PASS\n");
    return 0;
}
