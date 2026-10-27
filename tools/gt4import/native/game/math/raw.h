#pragma once
// Little helpers for native ports that work on byte images of guest structures (unaligned-safe loads/stores of
// raw 32-bit float patterns), so that the access order of the original - and therefore aliasing behaviour - is kept.
#include <cstdint>
#include <cstring>

namespace gt4::game::raw {

inline uint32_t Ld(const uint8_t* p, int offset = 0) {
    uint32_t v;
    std::memcpy(&v, p + offset, 4);
    return v;
}
inline void St(uint8_t* p, int offset, uint32_t v) { std::memcpy(p + offset, &v, 4); }
inline uint16_t Ld16(const uint8_t* p, int offset) {
    uint16_t v;
    std::memcpy(&v, p + offset, 2);
    return v;
}
inline void St16(uint8_t* p, int offset, uint16_t v) { std::memcpy(p + offset, &v, 2); }
inline uint64_t Sext32(uint32_t v) { return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); }

} // namespace gt4::game::raw
