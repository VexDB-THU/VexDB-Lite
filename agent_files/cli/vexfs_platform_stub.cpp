#include "vexfs_platform.h"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <direct.h>
#else
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#endif

namespace {

std::filesystem::path UserDataDirectory() {
#if defined(VEXFS_PLATFORM_STUB_LINUX)
    if (const char *xdg = std::getenv("XDG_DATA_HOME")) return xdg;
    if (const char *home = std::getenv("HOME")) return std::filesystem::path(home) / ".local/share";
    return std::filesystem::current_path();
#elif defined(VEXFS_PLATFORM_STUB_WINDOWS) || defined(_WIN32)
    if (const char *local = std::getenv("LOCALAPPDATA")) return local;
    if (const char *profile = std::getenv("USERPROFILE")) return profile;
    return std::filesystem::current_path();
#else
    if (const char *xdg = std::getenv("XDG_DATA_HOME")) return xdg;
    if (const char *home = std::getenv("HOME")) return std::filesystem::path(home) / ".local/share";
    return std::filesystem::current_path();
#endif
}

const char *PlatformName() {
#if defined(VEXFS_PLATFORM_STUB_LINUX)
    return "linux";
#elif defined(VEXFS_PLATFORM_STUB_WINDOWS) || defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#else
    return "unsupported";
#endif
}

const char *MountDriverName() {
#if defined(VEXFS_PLATFORM_STUB_LINUX)
    return "libfuse3";
#elif defined(VEXFS_PLATFORM_STUB_WINDOWS) || defined(_WIN32)
    return "WinFsp";
#elif defined(__linux__)
    return "libfuse3";
#else
    return "none";
#endif
}

}  // namespace

std::string VexFSPlatformDefaultDatabasePath() {
    if (const char *configured = std::getenv("VEXDB_LITE_DATABASE")) return configured;
    if (const char *configured = std::getenv("VEXFS_DATABASE")) return configured;
#if defined(VEXFS_PLATFORM_STUB_LINUX)
    return (UserDataDirectory() / "vexdb-lite/default.sqlite3").string();
#elif defined(VEXFS_PLATFORM_STUB_WINDOWS) || defined(_WIN32)
    return (UserDataDirectory() / "VexDB-Lite/default.sqlite3").string();
#else
    return (UserDataDirectory() / "vexdb-lite/default.sqlite3").string();
#endif
}

void VexFSPlatformProtectDirectory(const std::filesystem::path &path) {
#if defined(_WIN32)
    (void)path;
#else
    if (chmod(path.c_str(), 0700) != 0) throw std::runtime_error(std::strerror(errno));
#endif
}

VexFSPlatformState VexFSPlatformInspect() {
    VexFSPlatformState state;
    state.platform = PlatformName();
    state.version = "unknown";
    state.platform_supported = true;
    state.mount_driver = MountDriverName();
    state.mount_driver_available = false;
    state.extension_state = "not-implemented";
    state.mount_ready = false;
    return state;
}

int VexFSPlatformMount(const std::string &, const std::string &,
                       const std::string &) {
    throw std::runtime_error(std::string("VexFS ") + MountDriverName() +
                             " mount adapter is not implemented yet");
}

int VexFSPlatformUnmount(const std::string &, bool) {
    throw std::runtime_error(std::string("VexFS ") + MountDriverName() +
                             " mount adapter is not implemented yet");
}
