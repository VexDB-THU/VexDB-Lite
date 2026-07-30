// GRAPH_INDEX 虚拟表模块 —— M2：shadow table 持久化 + xUpdate + 暴力 KNN。
//
// 用法：
//   CREATE VIRTUAL TABLE idx USING GRAPH_INDEX(
//       embedding FLOAT[128], metric=cosine, m=16, ef_construction=200);
//   INSERT INTO idx(rowid, embedding) VALUES (1, :vec);   -- BLOB 或 JSON 文本
//   SELECT rowid, distance FROM idx WHERE embedding MATCH :q AND k = 10;
//
// Shadow tables（普通表，经宿主连接 SQL 读写 → 随宿主事务原子回滚，这是
// FTS5/RTREE 的标准范式；铁律：禁止任何旁路 mmap/裸文件写）：
//   <name>_config (key TEXT PRIMARY KEY, value TEXT)   维度/metric/m/efc/格式版本
//   <name>_vectors(rowid INTEGER PRIMARY KEY, vec BLOB) 向量数据
//
// M2 查询是暴力扫描（功能对标 sqlite-vec）；M3 把 xFilter 切到 GraphIndexCore
// 的 ANN 搜索，%_graph shadow 表落图结构。distance 列三 metric 统一
// lower=closer（L2=sqrt、cosine=1-sim、ip=负内积），ORDER BY distance ASC 即最近。
#include "vexdb_sqlite_internal.h"
#include "vtab/graph_index_vtab.h"
#include "functions/vector_codec.h"
#include "index/graph_bridge.h"
#include "quantizer/product_quantizer.h"
#include "rabitq/code_distancer.h"
#include "vex_distance_entry.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using vexdb_sqlite::GetDistanceFn;
using vexdb_sqlite::GetVector;
using vexdb_sqlite::GraphBridge;
using vexdb_sqlite::VectorView;

constexpr int kFormatVersion = 1;
constexpr int kDefaultM = 16;
constexpr int kDefaultEfConstruction = 200;
constexpr int kDefaultEfSearch = 40;
// 行数不超过此值时 KNN 直接暴力扫描（小表 ANN 无性能收益反损 recall，
// 语义对齐 duck 端 vexdb_brute_force_threshold 的精度优先取向）。
constexpr sqlite3_int64 kBruteForceThreshold = 64;
// 图 blob 分块大小：SQLite 单 blob 上限默认 1GB（SIFT1M m=32 图 ~1.1GB 实证撞限），
// 按块写 %_graph 多行。256MB 默认；建表参数 graph_chunk_size 可调（测试用小值强制多块）。
constexpr sqlite3_int64 kDefaultGraphChunk = 256LL * 1024 * 1024;

// declare_vtab 的列序（rowid 之外）。M6' 起 meta 列动态插在向量列与 distance
// 之间：embedding=0, meta[i]=1+i, distance=1+n_meta, k=2+n_meta(HIDDEN),
// cmd=3+n_meta(与表同名的 HIDDEN 列，fts5 风格 special insert)。
// 动态列号经 GraphIndexVtab::ColDistance()/ColK()/ColCmd() 取。
enum Col { COL_EMBEDDING = 0 };

// xBestIndex/xFilter 间的计划标记。低 8 位=计划类型，高位=KNN 修饰 flag。
enum Plan { PLAN_SCAN = 0, PLAN_KNN = 1, PLAN_ROWID = 2 };
constexpr int KNN_HAS_OFFSET = 0x100;    // argv 含 OFFSET（紧随 k 之后）
constexpr int KNN_K_FROM_LIMIT = 0x200;  // k 来自 LIMIT 下推（k<=0 = 不限）
inline int PlanOf(int idx_num) { return idx_num & 0xff; }

// metadata 列（Stage B：标量列随向量存 %_vectors 同表，supports filtered search）。
struct MetaCol {
    std::string name;
    std::string type;  // TEXT | INTEGER | REAL（SQLite affinity 白名单）
};

struct GraphIndexVtab {
    sqlite3_vtab base;
    sqlite3 *db = nullptr;
    std::string schema;   // attached database 名（main/temp/…）
    std::string name;     // 虚拟表名
    std::vector<MetaCol> meta_cols;
    int dim = 0;
    VexMetric metric = VexMetric::L2;
    int m = kDefaultM;
    int ef_construction = kDefaultEfConstruction;
    int ef_search = kDefaultEfSearch;
    QuantizerType quantizer = QuantizerType::NONE;
    bool quantizer_explicit = false;
    uint32_t pq_m = 0;
    bool compact_mode = false;
    sqlite3_int64 brute_force_threshold = kBruteForceThreshold;
    sqlite3_int64 graph_chunk_size = kDefaultGraphChunk;  // v2 起不再生效（段式天然分块），保留解析兼容旧建表 SQL
    // M9'：查询期图内存预算（字节）。0=无限制（全内存模式，性能形态）；
    // >0 且全图估算超限 → DiskStore 段式懒加载（meta/elems/upper 常驻，
    // base/vec 段 LRU，预算管段缓存）。
    sqlite3_int64 graph_memory_limit = 0;

    // M3：内存 HNSW 图（懒加载：首次 KNN 时从 %_graph blob 还原或 %_vectors 重建）。
    // 一致性协议：任何写路径先清 %_graph（宿主事务内）；xSync 把 dirty 内存图
    // 序列化写回——磁盘上存在 blob 即保证与 %_vectors 一致；崩溃中途=无 blob=重建。
    std::unique_ptr<GraphBridge> graph;
    bool graph_dirty = false;
    // 行数缓存：-1=未知（首查时 count 一次），之后随 INSERT/DELETE 增减维护。
    // 1M 行表上每次 KNN 都 count(*) 全表扫（~80ms）会淹没 HNSW 本身（μs 级）。
    sqlite3_int64 row_count = -1;
    // 跨连接失效 cookie（FTS5 iCookie 同理）：%_config 'cookie' 整数，任何
    // 连接的图/段变更事务在 xSync bump；本连接在查询/写入口比对，变了就作废
    // 缓存的图与行数——否则他端提交的写永远不可见，且本端的 stale 图写回
    // 会把他端的行从持久化图中抹掉。
    sqlite3_int64 cookie_seen = -1;
    bool cookie_bump_pending = false;
    // DML stmt 缓存（schema 固定 → SQL 形态固定；多行 DML 免每行 prepare）。
    // [0]=无 rowid INSERT / 含 vec UPDATE；[1]=带 rowid INSERT / vec 不变 UPDATE
    sqlite3_stmt *dml_insert[2] = {nullptr, nullptr};
    sqlite3_stmt *dml_update[2] = {nullptr, nullptr};
    sqlite3_stmt *vec_fetch_stmt = nullptr;

    // 动态列号（meta 列数决定 distance/k/cmd 偏移）
    int ColDistance() const { return 1 + int(meta_cols.size()); }
    int ColK() const { return 2 + int(meta_cols.size()); }
    int ColCmd() const { return 3 + int(meta_cols.size()); }

    std::string ShadowName(const char *suffix) const {
        char *q = sqlite3_mprintf("\"%w\".\"%w_%s\"", schema.c_str(), name.c_str(), suffix);
        std::string s = q ? q : "";
        sqlite3_free(q);
        return s;
    }
    int SetError(const char *msg) {
        sqlite3_free(base.zErrMsg);
        base.zErrMsg = sqlite3_mprintf("%s", msg);
        return SQLITE_ERROR;
    }
};

struct GraphIndexCursor {
    sqlite3_vtab_cursor base;
    int plan = PLAN_SCAN;
    // SCAN 模式：流式遍历 shadow 表
    sqlite3_stmt *scan_stmt = nullptr;
    int scan_eof = 1;
    // KNN 模式：物化 top-k 结果
    struct Hit { double dist; sqlite3_int64 rowid; };
    std::vector<Hit> hits;
    size_t pos = 0;
    // KNN 模式 meta 列点查缓存（持久 stmt + 当前行）：免每 行×列 重 prepare
    sqlite3_stmt *meta_stmt = nullptr;
    sqlite3_int64 meta_rowid = -1;
    bool meta_row_ok = false;
};

GraphIndexVtab *asVtab(sqlite3_vtab *v) { return reinterpret_cast<GraphIndexVtab *>(v); }
GraphIndexCursor *asCursor(sqlite3_vtab_cursor *c) { return reinterpret_cast<GraphIndexCursor *>(c); }

// ---------- 参数解析 ----------

