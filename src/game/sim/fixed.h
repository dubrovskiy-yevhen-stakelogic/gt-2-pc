#pragma once
#include <cstdint>

// Fixed-point arithmetic of the Gran Turismo 2 simulation, ported from the original routines and verified
// bit-for-bit against them (tools/gt2verify, table `kFixedTests`). Addresses are those of the US
// Simulation v1.2 executable and are kept only as provenance for the verification tool.
//
// Two families of products exist in the original:
//   * "rounded": a bias of (2^shift - 1) is added when the operand signs differ, so the result rounds
//     TOWARDS ZERO. The narrow variants keep only the low 32 bits of the product (overflow wraps, as in
//     the original); the wide variants use the full 64-bit product.
//   * "floor": the plain arithmetic shift of the 64-bit product (rounds towards minus infinity).
namespace gt2::sim {

namespace detail {
inline int32_t MulNarrow(int32_t a, int32_t b, uint32_t shift) {
    const uint32_t product = uint32_t(a) * uint32_t(b); // wraps like MIPS `mult` + `mflo`
    const uint32_t bias = uint32_t((a ^ b) >> 31) & ((1u << shift) - 1u);
    return int32_t(product + bias) >> shift;
}
inline int32_t MulWide(int32_t a, int32_t b, uint32_t shift) {
    const uint64_t product = uint64_t(int64_t(a) * int64_t(b));
    const uint32_t mask = (1u << shift) - 1u;
    const uint32_t low = uint32_t(product), high = uint32_t(product >> 32);
    const uint32_t bias = uint32_t((a ^ b) >> 31) & mask;
    const uint32_t shifted = (low >> shift) | (high << (32 - shift));
    return int32_t(shifted + uint32_t(int32_t((low & mask) + bias) >> shift));
}
inline int32_t MulFloor(int32_t a, int32_t b, uint32_t shift) {
    const int64_t product = int64_t(a) * int64_t(b);
    return int32_t(uint64_t(product >> shift)); // arithmetic shift, low word
}
} // namespace detail

inline int32_t Mul12(int32_t a, int32_t b) { return detail::MulNarrow(a, b, 12); }     // 0x8007596C
inline int32_t Mul16(int32_t a, int32_t b) { return detail::MulNarrow(a, b, 16); }     // 0x8007598C
inline int32_t Mul8(int32_t a, int32_t b) { return detail::MulNarrow(a, b, 8); }       // 0x800759AC
inline int32_t Mul12Shift(int32_t a, int32_t b, uint32_t extra) { return detail::MulNarrow(a, b, 12 + extra); } // 0x800759CC
// (a*b + c*d) in 64 bits, rounded towards zero, shifted right by 12 + extra; low 32 bits (0x80075EF8).
inline int32_t Dot2Shift12(int32_t a, int32_t b, int32_t c, int32_t d, uint32_t extra) {
    int64_t sum = int64_t(a) * b + int64_t(c) * d;
    const uint32_t shift = 12 + extra;
    if (sum < 0) sum += (int64_t(1) << shift) - 1;
    return int32_t(sum >> shift); // low word of the 64-bit result (arithmetic shift)
}

// MIPS `div`: quotient with the hardware's results for division by zero and INT_MIN / -1.
inline int32_t Div(int32_t n, int32_t d) {
    if (d == 0) return n >= 0 ? -1 : 1;
    if (uint32_t(n) == 0x80000000u && d == -1) return int32_t(0x80000000u);
    return n / d;
}

// 64-bit signed division of the compiler's runtime (0x80086084): truncates towards zero. The original traps
// on a zero divisor (the game never divides by zero there); we return 0 so that the caller stays defined.
inline int64_t Div64(int64_t n, int64_t d) {
    if (d == 0) return 0;
    if (d == -1) return int64_t(0u - uint64_t(n)); // INT64_MIN / -1 wraps to INT64_MIN like the original
    return n / d;
}

// (a << (12 + extra)) / b by the 64-bit runtime division, low word (0x80075E90).
inline int32_t Div12Shift(int32_t a, int32_t b, uint32_t extra = 0) { return int32_t(uint64_t(Div64(int64_t(a) << (12 + extra), b))); }

inline int32_t Mul12Wide(int32_t a, int32_t b) { return detail::MulWide(a, b, 12); }   // 0x80075A5C
inline int32_t Mul16Wide(int32_t a, int32_t b) { return detail::MulWide(a, b, 16); }   // 0x80075A94
inline int32_t Mul12ShiftWide(int32_t a, int32_t b, uint32_t extra) { return detail::MulWide(a, b, 12 + extra); } // 0x80075B04
inline int32_t Mul16ShiftWide(int32_t a, int32_t b, uint32_t extra) { return detail::MulWide(a, b, 16 + extra); } // 0x80075B54

inline int32_t Mul12Floor(int32_t a, int32_t b) { return detail::MulFloor(a, b, 12); } // 0x80075BF4
inline int32_t Mul16Floor(int32_t a, int32_t b) { return detail::MulFloor(a, b, 16); } // 0x80075C14
inline int32_t Mul12ShiftFloor(int32_t a, int32_t b, uint32_t extra) { return detail::MulFloor(a, b, 12 + extra); } // 0x80075C54

// Piecewise-linear curve lookup (0x80075D2C): `xs` ascending, `count` >= 1 points; clamps outside the range,
// binary search down to a span of 3 and a linear scan inside it, interpolation by 64-bit product / 64-bit
// division (the original's table object is { u16 count; s32* xs; s32* ys }).
inline int32_t Interpolate(const int32_t* xs, const int32_t* ys, uint32_t count, int32_t v) {
    const uint32_t last = (count - 1u) & 0xFFFFu;
    if (v <= xs[0]) return ys[0];
    if (v >= xs[last]) return ys[last];
    uint32_t low = 0, high = last;
    if (last > 3) {
        do {
            const uint32_t mid = (high + low) >> 1;
            if (xs[mid] <= v) {
                low = mid;
                if (v == xs[mid]) return ys[mid];
            } else {
                high = mid;
            }
        } while (high - low > 3);
    }
    uint32_t k = low;
    while (k < high && v >= xs[k]) k++;
    const int32_t x0 = xs[k - 1], y0 = ys[k - 1];
    const int64_t numerator = int64_t(ys[k] - y0) * int64_t(v - x0);
    return int32_t(uint64_t(Div64(numerator, int64_t(xs[k] - x0)))) + y0;
}

} // namespace gt2::sim
