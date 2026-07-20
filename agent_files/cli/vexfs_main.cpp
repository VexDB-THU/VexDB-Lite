#include "vexfs_mount_contract.h"
#include "vexfs_fskit_state.h"

#include <dlfcn.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Options {
    std::string database;
    std::string workspace = "default";
    std::string mount_point;
    bool json = false;
    std::vector<std::string> arguments;
};

std::string HomeDirectory() {
    const char *home = std::getenv("HOME");
    return home == nullptr ? std::string(".") : std::string(home);
}

std::string DefaultDatabasePath() {
    if (const char *configured = std::getenv("VEXFS_DATABASE")) return configured;
    const std::filesystem::path directory = std::filesystem::path(HomeDirectory()) /
        "Library/Application Support/VexFS";
    return (directory / "vexfs.sqlite3").string();
}

void Usage(std::ostream &output) {
    output <<
        "Usage: vexfs [--db PATH] [--workspace NAME] COMMAND [ARGS]\n"
        "\n"
        "Commands:\n"
        "  setup [--mount PATH]         Initialize and optionally mount a workspace\n"
        "  init                         Alias for setup without mount\n"
        "  mkdir PATH...                Create directories (including parents)\n"
        "  write PATH [LOCAL_FILE]      Write a local file or stdin\n"
        "  cat PATH                     Print a file\n"
        "  ls [PATH] [--json]           List a directory\n"
        "  stat PATH                    Print file metadata as JSON\n"
        "  history PATH [--limit N] [--before N] [--json]\n"
        "                               List one page of file versions\n"
        "  show PATH --version N        Print one historical version\n"
        "  diff PATH --from N [--to N] Compare two versions (default: current)\n"
        "  restore PATH --version N [--dry-run]\n"
        "                               Restore as a new version\n"
        "  mv SOURCE DESTINATION        Move a file or directory\n"
        "  rm [-r] PATH                 Remove a file or directory\n"
        "  descriptor OUTPUT            Write an FSKit .vexfs descriptor\n"
        "  mount MOUNT_POINT            Mount through the enabled FSKit extension\n"
        "  mount status [MOUNT_POINT]   Show active VexFS mounts\n"
        "  unmount MOUNT_POINT          Unmount a VexFS mount\n"
        "  doctor [--json]              Check macOS, extension and SQLite state\n";
}

Options ParseOptions(int argc, char **argv) {
    Options options;
    options.database = DefaultDatabasePath();
    for (int index = 1; index < argc; ++index) {
        std::string argument = argv[index];
        if (argument == "--db" && index + 1 < argc) {
            options.database = argv[++index];
        } else if (argument == "--workspace" && index + 1 < argc) {
            options.workspace = argv[++index];
        } else if (argument == "--mount" && index + 1 < argc) {
            options.mount_point = argv[++index];
        } else if (argument == "--json") {
            options.json = true;
        } else if (argument == "--help" || argument == "-h") {
            Usage(std::cout);
            std::exit(0);
        } else {
            options.arguments.push_back(std::move(argument));
        }
    }
    return options;
}

std::string ErrorMessage(const vexfs_mount_error &error) {
    return std::string(error.message);
}

const char *ErrorCode(vexfs_mount_status status) {
    switch (status) {
        case VEXFS_MOUNT_INVALID_ARGUMENT: return "VEXFS_INVALID_ARGUMENT";
        case VEXFS_MOUNT_NOT_FOUND: return "VEXFS_NOT_FOUND";
        case VEXFS_MOUNT_CONFLICT: return "VEXFS_CONFLICT";
        case VEXFS_MOUNT_READ_ONLY: return "VEXFS_READ_ONLY";
        case VEXFS_MOUNT_BUSY: return "VEXFS_BUSY";
        case VEXFS_MOUNT_PERMISSION_DENIED: return "VEXFS_PERMISSION_DENIED";
        case VEXFS_MOUNT_NO_SPACE: return "VEXFS_NO_SPACE";
        case VEXFS_MOUNT_CORRUPTION: return "VEXFS_CORRUPTION";
        case VEXFS_MOUNT_UNSUPPORTED: return "VEXFS_UNSUPPORTED";
        case VEXFS_MOUNT_DATABASE_ERROR: return "VEXFS_DATABASE_ERROR";
        case VEXFS_MOUNT_INTERNAL_ERROR: return "VEXFS_INTERNAL_ERROR";
        default: return "VEXFS_ERROR";
    }
}

