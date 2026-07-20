// VexFS 平台挂载合同（C ABI）。
//
// FSKit、libfuse 和 WinFsp 适配层只依赖这个头。实现通过公开的 vexfs_* SQL
// 函数访问 SQLite，数据库仍然是状态和事务的唯一管理者。
#ifndef VEXFS_MOUNT_CONTRACT_H
#define VEXFS_MOUNT_CONTRACT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VEXFS_MOUNT_ABI_VERSION 3u
#define VEXFS_MOUNT_OPEN_NO_CREATE 1u
#define VEXFS_MOUNT_EXCLUSIVE_GATEWAY 2u

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
    VEXFS_MOUNT_UNSUPPORTED = 11
} vexfs_mount_status;

typedef struct vexfs_mount_error {
    vexfs_mount_status status;
    int sqlite_code;
    char message[512];
} vexfs_mount_error;

typedef struct vexfs_mount_config {
    uint32_t abi_version;
    const char *database_path;
    const char *workspace;
    uint32_t busy_timeout_ms;
    uint32_t flags;
} vexfs_mount_config;

typedef struct vexfs_mount_bytes {
    void *data;
    uint64_t size;
} vexfs_mount_bytes;

// 打开数据库、注册 vexdb_lite/VexFS、初始化内部表并创建（或复用）workspace。
vexfs_mount_status vexfs_mount_session_open(const vexfs_mount_config *config,
                                             vexfs_mount_session **session,
                                             vexfs_mount_error *error);
void vexfs_mount_session_close(vexfs_mount_session *session);

// 返回数据库合同、durability 和待恢复 staging 的 JSON 诊断信息。
vexfs_mount_status vexfs_mount_diagnostics(vexfs_mount_session *session,
                                           vexfs_mount_bytes *json,
                                           vexfs_mount_error *error);

vexfs_mount_status vexfs_mount_mkdir(vexfs_mount_session *session, const char *path,
                                     vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_write_file(vexfs_mount_session *session, const char *path,
                                          const void *data, uint64_t size,
                                          int64_t *version, vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_read_file(vexfs_mount_session *session, const char *path,
                                         vexfs_mount_bytes *content,
                                         vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_history(vexfs_mount_session *session, const char *path,
                                       vexfs_mount_bytes *json,
                                       vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_history_page(vexfs_mount_session *session, const char *path,
                                            uint32_t limit, int64_t before_version,
                                            vexfs_mount_bytes *json,
                                            vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_read_version(vexfs_mount_session *session, const char *path,
                                            int64_t version, vexfs_mount_bytes *content,
                                            vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_compare_versions(vexfs_mount_session *session, const char *path,
                                                int64_t from_version, int64_t to_version,
                                                vexfs_mount_bytes *json,
                                                vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_restore_version(vexfs_mount_session *session, const char *path,
                                               int64_t target_version,
                                               int64_t expected_version,
                                               int64_t *new_version,
                                               vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_stat(vexfs_mount_session *session, const char *path,
                                    vexfs_mount_bytes *json, vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_path_for_inode(vexfs_mount_session *session, int64_t inode,
                                               vexfs_mount_bytes *path,
                                               vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_list(vexfs_mount_session *session, const char *path,
                                    vexfs_mount_bytes *json, vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_move(vexfs_mount_session *session, const char *source,
                                    const char *destination, vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_rename(vexfs_mount_session *session, const char *source,
                                      const char *destination, int replace,
                                      vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_remove(vexfs_mount_session *session, const char *path,
                                      int recursive, vexfs_mount_error *error);

vexfs_mount_status vexfs_mount_handle_open(vexfs_mount_session *session, const char *path,
                                           const char *flags, const char *request_id,
                                           vexfs_mount_bytes *handle,
                                           vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_handle_stage_write(vexfs_mount_session *session,
                                                  const char *handle, uint64_t offset,
                                                  const void *data, uint64_t size,
                                                  const char *request_id,
                                                  int64_t *generation,
                                                  vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_handle_truncate(vexfs_mount_session *session,
                                               const char *handle, uint64_t size,
                                               const char *request_id,
                                               int64_t *generation,
                                               vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_handle_read(vexfs_mount_session *session, const char *handle,
                                           uint64_t offset, uint64_t length,
                                           vexfs_mount_bytes *content,
                                           vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_handle_publish(vexfs_mount_session *session,
                                              const char *handle, int64_t generation,
                                              const char *durability,
                                              const char *request_id, int64_t *version,
                                              vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_handle_close(vexfs_mount_session *session,
                                            const char *handle, int retain_unpublished,
                                            const char *request_id,
                                            vexfs_mount_bytes *state,
                                            vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_synchronize(vexfs_mount_session *session,
                                           const char *request_id, int64_t *published,
                                           vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_reclaim(vexfs_mount_session *session,
                                       const char *request_id, int64_t *reclaimed,
                                       vexfs_mount_error *error);

// 释放本合同返回的 vexfs_mount_bytes.data。
void vexfs_mount_free(void *memory);

#ifdef __cplusplus
}
#endif

#endif  // VEXFS_MOUNT_CONTRACT_H
