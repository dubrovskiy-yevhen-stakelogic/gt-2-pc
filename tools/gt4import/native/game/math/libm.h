#pragma once
// Native port of the single-precision math routines of CORE.GT4 US 1.01 at 0x0057d008..0x0057d9b8 (EE FPU only,
// polynomial approximations with their own constants). Raw 32-bit EE float patterns in and out, arithmetic on
// src/ee/fpu (decision D2). Verified by gt4verify rows "libm_*" (docs/research/vu0.md, math library).
#include <cstdint>

namespace gt4::game {

uint32_t LibmAtan(uint32_t x);                 // 0x0057d008
uint32_t LibmAtan2(uint32_t y, uint32_t x);    // 0x0057d118 ($f12 = y, $f13 = x)
uint32_t LibmCos(uint32_t x);                  // 0x0057d1f0
uint32_t LibmSin(uint32_t x);                  // 0x0057d2b8
uint32_t LibmTan(uint32_t x);                  // 0x0057d380
uint32_t LibmExp(uint32_t x);                  // 0x0057d488
uint32_t LibmLog(uint32_t x);                  // 0x0057d5a8 (x <= 0 -> 0xffffffff, the word at 0x006cc93c)
uint32_t LibmPow(uint32_t x, uint32_t y);      // 0x0057d6c0 = exp(y * log(x))
// 0x0057d8a0: *out1 ($a1) and *out0 ($a0) get the two polynomial results (a sin/cos pair) of x.
void LibmSinCos(uint32_t x, uint32_t* out0, uint32_t* out1);
// 0x005a2e20: |x| (sign bit cleared through memory).
uint32_t LibmFabs(uint32_t x);
// 0x0059f050: software square root (bit-by-bit integer method). Zero / denormal inputs are returned unchanged;
// negative inputs give (x - x) / (x - x).
uint32_t LibmSqrtSoft(uint32_t x);
// 0x0059e890 (reached through 0x0059d2a0): arcsine, rational approximation with LibmSqrtSoft for |x| >= 0.5.
uint32_t LibmAsin(uint32_t x);

} // namespace gt4::game
