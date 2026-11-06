#ifndef VEXFS_NFS_GATEWAY_H
#define VEXFS_NFS_GATEWAY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vexfs_nfs_gateway_config {
    uint32_t abi_version;
    const char *backend;
    const char *connection;
    const char *workspace;
    const char *principal;
    const char *listen_address;
    uint16_t port;
    uint32_t operation_timeout_ms;
} vexfs_nfs_gateway_config;

// Blocks until SIGINT/SIGTERM or a fatal listener error. The caller owns
// process creation and lifecycle; this function owns one runtime session.
int vexfs_nfs_gateway_run(const vexfs_nfs_gateway_config *config,
                          char *error_message, size_t error_message_size);

#ifdef __cplusplus
}
#endif

#endif  // VEXFS_NFS_GATEWAY_H
