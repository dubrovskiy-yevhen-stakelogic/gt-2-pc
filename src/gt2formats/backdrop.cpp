#include "gt2formats/backdrop.h"

#include <cstring>
#include <stdexcept>

namespace gt2 {
namespace {

struct Reader {
    std::span<const uint8_t> d;

    void Need(size_t off, size_t n) const {
        if (off > d.size() || n > d.size() - off)
            throw std::runtime_error("backdrop: read out of bounds at 0x" + std::to_string(off));
    }
    uint8_t U8(size_t o) const { Need(o, 1); return d[o]; }
    uint16_t U16(size_t o) const { Need(o, 2); return static_cast<uint16_t>(d[o] | (d[o + 1] << 8)); }
    int16_t S16(size_t o) const { return static_cast<int16_t>(U16(o)); }
    uint32_t U32(size_t o) const {
        Need(o, 4);
        return static_cast<uint32_t>(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (static_cast<uint32_t>(d[o + 3]) << 24));
    }
};

// Header: "BG\0\0"; u8 sky R,G,B + 0x28; u8 ground R,G,B + 0x28 (the two words are the colour words of the
// two POLY_F4 background fills the original draws first); u32 vertex count; u16 count[8] by libgpu type
// F3 F4 G3 G4 FT3 FT4 GT3 GT4; 0x20: vertices { s16 x, y, z, 0 }; then the polygon records list by list.
// Record: u32 w0 (v0 = bits 0-11, v1 = bits 12-23, flags = bits 24-31), u32 w1 (v3 = bits 0-11, v2 = bits
// 12-23), then TWO copies (double-buffered) of the ready-made libgpu packet: u32 tag (word count << 24),
// u32 rgb | code << 24, then per corner [u32 rgb (gouraud, corners 1..)] u32 xy (0) [u32 uv | clut/tpage << 16].
constexpr uint8_t kCodes[8] = {0x20, 0x28, 0x30, 0x38, 0x24, 0x2C, 0x34, 0x3C};
constexpr uint32_t kPacketWords[8] = {4, 5, 6, 8, 7, 9, 9, 12};

} // namespace

Backdrop ParseBackdrop(std::span<const uint8_t> data) {
    Reader r{data};
    if (data.size() < 0x20 || std::memcmp(data.data(), "BG\0\0", 4) != 0) throw std::runtime_error("backdrop: bad magic");
    Backdrop b;
    for (size_t i = 0; i < 3; i++) {
        b.skyColor[i] = r.U8(4 + i);
        b.groundColor[i] = r.U8(8 + i);
    }
    if (r.U8(7) != 0x28 || r.U8(11) != 0x28) throw std::runtime_error("backdrop: unexpected fill primitive codes");
    const uint32_t vertexCount = r.U32(0xC);
    if (vertexCount > 4096) throw std::runtime_error("backdrop: bad vertex count");
    std::array<uint16_t, 8> counts{};
    for (size_t i = 0; i < 8; i++) counts[i] = r.U16(0x10 + i * 2);

    size_t pos = 0x20;
    for (uint32_t i = 0; i < vertexCount; i++, pos += 8) b.vertices.push_back({r.S16(pos), r.S16(pos + 2), r.S16(pos + 4)});

    for (size_t li = 0; li < 8; li++) {
        const bool quad = (li & 1) != 0, gouraud = (li & 2) != 0, textured = (li & 4) != 0;
        const size_t corners = quad ? 4 : 3;
        for (uint16_t k = 0; k < counts[li]; k++) {
            BackdropPolygon p;
            const uint32_t w0 = r.U32(pos), w1 = r.U32(pos + 4);
            p.vertex = {uint16_t(w0 & 0xFFF), uint16_t((w0 >> 12) & 0xFFF), uint16_t((w1 >> 12) & 0xFFF), uint16_t(w1 & 0xFFF)};
            p.flags = uint8_t(w0 >> 24);
            const uint32_t tag = r.U32(pos + 8);
            if ((tag >> 24) != kPacketWords[li]) throw std::runtime_error("backdrop: packet length does not match its list");
            size_t w = pos + 12;
            p.primCode = r.U8(w + 3);
            if ((p.primCode & 0xFC) != kCodes[li]) throw std::runtime_error("backdrop: primitive code does not match its list");
            p.color[0] = {r.U8(w), r.U8(w + 1), r.U8(w + 2)};
            w += 4;
            for (size_t c = 0; c < corners; c++) {
                if (c > 0) {
                    if (gouraud) {
                        p.color[c] = {r.U8(w), r.U8(w + 1), r.U8(w + 2)};
                        w += 4;
                    } else {
                        p.color[c] = p.color[0];
                    }
                }
                w += 4; // xy placeholder
                if (textured) {
                    p.u[c] = r.U8(w);
                    p.v[c] = r.U8(w + 1);
                    if (c == 0) p.clut = r.U16(w + 2);
                    if (c == 1) p.tpage = r.U16(w + 2);
                    w += 4;
                }
            }
            for (size_t c = 0; c < corners; c++)
                if (p.vertex[c] >= vertexCount) throw std::runtime_error("backdrop: vertex index out of range");
            if (r.U32(w) >> 24 != kPacketWords[li]) throw std::runtime_error("backdrop: second packet copy missing");
            pos = w + 4 + kPacketWords[li] * 4;
            b.polygons.push_back(p);
        }
    }
    if (pos != data.size()) throw std::runtime_error("backdrop: trailing bytes after the last polygon");
    return b;
}

uint16_t BackdropIndexOf(std::span<const uint8_t, 14> rest) { return uint16_t(rest[0] | (rest[1] << 8)); }

std::string BackdropNameAt(const std::vector<std::string>& bsoPaths, uint16_t index) {
    if (index >= bsoPaths.size()) throw std::runtime_error("backdrop: index beyond the bgsobj directory");
    const std::string& p = bsoPaths[index];
    const size_t slash = p.rfind('/'), dot = p.find('.', slash == std::string::npos ? 0 : slash);
    return p.substr(slash == std::string::npos ? 0 : slash + 1, dot - (slash == std::string::npos ? 0 : slash + 1));
}

} // namespace gt2
