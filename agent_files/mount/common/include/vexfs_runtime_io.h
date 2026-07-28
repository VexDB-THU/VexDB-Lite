// 平台文件系统 adapter 允许调用的数据面接口。
#ifndef VEXFS_RUNTIME_IO_H
#define VEXFS_RUNTIME_IO_H

#include "vexfs_runtime_types.h"

#ifdef __cplusplus
extern "C" {
#endif

vexfs_mount_status vexfs_mount_session_open(const vexfs_mount_config *config,
                                             vexfs_mount_session **session,
                                             vexfs_mount_error *error);
void vexfs_mount_session_close(vexfs_mount_session *session);

vexfs_mount_status vexfs_mount_mkdir(vexfs_mount_session *session, const char *path,
                                     vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_create(vexfs_mount_session *session, const char *path,
                                      const char *kind, uint32_t mode,
                                      vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_set_mode(vexfs_mount_session *session, int64_t inode,
                                        uint32_t mode, vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_set_times(vexfs_mount_session *session, int64_t inode,
                                         int64_t accessed_at_ms, int64_t modified_at_ms,
                                         uint32_t mask, vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_symlink(vexfs_mount_session *session, const char *path,
                                       const void *target, uint64_t target_size,
                                       vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_link(vexfs_mount_session *session, const char *source,
                                    const char *destination, vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_chown(vexfs_mount_session *session, int64_t inode,
                                     int64_t uid, int64_t gid, vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_readlink(vexfs_mount_session *session, int64_t inode,
                                        vexfs_mount_bytes *target,
                                        vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_write_file(vexfs_mount_session *session, const char *path,
                                          const void *data, uint64_t size,
                                          int64_t *version, vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_write_file_range(vexfs_mount_session *session,
                                                const char *path, uint64_t offset,
                                                const void *data, uint64_t size,
                                                const char *request_id,
                                                const char *durability,
                                                int64_t *version,
                                                vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_append_file(vexfs_mount_session *session, int64_t inode,
                                           const void *data, uint64_t size,
                                           int64_t *version, vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_read_file(vexfs_mount_session *session, const char *path,
                                         vexfs_mount_bytes *content,
                                         vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_read_file_range(vexfs_mount_session *session,
                                               const char *path, uint64_t offset,
                                               uint64_t length,
                                               vexfs_mount_bytes *content,
                                               vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_stat(vexfs_mount_session *session, const char *path,
                                    vexfs_mount_bytes *json, vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_path_for_inode(vexfs_mount_session *session, int64_t inode,
                                              vexfs_mount_bytes *path,
                                              vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_list(vexfs_mount_session *session, const char *path,
                                    vexfs_mount_bytes *json, vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_list_versioned(vexfs_mount_session *session,
                                              const char *path,
                                              vexfs_mount_bytes *json,
                                              vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_move(vexfs_mount_session *session, const char *source,
                                    const char *destination, vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_rename(vexfs_mount_session *session, const char *source,
                                      const char *destination, int replace,
                                      vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_remove(vexfs_mount_session *session, const char *path,
                                      int recursive, vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_xattr_get(vexfs_mount_session *session, int64_t inode,
                                         const char *name, vexfs_mount_bytes *value,
                                         vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_xattr_list(vexfs_mount_session *session, int64_t inode,
                                          vexfs_mount_bytes *json,
                                          vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_xattr_set(vexfs_mount_session *session, int64_t inode,
                                         const char *name, const void *value,
                                         uint64_t size, int policy,
                                         vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_acl_get(vexfs_mount_session *session, int64_t inode,
                                       vexfs_mount_bytes *json,
                                       vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_acl_set(vexfs_mount_session *session, int64_t inode,
                                       const void *json, uint64_t size,
                                       vexfs_mount_error *error);

// 快速检测其他数据库连接是否已经提交。只有 data_version 变化时才读取 HEAD；
// adapter 用 cache_generation 对已有 vnode 做懒失效，不扫描整个缓存。
vexfs_mount_status vexfs_mount_refresh_visibility(vexfs_mount_session *session,
                                                  vexfs_mount_visibility *visibility,
                                                  vexfs_mount_error *error);

// 平台 adapter 用它实现 statfs。限制值为 null 时，后端没有设置硬上限。
vexfs_mount_status vexfs_mount_quota_get(vexfs_mount_session *session,
                                         vexfs_mount_bytes *json,
                                         vexfs_mount_error *error);

vexfs_mount_status vexfs_mount_handle_open(vexfs_mount_session *session, const char *path,
                                           const char *flags, const char *request_id,
                                           vexfs_mount_bytes *handle,
                                           vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_handle_create(vexfs_mount_session *session,
                                             const char *path, uint32_t mode,
                                             const char *request_id,
                                             vexfs_mount_bytes *handle,
                                             vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_handle_create_owned_durable(
    vexfs_mount_session *session, const char *path, uint32_t mode,
    int64_t uid, int64_t gid, const char *request_id,
    const char *durability, vexfs_mount_bytes *handle,
    vexfs_mount_error *error);
// 创建文件时同时返回 stat。PostgreSQL adapter 会在同一条 SQL 内完成，
// 避免远程挂载在 CREATE 后再发一次 vexfs_stat 往返。
vexfs_mount_status vexfs_mount_handle_create_owned_stat_durable(
    vexfs_mount_session *session, const char *path, uint32_t mode,
    int64_t uid, int64_t gid, const char *request_id,
    const char *durability, vexfs_mount_bytes *handle,
    vexfs_mount_bytes *stat_json, vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_handle_stage_write(vexfs_mount_session *session,
                                                  const char *handle, uint64_t offset,
                                                  const void *data, uint64_t size,
                                                  const char *request_id,
                                                  int64_t *generation,
                                                  vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_handle_stage_write_durable(
    vexfs_mount_session *session, const char *handle, uint64_t offset,
    const void *data, uint64_t size, const char *request_id,
    const char *durability, int64_t *generation, vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_handle_append(vexfs_mount_session *session,
                                             const char *handle, const void *data,
                                             uint64_t size, const char *request_id,
                                             int64_t *version,
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
vexfs_mount_status vexfs_mount_handle_publish_close(vexfs_mount_session *session,
                                                    const char *handle,
                                                    int64_t generation,
                                                    const char *durability,
                                                    int64_t *version,
                                                    vexfs_mount_error *error);
// PostgreSQL-only single-file background path. It uses the dedicated publisher
// connection, so one implicit transaction releases the workspace row lock
// before the next claimed file begins publishing.
vexfs_mount_status vexfs_mount_handle_publish_close_background(
    vexfs_mount_session *session, const char *handle, int64_t generation,
    const char *durability, int64_t *version, vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_handle_close(vexfs_mount_session *session,
                                            const char *handle, int retain_unpublished,
                                            const char *request_id,
                                            vexfs_mount_bytes *state,
                                            vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_synchronize(vexfs_mount_session *session,
                                           const char *request_id, int64_t *published,
                                           vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_publish_close_all(
    vexfs_mount_session *session, const char *durability,
    int64_t *published, vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_publish_close_batch(
    vexfs_mount_session *session, const char *durability, int64_t max_count,
    int64_t *published, vexfs_mount_error *error);
// PostgreSQL-only background path. It uses a dedicated libpq connection and
// publishes exactly the handle generations encoded in claims_json, so callers
// do not need to hold the gateway-wide RuntimeState mutex during database I/O.
vexfs_mount_status vexfs_mount_publish_close_claimed(
    vexfs_mount_session *session, const char *durability,
    const char *claims_json, vexfs_mount_bytes *result_json,
    vexfs_mount_error *error);
vexfs_mount_status vexfs_mount_reclaim(vexfs_mount_session *session,
                                       const char *request_id, int64_t *reclaimed,
                                       vexfs_mount_error *error);

void vexfs_mount_free(void *memory);

#ifdef __cplusplus
}
#endif

#endif  // VEXFS_RUNTIME_IO_H
