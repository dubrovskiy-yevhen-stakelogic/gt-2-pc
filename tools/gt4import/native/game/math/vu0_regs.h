#pragma once
// Native image of the VU0 register state used by the core's VU0 macro/micro library (decision D2): the current
// matrix of 0x004a7xxx lives in vf28..vf31, the angle helpers leave results in vf1, the micro routines use
// vf1..vf6, ACC, I and vi15. Lane helpers mirror one VU instruction each (operand order fs, ft kept, results
// computed from the pre-instruction values, `mask` bit 0 = x .. bit 3 = w) on src/ee/vu_float.
// MAC/status/clip flags are not modelled: none of the ported functions reads them.
#include <cstdint>

#include "ee/vu_float.h"

namespace gt4::game {

struct Vec4 {
    uint32_t f[4] = {0, 0, 0, 0};
    bool operator==(const Vec4&) const = default;
};

struct Vu0Regs {
    Vec4 vf[32];   // vf0 must hold (0, 0, 0, 1.0)
    Vec4 acc;
    uint16_t vi[16] = {};
    uint32_t i = 0;
};

namespace vu {

constexpr unsigned X = 1, Y = 2, Z = 4, W = 8, XYZ = 7, XYZW = 15;
enum Field { kX = 0, kY = 1, kZ = 2, kW = 3 };

template <typename Op> inline void Lanes(Vec4& d, unsigned mask, Op op) {
    Vec4 r = d;
    for (int k = 0; k < 4; ++k)
        if (mask & (1u << k)) r.f[k] = op(k);
    d = r;
}

// d = s * t (vector) / s * scalar (bc, I, Q)
inline void Mul(Vec4& d, const Vec4& s, const Vec4& t, unsigned m) {
    Lanes(d, m, [&](int k) { return ee::VuMul(s.f[k], t.f[k], nullptr); });
}
inline void MulS(Vec4& d, const Vec4& s, uint32_t t, unsigned m) {
    Lanes(d, m, [&](int k) { return ee::VuMul(s.f[k], t, nullptr); });
}
inline void Add(Vec4& d, const Vec4& s, const Vec4& t, unsigned m) {
    Lanes(d, m, [&](int k) { return ee::VuAdd(s.f[k], t.f[k], nullptr); });
}
inline void AddS(Vec4& d, const Vec4& s, uint32_t t, unsigned m) {
    Lanes(d, m, [&](int k) { return ee::VuAdd(s.f[k], t, nullptr); });
}
inline void Sub(Vec4& d, const Vec4& s, const Vec4& t, unsigned m) {
    Lanes(d, m, [&](int k) { return ee::VuSub(s.f[k], t.f[k], nullptr); });
}
// d = acc + s * t / d = acc - s * t (scalar t)
inline void MaddS(Vec4& d, const Vec4& acc, const Vec4& s, uint32_t t, unsigned m) {
    Lanes(d, m, [&](int k) { return ee::VuMadd(acc.f[k], s.f[k], t, nullptr); });
}
inline void MsubS(Vec4& d, const Vec4& acc, const Vec4& s, uint32_t t, unsigned m) {
    Lanes(d, m, [&](int k) { return ee::VuMsub(acc.f[k], s.f[k], t, nullptr); });
}
// OPMULA: acc.xyz = s.yzx * t.zxy ; OPMSUB: d.xyz = acc.xyz - s.yzx * t.zxy
inline void OpMula(Vec4& acc, const Vec4& s, const Vec4& t) {
    Lanes(acc, XYZ, [&](int k) { return ee::VuMul(s.f[(k + 1) % 3], t.f[(k + 2) % 3], nullptr); });
}
inline void OpMsub(Vec4& d, const Vec4& acc, const Vec4& s, const Vec4& t) {
    Lanes(d, XYZ, [&](int k) { return ee::VuMsub(acc.f[k], s.f[(k + 1) % 3], t.f[(k + 2) % 3], nullptr); });
}
inline void Itof(Vec4& d, const Vec4& s, int bits, unsigned m) {
    Lanes(d, m, [&](int k) { return ee::VuItof(s.f[k], bits); });
}
inline void Move(Vec4& d, const Vec4& s, unsigned m) {
    Lanes(d, m, [&](int k) { return s.f[k]; });
}
inline void Mr32(Vec4& d, const Vec4& s, unsigned m) {
    Lanes(d, m, [&](int k) { return s.f[(k + 1) & 3]; });
}

} // namespace vu
} // namespace gt4::game
