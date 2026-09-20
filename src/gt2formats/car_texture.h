#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace gt2 {

// GT2 car texture + palettes (.cdp day / .cnp night), always 45,984 bytes.
// Layout per Nenkai's MIT 010 template (research: SUBMANIAC).
struct CarTexture {
    static constexpr int kWidth = 256;
    static constexpr int kHeight = 224;
    static constexpr int kClutCount = 16;

    struct Paint {
        uint8_t id = 0; // matches paint ids in .carcolor / .carinfo
        std::array<std::array<uint16_t, 16>, kClutCount> cluts{}; // PS1 15-bit: R low, then G, B, STP
        std::array<uint16_t, 16> illuminationMask{};
    };

    std::vector<Paint> paints;
    std::vector<uint8_t> indices; // kWidth * kHeight, values 0..15

    // RGBA8 for one CLUT. Colour 0x0000 is fully transparent (PS1 rule).
    std::vector<uint8_t> DecodeRgba(size_t paint, size_t clut) const;

    // Index of the paint with the given id (.carinfo / .carcolor ids), -1 when absent.
    int PaintIndex(uint8_t id) const;

    // The wheel face: the original draws each wheel's rim as the top-left 48 x 48 texels of the car texture
    // with CLUT 0 (verified on the Seattle attract race: GP0 quads uv (0..47, 0..47), clut = slot CLUT 0).
    static constexpr int kWheelFaceSize = 48;
    static constexpr uint8_t kWheelFaceClut = 0;
};

CarTexture ParseCarTexture(std::span<const uint8_t> data);

} // namespace gt2