void TrimSpaces(std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    s = (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

bool ParseMetric(const std::string &v, VexMetric &out) {
    if (v == "l2") { out = VexMetric::L2; return true; }
    if (v == "cosine") { out = VexMetric::COSINE; return true; }
    if (v == "ip" || v == "inner_product") { out = VexMetric::INNER_PRODUCT; return true; }
    return false;
}

const char *QuantizerName(QuantizerType quantizer) {
    switch (quantizer) {
    case QuantizerType::PQ: return "pq";
    case QuantizerType::RABITQ: return "rabitq";
    case QuantizerType::NONE: return "none";
    }
    return "none";
}

// 解析 CREATE VIRTUAL TABLE 的参数串（argv[3..]）：
//   恰好一个向量列声明 "<col> FLOAT[<dim>]"（大小写不敏感），
//   加可选 "metric=..." "m=..." "ef_construction=..."。
bool ParseCreateArgs(int argc, const char *const *argv, GraphIndexVtab &vt,
                     std::string &col_name, std::string &err) {
    bool have_col = false;
    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];
        TrimSpaces(arg);
        if (arg.empty()) continue;
        size_t eq = arg.find('=');
        if (eq != std::string::npos && arg.find(' ') > eq) {
            std::string key = arg.substr(0, eq), val = arg.substr(eq + 1);
            TrimSpaces(key); TrimSpaces(val);
            for (auto &c : key) c = static_cast<char>(tolower(c));
            for (auto &c : val) c = static_cast<char>(tolower(c));
            // 容忍 'cosine' / "cosine" 引号
            if (val.size() >= 2 && (val.front() == '\'' || val.front() == '"'))
                val = val.substr(1, val.size() - 2);
            if (key == "metric") {
                if (!ParseMetric(val, vt.metric)) { err = "unknown metric: " + val; return false; }
            } else if (key == "quantizer") {
                vt.quantizer_explicit = true;
                if (val == "rabitq") {
                    vt.quantizer = QuantizerType::RABITQ;
                } else if (val == "pq") {
                    vt.quantizer = QuantizerType::PQ;
                } else if (val == "none" || val.empty()) {
                    vt.quantizer = QuantizerType::NONE;
                } else {
                    err = "quantizer must be 'pq', 'rabitq' or 'none'";
                    return false;
                }
            } else if (key == "pq_m") {
                char *end = nullptr;
                long parsed = std::strtol(val.c_str(), &end, 10);
                if (end == val.c_str() || *end != '\0' || parsed < 0 || parsed > INT_MAX) {
                    err = "pq_m must be a non-negative integer";
                    return false;
                }
                vt.pq_m = uint32_t(parsed);
            } else if (key == "memory_mode") {
                if (val == "compact") {
                    vt.compact_mode = true;
                } else if (val == "full") {
                    vt.compact_mode = false;
                } else {
                    err = "memory_mode must be 'full' or 'compact'";
                    return false;
                }
            } else if (key == "m") {
                vt.m = atoi(val.c_str());
                if (vt.m < 2 || vt.m > 256) { err = "m out of range [2,256]"; return false; }
            } else if (key == "ef_construction") {
                vt.ef_construction = atoi(val.c_str());
                if (vt.ef_construction < 1) { err = "ef_construction must be positive"; return false; }
            } else if (key == "ef_search") {
                vt.ef_search = atoi(val.c_str());
                if (vt.ef_search < 1) { err = "ef_search must be positive"; return false; }
            } else if (key == "brute_force_threshold") {
                vt.brute_force_threshold = atoll(val.c_str());
                if (vt.brute_force_threshold < 0) { err = "brute_force_threshold must be >= 0"; return false; }
            } else if (key == "graph_chunk_size") {
                vt.graph_chunk_size = atoll(val.c_str());
                if (vt.graph_chunk_size < 4096) { err = "graph_chunk_size must be >= 4096"; return false; }
            } else if (key == "graph_memory_limit") {
                vt.graph_memory_limit = atoll(val.c_str());
                if (vt.graph_memory_limit < 0) { err = "graph_memory_limit must be >= 0"; return false; }
            } else {
                err = "unknown parameter: " + key;
                return false;
            }
            continue;
        }
        // 列声明 "<name> FLOAT[<dim>]"（向量列，恰好一个、必须最先声明）或
        // "<name> TEXT|INTEGER|REAL"（metadata 标量列，Stage B filtered search）
        size_t sp = arg.find_first_of(" \t");
        if (sp == std::string::npos) { err = "bad column declaration: " + arg; return false; }
        std::string cname = arg.substr(0, sp), ctype = arg.substr(sp + 1);
        TrimSpaces(ctype);
        std::string upper = ctype;
        for (auto &c : upper) c = static_cast<char>(toupper(c));
        if (upper.compare(0, 6, "FLOAT[") == 0 && upper.back() == ']') {
            if (have_col) { err = "exactly one vector column is supported"; return false; }
            if (!vt.meta_cols.empty()) {
                err = "vector column must be declared before metadata columns";
                return false;
            }
            int d = atoi(ctype.substr(6).c_str());
            if (d <= 0 || static_cast<size_t>(d) > vexdb_sqlite::kMaxDim) {
                err = "dimension out of range [1,65535]";
                return false;
            }
            vt.dim = d;
            col_name = cname;
            have_col = true;
            continue;
        }
        if (upper == "TEXT" || upper == "INTEGER" || upper == "INT" || upper == "REAL") {
            if (!have_col) {
                err = "vector column must be declared before metadata columns";
                return false;
            }
            // ':' 与 ',' 是 meta_cols 的 config 序列化分隔符（"name:TYPE,..."），
            // 列名含它们会在重连解析时错位 → DeclareSchema 失败把表永久砖死。
            if (cname.find(':') != std::string::npos || cname.find(',') != std::string::npos) {
                err = "metadata column name must not contain ':' or ','";
                return false;
            }
            vt.meta_cols.push_back({cname, upper == "INT" ? "INTEGER" : upper});
            continue;
        }
        err = "column type must be FLOAT[N] or TEXT/INTEGER/REAL: " + arg;
        return false;
    }
    if (!have_col) { err = "missing vector column declaration, e.g. embedding FLOAT[128]"; return false; }
    if (vt.compact_mode && vt.quantizer_explicit && vt.quantizer == QuantizerType::NONE) {
        err = "memory_mode='compact' requires quantizer='pq' or quantizer='rabitq'";
        return false;
    }
    if (vt.quantizer == QuantizerType::RABITQ && vt.pq_m != 0) {
        err = "pq_m cannot be combined with quantizer='rabitq'";
        return false;
    }
#if defined(_WIN32)
    if (vt.quantizer == QuantizerType::RABITQ) {
        err = "quantizer='rabitq' is not supported on Windows yet";
        return false;
    }
#endif
    if (vt.quantizer == QuantizerType::NONE && (vt.compact_mode || vt.pq_m != 0)) {
        vt.quantizer = QuantizerType::PQ;
    }
    if (vt.quantizer == QuantizerType::PQ) {
        if (vt.pq_m == 0) {
            vt.pq_m = ::vex::quantizer::ProductQuantizer::AutoSelectM(uint32_t(vt.dim));
        }
        if (vt.pq_m == 0 || uint32_t(vt.dim) % vt.pq_m != 0) {
            err = "pq_m must be a positive divisor of the vector dimension";
            return false;
        }
    }
    return true;
}

// ---------- shadow table 辅助 ----------

int ExecFmt(sqlite3 *db, const char *sql) {
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
}

int ConfigSet(GraphIndexVtab &vt, const char *key, const std::string &val) {
    char *sql = sqlite3_mprintf(
        "INSERT OR REPLACE INTO %s(key, value) VALUES (%Q, %Q)",
        vt.ShadowName("config").c_str(), key, val.c_str());
    int rc = ExecFmt(vt.db, sql);
    sqlite3_free(sql);
    return rc;
}

bool ConfigGet(GraphIndexVtab &vt, const char *key, std::string &out) {
    char *sql = sqlite3_mprintf("SELECT value FROM %s WHERE key = %Q",
                                vt.ShadowName("config").c_str(), key);
    sqlite3_stmt *st = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(vt.db, sql, -1, &st, nullptr) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        // NULL 防御（手编/损坏的 config 行、或 OOM 下 column_text 返回 NULL）：
        // std::string 由空指针构造是 UB，会把"打开表"变成进程崩溃。
        const char *txt = reinterpret_cast<const char *>(sqlite3_column_text(st, 0));
        if (txt) {
            out = txt;
            ok = true;
        }
    }
    sqlite3_finalize(st);
    sqlite3_free(sql);
    return ok;
}

// 引号安全的列名片段（"name" 或 "name" TYPE）
std::string QuotedCol(const std::string &name, const char *type = nullptr) {
    char *q = type ? sqlite3_mprintf("\"%w\" %s", name.c_str(), type)
                   : sqlite3_mprintf("\"%w\"", name.c_str());
    std::string s = q ? q : "";
    sqlite3_free(q);
    return s;
}

int DeclareSchema(sqlite3 *db, const GraphIndexVtab &vt, const std::string &col_name) {
    // 列序 = [向量列, meta..., distance, k HIDDEN, cmd HIDDEN]；distance 只在
    // KNN 计划下有值；末列与表同名（fts5 风格 special insert 入口）。
    std::string cols = QuotedCol(col_name);
    for (const auto &mc : vt.meta_cols)
        cols += ", " + QuotedCol(mc.name, mc.type.c_str());
    char *sql = sqlite3_mprintf(
        "CREATE TABLE x(%s, distance REAL, k INTEGER HIDDEN, \"%w\" TEXT HIDDEN)",
        cols.c_str(), vt.name.c_str());
    int rc = sqlite3_declare_vtab(db, sql);
    sqlite3_free(sql);
    return rc;
}

// meta 列定义 config 序列化："name:TYPE,name:TYPE"（列名经 argv 解析不含逗号/冒号）
std::string SerializeMetaCols(const std::vector<MetaCol> &mcs) {
    std::string s;
    for (const auto &mc : mcs) {
        if (!s.empty()) s += ",";
        s += mc.name + ":" + mc.type;
    }
    return s;
}

void ParseMetaCols(const std::string &s, std::vector<MetaCol> &out) {
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos
                                                                    : comma - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos)
            out.push_back({item.substr(0, colon), item.substr(colon + 1)});
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
}

// xCreate/xConnect 公共体。create=true 时建 shadow 表并写 config，
// false 时从 config 恢复参数。
int ConnectImpl(sqlite3 *db, int argc, const char *const *argv,
                sqlite3_vtab **ppVtab, char **pzErr, bool create) {
    // 声明约束支持：xUpdate 返回 SQLITE_CONSTRAINT 时由核心按 OR 子句处理
    //（INSERT OR IGNORE 跳行继续）。不声明则任何 OR 子句都直接 abort。
    sqlite3_vtab_config(db, SQLITE_VTAB_CONSTRAINT_SUPPORT, 1);
    auto *vt = new (std::nothrow) GraphIndexVtab();
    if (!vt) return SQLITE_NOMEM;
    vt->db = db;
    vt->schema = argv[1];
    vt->name = argv[2];

    std::string err, col_name;
    if (create) {
        if (!ParseCreateArgs(argc, argv, *vt, col_name, err)) {
            *pzErr = sqlite3_mprintf("GRAPH_INDEX: %s", err.c_str());
            delete vt;
            return SQLITE_ERROR;
        }
        // %_vectors 带 meta 列（用户可在其上自建 SQLite 索引加速过滤谓词）
        std::string meta_decl;
        for (const auto &mc : vt->meta_cols)
            meta_decl += ", " + QuotedCol(mc.name, mc.type.c_str());
        // %_graph 格式 v2（M9' 段式）：kind 0=meta 1=elems 2=upper 3=base 段 4=vec 段
        char *sql = sqlite3_mprintf(
            "CREATE TABLE %s(key TEXT PRIMARY KEY, value TEXT);"
            "CREATE TABLE %s(rowid INTEGER PRIMARY KEY, vec BLOB NOT NULL%s);"
            "CREATE TABLE %s(kind INTEGER NOT NULL, seg INTEGER NOT NULL, data BLOB NOT NULL,"
            " PRIMARY KEY(kind, seg));",
            vt->ShadowName("config").c_str(), vt->ShadowName("vectors").c_str(),
            meta_decl.c_str(), vt->ShadowName("graph").c_str());
        int rc = ExecFmt(db, sql);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) {
            *pzErr = sqlite3_mprintf("GRAPH_INDEX: failed to create shadow tables: %s",
                                     sqlite3_errmsg(db));
            delete vt;
            return rc;
        }
        ConfigSet(*vt, "format_version", std::to_string(kFormatVersion));
        ConfigSet(*vt, "dim", std::to_string(vt->dim));
        ConfigSet(*vt, "metric", std::to_string(static_cast<uint32_t>(vt->metric)));
        ConfigSet(*vt, "m", std::to_string(vt->m));
        ConfigSet(*vt, "ef_construction", std::to_string(vt->ef_construction));
        ConfigSet(*vt, "ef_search", std::to_string(vt->ef_search));
        ConfigSet(*vt, "brute_force_threshold", std::to_string(vt->brute_force_threshold));
        ConfigSet(*vt, "graph_chunk_size", std::to_string(vt->graph_chunk_size));
        ConfigSet(*vt, "graph_memory_limit", std::to_string(vt->graph_memory_limit));
        ConfigSet(*vt, "quantizer", QuantizerName(vt->quantizer));
        ConfigSet(*vt, "pq_m", std::to_string(vt->pq_m));
        ConfigSet(*vt, "memory_mode", vt->compact_mode ? "compact" : "full");
        ConfigSet(*vt, "column", col_name);
        ConfigSet(*vt, "cookie", "0");
        if (!vt->meta_cols.empty())
            ConfigSet(*vt, "meta_cols", SerializeMetaCols(vt->meta_cols));
    } else {
        std::string v;
        if (ConfigGet(*vt, "column", v)) {
            col_name = v;
        }
        if (ConfigGet(*vt, "meta_cols", v)) {
            ParseMetaCols(v, vt->meta_cols);
        }
        if (!ConfigGet(*vt, "format_version", v)) {
            *pzErr = sqlite3_mprintf("GRAPH_INDEX: missing shadow config for %s", argv[2]);
            delete vt;
            return SQLITE_ERROR;
        }
        if (atoi(v.c_str()) > kFormatVersion) {
            *pzErr = sqlite3_mprintf(
                "GRAPH_INDEX: on-disk format v%s is newer than supported v%d", v.c_str(),
                kFormatVersion);
            delete vt;
            return SQLITE_ERROR;
        }
        ConfigGet(*vt, "dim", v);
        vt->dim = atoi(v.c_str());
        ConfigGet(*vt, "metric", v);
        vt->metric = static_cast<VexMetric>(atoi(v.c_str()));
        ConfigGet(*vt, "m", v);
        vt->m = atoi(v.c_str());
        ConfigGet(*vt, "ef_construction", v);
        vt->ef_construction = atoi(v.c_str());
        if (ConfigGet(*vt, "ef_search", v)) {
            vt->ef_search = atoi(v.c_str());
        }
        if (ConfigGet(*vt, "brute_force_threshold", v)) {
            vt->brute_force_threshold = atoll(v.c_str());
        }
        if (ConfigGet(*vt, "graph_chunk_size", v)) {
            vt->graph_chunk_size = atoll(v.c_str());
        }
        if (ConfigGet(*vt, "graph_memory_limit", v)) {
            vt->graph_memory_limit = atoll(v.c_str());
        }
        if (ConfigGet(*vt, "quantizer", v)) {
            if (v == "pq") vt->quantizer = QuantizerType::PQ;
            else if (v == "rabitq") vt->quantizer = QuantizerType::RABITQ;
            else vt->quantizer = QuantizerType::NONE;
        }
        if (ConfigGet(*vt, "pq_m", v)) {
            vt->pq_m = uint32_t(std::max(0, atoi(v.c_str())));
        }
        if (ConfigGet(*vt, "memory_mode", v)) {
            vt->compact_mode = v == "compact";
        }
        if (vt->quantizer == QuantizerType::PQ &&
            (vt->pq_m == 0 || uint32_t(vt->dim) % vt->pq_m != 0)) {
            *pzErr = sqlite3_mprintf(
                "GRAPH_INDEX: persisted pq_m must be a positive divisor of the vector dimension");
            delete vt;
            return SQLITE_CORRUPT;
        }
        if (vt->quantizer == QuantizerType::RABITQ && vt->pq_m != 0) {
            *pzErr = sqlite3_mprintf(
                "GRAPH_INDEX: persisted pq_m cannot be combined with quantizer='rabitq'");
            delete vt;
            return SQLITE_CORRUPT;
        }
#if defined(_WIN32)
        if (vt->quantizer == QuantizerType::RABITQ) {
            *pzErr = sqlite3_mprintf(
                "GRAPH_INDEX: persisted quantizer='rabitq' is not supported on Windows yet");
            delete vt;
            return SQLITE_ERROR;
        }
#endif
        if (vt->compact_mode && vt->quantizer == QuantizerType::NONE) {
            *pzErr = sqlite3_mprintf(
                "GRAPH_INDEX: persisted compact index requires a quantizer");
            delete vt;
            return SQLITE_CORRUPT;
        }
    }

    if (col_name.empty()) col_name = "embedding";
    int rc = DeclareSchema(db, *vt, col_name);
    if (rc != SQLITE_OK) {
        delete vt;
        return rc;
    }
    *ppVtab = &vt->base;
    return SQLITE_OK;
}

