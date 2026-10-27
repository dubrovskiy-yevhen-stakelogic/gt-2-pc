#pragma once
// VU (VU0/VU1 FMAC and FDIV units) single-precision arithmetic on raw 32-bit patterns, shared by the oracle
// interpreter (src/interp/vu0) and native simulation code (decision D2), so that native ports of VU0 code agree
// with the original bit for bit.
//
// Model v0 - docs/research/vu0.md:
//   * the FMAC follows the EE FPU model of src/ee/fpu (no NaN/Inf/denormals, truncation, clamping to +-max),
//     including the MADD/MSUB rule "product overflow -> clamped product". ps2autotests has no FMAC add/mul
//     vectors except tests/vu/games/triace (MULi/ADDi with large and tiny operands, passes) and the 1.0/4.0-type
//     values of tests/vu/lower/fdivdelay; the VU-specific rounding is therefore only weakly verified;
//   * MAC flag bits per lane: Z (result is +-0), S (sign bit of result), U (underflow), O (overflow);
//   * DIV/SQRT/RSQRT follow FpuDiv/FpuSqrt/FpuRsqrt; status D = divide by zero, I = invalid (0/0, sqrt(-x));
//   * FTOIn: truncation toward zero of x * 2^n, saturating to 0x7FFFFFFF / 0x80000000; ITOFn: int -> float
//     (truncation), then exact scaling by 2^-n;
//   * MAX/MINI: raw sign-magnitude ordering as FpuMax/FpuMin (no vectors);
//   * CLIP: +x if x > +|w|, -x if x < -|w| (and y, z), on flushed values (tests/vu/upper/clip passes).
#include <cstdint>

namespace gt4::ee {

struct VuLaneFlags {
    bool o = false, u = false, s = false, z = false;
};

uint32_t VuAdd(uint32_t a, uint32_t b, VuLaneFlags* flags);
uint32_t VuSub(uint32_t a, uint32_t b, VuLaneFlags* flags);
uint32_t VuMul(uint32_t a, uint32_t b, VuLaneFlags* flags);
uint32_t VuMadd(uint32_t acc, uint32_t a, uint32_t b, VuLaneFlags* flags);   // acc + a*b
uint32_t VuMsub(uint32_t acc, uint32_t a, uint32_t b, VuLaneFlags* flags);   // acc - a*b
uint32_t VuMax(uint32_t a, uint32_t b);
uint32_t VuMin(uint32_t a, uint32_t b);
inline uint32_t VuAbs(uint32_t a) { return a & 0x7fffffffu; }
uint32_t VuFtoi(uint32_t a, int fractionBits);   // FTOI0/4/12/15
uint32_t VuItof(uint32_t a, int fractionBits);   // ITOF0/4/12/15

struct VuDivFlags {
    bool d = false;   // divide by zero
    bool i = false;   // invalid
};
uint32_t VuDiv(uint32_t a, uint32_t b, VuDivFlags* flags);     // Q = a / b
uint32_t VuSqrt(uint32_t b, VuDivFlags* flags);                // Q = sqrt(|b|)
uint32_t VuRsqrt(uint32_t a, uint32_t b, VuDivFlags* flags);   // Q = a / sqrt(|b|)

// Six CLIP judgement bits for one CLIPw: bit0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z.
uint32_t VuClip(uint32_t x, uint32_t y, uint32_t z, uint32_t w);

} // namespace gt4::ee
