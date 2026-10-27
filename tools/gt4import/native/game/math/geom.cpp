#include "game/math/geom.h"

#include "ee/fpu.h"
#include "game/math/libm.h"
#include "game/math/raw.h"
#include "game/math/trig.h"

namespace gt4::game {

namespace {
using namespace gt4::ee;
using raw::Ld;
using raw::St;

constexpr uint32_t kZero = 0, kOne = 0x3f800000u, kHalf = 0x3f000000u;
constexpr uint32_t k180 = 0x43340000u, kMinus180 = 0xc3340000u, k360 = 0x43b40000u;
constexpr uint32_t kPiLow = 0x40490fdau;          // pi rounded down, as loaded by these routines
constexpr uint32_t kRadToDeg = 0x42652ee0u;       // 57.29578

inline uint32_t Mul(uint32_t a, uint32_t b) { return FpuMul(a, b, nullptr); }
inline uint32_t Add(uint32_t a, uint32_t b) { return FpuAdd(a, b, nullptr); }
inline uint32_t Sub(uint32_t a, uint32_t b) { return FpuSub(a, b, nullptr); }
inline uint32_t Div(uint32_t a, uint32_t b) { return FpuDiv(a, b, nullptr); }

// 0x00620e30 / 0x00620e50: per-octant offset and sign for 0x0036ecd0.
constexpr uint32_t kOctantOffset[8] = {0x00000000u, 0x43340000u, 0x43b40000u, 0x43340000u,
                                       0x42b40000u, 0x42b40000u, 0x43870000u, 0x43870000u};
constexpr uint32_t kOctantSign[8] = {0x3f800000u, 0xbf800000u, 0xbf800000u, 0x3f800000u,
                                     0xbf800000u, 0x3f800000u, 0x3f800000u, 0xbf800000u};

uint32_t Interpolate(uint32_t x, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1, bool dyFirst) {
    const uint32_t dx = Sub(x, x0);
    const uint32_t span = Sub(x1, x0);
    const uint32_t dy = Sub(y1, y0);
    const uint32_t product = dyFirst ? Mul(dy, dx) : Mul(dx, dy);
    return Add(y0, Div(product, span));
}
} // namespace

uint32_t GeoWrap360(uint32_t x) {
    if (FpuLe(k360, x)) x = Sub(x, Mul(FpuCvtSW(FpuCvtWS(Div(x, k360))), k360));
    if (FpuLt(x, kZero)) {
        x = FpuNeg(x);
        x = Sub(x, Mul(FpuCvtSW(FpuCvtWS(Div(x, k360))), k360));
        x = Sub(k360, x);
    }
    return x;
}

uint32_t GeoWrap180(uint32_t x) {
    x = GeoWrap360(x);
    if (FpuLt(k180, x)) x = Sub(x, k360);
    return x;
}

uint32_t GeoAngleDiff(uint32_t a, uint32_t b) {
    uint32_t x = Sub(a, b);
    if (FpuLt(k180, x)) x = Sub(x, k360);
    if (FpuLt(x, kMinus180)) x = Add(x, k360);
    return x;
}

uint32_t GeoAngleLerp(uint32_t a, uint32_t b, uint32_t t) {
    const uint32_t wa = GeoWrap360(a);
    const uint32_t wb = GeoWrap360(b);
    return Add(wa, Mul(GeoAngleDiff(wb, wa), t));
}

uint32_t GeoHeading(const uint8_t* v) {
    const uint32_t a = Atan2(Ld(v, 0), Ld(v, 8));
    return Mul(Sub(kPiLow, a), kRadToDeg);
}

uint32_t GeoPitch(const uint8_t* v) {
    uint32_t h = Mul(Ld(v, 0), Ld(v, 0));
    h = Add(h, Mul(Ld(v, 8), Ld(v, 8)));
    h = FpuSqrt(h, nullptr);
    const uint32_t a = Atan2(Ld(v, 4), h);
    return Mul(FpuNeg(a), kRadToDeg);
}

uint32_t GeoAtan2Degrees(uint32_t y, uint32_t x) {
    const uint32_t ay = FpuAbs(y), ax = FpuAbs(x);
    unsigned octant = 0;
    uint32_t q;
    if (FpuLt(ay, ax)) {
        if (FpuEq(ax, kZero)) return kZero;
        octant = 4;
        q = Div(ay, ax);
    } else {
        if (FpuEq(ay, kZero)) return kZero;
        q = Div(ax, ay);
    }
    if (FpuLt(y, kZero)) octant |= 1;
    if (FpuLt(x, kZero)) octant |= 2;
    uint32_t a = Mul(LibmAtan(q), k180);
    a = Div(a, kPiLow);
    return Add(kOctantOffset[octant], Mul(kOctantSign[octant], a));
}

uint32_t GeoCosineOffset(uint32_t a, uint32_t b, uint32_t c) {
    const uint32_t cosine = LibmCos(Div(Mul(a, kPiLow), k180));
    const uint32_t halfB = Mul(b, kHalf);
    return Sub(a, Mul(Mul(cosine, halfB), c));
}

uint32_t GeoWrapRadiansGeneral(uint32_t x) {
    constexpr uint32_t kTwoPi = 0x40c90fdau;
    const bool negative = FpuLt(x, kZero);
    const uint32_t q = Div(Add(FpuAbs(x), kPiLow), kTwoPi);
    const uint32_t k = FpuCvtSW(FpuCvtWS(q));
    return Add(x, Mul(k, negative ? kTwoPi : 0xc0c90fdau));
}

uint32_t GeoWrapRadians(uint32_t x) {
    constexpr uint32_t kTwoPi = 0x40c90fdau;
    if (FpuLt(x, 0xc0490fdau)) {
        if (FpuLt(x, 0xc116cbe3u)) return GeoWrapRadiansGeneral(x);
        return Add(x, kTwoPi);
    }
    if (FpuLe(kPiLow, x)) {
        if (FpuLt(0x4116cbe3u, x)) return GeoWrapRadiansGeneral(x);
        return Sub(x, kTwoPi);
    }
    return x;
}

uint32_t GeoAngleDiffRadians(uint32_t a, uint32_t b) {
    uint32_t d = Sub(GeoWrapRadians(a), GeoWrapRadians(b));
    if (FpuLe(kPiLow, d)) return Sub(d, 0x40c90fdau);
    if (FpuLt(d, 0xc0490fdau)) d = Add(d, 0x40c90fdau);
    return d;
}

uint32_t GeoClampScaled(uint32_t x) {
    const uint32_t y = Mul(x, 0x40747648u);
    if (FpuLt(kOne, y)) return kOne;
    if (FpuLt(y, 0xbf800000u)) return 0xbf800000u;
    return y;
}

void GeoCross(uint8_t* out, const uint8_t* a, const uint8_t* b) {
    St(out, 0, Sub(Mul(Ld(a, 4), Ld(b, 8)), Mul(Ld(a, 8), Ld(b, 4))));
    St(out, 4, Sub(Mul(Ld(a, 8), Ld(b, 0)), Mul(Ld(a, 0), Ld(b, 8))));
    St(out, 8, Sub(Mul(Ld(a, 0), Ld(b, 4)), Mul(Ld(a, 4), Ld(b, 0))));
}

uint32_t GeoLengthSquared(const uint8_t* v) {
    uint32_t s = kZero;
    for (int k = 0; k < 3; ++k) s = Add(s, Mul(Ld(v, 4 * k), Ld(v, 4 * k)));
    return s;
}

void GeoNormalize(uint8_t* v) {
    const uint32_t length = FpuSqrt(GeoLengthSquared(v), nullptr);
    if (FpuLe(length, kZero)) return;
    for (int k = 0; k < 3; ++k) St(v, 4 * k, Div(Ld(v, 4 * k), length));
}

void GeoAddAssign(uint8_t* a, const uint8_t* b) {
    const uint32_t z = Add(Ld(a, 8), Ld(b, 8));
    const uint32_t y0 = Ld(a, 4), x0 = Ld(a, 0);
    St(a, 8, z);
    St(a, 4, Add(y0, Ld(b, 4)));
    St(a, 0, Add(x0, Ld(b, 0)));
}

void GeoSubAssign(uint8_t* a, const uint8_t* b) {
    const uint32_t z = Sub(Ld(a, 8), Ld(b, 8));
    const uint32_t y0 = Ld(a, 4), x0 = Ld(a, 0);
    St(a, 8, z);
    St(a, 4, Sub(y0, Ld(b, 4)));
    St(a, 0, Sub(x0, Ld(b, 0)));
}

uint32_t GeoCurveBinary(const GeoCurve& c, uint32_t x) {
    const uint32_t last = static_cast<uint32_t>(static_cast<int32_t>(c.count) - 1);
    auto xs = [&](uint32_t i) { return Ld(c.xs, static_cast<int>(4 * i)); };
    auto ys = [&](uint32_t i) { return Ld(c.ys, static_cast<int>(4 * i)); };
    if (FpuLe(x, xs(0))) return ys(0);
    if (FpuLe(xs(last), x)) return ys(last);
    uint32_t hi = last, lo = 0;
    if (hi >= 4) {
        uint32_t mid = 0;
        do {
            mid = (hi + lo) >> 1;
            const uint32_t m = xs(mid);
            if (FpuLt(x, m)) hi = mid;
            else {
                lo = mid;
                if (FpuEq(x, m)) return ys(mid);
            }
        } while (hi - lo >= 4);
    }
    uint32_t i = lo;
    if (i < hi && !FpuLt(x, xs(i))) {
        ++i;
        while (i < hi && !FpuLt(x, xs(i))) ++i;
    }
    return Interpolate(x, xs(i - 1), xs(i), ys(i - 1), ys(i), true);
}

uint32_t GeoCurveLinear(const GeoCurve& c, uint32_t x) {
    const uint32_t last = static_cast<uint32_t>(static_cast<int32_t>(c.count) - 1);
    auto xs = [&](uint32_t i) { return Ld(c.xs, static_cast<int>(4 * i)); };
    auto ys = [&](uint32_t i) { return Ld(c.ys, static_cast<int>(4 * i)); };
    if (FpuLe(x, xs(0))) return ys(0);
    if (FpuLe(xs(last), x)) return ys(last);
    uint32_t i = 1;
    while (i < last && !FpuLt(x, xs(i))) ++i;
    return Interpolate(x, xs(i - 1), xs(i), ys(i - 1), ys(i), false);
}

uint32_t GeoPairTable(const uint8_t* t, uint32_t x) {
    if (FpuLe(x, kZero)) x = kZero;
    if (FpuLe(kOne, x)) x = kOne;
    const int32_t n = static_cast<int32_t>(Ld(t, 0));
    int32_t lo = 0, hi = n;
    if (n > 0) {
        while (lo < hi) {
            const int32_t mid = (lo + hi) >> 1;
            if (FpuLt(x, Ld(t, 4 + 8 * mid))) hi = mid;
            else lo = mid + 1;
        }
    }
    int32_t i = lo - 1;
    if (!(i < n - 1)) i = n - 2;
    const uint8_t* p = t + 4 + 8 * i;
    const uint32_t x0 = Ld(p, 0), y0 = Ld(p, 4), x1 = Ld(p, 8), y1 = Ld(p, 12);
    const uint32_t dx = Sub(x, x0);
    const uint32_t dy = Sub(y1, y0);
    const uint32_t span = Sub(x1, x0);
    return Add(Div(Mul(dy, dx), span), y0);
}

uint32_t GeoPiecewise(const uint8_t* p, uint32_t x) {
    uint32_t x0 = kZero, y0 = kZero;
    uint32_t x1 = Ld(p, 0), y1 = Ld(p, 8);
    if (FpuLe(x, kZero)) x = kZero;
    if (FpuLe(kOne, x)) x = kOne;
    uint32_t next;
    if (FpuLt(x1, x)) {
        x0 = x1;
        x1 = Ld(p, 4);
        y0 = y1;
        y1 = Ld(p, 12);
        next = x1;
    } else {
        next = Ld(p, 4);
    }
    if (FpuLt(next, x)) {
        x0 = x1;
        x1 = kOne;
        y0 = y1;
        y1 = kOne;
    }
    if (FpuLe(x1, x0)) return kZero;
    return Add(Div(Mul(Sub(y1, y0), Sub(x, x0)), Sub(x1, x0)), y0);
}

void GeoHermiteSetup(uint8_t* h, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3) {
    const uint32_t c = Div(p1, p0);
    St(h, 0, p0);
    St(h, 4, p1);
    St(h, 8, p2);
    const uint32_t r = Div(p3, p2);
    St(h, 12, p3);
    uint32_t b = Mul(c, 0xc0000000u);
    St(h, 24, c);
    b = Sub(b, r);
    uint32_t a = Add(c, r);
    b = Add(b, 0x40400000u);
    a = Sub(a, 0x40000000u);
    St(h, 20, b);
    St(h, 16, a);
}

uint32_t GeoHermiteEval(const uint8_t* h, uint32_t t) {
    if (FpuLe(t, kZero)) t = kZero;
    if (FpuLe(kOne, t)) t = kOne;
    uint32_t r = Add(Mul(Ld(h, 16), t), Ld(h, 20));
    r = Add(Mul(r, t), Ld(h, 24));
    return Mul(r, t);
}

uint32_t GeoHermiteRemap(const uint8_t* obj, uint32_t x) {
    x = Div(x, Ld(obj, 36));
    if (FpuLe(x, kZero)) x = kZero;
    if (FpuLe(kOne, x)) x = kOne;
    uint32_t r = GeoHermiteEval(obj + 8, x);
    if (FpuLe(r, kZero)) r = kZero;
    if (FpuLe(kOne, r)) r = kOne;
    const uint32_t lo = Ld(obj, 4);
    return Add(lo, Mul(Sub(Ld(obj, 0), lo), r));
}

uint64_t GeoSaturatingIncrement(uint64_t v) {
    const uint64_t next = raw::Sext32(static_cast<uint32_t>(v) + 1);
    return static_cast<int64_t>(v) > 0x7ffffffe ? v : next;
}

uint32_t GeoPercentOrReciprocal(uint64_t v) {
    const uint32_t lo = static_cast<uint32_t>(v);
    if (static_cast<int64_t>(v) < 0) return Div(kOne, FpuCvtSW(0u - lo));
    if (v == 0) return kOne;
    return Div(FpuCvtSW(lo), 0x42c80000u);
}

uint32_t GeoRampCounter(uint8_t* counter, uint64_t dir, uint32_t x) {
    if (dir == 0) {
        counter[0] = 0;
        return kZero;
    }
    int64_t cur = static_cast<int8_t>(counter[0]);
    const int64_t max = static_cast<int8_t>(counter[1]);
    const uint32_t maxF = FpuCvtSW(static_cast<uint32_t>(max));
    if (cur < max) {
        counter[0] = static_cast<uint8_t>(cur + 1);
        cur = cur + 1;
    }
    const uint32_t curF = FpuCvtSW(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(cur))));
    uint32_t r = Div(Mul(x, curF), maxF);
    if (static_cast<int64_t>(dir) < 0) r = FpuNeg(r);
    return r;
}

