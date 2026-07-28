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
// Finds current workspace entries without reading file bodies. Empty pattern,
// kind, and cursor values disable those filters. Numeric filters use -1 when
// disabled. Results are ordered by binary path and use an exclusive path cursor.
vexfs_mount_status vexfs_mount_find(
    vexfs_mount_session *session, const char *path, const char *name_pattern,
    const char *kind, int64_t min_size, int64_t max_size,
    int64_t modified_after_ms, int64_t modified_before_ms,
    const char *after_path, uint32_t limit, vexfs_mount_bytes *json,
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
// Lists workspace commits from newest to oldest. before_commit=0 starts at HEAD;
// a returned next_before cursor is exclusive and can fetch the following page.
vexfs_mount_status vexfs_mount_workspace_log_page(
    vexfs_mount_session *session, uint32_t limit, int64_t before_commit,
    vexfs_mount_bytes *json, vexfs_mount_error *error);
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
// Restores and atomically preserves the pre-restore workspace as safety_name.
// The safety snapshot is created only when the restore transaction commits.
vexfs_mount_status vexfs_mount_snapshot_restore_safe(vexfs_mount_session *session,
                                                     const char *name,
                                                     int64_t expected_head,
                                                     const char *safety_name,
                                                     int64_t *new_commit,
                                                     vexfs_mount_error *error);
// Quota values use -1 for unlimited and non-negative values for a hard limit.
vexfs_mount_status vexfs_mount_quota_set(vexfs_mount_session *session,
                                         int64_t max_bytes, int64_t max_files,
                                         int64_t max_file_bytes,
                                         vexfs_mount_bytes *json,
                                         vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_retention_get(vexfs_mount_session *session,
                                             vexfs_mount_bytes *json,
                                             vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_retention_set(vexfs_mount_session *session,
                                             uint32_t keep_versions,
                                             uint32_t keep_days,
                                             vexfs_mount_bytes *json,
                                             vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_gc_pause(vexfs_mount_session *session, int paused,
                                        vexfs_mount_bytes *json,
                                        vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_gc(vexfs_mount_session *session, uint32_t batch,
                                  vexfs_mount_bytes *json,
                                  vexfs_mount_error *error);

#ifdef __cplusplus
}
#endif

#endif  // VEXFS_RUNTIME_ADMIN_H