// ---------- module 回调 ----------

int vtabCreate(sqlite3 *db, void *, int argc, const char *const *argv,
               sqlite3_vtab **ppVtab, char **pzErr) {
    return ConnectImpl(db, argc, argv, ppVtab, pzErr, /*create=*/true);
}

int vtabConnect(sqlite3 *db, void *, int argc, const char *const *argv,
                sqlite3_vtab **ppVtab, char **pzErr) {
    return ConnectImpl(db, argc, argv, ppVtab, pzErr, /*create=*/false);
}

int vtabDisconnect(sqlite3_vtab *pVtab) {
    auto *vt = asVtab(pVtab);
    for (auto *st : vt->dml_insert) sqlite3_finalize(st);
    for (auto *st : vt->dml_update) sqlite3_finalize(st);
    sqlite3_finalize(vt->vec_fetch_stmt);
    delete vt;
    return SQLITE_OK;
}

int vtabDestroy(sqlite3_vtab *pVtab) {
    auto *vt = asVtab(pVtab);
    char *sql = sqlite3_mprintf(
        "DROP TABLE IF EXISTS %s; DROP TABLE IF EXISTS %s; DROP TABLE IF EXISTS %s;",
        vt->ShadowName("config").c_str(), vt->ShadowName("vectors").c_str(),
        vt->ShadowName("graph").c_str());
    int rc = ExecFmt(vt->db, sql);
    sqlite3_free(sql);
    // DROP 失败（如 SQLITE_LOCKED）时核心保留 pVtab 指针并随后调 xDisconnect
    //（sqlite3VtabCallDestroy 仅在 rc==OK 时清指针）——此时绝不能先 delete，
    // 否则 xDisconnect 二次释放。
    if (rc != SQLITE_OK) return rc;
    for (auto *st : vt->dml_insert) sqlite3_finalize(st);
    for (auto *st : vt->dml_update) sqlite3_finalize(st);
    sqlite3_finalize(vt->vec_fetch_stmt);
    delete vt;
    return SQLITE_OK;
}

// ---------- M3：内存图生命周期 ----------

sqlite3_int64 CountVectors(GraphIndexVtab &vt) {
    if (vt.row_count >= 0) return vt.row_count;
    char *sql = sqlite3_mprintf("SELECT count(*) FROM %s", vt.ShadowName("vectors").c_str());
    sqlite3_stmt *st = nullptr;
    sqlite3_int64 n = 0;
    if (sqlite3_prepare_v2(vt.db, sql, -1, &st, nullptr) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    sqlite3_free(sql);
    vt.row_count = n;
    return n;
}

// 任何写路径调用：作废持久化图（宿主事务内，随事务回滚）。段变更需在事务
// 提交时 bump 跨连接 cookie（xSync 统一执行）。
int InvalidatePersistedGraph(GraphIndexVtab &vt) {
    char *sql = sqlite3_mprintf("DELETE FROM %s", vt.ShadowName("graph").c_str());
    int rc = ExecFmt(vt.db, sql);
    sqlite3_free(sql);
    vt.cookie_bump_pending = true;
    return rc;
}

// %_graph v2 段读回调（kind, seg → data）。持久 prepared stmt（shared_ptr 析构
// finalize）：缓存 miss 是 DiskStore 热路径，per-miss prepare 开销不可忽略。
// stmt 生命周期随闭包（graph 成员）≤ vt.db 连接，安全。
GraphBridge::SegReadFn MakeSegReader(GraphIndexVtab &vt) {
    char *sql = sqlite3_mprintf("SELECT data FROM %s WHERE kind = ?1 AND seg = ?2",
                                vt.ShadowName("graph").c_str());
    sqlite3_stmt *raw = nullptr;
    int rc = sqlite3_prepare_v2(vt.db, sql, -1, &raw, nullptr);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) return nullptr;
    std::shared_ptr<sqlite3_stmt> st(raw, sqlite3_finalize);
    return [st](int kind, uint32_t seg, std::vector<char> &out) -> bool {
        sqlite3_bind_int(st.get(), 1, kind);
        sqlite3_bind_int64(st.get(), 2, sqlite3_int64(seg));
        bool ok = false;
        if (sqlite3_step(st.get()) == SQLITE_ROW) {
            const char *blob = static_cast<const char *>(sqlite3_column_blob(st.get(), 0));
            // NULL 防御（schema 是 NOT NULL，但手编/损坏库或 OOM 可返回 NULL；
            // nullptr+0 指针算术是 UB）
            if (blob) {
                out.assign(blob, blob + sqlite3_column_bytes(st.get(), 0));
                ok = true;
            }
        }
        // 用后立即 reset：停在 ROW 态的活跃语句会钉住连接的隐式读事务
        //（连接空闲期阻塞他端写 / WAL checkpoint 永不能 restart）。
        sqlite3_reset(st.get());
        return ok;
    };
}

GraphBridge::SegWriteFn MakeSegWriter(GraphIndexVtab &vt) {
    return [&vt](int kind, uint32_t seg, const std::vector<char> &data) -> bool {
        // UPSERT（非 INSERT OR REPLACE）：UPDATE 分支保持段行 rowid 不变——
        // 记录粒度读的 blob handle 按 rowid 定位，REPLACE 的 delete+insert
        // 会让同事务内后续 read_rec 的 rowid 缓存失效。
        char *sql = sqlite3_mprintf(
            "INSERT INTO %s(kind, seg, data) VALUES (?1, ?2, ?3)"
            " ON CONFLICT(kind, seg) DO UPDATE SET data = excluded.data",
            vt.ShadowName("graph").c_str());
        sqlite3_stmt *st = nullptr;
        int rc = sqlite3_prepare_v2(vt.db, sql, -1, &st, nullptr);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) return false;
        sqlite3_bind_int(st, 1, kind);
        sqlite3_bind_int64(st, 2, sqlite3_int64(seg));
        sqlite3_bind_blob64(st, 3, data.data(), data.size(), SQLITE_STATIC);
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        return rc == SQLITE_DONE;
    };
}

// 记录粒度读（M9'b）：sqlite3_blob 增量 I/O 只触达 offset 所在页，免整段拷贝。
// (kind,seg)→rowid 持久 stmt 点查 + 缓存；blob handle reopen 复用，语句事务
// 过期（SQLITE_ABORT）或 rowid 失效时 close 重开/重查兜底。
GraphBridge::SegRecReadFn MakeRecReader(GraphIndexVtab &vt) {
    char *sql = sqlite3_mprintf("SELECT rowid FROM %s WHERE kind = ?1 AND seg = ?2",
                                vt.ShadowName("graph").c_str());
    sqlite3_stmt *raw = nullptr;
    int rc = sqlite3_prepare_v2(vt.db, sql, -1, &raw, nullptr);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) return nullptr;
    std::shared_ptr<sqlite3_stmt> st(raw, sqlite3_finalize);

    struct RecState {
        std::unordered_map<uint64_t, sqlite3_int64> rowids;
        sqlite3_blob *blob = nullptr;
        ~RecState() {
            if (blob) sqlite3_blob_close(blob);
        }
    };
    auto state = std::make_shared<RecState>();
    sqlite3 *db = vt.db;
    std::string zdb = vt.schema;
    std::string ztable = vt.name + "_graph";

    return [st, state, db, zdb, ztable](int kind, uint32_t seg, size_t offset, size_t len,
                                        char *dst) -> bool {
        const uint64_t key = (uint64_t(uint32_t(kind)) << 32) | seg;
        bool ok = false;
        for (int pass = 0; pass < 2 && !ok; pass++) {
            sqlite3_int64 rowid;
            auto it = state->rowids.find(key);
            if (it != state->rowids.end() && pass == 0) {
                rowid = it->second;
            } else {
                sqlite3_bind_int(st.get(), 1, kind);
                sqlite3_bind_int64(st.get(), 2, sqlite3_int64(seg));
                int src = sqlite3_step(st.get());
                if (src != SQLITE_ROW) {
                    sqlite3_reset(st.get());
                    return false;
                }
                rowid = sqlite3_column_int64(st.get(), 0);
                sqlite3_reset(st.get());  // 用后即 reset：活跃语句钉读事务
                state->rowids[key] = rowid;
            }
            for (int attempt = 0; attempt < 2; attempt++) {
                if (sqlite3_blob_open(db, zdb.c_str(), ztable.c_str(), "data", rowid, 0,
                                      &state->blob) != SQLITE_OK) {
                    state->blob = nullptr;
                    break;  // rowid 可能失效 → 外层重查
                }
                int rc2 = sqlite3_blob_read(state->blob, dst, int(len), int(offset));
                // blob handle 用后立即关闭：打开的 handle 内部是停在 ROW 的
                // 语句，同样钉住读事务（reopen 复用的微优化让位正确性）。
                sqlite3_blob_close(state->blob);
                state->blob = nullptr;
                if (rc2 == SQLITE_OK) {
                    ok = true;
                    break;
                }
                if (rc2 != SQLITE_ABORT) break;  // ABORT=事务过期重试一次，其余失败
            }
        }
        return ok;
    };
}

// 全内存图占用估算（DiskStore 分流判定；记录定长可精确推算，留 ~10% 容器开销不计较）
sqlite3_int64 EstimateGraphBytes(const GraphIndexVtab &vt, sqlite3_int64 n) {
    auto sat_add = [](sqlite3_int64 a, sqlite3_int64 b) {
        return a > std::numeric_limits<sqlite3_int64>::max() - b
            ? std::numeric_limits<sqlite3_int64>::max()
            : a + b;
    };
    auto sat_mul = [](sqlite3_int64 a, sqlite3_int64 b) {
        if (a == 0 || b == 0) return sqlite3_int64(0);
        return a > std::numeric_limits<sqlite3_int64>::max() / b
            ? std::numeric_limits<sqlite3_int64>::max()
            : a * b;
    };
    if (n <= 0) return 0;
    sqlite3_int64 code_bytes = 0;
    if (vt.quantizer == QuantizerType::PQ) {
        code_bytes = sqlite3_int64(vt.pq_m);
    } else if (vt.quantizer == QuantizerType::RABITQ) {
        code_bytes = sqlite3_int64(::rabitq::CodeSize(vt.dim));
    }
    sqlite3_int64 vector_bytes = vt.compact_mode
        ? 0 : sat_mul(sqlite3_int64(vt.dim), 4);
    sqlite3_int64 per = sat_add(vector_bytes, code_bytes);
    per = sat_add(per, sat_mul(sqlite3_int64(vt.m), 16)); // base neighbors+dists
    per = sat_add(per, 48);                               // elems/tids/容器头
    sqlite3_int64 upper_per = sat_add(sat_mul(sqlite3_int64(vt.m), 12), 64);
    sqlite3_int64 upper = sat_mul(n / std::max(1, vt.m), upper_per);
    return sat_add(sat_mul(n, per), upper);
}

