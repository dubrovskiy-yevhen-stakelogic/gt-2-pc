#include "ee/vu_float.h"

#include "ee/fpu.h"

namespace gt4::ee {

namespace {

void Lane(uint32_t result, const FpuFlags& f, VuLaneFlags* flags) {
    if (!flags) return;
    flags->o = f.o;
    flags->u = f.u;
    flags->s = (result >> 31) != 0;
    flags->z = (result & 0x7fffffffu) == 0;
}

// Flushed signed value for comparisons: exponent 0 -> 0, otherwise signed magnitude.
int64_t Value(uint32_t x) {
    if (((x >> 23) & 0xff) == 0) return 0;
    const int64_t magnitude = static_cast<int64_t>(x & 0x7fffffffu);
    return (x >> 31) ? -magnitude : magnitude;
}

} // namespace

uint32_t VuAdd(uint32_t a, uint32_t b, VuLaneFlags* flags) {
    FpuFlags f;
    const uint32_t r = FpuAdd(a, b, &f);
    Lane(r, f, flags);
    return r;
}

uint32_t VuSub(uint32_t a, uint32_t b, VuLaneFlags* flags) {
    FpuFlags f;
    const uint32_t r = FpuSub(a, b, &f);
    Lane(r, f, flags);
    return r;
}

uint32_t VuMul(uint32_t a, uint32_t b, VuLaneFlags* flags) {
    FpuFlags f;
    const uint32_t r = FpuMul(a, b, &f);
    Lane(r, f, flags);
    return r;
}

uint32_t VuMadd(uint32_t acc, uint32_t a, uint32_t b, VuLaneFlags* flags) {
    FpuFlags f;
    const uint32_t r = FpuMadd(acc, a, b, &f);
    Lane(r, f, flags);
    return r;
}

uint32_t VuMsub(uint32_t acc, uint32_t a, uint32_t b, VuLaneFlags* flags) {
    FpuFlags f;
    const uint32_t r = FpuMsub(acc, a, b, &f);
    Lane(r, f, flags);
    return r;
}

uint32_t VuMax(uint32_t a, uint32_t b) { return FpuMax(a, b); }
uint32_t VuMin(uint32_t a, uint32_t b) { return FpuMin(a, b); }

uint32_t VuFtoi(uint32_t a, int fractionBits) {
    const int exp = static_cast<int>((a >> 23) & 0xff);
    if (exp == 0) return 0;
    const bool negative = (a >> 31) != 0;
    const uint64_t mant = (a & 0x7fffffu) | 0x800000u;
    const int shift = exp - 150 + fractionBits;   // value * 2^n = mant * 2^shift
    uint64_t magnitude;
    if (shift >= 8) {
        magnitude = uint64_t{1} << 32;            // >= 2^31 for sure: saturate below
    } else if (shift >= 0) {
        magnitude = mant << shift;
    } else {
        magnitude = shift > -32 ? (mant >> -shift) : 0;
    }
    if (negative) {
        if (magnitude >= 0x80000000ull) return 0x80000000u;
        return static_cast<uint32_t>(0u - static_cast<uint32_t>(magnitude));
    }
    if (magnitude >= 0x80000000ull) return 0x7fffffffu;
    return static_cast<uint32_t>(magnitude);
}

uint32_t VuItof(uint32_t a, int fractionBits) {
    const uint32_t f = FpuCvtSW(a);
    if ((f & 0x7fffffffu) == 0) return f;
    // |int| >= 1 gives exponent >= 127, so scaling by at most 2^-15 cannot underflow.
    return f - (static_cast<uint32_t>(fractionBits) << 23);
}

uint32_t VuDiv(uint32_t a, uint32_t b, VuDivFlags* flags) {
    FpuFlags f;
    const uint32_t r = FpuDiv(a, b, &f);
    if (flags) { flags->d = f.d; flags->i = f.i; }
    return r;
}

uint32_t VuSqrt(uint32_t b, VuDivFlags* flags) {
    FpuFlags f;
    const uint32_t r = FpuSqrt(b, &f);
    if (flags) { flags->d = f.d; flags->i = f.i; }
    return r;
}

uint32_t VuRsqrt(uint32_t a, uint32_t b, VuDivFlags* flags) {
    FpuFlags f;
    const uint32_t r = FpuRsqrt(a, b, &f);
    if (flags) { flags->d = f.d; flags->i = f.i; }
    return r;
}

uint32_t VuClip(uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
    const int64_t limit = Value(w & 0x7fffffffu);
    uint32_t bits = 0;
    const uint32_t v[3] = {x, y, z};
    for (int k = 0; k < 3; ++k) {
        const int64_t value = Value(v[k]);
        if (value > limit) bits |= 1u << (2 * k);
        if (value < -limit) bits |= 2u << (2 * k);
    }
    return bits;
}

} // namespace gt4::ee