int ExitCode(vexfs_mount_status status) {
    switch (status) {
        case VEXFS_MOUNT_INVALID_ARGUMENT: return 2;
        case VEXFS_MOUNT_NOT_FOUND: return 3;
        case VEXFS_MOUNT_PERMISSION_DENIED:
        case VEXFS_MOUNT_READ_ONLY: return 4;
        case VEXFS_MOUNT_CONFLICT: return 5;
        case VEXFS_MOUNT_NO_SPACE: return 6;
        case VEXFS_MOUNT_BUSY:
        case VEXFS_MOUNT_DATABASE_ERROR: return 7;
        case VEXFS_MOUNT_CORRUPTION: return 8;
        case VEXFS_MOUNT_UNSUPPORTED: return 9;
        default: return 1;
    }
}

class CliError : public std::runtime_error {
  public:
    CliError(vexfs_mount_status status, const std::string &message)
        : std::runtime_error(message), status(status), exit_code(ExitCode(status)),
          code(ErrorCode(status)) {}

    vexfs_mount_status status;
    int exit_code;
    std::string code;
};

class Session {
  public:
    explicit Session(const Options &options, bool initialize = true) {
        const std::filesystem::path path(options.database);
        if (initialize && path.has_parent_path()) {
            const bool existed = std::filesystem::exists(path.parent_path());
            std::filesystem::create_directories(path.parent_path());
            if (!existed && chmod(path.parent_path().c_str(), 0700) != 0)
                throw std::runtime_error(std::strerror(errno));
        }
        vexfs_mount_config config{};
        config.abi_version = VEXFS_MOUNT_ABI_VERSION;
        config.database_path = options.database.c_str();
        config.workspace = options.workspace.c_str();
        config.busy_timeout_ms = 5000;
        config.flags = initialize ? 0 : VEXFS_MOUNT_OPEN_NO_CREATE;
        vexfs_mount_error error{};
        const auto status = vexfs_mount_session_open(&config, &session_, &error);
        if (status != VEXFS_MOUNT_OK) throw CliError(status, ErrorMessage(error));
    }
    ~Session() { vexfs_mount_session_close(session_); }
    vexfs_mount_session *get() const { return session_; }

  private:
    vexfs_mount_session *session_ = nullptr;
};