// 模式分流判定（统一三处调用点防漂移）。体量取 max(存活行数, 持久化节点数)：
// 删除标记保留空壳节点（≤20%），实际加载体量由 base_count 决定，只看行数会在
// 阈值边界低估 ~25% 并误选全内存模式、超限载入。
bool OverLimit(GraphIndexVtab &vt) {
    if (vt.graph_memory_limit <= 0) return false;
    sqlite3_int64 n = CountVectors(vt);
    sqlite3_int64 persisted = GraphBridge::PeekPersistedNodeCount(MakeSegReader(vt));
    if (persisted > n) n = persisted;
    return EstimateGraphBytes(vt, n) > vt.graph_memory_limit;
}

// 跨连接缓存失效：%_config 'cookie' 变了 → 他端提交过图/段变更，作废本端
// 缓存（图+row_count）。查询与写入口各调一次（一次 config 点查 ~µs 级）。
void CheckCookie(GraphIndexVtab &vt) {
    std::string v;
    sqlite3_int64 cur = ConfigGet(vt, "cookie", v) ? atoll(v.c_str()) : 0;
    if (cur != vt.cookie_seen) {
        if (vt.cookie_seen >= 0) {  // 首查（-1）只记录，无需作废
            vt.graph.reset();
            vt.graph_dirty = false;
            vt.row_count = -1;
        }
        vt.cookie_seen = cur;
    }
}

// 串行预读 (rowid, vec)（limit<0=全部）。计算线程绝不触碰 sqlite3 句柄的
// 边界由此保证——预读完成后 BuildBulk 才 spawn worker。
int PrereadVectors(GraphIndexVtab &vt, sqlite3_int64 limit, std::vector<float> &vecs,
                   std::vector<int64_t> &rowids) {
    char *sql = limit >= 0
        ? sqlite3_mprintf("SELECT rowid, vec FROM %s ORDER BY rowid LIMIT %lld",
                          vt.ShadowName("vectors").c_str(), (long long)limit)
        : sqlite3_mprintf("SELECT rowid, vec FROM %s ORDER BY rowid",
                          vt.ShadowName("vectors").c_str());
    sqlite3_stmt *st = nullptr;
    int rc = sqlite3_prepare_v2(vt.db, sql, -1, &st, nullptr);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) return rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (sqlite3_column_bytes(st, 1) != vt.dim * 4) continue;  // 防御脏数据
        const float *v = static_cast<const float *>(sqlite3_column_blob(st, 1));
        vecs.insert(vecs.end(), v, v + vt.dim);
        rowids.push_back(sqlite3_column_int64(st, 0));
    }
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

int DefaultBuildThreads() {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    return 1;
#else
    unsigned hw = std::thread::hardware_concurrency();
    return int(hw > 1 ? (hw > 8 ? 8 : hw) : 1);  // 端侧保守上限 8
#endif
}

// 从 %_vectors 全量重建（全内存）。两段式：先串行预读再 BuildBulk 多线程建图
//（M3+ 并行；构建期独占，xFilter 同线程等待完成）。
int RebuildInMemory(GraphIndexVtab &vt) {
    std::vector<float> vecs;
    std::vector<int64_t> rowids;
    int rc = PrereadVectors(vt, -1, vecs, rowids);
    if (rc != SQLITE_OK) return rc;
    auto bridge = std::make_unique<GraphBridge>(uint16_t(vt.dim), vt.m, vt.ef_construction,
                                                vt.metric, vt.quantizer, vt.pq_m,
                                                vt.compact_mode);
    try {
        bridge->BuildBulk(vecs.data(), rowids.data(), rowids.size(), DefaultBuildThreads());
    } catch (const std::exception &e) {
        return vt.SetError(e.what());
    }
    vt.graph = std::move(bridge);
    vt.graph_dirty = true;
    return SQLITE_OK;
}

// 两阶段构建（M9'c，对齐 PG openGauss"内存建到预算线 → flush → 磁盘逐条"）。
// 仅在写事务内调用（xSync 恢复路径）：
//   phase 1（MEMORY）：前 K 行（rowid 升序）预读 + 并行 BuildBulk → SerializeV2
//   落段 → 释放（K 由预算反推：图 + 预读数组合计 ≤ limit）
//   phase 2（DISK）：OpenV2Disk 打开段图，剩余行流式逐条 algo.insert（段缓存内
//   增量，dirty 段超预算 evict 写回）→ flush dirty + 常驻
// 构建期峰值 ≈ graph_memory_limit（phase 2 流式游标 O(1)；%_vectors 只读与
// %_graph 只写不同表，同连接嵌套语句合法）。
int BuildTwoPhase(GraphIndexVtab &vt, sqlite3_int64 n) {
    // 构建期预算 = 4×graph_memory_limit（对齐 PG maintenance_work_mem >
    // work_mem 的惯例：构建是一次性 maintenance 操作，临时放大仍有界；
    // phase 2 的写工作集是反向边触碰的 base 段，预算太紧会驱逐 dirty 段
    // 反复写回——写放大不可用）。
    sqlite3_int64 build_budget = vt.graph_memory_limit >
            std::numeric_limits<sqlite3_int64>::max() / 4
        ? std::numeric_limits<sqlite3_int64>::max()
        : vt.graph_memory_limit * 4;
    // 每行内存：图（vec+base+upper 摊销，对齐 EstimateGraphBytes）+ 预读数组
    sqlite3_int64 per_graph = EstimateGraphBytes(vt, 1024) / 1024 + 1;
    sqlite3_int64 per_row = per_graph + sqlite3_int64(vt.dim) * 4 + 16;
    sqlite3_int64 K = build_budget / std::max<sqlite3_int64>(per_row, 1);
    K = std::max<sqlite3_int64>(K, 1024);  // 太小的内存图没有锚定意义
    if (K > n) K = n;

    sqlite3_int64 last_rowid = 0;
    {
        // phase 1：前 K 行并行建图
        std::vector<float> vecs;
        std::vector<int64_t> rowids;
        vecs.reserve(size_t(K) * vt.dim);
        rowids.reserve(size_t(K));
        int rc = PrereadVectors(vt, K, vecs, rowids);
        if (rc != SQLITE_OK) return rc;
        if (rowids.empty()) return SQLITE_OK;
        last_rowid = rowids.back();

        GraphBridge mem_bridge(uint16_t(vt.dim), vt.m, vt.ef_construction, vt.metric,
                               vt.quantizer, vt.pq_m, vt.compact_mode);
        try {
            mem_bridge.BuildBulk(vecs.data(), rowids.data(), rowids.size(),
                                 DefaultBuildThreads());
        } catch (const std::exception &e) {
            return vt.SetError(e.what());
        }
        // 落段前清残段（进入条件=meta 缺失，可能仍有孤儿段）
        int rc2 = InvalidatePersistedGraph(vt);
        if (rc2 != SQLITE_OK) return rc2;
        if (!mem_bridge.SerializeV2(MakeSegWriter(vt)))
            return vt.SetError(sqlite3_errmsg(vt.db));
    }  // 内存图与预读数组在此释放

    // phase 2：DiskStore 打开（构建期预算）+ 剩余行流式逐条插入。构建完成后
    // 图保留使用（预算大于查询期 limit 是软超额；图作废/重开即回到查询预算）。
    std::string err;
    vt.graph = GraphBridge::OpenV2Disk(MakeSegReader(vt), MakeSegWriter(vt), MakeRecReader(vt),
                                       uint16_t(vt.dim), vt.m, vt.ef_construction, vt.metric,
                                       vt.quantizer, vt.pq_m, vt.compact_mode,
                                       size_t(build_budget), err);
    if (!vt.graph) return vt.SetError(err.c_str());
    vt.graph_dirty = false;
    if (last_rowid > 0 && K < n) {
        char *sql = sqlite3_mprintf(
            "SELECT rowid, vec FROM %s WHERE rowid > %lld ORDER BY rowid",
            vt.ShadowName("vectors").c_str(), (long long)last_rowid);
        sqlite3_stmt *st = nullptr;
        int rc = sqlite3_prepare_v2(vt.db, sql, -1, &st, nullptr);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) return rc;
        try {
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                if (sqlite3_column_bytes(st, 1) != vt.dim * 4) continue;
                const float *v = static_cast<const float *>(sqlite3_column_blob(st, 1));
                vt.graph->Insert(v, sqlite3_column_int64(st, 0));
            }
        } catch (const std::exception &e) {
            sqlite3_finalize(st);
            vt.graph.reset();
            return vt.SetError(e.what());
        }
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return rc;
    }
    if (!vt.graph->SerializeV2(MakeSegWriter(vt)))
        return vt.SetError(sqlite3_errmsg(vt.db));
    return SQLITE_OK;
}

// 仅尝试从 v2/v3/v4 段打开图（按 limit 分流两模式），不重建。成功后 vt.graph
// 非空。供 EnsureGraph 与 DELETE 标记路径（图未加载时先 open 再标记）共用。
void TryOpenGraph(GraphIndexVtab &vt) {
    if (vt.graph) return;
    auto reader = MakeSegReader(vt);
    if (!reader) return;
    std::string err;
    try {
        if (OverLimit(vt)) {
            // write 用于写事务内 dirty 段写回（增量 INSERT/evict）；查询期段永不
            // dirty，不会触发写。read_rec=记录粒度直读（缓存冻结后的 miss 路径）。
            vt.graph = GraphBridge::OpenV2Disk(reader, MakeSegWriter(vt), MakeRecReader(vt),
                                               uint16_t(vt.dim), vt.m, vt.ef_construction,
                                               vt.metric, vt.quantizer, vt.pq_m,
                                               vt.compact_mode,
                                               size_t(vt.graph_memory_limit), err);
        } else {
            vt.graph = GraphBridge::OpenV2(reader, uint16_t(vt.dim), vt.m, vt.ef_construction,
                                           vt.metric, vt.quantizer, vt.pq_m,
                                           vt.compact_mode, err);
        }
    } catch (const std::exception &) {
        // bad_alloc 等：open 失败按"无图"处理（调用方有暴力退化/重建兜底），
        // 异常不得穿 SQLite C 帧。
        vt.graph.reset();
    }
    if (vt.graph) vt.graph_dirty = false;
}

// 确保图就绪。分流（M9'）：
//   全图估算 ≤ graph_memory_limit（或 limit=0）→ 全内存：v2 段载入，无/损坏则重建
//   超限 → DiskStore 懒加载 open（只读形态）；无 v2 段时**不**重建（重建峰值违背
//          limit 本意）→ graph 保持空，调用方退化暴力扫描；恢复路径=下一次写
//          事务的 xSync 重建+落盘（见 vtabSync）。
int EnsureGraph(GraphIndexVtab &vt) {
    if (vt.graph) return SQLITE_OK;
    TryOpenGraph(vt);
    if (vt.graph) return SQLITE_OK;
    if (OverLimit(vt)) return SQLITE_OK;  // graph 仍空 = 暴力退化信号
    // 无 v2 段/损坏/参数不符 → fall through 重建（标 dirty 待写事务落盘）
    return RebuildInMemory(vt);
}

