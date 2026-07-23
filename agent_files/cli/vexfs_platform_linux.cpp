#include "vexfs_platform.h"
#include "vexfs_platform_registry.h"

#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::filesystem::path UserDataDirectory() {
    if (const char *xdg = std::getenv("XDG_DATA_HOME")) return xdg;
    if (const char *home = std::getenv("HOME")) return std::filesystem::path(home) / ".local/share";
    return std::filesystem::current_path();
}

std::string DecodeMountField(const std::string &value) {
    std::string output;
    output.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '\\' && index + 3 < value.size()) {
            const std::string code = value.substr(index + 1, 3);
            if (code == "040") output.push_back(' ');
            else if (code == "011") output.push_back('\t');
            else if (code == "012") output.push_back('\n');
            else if (code == "134") output.push_back('\\');
            else {
                output.push_back(value[index]);
                continue;
            }
            index += 3;
        } else {
            output.push_back(value[index]);
        }
    }
    return output;
}

std::string NormalizedPath(const std::string &path) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    if (!error) return canonical.string();
    return std::filesystem::absolute(path).lexically_normal().string();
}

std::vector<VexFSPlatformMountEntry> MountTable(bool vexfs_only) {
    std::ifstream input("/proc/self/mountinfo");
    std::vector<VexFSPlatformMountEntry> mounts;
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::vector<std::string> values;
        std::string value;
        while (fields >> value) values.push_back(value);
        if (values.size() < 10) continue;
        size_t separator = 0;
        while (separator < values.size() && values[separator] != "-") ++separator;
        if (separator + 3 >= values.size() || separator < 6) continue;
        const std::string type = DecodeMountField(values[separator + 1]);
        const std::string source = DecodeMountField(values[separator + 2]);
        const bool is_vexfs = type == "fuse.vexfs" || (type == "fuse" && source == "vexfs");
        if (vexfs_only && !is_vexfs) continue;
        mounts.push_back({source, DecodeMountField(values[4]), type});
    }
    return mounts;
}

std::vector<VexFSPlatformMountEntry> MountedVolumes() {
    std::filesystem::path registry;
    if (const char *runtime = std::getenv("XDG_RUNTIME_DIR"))
        registry = std::filesystem::path(runtime) / "vexdb-lite/mounts";
    else
        registry = UserDataDirectory() / "vexdb-lite/mounts";
    return VexFSPlatformAttachMountRegistry(MountTable(true), registry);
}

std::filesystem::path MountRegistryDirectory() {
    if (const char *runtime = std::getenv("XDG_RUNTIME_DIR"))
        return std::filesystem::path(runtime) / "vexdb-lite/mounts";
    return UserDataDirectory() / "vexdb-lite/mounts";
}

std::string MountedFileSystemAt(const std::string &path) {
    const std::string normalized = NormalizedPath(path);
    for (const auto &mount : MountTable(false)) {
        if (NormalizedPath(mount.target) == normalized) {
            if (mount.type == "fuse" && mount.source == "vexfs") return "fuse.vexfs";
            return mount.type;
        }
    }
    return {};
}

std::string FindExecutable(const std::string &name) {
    if (name.find('/') != std::string::npos)
        return access(name.c_str(), X_OK) == 0 ? name : std::string();
    const char *path_value = std::getenv("PATH");
    if (path_value == nullptr) return {};
    std::istringstream paths(path_value);
    std::string directory;
    while (std::getline(paths, directory, ':')) {
        if (directory.empty()) directory = ".";
        const std::filesystem::path candidate = std::filesystem::path(directory) / name;
        if (access(candidate.c_str(), X_OK) == 0) return candidate.string();
    }
    return {};
}

std::string FuseHelper() {
    if (const char *configured = std::getenv("VEXFS_FUSE_HELPER"))
        return FindExecutable(configured);
    char executable[4096]{};
    const ssize_t size = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (size > 0) {
        executable[size] = '\0';
        const std::filesystem::path sibling =
            std::filesystem::path(executable).parent_path() / "vexfs-fuse";
        if (access(sibling.c_str(), X_OK) == 0) return sibling.string();
    }
    return FindExecutable("vexfs-fuse");
}

int RunProcess(const std::string &program, const std::vector<std::string> &arguments) {
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error(std::strerror(errno));
    if (child == 0) {
        std::vector<char *> values;
        values.push_back(const_cast<char *>(program.c_str()));
        for (const std::string &argument : arguments)
            values.push_back(const_cast<char *>(argument.c_str()));
        values.push_back(nullptr);
        execv(program.c_str(), values.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) < 0) throw std::runtime_error(std::strerror(errno));
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

void PrepareMountPoint(const std::string &path) {
    const std::filesystem::path mount_point(path);
    if (std::filesystem::exists(mount_point) && !std::filesystem::is_directory(mount_point))
        throw std::runtime_error("mount point is not a directory: " + path);
    std::filesystem::create_directories(mount_point);
    VexFSPlatformProtectDirectory(mount_point);
    if (std::filesystem::directory_iterator(mount_point) != std::filesystem::directory_iterator())
        throw std::runtime_error("mount point must be empty: " + path);
}

std::string KernelVersion() {
    struct utsname information{};
    return uname(&information) == 0 ? information.release : "unknown";
}

}  // namespace

