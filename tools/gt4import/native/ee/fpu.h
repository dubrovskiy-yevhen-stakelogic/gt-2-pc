#pragma once
// EE (R5900) COP1 single-precision arithmetic with PS2 semantics, operating on raw 32-bit patterns.
// Shared by the oracle interpreter and by native simulation code (decision D2): both must call these
// functions so that a native port and the original agree bit for bit.
//
// Model v0, scored against ps2autotests hardware vectors (tests/cpu/ee_fpu, ISC) - docs/research/ee_fpu.md:
//   * no NaN/Inf/denormals: exponent 0 operands are signed zero, exponent 255 is an ordinary number;
//     overflow clamps to +-0x7FFFFFFF (O flag), underflow gives signed zero (U flag);
//   * ADD/SUB: smaller operand aligned with truncation and no guard bits, then truncated (all vectors pass);
//   * MUL: exact product truncated - KNOWN GAP: hardware returns one ulp less for 1.0 * (mantissa all ones)
//     (operand-order dependent multiplier artefact; 3 vectors per mul-family op fail);
//   * DIV: round half up on a quotient with one guard bit (all vectors pass, model not unique) - model v0, the
//     default. Candidate FpuDivModel::Srt2 (docs/research/ee_divider_20261018.md): radix-2 SRT with a carry-save
//     partial remainder, 25 quotient digits, truncated; matches 96.6% of the hardware quotients recovered from the
//     disc replays - NOT exact yet, so it is off by default;
//   * SQRT: round to nearest (all vectors pass); RSQRT: SQRT then truncating divide (1 vector fails);
//   * MADD/MSUB: if the product overflows the result is the clamped product, else ACC +- product.
#include <cstdint>

namespace gt4::ee {

struct FpuFlags {
    bool o = false;   // overflow (cause), set/cleared by add/sub/mul/madd families
    bool u = false;   // underflow (cause), same family
    bool d = false;   // divide by zero, set/cleared by div/rsqrt
    bool i = false;   // invalid (0/0, sqrt/rsqrt of negative), set/cleared by div/sqrt/rsqrt
};

uint32_t FpuAdd(uint32_t a, uint32_t b, FpuFlags* flags);
uint32_t FpuSub(uint32_t a, uint32_t b, FpuFlags* flags);
uint32_t FpuMul(uint32_t a, uint32_t b, FpuFlags* flags);
uint32_t FpuMadd(uint32_t acc, uint32_t a, uint32_t b, FpuFlags* flags);   // acc + a*b
uint32_t FpuMsub(uint32_t acc, uint32_t a, uint32_t b, FpuFlags* flags);   // acc - a*b
uint32_t FpuDiv(uint32_t a, uint32_t b, FpuFlags* flags);

// Divider model used by FpuDiv/FpuRsqrt (and the VU DIV/RSQRT that call them). Default: HalfUp (model v0), or the
// value of the compile definition GT4_FPU_DIV_MODEL (0 = HalfUp, 1 = Srt2). Changing it changes every regression hash.
enum class FpuDivModel : uint8_t { HalfUp = 0, Srt2 = 1 };
void SetFpuDivModel(FpuDivModel model);
FpuDivModel GetFpuDivModel();
uint32_t FpuDivWith(FpuDivModel model, uint32_t a, uint32_t b, FpuFlags* flags);
uint32_t FpuRsqrtWith(FpuDivModel model, uint32_t a, uint32_t b, FpuFlags* flags);
uint32_t FpuSqrt(uint32_t a, FpuFlags* flags);
uint32_t FpuRsqrt(uint32_t a, uint32_t b, FpuFlags* flags);                // a / sqrt(b)
uint32_t FpuMax(uint32_t a, uint32_t b);   // raw sign-magnitude ordering, no flushing
uint32_t FpuMin(uint32_t a, uint32_t b);
inline uint32_t FpuAbs(uint32_t a) { return a & 0x7fffffffu; }
inline uint32_t FpuNeg(uint32_t a) { return a ^ 0x80000000u; }
bool FpuEq(uint32_t a, uint32_t b);
bool FpuLt(uint32_t a, uint32_t b);
bool FpuLe(uint32_t a, uint32_t b);
uint32_t FpuCvtWS(uint32_t a);   // float -> int32, truncation, saturating
uint32_t FpuCvtSW(uint32_t a);   // int32 -> float, truncation

} // namespace gt4::ee
