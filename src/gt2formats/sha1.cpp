#include "gt2formats/sha1.h"

#include <array>
#include <cstdio>

namespace gt2 {
namespace {

uint32_t Rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

void Block(std::array<uint32_t, 5>& h, const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        const uint32_t t = Rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = Rol(b, 30);
        b = a;
        a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

} // namespace

std::string Sha1Hex(std::span<const uint8_t> bytes) {
    std::array<uint32_t, 5> h{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    size_t at = 0;
    for (; at + 64 <= bytes.size(); at += 64) Block(h, bytes.data() + at);
    uint8_t tail[128] = {};
    const size_t rest = bytes.size() - at;
    for (size_t i = 0; i < rest; i++) tail[i] = bytes[at + i];
    tail[rest] = 0x80;
    const size_t tailSize = rest + 1 + 8 <= 64 ? 64 : 128;
    const uint64_t bits = uint64_t(bytes.size()) * 8u;
    for (int i = 0; i < 8; i++) tail[tailSize - 1 - size_t(i)] = uint8_t(bits >> (8 * i));
    Block(h, tail);
    if (tailSize == 128) Block(h, tail + 64);
    char text[41];
    for (int i = 0; i < 5; i++) std::snprintf(text + i * 8, 9, "%08x", h[size_t(i)]);
    return std::string(text, 40);
}

} // namespace gt2
