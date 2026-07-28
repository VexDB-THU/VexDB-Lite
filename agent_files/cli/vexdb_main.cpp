#include "sqlite3.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fstream>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

#ifndef VEXDB_LITE_VERSION
#define VEXDB_LITE_VERSION "0.1.0-dev"
#endif
#ifndef VEXDB_SQLITE_GIT_HASH
#define VEXDB_SQLITE_GIT_HASH "unknown"
#endif

extern "C" int vexdb_sqlite_shell_main(int argc, char **argv);
int VexFsMain(int argc, char **argv);

namespace {

std::string ProgramName(const char *argument) {
    if (argument == nullptr || *argument == '\0') return "vexdb";
    const std::filesystem::path path(argument);
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension == ".exe" ? path.stem().string() : path.filename().string();
}

int RunWithArguments(int (*entry)(int, char **), const std::string &program_name,
                     int argc, char **argv, int first_argument) {
    std::vector<char *> forwarded;
    forwarded.reserve(static_cast<size_t>(argc - first_argument + 2));
    forwarded.push_back(const_cast<char *>(program_name.c_str()));
    for (int index = first_argument; index < argc; ++index) forwarded.push_back(argv[index]);
    forwarded.push_back(nullptr);
    return entry(static_cast<int>(forwarded.size() - 1), forwarded.data());
}

void Usage(std::ostream &output) {
    output <<
        "VexDB-Lite: SQLite, vector search and database-managed files\n"
        "\n"
        "Usage:\n"
        "  vexdb [SQLITE_OPTIONS] [DATABASE [SQL]]\n"
        "  vexdb sql [SQLITE_OPTIONS] [DATABASE [SQL]]\n"
        "  vexdb fs [--db DATABASE] COMMAND [ARGS]\n"
        "  vexdb backup SOURCE_DATABASE DESTINATION_DATABASE\n"
        "  vexfs [--db DATABASE] COMMAND [ARGS]\n"
        "\n"
        "Examples:\n"
        "  vexdb agent.db\n"
        "  vexdb agent.db \"SELECT vexdb_version();\"\n"
        "  vexdb fs --db agent.db ls /\n"
        "  vexdb fs --db agent.db mount ~/VexDB\n"
        "\n"
        "Run 'vexdb sql --help' for SQLite shell options or\n"
        "'vexdb fs --help' for file commands.\n";
}

void CreatePrivateBackupDestination(const std::filesystem::path &destination) {
#if defined(_WIN32)
    std::ofstream output(destination, std::ios::binary | std::ios::out);
    if (!output) throw std::runtime_error("cannot create backup destination");
#else
    int flags = O_CREAT | O_EXCL | O_WRONLY;
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = open(destination.c_str(), flags, 0600);
    if (descriptor < 0) {
        throw std::runtime_error("cannot create private backup destination: " +
                                 std::string(std::strerror(errno)));
    }
    if (close(descriptor) != 0) {
        const std::string message = std::strerror(errno);
        std::error_code ignored;
        std::filesystem::remove(destination, ignored);
        throw std::runtime_error("cannot close backup destination: " + message);
    }
#endif
}

int BackupDatabase(const std::filesystem::path &source,
                   const std::filesystem::path &destination) {
    if (source.empty() || destination.empty())
        throw std::runtime_error("source and destination database paths are required");
    if (std::filesystem::absolute(source).lexically_normal() ==
        std::filesystem::absolute(destination).lexically_normal())
        throw std::runtime_error("source and destination database must be different");
    if (!std::filesystem::is_regular_file(source))
        throw std::runtime_error("source database does not exist: " + source.string());
    if (std::filesystem::exists(destination))
        throw std::runtime_error("destination database already exists: " + destination.string());
    if (!destination.parent_path().empty())
        std::filesystem::create_directories(destination.parent_path());
    CreatePrivateBackupDestination(destination);

    sqlite3 *input = nullptr;
    sqlite3 *output = nullptr;
    sqlite3_backup *backup = nullptr;
    const std::string source_utf8 = source.u8string();
    const std::string destination_utf8 = destination.u8string();
    int source_flags = SQLITE_OPEN_READONLY;
    int destination_flags = SQLITE_OPEN_READWRITE;
    int rc = sqlite3_open_v2(source_utf8.c_str(), &input, source_flags, nullptr);
    if (rc == SQLITE_OK)
        rc = sqlite3_open_v2(destination_utf8.c_str(), &output, destination_flags, nullptr);
    if (rc == SQLITE_OK) {
        backup = sqlite3_backup_init(output, "main", input, "main");
        if (backup == nullptr) rc = sqlite3_errcode(output);
    }
    if (backup != nullptr) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do {
            rc = sqlite3_backup_step(backup, 256);
            if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
                if (std::chrono::steady_clock::now() >= deadline) break;
                sqlite3_sleep(25);
            }
        } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);
        const int finish_rc = sqlite3_backup_finish(backup);
        if (rc == SQLITE_DONE) rc = finish_rc;
    }
    const std::string error = output != nullptr ? sqlite3_errmsg(output) :
        (input != nullptr ? sqlite3_errmsg(input) : sqlite3_errstr(rc));
    if (output != nullptr) sqlite3_close(output);
    if (input != nullptr) sqlite3_close(input);
    if (rc != SQLITE_OK) {
        std::error_code ignored;
        std::filesystem::remove(destination, ignored);
        throw std::runtime_error("database backup failed: " + error);
    }
    std::cout << destination << '\n';
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    const std::string invoked_as = ProgramName(argc > 0 ? argv[0] : nullptr);
    if (invoked_as == "vexfs") return VexFsMain(argc, argv);

    if (argc >= 2) {
        const std::string command = argv[1];
        if (command == "fs") return RunWithArguments(VexFsMain, "vexdb fs", argc, argv, 2);
        if (command == "sql")
            return RunWithArguments(vexdb_sqlite_shell_main, "vexdb", argc, argv, 2);
        if (command == "backup") {
            if (argc != 4) {
                std::cerr << "vexdb: backup needs SOURCE_DATABASE DESTINATION_DATABASE\n";
                return 2;
            }
            try {
                return BackupDatabase(argv[2], argv[3]);
            } catch (const std::exception &error) {
                std::cerr << "vexdb: " << error.what() << '\n';
                return 1;
            }
        }
        if (command == "help" || command == "--help" || command == "-h") {
            Usage(std::cout);
            return 0;
        }
        if (command == "--version") {
            std::cout << "vexdb-lite " << VEXDB_LITE_VERSION
                      << " (" << VEXDB_SQLITE_GIT_HASH << "), SQLite "
                      << sqlite3_libversion() << '\n';
            return 0;
        }
    }
    return vexdb_sqlite_shell_main(argc, argv);
}
