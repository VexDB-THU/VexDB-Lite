#include "vexfs_runtime_admin.h"
#include "vexfs_archive.h"
#include "vexfs_platform.h"

#include <cerrno>
#include <algorithm>
#include <cctype>
#include <chrono>
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
#include <thread>
#include <utility>
#include <vector>

namespace {

struct Options {
    std::string backend = VEXFS_RUNTIME_BACKEND_SQLITE;
    std::string database;
    std::string dsn;
    std::string workspace = "default";
    std::string mount_point;
    std::string mount_driver;
    bool json = false;
    std::vector<std::string> arguments;
};

std::string g_program_name = "vexfs";

void Usage(std::ostream &output) {
    output <<
        "Usage: " << g_program_name << " [--db PATH | --backend pg --dsn DSN] "
        "[--workspace NAME] [--mount-driver DRIVER] COMMAND [ARGS]\n"
        "\n"
        "Commands:\n"
        "  setup [--mount PATH]         Initialize and optionally mount a workspace\n"
        "  init                         Alias for setup without mount\n"
        "  mkdir PATH...                Create directories (including parents)\n"
        "  write PATH [LOCAL_FILE]      Write a local file or stdin\n"
        "  cat PATH                     Print a file\n"
        "  ls [PATH] [--json]           List a directory\n"
        "  find [PATH] [--name GLOB] [--type KIND] [--min-size BYTES]\n"
        "       [--max-size BYTES] [--modified-after EPOCH_MS]\n"
        "       [--modified-before EPOCH_MS] [--limit N] [--after PATH]\n"
        "                               Query names and metadata without file bodies\n"
        "  grep [-i] [-l] [-n] [--max-results N] PATTERN [PATH]\n"
        "                               Search current text files in the workspace\n"
        "  index status|enable|rebuild|disable\n"
        "                               Manage the optional trigram text index\n"
        "  check [--quick]              Verify workspace metadata and content\n"
        "                               --quick skips SHA-256 BLOB hashing\n"
        "  quota show                   Show current usage and hard limits\n"
        "  quota set --max-bytes N|unlimited --max-files N|unlimited\n"
        "            --max-file-bytes N|unlimited\n"
        "                               Set workspace quota (sizes are bytes)\n"
        "  retention show               Show history retention and storage usage\n"
        "  retention set --keep-versions N --keep-days N\n"
        "                               Set explicit history retention policy\n"
        "  gc [--batch N]               Delete one bounded batch of unreferenced history\n"
        "                               Current files, snapshots and handles are protected\n"
        "  gc pause|resume              Pause or resume history deletion\n"
        "  stat PATH                    Print file metadata as JSON\n"
        "  workspace log [--limit N] [--before COMMIT] [--json]\n"
        "                               List one page of workspace commits\n"
        "  history PATH [--limit N] [--before N] [--json]\n"
        "                               List one page of file versions\n"
        "  show PATH --version N        Print one historical version\n"
        "  diff PATH --from N [--to N] Compare two versions (default: current)\n"
        "  restore PATH --version N [--dry-run]\n"
        "                               Restore as a new version\n"
        "  snapshot create NAME [--type manual|agent|safety] [--committed-only]\n"
        "                               Snapshot all published data; default refuses\n"
        "                               while another mount has unpublished writes\n"
        "  snapshot list                List workspace snapshots\n"
        "  snapshot policy show         Show snapshot classes and retention policy\n"
        "  snapshot policy set --agent-keep N --safety-keep N --days N\n"
        "                               Set automatic snapshot retention policy\n"
        "  snapshot prune [--dry-run]   Remove expired agent/safety snapshot references\n"
        "  snapshot show NAME           Show the complete historical tree as JSON\n"
        "  snapshot diff FROM [--to TO] Compare snapshots (default TO: HEAD)\n"
        "  snapshot restore NAME [--dry-run] [--force-unmount]\n"
        "                               Restore the complete tree as a new commit\n"
        "  snapshot drop NAME           Delete a snapshot name, not its history\n"
        "  export --output FILE [--snapshot NAME]\n"
        "                               Export a checked logical workspace package\n"
        "  import FILE                  Verify and atomically publish a new workspace\n"
        "  archive verify FILE          Verify package records and content checksums\n"
        "  mv SOURCE DESTINATION        Move a file or directory\n"
        "  ln SOURCE DESTINATION        Create a hard link to a regular file\n"
        "  chown UID:GID PATH            Store owner IDs (- keeps an ID)\n"
        "  getfacl PATH                  Print the portable ACL JSON\n"
        "  setfacl PATH [LOCAL_FILE]     Replace ACL JSON from file or stdin\n"
        "  rm [-r] PATH                 Remove a file or directory\n"
        "  descriptor OUTPUT            Write a portable workspace descriptor\n"
        "  mount MOUNT_POINT            Mount through the platform adapter\n"
        "                               macOS defaults to nfs; fskit is optional\n"
        "  mount status [MOUNT_POINT]   Show active VexFS mounts\n"
        "  unmount [--force] MOUNT_POINT\n"
        "                               Unmount; --force detaches a stale mount\n"
        "  doctor [--json]              Check platform adapter and database state\n";
}

Options ParseOptions(int argc, char **argv) {
    Options options;
    options.database = VexFSPlatformDefaultDatabasePath();
    for (int index = 1; index < argc; ++index) {
        std::string argument = argv[index];
        if (argument == "--db" && index + 1 < argc) {
            options.database = argv[++index];
        } else if (argument == "--backend" && index + 1 < argc) {
            options.backend = argv[++index];
            if (options.backend == "pg" || options.backend == "postgres") {
                options.backend = "postgresql";
            }
        } else if (argument == "--dsn" && index + 1 < argc) {
            options.dsn = argv[++index];
        } else if (argument == "--workspace" && index + 1 < argc) {
            options.workspace = argv[++index];
        } else if (argument == "--mount" && index + 1 < argc) {
            options.mount_point = argv[++index];
        } else if (argument == "--mount-driver" && index + 1 < argc) {
            options.mount_driver = argv[++index];
        } else if (argument == "--json") {
            options.json = true;
        } else if (argument == "--help" || argument == "-h") {
            Usage(std::cout);
            std::exit(0);
        } else {
            options.arguments.push_back(std::move(argument));
        }
    }
    if (options.backend != VEXFS_RUNTIME_BACKEND_SQLITE &&
        options.backend != "postgresql") {
        throw std::runtime_error("backend must be sqlite or pg");
    }
    if (options.backend == "postgresql" && options.dsn.empty()) {
        throw std::runtime_error("PostgreSQL backend needs --dsn DSN");
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
        case VEXFS_MOUNT_NOT_EMPTY: return "VEXFS_NOT_EMPTY";
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
        case VEXFS_MOUNT_NOT_EMPTY: return 5;
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
        const bool postgresql = options.backend == VEXFS_RUNTIME_BACKEND_POSTGRESQL;
        const std::filesystem::path path(options.database);
        if (!postgresql && initialize && path.has_parent_path()) {
            const bool existed = std::filesystem::exists(path.parent_path());
            std::filesystem::create_directories(path.parent_path());
            if (!existed) VexFSPlatformProtectDirectory(path.parent_path());
        }
        vexfs_mount_config config{};
        config.abi_version = VEXFS_RUNTIME_ABI_VERSION;
        config.backend = options.backend.c_str();
        config.connection = postgresql ? options.dsn.c_str() : options.database.c_str();
        config.workspace = options.workspace.c_str();
        config.principal = postgresql ? "" : "local";
        config.operation_timeout_ms = 5000;
        config.flags = initialize ? 0 : VEXFS_RUNTIME_OPEN_NO_CREATE;
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

void PrintFindPaths(const std::string &json) {
    const std::string marker = "\"path\"";
    size_t position = 0;
    while ((position = json.find(marker, position)) != std::string::npos) {
        position += marker.size();
        position = json.find(':', position);
        if (position == std::string::npos) throw std::runtime_error("invalid find JSON");
        ++position;
        while (position < json.size() && std::isspace(
                   static_cast<unsigned char>(json[position]))) ++position;
        if (position >= json.size() || json[position] != '"') {
            throw std::runtime_error("invalid find JSON");
        }
        ++position;
        std::cout << JsonUnescape(json, &position) << '\n';
    }
}

int64_t JsonInteger(const std::string &json, const std::string &name);
std::string JsonString(const std::string &json, const std::string &name);

void PrintGrep(const std::string &json, bool files_only, bool show_line) {
    const std::string matches_marker = "\"matches\":[";
    size_t position = json.find(matches_marker);
    if (position == std::string::npos) throw std::runtime_error("invalid grep JSON");
    position += matches_marker.size();
    while (position < json.size() && json[position] != ']') {
        if (json[position] == ',') ++position;
        if (position >= json.size() || json[position] != '{') {
            throw std::runtime_error("invalid grep JSON");
        }
        const size_t row_start = position;
        const size_t row_end = json.find('}', row_start);
        if (row_end == std::string::npos) throw std::runtime_error("invalid grep JSON");
        const std::string row = json.substr(row_start, row_end - row_start + 1);
        const std::string path = JsonString(row, "path");
        const long long line = JsonInteger(row, "line");
        const std::string text = JsonString(row, "text");
        std::cout << path;
        if (!files_only) {
            if (show_line) std::cout << ':' << line;
            std::cout << ':' << text;
        }
        std::cout << '\n';
        position = row_end + 1;
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

int64_t QuotaValue(const std::string &value, const std::string &name) {
    if (value == "unlimited") return -1;
    return NonnegativeInteger(value, name);
}

int64_t OwnerId(const std::string &value, const std::string &name) {
    if (value == "-") return -1;
    char *end = nullptr;
    errno = 0;
    const long long result = std::strtoll(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0' || result < 0 ||
        static_cast<unsigned long long>(result) > 0xffffffffULL) {
        throw CliError(VEXFS_MOUNT_INVALID_ARGUMENT,
                       name + " must be - or an integer in 0..4294967295");
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

void PrintSnapshots(const std::string &json) {
    std::cout << "NAME\tTYPE\tCOMMIT\tCREATED_AT\n";
    const std::string marker = "\"name\":\"";
    size_t position = 0;
    while ((position = json.find(marker, position)) != std::string::npos) {
        position += marker.size();
        const std::string name = JsonUnescape(json, &position);
        const std::string row = json.substr(position);
        std::cout << name << '\t'
                  << JsonString(row, "type") << '\t'
                  << JsonInteger(row, "commit") << '\t'
                  << JsonInteger(row, "created_at") << '\n';
    }
}

void PrintCheck(const std::string &json) {
    const bool ok = JsonBoolean(json, "ok");
    std::cout << (ok ? "OK" : "CORRUPT")
              << " workspace=" << JsonString(json, "workspace")
              << " mode=" << JsonString(json, "mode")
              << " issues=" << JsonInteger(json, "issue_count")
              << " versions=" << JsonInteger(json, "versions")
              << " content_bytes=" << JsonInteger(json, "content_bytes")
              << " elapsed_ms=" << JsonInteger(json, "elapsed_ms") << '\n';
    const std::string code_marker = "\"code\":\"";
    size_t position = 0;
    while ((position = json.find(code_marker, position)) != std::string::npos) {
        const size_t row_end = json.find('}', position);
        if (row_end == std::string::npos) throw std::runtime_error("invalid check JSON");
        const std::string row = json.substr(position, row_end - position + 1);
        std::cout << JsonString(row, "code") << '\t'
                  << JsonString(row, "object") << '\t'
                  << JsonString(row, "message") << '\t'
                  << JsonString(row, "suggestion") << '\n';
        position = row_end + 1;
    }
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

void PrintWorkspaceLog(const std::string &json) {
    std::cout << "COMMIT\tCREATED_AT\tACTOR\tOPERATION\tPATH\tSNAPSHOT\n";
    const std::string entries_marker = "\"entries\":[";
    size_t position = json.find(entries_marker);
    if (position == std::string::npos) throw std::runtime_error("invalid workspace log JSON");
    position += entries_marker.size();
    while ((position = json.find('{', position)) != std::string::npos) {
        const size_t end = json.find('}', position);
        if (end == std::string::npos) throw std::runtime_error("invalid workspace log JSON");
        const std::string row = json.substr(position, end - position + 1);
        std::cout << JsonInteger(row, "commit") << '\t'
                  << JsonInteger(row, "created_at") << '\t'
                  << JsonString(row, "actor") << '\t'
                  << JsonString(row, "operation") << '\t'
                  << JsonString(row, "path") << '\t'
                  << (JsonBoolean(row, "has_snapshot") ? "*" : "") << '\n';
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
    auto has_inline_password = [](const std::string &connection) {
        std::string compact;
        compact.reserve(connection.size());
        for (const unsigned char value : connection) {
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n')
                compact.push_back(static_cast<char>(std::tolower(value)));
        }
        if (compact.find("password=") != std::string::npos) return true;
        const size_t scheme = compact.find("://");
        if (scheme == std::string::npos) return false;
        const size_t authority = scheme + 3;
        const size_t at = compact.find('@', authority);
        return at != std::string::npos && compact.find(':', authority) < at;
    };
    if (options.backend == VEXFS_RUNTIME_BACKEND_POSTGRESQL &&
        has_inline_password(options.dsn)) {
        throw std::runtime_error(
            "descriptor refuses an inline PostgreSQL password; use a client certificate "
            "or a libpq service without inline secrets");
    }
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write descriptor: " + path.string());
    const std::string connection = options.backend == VEXFS_RUNTIME_BACKEND_POSTGRESQL
        ? options.dsn : std::filesystem::absolute(options.database).string();
    output << "{\n  \"version\": 2,\n  \"backend\": \""
           << JsonEscape(options.backend)
           << "\",\n  \"connection\": \"" << JsonEscape(connection)
           << "\",\n  \"workspace\": \"" << JsonEscape(options.workspace) << "\"\n}\n";
    output.close();
    std::error_code error;
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, error);
    if (error) throw std::runtime_error("cannot protect descriptor: " + error.message());
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

std::string NormalizedPath(const std::string &path) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    if (!error) return canonical.string();
    return std::filesystem::absolute(path).lexically_normal().string();
}

std::vector<VexFSPlatformMountEntry> WorkspaceMounts(const Options &options) {
    const bool postgresql = options.backend == VEXFS_RUNTIME_BACKEND_POSTGRESQL;
    const std::string database = postgresql ? options.dsn : NormalizedPath(options.database);
    std::vector<VexFSPlatformMountEntry> matches;
    for (const auto &mount : VexFSPlatformInspect(options.mount_driver).mounts) {
        if (mount.backend != options.backend) continue;
        const bool same_connection = postgresql
            ? mount.database == database
            : !mount.database.empty() && NormalizedPath(mount.database) == database;
        if (same_connection &&
            mount.workspace == options.workspace) {
            matches.push_back(mount);
        }
    }
    return matches;
}

int PrintMountStatus(const Options &options, const std::string &requested_path) {
    const std::string requested = requested_path.empty() ? "" : NormalizedPath(requested_path);
    std::vector<VexFSPlatformMountEntry> mounts;
    for (const auto &mount : VexFSPlatformInspect(options.mount_driver).mounts) {
        if (requested.empty() || NormalizedPath(mount.target) == requested) mounts.push_back(mount);
    }
    if (options.json) {
        std::cout << '[';
        for (size_t index = 0; index < mounts.size(); ++index) {
            if (index != 0) std::cout << ',';
            std::cout << "{\"source\":\"" << JsonEscape(mounts[index].source)
                      << "\",\"target\":\"" << JsonEscape(mounts[index].target)
                      << "\",\"type\":\"" << JsonEscape(mounts[index].type)
                      << "\",\"backend\":\"" << JsonEscape(mounts[index].backend)
                      << "\",\"database\":\"" << JsonEscape(mounts[index].database)
                      << "\",\"workspace\":\"" << JsonEscape(mounts[index].workspace)
                      << "\"}";
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

int MountWorkspace(const Options &options, const std::string &mount_point) {
    return VexFSPlatformMount(
        options.backend,
        options.backend == VEXFS_RUNTIME_BACKEND_POSTGRESQL
            ? options.dsn : options.database,
        options.workspace, mount_point, options.mount_driver);
}

int UnmountWorkspace(const std::string &mount_point, bool force) {
    return VexFSPlatformUnmount(mount_point, force);
}

void RemountWorkspace(const Options &options, const std::string &mount_point) {
    std::string last_error = "mount returned a non-zero status";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    do {
        try {
            const int result = MountWorkspace(options, mount_point);
            if (result == 0) return;
            last_error = "mount exited with status " + std::to_string(result);
        } catch (const std::exception &error) {
            last_error = error.what();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error(last_error);
}

std::string SafetySnapshotName(const std::string &workspace, int64_t head) {
    std::string safe_workspace;
    safe_workspace.reserve(std::min<std::size_t>(workspace.size(), 32));
    for (const unsigned char byte : workspace) {
        if (safe_workspace.size() >= 32) break;
        safe_workspace.push_back(
            std::isalnum(byte) || byte == '-' || byte == '_' ?
                static_cast<char>(byte) : '_');
    }
    if (safe_workspace.empty()) safe_workspace = "workspace";
    const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto monotonic = std::chrono::steady_clock::now().time_since_epoch().count();
    return "vexfs-safety-" + safe_workspace + "-h" + std::to_string(head) + "-" +
           std::to_string(wall) + "-" +
           std::to_string(static_cast<unsigned long long>(monotonic));
}

int64_t RestoreSnapshotAfterUnmount(Session &session, const std::string &name,
                                    int64_t head, const std::string &safety_name,
                                    bool wait_for_mount_shutdown,
                                    std::chrono::seconds shutdown_timeout =
                                        std::chrono::seconds(35)) {
    const auto deadline = std::chrono::steady_clock::now() + shutdown_timeout;
    while (true) {
        int64_t commit = 0;
        vexfs_mount_error error{};
        const vexfs_mount_status status = vexfs_mount_snapshot_restore_safe(
            session.get(), name.c_str(), head, safety_name.c_str(), &commit, &error);
        if (status == VEXFS_MOUNT_OK) return commit;
        const std::string message = ErrorMessage(error);
        const bool mount_state_is_draining =
            message.find("active mount session") != std::string::npos ||
            message.find("file handle") != std::string::npos;
        if (!wait_for_mount_shutdown || status != VEXFS_MOUNT_BUSY ||
            !mount_state_is_draining ||
            std::chrono::steady_clock::now() >= deadline) {
            Check(status, error);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

int RunDoctor(const Options &options) {
    const VexFSPlatformState platform = VexFSPlatformInspect(options.mount_driver);
    std::string database_details;
    std::string database_error;
    try {
        Session session(options, false);
        database_details = DatabaseDiagnostics(session);
    } catch (const std::exception &error) {
        database_error = error.what();
    }
    const bool database_readable = !database_details.empty();
    const bool database_ok = database_readable && JsonBoolean(database_details, "schema_ready");
    if (options.json) {
        std::cout << "{\"platform\":\"" << JsonEscape(platform.platform)
                  << "\",\"runtime_abi\":" << VEXFS_RUNTIME_ABI_VERSION
                  << ",\"platform_version\":\"" << JsonEscape(platform.version)
                  << "\",\"platform_supported\":"
                  << (platform.platform_supported ? "true" : "false")
                  << ",\"mount_driver\":\"" << JsonEscape(platform.mount_driver)
                  << "\",\"mount_driver_available\":"
                  << (platform.mount_driver_available ? "true" : "false")
                  << ",\"mount_ready\":" << (platform.mount_ready ? "true" : "false")
                  << ",\"extension\":\"" << JsonEscape(platform.extension_state) << "\""
                  << ",\"extension_path\":\"" << JsonEscape(platform.extension_path) << "\""
                  << ",\"extension_path_matches\":"
                  << (platform.extension_path_matches ? "true" : "false");
        if (platform.platform == "macos") {
            std::cout << ",\"macos\":\"" << JsonEscape(platform.version)
                      << "\",\"macos_supported\":"
                      << (platform.platform_supported ? "true" : "false")
                      << ",\"fskit\":"
                      << (platform.extension_state == "enabled" ? "true" : "false");
        }
        std::cout << ",\"backend\":\"" << JsonEscape(options.backend)
                  << "\",\"database\":";
        if (database_readable) std::cout << database_details;
        else std::cout << "{\"connection\":\""
                       << (options.backend == VEXFS_RUNTIME_BACKEND_POSTGRESQL
                           ? "postgresql" : JsonEscape(options.database))
                       << "\",\"error\":\"" << JsonEscape(database_error) << "\"}";
        std::cout << ",\"mount_count\":" << platform.mounts.size() << "}\n";
    } else {
        std::cout << "platform: " << platform.platform << ' ' << platform.version
                  << (platform.platform_supported ? " (supported)" : " (unsupported)") << '\n'
                  << "mount driver: " << platform.mount_driver << ' '
                  << (platform.mount_driver_available ? "(available)" : "(unavailable)") << '\n'
                  << (platform.platform == "macos" && platform.mount_driver == "NFSv3"
                      ? "optional FSKit: " : "mount state: ")
                  << platform.extension_state << '\n'
                  << (platform.extension_path.empty() ? std::string() :
                      "extension path: " + platform.extension_path + "\n")
                  << (platform.extension_path_matches ? std::string() :
                      "extension path: does not match installed App\n")
                  << "backend: " << options.backend << '\n'
                  << "database: "
                  << (options.backend == VEXFS_RUNTIME_BACKEND_POSTGRESQL
                      ? "PostgreSQL (DSN hidden)" : options.database) << '\n'
                  << "workspace: " << options.workspace << '\n'
                  << "database schema: " << (database_ok ? "ok" :
                      (database_readable ? "version mismatch" : database_error)) << '\n'
                  << "active mounts: " << platform.mounts.size() << '\n';
        if (database_readable) std::cout << "database details: " << database_details << '\n';
    }
    return platform.mount_ready && database_ok ? 0 : 1;
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
        if (options.arguments.size() == 2)
            return UnmountWorkspace(options.arguments[1], false);
        if (options.arguments.size() == 3 && options.arguments[1] == "--force")
            return UnmountWorkspace(options.arguments[2], true);
        throw std::runtime_error("unmount needs [--force] MOUNT_POINT");
    }
    if (command == "doctor") {
        if (options.arguments.size() != 1) throw std::runtime_error("doctor accepts only global options");
        return RunDoctor(options);
    }
    if (command == "export") {
        const ParsedCommand parsed = ParseCommand(
            options.arguments, {"--output", "--snapshot"}, {});
        if (!parsed.positional.empty()) {
            throw std::runtime_error("export accepts --output FILE and optional --snapshot NAME");
        }
        const std::string output = parsed.Value("--output");
        const std::string snapshot = parsed.Value("--snapshot", false);
        if (options.backend == "postgresql") {
            std::cout << vexfs_cli::ExportPostgresArchive(
                options.dsn, options.workspace, snapshot, output) << '\n';
        } else {
            std::cout << vexfs_cli::ExportArchive(
                options.database, options.workspace, snapshot, output) << '\n';
        }
        return 0;
    }
    if (command == "import") {
        if (options.arguments.size() != 2) throw std::runtime_error("import needs FILE");
        if (options.backend == "postgresql") {
            std::cout << vexfs_cli::ImportPostgresArchive(
                options.dsn, options.workspace, options.arguments[1]) << '\n';
        } else {
            std::cout << vexfs_cli::ImportArchive(
                options.database, options.workspace, options.arguments[1]) << '\n';
        }
        return 0;
    }
    if (command == "archive") {
        if (options.arguments.size() != 3 || options.arguments[1] != "verify") {
            throw std::runtime_error("archive needs verify FILE");
        }
        std::cout << vexfs_cli::VerifyArchive(options.arguments[2]) << '\n';
        return 0;
    }

    // check 必须保持只读边界：不创建数据库、workspace、WAL/SHM，也不发布 staging。
    Session session(options, command != "check");
    vexfs_mount_error error{};
    if (command == "init" || command == "setup") {
        if (options.arguments.size() != 1) throw std::runtime_error(command + " accepts only global options");
        std::cout << (options.backend == VEXFS_RUNTIME_BACKEND_POSTGRESQL
            ? "postgresql" : options.database) << " [" << options.workspace << "]\n";
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
    } else if (command == "find") {
        const ParsedCommand parsed = ParseCommand(
            options.arguments,
            {"--name", "--type", "--min-size", "--max-size", "--modified-after",
             "--modified-before", "--limit", "--after"}, {});
        if (parsed.positional.size() > 1) {
            throw std::runtime_error("find accepts one optional PATH");
        }
        const std::string path = parsed.positional.empty() ? "/" : parsed.positional[0];
        const std::string name_pattern = parsed.Value("--name", false);
        std::string kind = parsed.Value("--type", false);
        if (kind == "f") kind = "file";
        if (kind == "d") kind = "directory";
        if (kind == "l") kind = "symlink";
        if (!kind.empty() && kind != "file" && kind != "directory" && kind != "symlink") {
            throw CliError(VEXFS_MOUNT_INVALID_ARGUMENT,
                           "type must be file, directory, symlink, f, d or l");
        }
        const std::string min_value = parsed.Value("--min-size", false);
        const std::string max_value = parsed.Value("--max-size", false);
        const std::string after_time_value = parsed.Value("--modified-after", false);
        const std::string before_time_value = parsed.Value("--modified-before", false);
        const std::string limit_value = parsed.Value("--limit", false);
        const std::string after_path = parsed.Value("--after", false);
        const int64_t min_size = min_value.empty() ? -1 :
            NonnegativeInteger(min_value, "min-size");
        const int64_t max_size = max_value.empty() ? -1 :
            NonnegativeInteger(max_value, "max-size");
        const int64_t modified_after = after_time_value.empty() ? -1 :
            NonnegativeInteger(after_time_value, "modified-after");
        const int64_t modified_before = before_time_value.empty() ? -1 :
            NonnegativeInteger(before_time_value, "modified-before");
        const int64_t limit = limit_value.empty() ? 100 :
            PositiveInteger(limit_value, "limit");
        if (limit > 1000) {
            throw CliError(VEXFS_MOUNT_INVALID_ARGUMENT, "limit must be at most 1000");
        }
        if (min_size >= 0 && max_size >= 0 && min_size > max_size) {
            throw CliError(VEXFS_MOUNT_INVALID_ARGUMENT,
                           "min-size must not exceed max-size");
        }
        if (modified_after >= 0 && modified_before >= 0 &&
            modified_after > modified_before) {
            throw CliError(VEXFS_MOUNT_INVALID_ARGUMENT,
                           "modified-after must not exceed modified-before");
        }
        if (!after_path.empty() && after_path.front() != '/') {
            throw CliError(VEXFS_MOUNT_INVALID_ARGUMENT,
                           "after cursor must be an absolute path");
        }
        vexfs_mount_bytes json{};
        Check(vexfs_mount_find(
            session.get(), path.c_str(), name_pattern.c_str(), kind.c_str(),
            min_size, max_size, modified_after, modified_before, after_path.c_str(),
            static_cast<uint32_t>(limit), &json, &error), error);
        const std::string value = BytesToString(&json);
        if (options.json) std::cout << value << '\n'; else PrintFindPaths(value);
    } else if (command == "grep") {
        const ParsedCommand parsed = ParseCommand(
            options.arguments, {"--max-results"}, {"-i", "-l", "-n"});
        if (parsed.positional.empty() || parsed.positional.size() > 2) {
            throw std::runtime_error("grep needs PATTERN and optional PATH");
        }
        const std::string limit_value = parsed.Value("--max-results", false);
        const int64_t limit = limit_value.empty() ? 1000 : PositiveInteger(limit_value, "max-results");
        if (limit > 10240) {
            throw CliError(VEXFS_MOUNT_INVALID_ARGUMENT, "max-results must be at most 10240");
        }
        uint32_t flags = 0;
        if (parsed.Flag("-i")) flags |= VEXFS_MOUNT_GREP_IGNORE_CASE;
        if (parsed.Flag("-l")) flags |= VEXFS_MOUNT_GREP_FILES_ONLY;
        const std::string path = parsed.positional.size() == 2 ? parsed.positional[1] : "/";
        vexfs_mount_bytes json{};
        Check(vexfs_mount_grep(session.get(), path.c_str(), parsed.positional[0].c_str(),
                               flags, static_cast<uint32_t>(limit), &json, &error), error);
        const std::string value = BytesToString(&json);
        if (options.json) std::cout << value << '\n';
        else PrintGrep(value, parsed.Flag("-l"), parsed.Flag("-n"));
        return JsonInteger(value, "match_count") == 0 ? 1 : 0;
    } else if (command == "index") {
        if (options.arguments.size() != 2) {
            throw std::runtime_error("index needs status, enable, rebuild or disable");
        }
        vexfs_mount_bytes json{};
        Check(vexfs_mount_grep_index(session.get(), options.arguments[1].c_str(),
                                     &json, &error), error);
        std::cout << BytesToString(&json) << '\n';
    } else if (command == "check") {
        const ParsedCommand parsed = ParseCommand(options.arguments, {}, {"--quick"});
        if (!parsed.positional.empty()) {
            throw std::runtime_error("check accepts only --quick and global --json");
        }
        vexfs_mount_bytes json{};
        const uint32_t flags = parsed.Flag("--quick") ? VEXFS_MOUNT_CHECK_QUICK : 0;
        Check(vexfs_mount_check(session.get(), flags, &json, &error), error);
        const std::string value = BytesToString(&json);
        if (options.json) std::cout << value << '\n'; else PrintCheck(value);
        return JsonBoolean(value, "ok") ? 0 : ExitCode(VEXFS_MOUNT_CORRUPTION);
    } else if (command == "quota") {
        if (options.arguments.size() < 2)
            throw std::runtime_error("quota needs show or set");
        const std::string &action = options.arguments[1];
        vexfs_mount_bytes json{};
        if (action == "show") {
            if (options.arguments.size() != 2)
                throw std::runtime_error("quota show accepts no arguments");
            Check(vexfs_mount_quota_get(session.get(), &json, &error), error);
        } else if (action == "set") {
            const ParsedCommand parsed = ParseCommand(
                options.arguments,
                {"--max-bytes", "--max-files", "--max-file-bytes"}, {});
            if (parsed.positional.size() != 1 || parsed.positional[0] != "set") {
                throw std::runtime_error(
                    "quota set needs --max-bytes, --max-files and --max-file-bytes");
            }
            const int64_t max_bytes = QuotaValue(
                parsed.Value("--max-bytes"), "max-bytes");
            const int64_t max_files = QuotaValue(
                parsed.Value("--max-files"), "max-files");
            const int64_t max_file_bytes = QuotaValue(
                parsed.Value("--max-file-bytes"), "max-file-bytes");
            Check(vexfs_mount_quota_set(session.get(), max_bytes, max_files,
                                        max_file_bytes, &json, &error), error);
        } else {
            throw std::runtime_error("unknown quota command: " + action);
        }
        std::cout << BytesToString(&json) << '\n';
    } else if (command == "retention") {
        if (options.arguments.size() < 2)
            throw std::runtime_error("retention needs show or set");
        const std::string &action = options.arguments[1];
        vexfs_mount_bytes json{};
        if (action == "show") {
            if (options.arguments.size() != 2)
                throw std::runtime_error("retention show accepts no arguments");
            Check(vexfs_mount_retention_get(session.get(), &json, &error), error);
        } else if (action == "set") {
            const ParsedCommand parsed = ParseCommand(
                options.arguments, {"--keep-versions", "--keep-days"}, {});
            if (parsed.positional.size() != 1 || parsed.positional[0] != "set") {
                throw std::runtime_error(
                    "retention set needs --keep-versions N and --keep-days N");
            }
            const int64_t keep_versions = NonnegativeInteger(
                parsed.Value("--keep-versions"), "keep-versions");
            const int64_t keep_days = NonnegativeInteger(
                parsed.Value("--keep-days"), "keep-days");
            if (keep_versions > 1000000 || keep_days > 36500) {
                throw CliError(VEXFS_MOUNT_INVALID_ARGUMENT,
                    "keep-versions must be at most 1000000 and keep-days at most 36500");
            }
            Check(vexfs_mount_retention_set(
                session.get(), static_cast<uint32_t>(keep_versions),
                static_cast<uint32_t>(keep_days), &json, &error), error);
        } else {
            throw std::runtime_error("unknown retention command: " + action);
        }
        std::cout << BytesToString(&json) << '\n';
    } else if (command == "gc") {
        if (options.arguments.size() == 2 &&
            (options.arguments[1] == "pause" || options.arguments[1] == "resume")) {
            vexfs_mount_bytes json{};
            Check(vexfs_mount_gc_pause(session.get(), options.arguments[1] == "pause",
                                       &json, &error), error);
            std::cout << BytesToString(&json) << '\n';
            return 0;
        }
        const ParsedCommand parsed = ParseCommand(options.arguments, {"--batch"}, {});
        if (!parsed.positional.empty())
            throw std::runtime_error("gc accepts only --batch N");
        const std::string batch_value = parsed.Value("--batch", false);
        const int64_t batch = batch_value.empty() ? 1000 :
            PositiveInteger(batch_value, "batch");
        if (batch > 10000)
            throw CliError(VEXFS_MOUNT_INVALID_ARGUMENT, "batch must be at most 10000");
        vexfs_mount_bytes json{};
        Check(vexfs_mount_gc(session.get(), static_cast<uint32_t>(batch), &json, &error), error);
        std::cout << BytesToString(&json) << '\n';
    } else if (command == "stat") {
        if (options.arguments.size() != 2) throw std::runtime_error("stat needs PATH");
        vexfs_mount_bytes json{};
        Check(vexfs_mount_stat(session.get(), options.arguments[1].c_str(), &json, &error), error);
        std::cout << BytesToString(&json) << '\n';
    } else if (command == "workspace") {
        const ParsedCommand parsed = ParseCommand(
            options.arguments, {"--limit", "--before"}, {});
        if (parsed.positional.size() != 1 || parsed.positional[0] != "log") {
            throw std::runtime_error("workspace needs log with optional --limit/--before");
        }
        const std::string limit_value = parsed.Value("--limit", false);
        const std::string before_value = parsed.Value("--before", false);
        const int64_t limit = limit_value.empty() ? 100 :
            PositiveInteger(limit_value, "limit");
        const int64_t before = before_value.empty() ? 0 :
            NonnegativeInteger(before_value, "before");
        if (limit > 1000) {
            throw CliError(VEXFS_MOUNT_INVALID_ARGUMENT, "limit must be at most 1000");
        }
        vexfs_mount_bytes json{};
        Check(vexfs_mount_workspace_log_page(
            session.get(), static_cast<uint32_t>(limit), before, &json, &error), error);
        const std::string value = BytesToString(&json);
        if (options.json) std::cout << value << '\n'; else PrintWorkspaceLog(value);
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
    } else if (command == "snapshot") {
        if (options.arguments.size() < 2)
            throw std::runtime_error(
                "snapshot needs create, list, show, diff, restore, drop, policy or prune");
        const std::string &action = options.arguments[1];
        if (action == "create") {
            const ParsedCommand parsed = ParseCommand(
                options.arguments, {"--type"}, {"--committed-only"});
            if (parsed.positional.size() != 2 || parsed.positional[0] != "create")
                throw std::runtime_error(
                    "snapshot create needs NAME and optional --type/--committed-only");
            const std::string snapshot_type = parsed.Value("--type", false).empty()
                ? "manual" : parsed.Value("--type");
            if (snapshot_type != "manual" && snapshot_type != "agent" &&
                snapshot_type != "safety") {
                throw CliError(
                    VEXFS_MOUNT_INVALID_ARGUMENT,
                    "snapshot type must be manual, agent or safety");
            }
            const uint32_t flags = parsed.Flag("--committed-only")
                ? VEXFS_SNAPSHOT_COMMITTED_ONLY : 0;
            int64_t commit = 0;
            Check(vexfs_mount_snapshot_create_typed(
                session.get(), parsed.positional[1].c_str(), snapshot_type.c_str(),
                flags, &commit, &error), error);
            if (options.json) {
                std::cout << "{\"name\":\"" << JsonEscape(parsed.positional[1])
                          << "\",\"commit\":" << commit
                          << ",\"type\":\"" << snapshot_type << "\""
                          << ",\"consistency\":\""
                          << (flags == 0 ? "consistent" : "committed-only") << "\"}\n";
            } else {
                std::cout << commit << '\n';
            }
        } else if (action == "list") {
            if (options.arguments.size() != 2)
                throw std::runtime_error("snapshot list accepts no arguments");
            vexfs_mount_bytes json{};
            Check(vexfs_mount_snapshot_list(session.get(), &json, &error), error);
            const std::string value = BytesToString(&json);
            if (options.json) std::cout << value << '\n'; else PrintSnapshots(value);
        } else if (action == "policy") {
            if (options.arguments.size() < 3) {
                throw std::runtime_error("snapshot policy needs show or set");
            }
            vexfs_mount_bytes json{};
            if (options.arguments[2] == "show") {
                if (options.arguments.size() != 3) {
                    throw std::runtime_error("snapshot policy show accepts no arguments");
                }
                Check(vexfs_mount_snapshot_policy_get(session.get(), &json, &error), error);
            } else if (options.arguments[2] == "set") {
                const ParsedCommand parsed = ParseCommand(
                    options.arguments,
                    {"--agent-keep", "--safety-keep", "--days"}, {});
                if (parsed.positional.size() != 2 ||
                    parsed.positional[0] != "policy" ||
                    parsed.positional[1] != "set") {
                    throw std::runtime_error(
                        "snapshot policy set needs --agent-keep N --safety-keep N --days N");
                }
                const int64_t agent_keep = NonnegativeInteger(
                    parsed.Value("--agent-keep"), "agent-keep");
                const int64_t safety_keep = NonnegativeInteger(
                    parsed.Value("--safety-keep"), "safety-keep");
                const int64_t days = NonnegativeInteger(parsed.Value("--days"), "days");
                if (agent_keep > 1000000 || safety_keep > 1000000 || days > 36500) {
                    throw CliError(
                        VEXFS_MOUNT_INVALID_ARGUMENT,
                        "snapshot keep counts must be at most 1000000 and days at most 36500");
                }
                Check(vexfs_mount_snapshot_policy_set(
                    session.get(), static_cast<uint32_t>(agent_keep),
                    static_cast<uint32_t>(safety_keep), static_cast<uint32_t>(days),
                    &json, &error), error);
            } else {
                throw std::runtime_error("unknown snapshot policy command");
            }
            std::cout << BytesToString(&json) << '\n';
        } else if (action == "prune") {
            const ParsedCommand parsed = ParseCommand(
                options.arguments, {}, {"--dry-run"});
            if (parsed.positional.size() != 1 || parsed.positional[0] != "prune") {
                throw std::runtime_error("snapshot prune accepts only --dry-run");
            }
            vexfs_mount_bytes json{};
            Check(vexfs_mount_snapshot_prune(
                session.get(), parsed.Flag("--dry-run") ? 1 : 0, &json, &error), error);
            std::cout << BytesToString(&json) << '\n';
        } else if (action == "show") {
            if (options.arguments.size() != 3)
                throw std::runtime_error("snapshot show needs NAME");
            vexfs_mount_bytes json{};
            Check(vexfs_mount_snapshot_show(session.get(), options.arguments[2].c_str(),
                                            &json, &error), error);
            std::cout << BytesToString(&json) << '\n';
        } else if (action == "diff") {
            const ParsedCommand parsed = ParseCommand(options.arguments, {"--to"}, {});
            if (parsed.positional.size() != 2 || parsed.positional[0] != "diff")
                throw std::runtime_error("snapshot diff needs FROM and optional --to TO");
            const std::string to = parsed.Value("--to", false).empty() ?
                "HEAD" : parsed.Value("--to");
            vexfs_mount_bytes json{};
            Check(vexfs_mount_snapshot_diff(session.get(), parsed.positional[1].c_str(),
                                            to.c_str(), &json, &error), error);
            const std::string value = BytesToString(&json);
            std::cout << value << '\n';
            return value.find("\"changes\":[]") == std::string::npos ? 1 : 0;
        } else if (action == "restore") {
            const ParsedCommand parsed = ParseCommand(
                options.arguments, {}, {"--dry-run", "--force-unmount"});
            if (parsed.positional.size() != 2 || parsed.positional[0] != "restore")
                throw std::runtime_error(
                    "snapshot restore needs NAME and optional --dry-run/--force-unmount");
            if (parsed.Flag("--dry-run") && parsed.Flag("--force-unmount"))
                throw std::runtime_error("--force-unmount cannot be combined with --dry-run");
            const std::string &name = parsed.positional[1];
            if (parsed.Flag("--dry-run")) {
                vexfs_mount_bytes json{};
                Check(vexfs_mount_snapshot_diff(session.get(), "HEAD", name.c_str(),
                                                &json, &error), error);
                std::cout << BytesToString(&json) << '\n';
            } else {
                const std::vector<VexFSPlatformMountEntry> mounts = WorkspaceMounts(options);
                if (mounts.size() > 1) {
                    throw CliError(VEXFS_MOUNT_BUSY,
                        "workspace has more than one active mount; unmount them before restore");
                }
                const std::string mount_point = mounts.empty() ? "" : mounts.front().target;
                if (!mount_point.empty()) {
                    const int unmount = UnmountWorkspace(
                        mount_point, parsed.Flag("--force-unmount"));
                    if (unmount != 0) {
                        throw CliError(VEXFS_MOUNT_BUSY,
                            "cannot unmount active workspace; snapshot restore was not started");
                    }
                }

                // Unmount can publish dirty FSKit handles and legitimately advance
                // the workspace head. Read expected-head only after the last mount
                // is gone, otherwise our own close path causes a false conflict.
                int64_t head = 0;
                Check(vexfs_mount_workspace_head(session.get(), &head, &error), error);
                const std::string safety_name = SafetySnapshotName(options.workspace, head);
                int64_t commit = 0;
                try {
                    commit = RestoreSnapshotAfterUnmount(
                        session, name, head, safety_name, !mount_point.empty(),
                        std::chrono::seconds(35));
                } catch (const std::exception &restore_error) {
                    const std::string restore_message = restore_error.what();
                    if (!mount_point.empty()) {
                        try {
                            RemountWorkspace(options, mount_point);
                        } catch (const std::exception &remount_error) {
                            throw std::runtime_error(
                                "snapshot restore failed: " + restore_message +
                                "; workspace remount also failed: " + remount_error.what());
                        }
                    }
                    throw;
                }
                if (!mount_point.empty()) {
                    try {
                        RemountWorkspace(options, mount_point);
                    } catch (const std::exception &remount_error) {
                        throw std::runtime_error(
                            "snapshot was restored at commit " + std::to_string(commit) +
                            "; safety snapshot is " + safety_name +
                            " but workspace remount failed: " + remount_error.what() +
                            "; mount it again at " + mount_point);
                    }
                }
                if (options.json) {
                    std::cout << "{\"name\":\"" << JsonEscape(name)
                              << "\",\"previous_head\":" << head
                              << ",\"commit\":" << commit
                              << ",\"safety_snapshot\":\""
                              << JsonEscape(safety_name) << "\""
                              << ",\"remounted\":"
                              << (mount_point.empty() ? "false" : "true");
                    if (!mount_point.empty())
                        std::cout << ",\"mount_point\":\"" << JsonEscape(mount_point) << "\"";
                    std::cout << "}\n";
                } else {
                    std::cout << commit << '\n';
                }
            }
        } else if (action == "drop") {
            if (options.arguments.size() != 3)
                throw std::runtime_error("snapshot drop needs NAME");
            Check(vexfs_mount_snapshot_drop(session.get(), options.arguments[2].c_str(),
                                            &error), error);
        } else {
            throw std::runtime_error("unknown snapshot command: " + action);
        }
    } else if (command == "mv") {
        if (options.arguments.size() != 3) throw std::runtime_error("mv needs SOURCE DESTINATION");
        Check(vexfs_mount_move(session.get(), options.arguments[1].c_str(),
                               options.arguments[2].c_str(), &error), error);
    } else if (command == "ln") {
        if (options.arguments.size() != 3) throw std::runtime_error("ln needs SOURCE DESTINATION");
        Check(vexfs_mount_link(session.get(), options.arguments[1].c_str(),
                               options.arguments[2].c_str(), &error), error);
    } else if (command == "chown") {
        if (options.arguments.size() != 3) throw std::runtime_error("chown needs UID:GID PATH");
        const std::string &owner = options.arguments[1];
        const size_t separator = owner.find(':');
        if (separator == std::string::npos || owner.find(':', separator + 1) != std::string::npos)
            throw std::runtime_error("chown owner must be UID:GID");
        const int64_t uid = OwnerId(owner.substr(0, separator), "uid");
        const int64_t gid = OwnerId(owner.substr(separator + 1), "gid");
        vexfs_mount_bytes stat{};
        Check(vexfs_mount_stat(session.get(), options.arguments[2].c_str(), &stat, &error), error);
        const int64_t inode = JsonInteger(BytesToString(&stat), "inode");
        Check(vexfs_mount_chown(session.get(), inode, uid, gid, &error), error);
    } else if (command == "getfacl") {
        if (options.arguments.size() != 2) throw std::runtime_error("getfacl needs PATH");
        vexfs_mount_bytes stat{};
        Check(vexfs_mount_stat(session.get(), options.arguments[1].c_str(), &stat, &error), error);
        const int64_t inode = JsonInteger(BytesToString(&stat), "inode");
        vexfs_mount_bytes acl{};
        Check(vexfs_mount_acl_get(session.get(), inode, &acl, &error), error);
        std::cout << BytesToString(&acl) << '\n';
    } else if (command == "setfacl") {
        if (options.arguments.size() < 2 || options.arguments.size() > 3)
            throw std::runtime_error("setfacl needs PATH and optional LOCAL_FILE");
        vexfs_mount_bytes stat{};
        Check(vexfs_mount_stat(session.get(), options.arguments[1].c_str(), &stat, &error), error);
        const int64_t inode = JsonInteger(BytesToString(&stat), "inode");
        const auto acl = ReadInput(options.arguments, 2);
        Check(vexfs_mount_acl_set(session.get(), inode, acl.data(), acl.size(), &error), error);
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
        std::cerr << g_program_name << ": " << message << '\n';
    }
}

}  // namespace

int VexFsMain(int argc, char **argv) {
    if (argc > 0 && argv[0] != nullptr && *argv[0] != '\0') {
        g_program_name = std::filesystem::path(argv[0]).filename().string();
    }
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

#ifndef VEXFS_EMBEDDED_MAIN
int main(int argc, char **argv) { return VexFsMain(argc, argv); }
#endif
