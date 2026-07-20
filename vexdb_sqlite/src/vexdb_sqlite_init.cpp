// vexdb_lite SQLite 适配层入口 —— loadable 与静态注册两形态汇聚点。
#include "vexdb_sqlite_internal.h"
#ifndef VEXDB_SQLITE_CORE
SQLITE_EXTENSION_INIT1
#endif

#include "vexdb_sqlite.h"
#include "agent_files/vexfs_sqlite.h"
#include "functions/distance_functions.h"
#include "vtab/graph_index_vtab.h"

#ifndef VEXDB_SQLITE_GIT_HASH
#define VEXDB_SQLITE_GIT_HASH "unknown"
#endif
#ifndef VEXDB_SQLITE_BUILD_TIME
#define VEXDB_SQLITE_BUILD_TIME "unknown"
#endif

namespace {

void VexVersionFunc(sqlite3_context *ctx, int /*argc*/, sqlite3_value ** /*argv*/) {
    sqlite3_result_text(
        ctx,
        "vexdb_lite sqlite extension " VEXDB_SQLITE_GIT_HASH " (" VEXDB_SQLITE_BUILD_TIME ")",
        -1, SQLITE_STATIC);
}

}  // namespace

// 把全部对象注册到连接 db。loadable 与静态注册路径都汇聚到此。
//
// 注意：loadable 形态下，调用本函数前必须已经过 sqlite3_vexdblite_init（其中
// SQLITE_EXTENSION_INIT2 设置了 sqlite3_api 间接表），否则 sqlite3_* 调用会空指针。
// 宿主请勿在 loadable 下绕过 init 直接调本函数；静态注册（VEXDB_SQLITE_CORE）则
// 直接链接真实符号，可安全直调。
extern "C" int vexdb_sqlite_register(sqlite3 *db) {
    // 宿主版本门禁：sqlite3_value_nochange/vtab_nochange（3.22）经间接表槽位
    // 调用，旧宿主的 api 结构体更短——越界槽位是野函数指针；%_graph 段写的
    // UPSERT 语法需 3.24。低于线直接拒载，好过首个 UPDATE 时崩溃。
    //（LIMIT 下推需 3.38，但缺失时优雅降级为显式 k=，不作硬门禁。）
    if (sqlite3_libversion_number() < 3024000) {
        return SQLITE_ERROR;  // host SQLite too old (need >= 3.24)
    }
    int rc = sqlite3_create_module(db, "GRAPH_INDEX", &vexdb_graph_index_module, nullptr);
    if (rc != SQLITE_OK) return rc;

    rc = vexdb_sqlite_register_distance_functions(db);
    if (rc != SQLITE_OK) return rc;

    rc = vexfs_sqlite_register(db);
    if (rc != SQLITE_OK) return rc;

    rc = sqlite3_create_function(db, "vexdb_version", 0,
                                 SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr,
                                 VexVersionFunc, nullptr, nullptr);
    return rc;
}

extern "C" int sqlite3_vexdblite_init(sqlite3 *db, char ** /*pzErrMsg*/,
                                      const sqlite3_api_routines *pApi) {
#ifndef VEXDB_SQLITE_CORE
    SQLITE_EXTENSION_INIT2(pApi);
#else
    (void)pApi;
#endif
    return vexdb_sqlite_register(db);
}
