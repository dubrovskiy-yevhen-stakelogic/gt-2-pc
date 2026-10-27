#include "game/physics/wheel_ground.h"

#include "ee/fpu.h"
#include "game/math/libm.h"
#include "game/math/raw.h"

namespace gt4::game {

namespace {
using namespace gt4::ee;
using raw::Ld;
using raw::St;

inline uint32_t Mul(uint32_t a, uint32_t b) { return FpuMul(a, b, nullptr); }
inline uint32_t Add(uint32_t a, uint32_t b) { return FpuAdd(a, b, nullptr); }
inline uint32_t Sub(uint32_t a, uint32_t b) { return FpuSub(a, b, nullptr); }
inline uint32_t Div(uint32_t a, uint32_t b) { return FpuDiv(a, b, nullptr); }
inline uint32_t Sqrt(uint32_t a) { return FpuSqrt(a, nullptr); }

constexpr uint32_t kOne = 0x3f800000u, kMinusOne = 0xbf800000u, kHalf = 0x3f000000u;

inline int WheelIndex(const uint8_t* entry, int64_t i) { return i < entry[1] ? entry[20 + i] : 0; }
} // namespace

uint32_t PhysAntiRoll(const uint8_t* wa, const uint8_t* wb, const uint8_t* axle) {
    const uint32_t d = Sub(Ld(wa, 148), Ld(wb, 148));
    const uint32_t k = Ld(axle, 48);
    if (FpuLe(0, d)) return Mul(Mul(k, d), d);
    return Mul(Mul(FpuNeg(k), d), d);
}

uint32_t PhysAxleScale(const uint8_t* car, const uint8_t* spec, const uint8_t* entry, int64_t i) {
    const uint32_t v = Ld(spec, 796 + 88 * WheelIndex(entry, i));
    if (spec[50] != 1) return v;   // 0x0035c7c8
    const int32_t sel = car[0x788];
    if (sel == 0) return v;
    const uint8_t pos = car[0x544];
    const int32_t a = static_cast<int32_t>(i) + pos;
    int32_t r = a - ((a < 0 ? a + 3 : a) >> 2 << 2);
    if ((pos & 7) >= 4) r = 3 - r;
    const int32_t base = 7 * r;
    if (sel < base) return v;
    int32_t f = 1 - (sel - base);
    if (f < 0) f = 0;
    return Mul(v, FpuCvtSW(static_cast<uint32_t>(f)));
}

uint32_t PhysDamper(const uint8_t* s, int64_t k, uint32_t x, const WheelGroundGlobals& gl) {
    const uint8_t* g = gl.g;
    const int kk = static_cast<int>(k);
    if (g[0x129] != 0) {
        if (FpuLt(x, 0)) {
            const uint32_t s32 = Ld(s, 32);
            if (FpuLe(x, FpuNeg(s32))) return Ld(s, 72);
            const uint32_t s24 = Ld(s, 24);
            const uint32_t nx = FpuNeg(x);
            if (FpuLe(FpuNeg(s24), x)) return Div(Mul(nx, Ld(s, 28)), s24);
            const uint32_t s28 = Ld(s, 28);
            return Add(s28, Div(Mul(Sub(nx, s24), Sub(Ld(s, 72), s28)), Sub(s32, s24)));
        }
        if (FpuLt(0, x)) {
            const uint32_t s44 = Ld(s, 44);
            if (FpuLe(s44, x)) return FpuNeg(Ld(s, 76));
            const uint32_t s36 = Ld(s, 36);
            if (FpuLe(x, s36)) return Div(Mul(FpuNeg(x), Ld(s, 40)), s36);
            const uint32_t s40 = Ld(s, 40);
            return FpuNeg(Add(s40, Div(Mul(Sub(x, s36), Sub(Ld(s, 76), s40)), Sub(s44, s36))));
        }
        return 0;
    }
    const uint32_t blend = Ld(g, 0);
    if (FpuLt(x, 0)) {
        if (FpuLt(x, kMinusOne)) x = kMinusOne;
        const uint32_t s24 = Ld(s, 24), s32 = Ld(s, 32), s28 = Ld(s, 28);
        const uint32_t f5 = Add(s28, Mul(s32, Sqrt(Sub(FpuNeg(x), s24))));
        if (blend == 0) return f5;
        const uint32_t t = Ld(g, 8 + 4 * kk);
        const uint32_t nt = FpuNeg(t);
        if (!FpuLt(nt, x)) return f5;
        const uint32_t at = Add(s28, Mul(s32, Sqrt(Sub(t, s24))));
        const uint32_t w = Ld(g, 0x10 + 4 * kk);
        const uint32_t part = Mul(Sub(kOne, w), f5);
        const uint32_t q = Div(Mul(at, Mul(x, x)), Mul(nt, nt));
        return Add(part, Mul(w, q));
    }
    if (FpuLt(kOne, x)) x = kOne;
    const uint32_t s36 = Ld(s, 36), s44 = Ld(s, 44);
    const uint32_t n40 = FpuNeg(Ld(s, 40));
    const uint32_t f5 = Sub(n40, Mul(s44, Sqrt(Sub(x, s36))));
    if (blend == 0) return f5;
    const uint32_t t = Ld(g, 0x18 + 4 * kk);
    if (!FpuLt(x, t)) return f5;
    const uint32_t at = Sub(n40, Mul(s44, Sqrt(Sub(t, s36))));
    const uint32_t w = Ld(g, 0x20 + 4 * kk);
    const uint32_t part = Mul(Sub(kOne, w), f5);
    const uint32_t q = Div(Mul(at, Mul(x, x)), Mul(t, t));
    return Add(part, Mul(w, q));
}

uint32_t PhysTyreTerm(const uint8_t* car, int64_t i, const WheelGroundGlobals& gl) {
    const uint8_t* t = gl.tyre;
    if (t[0] == 2) {
        const uint32_t v = Ld(car, 0x640), hi = Ld(t, 28);
        if (FpuLe(hi, v)) return Ld(t, 20);
        const uint32_t lo = Ld(t, 24);
        if (!FpuLt(lo, v)) return 0;
        return Div(Mul(Ld(t, 20), Sub(v, lo)), Sub(hi, lo));
    }
    if (t[0] != 3) return 0;
    const uint8_t cls = car[493 + 236 * static_cast<int32_t>(i)];
    if (gl.surface[cls] != 0) return 0;
    return Ld(t, 20);
}

uint32_t PhysTyreForce(const uint8_t* car, const uint8_t* spec, int64_t i, uint32_t x, const WheelGroundGlobals& gl) {
    const int32_t axle = static_cast<int32_t>(i) >> 1;
    const uint8_t* t = gl.tyre;
    if (t[0] == 0) return 0;
    const uint8_t* s = spec + 776 + 88 * axle;
    uint32_t stop = 0;
    const uint32_t over = Sub(x, Ld(s, 0));
    if (FpuLt(over, 0)) stop = FpuNeg(Mul(Mul(over, over), Ld(s, 52)));
    const uint32_t term = PhysTyreTerm(car, i, gl);
    const uint32_t y = Add(Sub(x, Ld(s, 16)), term);
    uint32_t force = 0;
    if (FpuLt(y, 0)) {
        const uint32_t lim = FpuNeg(Ld(t, 4));
        uint32_t k = Div(Ld(t, 8), Ld(t, 12 + 4 * axle));
        if (FpuLt(lim, y)) k = Div(Mul(k, y), lim);
        force = Mul(Mul(k, Ld(spec, 4160 + 4 * axle)), kHalf);
    }
    force = Sub(force, Mul(Ld(s, 20), term));
    return Add(force, stop);
}

void PhysWheelSpring(uint8_t* car, uint8_t* w, const uint8_t* spec, const uint8_t* s, int64_t i, int64_t k, uint32_t x,
                     uint32_t r, const WheelGroundGlobals& gl) {
    const uint32_t hiLimit = raw::Ld16(car, 0x5be) != 0 ? Ld(s, 68) : Ld(s, 8);
    const uint32_t dt = Ld(car, 0x54c), pos = Ld(w, 148);
    uint32_t f0 = Mul(Mul(Div(Add(Ld(w, 156), Ld(w, 160)), Ld(s, 56)), kHalf), dt);
    uint32_t travel = Add(pos, Mul(Add(Ld(w, 152), f0), dt));
    if (FpuLt(hiLimit, travel)) travel = hiLimit;
    else if (FpuLt(travel, Ld(s, 4))) travel = Ld(s, 4);
    if (FpuLt(travel, x)) {
        St(w, 156, Mul(FpuNeg(travel), r));
        if (FpuEq(travel, hiLimit)) {
            St(w, 152, 0);
        } else {
            const uint32_t rate = Mul(Ld(car, 0x550), Sub(travel, pos));
            St(w, 152, rate);
            if (FpuLt(kOne, rate)) St(w, 152, kOne);
            else if (FpuLt(rate, kMinusOne)) St(w, 152, kMinusOne);
        }
        const uint32_t damper = PhysDamper(s, k, Ld(w, 152), gl);
        St(w, 148, travel);
        St(w, 160, damper);
        St(w, 72, 0);
        return;
    }
    const uint32_t len = FpuLt(x, Ld(s, 4)) ? Ld(s, 4) : x;
    const uint32_t comp = Sub(len, Ld(s, 0));
    uint32_t spring = Mul(FpuNeg(len), r);
    if (FpuLt(comp, 0)) spring = Add(spring, Mul(Mul(comp, comp), Ld(s, 52)));
    uint32_t rate = Mul(Ld(car, 0x550), Sub(x, pos));
    St(w, 152, rate);
    uint32_t damper = PhysDamper(s, k, rate, gl);
    if (FpuLt(rate, 0) && FpuLt(comp, 0)) {
        if (FpuLt(rate, kMinusOne)) rate = kMinusOne;
        damper = Sub(damper, Mul(Ld(s, 64), rate));
    }
    if (spec[4214] != 0) damper = Add(damper, PhysTyreForce(car, spec, i, len, gl));
    St(w, 148, len);
    St(w, 156, spring);
    St(w, 160, damper);
    const uint32_t load = Add(Add(spring, damper), Ld(s, 60));
    St(w, 72, load);
    if (FpuLt(load, 0)) {
        St(w, 72, 0);
        St(w, 160, 0);
        St(w, 156, FpuNeg(Ld(s, 60)));
    }
}

uint32_t PhysWheelTermA(const uint8_t* spec, const uint8_t* entry, int64_t i, uint32_t a, uint32_t b) {
    const int k = WheelIndex(entry, i);
    uint32_t v = Add(Ld(spec, 4068 + 4 * k), Mul(Ld(spec, 4076 + 4 * k), b));
    if (!(i < entry[1] && static_cast<int8_t>(entry[60 + i]) > 0)) return v;
    const uint32_t g = Ld(spec, 4100);
    if (FpuEq(g, 0)) return v;
    uint32_t t = Mul(a, g);
    if (entry[36 + i] != 127) {
        const bool first = i == 0;
        if (FpuLe(0, t)) t = first ? Mul(t, Ld(spec, 268)) : FpuNeg(Mul(Ld(spec, 272), t));
        else t = first ? Mul(t, Ld(spec, 272)) : FpuNeg(Mul(Ld(spec, 268), t));
    }
    return Add(v, t);
}

uint32_t PhysWheelTermB(const uint8_t* spec, const uint8_t* entry, int64_t i, uint32_t, uint32_t b) {
    const uint8_t mode = entry[28 + i];
    if (mode == 2) return 0;
    const int j = entry[44 + i];
    const uint32_t r = Ld(spec, 4092 + 4 * j);
    uint32_t v = Ld(spec, 40 + 4 * j);
    if (!FpuEq(r, 0)) {
        const uint32_t c = LibmCos(LibmAtan2(b, r));
        v = Sub(v, Mul(Sub(kOne, c), r));
    }
    if (mode == 0) v = FpuNeg(v);
    return v;
}

bool PhysWheelGround(uint8_t* car, const uint8_t* spec, const uint8_t* entry, const WheelGroundGlobals& gl) {
    const int n = entry[1], axles = entry[2];
    if (axles > 4) return false;
    for (int i = 0; i < n; ++i)
        if (entry[36 + i] != 127 && entry[44 + i] >= axles) return false;
    for (int i = 0; i < n; ++i) {
        uint8_t* w = car + 356 + 236 * i;
        const uint8_t* p = car + 0x7b8 + 52 * i;
        const int k = entry[44 + i];
        const uint32_t f0 = Mul(Ld(spec, 40 + 4 * k), Ld(car, 0x688));
        const uint32_t f1 = Mul(Ld(spec, 16 + 4 * k), Ld(car, 0x684));
        uint32_t arm = 0;
        if (entry[28 + i] == 0) arm = Add(f0, 0);
        else if (entry[28 + i] == 1) arm = Sub(0, f0);
        const int8_t side = static_cast<int8_t>(entry[60 + i]);
        if (side > 0) arm = Add(arm, f1);
        else if (side < 0) arm = Sub(arm, f1);
        const uint32_t vx = Add(Ld(car, 0x648), Mul(Ld(car, 0x128), arm));
        const uint32_t vy = Add(Ld(car, 0x64c), Mul(Ld(car, 0x12c), arm));
        const uint32_t vz = Add(Ld(car, 0x650), Mul(Ld(car, 0x130), arm));
        St(w, 216, Add(Add(Mul(vx, Ld(p, 28)), Mul(vy, FpuNeg(Ld(p, 36)))), Mul(vz, Ld(p, 32))));
    }
    for (int i = 0; i < n; ++i) {
        uint8_t* w = car + 356 + 236 * i;
        const int k = entry[20 + i];
        const uint8_t* s = spec + 776 + 88 * k;
        const uint32_t len = Ld(car, 0x514 + 4 * i);
        int state = FpuLt(0, Ld(w, 72)) ? 1 : 0;
        PhysWheelSpring(car, w, spec, s, i, k, len, PhysAxleScale(car, spec, entry, i), gl);
        if (state == 0) {
            if (FpuLt(0, Ld(w, 72))) state = 2;
        } else if (state == 1) {
            if (FpuEq(Ld(w, 72), 0)) state = 3;
        }
        w[220] = static_cast<uint8_t>(state);
        const uint32_t travel = Sub(Ld(w, 148), Ld(s, 16));
        St(w, 8, PhysWheelTermB(spec, entry, i, Ld(car, 0x56c), travel));
        if (entry[0] != 1) {
            St(w, 4, PhysWheelTermA(spec, entry, i, Ld(car, 0x56c), travel));
        } else {
            const uint32_t c = LibmCos(Sub(Ld(car, 0x670), Ld(car, 0x67c)));
            St(w, 4, Mul(Ld(car, 0x56c), Mul(Ld(spec, 4100), c)));
        }
    }
    uint32_t roll[4] = {0, 0, 0, 0};
    for (int a = 0; a < axles; ++a) {
        const int wi = entry[12 + a];
        const int other = entry[36 + wi];
        if (other == 127) continue;
        roll[a] = PhysAntiRoll(car + 356 + 236 * wi, car + 356 + 236 * other, spec + 776 + 88 * entry[16 + a]);
    }
    const uint8_t* g = gl.g;
    uint32_t lift = 0;
    if (g[0x161] == 0) lift = Mul(Ld(spec, 52), FpuNeg(Ld(car, 0x658)));
    St(car, 0x6bc, 0);
    if (entry[0] == 1) St(car, 0x6bc, Mul(Mul(Ld(car, 0x658), Ld(car, 0x5d8)), Ld(car, 0x130)));
    const uint32_t brake = Mul(Ld(spec, 56), Ld(car, 0x668));
    for (int i = 0; i < n; ++i) {
        const uint8_t* w = car + 356 + 236 * i;
        uint32_t f = Ld(w, 156);
        if (FpuEq(Ld(w, 72), 0)) f = FpuNeg(Ld(spec, 836 + 88 * entry[20 + i]));
        if (entry[36 + i] != 127) {
            const uint32_t r = roll[entry[44 + i]];
            if (entry[28 + i] == 0) f = Sub(f, r);
            else if (entry[28 + i] == 1) f = Add(f, r);
        }
        const int8_t side = static_cast<int8_t>(entry[60 + i]);
        if (side > 0) {
            const uint8_t m = entry[4];
            const uint32_t c = FpuCvtSW(m);
            f = Sub(f, Div(Ld(car, 0x6ac), c));
            const uint32_t b = m == 2 ? brake : Mul(brake, Div(0x40000000u, c));
            f = Add(f, b);
        } else if (side < 0) {
            const uint8_t m = entry[5];
            const uint32_t c = FpuCvtSW(m);
            f = Sub(f, Div(Ld(car, 0x6b0), c));
            const uint32_t b = m == 2 ? brake : Mul(brake, Div(0x40000000u, c));
            f = Sub(f, b);
        }
        if (entry[36 + i] != 127) {
            if (g[0x161] != 0) {
                const int k = entry[20 + i];
                uint32_t h = Add(Sub(Ld(car, 0x514 + 4 * i), Ld(spec, 788 + 88 * k)), Ld(spec, 708 + 4 * k));
                h = Mul(Div(Add(Ld(spec, 4192), h), Ld(spec, 40 + 4 * k)), Ld(w, 212));
                if (entry[28 + i] == 0) f = Sub(f, h);
                else if (entry[28 + i] == 1) f = Add(f, h);
            } else {
                if (entry[28 + i] == 0) f = Add(f, lift);
                else if (entry[28 + i] == 1) f = Sub(f, lift);
            }
        }
        St(car, 0x524 + 4 * i, f);
    }
    return true;
}

} // namespace gt4::game
