#include "game/math/vu0_matrix.h"

#include <cstring>

#include "ee/fpu.h"

namespace gt4::game {

namespace {

using namespace gt4::game::vu;
using namespace gt4::ee;

constexpr uint32_t kDirty = 0x2000;   // scratchpad offset of the "matrix changed" byte

void SetDirty(uint8_t* scratchpad) { scratchpad[kDirty] = 1; }

Vec4 Row(const uint32_t* m, int row) {
    Vec4 r;
    std::memcpy(r.f, m + 4 * row, 16);
    return r;
}

uint32_t SignExtendHigh(uint32_t x) { return (x & 0x80000000u) ? 0xffffffffu : 0u; }

// QMTC2 of a GPR loaded by MFC1: x = value, y = sign extension; z, w come from the GPR's upper half (undefined).
void LoadScalarLanes(Vec4& d, uint32_t x) {
    d.f[0] = x;
    d.f[1] = SignExtendHigh(x);
}

// v.vf[k] = ACC(vf28*s.x + vf29*s.y + vf30*s.z) + vf31*s.w  (vmulax/vmadday/vmaddaz/vmaddw)
void RowTimesMatrix(Vu0Regs& v, int target, int src) {
    const Vec4 s = v.vf[src];
    MulS(v.acc, v.vf[28], s.f[kX], XYZW);
    MaddS(v.acc, v.acc, v.vf[29], s.f[kY], XYZW);
    MaddS(v.acc, v.acc, v.vf[30], s.f[kZ], XYZW);
    MaddS(v.vf[target], v.acc, v.vf[31], s.f[kW], XYZW);
}

// 0x004a7350: angle (degrees) -> vf1 = (f13r, f12r, sext f13r, sext f12r), reduced sin arguments for x - 90 and x.
void AngleToVf1(Vu0Regs& v, uint32_t x) {
    constexpr uint32_t k90 = 0x42b40000u, kInv180 = 0x3bb60b61u, kPiOver180 = 0x3c8efa35u, kPi = 0x40490fdbu;
    constexpr uint32_t kOne = 0x3f800000u;
    uint32_t f13 = FpuSub(x, k90, nullptr);
    uint32_t f2 = FpuMul(x, kInv180, nullptr);
    const uint32_t v0 = (x & 0x80000000u) ? 0xffffffffu : 0u;   // sra of the original x
    uint32_t f3 = FpuMul(f13, kInv180, nullptr);
    const uint32_t v1 = (f13 & 0x80000000u) ? 0xffffffffu : 0u;
    uint32_t f12 = FpuSub(k90, x, nullptr);
    f13 = FpuSub(k90, f13, nullptr);
    f2 = FpuCvtWS(f2);
    f3 = FpuCvtWS(f3);
    const uint32_t a0pi = v0 & kPi, a1pi = v1 & kPi;
    f12 = FpuMul(f12, kPiOver180, nullptr);
    f13 = FpuMul(f13, kPiOver180, nullptr);
    uint32_t f4 = FpuCvtSW(f2), f5 = FpuCvtSW(f3);
    uint32_t a0 = f2, a1 = f3;
    f12 = FpuSub(f12, a0pi, nullptr);
    f13 = FpuSub(f13, a1pi, nullptr);
    f4 = FpuMul(f4, kPi, nullptr);
    a0 += v0;   // addu
    f5 = FpuMul(f5, kPi, nullptr);
    a1 += v1;
    a0 = ((a0 & 1) << 31) | kOne;
    a1 = ((a1 & 1) << 31) | kOne;
    f12 = FpuAdd(f12, f4, nullptr);
    f13 = FpuAdd(f13, f5, nullptr);
    f12 = FpuMul(f12, a0, nullptr);
    f13 = FpuMul(f13, a1, nullptr);
    // pextlw t0, t0(f12), t1(f13) ; qmtc2.i t0, vf1
    v.vf[1].f[0] = f13;
    v.vf[1].f[1] = f12;
    v.vf[1].f[2] = SignExtendHigh(f13);
    v.vf[1].f[3] = SignExtendHigh(f12);
}

// VU0 micro 0x10..0x50 (entered after vf2 = vf1*vf1, vf4 = vf2*vf2): odd polynomial of vf1, result in vf1.
void MicroSinPoly(Vu0Regs& v) {
    Mul(v.vf[3], v.vf[2], v.vf[1], XYZW);      // 0x10
    Mul(v.vf[2], v.vf[4], v.vf[1], XYZW);      // 0x18
    Mul(v.vf[5], v.vf[4], v.vf[3], XYZW);      // 0x20  I = be2aaaa4
    v.i = 0xbe2aaaa4u;
    MulS(v.acc, v.vf[3], v.i, XYZW);           // 0x28  MULAi
    Mul(v.vf[4], v.vf[4], v.vf[2], XYZW);      // 0x30  I = 3c08873e
    v.i = 0x3c08873eu;
    MaddS(v.acc, v.acc, v.vf[2], v.i, XYZW);   // 0x38  MADDAi, I = b94fb21f
    v.i = 0xb94fb21fu;
    MaddS(v.acc, v.acc, v.vf[5], v.i, XYZW);   // 0x40  MADDAi, I = 362e9c14
    v.i = 0x362e9c14u;
    MaddS(v.acc, v.acc, v.vf[1], v.vf[0].f[kW], XYZW);   // 0x48  MADDAw | JR vi15
    MaddS(v.vf[1], v.acc, v.vf[4], v.i, XYZW);           // 0x50  MADDi (delay slot)
}

// Common head of the micro entries 0x58/0x78/0xb8/0xf8: MUL vf2 | BAL vi15 ; MUL vf4 | (lower) ; subroutine.
void MicroHead(Vu0Regs& v, uint32_t entry, int moveSource) {
    Mul(v.vf[2], v.vf[1], v.vf[1], XYZW);
    v.vi[15] = static_cast<uint16_t>((entry + 16) / 8);
    Mul(v.vf[4], v.vf[2], v.vf[2], XYZW);
    if (moveSource >= 0) Move(v.vf[6], v.vf[moveSource], XYZW);
    MicroSinPoly(v);
}

} // namespace

void Vu0MatIdentity(Vu0Regs& v, uint8_t* scratchpad) {
    Sub(v.vf[29], v.vf[0], v.vf[0], XYZW);
    Sub(v.vf[28], v.vf[0], v.vf[0], XYZW);
    Move(v.vf[31], v.vf[0], XYZW);
    Mr32(v.vf[30], v.vf[0], XYZW);
    AddS(v.vf[29], v.vf[0], v.vf[0].f[kW], Y);
    SetDirty(scratchpad);
    AddS(v.vf[28], v.vf[0], v.vf[0].f[kW], X);
}

void Vu0MatResetRotation(Vu0Regs& v, uint8_t* scratchpad) {
    Sub(v.vf[29], v.vf[0], v.vf[0], XYZ);
    Sub(v.vf[28], v.vf[0], v.vf[0], XYZ);
    Mr32(v.vf[30], v.vf[0], XYZ);
    AddS(v.vf[29], v.vf[29], v.vf[0].f[kW], Y);
    SetDirty(scratchpad);
    AddS(v.vf[28], v.vf[28], v.vf[0].f[kW], X);
}

void Vu0MatClearTranslation(Vu0Regs& v, uint8_t* scratchpad) {
    Sub(v.vf[31], v.vf[0], v.vf[0], XYZ);
    SetDirty(scratchpad);
}

void Vu0MatLoad(Vu0Regs& v, uint8_t* scratchpad, const uint32_t m[16]) {
    for (int r = 0; r < 4; ++r) v.vf[28 + r] = Row(m, r);
    SetDirty(scratchpad);
}

void Vu0MatStore(const Vu0Regs& v, uint32_t out[16]) {
    for (int r = 0; r < 4; ++r) std::memcpy(out + 4 * r, v.vf[28 + r].f, 16);
}

void Vu0MatMultiply(Vu0Regs& v, uint8_t* scratchpad, const uint32_t m[16]) {
    for (int r = 0; r < 4; ++r) v.vf[1 + r] = Row(m, r);
    RowTimesMatrix(v, 1, 1);
    RowTimesMatrix(v, 2, 2);
    RowTimesMatrix(v, 3, 3);
    RowTimesMatrix(v, 31, 4);   // vmaddw vf31, vf31, vf4w: vf4 keeps the loaded row
    MulS(v.vf[28], v.vf[1], v.vf[0].f[kW], XYZW);
    MulS(v.vf[29], v.vf[2], v.vf[0].f[kW], XYZW);
    SetDirty(scratchpad);
    MulS(v.vf[30], v.vf[3], v.vf[0].f[kW], XYZW);
}

void Vu0MatPreMultiply(Vu0Regs& v, uint8_t* scratchpad, const uint32_t m[16]) {
    for (int r = 0; r < 4; ++r) v.vf[1 + r] = Row(m, r);
    for (int r = 28; r < 32; ++r) {
        const Vec4 row = v.vf[r];
        MulS(v.acc, v.vf[1], row.f[kX], XYZW);
        MaddS(v.acc, v.acc, v.vf[2], row.f[kY], XYZW);
        MaddS(v.acc, v.acc, v.vf[3], row.f[kZ], XYZW);
        MaddS(v.vf[r], v.acc, v.vf[4], row.f[kW], XYZW);
    }
    SetDirty(scratchpad);
}

void Vu0MatTranslate(Vu0Regs& v, uint8_t* scratchpad, uint32_t x, uint32_t y, uint32_t z) {
    LoadScalarLanes(v.vf[1], x);
    LoadScalarLanes(v.vf[2], y);
    LoadScalarLanes(v.vf[3], z);
    MulS(v.acc, v.vf[28], v.vf[1].f[kX], XYZW);
    MaddS(v.acc, v.acc, v.vf[29], v.vf[2].f[kX], XYZW);
    MaddS(v.acc, v.acc, v.vf[30], v.vf[3].f[kX], XYZW);
    SetDirty(scratchpad);
    MaddS(v.vf[31], v.acc, v.vf[31], v.vf[0].f[kW], XYZW);
}

void Vu0MatScale(Vu0Regs& v, uint8_t* scratchpad, uint32_t x, uint32_t y, uint32_t z) {
    LoadScalarLanes(v.vf[1], x);
    LoadScalarLanes(v.vf[2], y);
    LoadScalarLanes(v.vf[3], z);
    MulS(v.vf[28], v.vf[28], v.vf[1].f[kX], XYZW);
    MulS(v.vf[29], v.vf[29], v.vf[2].f[kX], XYZW);
    SetDirty(scratchpad);
    MulS(v.vf[30], v.vf[30], v.vf[3].f[kX], XYZW);
}

void Vu0MatTransformPoint(Vu0Regs& v, uint32_t out[4], const uint32_t in[4]) {
    std::memcpy(v.vf[1].f, in, 16);
    RowTimesMatrix(v, 1, 1);
    std::memcpy(out, v.vf[1].f, 16);
}

void Vu0MatTranspose(Vu0Regs& v, uint8_t* scratchpad) {
    Vec4 t[4];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) t[r].f[c] = v.vf[28 + c].f[r];
    for (int r = 0; r < 4; ++r) v.vf[28 + r] = t[r];
    SetDirty(scratchpad);
}

