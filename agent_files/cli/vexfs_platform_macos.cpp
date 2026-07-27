#include "vexfs_platform.h"
#include "vexfs_platform_registry.h"
#include "vexfs_fskit_state.h"
#include "vexfs_runtime_types.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <netinet/in.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(VEXFS_HAVE_LIBPQ)
#include <libpq-fe.h>
#endif

namespace {

enum class ExtensionState {
    kMissing,
    kEnabled,
    kDisabled,
    kRegistered,
    kServiceUnavailable,
};

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

int RunProcess(const char *program, const std::vector<std::string> &arguments,
               int timeout_ms = 30'000) {
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
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    while (true) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child)
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        if (waited < 0 && errno != EINTR)
            throw std::runtime_error(std::strerror(errno));
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGTERM);
            for (int attempt = 0; attempt < 10; ++attempt) {
                if (waitpid(child, &status, WNOHANG) == child) return 124;
                usleep(50 * 1000);
            }
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            return 124;
        }
        usleep(50 * 1000);
    }
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
    // Negative results mean FSKit itself timed out or rejected the XPC request.
    // Falling through to pluginkit used to turn that into the misleading
    // "missing" state even when the signed App and extension were on disk.
    if (fskit_state < 0) return ExtensionState::kServiceUnavailable;

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
        case ExtensionState::kServiceUnavailable: return "service-unavailable";
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

bool IsLoopbackNfsSource(const std::string &source) {
    return source == "127.0.0.1:/" || source.rfind("127.0.0.1:", 0) == 0;
}

std::vector<VexFSPlatformMountEntry> MountedVolumes() {
    struct statfs *entries = nullptr;
    const int count = getmntinfo(&entries, MNT_NOWAIT);
    std::vector<VexFSPlatformMountEntry> mounts;
    for (int index = 0; index < count; ++index) {
        const std::string type = entries[index].f_fstypename;
        const std::string source = entries[index].f_mntfromname;
        if (type != "vexfs" && !(type == "nfs" && IsLoopbackNfsSource(source))) continue;
        mounts.push_back({entries[index].f_mntfromname, entries[index].f_mntonname,
                          entries[index].f_fstypename});
    }
    mounts = VexFSPlatformAttachMountRegistry(std::move(mounts), MountRegistryDirectory());
    mounts.erase(std::remove_if(mounts.begin(), mounts.end(), [](const auto &mount) {
        // A loopback NFS mount is VexFS only when the protected registry proves
        // the target/source identity. Do not claim unrelated localhost exports.
        return mount.type == "nfs" && mount.backend.empty();
    }), mounts.end());
    return mounts;
}

std::string ProductVersion() {
    char buffer[128] = {};
    size_t size = sizeof(buffer);
    if (sysctlbyname("kern.osproductversion", buffer, &size, nullptr, 0) != 0) return "unknown";
    return buffer;
}

