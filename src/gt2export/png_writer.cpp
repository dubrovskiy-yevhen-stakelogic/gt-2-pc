#include "gt2export/png_writer.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace gt2 {
namespace {

uint32_t Crc32Update(uint32_t crc, const uint8_t* data, size_t size) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    for (size_t i = 0; i < size; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

void PutU32Be(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<uint8_t>(x >> s));
}

void WriteChunk(std::FILE* f, const char type[4], const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> head;
    PutU32Be(head, static_cast<uint32_t>(payload.size()));
    std::fwrite(head.data(), 1, 4, f);
    std::fwrite(type, 1, 4, f);
    if (!payload.empty()) std::fwrite(payload.data(), 1, payload.size(), f);
    uint32_t crc = Crc32Update(0xFFFFFFFFu, reinterpret_cast<const uint8_t*>(type), 4);
    crc = ~Crc32Update(crc, payload.data(), payload.size());
    std::vector<uint8_t> tail;
    PutU32Be(tail, crc);
    std::fwrite(tail.data(), 1, 4, f);
}

} // namespace

void WritePngRgba(const std::string& path, int width, int height, std::span<const uint8_t> rgba) {
    if (rgba.size() != static_cast<size_t>(width) * height * 4) throw std::runtime_error("png: bad buffer size");

    std::vector<uint8_t> raw;
    raw.reserve((static_cast<size_t>(width) * 4 + 1) * height);
    for (int y = 0; y < height; y++) {
        raw.push_back(0); // filter: none
        const uint8_t* row = rgba.data() + static_cast<size_t>(y) * width * 4;
        raw.insert(raw.end(), row, row + static_cast<size_t>(width) * 4);
    }

    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t pos = 0; pos < raw.size();) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    PutU32Be(z, (b << 16) | a);

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("png: cannot create " + path);
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    PutU32Be(ihdr, static_cast<uint32_t>(width));
    PutU32Be(ihdr, static_cast<uint32_t>(height));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    WriteChunk(f, "IHDR", ihdr);
    WriteChunk(f, "IDAT", z);
    WriteChunk(f, "IEND", {});
    std::fclose(f);
}

} // namespace gt2
