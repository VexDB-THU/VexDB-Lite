#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct VexFSPlatformMountEntry {
    std::string source;
    std::string target;
    std::string type;
    // Filled only when the mount was created by VexDB-Lite and its live mount
    // table entry still matches the saved identity record.
    std::string backend;
    std::string database;
    std::string workspace;
};

struct VexFSPlatformState {
    std::string platform;
    std::string version;
    bool platform_supported = false;
    std::string mount_driver;
    bool mount_driver_available = false;
    std::string extension_state;
    std::string extension_path;
    bool extension_path_matches = true;
    bool mount_ready = false;
    std::vector<VexFSPlatformMountEntry> mounts;
};

// These functions are the only operating-system boundary used by the shared CLI.
// Database operations use vexfs_runtime_admin.h; platform adapters stay independent.
std::string VexFSPlatformDefaultDatabasePath();
void VexFSPlatformProtectDirectory(const std::filesystem::path &path);
VexFSPlatformState VexFSPlatformInspect();
int VexFSPlatformMount(const std::string &backend, const std::string &connection,
                       const std::string &workspace,
                       const std::string &mount_point);
int VexFSPlatformUnmount(const std::string &mount_point, bool force);
