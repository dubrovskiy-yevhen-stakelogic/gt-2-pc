// GT4 paint variants stored as a delta against the base texture.
//
// File (gzip, or raw while inspecting): "GT4P", u32 version=1, u32 type, then:
//   0: u32 count, count x {u32 old RGBA, u32 new RGBA}   (palette remap)
//   1: u32 x, y, w, h, w*h*4 RGBA bytes                  (cropped rectangle)
//   2: u32 count, count x {u32 pixel index, u32 RGBA}    (sparse pixels)
// All values are little-endian and RGBA is packed r | g<<8 | b<<16 | a<<24.
#include "gt2formats/paint_delta.h"
#include "gt2vfs/inflate.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace gt2 {
namespace {

std::vector<uint8_t> ReadAll(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("paint delta: cannot open " + path);
    return {std::istreambuf_iterator<char>(file), {}};
}

uint32_t U32(const std::vector<uint8_t>& data, size_t& at) {
    if (at + 4 > data.size()) throw std::runtime_error("paint delta: truncated");
    const uint32_t value = uint32_t(data[at]) | uint32_t(data[at + 1]) << 8 | uint32_t(data[at + 2]) << 16 |
                           uint32_t(data[at + 3]) << 24;
    at += 4;
    return value;
}

} // namespace

PngImage ReadPaintVariant(const PngImage& original, const std::string& path) {
    constexpr const char kSuffix[] = ".delta.gz";
    if (path.size() < sizeof(kSuffix) - 1 || path.compare(path.size() - (sizeof(kSuffix) - 1), sizeof(kSuffix) - 1, kSuffix) != 0)
        return ReadPngFile(path);

    const std::vector<uint8_t> stored = ReadAll(path);
    const std::vector<uint8_t> data = IsGzip(stored) ? Gunzip(stored) : stored;
    if (data.size() < 12 || std::memcmp(data.data(), "GT4P", 4) != 0) throw std::runtime_error("paint delta: bad header");
    size_t at = 4;
    if (U32(data, at) != 1) throw std::runtime_error("paint delta: unsupported version");
    const uint32_t type = U32(data, at);
    if (original.width <= 0 || original.height <= 0 ||
        original.rgba.size() != size_t(original.width) * size_t(original.height) * 4)
        throw std::runtime_error("paint delta: invalid base image");

    PngImage result = original;
    if (type == 0) {
        const uint32_t count = U32(data, at);
        if (size_t(count) * 8 > data.size() - at) throw std::runtime_error("paint delta: truncated remap");
        std::unordered_map<uint32_t, uint32_t> remap;
        remap.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t old = U32(data, at), replacement = U32(data, at);
            remap[old] = replacement;
        }
        for (size_t i = 0; i + 4 <= result.rgba.size(); i += 4) {
            uint32_t value;
            std::memcpy(&value, &result.rgba[i], 4);
            const auto found = remap.find(value);
            if (found != remap.end()) std::memcpy(&result.rgba[i], &found->second, 4);
        }
    } else if (type == 1) {
        const uint32_t x = U32(data, at), y = U32(data, at), w = U32(data, at), h = U32(data, at);
        if (!w || !h || size_t(x) + w > size_t(result.width) || size_t(y) + h > size_t(result.height))
            throw std::runtime_error("paint delta: rectangle outside base image");
        const size_t rowBytes = size_t(w) * 4;
        if (h > (data.size() - at) / rowBytes) throw std::runtime_error("paint delta: truncated rectangle");
        for (uint32_t row = 0; row < h; ++row)
            std::memcpy(&result.rgba[(size_t(y + row) * size_t(result.width) + x) * 4], data.data() + at + row * rowBytes,
                        rowBytes);
    } else if (type == 2) {
        const uint32_t count = U32(data, at);
        if (size_t(count) * 8 > data.size() - at) throw std::runtime_error("paint delta: truncated pixels");
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t index = U32(data, at), value = U32(data, at);
            if (size_t(index) >= size_t(result.width) * size_t(result.height))
                throw std::runtime_error("paint delta: pixel outside base image");
            std::memcpy(&result.rgba[size_t(index) * 4], &value, 4);
        }
    } else {
        throw std::runtime_error("paint delta: unsupported type");
    }
    return result;
}

} // namespace gt2
