#include "vexfs_platform.h"
#include "vexfs_platform_registry.h"
#include "vexfs_fskit_state.h"

#include <dlfcn.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

enum class ExtensionState { kMissing, kEnabled, kDisabled, kRegistered };

std::string HomeDirectory() {
    const char *home = std::getenv("HOME");
    return home == nullptr ? std::string(".") : std::string(home);
}

std::string JsonEscape(const std::string &value) {
    std::string output;
    output.reserve(value.size() + 8);
    for (const unsigned char c : value) {
        switch (c) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (c < 0x20) {
                    char escaped[7] = {};
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                    output += escaped;
                } else {
                    output.push_back(static_cast<char>(c));
                }
        }
    }
    return output;
}

struct ProcessOutput {
    int exit_code = 1;
    std::string output;
};

int RunProcess(const char *program, const std::vector<std::string> &arguments) {
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error(std::strerror(errno));
    if (child == 0) {
        std::vector<char *> values;
        values.push_back(const_cast<char *>(program));
        for (const std::string &argument : arguments)
            values.push_back(const_cast<char *>(argument.c_str()));
        values.push_back(nullptr);
        execv(program, values.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) < 0) throw std::runtime_error(std::strerror(errno));
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

ProcessOutput CaptureProcess(const char *program, const std::vector<std::string> &arguments) {
    int descriptors[2] = {-1, -1};
    if (pipe(descriptors) != 0) throw std::runtime_error(std::strerror(errno));
    const pid_t child = fork();
    if (child < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        throw std::runtime_error(std::strerror(errno));
    }
    if (child == 0) {
        close(descriptors[0]);
        dup2(descriptors[1], STDOUT_FILENO);
        dup2(descriptors[1], STDERR_FILENO);
        close(descriptors[1]);
        std::vector<char *> values;
        values.push_back(const_cast<char *>(program));
        for (const std::string &argument : arguments)
            values.push_back(const_cast<char *>(argument.c_str()));
        values.push_back(nullptr);
        execv(program, values.data());
        _exit(127);
    }
    close(descriptors[1]);
    ProcessOutput result;
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(descriptors[0], buffer, sizeof(buffer));
        if (count > 0) result.output.append(buffer, static_cast<size_t>(count));
        else if (count == 0) break;
        else if (errno != EINTR) break;
    }
    close(descriptors[0]);
    int status = 0;
    if (waitpid(child, &status, 0) < 0) throw std::runtime_error(std::strerror(errno));
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return result;
}

ExtensionState GetExtensionState(std::string *module_path) {
    char module_url[4096]{};
    const int fskit_state = vexfs_fskit_extension_state(
        "io.vexdb.vexfs.extension", module_url, sizeof(module_url));
    if (std::getenv("VEXFS_DEBUG") != nullptr) {
        std::fprintf(stderr, "vexfs: FSKit state=%d module=%s\n", fskit_state, module_url);
    }
    if (module_path != nullptr) *module_path = module_url;
    if (fskit_state == 2) return ExtensionState::kEnabled;
    if (fskit_state == 1) return ExtensionState::kDisabled;

    const ProcessOutput result = CaptureProcess("/usr/bin/pluginkit",
        {"-m", "-A", "-D", "-i", "io.vexdb.vexfs.extension"});
    if (result.exit_code != 0 || result.output.empty()) return ExtensionState::kMissing;
    for (size_t position = 0; position < result.output.size();) {
        const size_t end = result.output.find('\n', position);
        const std::string line = result.output.substr(position,
            end == std::string::npos ? std::string::npos : end - position);
        const size_t marker = line.find_first_not_of(" \t");
        if (marker != std::string::npos) {
            if (line[marker] == '+' || line[marker] == '!') return ExtensionState::kRegistered;
            if (line[marker] == '-') return ExtensionState::kDisabled;
        }
        if (end == std::string::npos) break;
        position = end + 1;
    }
    return ExtensionState::kRegistered;
}

const char *ExtensionStateName(ExtensionState state) {
    switch (state) {
        case ExtensionState::kEnabled: return "enabled";
        case ExtensionState::kDisabled: return "disabled";
        case ExtensionState::kRegistered: return "registered";
        case ExtensionState::kMissing: return "missing";
    }
    return "missing";
}