void GeoCopy64(uint8_t* dst, const uint8_t* src) {
    for (int k = 0; k < 64; ++k) St(dst, 4 * k, Ld(src, 4 * k));
}

void GeoQuatRotate(const uint8_t* q, uint8_t* out, const uint8_t* v) {
    const auto mul = [](uint32_t a, uint32_t b) { return FpuMul(a, b, nullptr); };
    const auto add = [](uint32_t a, uint32_t b) { return FpuAdd(a, b, nullptr); };
    const auto sub = [](uint32_t a, uint32_t b) { return FpuSub(a, b, nullptr); };
    const uint32_t x = Ld(q, 0), y = Ld(q, 4), z = Ld(q, 8), w = Ld(q, 12);
    const uint32_t yy = mul(y, y), xy = mul(x, y), zz = mul(z, z), zw = mul(z, w), yw = mul(y, w), xz = mul(x, z);
    const uint32_t xx = mul(x, x), xw = mul(x, w), yz = mul(y, z);
    const uint32_t r00 = sub(sub(kHalf, yy), zz), r01 = sub(xy, zw), r02 = add(xz, yw);
    const uint32_t r10 = add(xy, zw), r11 = sub(sub(kHalf, zz), xx), r12 = sub(yz, xw);
    const uint32_t r20 = sub(xz, yw), r21 = add(yz, xw), r22 = sub(sub(kHalf, xx), yy);
    const auto row = [&](uint32_t a, uint32_t b, uint32_t c) {
        const uint32_t s = add(add(mul(a, Ld(v, 0)), mul(b, Ld(v, 4))), mul(c, Ld(v, 8)));
        return add(s, s);
    };
    St(out, 0, row(r00, r01, r02));
    St(out, 4, row(r10, r11, r12));
    St(out, 8, row(r20, r21, r22));
}

} // namespace gt4::game
