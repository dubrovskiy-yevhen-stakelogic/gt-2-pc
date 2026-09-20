#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gt2 {

// zlib stream (RFC 1950) of `data`, DEFLATE (RFC 1951) with fixed Huffman codes and LZ77 matches (32 KiB window,
// hash chains). Own code.
std::vector<uint8_t> ZlibCompress(std::span<const uint8_t> data);

// RGBA8 PNG with per-row adaptive filtering and a compressed IDAT (for exports that write many / large images).
void WritePngRgbaCompressed(const std::string& path, int width, int height, std::span<const uint8_t> rgba);

} // namespace gt2
