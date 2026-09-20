#include "gt2formats/track.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace gt2 {
namespace {

constexpr int64_t kCellSize = 64 * 65536; // 64 m in 16.16
constexpr size_t kChunkShapeOffset = 0xA4;

struct Reader {
    std::span<const uint8_t> d;

    void Need(size_t off, size_t n) const {
        if (off > d.size() || n > d.size() - off)
            throw std::runtime_error("track: read out of bounds at 0x" + std::to_string(off));
    }
    uint8_t U8(size_t o) const { Need(o, 1); return d[o]; }
    uint16_t U16(size_t o) const { Need(o, 2); return static_cast<uint16_t>(d[o] | (d[o + 1] << 8)); }
    int16_t S16(size_t o) const { return static_cast<int16_t>(U16(o)); }
    uint32_t U32(size_t o) const {
        Need(o, 4);
        return static_cast<uint32_t>(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (static_cast<uint32_t>(d[o + 3]) << 24));
    }
    int32_t S32(size_t o) const { return static_cast<int32_t>(U32(o)); }
};

// The glow records of a chunk shape or scenery model at `s`: u16 count at s + 0x42, 20-byte records at *(s + 0x28).
std::vector<TrackGlow> ReadGlows(const Reader& r, size_t s) {
    std::vector<TrackGlow> glows;
    const uint16_t count = r.U16(s + 0x42);
    for (size_t k = 0, o = count ? r.U32(s + 0x28) : 0; k < count; k++, o += 20) {
        TrackGlow g;
        g.position = {r.S16(o), r.S16(o + 2), r.S16(o + 4)};
        g.size = r.S16(o + 6);
        for (size_t b = 0; b < 8; b++) g.unknown[b] = r.U8(o + 8 + b);
        g.colour = r.U32(o + 16);
        glows.push_back(g);
    }
    return glows;
}

// Shape header: u32 vertexOffset, u32 listOffset[8], u32 off24, u32 off28, s32 vertexCount,
// s16 listCount[8], s32 count24. Lists by libgpu type: F3 F4 G3 G4 FT3 FT4 GT3 GT4.
// Polygon: 12 bytes + 4 bytes per extra gouraud corner colour.
TrackShape ReadShape(const Reader& r, size_t s, size_t uvCount) {
    static constexpr size_t kStride[8] = {12, 12, 20, 24, 12, 12, 20, 24};
    static constexpr uint8_t kExpectedCode[8] = {0x20, 0x28, 0x30, 0x38, 0x24, 0x2C, 0x34, 0x3C};

    TrackShape shape;
    shape.glows = ReadGlows(r, s);
    int32_t vertexCount = r.S32(s + 0x2C);
    if (vertexCount < 0 || vertexCount > 512) throw std::runtime_error("track: bad shape vertex count");
    size_t vo = r.U32(s);
    for (int32_t i = 0; i < vertexCount; i++, vo += 8) shape.vertices.push_back({r.S16(vo), r.S16(vo + 2), r.S16(vo + 4)});

    for (size_t li = 0; li < 8; li++) {
        int16_t count = r.S16(s + 0x30 + li * 2);
        if (count < 0) throw std::runtime_error("track: negative polygon count");
        size_t po = r.U32(s + 4 + li * 4);
        const bool quad = (li & 1) != 0, gouraud = (li & 2) != 0;
        for (int16_t i = 0; i < count; i++, po += kStride[li]) {
            TrackPolygon p;
            uint32_t w0 = r.U32(po), w1 = r.U32(po + 4);
            p.vertex = {uint16_t(w0 & 0x1FF), uint16_t((w0 >> 9) & 0x1FF), uint16_t((w0 >> 18) & 0x1FF), uint16_t(w1 & 0x1FF)};
            p.renderOrder = uint8_t(w0 >> 27);
            // 14 bits: the chunk drawers address the entry as ((w1 >> 4) & 0x7FFE0) + table (0x8002106C, 0x800234F8);
            // bits 19-22 are shared with `flags` below (kept as w1 >> 19 for the physics' surface lookup).
            p.uvIndex = uint16_t((w1 >> 9) & 0x3FFF);
            p.flags = w1 >> 19;
            p.primCode = r.U8(po + 11);
            if (p.IsTextured() && p.uvIndex >= uvCount) throw std::runtime_error("track: polygon UV index out of range");
            if ((p.primCode & 0xFC) != kExpectedCode[li]) throw std::runtime_error("track: polygon code does not match its list");
            const size_t corners = quad ? 4 : 3;
            for (size_t c = 0; c < corners; c++) {
                size_t co = gouraud ? po + 8 + c * 4 : po + 8;
                p.color[c] = {r.U8(co), r.U8(co + 1), r.U8(co + 2)};
            }
            for (size_t c = 0; c < corners; c++)
                if (p.vertex[c] >= shape.vertices.size()) throw std::runtime_error("track: polygon vertex index out of range");
            shape.polygons.push_back(p);
        }
    }
    return shape;
}

// The surface grid at chunk + 0x9C: { s16 originX, s16 originZ, s16 shiftX, s16 shiftZ; u16 count[16];
// u32 listOffset[16] } with each list holding u32 offsets of polygons inside the chunk's road shape (the offsets
// become pointers when the game loads the file; here they become polygon numbers of the parsed shape).
TrackSurfaceGrid ReadSurfaceGrid(const Reader& r, size_t chunk, const TrackShape& road) {
    static constexpr size_t kStride[8] = {12, 12, 20, 24, 12, 12, 20, 24};
    const size_t shape = chunk + kChunkShapeOffset;
    size_t listOffset[8], listCount[8], cumulative[8], total = 0;
    for (size_t li = 0; li < 8; li++) {
        listOffset[li] = r.U32(shape + 4 + li * 4);
        const int16_t count = r.S16(shape + 0x30 + li * 2);
        listCount[li] = count < 0 ? 0 : size_t(count);
        cumulative[li] = total;
        total += listCount[li];
    }
    if (total != road.polygons.size()) throw std::runtime_error("track: surface grid: polygon count differs from the road shape");
    const size_t grid = r.U32(chunk + 0x9C);
    TrackSurfaceGrid g;
    g.originX = r.S16(grid);
    g.originZ = r.S16(grid + 2);
    g.shiftX = r.S16(grid + 4);
    g.shiftZ = r.S16(grid + 6);
    for (size_t cell = 0; cell < 16; cell++) {
        const size_t n = r.U16(grid + 8 + cell * 2), list = r.U32(grid + 0x28 + cell * 4);
        for (size_t j = 0; j < n; j++) {
            const size_t polygon = r.U32(list + j * 4);
            bool found = false;
            for (size_t li = 0; li < 8 && !found; li++) {
                if (polygon < listOffset[li] || polygon >= listOffset[li] + listCount[li] * kStride[li]) continue;
                if ((polygon - listOffset[li]) % kStride[li] != 0) throw std::runtime_error("track: surface grid polygon offset is not aligned to its list");
                g.cells[cell].push_back(uint16_t(cumulative[li] + (polygon - listOffset[li]) / kStride[li]));
                found = true;
            }
            if (!found) throw std::runtime_error("track: surface grid polygon offset outside the chunk's road shape");
        }
    }
    return g;
}

// Scenery model at `s` (same 0x44-byte shape header as the chunk shapes, then bounds, scale exponent and two
// reference vertices). Polygons: word0 = v0 | v1 << 10 | v2 << 20 | sortNearest << 30 | cullBackface << 31,
// word1 = v3 (quads), then (r, g, b, code), then for textured lists the three texture words
// (u0 v0 clut) (u1 v1 tpage) (u2 v2 u3 v3), then the extra gouraud colours. Strides: F 12, G3 20, G4 24,
// FT 24, GT3 32, GT4 36 (0x80019B58; corner/UV order verified against the captured GP0 output).
TrackSceneryModel ReadSceneryModel(const Reader& r, size_t s) {
    static constexpr size_t kStride[8] = {12, 12, 20, 24, 24, 24, 32, 36};
    static constexpr uint8_t kExpectedCode[8] = {0x20, 0x28, 0x30, 0x38, 0x24, 0x2C, 0x34, 0x3C};

    TrackSceneryModel model;
    const int32_t vertexCount = r.S32(s + 0x2C);
    if (vertexCount < 0 || vertexCount > 1024) throw std::runtime_error("track: bad scenery model vertex count");
    size_t vo = r.U32(s);
    for (int32_t i = 0; i < vertexCount; i++, vo += 8) model.vertices.push_back({r.S16(vo), r.S16(vo + 2), r.S16(vo + 4)});
    model.billboardCount = r.U16(s + 0x40);
    model.lightCount = r.U16(s + 0x42);
    model.glows = ReadGlows(r, s);
    for (size_t k = 0, o = r.U32(s + 0x24); k < model.billboardCount; k++, o += 28) {
        TrackBillboard b;
        b.position = {r.S16(o), r.S16(o + 2), r.S16(o + 4)}; // the word at +4 feeds the 16-bit VZ register
        b.width = r.S16(o + 8);
        b.height = r.S16(o + 10);
        b.color = {r.U8(o + 12), r.U8(o + 13), r.U8(o + 14)};
        b.primCode = r.U8(o + 15);
        const uint32_t t0 = r.U32(o + 16), t1 = r.U32(o + 20), t2 = r.U32(o + 24);
        b.u = {uint8_t(t0 & 0xFF), uint8_t(t1 & 0xFF), uint8_t(t2 & 0xFF), uint8_t((t2 >> 16) & 0xFF)};
        b.v = {uint8_t((t0 >> 8) & 0xFF), uint8_t((t1 >> 8) & 0xFF), uint8_t((t2 >> 8) & 0xFF), uint8_t((t2 >> 24) & 0xFF)};
        b.clut = uint16_t(t0 >> 16);
        b.tpage = uint16_t(t1 >> 16);
        model.billboards.push_back(b);
    }
    model.boundsMin = {r.S16(s + 0x44), r.S16(s + 0x46), r.S16(s + 0x48)};
    model.boundsMax = {r.S16(s + 0x4C), r.S16(s + 0x4E), r.S16(s + 0x50)};
    model.scaleExponent = r.S16(s + 0x54);
    if (model.scaleExponent < 8 || model.scaleExponent > 31) throw std::runtime_error("track: bad scenery model scale exponent");

    for (size_t li = 0; li < 8; li++) {
        const int16_t count = r.S16(s + 0x30 + li * 2);
        if (count < 0) throw std::runtime_error("track: negative scenery polygon count");
        size_t po = r.U32(s + 4 + li * 4);
        const bool quad = (li & 1) != 0, gouraud = (li & 2) != 0, textured = (li & 4) != 0;
        for (int16_t i = 0; i < count; i++, po += kStride[li]) {
            TrackSceneryPolygon p;
            const uint32_t w0 = r.U32(po), w1 = r.U32(po + 4);
            p.vertex = {uint16_t(w0 & 0x3FF), uint16_t((w0 >> 10) & 0x3FF), uint16_t((w0 >> 20) & 0x3FF), uint16_t(w1 & 0x3FF)};
            p.sortNearest = (w0 & 0x40000000u) != 0;
            p.cullBackface = (w0 & 0x80000000u) != 0;
            p.primCode = r.U8(po + 11);
            if ((p.primCode & 0xFC) != kExpectedCode[li]) throw std::runtime_error("track: scenery polygon code does not match its list");
            const size_t corners = quad ? 4 : 3;
            size_t next = po + 12;
            if (textured) {
                const uint32_t t0 = r.U32(next), t1 = r.U32(next + 4), t2 = r.U32(next + 8);
                p.u = {uint8_t(t0 & 0xFF), uint8_t(t1 & 0xFF), uint8_t(t2 & 0xFF), uint8_t((t2 >> 16) & 0xFF)};
                p.v = {uint8_t((t0 >> 8) & 0xFF), uint8_t((t1 >> 8) & 0xFF), uint8_t((t2 >> 8) & 0xFF), uint8_t((t2 >> 24) & 0xFF)};
                p.clut = uint16_t(t0 >> 16);
                p.tpage = uint16_t(t1 >> 16);
                next += 12;
            }
            p.color[0] = {r.U8(po + 8), r.U8(po + 9), r.U8(po + 10)};
            for (size_t c = 1; c < corners; c++) {
                const size_t co = gouraud ? next + (c - 1) * 4 : po + 8;
                p.color[c] = {r.U8(co), r.U8(co + 1), r.U8(co + 2)};
            }
            for (size_t c = 0; c < corners; c++)
                if (p.vertex[c] >= model.vertices.size()) throw std::runtime_error("track: scenery polygon vertex index out of range");
            model.polygons.push_back(p);
        }
    }
    return model;
}

} // namespace