std::vector<unsigned char> ReadInput(const std::vector<std::string> &arguments, size_t index) {
    if (arguments.size() > index) {
        std::ifstream input(arguments[index], std::ios::binary);
        if (!input) throw std::runtime_error("cannot open local file: " + arguments[index]);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
    return {std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
}

void Check(vexfs_mount_status status, const vexfs_mount_error &error) {
    if (status != VEXFS_MOUNT_OK) throw CliError(status, ErrorMessage(error));
}

std::string BytesToString(vexfs_mount_bytes *bytes) {
    std::string result;
    if (bytes->data != nullptr) {
        result.assign(static_cast<const char *>(bytes->data), static_cast<size_t>(bytes->size));
        vexfs_mount_free(bytes->data);
    }
    bytes->data = nullptr;
    bytes->size = 0;
    return result;
}

std::string JsonUnescape(const std::string &json, size_t *position) {
    std::string value;
    while (*position < json.size()) {
        const char c = json[(*position)++];
        if (c == '"') break;
        if (c != '\\' || *position >= json.size()) {
            value.push_back(c);
            continue;
        }
        const char escaped = json[(*position)++];
        switch (escaped) {
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            default: value.push_back('?'); break;
        }
    }
    return value;
}

std::string JsonEscape(const std::string &value);

void PrintNames(const std::string &json) {
    const std::string marker = "\"name\":\"";
    size_t position = 0;
    while ((position = json.find(marker, position)) != std::string::npos) {
        position += marker.size();
        std::cout << JsonUnescape(json, &position) << '\n';
    }
}

struct ParsedCommand {
    std::vector<std::string> positional;
    std::vector<std::pair<std::string, std::string>> values;
    std::vector<std::string> flags;

    std::string Value(const std::string &name, bool required = true) const {
        for (const auto &entry : values) {
            if (entry.first == name) return entry.second;
        }
        if (required) throw std::runtime_error("missing required option: " + name);
        return {};
    }

    bool Flag(const std::string &name) const {
        for (const std::string &flag : flags) if (flag == name) return true;
        return false;
    }
};

bool Contains(const std::vector<std::string> &values, const std::string &value) {
    for (const std::string &candidate : values) if (candidate == value) return true;
    return false;
}

ParsedCommand ParseCommand(const std::vector<std::string> &arguments,
                           const std::vector<std::string> &value_options,
                           const std::vector<std::string> &flag_options) {
    ParsedCommand parsed;
    for (size_t index = 1; index < arguments.size(); ++index) {
        const std::string &argument = arguments[index];
        if (Contains(value_options, argument)) {
            if (index + 1 >= arguments.size())
                throw std::runtime_error("option needs a value: " + argument);
            for (const auto &existing : parsed.values) {
                if (existing.first == argument)
                    throw std::runtime_error("option may only be used once: " + argument);
            }
            parsed.values.emplace_back(argument, arguments[++index]);
        } else if (Contains(flag_options, argument)) {
            if (parsed.Flag(argument))
                throw std::runtime_error("option may only be used once: " + argument);
            parsed.flags.push_back(argument);
        } else if (!argument.empty() && argument.front() == '-') {
            throw std::runtime_error("unknown option: " + argument);
        } else {
            parsed.positional.push_back(argument);
        }
    }
    return parsed;
}

int64_t PositiveInteger(const std::string &value, const std::string &name) {
    if (value.empty())
        throw CliError(VEXFS_MOUNT_INVALID_ARGUMENT, name + " must be a positive integer");
    char *end = nullptr;
    errno = 0;
    const long long result = std::strtoll(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0' || result <= 0) {
        throw CliError(VEXFS_MOUNT_INVALID_ARGUMENT, name + " must be a positive integer");
    }
    return static_cast<int64_t>(result);
}

int64_t NonnegativeInteger(const std::string &value, const std::string &name) {
    if (value.empty())
        throw CliError(VEXFS_MOUNT_INVALID_ARGUMENT, name + " must be a non-negative integer");
    char *end = nullptr;
    errno = 0;
    const long long result = std::strtoll(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0' || result < 0) {
        throw CliError(VEXFS_MOUNT_INVALID_ARGUMENT, name + " must be a non-negative integer");
    }
    return static_cast<int64_t>(result);
}

int64_t JsonInteger(const std::string &json, const std::string &name) {
    const std::string marker = "\"" + name + "\":";
    size_t position = json.find(marker);
    if (position == std::string::npos)
        throw std::runtime_error("invalid JSON result: missing " + name);
    position += marker.size();
    char *end = nullptr;
    const long long value = std::strtoll(json.c_str() + position, &end, 10);
    if (end == json.c_str() + position)
        throw std::runtime_error("invalid JSON result: bad " + name);
    return static_cast<int64_t>(value);
}

std::string JsonString(const std::string &json, const std::string &name) {
    const std::string marker = "\"" + name + "\":\"";
    size_t position = json.find(marker);
    if (position == std::string::npos)
        throw std::runtime_error("invalid JSON result: missing " + name);
    position += marker.size();
    return JsonUnescape(json, &position);
}

bool JsonBoolean(const std::string &json, const std::string &name) {
    const std::string marker = "\"" + name + "\":";
    const size_t position = json.find(marker);
    if (position == std::string::npos)
        throw std::runtime_error("invalid JSON result: missing " + name);
    return json.compare(position + marker.size(), 4, "true") == 0;
}

void PrintHistory(const std::string &json) {
    std::cout << "VERSION\tCOMMIT\tSIZE\tCREATED_AT\tCURRENT\tMESSAGE\n";
    const std::string entries_marker = "\"entries\":[";
    size_t position = json.find(entries_marker);
    if (position == std::string::npos) throw std::runtime_error("invalid history JSON");
    position += entries_marker.size();
    while ((position = json.find('{', position)) != std::string::npos) {
        const size_t entries_end = json.find(']', position);
        if (entries_end != std::string::npos && entries_end < position) break;
        const size_t end = json.find('}', position);
        if (end == std::string::npos) throw std::runtime_error("invalid history JSON");
        const std::string row = json.substr(position, end - position + 1);
        std::cout << JsonInteger(row, "version") << '\t'
                  << JsonInteger(row, "commit") << '\t'
                  << JsonInteger(row, "size") << '\t'
                  << JsonInteger(row, "created_at") << '\t'
                  << (JsonBoolean(row, "current") ? "*" : "") << '\t'
                  << JsonString(row, "message") << '\n';
        position = end + 1;
    }
    const std::string next_marker = "\"next_before\":";
    const size_t next = json.find(next_marker);
    if (next != std::string::npos && json.compare(next + next_marker.size(), 4, "null") != 0) {
        std::cout << "NEXT_BEFORE\t" << JsonInteger(json, "next_before") << '\n';
    }
}

std::vector<std::string_view> SplitLines(const std::string &value) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start < value.size()) {
        const size_t newline = value.find('\n', start);
        if (newline == std::string::npos) {
            lines.emplace_back(value.data() + start, value.size() - start);
            break;
        }
        lines.emplace_back(value.data() + start, newline - start + 1);
        start = newline + 1;
    }
    return lines;
}

