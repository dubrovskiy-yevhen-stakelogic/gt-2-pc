#pragma once
// Minimal PNG decoder for mod textures (glTF images): non-interlaced images of every PNG colour type
// (grey, RGB, palette, grey + alpha, RGBA) at 1..16 bits per sample, decoded to RGBA8. Uses the raw DEFLATE
// decoder of gt2vfs/inflate.h. Adam7-interlaced files are rejected. Own code.
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gt2 {

struct PngImage {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba; // width * height * 4, rows top to bottom
};

// Throws std::runtime_error on malformed or unsupported input.
PngImage DecodePng(std::span<const uint8_t> file);
PngImage ReadPngFile(const std::string& path);

} // namespace gt2
