#pragma once
// Word reads through guest addresses for native ports that follow guest pointers. The reader is supplied by the
// caller: the game runtime binds it to EE RAM, gt4verify to captured pages or random-case regions. Raw 32-bit patterns.
#include <cstdint>

namespace gt4::game {

struct GuestWords {
    const void* ctx;
    uint32_t (*load)(const void* ctx, uint32_t address);
    uint32_t operator()(uint32_t address) const { return load(ctx, address); }
    // Halfword at `address` (2-aligned) through the aligned word, little-endian.
    uint16_t Half(uint32_t address) const {
        return static_cast<uint16_t>(load(ctx, address & ~3u) >> (8 * (address & 2u)));
    }
    uint8_t Byte(uint32_t address) const {
        return static_cast<uint8_t>(load(ctx, address & ~3u) >> (8 * (address & 3u)));
    }
};

} // namespace gt4::game