// meta 谓词下推的操作符编码（idxStr 用单字符；'g'/'l' 是 >=/<=）。
char MetaOpCode(unsigned char op) {
    switch (op) {
    case SQLITE_INDEX_CONSTRAINT_EQ: return '=';
    case SQLITE_INDEX_CONSTRAINT_GT: return '>';
    case SQLITE_INDEX_CONSTRAINT_GE: return 'g';
    case SQLITE_INDEX_CONSTRAINT_LT: return '<';
    case SQLITE_INDEX_CONSTRAINT_LE: return 'l';
    }
    return 0;
}

const char *MetaOpSql(char code) {
    switch (code) {
    case '=': return "=";
    case '>': return ">";
    case 'g': return ">=";
    case '<': return "<";
    case 'l': return "<=";
    }
    return nullptr;
}

int vtabBestIndex(sqlite3_vtab *pVtab, sqlite3_index_info *info) {
    auto *vt = asVtab(pVtab);
    const int n_meta = int(vt->meta_cols.size());
    int match_idx = -1, k_idx = -1, limit_idx = -1, offset_idx = -1, rowid_idx = -1;
    // (constraint 下标, meta 列下标, op 码)。EQ/GT/GE/LT/LE 都下推——白名单
    // 预查机制与谓词形态无关（SQL WHERE 拼接），同列多约束（范围双边）自然
    // AND 叠加。
    std::vector<std::tuple<int, int, char>> meta_preds;
    // 存在我们无法消费的 usable 约束（不可下推 op 的 meta 谓词、distance 列
    // 谓词、rowid 谓词等）时，引擎会对 vtab 输出再过滤——此时 LIMIT 不能当
    // k（vtab 只回 LIMIT 行，过滤后必少于 LIMIT，错误的 SQL 语义）。
    bool has_unconsumed = false;
    for (int i = 0; i < info->nConstraint; i++) {
        const auto &c = info->aConstraint[i];
        if (!c.usable) continue;
        if (c.iColumn == COL_EMBEDDING && c.op == SQLITE_INDEX_CONSTRAINT_MATCH) {
            match_idx = i;
            continue;
        }
        if (c.iColumn == vt->ColK() && c.op == SQLITE_INDEX_CONSTRAINT_EQ) {
            k_idx = i;
            continue;
        }
        if (c.op == SQLITE_INDEX_CONSTRAINT_LIMIT) {
            limit_idx = i;
            continue;
        }
        if (c.op == SQLITE_INDEX_CONSTRAINT_OFFSET) {
            offset_idx = i;
            continue;
        }
        if (c.iColumn < 0 && c.op == SQLITE_INDEX_CONSTRAINT_EQ) {
            rowid_idx = i;  // rowid 点查（PLAN_ROWID 用；KNN 下算未消费）
            has_unconsumed = true;
            continue;
        }
        if (c.iColumn >= 1 && c.iColumn <= n_meta) {
            char op = MetaOpCode(c.op);
            if (op) {
                meta_preds.emplace_back(i, c.iColumn - 1, op);
                continue;
            }
        }
        has_unconsumed = true;
    }
    // k 来源二选一：显式 k=?，或 LIMIT 下推（SQLite ≥3.38，sqlite-vec 习惯写法
    // `... MATCH ? ORDER BY distance LIMIT n`）。显式 k 优先；LIMIT 仅在引擎
    // 不会再过滤输出时才安全（见 has_unconsumed）。
    int k_src = k_idx;
    bool k_from_limit = false;
    if (k_src < 0 && limit_idx >= 0 && !has_unconsumed) {
        k_src = limit_idx;
        k_from_limit = true;
    }
    if (match_idx >= 0 && k_src >= 0) {
        info->aConstraintUsage[match_idx].argvIndex = 1;
        info->aConstraintUsage[match_idx].omit = 1;
        info->aConstraintUsage[k_src].argvIndex = 2;
        info->aConstraintUsage[k_src].omit = 1;
        int idx_num = PLAN_KNN;
        if (k_from_limit) idx_num |= KNN_K_FROM_LIMIT;
        int argv_i = 3;
        // LIMIT 当 k 且带 OFFSET：必须一并消费（omit=1 → 核心置零 OFFSET
        // 计数器），vtab 取 limit+offset 个最近邻再跳过前 offset 个。只消费
        // LIMIT 不消费 OFFSET 时核心会对已截断的 top-k 再跳行 → 错误结果。
        if (k_from_limit && offset_idx >= 0) {
            info->aConstraintUsage[offset_idx].argvIndex = argv_i++;
            info->aConstraintUsage[offset_idx].omit = 1;
            idx_num |= KNN_HAS_OFFSET;
        }
        // M7'：meta 列约束（EQ + 范围 GT/GE/LT/LE）下推进图内过滤（idxStr
        // 每项 "<meta 下标><op 码>" 逗号分隔，如 "0=,1g"）。omit 保持 0：
        // 引擎仍按 xColumn 复核谓词——图内过滤是性能优化非正确性来源，dedup
        // 多行节点/类型亲和/collation 边界全由引擎兜底。
        if (!meta_preds.empty()) {
            std::string enc;
            for (const auto &mp : meta_preds) {
                info->aConstraintUsage[std::get<0>(mp)].argvIndex = argv_i++;
                if (!enc.empty()) enc += ',';
                enc += std::to_string(std::get<1>(mp));
                enc += std::get<2>(mp);
            }
            info->idxStr = static_cast<char *>(sqlite3_malloc(int(enc.size()) + 1));
            if (!info->idxStr) return SQLITE_NOMEM;
            memcpy(info->idxStr, enc.c_str(), enc.size() + 1);
            info->needToFreeIdxStr = 1;
        }
        info->idxNum = idx_num;
        info->estimatedCost = 100.0;
        info->estimatedRows = 10;
        // KNN 结果已按 distance 升序物化
        if (info->nOrderBy == 1 && info->aOrderBy[0].iColumn == vt->ColDistance() &&
            !info->aOrderBy[0].desc) {
            info->orderByConsumed = 1;
        }
        return SQLITE_OK;
    }
    if (match_idx >= 0) {
        // 有 MATCH 没 k（或 LIMIT 因未消费谓词不能当 k）：明确报错好过静默全扫
        return SQLITE_CONSTRAINT;
    }
    if (rowid_idx >= 0) {
        // rowid 点查计划：DELETE/UPDATE/SELECT ... WHERE rowid=N 免全表扫
        //（此前 1M 行表删一行要流式读完全部向量 blob）。
        info->aConstraintUsage[rowid_idx].argvIndex = 1;
        info->idxNum = PLAN_ROWID;
        info->estimatedCost = 1.0;
        info->estimatedRows = 1;
        return SQLITE_OK;
    }
    info->idxNum = PLAN_SCAN;
    info->estimatedCost = 1e6;
    return SQLITE_OK;
}

int vtabOpen(sqlite3_vtab *, sqlite3_vtab_cursor **ppCur) {
    auto *c = new (std::nothrow) GraphIndexCursor();
    if (!c) return SQLITE_NOMEM;
    *ppCur = &c->base;
    return SQLITE_OK;
}

int vtabClose(sqlite3_vtab_cursor *cur) {
    auto *c = asCursor(cur);
    sqlite3_finalize(c->scan_stmt);
    sqlite3_finalize(c->meta_stmt);
    delete c;
    return SQLITE_OK;
}

// 大小 k 的有界最大堆 top-k 收集器（堆顶=当前第 k 近，更近则替换）。
// 全表暴力与白名单点查两条路径共用——堆逻辑/并列规则只此一份。
struct TopKCollector {
    std::vector<GraphIndexCursor::Hit> &heap;
    size_t k;
    static bool cmp(const GraphIndexCursor::Hit &a, const GraphIndexCursor::Hit &b) {
        return a.dist < b.dist;
    }
    TopKCollector(std::vector<GraphIndexCursor::Hit> &h, sqlite3_int64 k_in)
        : heap(h), k(size_t(k_in)) {
        heap.clear();
    }
    void offer(double d, sqlite3_int64 rowid) {
        if (heap.size() < k) {
            heap.push_back({d, rowid});
            std::push_heap(heap.begin(), heap.end(), cmp);
        } else if (d < heap.front().dist) {
            std::pop_heap(heap.begin(), heap.end(), cmp);
            heap.back() = {d, rowid};
            std::push_heap(heap.begin(), heap.end(), cmp);
        }
    }
    void finish() { std::sort_heap(heap.begin(), heap.end(), cmp); }  // 升序：最近在前
};

// 全表暴力扫描 top-k。小表精度优先路径 + M9' DiskStore 无图时的退化路径。
int KnnBruteScan(GraphIndexVtab &vt, GraphIndexCursor &cur, const VectorView &q,
                 sqlite3_int64 k) {
    const auto dist_fn = GetDistanceFn(vt.metric);
    char *sql = sqlite3_mprintf("SELECT rowid, vec FROM %s", vt.ShadowName("vectors").c_str());
    sqlite3_stmt *st = nullptr;
    int rc = sqlite3_prepare_v2(vt.db, sql, -1, &st, nullptr);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) return rc;

    TopKCollector topk(cur.hits, k);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        int len = sqlite3_column_bytes(st, 1);
        if (len != vt.dim * 4) continue;  // 防御：维度不符的脏数据跳过
        const float *vec = static_cast<const float *>(sqlite3_column_blob(st, 1));
        topk.offer(dist_fn(q.data, vec, static_cast<uint16_t>(vt.dim)),
                   sqlite3_column_int64(st, 0));
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return rc;
    topk.finish();
    cur.pos = 0;
    return SQLITE_OK;
}

// 谓词白名单内逐 rowid 点查算距离取 top-k（M7' 集合暴力退化路径：白名单
// 远小于全表时比图遍历+过滤更快且召回精确）。
int KnnBruteOverSet(GraphIndexVtab &vt, GraphIndexCursor &cur, const VectorView &q,
                    sqlite3_int64 k, const std::vector<sqlite3_int64> &rowids) {
    const auto dist_fn = GetDistanceFn(vt.metric);
    char *sql = sqlite3_mprintf("SELECT vec FROM %s WHERE rowid = ?1",
                                vt.ShadowName("vectors").c_str());
    sqlite3_stmt *st = nullptr;
    int rc = sqlite3_prepare_v2(vt.db, sql, -1, &st, nullptr);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) return rc;
    TopKCollector topk(cur.hits, k);
    for (sqlite3_int64 rid : rowids) {
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, rid);
        int srcrc = sqlite3_step(st);
        if (srcrc != SQLITE_ROW) {
            if (srcrc != SQLITE_DONE) {  // BUSY/IOERR/INTERRUPT 等必须上抛，
                sqlite3_finalize(st);    // 否则静默返回截断的 top-k
                return srcrc;
            }
            continue;  // DONE=行不存在（白名单与 %_vectors 瞬时漂移），跳过
        }
        if (sqlite3_column_bytes(st, 0) != vt.dim * 4) continue;
        const float *vec = static_cast<const float *>(sqlite3_column_blob(st, 0));
        topk.offer(dist_fn(q.data, vec, static_cast<uint16_t>(vt.dim)), rid);
    }
    sqlite3_finalize(st);
    topk.finish();
    cur.pos = 0;
    return SQLITE_OK;
}