void Vu0MatInvertRigid(Vu0Regs& v, uint8_t* scratchpad) {
    Sub(v.vf[31], v.vf[0], v.vf[31], XYZ);   // vf31.xyz = -t (through the FMAC)
    Vec4 t[3];
    for (int r = 0; r < 3; ++r) {             // transpose of the 3x3, w lanes cleared (pextlw with $zero)
        for (int c = 0; c < 3; ++c) t[r].f[c] = v.vf[28 + c].f[r];
        t[r].f[3] = 0;
    }
    for (int r = 0; r < 3; ++r) v.vf[28 + r] = t[r];
    MulS(v.acc, v.vf[28], v.vf[31].f[kX], XYZ);
    MaddS(v.acc, v.acc, v.vf[29], v.vf[31].f[kY], XYZ);
    SetDirty(scratchpad);
    MaddS(v.vf[31], v.acc, v.vf[30], v.vf[31].f[kZ], XYZ);
}

void Vu0MatSelectContext(Vu0Regs& v, uint8_t* scratchpad, uint64_t index) {
    uint8_t* base = scratchpad + 0x2000;
    const uint64_t current = base[1];
    if (current == index) return;
    const uint16_t vi13 = v.vi[13];
    auto slot = [&](uint64_t n) -> uint32_t { return n == 0 ? 0xed0u : n == 1 ? 0xf10u : 0xf50u; };
    auto viSlot = [&](uint64_t n) -> uint32_t { return n == 0 ? 0xf90u : n == 1 ? 0xf92u : 0xf94u; };
    std::memcpy(base + viSlot(current), &vi13, 2);
    for (int r = 0; r < 4; ++r) std::memcpy(base + slot(current) + 16 * r, v.vf[28 + r].f, 16);
    uint16_t restored;
    std::memcpy(&restored, base + viSlot(index), 2);
    for (int r = 0; r < 4; ++r) std::memcpy(v.vf[28 + r].f, base + slot(index) + 16 * r, 16);
    v.vi[13] = restored;
    base[1] = static_cast<uint8_t>(index);
}