void PrintDiffLine(char prefix, std::string_view line) {
    std::cout << prefix << line;
    if (line.empty() || line.back() != '\n') {
        std::cout << "\n\\ No newline at end of file\n";
    }
}

bool HasNul(const std::string &value) {
    return value.find('\0') != std::string::npos;
}

bool PrintDiff(const std::string &path, int64_t from_version, const std::string &from,
               int64_t to_version, const std::string &to, bool json) {
    const bool changed = from != to;
    const bool binary = HasNul(from) || HasNul(to);
    if (json) {
        std::cout << "{\"path\":\"" << JsonEscape(path) << "\",\"from\":"
                  << from_version << ",\"to\":" << to_version
                  << ",\"changed\":" << (changed ? "true" : "false")
                  << ",\"binary\":" << (binary ? "true" : "false")
                  << ",\"from_size\":" << from.size()
                  << ",\"to_size\":" << to.size() << "}\n";
        return changed;
    }
    if (!changed) return false;
    std::cout << "--- " << path << '@' << from_version << '\n'
              << "+++ " << path << '@' << to_version << '\n';
    if (binary) {
        std::cout << "Binary files differ\n";
        return true;
    }
    constexpr size_t kDetailedDiffLimit = 4u * 1024u * 1024u;
    if (from.size() > kDetailedDiffLimit || to.size() > kDetailedDiffLimit) {
        std::cout << "Text files differ; detailed diff omitted for files larger than 4 MiB\n";
        return true;
    }
    const auto old_lines = SplitLines(from);
    const auto new_lines = SplitLines(to);
    size_t prefix = 0;
    while (prefix < old_lines.size() && prefix < new_lines.size() &&
           old_lines[prefix] == new_lines[prefix]) ++prefix;
    size_t suffix = 0;
    while (suffix < old_lines.size() - prefix && suffix < new_lines.size() - prefix &&
           old_lines[old_lines.size() - suffix - 1] == new_lines[new_lines.size() - suffix - 1]) {
        ++suffix;
    }
    const size_t context_start = prefix > 3 ? prefix - 3 : 0;
    const size_t old_change_end = old_lines.size() - suffix;
    const size_t new_change_end = new_lines.size() - suffix;
    const size_t old_end = std::min(old_lines.size(), old_change_end + 3);
    const size_t new_end = std::min(new_lines.size(), new_change_end + 3);
    std::cout << "@@ -" << context_start + 1 << ',' << old_end - context_start
              << " +" << context_start + 1 << ',' << new_end - context_start << " @@\n";
    for (size_t index = context_start; index < prefix; ++index)
        PrintDiffLine(' ', old_lines[index]);
    for (size_t index = prefix; index < old_change_end; ++index)
        PrintDiffLine('-', old_lines[index]);
    for (size_t index = prefix; index < new_change_end; ++index)
        PrintDiffLine('+', new_lines[index]);
    for (size_t index = new_change_end; index < new_end; ++index)
        PrintDiffLine(' ', new_lines[index]);
    return true;
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

void WriteDescriptor(const Options &options, const std::filesystem::path &path) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write descriptor: " + path.string());
    output << "{\n  \"version\": 1,\n  \"database_path\": \""
           << JsonEscape(std::filesystem::absolute(options.database).string())
           << "\",\n  \"workspace\": \"" << JsonEscape(options.workspace) << "\"\n}\n";
}

std::filesystem::path WriteMountResourceDescriptor(const Options &options) {
    const std::filesystem::path database =
        std::filesystem::absolute(options.database).lexically_normal();
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
           << "\",\n  \"workspace\": \"" << JsonEscape(options.workspace) << "\"\n}\n";
    return directory;
}

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

struct ProcessOutput {
    int exit_code = 1;
    std::string output;
};

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

enum class ExtensionState { kMissing, kEnabled, kDisabled, kRegistered };

