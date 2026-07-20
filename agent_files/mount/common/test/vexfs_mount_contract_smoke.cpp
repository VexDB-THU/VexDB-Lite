#include "vexfs_mount_contract.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int Fail(const char *operation, const vexfs_mount_error &error) {
    std::fprintf(stderr, "VEXFS MOUNT CONTRACT FAIL: %s: status=%d sqlite=%d %s\n",
                 operation, error.status, error.sqlite_code, error.message);
    return 1;
}

bool Equals(const vexfs_mount_bytes &bytes, const char *expected) {
    return bytes.size == std::strlen(expected) &&
           std::memcmp(bytes.data, expected, static_cast<size_t>(bytes.size)) == 0;
}

bool Contains(const vexfs_mount_bytes &bytes, const char *expected) {
    if (bytes.data == nullptr) return false;
    const std::string value(static_cast<const char *>(bytes.data), static_cast<size_t>(bytes.size));
    return value.find(expected) != std::string::npos;
}

}  // namespace

int main() {
    char path[] = "/tmp/vexfs-contract-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) return 1;
    close(fd);
    unlink(path);

    vexfs_mount_config config{};
    config.abi_version = VEXFS_MOUNT_ABI_VERSION;
    config.database_path = path;
    config.workspace = "default";
    config.busy_timeout_ms = 1000;
    vexfs_mount_error error{};
    vexfs_mount_session *session = nullptr;
    if (vexfs_mount_session_open(&config, &session, &error) != VEXFS_MOUNT_OK)
        return Fail("open", error);

    vexfs_mount_bytes diagnostics{};
    if (vexfs_mount_diagnostics(session, &diagnostics, &error) != VEXFS_MOUNT_OK)
        return Fail("diagnostics", error);
    if (!Contains(diagnostics, "\"contract_version\":\"0.3.0\"") ||
        !Contains(diagnostics, "\"workspace_exists\":1"))
        return Fail("diagnostics content", error);
    vexfs_mount_free(diagnostics.data);

    if (vexfs_mount_mkdir(session, "/agent", &error) != VEXFS_MOUNT_OK)
        return Fail("mkdir", error);
    int64_t version = 0;
    if (vexfs_mount_write_file(session, "/agent/task.txt", "draft", 5, &version, &error) !=
        VEXFS_MOUNT_OK) return Fail("write", error);

    if (vexfs_mount_write_file(session, "/agent/versioned.txt", "old", 3, &version, &error) !=
            VEXFS_MOUNT_OK || version != 1)
        return Fail("version seed", error);
    if (vexfs_mount_write_file(session, "/agent/versioned.txt", "new", 3, &version, &error) !=
            VEXFS_MOUNT_OK || version != 2)
        return Fail("version update", error);
    vexfs_mount_bytes history{};
    if (vexfs_mount_history(session, "/agent/versioned.txt", &history, &error) != VEXFS_MOUNT_OK ||
        !Contains(history, "\"version\":2") || !Contains(history, "\"version\":1"))
        return Fail("version history", error);
    vexfs_mount_free(history.data);
    vexfs_mount_bytes historical{};
    if (vexfs_mount_read_version(session, "/agent/versioned.txt", 1, &historical, &error) !=
            VEXFS_MOUNT_OK || !Equals(historical, "old"))
        return Fail("read historical version", error);
    vexfs_mount_free(historical.data);
    int64_t restored = 0;
    if (vexfs_mount_restore_version(session, "/agent/versioned.txt", 1, 2, &restored, &error) !=
            VEXFS_MOUNT_OK || restored != 3)
        return Fail("restore version", error);
    vexfs_mount_bytes restored_content{};
    if (vexfs_mount_read_file(session, "/agent/versioned.txt", &restored_content, &error) !=
            VEXFS_MOUNT_OK || !Equals(restored_content, "old"))
        return Fail("restored content", error);
    vexfs_mount_free(restored_content.data);
    if (vexfs_mount_restore_version(session, "/agent/versioned.txt", 2, 2, &restored, &error) !=
        VEXFS_MOUNT_CONFLICT) return Fail("stale restore must conflict", error);

    vexfs_mount_bytes handle{};
    if (vexfs_mount_handle_open(session, "/agent/task.txt", "rw", "open-cabi-1",
                                &handle, &error) != VEXFS_MOUNT_OK)
        return Fail("handle open", error);
    std::string handle_id(static_cast<const char *>(handle.data), static_cast<size_t>(handle.size));
    vexfs_mount_free(handle.data);

    int64_t generation = 0;
    if (vexfs_mount_handle_stage_write(session, handle_id.c_str(), 5, " ready", 6,
                                       "write-cabi-1", &generation, &error) != VEXFS_MOUNT_OK)
        return Fail("stage write", error);
    if (vexfs_mount_handle_publish(session, handle_id.c_str(), generation, "data",
                                   "publish-cabi-1", &version, &error) != VEXFS_MOUNT_OK)
        return Fail("publish", error);
    vexfs_mount_bytes state{};
    if (vexfs_mount_handle_close(session, handle_id.c_str(), 1, "close-cabi-1", &state,
                                 &error) != VEXFS_MOUNT_OK)
        return Fail("handle close", error);
    vexfs_mount_free(state.data);

    // 多次 write 只进入 staging；synchronize 一次发布最终 generation。
    if (vexfs_mount_write_file(session, "/agent/sync.txt", "A", 1, &version, &error) !=
        VEXFS_MOUNT_OK) return Fail("sync seed", error);
    handle = {};
    if (vexfs_mount_handle_open(session, "/agent/sync.txt", "rw", "open-sync-1",
                                &handle, &error) != VEXFS_MOUNT_OK)
        return Fail("sync handle open", error);
    handle_id.assign(static_cast<const char *>(handle.data), static_cast<size_t>(handle.size));
    vexfs_mount_free(handle.data);
    if (vexfs_mount_handle_stage_write(session, handle_id.c_str(), 1, "B", 1,
                                       "write-sync-1", &generation, &error) != VEXFS_MOUNT_OK)
        return Fail("sync stage 1", error);
    if (vexfs_mount_handle_stage_write(session, handle_id.c_str(), 2, "C", 1,
                                       "write-sync-2", &generation, &error) != VEXFS_MOUNT_OK)
        return Fail("sync stage 2", error);
    vexfs_mount_bytes content{};
    if (vexfs_mount_read_file(session, "/agent/sync.txt", &content, &error) != VEXFS_MOUNT_OK)
        return Fail("read before synchronize", error);
    if (!Equals(content, "A")) return Fail("staging must be invisible before synchronize", error);
    vexfs_mount_free(content.data);
    diagnostics = {};
    if (vexfs_mount_diagnostics(session, &diagnostics, &error) != VEXFS_MOUNT_OK ||
        !Contains(diagnostics, "\"pending_handles\":1"))
        return Fail("pending diagnostics", error);
    vexfs_mount_free(diagnostics.data);
    int64_t published = 0;
    if (vexfs_mount_synchronize(session, "sync-cabi-1", &published, &error) != VEXFS_MOUNT_OK ||
        published != 1) return Fail("synchronize", error);
    content = {};
    if (vexfs_mount_read_file(session, "/agent/sync.txt", &content, &error) != VEXFS_MOUNT_OK)
        return Fail("read after synchronize", error);
    if (!Equals(content, "ABC")) return Fail("synchronized content", error);
    vexfs_mount_free(content.data);
    state = {};
    if (vexfs_mount_handle_close(session, handle_id.c_str(), 1, "close-sync-1", &state,
                                 &error) != VEXFS_MOUNT_OK)
        return Fail("sync handle close", error);
    vexfs_mount_free(state.data);
    vexfs_mount_session_close(session);

    // 重开真实数据库文件，确认不是进程内假状态。
    session = nullptr;
    config.flags = VEXFS_MOUNT_OPEN_NO_CREATE;
    if (vexfs_mount_session_open(&config, &session, &error) != VEXFS_MOUNT_OK)
        return Fail("reopen", error);
    content = {};
    if (vexfs_mount_read_file(session, "/agent/task.txt", &content, &error) != VEXFS_MOUNT_OK)
        return Fail("persistent read", error);
    if (!Equals(content, "draft ready")) return Fail("persistent content", error);
    vexfs_mount_free(content.data);
    content = {};
    if (vexfs_mount_read_file(session, "/agent/sync.txt", &content, &error) != VEXFS_MOUNT_OK ||
        !Equals(content, "ABC")) return Fail("persistent synchronized content", error);
    vexfs_mount_free(content.data);
    vexfs_mount_session_close(session);

    // 独占挂载进程崩溃后，新 session 只保留旧 staging，不能自动发布半成品。
    config.flags = VEXFS_MOUNT_EXCLUSIVE_GATEWAY;
    session = nullptr;
    if (vexfs_mount_session_open(&config, &session, &error) != VEXFS_MOUNT_OK)
        return Fail("crash seed open", error);
    if (vexfs_mount_write_file(session, "/agent/crash.txt", "old", 3, &version, &error) !=
        VEXFS_MOUNT_OK) return Fail("crash seed write", error);
    vexfs_mount_session *contender = nullptr;
    if (vexfs_mount_session_open(&config, &contender, &error) != VEXFS_MOUNT_BUSY)
        return Fail("exclusive gateway must reject a live contender", error);
    vexfs_mount_session_close(session);

    const pid_t child = fork();
    if (child < 0) return 1;
    if (child == 0) {
        vexfs_mount_session *crashed = nullptr;
        vexfs_mount_error child_error{};
        if (vexfs_mount_session_open(&config, &crashed, &child_error) != VEXFS_MOUNT_OK)
            _exit(2);
        vexfs_mount_bytes child_handle{};
        if (vexfs_mount_handle_open(crashed, "/agent/crash.txt", "rw", "crash-open-1",
                                    &child_handle, &child_error) != VEXFS_MOUNT_OK)
            _exit(3);
        std::string child_id(static_cast<const char *>(child_handle.data),
                             static_cast<size_t>(child_handle.size));
        int64_t child_generation = 0;
        if (vexfs_mount_handle_stage_write(crashed, child_id.c_str(), 0, "NEW", 3,
                                           "crash-write-1", &child_generation,
                                           &child_error) != VEXFS_MOUNT_OK)
            _exit(4);
        _exit(0);
    }
    int child_status = 0;
    if (waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0) return 1;

    session = nullptr;
    if (vexfs_mount_session_open(&config, &session, &error) != VEXFS_MOUNT_OK)
        return Fail("crash recovery open", error);
    published = -1;
    if (vexfs_mount_synchronize(session, "crash-sync-1", &published, &error) != VEXFS_MOUNT_OK ||
        published != 1) {
        std::fprintf(stderr, "crash synchronize published=%lld\n", (long long)published);
        return Fail("crash staging recovery publish", error);
    }
    content = {};
    if (vexfs_mount_read_file(session, "/agent/crash.txt", &content, &error) != VEXFS_MOUNT_OK ||
        !Equals(content, "NEW")) return Fail("crash content recovered", error);
    vexfs_mount_free(content.data);
    diagnostics = {};
    if (vexfs_mount_diagnostics(session, &diagnostics, &error) != VEXFS_MOUNT_OK ||
        !Contains(diagnostics, "\"retained_handles\":0"))
        return Fail("crash staging claimed", error);
    vexfs_mount_free(diagnostics.data);
    vexfs_mount_session_close(session);

    unlink(path);
    std::string wal = std::string(path) + "-wal";
    std::string shm = std::string(path) + "-shm";
    unlink(wal.c_str());
    unlink(shm.c_str());
    std::puts("VEXFS MOUNT CONTRACT SMOKE: PASS");
    return 0;
}