void Vu0SinCosDegrees(Vu0Regs& v, uint32_t degrees) {
    AngleToVf1(v, degrees);
    MicroHead(v, 0x58, -1);
}

void Vu0MatRotateX(Vu0Regs& v, uint8_t* scratchpad, uint32_t degrees) {
    SetDirty(scratchpad);
    AngleToVf1(v, degrees);
    MicroHead(v, 0x78, 29);
    MulS(v.acc, v.vf[29], v.vf[1].f[kY], XYZW);
    MaddS(v.vf[29], v.acc, v.vf[30], v.vf[1].f[kX], XYZW);
    MulS(v.acc, v.vf[30], v.vf[1].f[kY], XYZW);
    MsubS(v.vf[30], v.acc, v.vf[6], v.vf[1].f[kX], XYZW);
}

void Vu0MatRotateY(Vu0Regs& v, uint8_t* scratchpad, uint32_t degrees) {
    SetDirty(scratchpad);
    AngleToVf1(v, degrees);
    MicroHead(v, 0xb8, 30);
    MulS(v.acc, v.vf[28], v.vf[1].f[kX], XYZW);
    MaddS(v.vf[30], v.acc, v.vf[30], v.vf[1].f[kY], XYZW);
    MulS(v.acc, v.vf[28], v.vf[1].f[kY], XYZW);
    MsubS(v.vf[28], v.acc, v.vf[6], v.vf[1].f[kX], XYZW);
}

void Vu0MatRotateZ(Vu0Regs& v, uint8_t* scratchpad, uint32_t degrees) {
    SetDirty(scratchpad);
    AngleToVf1(v, degrees);
    MicroHead(v, 0xf8, 28);
    MulS(v.acc, v.vf[28], v.vf[1].f[kY], XYZW);
    MaddS(v.vf[28], v.acc, v.vf[29], v.vf[1].f[kX], XYZW);
    MulS(v.acc, v.vf[29], v.vf[1].f[kY], XYZW);
    MsubS(v.vf[29], v.acc, v.vf[6], v.vf[1].f[kX], XYZW);
}

} // namespace gt4::game