Track ParseTrack(std::span<const uint8_t> data) {
    Reader r{data};
    if (data.size() < 0x1A0 || std::memcmp(data.data(), "@(#)GT-PS", 9) != 0) throw std::runtime_error("track: bad magic");
    if (r.U32(0x118) != 0x19C) throw std::runtime_error("track: file is not zero-based (unexpected pointer base)");

    Track track;
    track.startAngle = r.S16(0x54);
    for (size_t i = 0; i < 16; i++)
        track.startGrid[i] = {r.S32(0x58 + i * 12), r.S32(0x5C + i * 12), r.S32(0x60 + i * 12)};
    size_t table = r.U32(0x10);
    track.courseLength = r.S32(table);
    track.lengthMetres = r.S16(table + 2);
    int16_t chunkCount = r.S16(table + 4);
    if (chunkCount <= 0) throw std::runtime_error("track: bad chunk count");

    // Course-wide UV table: 32 bytes = near set (u0 v0 clut u1 v1 tpage u2 v2 u3 v3), u32 extra, far set, u32 0.
    int16_t uvCount = r.S16(table + 6);
    size_t uvOffset = r.U32(table + 8);
    if (uvCount < 0) throw std::runtime_error("track: bad UV table count");
    auto readSet = [&](size_t o) {
        TrackUvSet s;
        s.u = {r.U8(o), r.U8(o + 4), r.U8(o + 8), r.U8(o + 10)};
        s.v = {r.U8(o + 1), r.U8(o + 5), r.U8(o + 9), r.U8(o + 11)};
        s.clut = r.U16(o + 2);
        s.tpage = r.U16(o + 6);
        return s;
    };
    for (int16_t i = 0; i < uvCount; i++) {
        size_t o = uvOffset + size_t(i) * 32;
        track.uvTable.push_back({readSet(o), readSet(o + 16), r.U32(o + 12)});
    }

    for (int16_t i = 0; i < chunkCount; i++) {
        size_t c = r.U32(table + 12 + size_t(i) * 4);
        TrackChunk chunk;
        chunk.prev = r.U16(c);
        chunk.next = r.U16(c + 2);
        chunk.distance = r.S32(c + 0x10);
        chunk.weightNext = r.U16(c + 0x14);
        chunk.weightThis = r.U16(c + 0x16);
        chunk.vcoord = r.S16(c + 0x12);
        chunk.origin = {r.S32(c + 0x18), r.S32(c + 0x1C), r.S32(c + 0x20)};
        chunk.direction = {r.S16(c + 0x24), r.S16(c + 0x26), r.S16(c + 0x28)};
        chunk.centre = {r.S32(c + 0x30), r.S32(c + 0x34), r.S32(c + 0x38)};
        chunk.extent = r.S32(c + 0x2C);
        for (size_t b = 0; b < 2; b++) { // + 0x44 / + 0x6C: {s32 x, s32 z} x 4, s32 yLow, s32 yHigh (0x80020EC4)
            const size_t o = c + 0x44 + b * 0x28;
            for (size_t k = 0; k < 4; k++) {
                chunk.bounds[b].x[k] = r.S32(o + k * 8);
                chunk.bounds[b].z[k] = r.S32(o + k * 8 + 4);
            }
            chunk.bounds[b].yLow = r.S32(o + 0x20);
            chunk.bounds[b].yHigh = r.S32(o + 0x24);
        }
        auto floorCell = [](int32_t v) {
            int64_t q = v / kCellSize;
            if (v % kCellSize < 0) q--;
            return int32_t(q * kCellSize);
        };
        chunk.cellOrigin = {floorCell(chunk.centre[0]), floorCell(chunk.centre[2])};
        const size_t boundaryBlock = r.U32(c + 0x98);
        const int32_t boundaryCount = r.S32(boundaryBlock);
        if (boundaryCount < 0 || boundaryCount > 4096) throw std::runtime_error("track: bad boundary count");
        for (int32_t b = 0; b < boundaryCount; b++) {
            const size_t e = boundaryBlock + 4 + size_t(b) * 8;
            chunk.boundaries.push_back({r.U16(e), r.U16(e + 2), r.S16(e + 4), r.S16(e + 6)});
        }
        chunk.road = ReadShape(r, c + kChunkShapeOffset, track.uvTable.size());
        for (const TrackBoundary& b : chunk.boundaries)
            if (b.vertexA >= chunk.road.vertices.size() || b.vertexB >= chunk.road.vertices.size())
                throw std::runtime_error("track: boundary vertex index out of range");
        chunk.surround = ReadShape(r, r.U32(c + 0x94), track.uvTable.size());
        { // billboards of the road shape: 16-byte records at shape + 0x24, count u16 at shape + 0x40
            const size_t shape = c + kChunkShapeOffset;
            const uint16_t count = r.U16(shape + 0x40);
            chunk.lightCount = r.U16(shape + 0x42);
            for (size_t k = 0, o = r.U32(shape + 0x24); k < count; k++, o += 16) {
                TrackBillboard b;
                b.position = {r.S16(o), r.S16(o + 2), r.S16(o + 4)};
                const uint16_t uvIndex = r.U16(o + 6);
                if (uvIndex >= track.uvTable.size()) throw std::runtime_error("track: billboard UV index out of range");
                const TrackUvSet& set = track.uvTable[uvIndex].nearSet;
                b.u = set.u;
                b.v = set.v;
                b.clut = set.clut;
                b.tpage = set.tpage;
                b.uvIndex = uvIndex;
                b.width = r.S16(o + 8);
                b.height = r.S16(o + 10);
                b.color = {r.U8(o + 12), r.U8(o + 13), r.U8(o + 14)};
                b.primCode = r.U8(o + 15);
                chunk.billboards.push_back(b);
            }
        }
        chunk.surfaceGrid = ReadSurfaceGrid(r, c, chunk.road);
        chunk.sceneryMask = r.U32(c + 0x0C);
        const size_t renderList = r.U32(c + 0xA0);
        const uint16_t renderCount = r.U16(renderList);
        if (renderCount > 1024) throw std::runtime_error("track: bad render list count");
        for (uint16_t k = 0; k < renderCount; k++) {
            const uint16_t entry = r.U16(renderList + 2 + size_t(k) * 2);
            if ((entry & 0x3FFF) >= uint16_t(chunkCount)) throw std::runtime_error("track: render list chunk index out of range");
            chunk.renderList.push_back(entry);
        }
        track.chunks.push_back(std::move(chunk));
    }

    // Scenery: models (header 0x18), LOD lists (header 0x14), instance lists (header 0x118, 33 of them).
    const size_t modelTable = r.U32(0x18);
    const uint32_t modelCount = r.U32(modelTable);
    if (modelCount > 4096) throw std::runtime_error("track: bad scenery model count");
    std::vector<uint32_t> modelOffsets;
    for (uint32_t i = 0; i < modelCount; i++) {
        modelOffsets.push_back(r.U32(modelTable + 4 + size_t(i) * 4));
        track.sceneryModels.push_back(ReadSceneryModel(r, modelOffsets.back()));
    }
    const size_t lodTable = r.U32(0x14);
    const uint32_t lodCount = r.U32(lodTable);
    if (lodCount > 4096) throw std::runtime_error("track: bad scenery LOD list count");
    for (uint32_t i = 0; i < lodCount; i++) {
        const size_t list = r.U32(lodTable + 4 + size_t(i) * 4);
        const uint32_t entries = r.U32(list);
        if (entries == 0 || entries > 16) throw std::runtime_error("track: bad scenery LOD entry count");
        std::vector<TrackLodEntry> lod;
        for (uint32_t k = 0; k < entries; k++) {
            const size_t e = list + 4 + size_t(k) * 8;
            const uint32_t modelOffset = r.U32(e + 4);
            size_t model = 0;
            while (model < modelOffsets.size() && modelOffsets[model] != modelOffset) model++;
            if (model == modelOffsets.size()) throw std::runtime_error("track: scenery LOD entry does not point at a model");
            lod.push_back({r.U32(e), uint16_t(model)});
        }
        track.sceneryLods.push_back(std::move(lod));
    }
    for (uint32_t li = 0; li < 33; li++) {
        const size_t list = r.U32(0x118 + size_t(li) * 4);
        const uint32_t records = r.U32(list);
        if (records > 65536) throw std::runtime_error("track: bad scenery instance count");
        for (uint32_t k = 0; k < records; k++) {
            const size_t e = list + 4 + size_t(k) * 28;
            TrackSceneryInstance inst;
            inst.angle = {r.S16(e), r.S16(e + 2), r.S16(e + 4)};
            inst.lodList = r.U16(e + 6);
            if (inst.lodList >= track.sceneryLods.size()) throw std::runtime_error("track: scenery instance LOD list out of range");
            inst.scale = {r.S16(e + 8), r.S16(e + 10), r.S16(e + 12)};
            inst.lodDivisor = r.S16(e + 14);
            inst.position = {r.S32(e + 16), r.S32(e + 20), r.S32(e + 24)};
            inst.list = uint8_t(li);
            track.sceneryInstances.push_back(inst);
        }
    }
    return track;
}