ExtensionState VexFSExtensionState() {
    char module_url[4096]{};
    const int fskit_state = vexfs_fskit_extension_state(
        "io.vexdb.vexfs.extension", module_url, sizeof(module_url));
    if (std::getenv("VEXFS_DEBUG") != nullptr) {
        std::cerr << "vexfs: FSKit state=" << fskit_state
                  << " module=" << module_url << '\n';
    }
    if (fskit_state == 2) return ExtensionState::kEnabled;
    if (fskit_state == 1) return ExtensionState::kDisabled;

    // FSKit filters out modules whose signature or provisioning it cannot accept.
    // pluginkit can still distinguish that state from a package that is not installed.
    // Its '+' marker is registration/election, not the FSKit per-user enable switch,
    // so it must never claim that mounting is ready.
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

struct MountedVolume {
    std::string source;
    std::string target;
    std::string type;
};

std::string NormalizedPath(const std::string &path) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    if (!error) return canonical.string();
    return std::filesystem::absolute(path).lexically_normal().string();
}

std::vector<MountedVolume> VexFSMounts() {
    struct statfs *entries = nullptr;
    const int count = getmntinfo(&entries, MNT_NOWAIT);
    std::vector<MountedVolume> mounts;
    for (int index = 0; index < count; ++index) {
        if (std::strcmp(entries[index].f_fstypename, "vexfs") != 0) continue;
        mounts.push_back({entries[index].f_mntfromname, entries[index].f_mntonname,
                          entries[index].f_fstypename});
    }
    return mounts;
}

bool ProductVersionSupported(const std::string &version) {
    char *end = nullptr;
    const long major = std::strtol(version.c_str(), &end, 10);
    return end != version.c_str() && major >= 26;
}

std::string ProductVersion() {
    char buffer[128] = {};
    size_t size = sizeof(buffer);
    if (sysctlbyname("kern.osproductversion", buffer, &size, nullptr, 0) != 0) return "unknown";
    return buffer;
}

std::string DatabaseDiagnostics(Session &session) {
    vexfs_mount_bytes json{};
    vexfs_mount_error error{};
    Check(vexfs_mount_diagnostics(session.get(), &json, &error), error);
    return BytesToString(&json);
}

std::string ReadHistoricalVersion(Session &session, const std::string &path, int64_t version) {
    vexfs_mount_bytes content{};
    vexfs_mount_error error{};
    Check(vexfs_mount_read_version(session.get(), path.c_str(), version, &content, &error), error);
    return BytesToString(&content);
}

int64_t CurrentFileVersion(Session &session, const std::string &path) {
    vexfs_mount_bytes json{};
    vexfs_mount_error error{};
    Check(vexfs_mount_stat(session.get(), path.c_str(), &json, &error), error);
    return JsonInteger(BytesToString(&json), "version");
}

struct VersionComparison {
    std::string json;
    bool changed = false;
    bool binary = false;
    int64_t from_size = 0;
    int64_t to_size = 0;
};

VersionComparison CompareHistoricalVersions(Session &session, const std::string &path,
                                             int64_t from_version, int64_t to_version) {
    vexfs_mount_bytes json{};
    vexfs_mount_error error{};
    Check(vexfs_mount_compare_versions(session.get(), path.c_str(), from_version, to_version,
                                       &json, &error), error);
    VersionComparison result;
    result.json = BytesToString(&json);
    result.changed = JsonBoolean(result.json, "changed");
    result.binary = JsonBoolean(result.json, "binary");
    result.from_size = JsonInteger(result.json, "from_size");
    result.to_size = JsonInteger(result.json, "to_size");
    return result;
}

bool RunVersionDiff(Session &session, const std::string &path, int64_t from_version,
                    int64_t to_version, bool json) {
    const VersionComparison comparison =
        CompareHistoricalVersions(session, path, from_version, to_version);
    if (json) {
        std::cout << comparison.json << '\n';
        return comparison.changed;
    }
    if (!comparison.changed) return false;
    constexpr int64_t kDetailedDiffLimit = 4LL * 1024LL * 1024LL;
    if (comparison.binary || comparison.from_size > kDetailedDiffLimit ||
        comparison.to_size > kDetailedDiffLimit) {
        std::cout << "--- " << path << '@' << from_version << '\n'
                  << "+++ " << path << '@' << to_version << '\n';
        if (comparison.binary) std::cout << "Binary files differ\n";
        else std::cout << "Text files differ; detailed diff omitted for files larger than 4 MiB\n";
        return true;
    }
    const std::string from = ReadHistoricalVersion(session, path, from_version);
    const std::string to = ReadHistoricalVersion(session, path, to_version);
    return PrintDiff(path, from_version, from, to_version, to, false);
}

