#include "game/math/trig.h"

#include "ee/fpu.h"
#include "ee/vu_float.h"

namespace gt4::game {

namespace {

using namespace gt4::ee;

constexpr uint32_t kZero = 0x00000000u;
constexpr uint32_t kOne = 0x3f800000u;
constexpr uint32_t kMinusOne = 0xbf800000u;
constexpr uint32_t kHalf = 0x3f000000u;
constexpr uint32_t kSign = 0x80000000u;
constexpr uint32_t kPiOver4 = 0x3f490fdbu;
constexpr uint32_t kTwoOverPi = 0x3f22f983u;
constexpr uint32_t kPiOver2 = 0x3fc90fdbu;   // sin path, loaded into vf3.x

// 0x006b0060 (sin) and 0x006b0070 (cos): polynomial coefficients loaded by LQC2.
constexpr uint32_t kSinPoly[4] = {0x3da335e3u, 0xbf255de7u, 0xbb996966u, 0x39283c1au};
constexpr uint32_t kCosPoly[4] = {0xbcaae9e4u, 0xbf9de9e6u, 0x3e81e0f8u, 0x3a70fa83u};

// 0x006b0018 / 0x006b0038: per-octant sign and offset for atan2.
constexpr uint32_t kAtanSign[8] = {0x3f800000u, 0xbf800000u, 0xbf800000u, 0x3f800000u,
                                   0xbf800000u, 0x3f800000u, 0x3f800000u, 0xbf800000u};
constexpr uint32_t kAtanOffset[8] = {0x00000000u, 0x3fc90fdau, 0x40490fdau, 0x3fc90fdau,
                                     0x00000000u, 0xbfc90fdau, 0xc0490fdau, 0xbfc90fdau};

// I-register immediates of the VU0 micro program at 0x548 (CORE.GT4 image 0x006d3280 + 0x548).
constexpr uint32_t kAtanI[] = {0x3f7ffff5u, 0xbeaaa61cu, 0x3e4c40a6u, 0xbe0e6c63u, 0x3dc577dfu,
                               0xbd6501c4u, 0x3cb31652u, 0x3f490fdbu, 0xbb84d7e7u};

// Common argument reduction of 0x004ac260 / 0x004ac334. Returns false on the trapping `sub`.
struct Reduced {
    uint32_t u;    // reduced argument (bits of $f4)
    int32_t k;     // quadrant index (t0 after `sub t0,t0,t8`)
};
bool Reduce(uint32_t x, Reduced* out) {
    const uint32_t f7 = FpuAdd(x, kPiOver4, nullptr);
    uint32_t f4 = FpuMul(f7, kTwoOverPi, nullptr);
    const uint32_t sign = f7 & kSign;
    const uint32_t f8 = FpuCvtWS(f4);
    const int32_t t8 = sign ? 1 : 0;
    const uint32_t f9 = kHalf ^ sign;
    f4 = FpuSub(f4, FpuCvtSW(f8), nullptr);
    const int64_t k = static_cast<int64_t>(static_cast<int32_t>(f8)) - t8;
    if (k != static_cast<int32_t>(k)) return false;   // `sub` overflow exception
    out->u = FpuSub(f4, f9, nullptr);
    out->k = static_cast<int32_t>(k);
    return true;
}

// 0x004ac2dc: x lane of the sin polynomial (VU0 macro ops).
uint32_t SinPoly(uint32_t u) {
    const uint32_t u2 = VuMul(u, u, nullptr);              // vmul.zw   vf1, vf1, vf1      (w lane)
    uint32_t acc = VuMul(u, kPiOver2, nullptr);            // vmulax    ACC, vf2, vf3x
    const uint32_t u3 = VuMul(u, u2, nullptr);             // vmulw.xz  vf1, vf1, vf1w     (x lane)
    const uint32_t u4 = VuMul(u2, u2, nullptr);            //                              (z lane)
    const uint32_t u7 = VuMul(u3, u4, nullptr);            // vmulz     vf2, vf1, vf1z     (x lane)
    const uint32_t u5 = VuMul(u, u4, nullptr);             //                              (y lane)
    acc = VuMadd(acc, u3, kSinPoly[1], nullptr);           // vmadday   ACC, vf1, vf4y
    const uint32_t u9 = VuMul(u7, u2, nullptr);            // vmulw     vf3, vf2, vf1w     (x lane)
    acc = VuMadd(acc, kSinPoly[0], u5, nullptr);           // vmadday   ACC, vf4, vf2y
    acc = VuMadd(acc, u7, kSinPoly[2], nullptr);           // vmaddaz   ACC, vf2, vf4z
    return VuMadd(acc, u9, kSinPoly[3], nullptr);          // vmaddw    vf1, vf3, vf4w
}

// 0x004ac3bc: x lane of the cos polynomial.
uint32_t CosPoly(uint32_t u) {
    const uint32_t u2 = VuMul(u, u, nullptr);              // vmul      vf1, vf1, vf1
    uint32_t acc = VuAdd(kZero, kOne, nullptr);            // vaddaw    ACC, vf0, vf0w
    acc = VuMadd(acc, u2, kCosPoly[1], nullptr);           // vmadday   ACC, vf1, vf4y
    const uint32_t u4 = VuMul(u2, u2, nullptr);            // vmulx.x   vf1, vf1, vf1x
    acc = VuMadd(acc, u4, kCosPoly[2], nullptr);           // vmaddaz   ACC, vf1, vf4z
    const uint32_t u8 = VuMul(u4, u4, nullptr);            // vmulx.xy  vf1, vf1, vf1x     (x lane)
    const uint32_t u6 = VuMul(u2, u4, nullptr);            //                              (y lane)
    acc = VuMadd(acc, kCosPoly[0], u6, nullptr);           // vmadday   ACC, vf4, vf1y
    return VuMadd(acc, u8, kCosPoly[3], nullptr);          // vmaddw    vf1, vf1, vf4w
}

// VU0 micro program at 0x548 (x lane): vf1.x = larger magnitude, vf2.x = smaller; returns vf4.x.
uint32_t AtanMicro(uint32_t big, uint32_t small) {
    const uint32_t sum = VuAdd(small, big, nullptr);       // ADD   vf5, vf2, vf1
    const uint32_t diff = VuSub(small, big, nullptr);      // SUB   vf6, vf2, vf1
    const uint32_t q = VuDiv(diff, sum, nullptr);          // DIV   Q, vf6x, vf5x ; WAITQ
    const uint32_t one = VuAdd(kZero, kOne, nullptr);      // ADDi  vf8, vf0, I(1.0)
    const uint32_t t = VuAdd(kZero, q, nullptr);           // ADDq  vf1, vf0, Q
    const uint32_t t2 = VuMul(t, t, nullptr);              // MUL   vf7, vf1, vf1
    uint32_t acc = VuMul(t, kAtanI[0], nullptr);           // MULAi ACC, vf1, I
    uint32_t odd = VuMul(t2, t, nullptr);                  // MUL   vf5, vf7, vf1          t^3
    uint32_t next = VuMul(odd, t2, nullptr);               // MUL   vf6, vf5, vf7          t^5
    for (int k = 1; k <= 6; ++k) {                         // MADDAi ACC, vf5|vf6, I ; MUL vf6|vf5 = ... * vf7
        acc = VuMadd(acc, odd, kAtanI[k], nullptr);
        odd = next;
        next = VuMul(odd, t2, nullptr);
    }
    acc = VuMadd(acc, one, kAtanI[7], nullptr);            // MADDAi ACC, vf8, I(pi/4)
    return VuMadd(acc, odd, kAtanI[8], nullptr);           // MADDi  vf4, vf5, I           t^15
}

} // namespace

TrigResult Sin(uint32_t x) {
    TrigResult result;
    Reduced r;
    if (!Reduce(x, &r)) { result.overflowException = true; return result; }
    const uint32_t flip = (r.k & 2) ? kSign : 0u;
    result.value = ((r.k & 1) ? CosPoly(r.u) : SinPoly(r.u)) ^ flip;
    return result;
}

TrigResult Cos(uint32_t x) {
    TrigResult result;
    Reduced r;
    if (!Reduce(x, &r)) { result.overflowException = true; return result; }
    const int64_t next = static_cast<int64_t>(r.k) + 1;   // `add t7,t0,t5` (trapping)
    if (next != static_cast<int32_t>(next)) { result.overflowException = true; return result; }
    uint32_t t7 = static_cast<uint32_t>(static_cast<int32_t>(next));
    if (t7 & 2) t7 = kSign;                                 // `movn t7,t1,t9`; otherwise t7 keeps k + 1
    result.value = ((r.k & 1) ? SinPoly(r.u) : CosPoly(r.u)) ^ t7;
    return result;
}

uint32_t Atan2(uint32_t y, uint32_t x) {
    if (FpuEq(y, kZero)) return FpuLe(kZero, x) ? kZero : 0x40490fdau;
    if (FpuEq(x, kZero)) return FpuLe(kZero, y) ? 0x3fc90fdau : 0xbfc90fdau;
    const uint32_t ax = FpuLt(x, kZero) ? FpuNeg(x) : x;
    const uint32_t ay = FpuLt(y, kZero) ? FpuNeg(y) : y;
    const bool yNotBigger = FpuLe(ay, ax);
    uint32_t r = yNotBigger ? AtanMicro(ax, ay) : AtanMicro(ay, ax);
    const unsigned octant = (FpuLt(y, kZero) ? 4u : 0u) | (FpuLt(x, kZero) ? 2u : 0u) | (FpuLt(ax, ay) ? 1u : 0u);
    const uint32_t sign = FpuLe(kZero, kAtanSign[octant]) ? kOne : kMinusOne;
    if (FpuLt(sign, kZero)) r = FpuNeg(r);
    return FpuAdd(r, kAtanOffset[octant], nullptr);
}

} // namespace gt4::game
