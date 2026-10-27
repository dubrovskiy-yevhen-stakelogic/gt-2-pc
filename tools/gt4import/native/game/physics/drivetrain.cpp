#include "game/physics/drivetrain.h"

#include "ee/fpu.h"
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
inline uint32_t I2F(int32_t v) { return FpuCvtSW(static_cast<uint32_t>(v)); }

constexpr uint32_t kOne = 0x3f800000u, kHalf = 0x3f000000u, kSixtieth = 0x3c888888u;
} // namespace

uint32_t PhysSoftLimit(const uint8_t* p, uint32_t x, uint32_t w) {
    if (p[0xdb] == 0) return x;
    const uint32_t f4 = Ld(p, 0xe0), f3 = Ld(p, 0xdc);
    const uint32_t f5 = Mul(x, w);
    const uint32_t knee = Mul(f3, Sub(kOne, f4));
    if (FpuLe(f5, knee)) return x;
    const bool saturated = FpuLe(f3, f5);
    uint32_t f2 = Mul(f3, Sub(kOne, Mul(f4, kHalf)));
    if (!saturated) {
        uint32_t d = Sub(f3, f5);
        d = Mul(d, d);
        const uint32_t den = Mul(Add(f4, f4), f3);
        f2 = Sub(f2, Div(d, den));
    }
    f2 = Div(f2, w);
    return FpuLt(f2, x) ? f2 : x;
}

uint32_t PhysRevLimiter(uint8_t* car, const uint8_t* spec, uint32_t throttle) {
    const uint8_t* limiter = spec + 0x134;
    const int64_t limit = raw::Ld16(limiter, 0xcc);
    const int64_t rpm = static_cast<int16_t>(raw::Ld16(car, 0x5fe));
    if (car[0x625] == 0) {
        if (!(rpm < limit)) car[0x625] = 1;
    } else {
        if (rpm < static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(limit) - 500))) car[0x625] = 0;
    }
    const uint32_t base = Ld(car, 0x5f0);
    uint32_t f0 = 0;
    if (car[0x625] == 0) f0 = Mul(throttle, Add(Ld(car, 0x5ec), base));
    return PhysSoftLimit(limiter, Sub(f0, base), Ld(car, 0x600));
}

uint32_t PhysDriveSelect(const uint8_t* car, const uint8_t* spec) {
    const uint8_t* mode = spec + 0x234;
    switch (mode[0]) {
    case 0: case 4: case 5: case 6: case 7: return Ld(car, 0x634);
    case 1: case 2: case 9: return Ld(car, 0x630);
    case 3: case 10: return Mul(Add(Ld(car, 0x630), Ld(car, 0x634)), kHalf);
    case 8: {
        const uint32_t r = Ld(car, 0x634);
        return Add(r, Mul(Ld(mode, 4), Sub(Ld(car, 0x630), r)));
    }
    default: return 0;
    }
}

void PhysFollower(uint8_t* p) {
    uint32_t a = Sub(Ld(p, 0x588), Ld(p, 0x63c));
    uint32_t v = Ld(p, 0x640);
    a = Mul(a, 0x43fa0000u);
    a = Sub(a, Mul(v, 0x41a00000u));
    a = Mul(a, kSixtieth);
    v = Add(v, a);
    St(p, 0x640, v);
    St(p, 0x63c, Add(Ld(p, 0x63c), Mul(v, kSixtieth)));
}

void PhysMassWithFuel(uint8_t* car, const uint8_t* spec) {
    const uint32_t fuel = Mul(Ld(car, 0x5d4), 0x3f3d70a3u);
    St(car, 0x5d8, Add(Ld(spec, 0x48), fuel));
    St(car, 0x5dc, Add(Ld(spec, 0x4c), fuel));
}

uint32_t PhysResponseCurve(const uint8_t* c, uint8_t* slopeOut, uint32_t x) {
    uint32_t ax = FpuLt(x, 0) ? FpuNeg(x) : x;
    const int32_t mode = static_cast<int32_t>(Ld(c, 0));
    uint32_t y, slope = kOne;
    bool hasSlope = true;
    if (mode == 2) {
        ax = Sub(ax, Ld(c, 4));
        uint32_t root = FpuSqrt(ax, nullptr);
        const uint32_t k = Ld(c, 12);
        y = Add(Mul(k, root), Ld(c, 8));
        if (!slopeOut) hasSlope = false;
        else {
            if (FpuLt(root, 0x3c23d70au)) root = 0x3c23d70au;
            slope = Div(k, Add(root, root));
        }
    } else if (mode == 1) {
        const uint32_t k = Ld(c, 12);
        ax = Sub(ax, Ld(c, 4));
        y = Add(Mul(Mul(k, ax), ax), Ld(c, 8));
        slope = Mul(Add(k, k), ax);
    } else if (mode == 3) {
        const uint32_t sq = Mul(ax, ax);
        const uint32_t k = Ld(c, 12), b = Ld(c, 8);
        const uint32_t lin = Mul(b, ax);
        const uint32_t q = Mul(sq, sq);
        const uint32_t cube = Mul(ax, sq);
        y = Add(Mul(Mul(k, q), q), lin);
        slope = Add(Mul(Mul(Mul(k, 0x41000000u), q), cube), b);
    } else {
        y = ax;
    }
    if (slopeOut && hasSlope) St(slopeOut, 0, slope);
    if (FpuLt(x, 0)) y = FpuNeg(y);
    return y;
}

