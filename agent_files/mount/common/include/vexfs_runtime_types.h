// VexFS runtime 的稳定 C 类型。数据库原生细节只能出现在 diagnostic 字段中。
#ifndef VEXFS_RUNTIME_TYPES_H
#define VEXFS_RUNTIME_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VEXFS_RUNTIME_ABI_VERSION 1u
#define VEXFS_RUNTIME_OPEN_NO_CREATE 1u
#define VEXFS_RUNTIME_EXCLUSIVE_GATEWAY 2u
#define VEXFS_RUNTIME_BACKEND_SQLITE "sqlite"
#define VEXFS_RUNTIME_BACKEND_POSTGRESQL "postgresql"
#define VEXFS_MOUNT_SYMLINK_MAX 4096u
#define VEXFS_MOUNT_TIME_ACCESS 1u
#define VEXFS_MOUNT_TIME_MODIFY 2u
#define VEXFS_MOUNT_GREP_IGNORE_CASE 1u
#define VEXFS_MOUNT_GREP_FILES_ONLY 2u
#define VEXFS_SNAPSHOT_COMMITTED_ONLY 1u

typedef enum vexfs_mount_xattr_policy {
    VEXFS_MOUNT_XATTR_ALWAYS_SET = 0,
    VEXFS_MOUNT_XATTR_MUST_CREATE = 1,
    VEXFS_MOUNT_XATTR_MUST_REPLACE = 2,
    VEXFS_MOUNT_XATTR_DELETE = 3
} vexfs_mount_xattr_policy;

typedef struct vexfs_mount_session vexfs_mount_session;

typedef enum vexfs_mount_status {
    VEXFS_MOUNT_OK = 0,
    VEXFS_MOUNT_INVALID_ARGUMENT = 1,
    VEXFS_MOUNT_NOT_FOUND = 2,
    VEXFS_MOUNT_CONFLICT = 3,
    VEXFS_MOUNT_READ_ONLY = 4,
    VEXFS_MOUNT_BUSY = 5,
    VEXFS_MOUNT_DATABASE_ERROR = 6,
    VEXFS_MOUNT_INTERNAL_ERROR = 7,
    VEXFS_MOUNT_PERMISSION_DENIED = 8,
    VEXFS_MOUNT_NO_SPACE = 9,
    VEXFS_MOUNT_CORRUPTION = 10,
    VEXFS_MOUNT_UNSUPPORTED = 11,
    VEXFS_MOUNT_NOT_EMPTY = 12
} vexfs_mount_status;

typedef struct vexfs_mount_error {
    vexfs_mount_status status;
    int native_code;
    char backend[16];
    char message[512];
} vexfs_mount_error;

// connection 的格式由 backend 决定。SQLite 使用数据库文件路径；后续 PG
// 使用 DSN。principal 是 runtime 的请求身份，不再由平台层自己猜测。
typedef struct vexfs_mount_config {
    uint32_t abi_version;
    const char *backend;
    const char *connection;
    const char *workspace;
    const char *principal;
    uint32_t operation_timeout_ms;
    uint32_t flags;
} vexfs_mount_config;

typedef struct vexfs_mount_bytes {
    void *data;
    uint64_t size;
} vexfs_mount_bytes;

typedef struct vexfs_mount_visibility {
    int64_t workspace_head;
    uint64_t cache_generation;
    int external_commit;
} vexfs_mount_visibility;

#ifdef __cplusplus
}
#endif

#endif  // VEXFS_RUNTIME_TYPES_H
