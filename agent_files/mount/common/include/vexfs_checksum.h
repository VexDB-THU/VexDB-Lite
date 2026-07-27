#ifndef VEXFS_CHECKSUM_H
#define VEXFS_CHECKSUM_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vexfs {

class Sha256 {
  public:
    Sha256();

    void Update(const void *data, size_t size);
    std::array<unsigned char, 32> Finish();

  private:
    void Transform(const unsigned char block[64]);

    std::array<uint32_t, 8> state_{};
    std::array<unsigned char, 64> buffer_{};
    uint64_t total_bytes_ = 0;
    size_t buffered_bytes_ = 0;
    bool finished_ = false;
};

std::string Sha256Hex(const void *data, size_t size);
std::string Hex(const std::array<unsigned char, 32> &digest);

struct ManifestChunkChecksum {
    uint64_t size = 0;
    std::string checksum;
};

// Must stay byte-for-byte compatible with _vexfs.compute_manifest_checksum in
// PostgreSQL. The root covers file length, chunk size, order, each chunk length,
// and each chunk's content SHA-256 without hashing the complete file again.
std::string ManifestChecksum(uint64_t file_size, uint64_t chunk_size,
                             const std::vector<ManifestChunkChecksum> &chunks);

}  // namespace vexfs

#endif  // VEXFS_CHECKSUM_H
