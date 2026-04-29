#pragma once

#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/block.hpp"
#include "duckdb/storage/block_manager.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace vex_disk {

struct SegmentBlockRef {
    uint32_t kind = 0;
    uint64_t size = 0;
    std::vector<BlockPointer> blocks;
};

struct DiskManifest {
    uint32_t magic = 0x58444d46; // XDMF
    uint32_t version = 1;
    std::vector<SegmentBlockRef> segments;
};

std::string SerializeManifest(const DiskManifest &manifest);
DiskManifest DeserializeManifest(const std::string &blob);

std::vector<BlockPointer> WriteBlobToBlocks(BlockManager &block_manager, QueryContext context, const std::string &blob);
std::string ReadBlobFromBlocks(BlockManager &block_manager, QueryContext context, const std::vector<BlockPointer> &blocks,
                               uint64_t total_size);

} // namespace vex_disk
} // namespace duckdb