bool ProductVersionSupported(const std::string &version, const std::string &driver) {
    char *end = nullptr;
    const long major = std::strtol(version.c_str(), &end, 10);
    if (end == version.c_str()) return false;
    return major >= (driver == "nfs" ? 13 : 26);
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

// Keep the directory below a live mount non-writable. If FSKit crashes and
// macOS removes the mount, Bash must fail instead of silently writing files to
// the local directory. A normal VexFS unmount restores the directory to 0700.
class UnderlyingMountPointGuard {
  public:
    explicit UnderlyingMountPointGuard(const std::string &path)
        : descriptor_(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) {
        if (descriptor_ < 0)
            throw std::runtime_error("cannot open mount point guard: " +
                                     std::string(std::strerror(errno)));
    }
    ~UnderlyingMountPointGuard() {
        if (descriptor_ >= 0) close(descriptor_);
    }
    UnderlyingMountPointGuard(const UnderlyingMountPointGuard &) = delete;
    UnderlyingMountPointGuard &operator=(const UnderlyingMountPointGuard &) = delete;

    void Arm() const {
        if (fchmod(descriptor_, 0500) != 0)
            throw std::runtime_error("cannot protect mount point: " +
                                     std::string(std::strerror(errno)));
    }

  private:
    int descriptor_ = -1;
};

void DisarmMountPointGuard(const std::string &path) {
    std::error_code error;
    if (!std::filesystem::is_directory(path, error)) return;
    if (chmod(path.c_str(), 0700) != 0)
        throw std::runtime_error("cannot restore mount point permissions: " +
                                 std::string(std::strerror(errno)));
}

uint64_t StableHash(const std::string &value) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string RequestedMountDriver(std::string driver) {
    if (driver.empty()) return "nfs";
    for (char &character : driver)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    if (driver == "nfs" || driver == "nfsv3") return "nfs";
    if (driver == "fskit") return "fskit";
    throw std::runtime_error("macOS mount driver must be nfs or fskit");
}

std::filesystem::path NfsGatewayExecutable() {
    if (const char *configured = std::getenv("VEXFS_NFS_GATEWAY"))
        return std::filesystem::absolute(configured).lexically_normal();
    uint32_t size = 4096;
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.resize(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    }
    return std::filesystem::path(buffer.data()).parent_path() / "vexfs-nfs-gateway";
}

std::filesystem::path NfsGatewayStateRoot() {
    return std::filesystem::path(HomeDirectory()) /
        "Library/Application Support/VexDB-Lite/nfs-gateways";
}

std::filesystem::path NfsGatewayStateDirectory(const std::string &mount_point) {
    std::ostringstream name;
    name << "mount-" << std::hex << std::setw(16) << std::setfill('0')
         << StableHash(NormalizedPath(mount_point));
    return NfsGatewayStateRoot() / name.str();
}

struct NfsGatewayRecord {
    pid_t pid = -1;
    uint16_t port = 0;
    std::filesystem::path executable;
    std::filesystem::path resource_directory;
};

std::filesystem::path NfsGatewayRecordPath(const std::string &mount_point) {
    return NfsGatewayStateDirectory(mount_point) / "gateway.record";
}

void WriteNfsGatewayRecord(const std::string &mount_point,
                           const NfsGatewayRecord &record) {
    const std::filesystem::path directory = NfsGatewayStateDirectory(mount_point);
    std::filesystem::create_directories(directory);
    VexFSPlatformProtectDirectory(directory);
    const std::filesystem::path destination = NfsGatewayRecordPath(mount_point);
    const std::filesystem::path temporary = destination.string() + ".tmp-" +
        std::to_string(getpid());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot write NFS gateway state");
        output << "VEXFS_NFS_GATEWAY_V1\n" << record.pid << '\n' << record.port << '\n'
               << std::quoted(record.executable.string()) << '\n'
               << std::quoted(record.resource_directory.string()) << '\n';
        if (!output) throw std::runtime_error("cannot finish NFS gateway state");
    }
    if (chmod(temporary.c_str(), 0600) != 0) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("cannot protect NFS gateway state");
    }
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        throw std::runtime_error("cannot publish NFS gateway state");
    }
}

bool ReadNfsGatewayRecord(const std::string &mount_point, NfsGatewayRecord *record) {
    std::ifstream input(NfsGatewayRecordPath(mount_point), std::ios::binary);
    std::string version;
    long long pid = -1;
    unsigned int port = 0;
    std::string executable;
    std::string resource;
    if (!std::getline(input, version) || version != "VEXFS_NFS_GATEWAY_V1" ||
        !(input >> pid >> port >> std::quoted(executable) >> std::quoted(resource)) ||
        pid <= 0 || port == 0 || port > 65535) return false;
    record->pid = static_cast<pid_t>(pid);
    record->port = static_cast<uint16_t>(port);
    record->executable = executable;
    record->resource_directory = resource;
    return true;
}

bool ProcessMatchesGateway(const NfsGatewayRecord &record) {
    char path[PROC_PIDPATHINFO_MAXSIZE]{};
    if (proc_pidpath(record.pid, path, sizeof(path)) <= 0) return false;
    return NormalizedPath(path) == NormalizedPath(record.executable.string());
}

uint16_t AvailableLoopbackPort() {
    const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) throw std::runtime_error("cannot allocate NFS loopback socket");
    struct SocketGuard {
        int value;
        ~SocketGuard() { if (value >= 0) close(value); }
    } guard{descriptor};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
        throw std::runtime_error("cannot reserve NFS loopback port: " +
                                 std::string(std::strerror(errno)));
    socklen_t size = sizeof(address);
    if (getsockname(descriptor, reinterpret_cast<sockaddr *>(&address), &size) != 0)
        throw std::runtime_error("cannot inspect NFS loopback port");
    return ntohs(address.sin_port);
}

bool LoopbackPortReady(uint16_t port) {
    const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    const bool ready = connect(descriptor, reinterpret_cast<sockaddr *>(&address),
                               sizeof(address)) == 0;
    close(descriptor);
    return ready;
}