int FilterKnn(GraphIndexVtab &vt, GraphIndexCursor &cur, int idx_num, const char *idxStr,
              int argc, sqlite3_value **argv) {
    VectorView q;
    std::string err;
    if (!GetVector(argv[0], q, err)) return vt.SetError(err.c_str());
    if (static_cast<int>(q.dim) != vt.dim) {
        err = "query dimension " + std::to_string(q.dim) + " != index dimension " +
              std::to_string(vt.dim);
        return vt.SetError(err.c_str());
    }
    sqlite3_int64 k = sqlite3_value_int64(argv[1]);
    if (k <= 0) {
        // LIMIT 下推来源：负 LIMIT 是 SQL 的"全部行"惯例（LIMIT -1）→ k=N；
        // 显式 k= 仍要求正数。
        if (idx_num & KNN_K_FROM_LIMIT) {
            k = CountVectors(vt);
            if (k <= 0) {
                cur.pos = 0;
                return SQLITE_OK;  // 空表
            }
        } else {
            return vt.SetError("k must be positive");
        }
    }
    // OFFSET 一并下推时（仅 LIMIT 当 k 的形态）：取 k+offset 个最近邻，
    // 输出从第 offset 个起（核心已被 omit 告知不再跳行）。
    sqlite3_int64 offset = 0;
    int next_argv = 2;
    if (idx_num & KNN_HAS_OFFSET) {
        offset = std::max<sqlite3_int64>(0, sqlite3_value_int64(argv[2]));
        next_argv = 3;
        k += offset;
    }

    // M7'：解析 idxStr 的 meta 谓词（"<下标><op 码>" 逗号分隔，argv 对位紧随
    // k/offset 之后；op 码见 MetaOpCode——EQ 与范围 GT/GE/LT/LE 都下推）。
    struct MetaPred {
        int col;
        char op;
    };
    std::vector<MetaPred> meta_preds;
    for (const char *p = idxStr; p && *p;) {
        MetaPred mp{atoi(p), '='};
        while (*p >= '0' && *p <= '9') p++;
        if (*p && *p != ',') mp.op = *p++;
        if (mp.col < 0 || mp.col >= int(vt.meta_cols.size()) || !MetaOpSql(mp.op))
            return vt.SetError("internal: bad idxStr meta predicate");
        meta_preds.push_back(mp);
        p = (*p == ',') ? p + 1 : nullptr;
    }
    const int meta_argv_base = next_argv;
    if (argc < meta_argv_base + int(meta_preds.size()))
        return vt.SetError("internal: KNN argv/idxStr mismatch");

    // 谓词预查：rowid 白名单（hash set 给图内过滤，list 给集合暴力保序遍历）。
    std::vector<sqlite3_int64> allowed_list;
    std::unordered_set<sqlite3_int64> allowed;
    if (!meta_preds.empty()) {
        std::string where;
        for (size_t i = 0; i < meta_preds.size(); i++) {
            if (i) where += " AND ";
            where += QuotedCol(vt.meta_cols[size_t(meta_preds[i].col)].name) + " " +
                     MetaOpSql(meta_preds[i].op) + " ?" + std::to_string(i + 1);
        }
        char *sql = sqlite3_mprintf("SELECT rowid FROM %s WHERE %s",
                                    vt.ShadowName("vectors").c_str(), where.c_str());
        sqlite3_stmt *st = nullptr;
        int rc = sqlite3_prepare_v2(vt.db, sql, -1, &st, nullptr);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) return rc;
        for (size_t i = 0; i < meta_preds.size(); i++)
            sqlite3_bind_value(st, int(i) + 1, argv[size_t(meta_argv_base) + i]);
        while ((rc = sqlite3_step(st)) == SQLITE_ROW)
            allowed_list.push_back(sqlite3_column_int64(st, 0));
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) return rc;
        // 无命中短路：结果必空，不碰图
        if (allowed_list.empty()) {
            cur.hits.clear();
            cur.pos = 0;
            return SQLITE_OK;
        }
        // 白名单足够小（≤ max(k*4, 暴力阈值)）：集合暴力，免图遍历+谓词补偿
        if (sqlite3_int64(allowed_list.size()) <=
            std::max<sqlite3_int64>(k * 4, vt.brute_force_threshold)) {
            return KnnBruteOverSet(vt, cur, q, k, allowed_list);
        }
        allowed.reserve(allowed_list.size() * 2);
        allowed.insert(allowed_list.begin(), allowed_list.end());
    }

    // 出口统一收口：OFFSET 下推时输出从第 offset 个命中起（hits 已物化
    // k+offset 个）。
    auto finish = [&](int rc2) {
        if (rc2 == SQLITE_OK && offset > 0)
            cur.pos = std::min<size_t>(size_t(offset), cur.hits.size());
        return rc2;
    };

    // M3：行数超过阈值走 HNSW 图（懒加载）；小表保持暴力（精度优先）。
    // 注：带谓词且到达此处时 |白名单| > 暴力阈值，全表行数必然也超阈值。
    if (CountVectors(vt) > vt.brute_force_threshold) {
        int rc = EnsureGraph(vt);
        if (rc != SQLITE_OK) return rc;
        if (!vt.graph) {
            // M9' DiskStore 模式且无 v2 段（写作废后未恢复）：查询路径不重建
            // （重建峰值违背 graph_memory_limit），退化暴力——带谓词时白名单
            // 点查，否则全表扫。恢复=下一次写事务 xSync 重建落盘。
            if (!allowed_list.empty())
                return finish(KnnBruteOverSet(vt, cur, q, k, allowed_list));
            return finish(KnnBruteScan(vt, cur, q, k));
        }
        std::vector<std::pair<double, int64_t>> hits;
        const size_t requested_k = size_t(k);
        auto pq_candidate_k = [&]() -> size_t {
            if (!vt.graph->UsesPQ()) return requested_k;
            // 图遍历宽度仍由 ef_search 控制，精排预算按 PQ 压缩强度调整：
            // 子段维度越大，单个 code 越粗，需要保留更多 ADC 候选。32 维下
            // pq_m=16(dsub=2) 取 4*k，pq_m=8(dsub=4) 取 8*k。旧逻辑固定
            // 1.25*ef_search，默认 top-10 总是做 200 次 B-tree 点查。
            const size_t dsub = vt.pq_m > 0 ? size_t(vt.dim) / vt.pq_m : 1;
            const size_t refine_factor = std::max<size_t>(4, dsub * 2);
            const size_t max_size = std::numeric_limits<size_t>::max();
            size_t candidates = requested_k > max_size / refine_factor
                ? max_size
                : requested_k * refine_factor;
            size_t available = allowed.empty() ? size_t(CountVectors(vt)) : allowed.size();
            return std::min(std::max(requested_k, candidates), available);
        };
        // DiskStore 段 I/O 可 throw（如写事务内 evict 脏段写回失败）——必须在
        // C 边界内 catch 转 SQL 错误，否则异常穿 SQLite C 帧 = UB/terminate。
        try {
            if (!allowed.empty()) {
                // ef 选择性补偿：通过率越低，图遍历途经的"废点"越多，按
                // N/|set| 放大 ef（上限 10×，防极端谓词把成本推到全图）。
                // 基数取 max(k, ef_search)：bridge 内部 ef=max(k, 传入值)，
                // 若只放大 ef_search，k 较大时补偿会被 max 整个吞掉。
                double sel = double(CountVectors(vt)) / double(allowed.size());
                double base_ef = std::max<double>(double(k), double(vt.ef_search));
                uint32_t ef_eff = uint32_t(base_ef * std::min(10.0, std::max(1.0, sel)));
                vt.graph->Search(q.data, pq_candidate_k(), ef_eff,
                                 [&](int64_t rid) { return allowed.count(rid) > 0; }, hits);
            } else {
                uint32_t ef = uint32_t(vt.ef_search);
                vt.graph->Search(q.data, pq_candidate_k(), ef, hits);
            }
        } catch (const std::exception &e) {
            return vt.SetError(e.what());
        }
        // distance 列重算为用户语义（与暴力路径/标量函数严格一致）。
        // 算法返回的 dist 是内核原始值，且 common 的 Distancer 主模板与各
        // Arch 特化对 COSINE 语义不一致（主模板=1-cos，特化=-cos；ODR 隐患，
        // 不同 TU/构建可能拿到不同实现）——排序单调同向不受影响，但输出值
        // 不可信。k 次点查+重算成本可忽略，换取对内核语义漂移彻底免疫。
        const auto dist_fn = GetDistanceFn(vt.metric);
        char *sql = sqlite3_mprintf("SELECT vec FROM %s WHERE rowid = ?1",
                                    vt.ShadowName("vectors").c_str());
        sqlite3_stmt *st = nullptr;
        rc = sqlite3_prepare_v2(vt.db, sql, -1, &st, nullptr);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) return rc;
        cur.hits.clear();
        cur.hits.reserve(hits.size());
        for (const auto &h : hits) {
            sqlite3_reset(st);
            sqlite3_bind_int64(st, 1, h.second);
            double d = h.first;
            if (sqlite3_step(st) == SQLITE_ROW &&
                sqlite3_column_bytes(st, 0) == vt.dim * 4) {
                d = dist_fn(q.data, static_cast<const float *>(sqlite3_column_blob(st, 0)),
                            static_cast<uint16_t>(vt.dim));
            }
            cur.hits.push_back({d, h.second});
        }
        sqlite3_finalize(st);
        std::sort(cur.hits.begin(), cur.hits.end(),
                  [](const GraphIndexCursor::Hit &a, const GraphIndexCursor::Hit &b) {
                      return a.dist < b.dist;
                  });
        if (cur.hits.size() > requested_k) cur.hits.resize(requested_k);
        cur.pos = 0;
        return finish(SQLITE_OK);
    }

    return finish(KnnBruteScan(vt, cur, q, k));
}

int vtabFilter(sqlite3_vtab_cursor *pCur, int idxNum, const char *idxStr, int argc,
               sqlite3_value **argv) {
    auto *cur = asCursor(pCur);
    auto *vt = asVtab(pCur->pVtab);
    CheckCookie(*vt);  // 他端连接提交过图变更 → 作废本端缓存
    cur->plan = PlanOf(idxNum);
    sqlite3_finalize(cur->scan_stmt);
    cur->scan_stmt = nullptr;
    cur->hits.clear();
    cur->pos = 0;
    cur->meta_rowid = -1;  // meta 行缓存失效（meta_stmt 本身可跨 Filter 复用）

    if (cur->plan == PLAN_KNN) {
        if (argc < 2) return vt->SetError("internal: KNN plan expects 2 args");
        return FilterKnn(*vt, *cur, idxNum, idxStr, argc, argv);
    }
    std::string meta_sel;
    for (const auto &mc : vt->meta_cols) meta_sel += ", " + QuotedCol(mc.name);
    char *sql;
    if (cur->plan == PLAN_ROWID) {
        // rowid 点查计划：单行结果，复用 scan 游标机制
        sql = sqlite3_mprintf("SELECT rowid, vec%s FROM %s WHERE rowid = ?1",
                              meta_sel.c_str(), vt->ShadowName("vectors").c_str());
    } else {
        sql = sqlite3_mprintf("SELECT rowid, vec%s FROM %s ORDER BY rowid",
                              meta_sel.c_str(), vt->ShadowName("vectors").c_str());
    }
    int rc = sqlite3_prepare_v2(vt->db, sql, -1, &cur->scan_stmt, nullptr);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) return rc;
    if (cur->plan == PLAN_ROWID && argc >= 1) sqlite3_bind_value(cur->scan_stmt, 1, argv[0]);
    cur->scan_eof = (sqlite3_step(cur->scan_stmt) != SQLITE_ROW);
    return SQLITE_OK;
}

int vtabNext(sqlite3_vtab_cursor *pCur) {
    auto *cur = asCursor(pCur);
    if (cur->plan == PLAN_KNN) {
        cur->pos++;
    } else {
        cur->scan_eof = (sqlite3_step(cur->scan_stmt) != SQLITE_ROW);
    }
    return SQLITE_OK;
}

int vtabEof(sqlite3_vtab_cursor *pCur) {
    auto *cur = asCursor(pCur);
    return cur->plan == PLAN_KNN ? cur->pos >= cur->hits.size() : cur->scan_eof;
}