double SceneryMetresPerUnit(const TrackSceneryModel& model) {
    return std::ldexp(1.0, model.scaleExponent - 16) / 4096.0;
}

std::array<float, 16> SceneryInstanceMatrix(const TrackSceneryInstance& instance, const TrackSceneryModel& model) {
    // 0x80081374(m, a = -angle[1], b = angle[0], c = angle[2]) builds Ry(a) * Rx(-b) * Rz(c) from the 4096-entry
    // sine/cosine tables (positive angle = counter-clockwise about the axis when looking down the axis).
    const double turn = 6.283185307179586 / 4096.0;
    const double a = -instance.angle[1] * turn, b = instance.angle[0] * turn, c = instance.angle[2] * turn;
    const double ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cc = std::cos(c), sc = std::sin(c);
    const double R[3][3] = {{ca * cc - sa * sb * sc, -ca * sc - sa * sb * cc, sa * cb},
                            {cb * sc, cb * cc, sb},
                            {-sa * cc - ca * sb * sc, sa * sc - ca * sb * cc, ca * cb}};
    // 0x8007B25C(m, scale[0], scale[1], -scale[2]): columns scaled, z negated.
    const double s[3] = {instance.scale[0] / 4096.0, instance.scale[1] / 4096.0, -instance.scale[2] / 4096.0};
    const double mu = SceneryMetresPerUnit(model);
    std::array<float, 16> out{};
    for (int col = 0; col < 3; col++)
        for (int row = 0; row < 3; row++) out[size_t(col * 4 + row)] = float(R[row][col] * s[col] * mu);
    for (int row = 0; row < 3; row++) out[size_t(12 + row)] = float(instance.position[size_t(row)] / 65536.0);
    out[15] = 1.0f;
    return out;
}