std::string NormalizedPath(const std::string &path) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    if (!error) return canonical.string();
    return std::filesystem::absolute(path).lexically_normal().string();
}

std::filesystem::path MountRegistryDirectory() {
    return std::filesystem::path(HomeDirectory()) /
        "Library/Application Support/VexDB-Lite/mounts";
}

std::vector<VexFSPlatformMountEntry> MountedVolumes() {
    struct statfs *entries = nullptr;
    const int count = getmntinfo(&entries, MNT_NOWAIT);
    std::vector<VexFSPlatformMountEntry> mounts;
    for (int index = 0; index < count; ++index) {
        if (std::strcmp(entries[index].f_fstypename, "vexfs") != 0) continue;
        mounts.push_back({entries[index].f_mntfromname, entries[index].f_mntonname,
                          entries[index].f_fstypename});
    }
    return VexFSPlatformAttachMountRegistry(std::move(mounts), MountRegistryDirectory());
}

std::string ProductVersion() {
    char buffer[128] = {};
    size_t size = sizeof(buffer);
    if (sysctlbyname("kern.osproductversion", buffer, &size, nullptr, 0) != 0) return "unknown";
    return buffer;
}

bool ProductVersionSupported(const std::string &version) {
    char *end = nullptr;
    const long major = std::strtol(version.c_str(), &end, 10);
    return end != version.c_str() && major >= 26;
}

std::string MountedFileSystemAt(const std::string &path) {
    const std::string normalized = NormalizedPath(path);
    struct statfs *entries = nullptr;
    const int count = getmntinfo(&entries, MNT_NOWAIT);
    for (int index = 0; index < count; ++index) {
        if (NormalizedPath(entries[index].f_mntonname) == normalized)
            return entries[index].f_fstypename;
    }
    return {};
}

void PrepareMountPoint(const std::string &path) {
    const std::filesystem::path mount_point(path);
    if (std::filesystem::exists(mount_point) && !std::filesystem::is_directory(mount_point)) {
        throw std::runtime_error("mount point is not a directory: " + path);
    }
    std::filesystem::create_directories(mount_point);
    VexFSPlatformProtectDirectory(mount_point);
    if (std::filesystem::directory_iterator(mount_point) != std::filesystem::directory_iterator()) {
        throw std::runtime_error("mount point must be empty: " + path);
    }
}

std::filesystem::path WriteMountResourceDescriptor(const std::string &database_path,
                                                    const std::string &workspace) {
    const std::filesystem::path database =
        std::filesystem::absolute(database_path).lexically_normal();
    const std::filesystem::path directory = database.parent_path();
    if (directory.empty() || database.filename().empty()) {
        throw std::runtime_error("database path must include a file name");
    }
    std::filesystem::create_directories(directory);
    const std::filesystem::path descriptor = directory / ".vexfs-volume.json";
    std::ofstream output(descriptor, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write mount resource: " + descriptor.string());
    output << "{\n  \"version\": 2,\n  \"database_file\": \""
           << JsonEscape(database.filename().string())
           << "\",\n  \"workspace\": \"" << JsonEscape(workspace) << "\"\n}\n";
    return directory;
}

}  // namespace

std::string VexFSPlatformDefaultDatabasePath() {
    if (const char *configured = std::getenv("VEXDB_LITE_DATABASE")) return configured;
    if (const char *configured = std::getenv("VEXFS_DATABASE")) return configured;
    const std::filesystem::path home = HomeDirectory();
    const std::filesystem::path current = home /
        "Library/Application Support/VexDB-Lite/default.sqlite3";
    return current.string();
}

void VexFSPlatformProtectDirectory(const std::filesystem::path &path) {
    if (chmod(path.c_str(), 0700) != 0) throw std::runtime_error(std::strerror(errno));
}

