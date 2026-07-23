#ifndef VEXFS_CHECKSUM_H
#define VEXFS_CHECKSUM_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

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

}  // namespace vexfs

#endif  // VEXFS_CHECKSUM_H
