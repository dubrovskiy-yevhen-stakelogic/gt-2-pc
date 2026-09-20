#include "gt2formats/car_texture.h"

#include <stdexcept>

namespace gt2 {
namespace {

constexpr size_t kPaintTableOffset = 0x20;
constexpr size_t kPaintSize = 0x240;
constexpr size_t kPixelOffset = 0x43A0;
constexpr size_t kMaxPaints = 16;

uint16_t ReadU16(std::span<const uint8_t> d, size_t o) { return static_cast<uint16_t>(d[o] | (d[o + 1] << 8)); }

} // namespace

CarTexture ParseCarTexture(std::span<const uint8_t> data) {
    const size_t pixelBytes = CarTexture::kWidth * CarTexture::kHeight / 2;
    if (data.size() < kPixelOffset + pixelBytes) throw std::runtime_error("car texture: file too small");

    CarTexture t;
    uint16_t paintCount = ReadU16(data, 0);
    if (paintCount == 0 || paintCount > kMaxPaints) throw std::runtime_error("car texture: bad paint count");

    for (size_t p = 0; p < paintCount; p++) {
        CarTexture::Paint paint;
        paint.id = data[2 + p]; // one byte per paint (corpus: up to 16 paints, u16 reads gave packed pairs)
        size_t base = kPaintTableOffset + p * kPaintSize;
        for (size_t c = 0; c < CarTexture::kClutCount; c++)
            for (size_t i = 0; i < 16; i++) paint.cluts[c][i] = ReadU16(data, base + c * 0x20 + i * 2);
        for (size_t i = 0; i < 16; i++) paint.illuminationMask[i] = ReadU16(data, base + 16 * 0x20 + i * 2);
        t.paints.push_back(paint);
    }

    t.indices.resize(static_cast<size_t>(CarTexture::kWidth) * CarTexture::kHeight);
    for (size_t i = 0; i < pixelBytes; i++) {
        uint8_t b = data[kPixelOffset + i];
        t.indices[i * 2] = b & 0x0F; // PS1 4bpp: low nibble is the left pixel
        t.indices[i * 2 + 1] = b >> 4;
    }
    return t;
}

int CarTexture::PaintIndex(uint8_t id) const {
    for (size_t p = 0; p < paints.size(); p++)
        if (paints[p].id == id) return int(p);
    return -1;
}

std::vector<uint8_t> CarTexture::DecodeRgba(size_t paint, size_t clut) const {
    const auto& pal = paints.at(paint).cluts.at(clut);
    std::vector<uint8_t> rgba(indices.size() * 4);
    for (size_t i = 0; i < indices.size(); i++) {
        uint16_t c = pal[indices[i]];
        auto expand = [](uint16_t v5) { return static_cast<uint8_t>((v5 << 3) | (v5 >> 2)); };
        rgba[i * 4 + 0] = expand(c & 0x1F);
        rgba[i * 4 + 1] = expand((c >> 5) & 0x1F);
        rgba[i * 4 + 2] = expand((c >> 10) & 0x1F);
        rgba[i * 4 + 3] = c == 0 ? 0 : 255;
    }
    return rgba;
}

} // namespace gt2
