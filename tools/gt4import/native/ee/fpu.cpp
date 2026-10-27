#include "ee/fpu.h"

namespace gt4::ee {

namespace {

constexpr uint32_t kMax = 0x7fffffffu;

struct Unpacked {
    uint32_t sign;
    int exp;        // biased exponent, 0 = zero (denormals flushed)
    uint64_t mant;  // 24-bit mantissa with hidden bit, 0 for zero
};

Unpacked Unpack(uint32_t x) {
    const uint32_t sign = x >> 31;
    const int exp = static_cast<int>((x >> 23) & 0xff);
    if (exp == 0) return {sign, 0, 0};
    return {sign, exp, (x & 0x7fffffu) | 0x800000u};
}

int BitLength(uint64_t v) {
    int n = 0;
    while (v) { ++n; v >>= 1; }
    return n;
}

enum class Round { Zero, HalfUp, NearestEven };

// value = mant * 2^lsbExp. Normalises to 24 bits, rounds, clamps. Sets *over / *under when given.
uint32_t Pack(uint32_t sign, uint64_t mant, int lsbExp, Round mode, bool* over, bool* under) {
    if (mant == 0) return sign << 31;
    const int shift = BitLength(mant) - 24;
    if (shift > 0) {
        const uint64_t rem = mant & ((uint64_t{1} << shift) - 1);
        const uint64_t half = uint64_t{1} << (shift - 1);
        mant >>= shift;
        lsbExp += shift;
        if ((mode == Round::HalfUp && rem >= half) ||
            (mode == Round::NearestEven && (rem > half || (rem == half && (mant & 1))))) {
            ++mant;
            if (mant >> 24) { mant >>= 1; ++lsbExp; }
        }
    } else if (shift < 0) {
        mant <<= -shift;
        lsbExp += shift;
    }
    const int biased = lsbExp + 150;
    if (biased > 255) {
        if (over) *over = true;
        return (sign << 31) | kMax;
    }
    if (biased < 1) {
        if (under) *under = true;
        return sign << 31;
    }
    return (sign << 31) | (static_cast<uint32_t>(biased) << 23) | static_cast<uint32_t>(mant & 0x7fffffu);
}

uint32_t AddImpl(uint32_t a, uint32_t b, bool subtract, FpuFlags* flags) {
    bool over = false, under = false;
    Unpacked x = Unpack(a), y = Unpack(b);
    if (subtract) y.sign ^= 1;
    uint32_t result;
    if (x.mant == 0 && y.mant == 0) {
        result = (x.sign & y.sign) << 31;
    } else if (x.mant == 0) {
        result = (y.sign << 31) | (b & kMax);
    } else if (y.mant == 0) {
        result = a;
    } else {
        if (x.exp < y.exp || (x.exp == y.exp && x.mant < y.mant)) { const Unpacked t = x; x = y; y = t; }
        const int d = x.exp - y.exp;
        const uint64_t aligned = d > 24 ? 0 : (y.mant >> d);   // truncating alignment, no guard bits
        const uint64_t total = x.sign == y.sign ? x.mant + aligned : x.mant - aligned;
        result = total == 0 ? 0 : Pack(x.sign, total, x.exp - 150, Round::Zero, &over, &under);
    }
    if (flags) { flags->o = over; flags->u = under; }
    return result;
}

// Exponent the exact product would have (biased), used for the MADD overflow rule.
int ProductExponent(const Unpacked& x, const Unpacked& y) {
    return x.exp + y.exp - 127 + (((x.mant * y.mant) >> 47) ? 1 : 0);
}

uint64_t Isqrt(uint64_t v) {
    uint64_t r = 0, bit = uint64_t{1} << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

int64_t OrderKey(uint32_t x) {
    return (x & 0x80000000u) ? -static_cast<int64_t>(x & kMax) - 1 : static_cast<int64_t>(x);
}

int64_t ValueKey(uint32_t x) {   // zero/denormal -> 0, otherwise signed magnitude
    if (((x >> 23) & 0xff) == 0) return 0;
    return (x & 0x80000000u) ? -static_cast<int64_t>(x & kMax) : static_cast<int64_t>(x & kMax);
}

} // namespace

uint32_t FpuAdd(uint32_t a, uint32_t b, FpuFlags* flags) { return AddImpl(a, b, false, flags); }
uint32_t FpuSub(uint32_t a, uint32_t b, FpuFlags* flags) { return AddImpl(a, b, true, flags); }

uint32_t FpuMul(uint32_t a, uint32_t b, FpuFlags* flags) {
    bool over = false, under = false;
    const Unpacked x = Unpack(a), y = Unpack(b);
    const uint32_t sign = x.sign ^ y.sign;
    const uint32_t result = (x.mant == 0 || y.mant == 0)
        ? sign << 31
        : Pack(sign, x.mant * y.mant, x.exp + y.exp - 300, Round::Zero, &over, &under);
    if (flags) { flags->o = over; flags->u = under; }
    return result;
}

static uint32_t MaddImpl(uint32_t acc, uint32_t a, uint32_t b, bool subtract, FpuFlags* flags) {
    const Unpacked x = Unpack(a), y = Unpack(b);
    if (x.mant && y.mant && ProductExponent(x, y) > 255) {
        if (flags) { flags->o = true; flags->u = false; }
        return (((x.sign ^ y.sign) ^ (subtract ? 1u : 0u)) << 31) | kMax;
    }
    FpuFlags productFlags;
    const uint32_t product = FpuMul(a, b, &productFlags);
    const uint32_t result = AddImpl(acc, product, subtract, flags);
    if (flags) flags->u = flags->u || productFlags.u;
    return result;
}

uint32_t FpuMadd(uint32_t acc, uint32_t a, uint32_t b, FpuFlags* flags) { return MaddImpl(acc, a, b, false, flags); }
uint32_t FpuMsub(uint32_t acc, uint32_t a, uint32_t b, FpuFlags* flags) { return MaddImpl(acc, a, b, true, flags); }

#ifndef GT4_FPU_DIV_MODEL
#define GT4_FPU_DIV_MODEL 0
#endif
namespace {
FpuDivModel g_divModel = static_cast<FpuDivModel>(GT4_FPU_DIV_MODEL);

// Candidate hardware divider (docs/research/ee_divider_20261018.md): radix-2 SRT, digits {-1,0,1}, partial remainder
// in carry-save form (S, C; 3:2 full-adder row, -B as ~B + carry-in), w0 = x (mantissa of the dividend, 24 bits),
// w(j+1) = 2 (w(j) - q(j) y). Digit selection from the truncated carry-save estimates
//   e22 = (S >> 22) + (C >> 22), e23 = (S >> 23) + (C >> 23)  (28-bit registers, estimates modulo their width):
//   q = +1 if e22 >= 1, 0 if e23 >= -1, else -1.
// 25 digits (weights 2^0 .. 2^-24), no remainder correction: x >= y -> mantissa = Q >> 1, else Q.
// Returns the 24-bit mantissa; *bump is 1 when the quotient is >= 1 (exponent of x/y unchanged), else 0.
uint64_t SrtMantissa(uint64_t x, uint64_t y, int* bump) {
    constexpr uint32_t kBits = 28;
    constexpr uint64_t kMask = (uint64_t{1} << kBits) - 1;
    auto estimate = [](uint64_t s, uint64_t c, int t) {
        const int width = static_cast<int>(kBits) - t;
        const uint64_t v = ((s >> t) + (c >> t)) & ((uint64_t{1} << width) - 1);
        return (v >> (width - 1)) ? static_cast<int64_t>(v) - (int64_t{1} << width) : static_cast<int64_t>(v);
    };
    uint64_t s = x, c = 0;
    int64_t q = 0;
    for (int j = 0; j < 25; ++j) {
        const int digit = estimate(s, c, 22) >= 1 ? 1 : (estimate(s, c, 23) >= -1 ? 0 : -1);
        q = q * 2 + digit;
        uint64_t add = 0, carryIn = 0;
        if (digit == 1) { add = ~y & kMask; carryIn = 1; }
        else if (digit == -1) { add = y; }
        const uint64_t sum = s ^ c ^ add;
        const uint64_t carry = ((((s & c) | (s & add) | (c & add)) << 1) | carryIn) & kMask;
        s = (sum << 1) & kMask;
        c = (carry << 1) & kMask;
    }
    *bump = x >= y ? 1 : 0;
    return static_cast<uint64_t>(x >= y ? (q >> 1) : q);
}

uint32_t DivMantissas(uint32_t sign, const Unpacked& x, const Unpacked& y, FpuDivModel model) {
    if (model == FpuDivModel::Srt2) {
        int bump = 0;
        const uint64_t m = SrtMantissa(x.mant, y.mant, &bump);
        // value = m * 2^(x.exp - y.exp + bump - 1 - 23)
        return Pack(sign, m, x.exp - y.exp + bump - 24, Round::Zero, nullptr, nullptr);
    }
    const uint64_t quotient = (x.mant << 25) / y.mant;   // 25 or 26 significant bits
    return Pack(sign, quotient, x.exp - y.exp - 25, Round::HalfUp, nullptr, nullptr);   // DIV leaves O/U alone
}
} // namespace

void SetFpuDivModel(FpuDivModel model) { g_divModel = model; }
FpuDivModel GetFpuDivModel() { return g_divModel; }

uint32_t FpuDivWith(FpuDivModel model, uint32_t a, uint32_t b, FpuFlags* flags) {
    const Unpacked x = Unpack(a), y = Unpack(b);
    const uint32_t sign = x.sign ^ y.sign;
    if (flags) { flags->d = false; flags->i = false; }
    if (y.mant == 0) {
        if (flags) { if (x.mant == 0) flags->i = true; else flags->d = true; }
        return (sign << 31) | kMax;
    }
    if (x.mant == 0) return sign << 31;
    return DivMantissas(sign, x, y, model);
}

uint32_t FpuDiv(uint32_t a, uint32_t b, FpuFlags* flags) { return FpuDivWith(g_divModel, a, b, flags); }

uint32_t FpuSqrt(uint32_t a, FpuFlags* flags) {
    const Unpacked x = Unpack(a);
    if (flags) { flags->d = false; flags->i = x.mant != 0 && x.sign != 0; }
    if (x.mant == 0) return 0;
    int e = x.exp - 150;
    uint64_t m = x.mant;
    if (e & 1) { m <<= 1; --e; }
    constexpr int k = 14;   // radicand <= 2^53, root ~27 bits
    const uint64_t radicand = m << (2 * k);
    const uint64_t root = Isqrt(radicand);
    const uint64_t sticky = root * root != radicand ? 1 : 0;
    return Pack(0, (root << 1) | sticky, e / 2 - k - 1, Round::NearestEven, nullptr, nullptr);
}

uint32_t FpuRsqrtWith(FpuDivModel model, uint32_t a, uint32_t b, FpuFlags* flags) {
    const Unpacked x = Unpack(a), y = Unpack(b);
    if (flags) { flags->d = false; flags->i = y.mant != 0 && y.sign != 0; }
    if (y.mant == 0) {
        if (flags) { if (x.mant == 0) flags->i = true; else flags->d = true; }
        return (x.sign << 31) | kMax;
    }
    if (x.mant == 0) return x.sign << 31;
    const Unpacked r = Unpack(FpuSqrt(b & kMax, nullptr));
    if (model == FpuDivModel::Srt2) return DivMantissas(x.sign, x, r, model);
    const uint64_t quotient = (x.mant << 26) / r.mant;
    return Pack(x.sign, quotient, x.exp - r.exp - 26, Round::Zero, nullptr, nullptr);
}

uint32_t FpuRsqrt(uint32_t a, uint32_t b, FpuFlags* flags) { return FpuRsqrtWith(g_divModel, a, b, flags); }

uint32_t FpuMax(uint32_t a, uint32_t b) { return OrderKey(a) >= OrderKey(b) ? a : b; }
uint32_t FpuMin(uint32_t a, uint32_t b) { return OrderKey(a) <= OrderKey(b) ? a : b; }

bool FpuEq(uint32_t a, uint32_t b) { return ValueKey(a) == ValueKey(b); }
bool FpuLt(uint32_t a, uint32_t b) { return ValueKey(a) < ValueKey(b); }
bool FpuLe(uint32_t a, uint32_t b) { return ValueKey(a) <= ValueKey(b); }

uint32_t FpuCvtWS(uint32_t a) {
    const Unpacked x = Unpack(a);
    if (x.mant == 0) return 0;
    if (x.exp >= 158) return x.sign ? 0x80000000u : 0x7fffffffu;
    const int shift = x.exp - 150;
    const uint64_t v = shift >= 0 ? (x.mant << shift) : (shift > -32 ? (x.mant >> -shift) : 0);
    const uint32_t magnitude = static_cast<uint32_t>(v);
    return x.sign ? (0u - magnitude) : magnitude;
}

uint32_t FpuCvtSW(uint32_t a) {
    const int32_t v = static_cast<int32_t>(a);
    const uint64_t magnitude = v < 0 ? uint64_t{0} - static_cast<uint64_t>(static_cast<int64_t>(v)) : static_cast<uint64_t>(v);
    return Pack(v < 0 ? 1u : 0u, magnitude, 0, Round::Zero, nullptr, nullptr);
}

} // namespace gt4::ee