std::string ReadGatewayLog(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::string content((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
    constexpr size_t kLimit = 4096;
    if (content.size() > kLimit) content.erase(0, content.size() - kLimit);
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r'))
        content.pop_back();
    return content;
}

NfsGatewayRecord StartNfsGateway(const std::string &backend,
                                 const std::string &connection,
                                 const std::string &workspace,
                                 const std::string &mount_point,
                                 const std::filesystem::path &resource_directory) {
    const std::filesystem::path executable = NfsGatewayExecutable();
    if (executable.empty() || access(executable.c_str(), X_OK) != 0)
        throw std::runtime_error("vexfs-nfs-gateway is missing next to the vexdb command");
    const uint16_t port = AvailableLoopbackPort();
    const std::filesystem::path log = resource_directory / "gateway.log";
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error(std::strerror(errno));
    if (child == 0) {
        setsid();
        signal(SIGHUP, SIG_IGN);
        umask(0077);
        const int input = open("/dev/null", O_RDONLY);
        const int output = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (input >= 0) dup2(input, STDIN_FILENO);
        if (output >= 0) {
            dup2(output, STDOUT_FILENO);
            dup2(output, STDERR_FILENO);
        }
        if (input > STDERR_FILENO) close(input);
        if (output > STDERR_FILENO) close(output);
        const std::string port_text = std::to_string(port);
        const char *principal = backend == VEXFS_RUNTIME_BACKEND_SQLITE ? "local" : "";
        std::vector<std::string> arguments = {
            executable.string(), "--backend", backend, "--connection", connection,
            "--workspace", workspace, "--principal", principal,
            "--listen", "127.0.0.1", "--port", port_text,
            "--timeout-ms", "30000"};
        std::vector<char *> values;
        for (std::string &argument : arguments) values.push_back(argument.data());
        values.push_back(nullptr);
        execv(executable.c_str(), values.data());
        _exit(127);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (LoopbackPortReady(port)) {
            NfsGatewayRecord record{child, port, executable, resource_directory};
            try {
                WriteNfsGatewayRecord(mount_point, record);
            } catch (...) {
                kill(child, SIGTERM);
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
                throw;
            }
            return record;
        }
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            const std::string details = ReadGatewayLog(log);
            throw std::runtime_error(details.empty()
                ? "VexFS NFS gateway exited before it became ready" : details);
        }
        usleep(50 * 1000);
    }
    kill(child, SIGTERM);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    throw std::runtime_error("timed out waiting for VexFS NFS gateway");
}

void StopNfsGateway(const std::string &mount_point) {
    NfsGatewayRecord record;
    if (!ReadNfsGatewayRecord(mount_point, &record) || !ProcessMatchesGateway(record)) return;
    kill(record.pid, SIGTERM);
    // SIGTERM starts a graceful database-backed staging flush. Large small-file
    // bursts are committed in bounded batches, so allow them to finish before
    // falling back to SIGKILL. The filesystem is already detached at this point.
    for (int attempt = 0; attempt < 600; ++attempt) {
        if (kill(record.pid, 0) != 0 && errno == ESRCH) return;
        usleep(50 * 1000);
    }
    if (ProcessMatchesGateway(record)) kill(record.pid, SIGKILL);
}

void RemoveNfsGatewayState(const std::string &mount_point) {
    std::error_code ignored;
    std::filesystem::remove_all(NfsGatewayStateDirectory(mount_point), ignored);
}

bool HasInlinePostgreSQLPassword(const std::string &connection) {
    std::string lower = connection;
    for (char &character : lower)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    if (lower.find("password=") != std::string::npos) return true;
    const size_t scheme = lower.find("://");
    if (scheme == std::string::npos) return false;
    const size_t authority = scheme + 3;
    const size_t at = lower.find('@', authority);
    if (at == std::string::npos) return false;
    return lower.find(':', authority) < at;
}

std::string EscapeLibpqValue(const std::string &value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('\'');
    for (const char character : value) {
        if (character == '\'' || character == '\\') escaped.push_back('\\');
        escaped.push_back(character);
    }
    escaped.push_back('\'');
    return escaped;
}

struct PostgreSQLNetworkEndpoint {
    std::string host;
    int port = 5432;
};

bool ParseRemotePostgreSQLEndpoint(const std::string &connection,
                                   PostgreSQLNetworkEndpoint *endpoint) {
#if !defined(VEXFS_HAVE_LIBPQ)
    (void)connection;
    (void)endpoint;
    return false;
#else
    char *parse_error = nullptr;
    PQconninfoOption *options = PQconninfoParse(connection.c_str(), &parse_error);
    if (options == nullptr) {
        if (parse_error != nullptr) PQfreemem(parse_error);
        return false;
    }
    struct OptionsGuard {
        PQconninfoOption *value;
        ~OptionsGuard() { PQconninfoFree(value); }
    } guard{options};
    std::string host;
    std::string host_address;
    std::string port = "5432";
    for (PQconninfoOption *option = options; option->keyword != nullptr; ++option) {
        if (option->val == nullptr || option->val[0] == '\0') continue;
        if (std::strcmp(option->keyword, "host") == 0) host = option->val;
        else if (std::strcmp(option->keyword, "hostaddr") == 0) host_address = option->val;
        else if (std::strcmp(option->keyword, "port") == 0) port = option->val;
    }
    if (!host_address.empty()) host = host_address;
    const size_t host_separator = host.find(',');
    if (host_separator != std::string::npos) host.resize(host_separator);
    const size_t port_separator = port.find(',');
    if (port_separator != std::string::npos) port.resize(port_separator);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    std::string lower = host;
    for (char &character : lower)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    if (host.empty() || host.front() == '/' || lower == "localhost" || lower == "::1" ||
        lower.rfind("127.", 0) == 0) return false;
    char *end = nullptr;
    const long parsed_port = std::strtol(port.c_str(), &end, 10);
    if (end == port.c_str() || *end != '\0' || parsed_port < 1 || parsed_port > 65535)
        return false;
    endpoint->host = std::move(host);
    endpoint->port = static_cast<int>(parsed_port);
    return true;
#endif
}

bool RequestLocalNetworkAccess(const std::string &connection) {
    PostgreSQLNetworkEndpoint endpoint;
    if (!ParseRemotePostgreSQLEndpoint(connection, &endpoint)) return false;
    const std::filesystem::path app = std::filesystem::path(HomeDirectory()) /
        "Applications/VexDB Lite.app";
    if (!std::filesystem::exists(app)) return false;
    // The signed host App is sandboxed. Its home directory is the Data folder
    // below this container, while the standalone CLI sees the normal user home.
    // This request contains only host/port/nonce, never the DSN or credentials.
    const std::filesystem::path support = std::filesystem::path(HomeDirectory()) /
        "Library/Containers/io.vexdb.vexfs/Data/Library/Application Support/VexDB-Lite";
    std::filesystem::create_directories(support);
    VexFSPlatformProtectDirectory(support);
    const std::filesystem::path request = support / "local-network-probe.json";
    const std::filesystem::path response = support / "local-network-probe-response.json";
    std::error_code error;
    std::filesystem::remove(request, error);
    std::filesystem::remove(response, error);
    const uint64_t nonce_value = StableHash(connection + std::to_string(getpid()) +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::ostringstream nonce_stream;
    nonce_stream << std::hex << std::setw(16) << std::setfill('0') << nonce_value;
    const std::string nonce = nonce_stream.str();
    const std::filesystem::path temporary =
        support / ("local-network-probe.new." + std::to_string(getpid()));
    std::filesystem::remove(temporary, error);
    const int descriptor = open(temporary.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0) return false;
    std::ostringstream payload;
    payload << "{\"version\":1,\"nonce\":\"" << JsonEscape(nonce)
            << "\",\"host\":\"" << JsonEscape(endpoint.host)
            << "\",\"port\":" << endpoint.port << "}\n";
    const std::string data = payload.str();
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t count = write(descriptor, data.data() + written, data.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        written += static_cast<size_t>(count);
    }
    const bool write_ok = written == data.size() && fsync(descriptor) == 0;
    close(descriptor);
    if (!write_ok || rename(temporary.c_str(), request.c_str()) != 0) {
        std::filesystem::remove(temporary, error);
        std::filesystem::remove(request, error);
        return false;
    }
    std::fprintf(stderr,
        "VexDB Lite is requesting local network access for PostgreSQL; "
        "approve the macOS prompt if it appears.\n");
    RunProcess("/usr/bin/open", {"-a", app.string()});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(32);
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream input(response, std::ios::binary);
        if (input) {
            const std::string result((std::istreambuf_iterator<char>(input)),
                                     std::istreambuf_iterator<char>());
            if (result.find("\"nonce\":\"" + nonce + "\"") != std::string::npos) {
                const bool allowed = result.find("\"allowed\":true") != std::string::npos;
                std::filesystem::remove(request, error);
                std::filesystem::remove(response, error);
                if (!allowed) {
                    std::fprintf(stderr,
                        "VexDB Lite could not use the local network. Enable it in "
                        "System Settings > Privacy & Security > Local Network.\n");
                }
                return allowed;
            }
        }
        usleep(250 * 1000);
    }
    std::filesystem::remove(request, error);
    std::filesystem::remove(response, error);
    std::fprintf(stderr,
        "Timed out waiting for VexDB Lite local network approval.\n");
    return false;
}

std::string StagePostgreSQLPassfile(const std::string &connection,
                                    const std::filesystem::path &directory) {
#if !defined(VEXFS_HAVE_LIBPQ)
    (void)directory;
    return connection;
#else
    char *parse_error = nullptr;
    PQconninfoOption *options = PQconninfoParse(connection.c_str(), &parse_error);
    if (options == nullptr) {
        const std::string message = parse_error == nullptr
            ? "invalid PostgreSQL DSN" : std::string(parse_error);
        if (parse_error != nullptr) PQfreemem(parse_error);
        throw std::runtime_error(message);
    }
    struct OptionsGuard {
        PQconninfoOption *value;
        ~OptionsGuard() { PQconninfoFree(value); }
    } guard{options};

    std::filesystem::path source;
    for (PQconninfoOption *option = options; option->keyword != nullptr; ++option) {
        if (option->val == nullptr || option->val[0] == '\0') continue;
        if (std::strcmp(option->keyword, "password") == 0) {
            throw std::runtime_error(
                "PostgreSQL mount DSN must not resolve an inline password; use passfile");
        }
        if (std::strcmp(option->keyword, "passfile") == 0) source = option->val;
    }
    if (source.empty()) return connection;
    source = std::filesystem::absolute(source).lexically_normal();

    struct stat metadata {};
    const int source_fd = open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source_fd < 0)
        throw std::runtime_error("cannot read PostgreSQL passfile: " +
                                 std::string(std::strerror(errno)));
    struct FileGuard {
        int descriptor;
        ~FileGuard() { if (descriptor >= 0) close(descriptor); }
    } source_guard{source_fd};
    if (fstat(source_fd, &metadata) != 0)
        throw std::runtime_error("cannot inspect PostgreSQL passfile: " +
                                 std::string(std::strerror(errno)));
    if (!S_ISREG(metadata.st_mode))
        throw std::runtime_error("PostgreSQL passfile must be a regular file");
    if (metadata.st_uid != getuid())
        throw std::runtime_error("PostgreSQL passfile must be owned by the current user");
    if ((metadata.st_mode & 0077) != 0)
        throw std::runtime_error("PostgreSQL passfile permissions must be 0600 or stricter");
    if (metadata.st_size < 0 || metadata.st_size > 64 * 1024)
        throw std::runtime_error("PostgreSQL passfile is larger than 64 KiB");

    const std::filesystem::path staged = directory / ".vexfs-pgpass";
    const std::filesystem::path temporary =
        directory / (".vexfs-pgpass.new." + std::to_string(getpid()));
    std::error_code error;
    std::filesystem::remove(temporary, error);
    try {
        const int target_fd = open(temporary.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (target_fd < 0)
            throw std::runtime_error("cannot stage PostgreSQL passfile: " +
                                     std::string(std::strerror(errno)));
        FileGuard target_guard{target_fd};
        char buffer[8192];
        for (;;) {
            const ssize_t count = read(source_fd, buffer, sizeof(buffer));
            if (count == 0) break;
            if (count < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("cannot read PostgreSQL passfile: " +
                                         std::string(std::strerror(errno)));
            }
            ssize_t written = 0;
            while (written < count) {
                const ssize_t result = write(target_fd, buffer + written,
                                             static_cast<size_t>(count - written));
                if (result < 0 && errno == EINTR) continue;
                if (result <= 0)
                    throw std::runtime_error("cannot stage PostgreSQL passfile: " +
                                             std::string(std::strerror(errno)));
                written += result;
            }
        }
        if (fsync(target_fd) != 0)
            throw std::runtime_error("cannot sync PostgreSQL passfile: " +
                                     std::string(std::strerror(errno)));
        if (rename(temporary.c_str(), staged.c_str()) != 0)
            throw std::runtime_error(std::strerror(errno));
    } catch (...) {
        std::filesystem::remove(temporary, error);
        throw;
    }

    std::ostringstream rewritten;
    bool first = true;
    for (PQconninfoOption *option = options; option->keyword != nullptr; ++option) {
        if (option->val == nullptr) continue;
        if (!first) rewritten << ' ';
        first = false;
        rewritten << option->keyword << '=' << EscapeLibpqValue(
            std::strcmp(option->keyword, "passfile") == 0
                ? staged.string() : std::string(option->val));
    }
    return rewritten.str();
#endif
}

struct NfsGatewayResource {
    std::filesystem::path directory;
    std::string connection;
};

NfsGatewayResource PrepareNfsGatewayResource(const std::string &backend,
                                             const std::string &connection,
                                             const std::string &mount_point) {
    StopNfsGateway(mount_point);
    RemoveNfsGatewayState(mount_point);
    NfsGatewayResource resource;
    resource.directory = NfsGatewayStateDirectory(mount_point);
    std::filesystem::create_directories(resource.directory);
    VexFSPlatformProtectDirectory(resource.directory);
    try {
        if (backend == VEXFS_RUNTIME_BACKEND_SQLITE) {
            const std::filesystem::path database =
                std::filesystem::absolute(connection).lexically_normal();
            if (database.filename().empty())
                throw std::runtime_error("database path must include a file name");
            if (database.has_parent_path()) {
                const bool existed = std::filesystem::exists(database.parent_path());
                std::filesystem::create_directories(database.parent_path());
                if (!existed) VexFSPlatformProtectDirectory(database.parent_path());
            }
            resource.connection = database.string();
        } else if (backend == VEXFS_RUNTIME_BACKEND_POSTGRESQL) {
            if (connection.empty()) throw std::runtime_error("PostgreSQL DSN is required");
            if (HasInlinePostgreSQLPassword(connection)) {
                throw std::runtime_error(
                    "PostgreSQL mount DSN must not contain a password; use a 0600 passfile, "
                    "a client certificate, or a libpq service without inline secrets");
            }
            resource.connection = StagePostgreSQLPassfile(connection, resource.directory);
        } else {
            throw std::runtime_error("unsupported VexFS backend: " + backend);
        }
    } catch (...) {
        RemoveNfsGatewayState(mount_point);
        throw;
    }
    return resource;
}

void RemoveStagedPostgreSQLPassfile(const std::filesystem::path &directory) {
    if (directory.empty()) return;
    std::error_code error;
    std::filesystem::remove(directory / ".vexfs-pgpass", error);
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->path().filename().string().rfind(".vexfs-pgpass.new.", 0) == 0)
            std::filesystem::remove(iterator->path(), error);
    }
}