int PrintMountStatus(const Options &options, const std::string &requested_path) {
    const std::string normalized = requested_path.empty() ? "" : NormalizedPath(requested_path);
    std::vector<MountedVolume> mounts;
    for (const auto &mount : VexFSMounts()) {
        if (normalized.empty() || NormalizedPath(mount.target) == normalized) mounts.push_back(mount);
    }
    if (options.json) {
        std::cout << '[';
        for (size_t index = 0; index < mounts.size(); ++index) {
            if (index != 0) std::cout << ',';
            std::cout << "{\"source\":\"" << JsonEscape(mounts[index].source)
                      << "\",\"target\":\"" << JsonEscape(mounts[index].target)
                      << "\",\"type\":\"" << JsonEscape(mounts[index].type) << "\"}";
        }
        std::cout << "]\n";
    } else if (mounts.empty()) {
        std::cout << "no VexFS mounts\n";
    } else {
        for (const auto &mount : mounts)
            std::cout << mount.target << " <- " << mount.source << " (" << mount.type << ")\n";
    }
    return requested_path.empty() || !mounts.empty() ? 0 : 1;
}

void PrepareMountPoint(const std::string &path) {
    const std::filesystem::path mount_point(path);
    if (std::filesystem::exists(mount_point) && !std::filesystem::is_directory(mount_point)) {
        throw std::runtime_error("mount point is not a directory: " + path);
    }
    std::filesystem::create_directories(mount_point);
    if (chmod(mount_point.c_str(), 0700) != 0) throw std::runtime_error(std::strerror(errno));
    if (std::filesystem::directory_iterator(mount_point) != std::filesystem::directory_iterator()) {
        throw std::runtime_error("mount point must be empty: " + path);
    }
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

int MountWorkspace(const Options &options, const std::string &mount_point) {
    const std::string mounted_type = MountedFileSystemAt(mount_point);
    if (mounted_type == "vexfs") {
        std::cout << mount_point << " is already mounted\n";
        return 0;
    }
    if (!mounted_type.empty()) {
        throw std::runtime_error("mount point is already used by " + mounted_type);
    }
    const ExtensionState extension = VexFSExtensionState();
    if (extension != ExtensionState::kEnabled) {
        throw std::runtime_error(std::string("VexFS extension is ") +
            ExtensionStateName(extension) +
            "; install VexFS.app and enable its file system extension in System Settings");
    }
    PrepareMountPoint(mount_point);
    const std::filesystem::path resource_directory = WriteMountResourceDescriptor(options);
    int result = 1;
    for (int attempt = 0; attempt < 3; ++attempt) {
        result = RunProcess("/sbin/mount",
            {"-F", "-t", "vexfs", resource_directory.string(), mount_point});
        if (MountedFileSystemAt(mount_point) == "vexfs") {
            result = 0;
            break;
        }
        if (result == 0) break;
        if (attempt != 2) usleep(500 * 1000);
    }
    if (result == 0 && MountedFileSystemAt(mount_point) != "vexfs") {
        throw std::runtime_error("mount command returned success but VexFS is not mounted");
    }
    return result;
}

int RunDoctor(const Options &options) {
    const std::string version = ProductVersion();
    const bool supported_macos = ProductVersionSupported(version);
    void *framework = dlopen("/System/Library/Frameworks/FSKit.framework/FSKit", RTLD_LAZY);
    const bool fskit = framework != nullptr;
    if (framework != nullptr) dlclose(framework);
    const ExtensionState extension = VexFSExtensionState();
    const auto mounts = VexFSMounts();
    std::string database_details;
    std::string database_error;
    try {
        Session session(options, false);
        database_details = DatabaseDiagnostics(session);
    } catch (const std::exception &error) {
        database_error = error.what();
    }
    const bool database_readable = !database_details.empty();
    const bool database_ok = database_readable && JsonBoolean(database_details, "compatible");
    if (options.json) {
        std::cout << "{\"macos\":\"" << JsonEscape(version)
                  << "\",\"macos_supported\":" << (supported_macos ? "true" : "false")
                  << ",\"fskit\":" << (fskit ? "true" : "false")
                  << ",\"extension\":\"" << ExtensionStateName(extension) << "\""
                  << ",\"database\":";
        if (database_readable) std::cout << database_details;
        else std::cout << "{\"path\":\"" << JsonEscape(options.database)
                       << "\",\"error\":\"" << JsonEscape(database_error) << "\"}";
        std::cout << ",\"mount_count\":" << mounts.size() << "}\n";
    } else {
        std::cout << "macOS: " << version << (supported_macos ? " (supported)" : " (requires 26+)") << '\n'
                  << "FSKit: " << (fskit ? "available" : "missing") << '\n'
                  << "extension: " << ExtensionStateName(extension) << '\n'
                  << "database: " << options.database << '\n'
                  << "workspace: " << options.workspace << '\n'
                  << "SQLite contract: " << (database_ok ? "ok" :
                      (database_readable ? "upgrade required or incompatible" : database_error)) << '\n'
                  << "active mounts: " << mounts.size() << '\n';
        if (database_readable) std::cout << "database details: " << database_details << '\n';
    }
    return supported_macos && fskit && extension == ExtensionState::kEnabled && database_ok ? 0 : 1;
}

int Run(const Options &options) {
    if (options.arguments.empty()) {
        Usage(std::cerr);
        return 2;
    }
    const std::string &command = options.arguments[0];

    if (command == "descriptor") {
        if (options.arguments.size() != 2) throw std::runtime_error("descriptor needs OUTPUT");
        WriteDescriptor(options, options.arguments[1]);
        std::cout << options.arguments[1] << '\n';
        return 0;
    }
    if (command == "mount") {
        if (options.arguments.size() >= 2 && options.arguments[1] == "status") {
            if (options.arguments.size() > 3) throw std::runtime_error("mount status accepts one path");
            return PrintMountStatus(options,
                options.arguments.size() == 3 ? options.arguments[2] : std::string());
        }
        if (options.arguments.size() != 2) throw std::runtime_error("mount needs MOUNT_POINT");
        return MountWorkspace(options, options.arguments[1]);
    }
    if (command == "unmount") {
        if (options.arguments.size() != 2) throw std::runtime_error("unmount needs MOUNT_POINT");
        return RunProcess("/sbin/umount", {options.arguments[1]});
    }
    if (command == "doctor") {
        if (options.arguments.size() != 1) throw std::runtime_error("doctor accepts only global options");
        return RunDoctor(options);
    }

    Session session(options);
    vexfs_mount_error error{};
    if (command == "init" || command == "setup") {
        if (options.arguments.size() != 1) throw std::runtime_error(command + " accepts only global options");
        std::cout << options.database << " [" << options.workspace << "]\n";
        if (!options.mount_point.empty()) return MountWorkspace(options, options.mount_point);
    } else if (command == "mkdir") {
        if (options.arguments.size() < 2) throw std::runtime_error("mkdir needs PATH");
        for (size_t i = 1; i < options.arguments.size(); ++i)
            Check(vexfs_mount_mkdir(session.get(), options.arguments[i].c_str(), &error), error);
    } else if (command == "write") {
        if (options.arguments.size() < 2 || options.arguments.size() > 3)
            throw std::runtime_error("write needs PATH and optional LOCAL_FILE");
        const auto content = ReadInput(options.arguments, 2);
        int64_t version = 0;
        Check(vexfs_mount_write_file(session.get(), options.arguments[1].c_str(),
                                     content.data(), content.size(), &version, &error), error);
        std::cout << version << '\n';
    } else if (command == "cat") {
        if (options.arguments.size() != 2) throw std::runtime_error("cat needs PATH");
        vexfs_mount_bytes content{};
        Check(vexfs_mount_read_file(session.get(), options.arguments[1].c_str(), &content, &error), error);
        if (content.data != nullptr) std::cout.write(static_cast<const char *>(content.data), content.size);
        vexfs_mount_free(content.data);
    } else if (command == "ls") {
        const std::string path = options.arguments.size() >= 2 ? options.arguments[1] : "/";
        vexfs_mount_bytes json{};
        Check(vexfs_mount_list(session.get(), path.c_str(), &json, &error), error);
        const std::string value = BytesToString(&json);
        if (options.json) std::cout << value << '\n'; else PrintNames(value);
    } else if (command == "stat") {
        if (options.arguments.size() != 2) throw std::runtime_error("stat needs PATH");
        vexfs_mount_bytes json{};
        Check(vexfs_mount_stat(session.get(), options.arguments[1].c_str(), &json, &error), error);
        std::cout << BytesToString(&json) << '\n';
    } else if (command == "history") {
        const ParsedCommand parsed = ParseCommand(options.arguments, {"--limit", "--before"}, {});
        if (parsed.positional.size() != 1) throw std::runtime_error("history needs PATH");
        const std::string limit_value = parsed.Value("--limit", false);
        const std::string before_value = parsed.Value("--before", false);
        const int64_t limit = limit_value.empty() ? 100 : PositiveInteger(limit_value, "limit");
        const int64_t before = before_value.empty() ? 0 : NonnegativeInteger(before_value, "before");
        if (limit > 1000)
            throw CliError(VEXFS_MOUNT_INVALID_ARGUMENT, "limit must be at most 1000");
        vexfs_mount_bytes json{};
        Check(vexfs_mount_history_page(session.get(), parsed.positional[0].c_str(),
                                       static_cast<uint32_t>(limit), before, &json, &error), error);
        const std::string value = BytesToString(&json);
        if (options.json) std::cout << value << '\n'; else PrintHistory(value);
    } else if (command == "show") {
        const ParsedCommand parsed = ParseCommand(options.arguments, {"--version"}, {});
        if (parsed.positional.size() != 1)
            throw std::runtime_error("show needs PATH and --version N");
        const int64_t version = PositiveInteger(parsed.Value("--version"), "version");
        const std::string content = ReadHistoricalVersion(session, parsed.positional[0], version);
        if (!content.empty()) std::cout.write(content.data(), static_cast<std::streamsize>(content.size()));
    } else if (command == "diff") {
        const ParsedCommand parsed = ParseCommand(options.arguments, {"--from", "--to"}, {});
        if (parsed.positional.size() != 1)
            throw std::runtime_error("diff needs PATH, --from N and optional --to N");
        const std::string &path = parsed.positional[0];
        const int64_t from_version = PositiveInteger(parsed.Value("--from"), "from version");
        const std::string to_value = parsed.Value("--to", false);
        const int64_t to_version = to_value.empty() ? CurrentFileVersion(session, path) :
            PositiveInteger(to_value, "to version");
        return RunVersionDiff(session, path, from_version, to_version, options.json) ? 1 : 0;
    } else if (command == "restore") {
        const ParsedCommand parsed = ParseCommand(options.arguments, {"--version"}, {"--dry-run"});
        if (parsed.positional.size() != 1)
            throw std::runtime_error("restore needs PATH and --version N");
        const std::string &path = parsed.positional[0];
        const int64_t target_version = PositiveInteger(parsed.Value("--version"), "version");
        const int64_t current_version = CurrentFileVersion(session, path);
        if (parsed.Flag("--dry-run")) {
            RunVersionDiff(session, path, current_version, target_version, options.json);
        } else {
            int64_t new_version = 0;
            Check(vexfs_mount_restore_version(session.get(), path.c_str(), target_version,
                                              current_version, &new_version, &error), error);
            if (options.json) {
                std::cout << "{\"path\":\"" << JsonEscape(path) << "\",\"restored_from\":"
                          << target_version << ",\"previous_version\":" << current_version
                          << ",\"version\":" << new_version << "}\n";
            } else {
                std::cout << new_version << '\n';
            }
        }
    } else if (command == "mv") {
        if (options.arguments.size() != 3) throw std::runtime_error("mv needs SOURCE DESTINATION");
        Check(vexfs_mount_move(session.get(), options.arguments[1].c_str(),
                               options.arguments[2].c_str(), &error), error);
    } else if (command == "rm") {
        bool recursive = false;
        size_t path_index = 1;
        if (options.arguments.size() > 1 && (options.arguments[1] == "-r" || options.arguments[1] == "-R")) {
            recursive = true;
            path_index = 2;
        }
        if (options.arguments.size() != path_index + 1) throw std::runtime_error("rm needs PATH");
        Check(vexfs_mount_remove(session.get(), options.arguments[path_index].c_str(),
                                 recursive ? 1 : 0, &error), error);
    } else {
        throw std::runtime_error("unknown command: " + command);
    }
    return 0;
}

bool WantsJson(int argc, char **argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--json") == 0) return true;
    }
    return false;
}

void PrintError(const std::string &code, const std::string &message, int exit_code,
                bool json) {
    if (json) {
        std::cerr << "{\"error\":{\"code\":\"" << JsonEscape(code)
                  << "\",\"message\":\"" << JsonEscape(message)
                  << "\",\"exit_code\":" << exit_code << "}}\n";
    } else {
        std::cerr << "vexfs: " << message << '\n';
    }
}

}  // namespace

int main(int argc, char **argv) {
    try {
        return Run(ParseOptions(argc, argv));
    } catch (const CliError &error) {
        PrintError(error.code, error.what(), error.exit_code, WantsJson(argc, argv));
        return error.exit_code;
    } catch (const std::exception &error) {
        PrintError("VEXFS_ERROR", error.what(), 1, WantsJson(argc, argv));
        return 1;
    }
}
