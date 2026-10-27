#pragma once
// Internal helpers of the car setup ports (carparams_init.cpp, carsetup.cpp): EE float operations on raw bit
// patterns (src/ee/fpu) and the guest memory view (EE RAM image, physical addresses) the ports work on.
#include <cstdint>
#include <cstring>

#include "ee/fpu.h"
#include "game/math/geom.h"

namespace gt4::game::carparams::guest {

using namespace gt4::ee;

// ---- EE float helpers (raw bit patterns) ----
inline uint32_t Mul(uint32_t a, uint32_t b) { return FpuMul(a, b, nullptr); }
inline uint32_t Add(uint32_t a, uint32_t b) { return FpuAdd(a, b, nullptr); }
inline uint32_t Sub(uint32_t a, uint32_t b) { return FpuSub(a, b, nullptr); }
inline uint32_t Div(uint32_t a, uint32_t b) { return FpuDiv(a, b, nullptr); }
inline uint32_t I2F(int32_t v) { return FpuCvtSW(static_cast<uint32_t>(v)); }
inline int32_t F2I(uint32_t f) { return static_cast<int32_t>(FpuCvtWS(f)); }
inline uint32_t Neg(uint32_t a) { return FpuNeg(a); }
inline bool Lt(uint32_t a, uint32_t b) { return FpuLt(a, b); }
inline bool Le(uint32_t a, uint32_t b) { return FpuLe(a, b); }
inline bool Eq(uint32_t a, uint32_t b) { return FpuEq(a, b); }

constexpr uint32_t kZero = 0, kOne = 0x3f800000u, kHalf = 0x3f000000u;
constexpr uint32_t kMilli = 0x3a83126eu;      // 0.0009999999
constexpr uint32_t kCenti = 0x3c23d70au;      // 0.01
constexpr uint32_t kTenth = 0x3dccccccu;      // 0.099999994
constexpr uint32_t kG = 0x411ccccc;           // 9.799999
constexpr uint32_t kKmh = 0x3e8e38e3u;        // 0.27777776 (1/3.6)
constexpr uint32_t kDeg = 0x3c8efa34u;        // 0.01745329
constexpr uint32_t kRpm = 0x3dd6774eu;        // 0.10471974 (2pi/60)
constexpr uint32_t kHundred = 0x42c80000u, kSixty = 0x42700000u, kTwoPi = 0x40c90fdau;
constexpr uint32_t kInch = 0x3cd013a9u;       // 0.0254

// ---- guest memory (EE RAM image; addresses >= kStack map to a native scratch area for the original's stack
// buffers, e.g. the 0x130-byte engine peak record of 0x00360780) ----
struct M {
    static constexpr uint32_t kStack = 0xf0000000u;
    uint8_t* r;
    uint8_t* stack = nullptr;
    uint8_t* p(uint32_t a) const { return a >= kStack ? stack + (a - kStack) : r + a; }
    uint8_t u8(uint32_t a) const { return *p(a); }
    int8_t s8(uint32_t a) const { return static_cast<int8_t>(*p(a)); }
    uint16_t u16(uint32_t a) const {
        const uint8_t* q = p(a);
        return static_cast<uint16_t>(q[0] | q[1] << 8);
    }
    int16_t s16(uint32_t a) const { return static_cast<int16_t>(u16(a)); }
    uint32_t u32(uint32_t a) const {
        uint32_t v;
        std::memcpy(&v, p(a), 4);
        return v;
    }
    uint32_t f(uint32_t a) const { return u32(a); }
    void w8(uint32_t a, uint32_t v) { *p(a) = static_cast<uint8_t>(v); }
    void w16(uint32_t a, uint32_t v) {
        uint8_t* q = p(a);
        q[0] = static_cast<uint8_t>(v);
        q[1] = static_cast<uint8_t>(v >> 8);
    }
    void w32(uint32_t a, uint32_t v) { std::memcpy(p(a), &v, 4); }
};

inline uint32_t U8F(uint32_t b) { return I2F(static_cast<int32_t>(b)); }

// Option record (0x003433e8: 0x20 bytes zeroed, +0x14 = 1); `opt` 0 -> the defaults.
struct Opt {
    uint8_t bytes[0x20] = {};
    Opt() { bytes[0x14] = 1; }
};

// ---- small accessors ----
inline uint32_t SpecEntry(M& m, uint32_t phys) { return static_cast<uint32_t>(m.u8(phys + 0x30)) * 0x54 + 0x620478u; }   // 0x00359510
inline uint32_t KindEntry(uint32_t kind) { return kind * 0x54 + 0x620478u; }                                               // 0x003594e8
inline bool Special(M& m, uint32_t cprm) { return m.u8(cprm + 2) == 1 || m.u8(cprm + 1) == 6; }                          // 0x0035a058
inline uint32_t OverrideTable(uint32_t index) { return (index > 2 ? 0 : index) * 0x78 + 0x6205c8u; }                      // 0x00359ef8

// 0x00359a38(curve, n, xs, ys): {s16 n, u8 0, u8 0, u32 xs, u32 ys}
inline void SetCurve(M& m, uint32_t c, uint32_t n, uint32_t xs, uint32_t ys) {
    m.w16(c, n);
    m.w32(c + 4, xs);
    m.w32(c + 8, ys);
    m.w8(c + 2, 0);
    m.w8(c + 3, 0);
}

inline GeoCurve Curve(M& m, uint32_t c) { return GeoCurve{m.s16(c), m.p(m.u32(c + 4)), m.p(m.u32(c + 8))}; }

// 0x00360468(curve, xs, ys, n, b1, b2): xs = b1 km/h -> m/s, ys = b2 * 0.2 deg -> rad.
inline void SpeedAngleCurve(M& m, uint32_t c, uint32_t xs, uint32_t ys, uint32_t n, uint32_t b1, uint32_t b2) {
    n &= 0xff;
    for (uint32_t i = 0; i < n; ++i) {
        m.w32(xs + 4 * i, Mul(U8F(m.u8(b1 + i)), kKmh));
        m.w32(ys + 4 * i, Mul(Mul(U8F(m.u8(b2 + i)), 0x3e4ccccc), kDeg));
    }
    SetCurve(m, c, n, xs, ys);
}

// 0x00466b28
inline uint32_t DriveLayout(int32_t a, int32_t b) {
    if (a == 1) return 1;
    if (a != 2) return 0;
    if (b == 3) return 3;
    if (b < 4) return b == 0 ? 1 : 2;
    return b == 5 ? 0 : 2;
}

// 0x0036cd98
inline void Redline(M& m, uint32_t cprm, uint32_t phys) {
    const uint8_t a = m.u8(cprm + 0x110);
    m.w8(phys + 0x1074, a ? a : m.u16(phys + 0x262) / 100u);
    const uint8_t b = m.u8(cprm + 0x111);
    m.w8(phys + 0x1075, b ? b : static_cast<uint32_t>((static_cast<int32_t>(m.u16(phys + 0x200)) + 900) / 1000));
}

// 0x003f1f78 / 0x003f1f98 / 0x00368398 on the drivetrain record (phys + 0x234)
inline bool GearKind1(M& m, uint32_t d) { return m.u8(d + 0x12) == 1; }
inline bool GearKind12(M& m, uint32_t d) { return static_cast<uint8_t>(m.u8(d + 0x12) - 1) < 2; }
inline bool GearKind3(M& m, uint32_t d) { return m.u8(d + 0x12) == 3; }

// 0x003f1fb0(d, *w, *ratio)
inline bool GearLimit(M& m, uint32_t d, uint32_t* w, uint32_t* ratio) {
    if (!GearKind1(m, d)) return false;
    const uint32_t w0 = *w;
    const uint32_t lim = Mul(U8F(m.u16(d + 0x30)), kRpm);
    if (Lt(w0, lim)) {
        *ratio = kOne;
        return true;
    }
    uint32_t r = Div(lim, w0);
    *ratio = r;
    const uint32_t floor = m.f(d + 0x40 + 4 * m.u8(d + 2));
    if (Lt(r, floor)) *ratio = floor;
    *w = Mul(*ratio, w0);
    return true;
}


// 0x0034ecf0 (defined in carparams_init.cpp): min(sum of the active torque shapes + 1, t[+0xfc] + 1).
uint32_t ShapeSumLimited(M& m, uint32_t t, int16_t a, int16_t b, uint32_t* out);

} // namespace gt4::game::carparams::guest
