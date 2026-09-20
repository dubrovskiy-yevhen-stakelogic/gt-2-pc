#pragma once
#include <cstdint>
#include <span>
#include <string>

namespace gt2 {

// Minimal RGBA8 PNG writer (stored deflate blocks - inspection output, size is not a concern).
void WritePngRgba(const std::string& path, int width, int height, std::span<const uint8_t> rgba);

} // namespace gt2
