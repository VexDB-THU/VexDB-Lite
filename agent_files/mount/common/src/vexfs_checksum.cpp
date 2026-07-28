#include "vexfs_checksum.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace vexfs {
namespace {

constexpr std::array<uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

uint32_t RotateRight(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32 - bits));
}

uint32_t LoadBigEndian(const unsigned char *input) {
    return (static_cast<uint32_t>(input[0]) << 24) |
           (static_cast<uint32_t>(input[1]) << 16) |
           (static_cast<uint32_t>(input[2]) << 8) |
           static_cast<uint32_t>(input[3]);
}

void StoreBigEndian(uint32_t value, unsigned char *output) {
    output[0] = static_cast<unsigned char>(value >> 24);
    output[1] = static_cast<unsigned char>(value >> 16);
    output[2] = static_cast<unsigned char>(value >> 8);
    output[3] = static_cast<unsigned char>(value);
}

}  // namespace

Sha256::Sha256()
    : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
             0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u} {}

void Sha256::Transform(const unsigned char block[64]) {
    std::array<uint32_t, 64> words{};
    for (size_t index = 0; index < 16; ++index) {
        words[index] = LoadBigEndian(block + index * 4);
    }
    for (size_t index = 16; index < words.size(); ++index) {
        const uint32_t s0 = RotateRight(words[index - 15], 7) ^
                            RotateRight(words[index - 15], 18) ^
                            (words[index - 15] >> 3);
        const uint32_t s1 = RotateRight(words[index - 2], 17) ^
                            RotateRight(words[index - 2], 19) ^
                            (words[index - 2] >> 10);
        words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];
    uint32_t f = state_[5];
    uint32_t g = state_[6];
    uint32_t h = state_[7];
    for (size_t index = 0; index < words.size(); ++index) {
        const uint32_t sigma1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
        const uint32_t choose = (e & f) ^ (~e & g);
        const uint32_t first = h + sigma1 + choose + kRoundConstants[index] + words[index];
        const uint32_t sigma0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t second = sigma0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::Update(const void *data, size_t size) {
    if (finished_) throw std::logic_error("SHA-256 is already finished");
    if (size != 0 && data == nullptr) throw std::invalid_argument("SHA-256 input is NULL");
    const auto *input = static_cast<const unsigned char *>(data);
    if (size > UINT64_MAX / 8 - total_bytes_) {
        throw std::overflow_error("SHA-256 input is too large");
    }
    total_bytes_ += static_cast<uint64_t>(size);

    while (size != 0) {
        const size_t copied = std::min(size, buffer_.size() - buffered_bytes_);
        std::memcpy(buffer_.data() + buffered_bytes_, input, copied);
        buffered_bytes_ += copied;
        input += copied;
        size -= copied;
        if (buffered_bytes_ == buffer_.size()) {
            Transform(buffer_.data());
            buffered_bytes_ = 0;
        }
    }
}

std::array<unsigned char, 32> Sha256::Finish() {
    if (finished_) throw std::logic_error("SHA-256 is already finished");
    finished_ = true;
    const uint64_t bit_length = total_bytes_ * 8;
    buffer_[buffered_bytes_++] = 0x80;
    if (buffered_bytes_ > 56) {
        std::fill(buffer_.begin() + buffered_bytes_, buffer_.end(), 0);
        Transform(buffer_.data());
        buffered_bytes_ = 0;
    }
    std::fill(buffer_.begin() + buffered_bytes_, buffer_.begin() + 56, 0);
    for (size_t index = 0; index < 8; ++index) {
        buffer_[63 - index] = static_cast<unsigned char>(bit_length >> (index * 8));
    }
    Transform(buffer_.data());

    std::array<unsigned char, 32> digest{};
    for (size_t index = 0; index < state_.size(); ++index) {
        StoreBigEndian(state_[index], digest.data() + index * 4);
    }
    return digest;
}

std::string Hex(const std::array<unsigned char, 32> &digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result(64, '0');
    for (size_t index = 0; index < digest.size(); ++index) {
        result[index * 2] = kHex[digest[index] >> 4];
        result[index * 2 + 1] = kHex[digest[index] & 0x0f];
    }
    return result;
}

std::string Sha256Hex(const void *data, size_t size) {
    Sha256 hash;
    hash.Update(data, size);
    return Hex(hash.Finish());
}

std::string ManifestChecksum(
    uint64_t file_size, uint64_t chunk_size,
    const std::vector<ManifestChunkChecksum> &chunks) {
    if (chunk_size == 0) throw std::invalid_argument("manifest chunk size is zero");
    const uint64_t expected_chunks = file_size == 0
        ? 0 : 1 + (file_size - 1) / chunk_size;
    if (expected_chunks != chunks.size()) {
        throw std::invalid_argument("manifest chunk count does not match file size");
    }
    uint64_t total = 0;
    Sha256 hash;
    const std::string header = "vexfs-manifest-v1:" + std::to_string(chunk_size) +
        ":" + std::to_string(file_size) + ":" + std::to_string(chunks.size()) + "\n";
    hash.Update(header.data(), header.size());
    for (size_t index = 0; index < chunks.size(); ++index) {
        const ManifestChunkChecksum &chunk = chunks[index];
        const uint64_t expected_size = std::min(chunk_size, file_size - total);
        const bool valid_checksum = chunk.checksum.size() == 64 &&
            std::all_of(chunk.checksum.begin(), chunk.checksum.end(), [](unsigned char value) {
                return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
            });
        if (chunk.size != expected_size || !valid_checksum ||
            chunk.size > std::numeric_limits<uint64_t>::max() - total) {
            throw std::invalid_argument("manifest chunk metadata is invalid");
        }
        if (index != 0) hash.Update("\n", 1);
        const std::string entry = std::to_string(index) + ":" +
            std::to_string(chunk.size) + ":" + chunk.checksum;
        hash.Update(entry.data(), entry.size());
        total += chunk.size;
    }
    if (total != file_size) {
        throw std::invalid_argument("manifest chunks do not cover the file");
    }
    return Hex(hash.Finish());
}

}  // namespace vexfs
