#include "vexfs_platform.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void SetEnvironment(const char *name, const char *value) {
#if defined(_WIN32)
    if (_putenv_s(name, value) != 0) throw std::runtime_error("_putenv_s failed");
#else
    if (setenv(name, value, 1) != 0) throw std::runtime_error("setenv failed");
#endif
}

void UnsetEnvironment(const char *name) {
#if defined(_WIN32)
    if (_putenv_s(name, "") != 0) throw std::runtime_error("_putenv_s failed");
#else
    if (unsetenv(name) != 0) throw std::runtime_error("unsetenv failed");
#endif
}

int Fail(const std::string &message) {
    std::cerr << "VEXFS PLATFORM STUB SMOKE: FAIL: " << message << '\n';
    return 1;
}

}  // namespace

int main() {
    try {
        UnsetEnvironment("VEXDB_LITE_DATABASE");
        UnsetEnvironment("VEXFS_DATABASE");
#if defined(VEXFS_PLATFORM_EXPECT_LINUX)
        SetEnvironment("XDG_DATA_HOME", "/tmp/vexfs-platform-data");
        const std::string expected_database =
            (std::filesystem::path("/tmp/vexfs-platform-data") /
             "vexdb-lite/default.sqlite3").string();
        const std::string expected_platform = "linux";
        const std::string expected_driver = "libfuse3";
#elif defined(VEXFS_PLATFORM_EXPECT_WINDOWS)
        SetEnvironment("LOCALAPPDATA", "C:/Users/VexFS/AppData/Local");
        const std::string expected_database =
            (std::filesystem::path("C:/Users/VexFS/AppData/Local") /
             "VexDB-Lite/default.sqlite3").string();
        const std::string expected_platform = "windows";
        const std::string expected_driver = "WinFsp";
#else
#error "expected platform is required"
#endif
        if (VexFSPlatformDefaultDatabasePath() != expected_database)
            return Fail("default database path");
        SetEnvironment("VEXDB_LITE_DATABASE", "/tmp/override.sqlite3");
        if (VexFSPlatformDefaultDatabasePath() != "/tmp/override.sqlite3")
            return Fail("database environment override");
        UnsetEnvironment("VEXDB_LITE_DATABASE");

        const VexFSPlatformState state = VexFSPlatformInspect();
        if (state.platform != expected_platform || state.mount_driver != expected_driver ||
            state.mount_driver_available || state.mount_ready ||
            state.extension_state != "not-implemented") {
            return Fail("platform state");
        }
        try {
            VexFSPlatformMount("workspace.sqlite3", "default", "/tmp/vexfs-mount");
            return Fail("unsupported mount succeeded");
        } catch (const std::runtime_error &error) {
            if (std::string(error.what()).find(expected_driver) == std::string::npos)
                return Fail("unsupported mount error");
        }
        std::cout << "VEXFS PLATFORM STUB SMOKE: PASS (" << expected_platform << ")\n";
        return 0;
    } catch (const std::exception &error) {
        return Fail(error.what());
    }
}
