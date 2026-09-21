#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace gt2 {

// GT2 car model (.cdo day / .cno night), magic "GT\x02\0".
// Field layout follows Nenkai's MIT 010 template (research: SUBMANIAC, commongear, pez2k),
// verified against the US v1.2 corpus by `gt2tool car-scan`.

struct CarVertex {
    int16_t x, y, z, w;
};

struct CarPolygon {
    static constexpr uint8_t kCodeQuad = 0x08;
    static constexpr uint8_t kCodeTextured = 0x04;
    static constexpr uint8_t kCodeGouraud = 0x10;
    static constexpr uint8_t kCodeSemiTransparent = 0x02;
    static constexpr uint8_t kCodeRawTexture = 0x01; // texture not modulated by the polygon colour

    std::array<uint8_t, 4> vertex{};
    uint8_t renderOrder = 0;       // 5 bits: authored face order (the PS1 has no Z-buffer)
    std::array<uint16_t, 4> normal{}; // 9-bit indices into the LOD normal table
    uint16_t renderFlags = 0;
    uint8_t r = 0, g = 0, b = 0;
    uint8_t primCode = 0;          // libgpu primitive code (POLY_F3 = 0x20 ... POLY_GT4 = 0x3C)

    // Textured polygons only.
    std::array<uint8_t, 4> u{}, v{};
    uint16_t rawPalette = 0;
    uint8_t palette = 0;           // CLUT 0..15

    bool IsQuad() const { return (primCode & kCodeQuad) != 0; }
    bool IsTextured() const { return (primCode & kCodeTextured) != 0; }
};

struct CarNormal {
    int16_t x, y, z; // signed 10-bit components
};

struct CarLod {
    uint32_t maxDistance = 0;
    std::array<uint16_t, 10> counts{};
    std::array<int16_t, 8> bbox{};
    int16_t scale = 0;
    int16_t scaleUnk = 0;
    std::vector<CarVertex> vertices;
    std::vector<CarNormal> normals;
    std::vector<CarPolygon> polygons; // tris, quads, uv tris, uv quads - in file order
};

struct CarWheelOffset {
    int16_t w, y, x, z;
};

// The ground shadow, stored after the last LOD: { u16 vertexCount, u16 triCount, u16 quadCount, u16 0;
// 4 x { s16 v, s16 0 } bbox (xmin, zmin, xmax, zmax); s16 scale, s16 radius; vertexCount x { s16 x, s16 z };
// (triCount + quadCount) x u32 polygon }. Polygon word: four 6-bit vertex indices (bits 0-23), bit 31 = fully
// shaded; otherwise corners 0 and 1 are the unshaded outer edge and 2, 3 the shaded inner edge. The original
// draws it as subtractive (B - F) gouraud quads on the ground plane with the car's heading (Seattle attract
// race, US v1.2: 18 primitives per car = 2 tris + 16 quads, corner colours 000000 / FFFFFF).
struct CarShadowPolygon {
    std::array<uint8_t, 4> vertex{};
    bool quad = false;
    bool fullyShaded = false;
};

struct CarShadow {
    int16_t scale = 17; // independent of the body LOD, applied by 0x80068004 through 0x8007B8A0
    std::vector<std::array<int16_t, 2>> vertices; // x, z in 2^(scale - 16) / 4096 metres
    std::vector<CarShadowPolygon> polygons;       // tris first, then quads
};

struct CarModel {
    std::array<uint8_t, 3> wheelDishColor{};
    int16_t wheelRadiusFront = 0, wheelWidthFront = 0;
    int16_t wheelRadiusRear = 0, wheelWidthRear = 0;
    std::array<CarWheelOffset, 4> wheels{}; // front L, front R, rear L, rear R
    std::vector<CarLod> lods;
    CarShadow shadow;                       // empty when the file has no shadow block
};

// Throws std::runtime_error on malformed data.
CarModel ParseCarModel(std::span<const uint8_t> data);

} // namespace gt2
