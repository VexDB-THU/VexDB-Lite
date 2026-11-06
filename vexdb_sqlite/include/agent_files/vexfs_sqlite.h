// VexFS SQLite 文件系统合同。
//
// 这是数据库插件能力，不是独立 SDK：注册后，宿主通过 SQL 操作文件、目录和
// 挂载写入句柄。所有写操作都参加调用者当前 SQLite 事务，插件不会自行提交。
#ifndef VEXFS_SQLITE_H
#define VEXFS_SQLITE_H

#ifdef VEXDB_SQLITE_CORE
#include "sqlite3.h"
#else
#include "sqlite3ext.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// VexFS 私有扩展错误码。低 8 位保留 SQLite 主错误码，使不了解该扩展码的
// 调用方仍可按 SQLITE_CONSTRAINT 处理；挂载合同则可还原成准确的 POSIX 错误。
#define VEXFS_SQLITE_CONSTRAINT_NOT_EMPTY (SQLITE_CONSTRAINT | (200 << 8))

// 注册 vexfs_* SQL 函数。成功返回 SQLITE_OK。
int vexfs_sqlite_register(sqlite3 *db);

#ifdef __cplusplus
}
#endif

#endif  // VEXFS_SQLITE_H