int vtabColumn(sqlite3_vtab_cursor *pCur, sqlite3_context *ctx, int col) {
    auto *cur = asCursor(pCur);
    auto *vt = asVtab(pCur->pVtab);
    // UPDATE 取未变列值（OPFLAG_NOCHNG）时不设结果——保持 nochange 标记，
    // 让 xUpdate 的 sqlite3_value_nochange 生效。否则 result_value/null 会
    // 消除标记：meta-only UPDATE 的跳图快路径变死代码（每次标签变更付全
    // 向量摘除+重插+全量重写），且 KNN 计划的 UPDATE 因 embedding 列回
    // NULL 直接报错。
    if (sqlite3_vtab_nochange(ctx)) return SQLITE_OK;
    const int n_meta = int(vt->meta_cols.size());
    if (cur->plan == PLAN_KNN) {
        if (col == vt->ColDistance()) {
            sqlite3_result_double(ctx, cur->hits[cur->pos].dist);
        } else if (col >= 1 && col <= n_meta) {
            // KNN 模式 meta 列点查回吐：供 SQLite 引擎层评估未下推的 WHERE
            // 谓词（M6' 的 KNN+后过滤形态；M7' 升级为图内过滤）。
            // 持久化 stmt（取全部 meta 列）+ 当前行缓存：此前每 行×列 一次
            // mprintf+prepare+finalize，k=100×3 列即 300 次编译同一 SQL。
            if (!cur->meta_stmt) {
                std::string cols;
                for (const auto &mc : vt->meta_cols) {
                    if (!cols.empty()) cols += ", ";
                    cols += QuotedCol(mc.name);
                }
                char *sql = sqlite3_mprintf("SELECT %s FROM %s WHERE rowid = ?1",
                                            cols.c_str(), vt->ShadowName("vectors").c_str());
                int rc = sqlite3_prepare_v2(vt->db, sql, -1, &cur->meta_stmt, nullptr);
                sqlite3_free(sql);
                if (rc != SQLITE_OK) {
                    sqlite3_result_null(ctx);
                    return SQLITE_OK;
                }
            }
            sqlite3_int64 rid = cur->hits[cur->pos].rowid;
            if (cur->meta_rowid != rid) {
                sqlite3_reset(cur->meta_stmt);
                sqlite3_bind_int64(cur->meta_stmt, 1, rid);
                cur->meta_row_ok = (sqlite3_step(cur->meta_stmt) == SQLITE_ROW);
                cur->meta_rowid = rid;
            }
            if (cur->meta_row_ok) {
                sqlite3_result_value(ctx, sqlite3_column_value(cur->meta_stmt, col - 1));
            } else {
                sqlite3_result_null(ctx);
            }
        } else {
            sqlite3_result_null(ctx);  // KNN 模式不回吐向量本体（用 rowid join 原表）
        }
        return SQLITE_OK;
    }
    if (col == COL_EMBEDDING) {
        sqlite3_result_value(ctx, sqlite3_column_value(cur->scan_stmt, 1));
    } else if (col >= 1 && col <= n_meta) {
        sqlite3_result_value(ctx, sqlite3_column_value(cur->scan_stmt, 1 + col));
    } else {
        sqlite3_result_null(ctx);
    }
    return SQLITE_OK;
}

int vtabRowid(sqlite3_vtab_cursor *pCur, sqlite3_int64 *pRowid) {
    auto *cur = asCursor(pCur);
    *pRowid = cur->plan == PLAN_KNN ? cur->hits[cur->pos].rowid
                                    : sqlite3_column_int64(cur->scan_stmt, 0);
    return SQLITE_OK;
}

// 作废内存图与持久化段（写事务内；shadow 段删除随宿主事务原子）。下次
// 查询/写事务按协议重建。row_count 不动（与 %_vectors 仍一致）。
int DropGraph(GraphIndexVtab &vt) {
    vt.graph.reset();
    vt.graph_dirty = false;
    return InvalidatePersistedGraph(vt);
}

// INSERT/DELETE/UPDATE。argv 布局（SQLite vtab 约定）：
//   DELETE: argc=1, argv[0]=旧 rowid
//   INSERT: argc=N+2, argv[0]=NULL, argv[1]=新 rowid（或 NULL 自动分配）, argv[2..]=列值
//   UPDATE: argc=N+2, argv[0]=旧 rowid, argv[1]=新 rowid, argv[2..]=列值
int vtabUpdate(sqlite3_vtab *pVtab, int argc, sqlite3_value **argv,
               sqlite3_int64 *pRowid) {
    auto *vt = asVtab(pVtab);
    CheckCookie(*vt);  // 用 stale 图做增量写会把他端已提交的行从持久化图抹掉
    // DELETE：tid 摘除（MySQL 二级索引 delete-mark 范式的精确版）——把 rowid
    // 从其节点的 tids 摘掉，空壳节点仍参与 HNSW 导航（保连通性）但天然零输出，
    // elems 变更随 xSync 落盘。空壳占比 ≥20% 时图质量与空间退化 → 作废重建
    //（对应 MySQL purge）。图加载不了（无段等）也走作废兜底。
    if (argc == 1) {
        sqlite3_int64 del_rowid = sqlite3_value_int64(argv[0]);
        char *sql = sqlite3_mprintf("DELETE FROM %s WHERE rowid = %lld",
                                    vt->ShadowName("vectors").c_str(), del_rowid);
        int rc = ExecFmt(vt->db, sql);
        sqlite3_free(sql);
        if (rc != SQLITE_OK) return rc;
        if (sqlite3_changes(vt->db) == 0) return SQLITE_OK;  // 行不存在：无事可做
        if (vt->row_count > 0) vt->row_count--;
        TryOpenGraph(*vt);  // 图未加载时先 open（段在即可摘除，免作废）
        if (vt->graph && vt->graph->RemoveTid(del_rowid)) {
            sqlite3_int64 dead = sqlite3_int64(vt->graph->DeadNodeCount());
            sqlite3_int64 total = sqlite3_int64(vt->graph->Count());  // 图节点数（含空壳）
            if (dead * 5 < total) {  // 空壳 < 20%：保留
                vt->graph_dirty = true;  // mem 模式 xSync 全量重写
                return SQLITE_OK;
            }
            // 空壳占比 ≥20% → 作废重建
        }
        return DropGraph(*vt);
    }
    // special insert（fts5 风格）：INSERT INTO t(t) VALUES('ef_search=N')
    // 运行时改参，同连接即时生效并持久化进 config，不触碰数据与内存图。
    // 仅接受纯命令 INSERT：UPDATE SET <t>='cmd' 或携带数据列的混合形态拒绝
    //（防 UPDATE 误改配置丢行更新 / INSERT 静默丢数据行）。
    const int col_cmd = vt->ColCmd();
    if (argc > 2 + col_cmd && !sqlite3_value_nochange(argv[2 + col_cmd]) &&
        sqlite3_value_type(argv[2 + col_cmd]) == SQLITE_TEXT) {
        if (sqlite3_value_type(argv[0]) != SQLITE_NULL)
            return vt->SetError("config command must use INSERT INTO t(t) VALUES('key=value')");
        if (sqlite3_value_type(argv[2 + COL_EMBEDDING]) != SQLITE_NULL)
            return vt->SetError("config command cannot be combined with data columns");
        std::string cmd = reinterpret_cast<const char *>(sqlite3_value_text(argv[2 + col_cmd]));
        size_t eq = cmd.find('=');
        std::string key = cmd.substr(0, eq == std::string::npos ? cmd.size() : eq);
        TrimSpaces(key);
        if (key == "ef_search" && eq != std::string::npos) {
            int v = atoi(cmd.c_str() + eq + 1);
            if (v < 1) return vt->SetError("ef_search must be positive");
            vt->ef_search = v;
            ConfigSet(*vt, "ef_search", std::to_string(v));
            return SQLITE_OK;
        }
        if (key == "brute_force_threshold" && eq != std::string::npos) {
            sqlite3_int64 v = atoll(cmd.c_str() + eq + 1);
            if (v < 0) return vt->SetError("brute_force_threshold must be >= 0");
            vt->brute_force_threshold = v;
            ConfigSet(*vt, "brute_force_threshold", std::to_string(v));
            return SQLITE_OK;
        }
        if (key == "graph_memory_limit" && eq != std::string::npos) {
            sqlite3_int64 v = atoll(cmd.c_str() + eq + 1);
            if (v < 0) return vt->SetError("graph_memory_limit must be >= 0");
            vt->graph_memory_limit = v;
            ConfigSet(*vt, "graph_memory_limit", std::to_string(v));
            // 模式分流在图加载时决定：作废当前图让下次按新 limit 重新分流。
            // 必须连段一起作废（DropGraph）：若本事务此前的 DELETE 走了
            // keep 分支（段未清、靠 xSync 全量重写兑现摘除），仅 reset 内存
            // 图会取消重写、让仍含已删 tid 的陈旧段幸存 COMMIT → 幽灵行复活。
            return DropGraph(*vt);
        }
        return vt->SetError(("unknown command: " + cmd).c_str());
    }

    // INSERT / UPDATE 的向量值。UPDATE 未触及向量列时（meta-only 更新，
    // sqlite3_value_nochange 为真）值内容 unspecified——跳过解析与 SET，
    // 且向量未变 ⇒ 图仍有效，免去全图重建（Stage B 标签变更高频场景）。
    sqlite3_value *vec_val = argv[2 + COL_EMBEDDING];
    bool is_insert = sqlite3_value_type(argv[0]) == SQLITE_NULL;
    bool vec_nochange = !is_insert && sqlite3_value_nochange(vec_val);
    VectorView v;
    if (!vec_nochange) {
        if (sqlite3_value_type(vec_val) == SQLITE_NULL)
            return vt->SetError("embedding must not be NULL");
        std::string err;
        if (!GetVector(vec_val, v, err)) return vt->SetError(err.c_str());
        if (static_cast<int>(v.dim) != vt->dim) {
            err = "vector dimension " + std::to_string(v.dim) + " != index dimension " +
                  std::to_string(vt->dim);
            return vt->SetError(err.c_str());
        }
    }
    // distance/k 列不可写
    // read-only 检查须放过 nochange（UPDATE 未 SET 的列经 xColumn nochange
    // 路径传回，值内容 unspecified——不能按"非 NULL"误判为用户写入）。
    auto col_written = [&](int col) {
        sqlite3_value *cv = argv[2 + col];
        return !sqlite3_value_nochange(cv) && sqlite3_value_type(cv) != SQLITE_NULL;
    };
    if (col_written(vt->ColDistance()) || col_written(vt->ColK()))
        return vt->SetError("distance/k columns are read-only");

    sqlite3_int64 old_rowid = is_insert ? 0 : sqlite3_value_int64(argv[0]);
    bool has_new_rowid = sqlite3_value_type(argv[1]) != SQLITE_NULL;
    sqlite3_int64 new_rowid = has_new_rowid ? sqlite3_value_int64(argv[1]) : 0;

    // meta 值从 argv[2 + 1 + i] 透传（参数位 ?4 起；?1=rowid ?2=vec ?3=UPDATE 的旧 rowid）。
    // stmt 按形态缓存（schema 固定 → SQL 固定），多行 DML 免每行 prepare。
    const int n_meta = int(vt->meta_cols.size());
    sqlite3_stmt *st = nullptr;
    int rc;
    if (is_insert) {
        sqlite3_stmt *&cached = vt->dml_insert[has_new_rowid ? 1 : 0];
        if (!cached) {
            std::string cols = "vec", vals = "?2";
            if (has_new_rowid) { cols = "rowid, " + cols; vals = "?1, " + vals; }
            for (int i = 0; i < n_meta; i++) {
                cols += ", " + QuotedCol(vt->meta_cols[i].name);
                vals += ", ?" + std::to_string(4 + i);
            }
            char *sql = sqlite3_mprintf("INSERT INTO %s(%s) VALUES (%s)",
                                        vt->ShadowName("vectors").c_str(), cols.c_str(),
                                        vals.c_str());
            rc = sqlite3_prepare_v2(vt->db, sql, -1, &cached, nullptr);
            sqlite3_free(sql);
            if (rc != SQLITE_OK) return rc;
        }
        st = cached;
        sqlite3_reset(st);
        if (has_new_rowid) sqlite3_bind_int64(st, 1, new_rowid);
    } else {
        // UPDATE（含 rowid 变更；vec 未触及时不 SET vec）
        sqlite3_stmt *&cached = vt->dml_update[vec_nochange ? 1 : 0];
        if (!cached) {
            std::string sets = vec_nochange ? "rowid = ?1" : "rowid = ?1, vec = ?2";
            for (int i = 0; i < n_meta; i++)
                sets += ", " + QuotedCol(vt->meta_cols[i].name) + " = ?" + std::to_string(4 + i);
            char *sql = sqlite3_mprintf("UPDATE %s SET %s WHERE rowid = ?3",
                                        vt->ShadowName("vectors").c_str(), sets.c_str());
            rc = sqlite3_prepare_v2(vt->db, sql, -1, &cached, nullptr);
            sqlite3_free(sql);
            if (rc != SQLITE_OK) return rc;
        }
        st = cached;
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, has_new_rowid ? new_rowid : old_rowid);
        sqlite3_bind_int64(st, 3, old_rowid);
    }
    if (!vec_nochange)
        sqlite3_bind_blob(st, 2, v.data, static_cast<int>(v.dim * 4), SQLITE_TRANSIENT);
    for (int i = 0; i < n_meta; i++)
        sqlite3_bind_value(st, 4 + i, argv[2 + 1 + i]);
    rc = sqlite3_step(st);
    sqlite3_reset(st);  // 缓存复用：用后即 reset（活跃语句钉读事务）
    if (rc != SQLITE_DONE) {
        if (rc == SQLITE_CONSTRAINT) return SQLITE_CONSTRAINT;  // 重复 rowid 等
        return vt->SetError(sqlite3_errmsg(vt->db));
    }
    sqlite3_int64 final_rowid =
        is_insert ? (has_new_rowid ? new_rowid : sqlite3_last_insert_rowid(vt->db))
                  : (has_new_rowid ? new_rowid : old_rowid);
    if (is_insert) *pRowid = final_rowid;

    // 图维护（INSERT/UPDATE 均增量，MySQL 二级索引 delete-mark+insert 范式）：
    //   INSERT          = 图内增量插入（旧节点若有同 rowid 的残留 tid 已被
    //                     此前 DELETE/UPDATE 摘除，无二义）
    //   UPDATE 向量     = RemoveTid(旧 rowid) + Insert(新向量)——旧节点变空壳
    //   UPDATE 仅 rowid = RemoveTid(旧) + Insert(原向量, 新 rowid)
    //   meta-only UPDATE = 不触图（向量拓扑未变）
    // 持久化段处置分模式：MemStore=清段+xSync 全量重写；DiskStore=不清段
    //（未动段仍有效，dirty 段 xSync UPSERT 覆盖——清了反而丢未动段）。
    bool rowid_changed = !is_insert && has_new_rowid && new_rowid != old_rowid;
    if (vec_nochange && !rowid_changed) return SQLITE_OK;
    if (is_insert && vt->row_count >= 0) vt->row_count++;

    // 图未加载时先 open：DELETE/UPDATE 需要它摘 tid；INSERT 需要它增量插入
    //（否则 fresh 连接的首个 INSERT 会把有效持久图整个作废，1M 行表的代价是
    // 写事务内整图重建）。open 失败（无段等）才走作废兜底。
    TryOpenGraph(*vt);
    bool can_incremental = vt->graph != nullptr;
    if (can_incremental && !is_insert) {
        // UPDATE：摘旧 tid。摘不到（图与 %_vectors 漂移）作废兜底。
        can_incremental = vt->graph->RemoveTid(old_rowid);
    }
    if (!can_incremental) {
        // 图缺失/摘除失败：作废重建兜底
        return DropGraph(*vt);
    }
    // 增量写两模式统一不清段（xSync 按 dirty 集 UPSERT；mem 全量重写只剩
    // 重建后首次落盘=NeedsFullRewrite）。
    // 取要插入的向量：INSERT/UPDATE 向量用解析好的 v.data；UPDATE 仅 rowid
    //（vec 未触及，内容 unspecified）从 %_vectors 点查新行。
    std::vector<float> vec_buf;
    const float *ins_vec = v.data;
    if (vec_nochange) {
        if (!vt->vec_fetch_stmt) {
            char *sql = sqlite3_mprintf("SELECT vec FROM %s WHERE rowid = ?1",
                                        vt->ShadowName("vectors").c_str());
            rc = sqlite3_prepare_v2(vt->db, sql, -1, &vt->vec_fetch_stmt, nullptr);
            sqlite3_free(sql);
            if (rc != SQLITE_OK) {
                // RemoveTid 之后的失败窗口必须作废半变异图
                vt->graph.reset();
                vt->graph_dirty = false;
                return rc;
            }
        }
        sqlite3_stmt *vst = vt->vec_fetch_stmt;
        sqlite3_reset(vst);
        sqlite3_bind_int64(vst, 1, final_rowid);
        if (sqlite3_step(vst) == SQLITE_ROW && sqlite3_column_bytes(vst, 0) == vt->dim * 4) {
            const float *p = static_cast<const float *>(sqlite3_column_blob(vst, 0));
            vec_buf.assign(p, p + vt->dim);
            ins_vec = vec_buf.data();
        }
        sqlite3_reset(vst);
        if (vec_buf.empty()) {  // 取不到：作废兜底
            return DropGraph(*vt);
        }
    }
    try {
        vt->graph->Insert(ins_vec, final_rowid);
    } catch (const std::exception &e) {
        // DiskStore evict 脏段写回失败等：异常不得穿 SQLite C 帧。图状态
        // 不可信，作废兜底。
        DropGraph(*vt);
        return vt->SetError(e.what());
    }
    vt->graph_dirty = true;
    // 空壳占比 ≥20% → 作废重建（对应 MySQL purge）
    sqlite3_int64 dead = sqlite3_int64(vt->graph->DeadNodeCount());
    if (dead * 5 >= sqlite3_int64(vt->graph->Count())) {
        return DropGraph(*vt);
    }
    return SQLITE_OK;
}

