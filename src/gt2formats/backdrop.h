#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gt2 {

// GT2 course backdrop (.bso, magic "BG\0\0") - the sky dome / horizon model drawn around the camera before the
// course, with its texture pack (.bsp, same TIM pack format as .trp). Derived from the bytes of US v1.2
// `bgsobj/*.bso` (all 34 files parse to the last byte) and the original's GTE/GP0 traffic on the Seattle attract
// race; see docs/formats/backdrop_bso.md.
//
// Model axes: x = -world X, y = up, z = -world Z; the model is centred on the camera (the original transforms
// it with the camera rotation and a zero translation). Units are arbitrary (radius ~4000).

struct BackdropVertex {
    int16_t x, y, z;
};

struct BackdropPolygon {
    std::array<uint16_t, 4> vertex{};              // 12-bit indices
    uint8_t flags = 0;                             // top byte of the first index word (0x80 seen)
    std::array<std::array<uint8_t, 3>, 4> color{}; // per corner; flat polygons repeat colour 0
    std::array<uint8_t, 4> u{}, v{};
    uint16_t clut = 0, tpage = 0;                  // PS1 texture words (textured polygons)
    uint8_t primCode = 0;                          // libgpu code: 0x20 F3 ... 0x3C GT4

    bool IsQuad() const { return (primCode & 0x08) != 0; }
    bool IsTextured() const { return (primCode & 0x04) != 0; }
    bool IsGouraud() const { return (primCode & 0x10) != 0; }
    bool IsSemiTransparent() const { return (primCode & 0x02) != 0; }
};

struct Backdrop {
    std::array<uint8_t, 3> skyColor{};    // the flat fill above the horizon
    std::array<uint8_t, 3> groundColor{}; // the flat fill below the horizon
    std::vector<BackdropVertex> vertices;
    std::vector<BackdropPolygon> polygons; // list order F3 F4 G3 G4 FT3 FT4 GT3 GT4
};

// Throws std::runtime_error on malformed data.
Backdrop ParseBackdrop(std::span<const uint8_t> data);

// Which backdrop a course uses: `.crsinfo` entry byte 10..11 (u16) indexes the `bgsobj/*.bso` files in VOL
// directory order (verified: Seattle -> sea_ha_e, Rome -> roma_sky, Rome-Night -> romadark_sky, Laguna Seca ->
// lagunasky, High Speed Ring -> tl_sky4, Grindelwald -> noon, Trial Mountain -> mskyX).
uint16_t BackdropIndexOf(std::span<const uint8_t, 14> courseInfoRest);

// Base name (e.g. "sea_ha_e") of the backdrop with the given index, from the VOL's sorted "bgsobj/*.bso.gz" paths.
std::string BackdropNameAt(const std::vector<std::string>& bsoPathsInVolOrder, uint16_t index);

} // namespace gt2
