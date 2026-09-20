#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace gt2 {

// Raw DEFLATE (RFC 1951). Throws std::runtime_error on malformed input.
std::vector<uint8_t> Inflate(std::span<const uint8_t> deflate, size_t* consumed = nullptr);

bool IsGzip(std::span<const uint8_t> data);

// Single-member gzip (RFC 1952). CRC32 and ISIZE are verified.
std::vector<uint8_t> Gunzip(std::span<const uint8_t> gz);

uint32_t Crc32(std::span<const uint8_t> data, uint32_t crc = 0);

} // namespace gt2
