#include "gt2formats/car_model.h"

#include <stdexcept>
#include <string>

namespace gt2 {
namespace {

constexpr size_t kLodTableOffset = 0x868;
constexpr size_t kLodHeaderSize = 0x50;
constexpr size_t kPolygonSize = 16;
constexpr size_t kUvPolygonSize = 28;

struct Reader {
    std::span<const uint8_t> d;

    void Need(size_t off, size_t n) const {
        if (off > d.size() || n > d.size() - off)
            throw std::runtime_error("car model: read out of bounds at 0x" + std::to_string(off));
    }
    uint8_t U8(size_t o) const { Need(o, 1); return d[o]; }
    uint16_t U16(size_t o) const { Need(o, 2); return static_cast<uint16_t>(d[o] | (d[o + 1] << 8)); }
    int16_t S16(size_t o) const { return static_cast<int16_t>(U16(o)); }
    uint32_t U32(size_t o) const {
        Need(o, 4);
        return static_cast<uint32_t>(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (static_cast<uint32_t>(d[o + 3]) << 24));
    }
};

int16_t SignExtend10(uint32_t v) {
    v &= 0x3FF;
    return static_cast<int16_t>((v & 0x200) ? static_cast<int32_t>(v) - 0x400 : static_cast<int32_t>(v));
}

CarPolygon ReadPolygon(const Reader& r, size_t o, bool textured) {
    CarPolygon p;
    for (size_t i = 0; i < 4; i++) p.vertex[i] = r.U8(o + i);
    uint16_t w1 = r.U16(o + 4);
    p.renderOrder = static_cast<uint8_t>(w1 & 0x1F);
    p.normal[0] = static_cast<uint16_t>((w1 >> 5) & 0x1FF);
    p.renderFlags = r.U16(o + 6);
    uint32_t w2 = r.U32(o + 8);
    p.normal[1] = static_cast<uint16_t>((w2 >> 1) & 0x1FF);
    p.normal[2] = static_cast<uint16_t>((w2 >> 10) & 0x1FF);
    p.normal[3] = static_cast<uint16_t>((w2 >> 19) & 0x1FF);
    p.r = r.U8(o + 12);
    p.g = r.U8(o + 13);
    p.b = r.U8(o + 14);
    p.primCode = r.U8(o + 15);
    if (textured) {
        p.u[0] = r.U8(o + 16); p.v[0] = r.U8(o + 17);
        p.rawPalette = r.U16(o + 18);
        p.palette = static_cast<uint8_t>(((p.rawPalette >> 4) + (p.rawPalette & 0x3F)) & 0x0F);
        p.u[1] = r.U8(o + 20); p.v[1] = r.U8(o + 21);
        p.u[2] = r.U8(o + 24); p.v[2] = r.U8(o + 25);
        p.u[3] = r.U8(o + 26); p.v[3] = r.U8(o + 27);
    }
    return p;
}

} // namespace

CarModel ParseCarModel(std::span<const uint8_t> data) {
    Reader r{data};
    if (data.size() < kLodTableOffset + 4 || data[0] != 'G' || data[1] != 'T' || r.U16(2) != 2)
        throw std::runtime_error("car model: bad magic/version");

    CarModel m;
    for (size_t i = 0; i < 3; i++) m.wheelDishColor[i] = r.U8(0x08 + i);
    m.wheelRadiusFront = r.S16(0x18);
    m.wheelWidthFront = r.S16(0x1A);
    m.wheelRadiusRear = r.S16(0x1C);
    m.wheelWidthRear = r.S16(0x1E);
    for (size_t i = 0; i < 4; i++) {
        size_t o = 0x20 + i * 8;
        m.wheels[i] = {r.S16(o), r.S16(o + 2), r.S16(o + 4), r.S16(o + 6)};
    }

    uint32_t lodCount = r.U32(kLodTableOffset);
    if (lodCount == 0 || lodCount > 3) throw std::runtime_error("car model: bad LOD count");

    // LOD table entry (8 bytes, verified on a-a7r.cdo): u16 0, u16 max distance, u32 file
    // offset of the LOD block (0 in some entries - runtime-filled). Blocks are stored back to
    // back starting right after the table, so they are located by walking their extents.
    //
    // LOD block header (0x50 bytes): u16 counts[10]; u32 offsets[10] relative to the block:
    //   [0] vertices [1] 0 [2] normals [3] tris [4] quads [5] unkA [6] unkB [7] uv tris
    //   [8] uv quads [9] unkC;  then s16 bbox[8], s16 scale, s16 unknown.
    // NOTE: this differs from the published 010 template, which names [2] as the tris offset.
    size_t lodPos = kLodTableOffset + 4 + 3 * 8;
    for (uint32_t li = 0; li < lodCount; li++) {
        CarLod lod;
        size_t entry = kLodTableOffset + 4 + li * 8;
        lod.maxDistance = r.U16(entry + 2);
        uint32_t tablePtr = r.U32(entry + 4);
        if (tablePtr != 0 && tablePtr != lodPos)
            throw std::runtime_error("car model: LOD table pointer disagrees with block walk");

        r.Need(lodPos, kLodHeaderSize);
        std::array<uint32_t, 10> off{};
        for (size_t i = 0; i < 10; i++) lod.counts[i] = r.U16(lodPos + i * 2);
        for (size_t i = 0; i < 10; i++) off[i] = r.U32(lodPos + 0x14 + i * 4);
        for (size_t i = 0; i < 8; i++) lod.bbox[i] = r.S16(lodPos + 0x3C + i * 2);
        lod.scale = r.S16(lodPos + 0x4C);
        lod.scaleUnk = r.S16(lodPos + 0x4E);

        const uint16_t vertCount = lod.counts[0], normalCount = lod.counts[1];
        const uint16_t triCount = lod.counts[2], quadCount = lod.counts[3];
        const uint16_t uvTriCount = lod.counts[6], uvQuadCount = lod.counts[7];
        if (lod.counts[4] || lod.counts[5] || lod.counts[8] || lod.counts[9])
            throw std::runtime_error("car model: LOD uses undocumented blocks (counts 4/5/8/9)");
        // Blocks are packed in this order; the offset of an empty block may be 0 or stale.
        const struct { size_t offsetIndex; size_t count; size_t stride; } blocks[] = {
            {0, vertCount, 8}, {2, normalCount, 4}, {3, triCount, kPolygonSize}, {4, quadCount, kPolygonSize},
            {7, uvTriCount, kUvPolygonSize}, {8, uvQuadCount, kUvPolygonSize}};
        size_t expected = kLodHeaderSize;
        for (const auto& blk : blocks) {
            if (blk.count != 0 && off[blk.offsetIndex] != expected)
                throw std::runtime_error("car model: LOD offsets are not the expected packed layout");
            expected += blk.count * blk.stride;
        }

        size_t pos = lodPos + kLodHeaderSize;
        for (uint16_t i = 0; i < vertCount; i++, pos += 8)
            lod.vertices.push_back({r.S16(pos), r.S16(pos + 2), r.S16(pos + 4), r.S16(pos + 6)});
        for (uint16_t i = 0; i < normalCount; i++, pos += 4) {
            uint32_t n = r.U32(pos);
            lod.normals.push_back({SignExtend10(n >> 22), SignExtend10(n >> 12), SignExtend10(n >> 2)});
        }
        for (uint16_t i = 0; i < triCount + quadCount; i++, pos += kPolygonSize)
            lod.polygons.push_back(ReadPolygon(r, pos, false));
        for (uint16_t i = 0; i < uvTriCount + uvQuadCount; i++, pos += kUvPolygonSize)
            lod.polygons.push_back(ReadPolygon(r, pos, true));
        r.Need(pos, 0);

        for (const auto& p : lod.polygons)
            for (size_t i = 0; i < (p.IsQuad() ? 4u : 3u); i++)
                if (p.vertex[i] >= vertCount) throw std::runtime_error("car model: vertex index out of range");

        m.lods.push_back(std::move(lod));
        lodPos = pos;
    }

    // Shadow block right after the last LOD (see CarShadow). Files without one (menu objects) leave it empty.
    if (lodPos + 28 <= data.size()) {
        const uint16_t vertexCount = r.U16(lodPos), triCount = r.U16(lodPos + 2), quadCount = r.U16(lodPos + 4);
        const size_t end = lodPos + 28 + size_t(vertexCount) * 4 + (size_t(triCount) + quadCount) * 4;
        if (r.U16(lodPos + 6) == 0 && vertexCount <= 64 && end == data.size()) {
            CarShadow s;
            size_t pos = lodPos + 28;
            for (uint16_t i = 0; i < vertexCount; i++, pos += 4) s.vertices.push_back({r.S16(pos), r.S16(pos + 2)});
            for (uint16_t i = 0; i < triCount + quadCount; i++, pos += 4) {
                const uint32_t w = r.U32(pos);
                CarShadowPolygon p;
                for (size_t c = 0; c < 4; c++) p.vertex[c] = uint8_t((w >> (6 * c)) & 0x3F);
                p.quad = i >= triCount;
                p.fullyShaded = (w & 0x80000000u) != 0;
                for (size_t c = 0; c < (p.quad ? 4u : 3u); c++)
                    if (p.vertex[c] >= vertexCount) throw std::runtime_error("car model: shadow vertex index out of range");
                s.polygons.push_back(p);
            }
            m.shadow = std::move(s);
        }
    }
    return m;
}

} // namespace gt2
