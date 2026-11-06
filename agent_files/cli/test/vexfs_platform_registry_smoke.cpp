#include "vexfs_platform_registry.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

int Fail(const std::string &message) {
    std::cerr << "VEXFS MOUNT REGISTRY SMOKE: FAIL: " << message << '\n';
    return 1;
}

}  // namespace

int main() {
    char directory_template[] = "/tmp/vexfs-mount-registry-XXXXXX";
    const char *created = mkdtemp(directory_template);
    if (created == nullptr) return Fail("mkdtemp");
    const std::filesystem::path root(created);
    const std::filesystem::path registry = root / "registry";
    const std::filesystem::path source = root / "source dir";
    const std::filesystem::path target = root / "mount point";
    const std::filesystem::path database = root / "workspace.sqlite3";
    std::filesystem::create_directories(source);
    std::filesystem::create_directories(target);

    try {
        VexFSPlatformRememberMount(registry,
            {source.string(), target.string(), "vexfs", "sqlite", database.string(), "agent"});
        std::string encoded_source = source.string();
        const size_t space = encoded_source.find(' ');
        if (space != std::string::npos) encoded_source.replace(space, 1, "%20");
        auto attached = VexFSPlatformAttachMountRegistry(
            {{"file://" + encoded_source + "/", target.string(), "vexfs"}}, registry);
        if (attached.size() != 1 ||
            attached[0].backend != "sqlite" ||
            attached[0].database != std::filesystem::weakly_canonical(database).string() ||
            attached[0].workspace != "agent") {
            const std::string details = attached.empty() ? "empty" :
                "database=" + attached[0].database + ", workspace=" + attached[0].workspace;
            std::filesystem::remove_all(root);
            return Fail("matching live mount identity: " + details);
        }
        attached = VexFSPlatformAttachMountRegistry(
            {{"file:///different/source", target.string(), "vexfs"}}, registry);
        if (!attached[0].database.empty() || !attached[0].workspace.empty()) {
            std::filesystem::remove_all(root);
            return Fail("source mismatch must not be trusted");
        }
        VexFSPlatformForgetMount(registry, target.string());
        attached = VexFSPlatformAttachMountRegistry(
            {{source.string(), target.string(), "vexfs"}}, registry);
        if (!attached[0].database.empty()) {
            std::filesystem::remove_all(root);
            return Fail("forgotten mount identity");
        }
        const std::string dsn = "postgresql://postgres@127.0.0.1:5432/test";
        VexFSPlatformRememberMount(registry,
            {source.string(), target.string(), "vexfs", "postgresql", dsn, "remote"});
        attached = VexFSPlatformAttachMountRegistry(
            {{source.string(), target.string(), "vexfs"}}, registry);
        if (attached[0].backend != "postgresql" || attached[0].database != dsn ||
            attached[0].workspace != "remote") {
            std::filesystem::remove_all(root);
            return Fail("PostgreSQL connection identity must not be normalized as a path");
        }
    } catch (const std::exception &error) {
        std::filesystem::remove_all(root);
        return Fail(error.what());
    }
    std::filesystem::remove_all(root);
    std::cout << "VEXFS MOUNT REGISTRY SMOKE: PASS\n";
    return 0;
}
