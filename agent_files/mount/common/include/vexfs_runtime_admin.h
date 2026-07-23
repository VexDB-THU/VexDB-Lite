// CLI 和管理工具允许调用的管理面接口。文件 I/O 来自 runtime_io。
#ifndef VEXFS_RUNTIME_ADMIN_H
#define VEXFS_RUNTIME_ADMIN_H

#include "vexfs_runtime_io.h"

#ifdef __cplusplus
extern "C" {
#endif

vexfs_mount_status vexfs_mount_diagnostics(vexfs_mount_session *session,
                                           vexfs_mount_bytes *json,
                                           vexfs_mount_error *error);
// flags=0 performs a deep checksum scan; VEXFS_MOUNT_CHECK_QUICK skips BLOB hashing.
#define VEXFS_MOUNT_CHECK_QUICK 1u
vexfs_mount_status vexfs_mount_check(vexfs_mount_session *session, uint32_t flags,
                                     vexfs_mount_bytes *json,
                                     vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_grep(vexfs_mount_session *session, const char *path,
                                    const char *pattern, uint32_t flags,
                                    uint32_t limit, vexfs_mount_bytes *json,
                                    vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_grep_index(vexfs_mount_session *session,
                                          const char *action,
                                          vexfs_mount_bytes *json,
                                          vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_history(vexfs_mount_session *session, const char *path,
                                       vexfs_mount_bytes *json,
                                       vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_history_page(vexfs_mount_session *session, const char *path,
                                            uint32_t limit, int64_t before_version,
                                            vexfs_mount_bytes *json,
                                            vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_read_version(vexfs_mount_session *session, const char *path,
                                            int64_t version,
                                            vexfs_mount_bytes *content,
                                            vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_compare_versions(vexfs_mount_session *session,
                                                const char *path,
                                                int64_t from_version,
                                                int64_t to_version,
                                                vexfs_mount_bytes *json,
                                                vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_restore_version(vexfs_mount_session *session, const char *path,
                                               int64_t target_version,
                                               int64_t expected_version,
                                               int64_t *new_version,
                                               vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_workspace_head(vexfs_mount_session *session,
                                              int64_t *head_commit,
                                              vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_snapshot_create(vexfs_mount_session *session,
                                               const char *name, uint32_t flags,
                                               int64_t *commit,
                                               vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_snapshot_list(vexfs_mount_session *session,
                                             vexfs_mount_bytes *json,
                                             vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_snapshot_show(vexfs_mount_session *session,
                                             const char *name,
                                             vexfs_mount_bytes *json,
                                             vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_snapshot_diff(vexfs_mount_session *session,
                                             const char *from, const char *to,
                                             vexfs_mount_bytes *json,
                                             vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_snapshot_drop(vexfs_mount_session *session,
                                             const char *name,
                                             vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_snapshot_restore(vexfs_mount_session *session,
                                                const char *name, int64_t expected_head,
                                                int64_t *new_commit,
                                                vexfs_mount_error *error);

#ifdef __cplusplus
}
#endif

#endif  // VEXFS_RUNTIME_ADMIN_H
