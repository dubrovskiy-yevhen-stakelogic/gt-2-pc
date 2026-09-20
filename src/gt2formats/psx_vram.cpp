#include "gt2formats/psx_vram.h"

#include <stdexcept>

namespace gt2 {
namespace {

uint16_t ReadU16(std::span<const uint8_t> d, size_t o) {
    if (o + 2 > d.size()) throw std::runtime_error("TIM: read out of bounds");
    return uint16_t(d[o] | (d[o + 1] << 8));
}
uint32_t ReadU32(std::span<const uint8_t> d, size_t o) { return uint32_t(ReadU16(d, o)) | (uint32_t(ReadU16(d, o + 2)) << 16); }

} // namespace

size_t PsxVram::LoadTim(std::span<const uint8_t> data, size_t offset) {
    if (ReadU32(data, offset) != 0x10) throw std::runtime_error("TIM: bad magic");
    uint32_t flags = ReadU32(data, offset + 4);
    offset += 8;
    const int blocks = (flags & 8) ? 2 : 1; // optional CLUT block, then the image block
    for (int b = 0; b < blocks; b++) {
        uint32_t length = ReadU32(data, offset);
        int x = ReadU16(data, offset + 4), y = ReadU16(data, offset + 6);
        int w = ReadU16(data, offset + 8), h = ReadU16(data, offset + 10);
        if (length < 12 || size_t(w) * h * 2 > length - 12 || x + w > kWidth || y + h > kHeight)
            throw std::runtime_error("TIM: block does not fit");
        for (int row = 0; row < h; row++)
            for (int col = 0; col < w; col++)
                words_[size_t(y + row) * kWidth + size_t(x + col)] = ReadU16(data, offset + 12 + (size_t(row) * w + col) * 2);
        offset += length;
    }
    return offset;
}

size_t PsxVram::LoadTimPack(std::span<const uint8_t> data) {
    uint32_t count = ReadU32(data, 0);
    size_t offset = 4;
    for (uint32_t i = 0; i < count; i++) offset = LoadTim(data, offset);
    if (offset != data.size()) throw std::runtime_error("TIM pack: trailing bytes after the last TIM");
    return count;
}

uint16_t PsxVram::Sample(uint16_t tpage, uint16_t clut, uint8_t u, uint8_t v) const {
    const int pageX = (tpage & 0xF) * 64, pageY = ((tpage >> 4) & 1) * 256;
    const int clutX = (clut & 0x3F) * 16, clutY = clut >> 6;
    switch ((tpage >> 7) & 3) {
    case 0: {
        uint16_t word = Word(pageX + u / 4, pageY + v);
        return Word(clutX + ((word >> ((u & 3) * 4)) & 0xF), clutY);
    }
    case 1: {
        uint16_t word = Word(pageX + u / 2, pageY + v);
        return Word(clutX + ((word >> ((u & 1) * 8)) & 0xFF), clutY);
    }
    default: return Word(pageX + u, pageY + v);
    }
}

} // namespace gt2
