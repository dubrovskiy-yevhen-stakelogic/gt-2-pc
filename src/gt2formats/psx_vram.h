#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace gt2 {

// 1024x512 16-bit image of PS1 video memory. Course (.trp) and sky (.bsp) texture packs are
// TIM sequences that carry their own VRAM destinations, and course polygons address textures
// by raw tpage/clut words - so the faithful way to texture them is to rebuild VRAM.
class PsxVram {
public:
    static constexpr int kWidth = 1024, kHeight = 512;

    PsxVram() : words_(size_t(kWidth) * kHeight, 0) {}

    // Pack = u32 count + count standard TIM files (verified: parses to the last byte of every .trp).
    // Returns the number of TIMs loaded.
    size_t LoadTimPack(std::span<const uint8_t> data);

    // Loads one TIM at `offset`, returns the offset just past it.
    size_t LoadTim(std::span<const uint8_t> data, size_t offset);

    uint16_t Word(int x, int y) const { return words_[size_t(y & (kHeight - 1)) * kWidth + size_t(x & (kWidth - 1))]; }

    // Texel colour (15-bit + STP) for PS1 texture words; handles 4/8/15-bit pages.
    uint16_t Sample(uint16_t tpage, uint16_t clut, uint8_t u, uint8_t v) const;

    const std::vector<uint16_t>& Words() const { return words_; }

private:
    std::vector<uint16_t> words_;
};

} // namespace gt2
