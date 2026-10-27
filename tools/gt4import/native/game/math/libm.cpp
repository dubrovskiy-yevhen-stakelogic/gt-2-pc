#include "game/math/libm.h"

#include "ee/fpu.h"

namespace gt4::game {

namespace {

using namespace gt4::ee;

constexpr uint32_t kZero = 0u, kOne = 0x3f800000u, kSign = 0x80000000u;
constexpr uint32_t kPiOver2 = 0x3fc90fdbu, kPiOver4 = 0x3f490fdbu, kPi = 0x40490fdbu, kInvPi = 0x3ea2f983u;
// sin(u) polynomial shared by 0x0057d1f0 / 0x0057d2b8 / 0x0057d8a0
constexpr uint32_t kS3 = 0xbe2aaaa4u, kS5 = 0x3c08873eu, kS7 = 0xb94fb21fu, kS9 = 0x362e9c14u;

inline uint32_t Mul(uint32_t a, uint32_t b) { return FpuMul(a, b, nullptr); }
inline uint32_t Add(uint32_t a, uint32_t b) { return FpuAdd(a, b, nullptr); }
inline uint32_t Sub(uint32_t a, uint32_t b) { return FpuSub(a, b, nullptr); }
inline uint32_t Madd(uint32_t acc, uint32_t a, uint32_t b) { return FpuMadd(acc, a, b, nullptr); }
inline uint32_t Msub(uint32_t acc, uint32_t a, uint32_t b) { return FpuMsub(acc, a, b, nullptr); }
inline uint32_t Div(uint32_t a, uint32_t b) { return FpuDiv(a, b, nullptr); }
inline uint32_t SignBit(uint32_t x) { return x >> 31; }

// u (already signed) -> u + kS3 u^3 + kS5 u^5 + kS7 u^7 + kS9 u^9 in the original's operation order.
// `square` is the unsigned u*u computed before the sign was applied.
uint32_t SinPoly(uint32_t u, uint32_t square) {
    const uint32_t u4 = Mul(square, square);
    const uint32_t u3 = Mul(square, u);
    uint32_t acc = Add(u, kZero);         // adda.s u, 0
    const uint32_t u5 = Mul(u4, u);
    const uint32_t u7 = Mul(u4, u3);
    acc = Madd(acc, kS3, u3);
    const uint32_t u9 = Mul(u5, u4);
    acc = Madd(acc, kS5, u5);
    acc = Madd(acc, kS7, u7);
    return Madd(acc, kS9, u9);
}

} // namespace

uint32_t LibmCos(uint32_t x) {
    const uint32_t k = FpuCvtWS(Mul(x, kInvPi));
    const uint32_t negative = SignBit(x);
    const uint32_t pi = Add(kPiOver2, kPiOver2);
    const uint32_t acc = Sub(x, kPiOver2);                 // suba.s x, pi/2
    const uint32_t parity = ~k & 1;
    const uint32_t kk = k - negative;
    const uint32_t sign = (((negative ^ parity) & 1) << 31) | kOne;
    const uint32_t u = Msub(acc, FpuCvtSW(kk), pi);
    return SinPoly(Mul(u, sign), Mul(u, u));
}

uint32_t LibmSin(uint32_t x) {
    const uint32_t shifted = Add(x, kPiOver2);
    const uint32_t pi = Add(kPiOver2, kPiOver2);
    const uint32_t negative = SignBit(shifted);
    const uint32_t k = FpuCvtWS(Mul(shifted, kInvPi));
    const uint32_t acc = Add(x, kZero);                    // adda.s x, 0
    const uint32_t parity = k & 1;
    const uint32_t kk = k - negative;
    const uint32_t sign = (((negative ^ parity) & 1) << 31) | kOne;
    const uint32_t u = Msub(acc, FpuCvtSW(kk), pi);
    return SinPoly(Mul(u, sign), Mul(u, u));
}

void LibmSinCos(uint32_t x, uint32_t* out0, uint32_t* out1) {
    const uint32_t mask = (x & kSign) ? 0xffffffffu : 0u;
    const uint32_t f13 = Add(x, kPi & mask);
    const uint32_t k = FpuCvtWS(Mul(x, kInvPi));
    const uint32_t kf = FpuCvtSW(k);
    uint32_t acc = Add(f13, kZero);
    uint32_t f14 = Msub(acc, kf, kPi);
    acc = Sub(kPi, f13);
    const uint32_t f4 = Madd(acc, kf, kPi);
    const uint32_t sign = (((mask ^ k) & 1) << 31) | kOne;
    acc = Sub(f13, kPiOver2);
    const uint32_t a = Msub(acc, kf, kPi);
    const uint32_t negSign = FpuNeg(sign);
    f14 = FpuMin(f14, f4);
    *out1 = SinPoly(Mul(a, negSign), Mul(a, a));
    *out0 = SinPoly(Mul(f14, sign), Mul(f14, f14));
}

uint32_t LibmAtan(uint32_t x) {
    int32_t mode = 0;
    uint32_t t = FpuAbs(x);
    const uint32_t reciprocal = Div(kOne, t);
    const uint32_t reduced = Div(Sub(kOne, t), Add(kOne, t));
    if (FpuLt(0x401a827au, t)) {          // |x| > tan(3pi/8)
        t = reciprocal;
        mode = 1;
    } else if (FpuLt(0x3ed413cdu, t)) {   // |x| > tan(pi/8)
        t = reduced;
        mode = -65536;                     // lui a0, 0xffff
    }
    const uint32_t t2 = Mul(t, t);
    uint32_t acc = Add(kOne, kZero);
    const uint32_t t4 = Mul(t2, t2);
    const uint32_t t3 = Mul(t2, t);
    acc = Madd(acc, 0x3f8e38e4u, t2);
    const uint32_t denominator = Madd(acc, 0x3e73cf3du, t4);
    acc = Add(t, kZero);
    const uint32_t t5 = Mul(t3, t2);
    const uint32_t inverse = Div(kOne, denominator);
    acc = Madd(acc, 0x3f471c72u, t3);
    uint32_t r = Madd(acc, 0x3d8ab356u, t5);
    r = Mul(r, inverse);
    if (mode < 0) r = Sub(kPiOver4, r);
    else if (mode > 0) r = Sub(kPiOver2, r);
    if (x & kSign) r = FpuNeg(r);
    return r;
}

uint32_t LibmAtan2(uint32_t y, uint32_t x) {
    if (FpuEq(x, kZero) && FpuEq(y, kZero)) return kZero;
    unsigned octant = 0;
    if (FpuLt(y, kZero)) { y = FpuNeg(y); octant = 4; }
    if (FpuLt(x, kZero)) { x = FpuNeg(x); octant |= 2; }
    uint32_t q;
    if (FpuLt(y, x)) {
        q = Div(y, x);
    } else {
        octant |= 1;
        q = Div(x, y);
    }
    uint32_t r = LibmAtan(q);
    if (octant & 1) r = Sub(0x3fc90fdau, r);
    if (octant & 2) r = Sub(0x40490fdau, r);
    if (octant & 4) r = FpuNeg(r);
    return r;
}

uint32_t LibmTan(uint32_t x) {
    uint32_t odd = 0;
    if (FpuLt(kPiOver4, FpuAbs(x))) {
        const uint32_t half = (x & kSign) ? 0xbf000000u : 0x3f000000u;
        const uint32_t k = FpuCvtWS(Add(Mul(x, 0x3f22f983u), half));
        odd = k & 1;
        x = Sub(x, Mul(FpuCvtSW(k), kPiOver2));
    }
    const uint32_t x2 = Mul(x, x);
    const uint32_t zero = kZero;   // sub.s $f3,$f3,$f3: +0 for every pattern under the FPU model
    const uint32_t x4 = Mul(x2, x2);
    const uint32_t x3 = Mul(x2, x);
    uint32_t acc = Add(kOne, zero);
    acc = Madd(acc, 0xbee38e39u, x2);
    const uint32_t x5 = Mul(x4, x);
    const uint32_t cosine = Madd(acc, 0x3c820821u, x4);
    acc = Add(x, zero);
    acc = Madd(acc, 0xbde38e39u, x3);
    const uint32_t sine = Madd(acc, 0x3a8ab356u, x5);
    if (odd) return Div(FpuNeg(cosine), sine);
    return Div(sine, cosine);
}

uint32_t LibmExp(uint32_t x) {
    const uint32_t k = FpuCvtWS(Mul(x, 0x3fb8aa3bu));
    const uint32_t biased = k + 127;
    const uint32_t r = Sub(x, Mul(FpuCvtSW(k), 0x3f317217u));
    uint32_t f1 = Mul(r, r);
    uint32_t f6 = Add(r, kOne);
    uint32_t f3 = Mul(f1, 0x3f000000u);
    uint32_t f12 = Mul(r, f1);
    uint32_t f2 = Mul(f1, f1);
    f6 = Add(f6, f3);
    const uint32_t f4 = Mul(f12, 0x3e2aaaaau);
    const uint32_t f5 = Mul(f2, 0x3d2aaaaau);
    f1 = Mul(f1, f12);
    f3 = Mul(f12, f12);
    f6 = Add(f6, f4);
    f12 = Mul(f12, f2);
    f1 = Mul(f1, 0x3c088888u);
    f3 = Mul(f3, 0x3ab60b60u);
    f6 = Add(f6, f5);
    f12 = Mul(f12, 0x39500d00u);
    f2 = Mul(f2, f2);
    f6 = Add(f6, f1);
    f2 = Mul(f2, 0x37d00d00u);
    f6 = Add(f6, f3);
    f6 = Add(f6, f12);
    f6 = Add(f6, f2);
    if (static_cast<int32_t>(biased) < 0) return kZero;
    if (static_cast<int32_t>(biased) >= 256) return 0x7fffffffu;   // word at 0x006cc938
    return Mul(f6, biased << 23);
}

uint32_t LibmLog(uint32_t x) {
    if (!FpuLt(kZero, x)) return 0xffffffffu;   // word at 0x006cc93c
    const uint32_t m = (x & 0x7fffffu) | kOne;
    uint32_t f2 = Mul(m, m);
    uint32_t f4 = Mul(m, 0x443b8000u);
    uint32_t f5 = Mul(m, 0x44cb2000u);
    const uint32_t m3 = Mul(m, f2);
    const uint32_t f9 = Mul(f2, 0x453b8000u);
    const int32_t exponent = (static_cast<int32_t>(x) >> 23) - 127;
    uint32_t f1 = Add(f4, 0x41f00000u);
    uint32_t f0 = Sub(0xc3090000u, f5);
    f5 = Mul(f2, 0x44fa0000u);
    f4 = Mul(f2, f2);
    const uint32_t f6 = Mul(m3, 0x453b8000u);
    const uint32_t f7 = Mul(m3, 0x44fa0000u);
    f1 = Add(f1, f9);
    f0 = Sub(f0, f5);
    f2 = Mul(f2, m3);
    const uint32_t f10 = Mul(f4, 0x443b8000u);
    f1 = Add(f1, f6);
    f4 = Mul(f4, 0x44cb2000u);
    f0 = Add(f0, f7);
    const uint32_t f8 = Mul(f2, 0x41f00000u);
    f1 = Add(f1, f10);
    f2 = Mul(f2, 0x43090000u);
    f0 = Add(f0, f4);
    uint32_t f3 = FpuCvtSW(static_cast<uint32_t>(exponent));
    f1 = Add(f1, f8);
    f0 = Add(f0, f2);
    f3 = Mul(f3, 0x3f317217u);
    f0 = Div(f0, f1);
    return Add(f3, f0);
}

uint32_t LibmPow(uint32_t x, uint32_t y) { return LibmExp(Mul(y, LibmLog(x))); }

uint32_t LibmFabs(uint32_t x) { return x & 0x7fffffffu; }

uint32_t LibmSqrtSoft(uint32_t x) {
    if ((x & 0x7fffffffu) <= 0x007fffffu) return x;
    if (static_cast<int32_t>(x) < 0) return Div(Sub(x, x), Sub(x, x));
    int32_t m = (static_cast<int32_t>(x) >> 23) - 127;
    int32_t ix = static_cast<int32_t>((x & 0x007fffffu) | 0x00800000u);
    ix <<= (m & 1);
    m >>= 1;
    ix <<= 1;
    int32_t q = 0, s = 0;
    uint32_t r = 0x01000000u;
    while (r != 0) {
        const int32_t t = s + static_cast<int32_t>(r);
        if (!(ix < t)) {
            ix -= t;
            s = t + static_cast<int32_t>(r);
            q += static_cast<int32_t>(r);
        }
        r >>= 1;
        ix <<= 1;
    }
    if (ix != 0) q += q & 1;
    return 0x3f000000u + static_cast<uint32_t>(q >> 1) + (static_cast<uint32_t>(m) << 23);
}

namespace {
// R(t) = p(t) / q(t) pieces of the arcsine approximation (0x0059e890), original operation order.
void AsinPoly(uint32_t t, uint32_t* p, uint32_t* q) {
    uint32_t a = Add(Mul(t, 0x3811ef08u), 0x3a4f7f04u);
    a = Add(Mul(t, a), 0xbd241146u);
    a = Add(Mul(t, a), 0x3e4e0aa8u);
    a = Add(Mul(t, a), 0xbea6b08fu);
    a = Add(Mul(t, a), 0x3e2aaaaau);
    *p = Mul(t, a);
    uint32_t b = Add(Mul(t, 0x3d9dc62du), 0xbf303360u);
    b = Add(Mul(t, b), 0x4001572cu);
    b = Add(Mul(t, b), 0xc019d138u);
    *q = Add(Mul(t, b), kOne);
}

uint32_t AsinLarge(uint32_t x, uint32_t ix) {
    constexpr uint32_t kPio2Hi = 0x3fc90fdau, kPio2Lo = 0x33a22168u, kPio4Hi = 0x3f490fdau;
    const uint32_t t = Mul(Sub(kOne, LibmFabs(x)), 0x3f000000u);
    uint32_t p, q;
    AsinPoly(t, &p, &q);
    const uint32_t s = LibmSqrtSoft(t);
    uint32_t r;
    if (static_cast<int32_t>(ix) > 0x3f799999) {
        const uint32_t w = Div(p, q);
        r = Sub(kPio2Hi, Sub(Add(Add(s, Mul(s, w)), Add(s, Mul(s, w))), kPio2Lo));
    } else {
        const uint32_t df = s & 0xfffff000u;
        const uint32_t c = Div(Sub(t, Mul(df, df)), Add(s, df));
        const uint32_t w = Div(p, q);
        const uint32_t pp = Sub(Mul(Add(s, s), w), Sub(kPio2Lo, Add(c, c)));
        const uint32_t qq = Sub(kPio4Hi, Add(df, df));
        r = Sub(kPio4Hi, Sub(pp, qq));
    }
    return static_cast<int32_t>(x) > 0 ? r : (r ^ kSign);
}
} // namespace

uint32_t LibmAsin(uint32_t x) {
    const uint32_t ix = x & 0x7fffffffu;
    if (ix == 0x3f800000u) return Add(Mul(x, 0x3fc90fdau), Mul(x, 0x33a22168u));
    if (static_cast<int32_t>(ix) > 0x3f800000) return Div(Sub(x, x), Sub(x, x));
    if (static_cast<int32_t>(ix) > 0x3effffff) return AsinLarge(x, ix);
    if (!(static_cast<int32_t>(ix) > 0x31ffffff)) {
        if (FpuLt(kOne, Add(x, 0x7149f2c9u))) return x;
        return AsinLarge(x, ix);
    }
    const uint32_t t = Mul(x, x);
    uint32_t p, q;
    AsinPoly(t, &p, &q);
    return Add(x, Mul(x, Div(p, q)));
}

} // namespace gt4::game