VexFSPlatformState VexFSPlatformInspect() {
    VexFSPlatformState state;
    state.platform = "macos";
    state.version = ProductVersion();
    state.platform_supported = ProductVersionSupported(state.version);
    state.mount_driver = "FSKit";
    void *framework = dlopen("/System/Library/Frameworks/FSKit.framework/FSKit", RTLD_LAZY);
    state.mount_driver_available = framework != nullptr;
    if (framework != nullptr) dlclose(framework);
    const ExtensionState extension = GetExtensionState(&state.extension_path);
    state.extension_state = ExtensionStateName(extension);
    if (const char *expected = std::getenv("VEXFS_EXPECTED_EXTENSION_PATH")) {
        state.extension_path_matches = !state.extension_path.empty() &&
            NormalizedPath(state.extension_path) == NormalizedPath(expected);
    }
    state.mount_ready = state.platform_supported && state.mount_driver_available &&
                        extension == ExtensionState::kEnabled && state.extension_path_matches;
    state.mounts = MountedVolumes();
    return state;
}

int VexFSPlatformMount(const std::string &database, const std::string &workspace,
                       const std::string &mount_point) {
    const std::string mounted_type = MountedFileSystemAt(mount_point);
    if (mounted_type == "vexfs") {
        const std::string requested_database = NormalizedPath(database);
        const std::string requested_target = NormalizedPath(mount_point);
        for (const auto &mount : MountedVolumes()) {
            if (NormalizedPath(mount.target) != requested_target) continue;
            if (mount.database == requested_database && mount.workspace == workspace) return 0;
            throw std::runtime_error(
                "mount point already contains a VexFS workspace with different or unknown identity");
        }
        throw std::runtime_error("mount point is VexFS but its identity cannot be verified");
    }
    if (!mounted_type.empty()) {
        throw std::runtime_error("mount point is already used by " + mounted_type);
    }
    const VexFSPlatformState state = VexFSPlatformInspect();
    if (!state.mount_ready) {
        throw std::runtime_error("VexFS FSKit extension is " + state.extension_state +
            "; install VexDB Lite.app and enable the VexFS file system extension in System Settings");
    }
    PrepareMountPoint(mount_point);
    const std::filesystem::path resource_directory =
        WriteMountResourceDescriptor(database, workspace);
    int result = 1;
    for (int attempt = 0; attempt < 3; ++attempt) {
        result = RunProcess("/sbin/mount",
            {"-F", "-t", "vexfs", resource_directory.string(), mount_point});
        if (MountedFileSystemAt(mount_point) == "vexfs") {
            try {
                VexFSPlatformRememberMount(MountRegistryDirectory(),
                    {resource_directory.string(), NormalizedPath(mount_point), "vexfs",
                     NormalizedPath(database), workspace});
            } catch (...) {
                RunProcess("/sbin/umount", {mount_point});
                throw;
            }
            return 0;
        }
        if (result == 0) break;
        if (attempt != 2) usleep(500 * 1000);
    }
    if (result == 0) {
        throw std::runtime_error("mount command returned success but VexFS is not mounted");
    }
    return result;
}

int VexFSPlatformUnmount(const std::string &mount_point, bool force) {
    const std::string mounted_type = MountedFileSystemAt(mount_point);
    if (mounted_type.empty()) {
        VexFSPlatformForgetMount(MountRegistryDirectory(), mount_point);
        return 0;
    }
    if (mounted_type != "vexfs")
        throw std::runtime_error("mount point is not VexFS: " + mounted_type);
    int result = 1;
    // fseventsd/Spotlight can briefly hold a newly mounted FSKit volume even
    // when no user process has an open file. Keep this a normal (non-force)
    // unmount, but allow that short system handoff to finish.
    constexpr int kUnmountAttempts = 12;
    for (int attempt = 0; attempt < kUnmountAttempts; ++attempt) {
        result = RunProcess("/sbin/umount",
                            force ? std::vector<std::string>{"-f", mount_point}
                                  : std::vector<std::string>{mount_point});
        if (MountedFileSystemAt(mount_point).empty()) {
            VexFSPlatformForgetMount(MountRegistryDirectory(), mount_point);
            return 0;
        }
        if (attempt + 1 != kUnmountAttempts) usleep(500 * 1000);
    }
    return result;
}