void RemoveStalePostgreSQLPassfiles() {
    const std::filesystem::path root = std::filesystem::path(HomeDirectory()) /
        "Library/Application Support/VexDB-Lite/mount-resources";
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) return;
    const auto mounts = MountedVolumes();
    for (std::filesystem::directory_iterator iterator(root, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_directory(error)) continue;
        const std::string resource = NormalizedPath(iterator->path().string());
        bool active = false;
        for (const auto &mount : mounts) {
            if (NormalizedPath(mount.source) == resource) {
                active = true;
                break;
            }
        }
        if (!active) RemoveStagedPostgreSQLPassfile(iterator->path());
    }
}

std::filesystem::path WriteMountResourceDescriptor(const std::string &backend,
                                                    const std::string &connection,
                                                    const std::string &workspace) {
    std::filesystem::path directory;
    std::string descriptor_connection;
    if (backend == VEXFS_RUNTIME_BACKEND_SQLITE) {
        const std::filesystem::path database =
            std::filesystem::absolute(connection).lexically_normal();
        directory = database.parent_path();
        if (directory.empty() || database.filename().empty())
            throw std::runtime_error("database path must include a file name");
        descriptor_connection = database.filename().string();
    } else if (backend == VEXFS_RUNTIME_BACKEND_POSTGRESQL) {
        if (connection.empty()) throw std::runtime_error("PostgreSQL DSN is required");
        if (HasInlinePostgreSQLPassword(connection)) {
            throw std::runtime_error(
                "PostgreSQL mount DSN must not contain a password; use a 0600 passfile, "
                "a client certificate, or a libpq service without inline secrets");
        }
        std::ostringstream name;
        name << std::hex << std::setw(16) << std::setfill('0')
             << StableHash(connection + std::string(1, '\0') + workspace);
        directory = std::filesystem::path(HomeDirectory()) /
            "Library/Application Support/VexDB-Lite/mount-resources" / name.str();
        descriptor_connection = connection;
    } else {
        throw std::runtime_error("unsupported VexFS backend: " + backend);
    }
    std::filesystem::create_directories(directory);
    VexFSPlatformProtectDirectory(directory);
    if (backend == VEXFS_RUNTIME_BACKEND_POSTGRESQL)
        descriptor_connection = StagePostgreSQLPassfile(connection, directory);
    const std::filesystem::path descriptor = directory / ".vexfs-volume.json";
    std::ofstream output(descriptor, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write mount resource: " + descriptor.string());
    output << "{\n  \"version\": 3,\n  \"backend\": \""
           << JsonEscape(backend)
           << "\",\n  \"connection\": \"" << JsonEscape(descriptor_connection)
           << "\",\n  \"workspace\": \"" << JsonEscape(workspace) << "\"\n}\n";
    output.close();
    if (chmod(descriptor.c_str(), 0600) != 0)
        throw std::runtime_error(std::strerror(errno));
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

VexFSPlatformState VexFSPlatformInspect(const std::string &mount_driver) {
    RemoveStalePostgreSQLPassfiles();
    const std::string driver = RequestedMountDriver(mount_driver);
    VexFSPlatformState state;
    state.platform = "macos";
    state.version = ProductVersion();
    state.platform_supported = ProductVersionSupported(state.version, driver);
    void *framework = dlopen("/System/Library/Frameworks/FSKit.framework/FSKit", RTLD_LAZY);
    const bool fskit_available = framework != nullptr;
    if (framework != nullptr) dlclose(framework);
    const ExtensionState extension = GetExtensionState(&state.extension_path);
    state.extension_state = ExtensionStateName(extension);
    if (const char *expected = std::getenv("VEXFS_EXPECTED_EXTENSION_PATH")) {
        state.extension_path_matches = !state.extension_path.empty() &&
            NormalizedPath(state.extension_path) == NormalizedPath(expected);
    }
    if (driver == "nfs") {
        state.mount_driver = "NFSv3";
        const std::filesystem::path gateway = NfsGatewayExecutable();
        state.mount_driver_available = access("/sbin/mount_nfs", X_OK) == 0 &&
            !gateway.empty() && access(gateway.c_str(), X_OK) == 0;
        // FSKit is still reported as an optional adapter, but its registration
        // never blocks the default NFS path.
        state.mount_ready = state.platform_supported && state.mount_driver_available;
    } else {
        state.mount_driver = "FSKit";
        state.mount_driver_available = fskit_available;
        state.mount_ready = state.platform_supported && state.mount_driver_available &&
                            extension == ExtensionState::kEnabled &&
                            state.extension_path_matches;
    }
    state.mounts = MountedVolumes();
    return state;
}

int VexFSPlatformMount(const std::string &backend, const std::string &connection,
                       const std::string &workspace,
                       const std::string &mount_point,
                       const std::string &mount_driver) {
    const std::string driver = RequestedMountDriver(mount_driver);
    const std::string mounted_type = MountedFileSystemAt(mount_point);
    if (mounted_type == "vexfs" || mounted_type == "nfs") {
        const std::string requested_database = backend == VEXFS_RUNTIME_BACKEND_SQLITE
            ? NormalizedPath(connection) : connection;
        const std::string requested_target = NormalizedPath(mount_point);
        for (const auto &mount : MountedVolumes()) {
            if (NormalizedPath(mount.target) != requested_target) continue;
            if (mount.backend == backend && mount.database == requested_database &&
                mount.workspace == workspace) return 0;
            throw std::runtime_error(
                "mount point already contains a VexFS workspace with different or unknown identity");
        }
        throw std::runtime_error("mount point is VexFS but its identity cannot be verified");
    }
    if (!mounted_type.empty()) {
        throw std::runtime_error("mount point is already used by " + mounted_type);
    }
    const VexFSPlatformState state = VexFSPlatformInspect(driver);
    if (!state.mount_ready) {
        if (driver == "nfs") {
            throw std::runtime_error(
                "VexFS NFS mount is not ready; verify that mount_nfs and "
                "vexfs-nfs-gateway are installed next to vexdb");
        }
        if (state.extension_state == "service-unavailable") {
            throw std::runtime_error(
                "macOS FSKit service is unavailable; reopen System Settings or restart "
                "macOS, then run `vexdb fs doctor` again");
        }
        throw std::runtime_error("VexFS FSKit extension is " + state.extension_state +
            "; install VexDB Lite.app and enable the VexFS file system extension in System Settings");
    }
    PrepareMountPoint(mount_point);
    if (driver == "nfs") {
        const NfsGatewayResource resource =
            PrepareNfsGatewayResource(backend, connection, mount_point);
        UnderlyingMountPointGuard mount_point_guard(mount_point);
        NfsGatewayRecord gateway;
        try {
            try {
                gateway = StartNfsGateway(backend, resource.connection, workspace,
                                          mount_point, resource.directory);
            } catch (...) {
                if (backend != VEXFS_RUNTIME_BACKEND_POSTGRESQL ||
                    !RequestLocalNetworkAccess(connection)) throw;
                gateway = StartNfsGateway(backend, resource.connection, workspace,
                                          mount_point, resource.directory);
            }
            // Keep I/O bounded if the per-mount gateway exits. Keep the normal
            // adaptive RTT estimator: dumbtimer makes sparse fsync workloads
            // wait one fixed timeout for every dirty NFS block on macOS.
            const std::string options =
                "vers=3,tcp,locallocks,soft,timeo=10,retrans=4,"
                "rsize=1048576,wsize=1048576,"
                "deadtimeout=60,actimeo=1,port=" +
                std::to_string(gateway.port) + ",mountport=" +
                std::to_string(gateway.port);
            const int result = RunProcess("/sbin/mount_nfs",
                {"-o", options, "127.0.0.1:/", mount_point}, 30'000);
            if (result != 0 || MountedFileSystemAt(mount_point) != "nfs") {
                if (result == 0)
                    throw std::runtime_error(
                        "mount_nfs returned success but VexFS is not mounted");
                StopNfsGateway(mount_point);
                RemoveNfsGatewayState(mount_point);
                DisarmMountPointGuard(mount_point);
                return result;
            }
            mount_point_guard.Arm();
            VexFSPlatformRememberMount(MountRegistryDirectory(),
                {"127.0.0.1:/", NormalizedPath(mount_point), "nfs", backend,
                 backend == VEXFS_RUNTIME_BACKEND_SQLITE
                     ? NormalizedPath(connection) : connection,
                 workspace});
            return 0;
        } catch (...) {
            if (MountedFileSystemAt(mount_point) == "nfs")
                RunProcess("/sbin/umount", {"-f", mount_point}, 5'000);
            StopNfsGateway(mount_point);
            RemoveNfsGatewayState(mount_point);
            DisarmMountPointGuard(mount_point);
            throw;
        }
    }
    const std::filesystem::path resource_directory =
        WriteMountResourceDescriptor(backend, connection, workspace);
    UnderlyingMountPointGuard mount_point_guard(mount_point);
    int result = 1;
    for (int attempt = 0; attempt < 3; ++attempt) {
        result = RunProcess("/sbin/mount",
            {"-F", "-t", "vexfs", resource_directory.string(), mount_point}, 15'000);
        if (MountedFileSystemAt(mount_point) == "vexfs") {
            // Keep the protected passfile while this resource is mounted. FSKit can
            // reload the resource after its process restarts, and libpq must still
            // be able to authenticate. The last unmount removes the staged copy.
            try {
                mount_point_guard.Arm();
                VexFSPlatformRememberMount(MountRegistryDirectory(),
                    {resource_directory.string(), NormalizedPath(mount_point), "vexfs", backend,
                     backend == VEXFS_RUNTIME_BACKEND_SQLITE
                         ? NormalizedPath(connection) : connection,
                     workspace});
            } catch (...) {
                RunProcess("/sbin/umount", {mount_point}, 5'000);
                RemoveStagedPostgreSQLPassfile(resource_directory);
                DisarmMountPointGuard(mount_point);
                throw;
            }
            return 0;
        }
        if (result == 0) break;
        if (attempt == 0 && backend == VEXFS_RUNTIME_BACKEND_POSTGRESQL)
            RequestLocalNetworkAccess(connection);
        if (attempt != 2) usleep(500 * 1000);
    }
    if (result == 0) {
        RemoveStagedPostgreSQLPassfile(resource_directory);
        DisarmMountPointGuard(mount_point);
        throw std::runtime_error("mount command returned success but VexFS is not mounted");
    }
    RemoveStagedPostgreSQLPassfile(resource_directory);
    DisarmMountPointGuard(mount_point);
    return result;
}

int VexFSPlatformUnmount(const std::string &mount_point, bool force) {
    const std::string mounted_type = MountedFileSystemAt(mount_point);
    if (mounted_type.empty()) {
        StopNfsGateway(mount_point);
        RemoveNfsGatewayState(mount_point);
        VexFSPlatformForgetMount(MountRegistryDirectory(), mount_point);
        RemoveStalePostgreSQLPassfiles();
        DisarmMountPointGuard(mount_point);
        return 0;
    }
    if (mounted_type != "vexfs" && mounted_type != "nfs")
        throw std::runtime_error("mount point is not VexFS: " + mounted_type);
    std::filesystem::path resource_directory;
    const std::string normalized_target = NormalizedPath(mount_point);
    bool verified_mount = false;
    for (const auto &mount : MountedVolumes()) {
        if (NormalizedPath(mount.target) == normalized_target) {
            resource_directory = mount.source;
            verified_mount = true;
            break;
        }
    }
    if (!verified_mount)
        throw std::runtime_error("mount point identity cannot be verified as VexFS");
    int result = 1;
    // fseventsd/Spotlight can briefly hold a newly mounted FSKit volume even
    // when no user process has an open file. Keep this a normal (non-force)
    // unmount, but allow that short system handoff to finish.
    constexpr int kUnmountAttempts = 12;
    for (int attempt = 0; attempt < kUnmountAttempts; ++attempt) {
        result = RunProcess("/sbin/umount",
                            force ? std::vector<std::string>{"-f", mount_point}
                                  : std::vector<std::string>{mount_point}, 5'000);
        if (MountedFileSystemAt(mount_point).empty()) {
            if (mounted_type == "nfs") {
                StopNfsGateway(mount_point);
                RemoveNfsGatewayState(mount_point);
            }
            VexFSPlatformForgetMount(MountRegistryDirectory(), mount_point);
            bool resource_still_mounted = false;
            for (const auto &mount : MountedVolumes()) {
                if (!resource_directory.empty() &&
                    NormalizedPath(mount.source) == NormalizedPath(resource_directory)) {
                    resource_still_mounted = true;
                    break;
                }
            }
            if (mounted_type == "vexfs" && !resource_still_mounted)
                RemoveStagedPostgreSQLPassfile(resource_directory);
            DisarmMountPointGuard(mount_point);
            return 0;
        }
        if (attempt + 1 != kUnmountAttempts) usleep(500 * 1000);
    }
    return result;
}
