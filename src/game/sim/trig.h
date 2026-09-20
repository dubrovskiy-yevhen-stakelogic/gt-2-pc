#pragma once
#include <array>
#include <cmath>
#include <cstdint>

#include "game/sim/fixed.h"

// Angle convention of the simulation: 4096 units per turn, results scaled by 4096 (1.0 = 4096).
// The tables are generated here, not copied from the game; tools/gt2verify checks that every entry equals
// the original tables (sine at 0x80093150, cosine at 0x80093950, arc tangent at 0x800A4AC8 in the US v1.2
// executable).
namespace gt2::sim {

constexpr int kAngleUnits = 4096;
constexpr double kPi = 3.14159265358979323846;

// The original tables were produced with C's `(int)(v * 4096 + 0.5)`: truncation towards zero AFTER adding the
// half, so negative values come out one step short (-6.28 -> -5, minimum -4095). Established by gt2verify.
inline int16_t Quantize(double v) { return int16_t(static_cast<int>(v * 4096.0 + 0.5)); }

inline const std::array<int16_t, kAngleUnits>& SinTable() {
    static const std::array<int16_t, kAngleUnits> table = [] {
        std::array<int16_t, kAngleUnits> t{};
        for (int i = 0; i < kAngleUnits; i++) t[size_t(i)] = Quantize(std::sin(i * (2.0 * kPi / kAngleUnits)));
        return t;
    }();
    return table;
}

inline const std::array<int16_t, kAngleUnits>& CosTable() {
    static const std::array<int16_t, kAngleUnits> table = [] {
        std::array<int16_t, kAngleUnits> t{};
        for (int i = 0; i < kAngleUnits; i++) t[size_t(i)] = Quantize(std::cos(i * (2.0 * kPi / kAngleUnits)));
        return t;
    }();
    return table;
}

inline int32_t Sin(uint32_t angle) { return SinTable()[angle & 0xFFF]; }
inline int32_t Cos(uint32_t angle) { return CosTable()[angle & 0xFFF]; }

// atan(i / 4096) in angle units for the first octant, 4097 entries (the library's `ratan2` table).
inline const std::array<int16_t, kAngleUnits + 1>& AtanTable() {
    static const std::array<int16_t, kAngleUnits + 1> table = [] {
        std::array<int16_t, kAngleUnits + 1> t{};
        for (int i = 0; i <= kAngleUnits; i++) t[size_t(i)] = int16_t(static_cast<int>(std::atan(i / 4096.0) * (kAngleUnits / (2.0 * kPi)) + 0.5));
        return t;
    }();
    return table;
}

// Octant unfolding of the first-octant table: octant = (y < 0 ? 4 : 0) | (x < 0 ? 2 : 0) | (|x| < |y| ? 1 : 0),
// angle = (table ^ flip[octant]) + offset[octant]. These 16 constants are the usual atan2 symmetry, written as
// the original's two tables (0x800A2A8C, 0x800A2A94) so that gt2verify can check them.
inline constexpr int8_t kAtanOctantFlip[8] = {0, -1, -1, 0, -1, 0, 0, -1};
inline constexpr int16_t kAtanOctantOffset[8] = {0, 1025, 2049, 1024, 1, -1024, -2048, -1023};

// The library's table was not produced by rounding the exact arc tangent: about 3 % of its entries are one
// unit below the exact value, in a slowly oscillating pattern (an approximation error of its generator, at most
// 0.06 units). The generated table is used by the game; the verification tool substitutes the original's table
// through this pointer so that routines built on top of the arc tangent can still be checked bit for bit.
inline const int16_t*& AtanTableOverride() {
    static const int16_t* table = nullptr;
    return table;
}

// Arc tangent of y / x for 16-bit operands, 4096 units per turn, 0 = along +x, counter-clockwise towards +y
// (library routine 0x80082E14).
inline int32_t Atan2Narrow(int16_t y, int16_t x) {
    if (x == 0 && y == 0) return 0;
    const int32_t ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    const int32_t larger = ay <= ax ? ax : ay, smaller = ay <= ax ? ay : ax;
    const uint32_t octant = (uint32_t(y >> 15) & 4u) | (x < 0 ? 2u : 0u) | (ax < ay ? 1u : 0u);
    const int32_t index = (smaller << 12) / larger;
    const int16_t* table = AtanTableOverride() ? AtanTableOverride() : AtanTable().data();
    return int16_t((table[index] ^ kAtanOctantFlip[octant]) + kAtanOctantOffset[octant]);
}

// Arc tangent for 32-bit operands (library routine 0x80081AF0): both are shifted right until they fit in
// 17 bits (the original counts leading zeros of |y| | |x| with the GTE), then truncated to 16 bits.
inline int32_t Atan2(int32_t y, int32_t x) {
    const uint32_t magnitude = uint32_t(y ^ (y >> 31)) | uint32_t(x ^ (x >> 31));
    int leadingZeros = 0;
    for (uint32_t bit = 0x80000000u; bit && !(magnitude & bit); bit >>= 1) leadingZeros++;
    const int shift = 0x12 - leadingZeros;
    if (shift > 0) { y >>= shift; x >>= shift; }
    return Atan2Narrow(int16_t(y), int16_t(x));
}

} // namespace gt2::sim
