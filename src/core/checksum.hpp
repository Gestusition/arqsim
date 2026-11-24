#pragma once
// Integrity primitives implemented from scratch (no external libraries):
//  - CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) for per-packet error detection
//  - SHA-256 (FIPS 180-4) for end-to-end file integrity verification
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace arq {

std::uint32_t crc32(const std::uint8_t* data, std::size_t len, std::uint32_t seed = 0);

class Sha256 {
public:
    Sha256();
    void update(const std::uint8_t* data, std::size_t len);
    std::array<std::uint8_t, 32> digest();
    static std::string hex(const std::vector<std::uint8_t>& data);

private:
    void transform(const std::uint8_t* block);
    std::uint32_t state_[8];
    std::uint8_t buffer_[64];
    std::size_t bufferLen_ = 0;
    std::uint64_t totalLen_ = 0;
};

std::string toHex(const std::uint8_t* data, std::size_t len);

}  // namespace arq