void PhysApproach(uint8_t* value, uint32_t target, uint32_t step) {
    uint32_t v = Ld(value, 0);
    if (FpuLt(v, target)) {
        v = Add(v, step);
        if (FpuLt(target, v)) v = target;
    } else if (FpuLt(target, v)) {
        v = Sub(v, step);
        if (FpuLt(v, target)) v = target;
    }
    St(value, 0, v);
}

uint32_t PhysQuadraticTerm(const uint8_t* obj, uint32_t x, uint8_t flagByte) {
    if (flagByte == 0) return Mul(Ld(obj, 0xd0), x);
    const int32_t i = static_cast<int16_t>(raw::Ld16(obj, 0));
    uint32_t s = Mul(x, x);
    uint32_t v = Ld(obj, 8 + 4 * i);
    v = Mul(v, v);
    s = Add(s, Mul(v, 0x3de38e38u));
    return Div(Mul(Ld(obj, 0xd0), s), v);
}

uint32_t PhysFoldAngle(uint8_t* signOut, uint32_t x) {
    uint32_t sign = kOne, r;
    if (FpuLt(x, 0xbfc90fdau)) {
        r = Add(x, 0x40490fdau);
    } else if (FpuLt(x, 0)) {
        sign = 0xbf800000u;
        r = FpuNeg(x);
    } else if (FpuLt(x, 0x3fc90fdau)) {
        r = x;
    } else {
        sign = 0xbf800000u;
        r = Sub(0x40490fdau, x);
    }
    St(signOut, 0, sign);
    return r;
}

uint32_t PhysWheelPointAxis(const uint8_t* car, uint64_t wheel, uint64_t axis, uint32_t d) {
    const uint32_t a = 4 * static_cast<uint32_t>(axis), w = 236 * static_cast<uint32_t>(wheel);
    const uint32_t up = Mul(Ld(car, static_cast<int>(296 + a)), d);
    const uint32_t s = Add(Ld(car, static_cast<int>(260 + a)), Ld(car, static_cast<int>(368 + w + a)));
    return Add(s, up);
}

void PhysStandstillFlag(uint8_t* p) {
    if (p[0x4bc] != 0 || raw::Ld16(p, 0x4ba) != 0 || p[0x467] == 8) {
        St(p, 0x514, 0);
        return;
    }
    const uint32_t tenth = 0x3dccccccu;
    if (p[0x522] == 0) {
        const bool slow = FpuLt(Ld(p, 0x48c), tenth);
        St(p, 0x514, 0);
        if (slow) St(p, 0x514, kOne);
        return;
    }
    const bool moving = FpuLt(tenth, Ld(p, 0x48c));
    St(p, 0x514, kOne);
    if (!moving) return;
    const uint32_t v = Ld(p, 0x53c);
    if (!FpuLt(v, 0x3e8e38e3u)) return;
    if (FpuLt(0xbe8e38e3u, v)) St(p, 0x514, 0);
}

void PhysLoadByteConstants(uint8_t* out, uint8_t* g) {
    if (g[0xcc] != 0) return;
    const uint32_t milli = 0x3a83126eu;
    auto big = [&](int k) { return I2F(static_cast<int32_t>(static_cast<uint32_t>(g[k]) * 100000u)); };
    St(out, 4, Mul(I2F(g[0xd1]), milli));
    St(out, 0, FpuNeg(big(0xd0)));
    const uint32_t f8 = big(0xd2);
    St(out, 8, f8);
    const uint32_t f12 = big(0xd3);
    St(out, 12, f12);
    St(out, 12, Add(f12, f8));
    St(out, 16, Mul(I2F(g[0xd4]), milli));
    St(out, 24, Mul(I2F(g[0xd6]), milli));
    St(out, 20, Mul(FpuNeg(big(0xd5)), kHalf));
    St(out, 28, big(0xd7));
    St(out, 32, big(0xd8));
    g[0xcc] = 1;
}

} // namespace gt4::game