std::string VexFSPlatformDefaultDatabasePath() {
    if (const char *configured = std::getenv("VEXDB_LITE_DATABASE")) return configured;
    if (const char *configured = std::getenv("VEXFS_DATABASE")) return configured;
    return (UserDataDirectory() / "vexdb-lite/default.sqlite3").string();
}

void VexFSPlatformProtectDirectory(const std::filesystem::path &path) {
    if (chmod(path.c_str(), 0700) != 0) throw std::runtime_error(std::strerror(errno));
}

VexFSPlatformState VexFSPlatformInspect() {
    VexFSPlatformState state;
    state.platform = "linux";
    state.version = KernelVersion();
    state.platform_supported = true;
    state.mount_driver = "libfuse3";
    const bool helper = !FuseHelper().empty();
    const bool fusermount = !FindExecutable("fusermount3").empty();
    const bool device = access("/dev/fuse", F_OK) == 0;
    const bool device_accessible = access("/dev/fuse", R_OK | W_OK) == 0;
    state.mount_driver_available = helper && fusermount;
    if (!helper) state.extension_state = "helper-missing";
    else if (!fusermount) state.extension_state = "fusermount3-missing";
    else if (!device) state.extension_state = "fuse-device-missing";
    else if (!device_accessible) state.extension_state = "fuse-device-permission-denied";
    else state.extension_state = "ready";
    state.mount_ready = state.mount_driver_available && device_accessible;
    state.mounts = MountedVolumes();
    return state;
}

int VexFSPlatformMount(const std::string &database, const std::string &workspace,
                       const std::string &mount_point) {
    const std::string mounted_type = MountedFileSystemAt(mount_point);
    if (mounted_type == "fuse.vexfs") {
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
    if (!mounted_type.empty())
        throw std::runtime_error("mount point is already used by " + mounted_type);
    const VexFSPlatformState state = VexFSPlatformInspect();
    if (!state.mount_ready)
        throw std::runtime_error("VexFS libfuse3 mount is not ready: " + state.extension_state);
    PrepareMountPoint(mount_point);
    const std::filesystem::path absolute_database =
        std::filesystem::absolute(database).lexically_normal();
    if (absolute_database.has_parent_path()) {
        std::filesystem::create_directories(absolute_database.parent_path());
        VexFSPlatformProtectDirectory(absolute_database.parent_path());
    }
    const std::string helper = FuseHelper();
    const int result = RunProcess(helper,
        {"--db", absolute_database.string(), "--workspace", workspace, mount_point});
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (!MountedFileSystemAt(mount_point).empty()) {
            try {
                VexFSPlatformRememberMount(MountRegistryDirectory(),
                    {"vexfs", NormalizedPath(mount_point), "fuse.vexfs",
                     NormalizedPath(database), workspace});
            } catch (...) {
                const std::string fusermount = FindExecutable("fusermount3");
                if (!fusermount.empty()) RunProcess(fusermount, {"-u", mount_point});
                throw;
            }
            return 0;
        }
        usleep(100 * 1000);
    }
    if (result == 0)
        throw std::runtime_error("vexfs-fuse returned success but the mount is not active");
    return result;
}

int VexFSPlatformUnmount(const std::string &mount_point, bool force) {
    const std::string mounted_type = MountedFileSystemAt(mount_point);
    if (mounted_type.empty()) {
        VexFSPlatformForgetMount(MountRegistryDirectory(), mount_point);
        return 0;
    }
    if (mounted_type != "fuse.vexfs")
        throw std::runtime_error("mount point is not VexFS: " + mounted_type);
    const std::string fusermount = FindExecutable("fusermount3");
    if (fusermount.empty()) throw std::runtime_error("fusermount3 is not installed");
    int result = 1;
    for (int attempt = 0; attempt < 5; ++attempt) {
        result = RunProcess(fusermount,
                            force ? std::vector<std::string>{"-u", "-z", mount_point}
                                  : std::vector<std::string>{"-u", mount_point});
        if (MountedFileSystemAt(mount_point).empty()) {
            VexFSPlatformForgetMount(MountRegistryDirectory(), mount_point);
            return 0;
        }
        usleep(200 * 1000);
    }
    return result;
}
