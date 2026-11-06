#pragma once

#include "vexfs_platform.h"

#include <filesystem>
#include <vector>

// The kernel mount table does not carry VexFS database/workspace options.  Keep
// a small per-mount identity record and only trust it while target+source still
// match a real kernel mount entry.
std::vector<VexFSPlatformMountEntry> VexFSPlatformAttachMountRegistry(
    std::vector<VexFSPlatformMountEntry> mounts,
    const std::filesystem::path &registry_directory);
void VexFSPlatformRememberMount(const std::filesystem::path &registry_directory,
                                const VexFSPlatformMountEntry &mount);
void VexFSPlatformForgetMount(const std::filesystem::path &registry_directory,
                              const std::string &target);
