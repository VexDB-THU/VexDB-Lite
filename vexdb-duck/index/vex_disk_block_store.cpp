#include "vex/vex_disk_block_store.hpp"

#include "duckdb/common/enums/memory_tag.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace duckdb {
namespace vex_disk {

namespace {

template <class T>
static void AppendBytes(std::string &out, const T &value) {
    auto old_size = out.size();
    out.resize(old_size + sizeof(T));
    std::memcpy(out.data() + old_size, &value, sizeof(T));
}

template <class T>
static T ReadBytes(const std::string &blob, size_t &offset) {
    if (offset + sizeof(T) > blob.size()) {
        throw InvalidInputException("Disk manifest truncated");
    }
    T value;
    std::memcpy(&value, blob.data() + offset, sizeof(T));
    offset += sizeof(T);
    return value;
}

} // namespace

std::string SerializeManifest(const DiskManifest &manifest) {
    std::string out;
    AppendBytes(out, manifest.magic);
    AppendBytes(out, manifest.version);
    uint32_t segment_count = uint32_t(manifest.segments.size());
    AppendBytes(out, segment_count);
    for (auto &seg : manifest.segments) {
        AppendBytes(out, seg.kind);
        AppendBytes(out, seg.size);
        uint32_t block_count = uint32_t(seg.blocks.size());
        AppendBytes(out, block_count);
        for (auto &ptr : seg.blocks) {
            AppendBytes(out, ptr);
        }
    }
    return out;
}

DiskManifest DeserializeManifest(const std::string &blob) {
    size_t offset = 0;
    DiskManifest manifest;
    manifest.magic = ReadBytes<uint32_t>(blob, offset);
    manifest.version = ReadBytes<uint32_t>(blob, offset);
    if (manifest.magic != 0x58444d46 || manifest.version != 1) {
        throw InvalidInputException("Disk manifest magic/version mismatch");
    }
    auto segment_count = ReadBytes<uint32_t>(blob, offset);
    manifest.segments.reserve(segment_count);
    for (uint32_t i = 0; i < segment_count; i++) {
        SegmentBlockRef seg;
        seg.kind = ReadBytes<uint32_t>(blob, offset);
        seg.size = ReadBytes<uint64_t>(blob, offset);
        auto block_count = ReadBytes<uint32_t>(blob, offset);
        seg.blocks.reserve(block_count);
        for (uint32_t j = 0; j < block_count; j++) {
            seg.blocks.push_back(ReadBytes<BlockPointer>(blob, offset));
        }
        manifest.segments.push_back(std::move(seg));
    }
    return manifest;
}

std::vector<BlockPointer> WriteBlobToBlocks(BlockManager &block_manager, QueryContext context, const std::string &blob) {
    auto &buffer_manager = block_manager.GetBufferManager();
    const idx_t header_size = sizeof(uint32_t);
    const idx_t payload_size = block_manager.GetBlockSize() - header_size;
    std::vector<BlockPointer> blocks;

    idx_t offset = 0;
    while (offset < blob.size()) {
        idx_t chunk_size = std::min<idx_t>(payload_size, blob.size() - offset);
        auto buffer = buffer_manager.Allocate(MemoryTag::ART_INDEX, &block_manager, false);
        auto block_id = block_manager.GetFreeBlockIdForCheckpoint();
        auto ptr = buffer.Ptr();
        uint32_t chunk_size_u32 = uint32_t(chunk_size);
        std::memcpy(ptr, &chunk_size_u32, sizeof(uint32_t));
        std::memcpy(ptr + sizeof(uint32_t), blob.data() + offset, chunk_size);
        auto handle = buffer.GetBlockHandle();
        block_manager.ConvertToPersistent(context, block_id, std::move(handle), std::move(buffer));
        blocks.emplace_back(block_id, 0);
        offset += chunk_size;
    }
    return blocks;
}

std::string ReadBlobFromBlocks(BlockManager &block_manager, QueryContext context, const std::vector<BlockPointer> &blocks,
                               uint64_t total_size) {
    auto &buffer_manager = block_manager.GetBufferManager();
    std::string out;
    out.reserve(total_size);
    for (auto &ptr : blocks) {
        auto handle = block_manager.RegisterBlock(ptr.block_id);
        auto pin = buffer_manager.Pin(context, handle);
        auto buf = pin.Ptr();
        uint32_t chunk_size = 0;
        std::memcpy(&chunk_size, buf, sizeof(uint32_t));
        auto old_size = out.size();
        out.resize(old_size + chunk_size);
        std::memcpy(out.data() + old_size, buf + sizeof(uint32_t), chunk_size);
    }
    if (out.size() != total_size) {
        throw InvalidInputException("Disk segment size mismatch while reading blocks");
    }
    return out;
}

} // namespace vex_disk
} // namespace duckdb
