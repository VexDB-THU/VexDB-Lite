#include "vexfs_platform_registry.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

constexpr const char *kRegistryVersion = "VEXFS_MOUNT_REGISTRY_V1";

std::string PercentDecode(std::string value) {
    const auto hex = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        return -1;
    };
    std::string output;
    output.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            const int high = hex(value[index + 1]);
            const int low = hex(value[index + 2]);
            if (high >= 0 && low >= 0) {
                output.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        output.push_back(value[index]);
    }
    return output;
}

std::string NormalizePath(const std::string &path) {
    if (path.empty()) return {};
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    if (!error) return canonical.string();
    return std::filesystem::absolute(path).lexically_normal().string();
}

std::string NormalizeSource(std::string source) {
    if (source.rfind("file://", 0) == 0) source = PercentDecode(source.substr(7));
    if (!source.empty() && source.front() == '/') return NormalizePath(source);
    return source;
}

uint64_t StableHash(const std::string &value) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::filesystem::path RecordPath(const std::filesystem::path &directory,
                                 const std::string &target) {
    std::ostringstream name;
    name << "mount-" << std::hex << std::setw(16) << std::setfill('0')
         << StableHash(NormalizePath(target)) << ".record";
    return directory / name.str();
}

void ProtectDirectory(const std::filesystem::path &directory) {
    std::filesystem::create_directories(directory);
#if !defined(_WIN32)
    if (chmod(directory.c_str(), 0700) != 0)
        throw std::runtime_error("cannot protect VexFS mount registry");
#endif
}

bool ReadRecord(const std::filesystem::path &path, VexFSPlatformMountEntry *entry) {
    std::ifstream input(path, std::ios::binary);
    std::string version;
    if (!std::getline(input, version) || version != kRegistryVersion) return false;
    return static_cast<bool>(input >> std::quoted(entry->source)
        >> std::quoted(entry->target) >> std::quoted(entry->type)
        >> std::quoted(entry->database) >> std::quoted(entry->workspace));
}

}  // namespace

std::vector<VexFSPlatformMountEntry> VexFSPlatformAttachMountRegistry(
    std::vector<VexFSPlatformMountEntry> mounts,
    const std::filesystem::path &registry_directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(registry_directory, error)) return mounts;
    for (const auto &file : std::filesystem::directory_iterator(registry_directory, error)) {
        if (error) break;
        if (!file.is_regular_file() || file.path().extension() != ".record") continue;
        VexFSPlatformMountEntry saved;
        if (!ReadRecord(file.path(), &saved)) continue;
        for (auto &live : mounts) {
            if (NormalizePath(live.target) != NormalizePath(saved.target) ||
                NormalizeSource(live.source) != NormalizeSource(saved.source)) continue;
            live.database = NormalizePath(saved.database);
            live.workspace = saved.workspace;
            break;
        }
    }
    return mounts;
}

void VexFSPlatformRememberMount(const std::filesystem::path &registry_directory,
                                const VexFSPlatformMountEntry &mount) {
    if (mount.target.empty() || mount.source.empty() || mount.database.empty() ||
        mount.workspace.empty()) {
        throw std::runtime_error("incomplete VexFS mount identity");
    }
    ProtectDirectory(registry_directory);
    const std::filesystem::path destination = RecordPath(registry_directory, mount.target);
    const std::filesystem::path temporary = destination.string() + ".tmp-" +
        std::to_string(static_cast<long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot write VexFS mount registry");
        output << kRegistryVersion << '\n'
               << std::quoted(mount.source) << '\n'
               << std::quoted(NormalizePath(mount.target)) << '\n'
               << std::quoted(mount.type) << '\n'
               << std::quoted(NormalizePath(mount.database)) << '\n'
               << std::quoted(mount.workspace) << '\n';
        if (!output) throw std::runtime_error("cannot finish VexFS mount registry");
    }
#if !defined(_WIN32)
    if (chmod(temporary.c_str(), 0600) != 0) {
        std::filesystem::remove(temporary, error);
        throw std::runtime_error("cannot protect VexFS mount registry record");
    }
#endif
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(destination, error);
        error.clear();
        std::filesystem::rename(temporary, destination, error);
    }
    if (error) {
        std::filesystem::remove(temporary, error);
        throw std::runtime_error("cannot publish VexFS mount registry record");
    }
}

void VexFSPlatformForgetMount(const std::filesystem::path &registry_directory,
                              const std::string &target) {
    std::error_code ignored;
    std::filesystem::remove(RecordPath(registry_directory, target), ignored);
}