int32_t SceneryLodK(int32_t projectionDistance, int16_t lodDivisor) {
    const int32_t view78 = (int32_t(160) << 12) / std::max<int32_t>(1, projectionDistance); // 0x8007B374: ((320 >> 1) << 12) / h
    return int32_t((int64_t(view78) << 12) / (lodDivisor != 0 ? lodDivisor : 4096));     // 0x8001F944.. (div by the record's s16)
}

uint32_t SceneryLodMeasure(const std::array<int32_t, 3>& t, int32_t k) {
    // 0x8007AE38: IR1..3 = t >> 14 (MTC2 keeps 16 bits), SQR (sf 0) -> MAC1..3, 32-bit sum, then two 64-bit products
    uint32_t sum = 0;
    for (int32_t c : t) {
        const int32_t q = int16_t(uint16_t(uint32_t(c >> 14) & 0xFFFF));
        sum += uint32_t(q * q);
    }
    const int64_t first = int64_t(int32_t(sum)) * int64_t(k);
    const uint32_t scaled = uint32_t(uint64_t(first) >> 12);
    const uint64_t second = uint64_t(scaled) * uint64_t(int64_t(k));
    return uint32_t(second >> 12);
}

std::array<int32_t, 3> SceneryCameraSpace(const TrackSceneryInstance& instance, const std::array<double, 3>& eye,
                                          const std::array<std::array<double, 3>, 3>& axes) {
    const double offset[3] = {instance.position[0] / 65536.0 - eye[0], instance.position[1] / 65536.0 - eye[1], instance.position[2] / 65536.0 - eye[2]};
    std::array<int32_t, 3> t{};
    for (size_t r = 0; r < 3; r++) {
        double c = axes[r][0] * offset[0] + axes[r][1] * offset[1] + axes[r][2] * offset[2];
        if (r == 1) c *= 3723.0 / 4096.0; // the camera matrix's y row carries the screen aspect (captured row norms 3717..3723)
        t[r] = int32_t(std::llround(std::clamp(c * 65536.0, -2147483648.0, 2147483647.0)));
    }
    return t;
}

uint32_t SceneryLodThreshold(uint32_t fileWord) {
    const uint32_t d = fileWord >> 14; // 0x8007ADC8 at course load: srl 14, mult, mflo (low 32 bits)
    return d * d;
}

int SceneryLodIndex(const std::vector<TrackLodEntry>& lods, uint32_t measure) {
    for (size_t i = 0; i < lods.size(); i++)
        if (SceneryLodThreshold(lods[i].threshold) > measure) return int(i);
    return -1;
}

std::array<float, 3> TrackVertexToWorld(const TrackChunk& chunk, const TrackVertex& v) {
    // All three coordinates are relative to a 64 m cell (ground.cpp InterpolateRoadHeight).
    // In particular, a centre just below zero has a -64 m vertical cell.
    const int32_t cellY = int32_t(uint32_t(chunk.centre[1]) & 0xFFC00000u);
    return {float(chunk.cellOrigin[0] / 65536.0 + v.x / 64.0), float(cellY / 65536.0 + v.z / 64.0),
            float(chunk.cellOrigin[1] / 65536.0 + v.y / 64.0)};
}

} // namespace gt2
