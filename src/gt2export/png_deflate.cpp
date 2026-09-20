#include "gt2export/png_deflate.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace gt2 {
namespace {

struct BitWriter {
    std::vector<uint8_t>& out;
    uint32_t acc = 0;
    int bits = 0;
    void Put(uint32_t value, int count) { // LSB first
        acc |= value << bits;
        bits += count;
        while (bits >= 8) {
            out.push_back(uint8_t(acc));
            acc >>= 8;
            bits -= 8;
        }
    }
    void PutReversed(uint32_t code, int length) { // Huffman codes go MSB first
        uint32_t r = 0;
        for (int i = 0; i < length; i++) r |= ((code >> i) & 1u) << (length - 1 - i);
        Put(r, length);
    }
    void Flush() {
        if (bits > 0) out.push_back(uint8_t(acc));
        acc = 0;
        bits = 0;
    }
};

void PutLiteral(BitWriter& w, uint32_t sym) { // fixed literal / length code
    if (sym < 144) w.PutReversed(0x30 + sym, 8);
    else if (sym < 256) w.PutReversed(0x190 + (sym - 144), 9);
    else if (sym < 280) w.PutReversed(sym - 256, 7);
    else w.PutReversed(0xC0 + (sym - 280), 8);
}

constexpr uint16_t kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void PutMatch(BitWriter& w, uint32_t length, uint32_t distance) {
    size_t l = 28;
    while (kLengthBase[l] > length) l--;
    PutLiteral(w, uint32_t(257 + l));
    if (kLengthExtra[l]) w.Put(length - kLengthBase[l], kLengthExtra[l]);
    size_t d = 29;
    while (kDistBase[d] > distance) d--;
    w.PutReversed(uint32_t(d), 5);
    if (kDistExtra[d]) w.Put(distance - kDistBase[d], kDistExtra[d]);
}

uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
    static std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

} // namespace

std::vector<uint8_t> ZlibCompress(std::span<const uint8_t> data) {
    std::vector<uint8_t> out = {0x78, 0x01};
    BitWriter w{out};
    w.Put(1, 1); // BFINAL
    w.Put(1, 2); // fixed Huffman
    constexpr size_t kWindow = 32768, kHashSize = 1 << 15, kMaxChain = 64, kMinMatch = 3, kMaxMatch = 258;
    std::vector<int32_t> head(kHashSize, -1), prev(data.size(), -1);
    auto hash = [&](size_t i) { return uint32_t(((uint32_t(data[i]) << 10) ^ (uint32_t(data[i + 1]) << 5) ^ data[i + 2]) & uint32_t(kHashSize - 1)); };
    size_t i = 0;
    const size_t n = data.size();
    auto insert = [&](size_t at) {
        if (at + 2 >= n) return;
        const uint32_t h = hash(at);
        prev[at] = head[h];
        head[h] = int32_t(at);
    };
    while (i < n) {
        size_t bestLen = 0, bestDist = 0;
        if (i + kMinMatch <= n) {
            int32_t cand = head[hash(i)];
            size_t chain = 0;
            while (cand >= 0 && i - size_t(cand) <= kWindow && chain++ < kMaxChain) {
                const size_t c = size_t(cand);
                size_t len = 0;
                const size_t maxLen = std::min(kMaxMatch, n - i);
                while (len < maxLen && data[c + len] == data[i + len]) len++;
                if (len > bestLen) {
                    bestLen = len;
                    bestDist = i - c;
                    if (len == maxLen) break;
                }
                cand = prev[c];
            }
        }
        if (bestLen >= kMinMatch) {
            PutMatch(w, uint32_t(bestLen), uint32_t(bestDist));
            for (size_t k = 0; k < bestLen; k++) insert(i + k);
            i += bestLen;
        } else {
            PutLiteral(w, data[i]);
            insert(i);
            i++;
        }
    }
    PutLiteral(w, 256);
    w.Flush();
    uint32_t a = 1, b = 0;
    for (uint8_t c : data) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = (b << 16) | a;
    out.push_back(uint8_t(adler >> 24));
    out.push_back(uint8_t(adler >> 16));
    out.push_back(uint8_t(adler >> 8));
    out.push_back(uint8_t(adler));
    return out;
}

void WritePngRgbaCompressed(const std::string& path, int width, int height, std::span<const uint8_t> rgba) {
    if (width <= 0 || height <= 0 || rgba.size() < size_t(width) * size_t(height) * 4) throw std::runtime_error("png: bad image for " + path);
    const size_t stride = size_t(width) * 4;
    std::vector<uint8_t> raw;
    raw.reserve((stride + 1) * size_t(height));
    std::vector<uint8_t> line(stride), best(stride);
    for (int y = 0; y < height; y++) {
        const uint8_t* cur = &rgba[size_t(y) * stride];
        const uint8_t* up = y > 0 ? &rgba[size_t(y - 1) * stride] : nullptr;
        uint64_t bestScore = UINT64_MAX;
        uint8_t bestFilter = 0;
        for (uint8_t f = 0; f < 5; f++) {
            uint64_t score = 0;
            for (size_t x = 0; x < stride; x++) {
                const int a = x >= 4 ? cur[x - 4] : 0, b = up ? up[x] : 0, c = (x >= 4 && up) ? up[x - 4] : 0;
                int pred = 0;
                if (f == 1) pred = a;
                else if (f == 2) pred = b;
                else if (f == 3) pred = (a + b) / 2;
                else if (f == 4) {
                    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                    pred = (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
                }
                line[x] = uint8_t(cur[x] - pred);
                score += uint64_t(std::abs(int(int8_t(line[x]))));
            }
            if (score < bestScore) {
                bestScore = score;
                bestFilter = f;
                best = line;
            }
        }
        raw.push_back(bestFilter);
        raw.insert(raw.end(), best.begin(), best.end());
    }
    const std::vector<uint8_t> idat = ZlibCompress(raw);
    std::vector<uint8_t> file = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    auto chunk = [&](const char* type, const std::vector<uint8_t>& payload) {
        const uint32_t n = uint32_t(payload.size());
        file.insert(file.end(), {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)});
        const size_t start = file.size();
        file.insert(file.end(), type, type + 4);
        file.insert(file.end(), payload.begin(), payload.end());
        const uint32_t crc = Crc32(&file[start], file.size() - start);
        file.insert(file.end(), {uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)});
    };
    const uint32_t w = uint32_t(width), h = uint32_t(height);
    chunk("IHDR", {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w), uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 6, 0, 0, 0});
    chunk("IDAT", idat);
    chunk("IEND", {});
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot create " + path);
    const size_t written = std::fwrite(file.data(), 1, file.size(), f);
    std::fclose(f);
    if (written != file.size()) throw std::runtime_error("cannot write " + path);
}

} // namespace gt2