// ---------- 事务钩子（M3） ----------
// 写入本身全经 shadow 表 SQL（随宿主事务原子回滚）；钩子只管内存图的一致性：
//   xSync    : 把 dirty 内存图序列化写回 %_graph（可失败的工作必须放这里）
//   xRollback: 事务回滚 → 内存图可能含已回滚的插入，作废
//   xCommit  : 清理无事可做（blob 已在 xSync 落盘）

int vtabBegin(sqlite3_vtab *) { return SQLITE_OK; }

int SyncImpl(GraphIndexVtab *vt) {
    // M9' 恢复路径：DiskStore 形态（超 limit）下写操作作废了图与 v2 段，查询
    // 路径不重建（峰值违背 limit）只能暴力——借写事务窗口两阶段构建+落盘
    //（M9'c：前 K 行并行内存建图 flush，剩余流式磁盘逐条，峰值 ≈ limit），
    // 之后查询恢复 DiskStore open。
    if (!vt->graph) {
        sqlite3_int64 n = CountVectors(*vt);
        if (!OverLimit(*vt) || n <= vt->brute_force_threshold) return SQLITE_OK;
        // 仅在 v2 段缺失时重建（有段说明图本就有效，无事可做）
        std::vector<char> probe;
        auto probe_reader = MakeSegReader(*vt);
        if (probe_reader && probe_reader(0, 0, probe)) return SQLITE_OK;
        int rc = BuildTwoPhase(*vt, n);
        if (rc != SQLITE_OK) return rc;
        if (!vt->graph) return SQLITE_OK;  // 空表等边界
        vt->cookie_bump_pending = true;    // 重建写了段
    }

    auto writer = MakeSegWriter(*vt);
    if (vt->graph->IsDiskMode()) {
        // DiskStore：只写 dirty 段 + 常驻（meta/elems/upper）
        if (!vt->graph->HasDirty()) return SQLITE_OK;
        if (!vt->graph->SerializeV2(writer)) return vt->SetError(sqlite3_errmsg(vt->db));
        vt->cookie_bump_pending = true;
        return SQLITE_OK;
    }
    if (!vt->graph_dirty) return SQLITE_OK;
    // 全内存：重建后首次落盘=清旧段+全量；增量写后=只 UPSERT dirty 段+常驻
    //（与 disk 模式统一协议，免单行 insert 的 commit 重写全图）。
    if (vt->graph->NeedsFullRewrite()) {
        int rc = InvalidatePersistedGraph(*vt);
        if (rc != SQLITE_OK) return rc;
    }
    if (!vt->graph->SerializeV2(writer)) return vt->SetError(sqlite3_errmsg(vt->db));
    vt->graph_dirty = false;
    vt->cookie_bump_pending = true;
    return SQLITE_OK;
}

int vtabSync(sqlite3_vtab *pVtab) {
    auto *vt = asVtab(pVtab);
    int rc = SyncImpl(vt);
    // 本事务变更过图/段：提交前 bump 跨连接 cookie（ConfigSet 随事务原子；
    // 失败回滚时 cookie_seen 与库面不一致会被下次 CheckCookie 自愈）。
    if (rc == SQLITE_OK && vt->cookie_bump_pending) {
        vt->cookie_seen = vt->cookie_seen < 0 ? 1 : vt->cookie_seen + 1;
        ConfigSet(*vt, "cookie", std::to_string(vt->cookie_seen));
        vt->cookie_bump_pending = false;
    }
    return rc;
}

int vtabCommit(sqlite3_vtab *) { return SQLITE_OK; }

int vtabRollback(sqlite3_vtab *pVtab) {
    auto *vt = asVtab(pVtab);
    vt->graph.reset();
    vt->graph_dirty = false;
    // 事务内 xUpdate 已对 row_count 做过 ±1，回滚后缓存与 %_vectors 漂移
    //（且永不自愈——只有 ==-1 才重数），必须一并失效。
    vt->row_count = -1;
    vt->cookie_bump_pending = false;  // 段变更已随事务回滚
    return SQLITE_OK;
}

// savepoint 钩子（语句级 abort 与 ROLLBACK TO 都经此）：savepoint 区间内的
// 图变异（增量 Insert/RemoveTid）无法精确撤销——保守作废内存图。shadow 表
// 的 SQL 写（含 %_graph 段）由核心随 savepoint 回滚，作废后下次重建即与
// %_vectors 一致。不实现此族时，多行 DML 中途失败只回滚 SQL 不回滚图，
// xSync 会把分叉图持久化成幽灵行。
int vtabSavepoint(sqlite3_vtab *, int) { return SQLITE_OK; }

int vtabRelease(sqlite3_vtab *, int) { return SQLITE_OK; }

int vtabRollbackTo(sqlite3_vtab *pVtab, int) {
    auto *vt = asVtab(pVtab);
    vt->graph.reset();
    vt->graph_dirty = false;
    vt->row_count = -1;
    // 注：cookie_bump_pending 保守保留——savepoint 之前的段变更可能仍在
    return SQLITE_OK;
}

int vtabShadowName(const char *name) {
    return strcmp(name, "config") == 0 || strcmp(name, "vectors") == 0 ||
           strcmp(name, "graph") == 0;  // graph 留给 M3
}

}  // namespace

// iVersion=3 启用 xShadowName（DEFENSIVE 模式下保护 shadow 表不被普通 SQL 误写）。
// M3 填 xBegin/xSync/xCommit/xRollback（内存图的事务钩子；M2 全部写入直接经
// shadow 表 SQL，事务性由宿主天然保证，无需钩子）。
const sqlite3_module vexdb_graph_index_module = {
    /* iVersion      */ 3,
    /* xCreate       */ vtabCreate,
    /* xConnect      */ vtabConnect,
    /* xBestIndex    */ vtabBestIndex,
    /* xDisconnect   */ vtabDisconnect,
    /* xDestroy      */ vtabDestroy,
    /* xOpen         */ vtabOpen,
    /* xClose        */ vtabClose,
    /* xFilter       */ vtabFilter,
    /* xNext         */ vtabNext,
    /* xEof          */ vtabEof,
    /* xColumn       */ vtabColumn,
    /* xRowid        */ vtabRowid,
    /* xUpdate       */ vtabUpdate,
    /* xBegin        */ vtabBegin,
    /* xSync         */ vtabSync,
    /* xCommit       */ vtabCommit,
    /* xRollback     */ vtabRollback,
    /* xFindFunction */ nullptr,
    /* xRename       */ nullptr,
    /* xSavepoint    */ vtabSavepoint,
    /* xRelease      */ vtabRelease,
    /* xRollbackTo   */ vtabRollbackTo,
    /* xShadowName   */ vtabShadowName,
};
