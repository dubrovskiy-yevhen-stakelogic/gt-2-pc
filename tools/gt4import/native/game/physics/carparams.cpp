// Car parameter builder 0x004413c8 and its per-part routines, race completion 0x003f3b68 (CORE.GT4 US 1.01).
// Every routine names its original address. Integer semantics follow the R5900 code: 32-bit mult, signed div/div1,
// byte / halfword stores truncate.
#include "game/physics/carparams.h"

#include <cstring>

#include "ee/fpu.h"

namespace gt4::game::carparams {

const PartSlot kPartSlots[30] = {
    {0x20, 1},  {0x28, 2},  {0x30, 6},  {0x38, 12}, {0x40, 10}, {0x48, 11}, {0x50, 3},  {0x58, 24}, {0x60, 25}, {0x68, 26},
    {0x70, 9},  {0x78, 8},  {0x80, 7},  {0x88, 15}, {0x90, 16}, {0x98, 17}, {0xa0, 18}, {0xa8, 13}, {0xb0, 14}, {0xb8, 22},
    {0xc0, 21}, {0xc8, 23}, {0xd0, 20}, {0xd8, 19}, {0xe0, 4},  {0xe8, 5},  {0xf0, 29}, {0xf8, 27}, {0x100, 30}, {0x108, 28},
};

namespace {

constexpr uint64_t kNone = ~0ull;

inline uint16_t L16(const uint8_t* p, int o) { return static_cast<uint16_t>(p[o] | p[o + 1] << 8); }
inline int16_t S16(const uint8_t* p, int o) { return static_cast<int16_t>(L16(p, o)); }
inline void W16(uint8_t* p, int o, uint32_t v) {
    p[o] = static_cast<uint8_t>(v);
    p[o + 1] = static_cast<uint8_t>(v >> 8);
}
inline uint32_t L32(const uint8_t* p, int o) {
    uint32_t v;
    std::memcpy(&v, p + o, 4);
    return v;
}
inline uint64_t L64(const uint8_t* p, int o) {
    uint64_t v;
    std::memcpy(&v, p + o, 8);
    return v;
}
// signed 32-bit division of the low words, like div/div1 + mflo
inline int32_t Div(int32_t a, int32_t b) { return b == 0 ? 0 : (a == INT32_MIN && b == -1 ? a : a / b); }

// 0x004452c0(-, a, b, n, k): a + (b - a) * (k - 1) / (n - 1) for n >= 2, else a (32-bit, signed).
int32_t Interp(int32_t a, int32_t b, int32_t n, int32_t k) {
    if (n < 2) return a;
    const int32_t prod = static_cast<int32_t>(static_cast<uint32_t>(b - a) * static_cast<uint32_t>(k - 1));
    return a + Div(prod, n - 1);
}

struct Ctx {
    uint8_t* car;
    uint8_t* out;
    const SpecSource& db;
    BuildFrame& f;
    const BuildGlobals& g;
};

// 0x004462e0 / 0x00446320: FRONTTIRE byte 0x3a / REARTIRE byte 0x38 of the car's tyre rows, -1 without a key.
int64_t TireClass(Ctx& c, int slot, int buf, int byte) {
    const uint64_t key = L64(c.car, slot);
    if (key == kNone) return -1;
    c.db.Fetch(key, c.f.sub + buf);
    return c.f.sub[buf + byte];
}

// 0x00446820: MUFFLER byte 6, -1 without a key.
int64_t MufflerClass(Ctx& c) {
    const uint64_t key = L64(c.car, 0xd0);
    if (key == kNone) return -1;
    c.db.Fetch(key, c.f.sub + 0x100);
    return c.f.sub[0x100 + 6];
}

// 0x004461f8: GENERIC_CAR byte 0x11 -> 3 / 2 / (MUFFLER class > 0 ? 1 : 0).
uint8_t DriveClass(Ctx& c) {
    uint8_t* buf = c.f.sub + 0xe0;
    if (c.db.Fetch(L64(c.car, 0), buf) == 0) return 0;
    const uint8_t b = buf[0x11];
    if (b == 1) return 3;
    if (b == 2) return 2;
    if (b != 0 && b != 3) return 0;
    return MufflerClass(c) > 0 ? 1 : 0;
}

// 0x00441950 BRAKE
void Brake(Ctx& c, const uint8_t* r) {
    c.out[0x178] = r[3];
    c.out[0x179] = r[4];
    c.out[0x17a] = r[5];
}

// 0x00441970 BRAKECONTROLLER
void BrakeController(Ctx& c, const uint8_t* r) {
    c.out[0x1a8] = static_cast<uint8_t>(Interp(r[4], r[5], r[3], c.car[0x12f]));
    c.out[0x1a9] = static_cast<uint8_t>(Interp(r[8], r[9], r[7], c.car[0x130]));
}

// 0x004419e0 STEER
void Steer(Ctx& c, const uint8_t* r) {
    uint8_t* o = c.out;
    o[0x182] = 6;
    o[0x28] = r[0x25];
    for (int i = 0; i < 12; ++i) o[0x19c + i] = r[0x0d + i];
    o[0x180] = 6;
    for (int i = 0; i < 12; ++i) o[0x184 + i] = r[0x01 + i];
    o[0x181] = 6;
    for (int i = 0; i < 12; ++i) o[0x190 + i] = r[0x19 + i];
}

// 0x00441b20 CHASSIS
void Chassis(Ctx& c, const uint8_t* r) {
    uint8_t* o = c.out;
    o[0x22] = r[0x1c];
    o[0x2a] = static_cast<uint8_t>(r[0x0e] + 124);
    o[0x2b] = static_cast<uint8_t>(r[0x10] + 124);
    W16(o, 0x1a, L16(r, 4));
    W16(o, 0x20, L16(r, 6));
    o[0x24] = r[0x1d];
    o[0x27] = r[0x1e];
    W16(o, 0x10, L16(r, 0));
    W16(o, 0x3a, L16(r, 6));
    o[0x02] = r[0x1f];
    W16(o, 0x30, L16(r, 0x12));
    W16(o, 0x32, L16(r, 0x14));
    W16(o, 0x34, L16(r, 0x16));
    W16(o, 0x36, L16(r, 0x18));
    o[0x38] = static_cast<uint8_t>(r[0x22] + c.car[0x168]);
    o[0x39] = static_cast<uint8_t>(r[0x23] + c.car[0x168]);
    if (static_cast<uint64_t>(TireClass(c, 0x60, 0x40, 0x3a) - 6) < 5) {
        o[0x1e2] = r[0x20];
        o[0x1e4] = r[0x21];
    }
    if (static_cast<uint64_t>(TireClass(c, 0x68, 0x40, 0x38) - 6) < 5) {
        o[0x1e3] = r[0x20];
        o[0x1e5] = r[0x21];
    }
}

// 0x00441c58 ENGINE
void Engine(Ctx& c, const uint8_t* r) {
    uint8_t* o = c.out;
    for (int i = 0; i < 24; ++i) W16(o, 0xd8 + 2 * i, L16(r, 0x1e + 2 * i));
    for (int i = 0; i < 24; ++i) o[0xc0 + i] = r[0x55 + i];
    W16(o, 0x108, static_cast<uint32_t>(r[0x6f]) * 10);
    o[0x160] = r[0x53];
    o[0xbc] = r[0x52];
    o[0xbd] = r[0x51];
    o[0xbe] = r[0x50];
    o[0xbf] = r[0x54];
    o[0x110] = r[0x6d];
    o[0x111] = r[0x6e];
}

// 0x00441e28 DRIVETRAIN
void Drivetrain(Ctx& c, const uint8_t* r) {
    uint8_t* o = c.out;
    o[0x12a] = r[0x14];
    o[0x128] = r[0x10];
    o[0x129] = r[0x11];
    W16(o, 0xba, L16(r, 2));
    W16(o, 0x174, L16(r, 4));
    W16(o, 0x176, L16(r, 6));
    W16(o, 0x164, L16(r, 8));
    W16(o, 0x166, L16(r, 10));
    W16(o, 0xb8, L16(r, 0));
}

// 0x00441e78 GEAR
void Gear(Ctx& c, const uint8_t* r) {
    uint8_t* o = c.out;
    W16(o, 0x152, L16(r, 0x1e));
    o[0x154] = r[0x29];
    for (int i = 0; i < 12; ++i) W16(o, 0x138 + 2 * i, L16(c.car, 0x110 + 2 * i));
    W16(o, 0x150, L16(c.car, 0x128));
    o[0x157] = c.car[0x12a];
    o[0x156] = 0;
    o[0x155] = r[0x24];
    o[0x158] = r[0x23];
}

// 0x00441ee0 SUSPENSION
void Suspension(Ctx& c, const uint8_t* r) {
    uint8_t* o = c.out;
    const uint8_t* car = c.car;
    o[0x92] = r[0x2d];
    o[0x93] = r[0x2e];
    o[0x94] = r[0x31];
    o[0x95] = r[0x32];
    o[0x5a] = r[0x3c];
    o[0x72] = r[0x3d];
    for (int i = 0; i < 4; ++i) {
        o[0x1d0 + i] = r[0x17 + i];
        o[0x1d8 + i] = r[0x1b + i];
        o[0x1d4 + i] = r[0x1f + i];
        o[0x1dc + i] = r[0x23 + i];
    }
    o[0x90] = r[0x15];
    o[0x91] = r[0x16];
    o[0x5b] = r[0x64];
    o[0x73] = r[0x65];
    o[0x5c] = r[0x40];
    o[0x5e] = r[0x42];
    o[0x60] = r[0x44];
    o[0x62] = r[0x46];
    o[0x74] = r[0x41];
    o[0x76] = r[0x43];
    o[0x78] = r[0x45];
    o[0x7a] = r[0x47];
    W16(o, 0x66, r[0x2f]);
    W16(o, 0x7e, r[0x30]);
    o[0x65] = r[0x3e];
    o[0x7d] = r[0x3f];
    W16(o, 0x9a, L16(car, 0x13c));
    W16(o, 0x9c, L16(car, 0x13e));
    o[0x88] = car[0x139];
    o[0x89] = car[0x13a];
    o[0x8e] = car[0x140];
    o[0x8f] = car[0x141];
    o[0x58] = car[0x142];
    o[0x70] = car[0x143];
    o[0x8c] = car[0x144];
    o[0x8d] = car[0x145];
    o[0x5d] = static_cast<uint8_t>(Interp(r[0x49], r[0x4a], r[0x48], car[0x146]));
    o[0x5f] = static_cast<uint8_t>(Interp(r[0x4c], r[0x4d], r[0x48], car[0x147]));
    o[0x61] = static_cast<uint8_t>(Interp(r[0x50], r[0x51], r[0x4f], car[0x146]));
    o[0x63] = static_cast<uint8_t>(Interp(r[0x53], r[0x54], r[0x4f], car[0x147]));
    o[0x75] = static_cast<uint8_t>(Interp(r[0x57], r[0x58], r[0x56], car[0x14a]));
    o[0x77] = static_cast<uint8_t>(Interp(r[0x5a], r[0x5b], r[0x56], car[0x14b]));
    o[0x79] = static_cast<uint8_t>(Interp(r[0x5e], r[0x5f], r[0x5d], car[0x14c]));
    o[0x7b] = static_cast<uint8_t>(Interp(r[0x61], r[0x62], r[0x5d], car[0x14d]));
    o[0x59] = static_cast<uint8_t>(Interp(r[0x67], r[0x68], r[0x66], car[0x14e]));
    o[0x71] = static_cast<uint8_t>(Interp(r[0x6b], r[0x6c], r[0x6a], car[0x14f]));
    o[0xa8] = r[0x6e];
}

// 0x004421d0 LSD
void Lsd(Ctx& c, const uint8_t* r) {
    uint8_t* o = c.out;
    o[0x168] = r[3];
    o[0x169] = r[0x0f];
    for (int i = 0; i < 6; ++i) o[0x16a + i] = c.car[0x150 + i];
}

// 0x00442490(car, tyre, compound row, force-volume row): one 0xa8-byte tyre block.
void TyreBlock(uint8_t* t, const uint8_t* a, const uint8_t* b) {
    t[0] = a[1];
    t[2] = a[10];
    t[8] = 8;
    t[1] = a[0];
    for (int i = 0; i < 16; ++i) t[0x10 + i] = a[0x0b + i];
    t[9] = 6;
    for (int i = 0; i < 6; ++i) t[0x20 + i] = a[0x27 + i];
    for (int i = 0; i < 6; ++i) t[0x30 + i] = a[0x2d + i];
    for (int i = 0; i < 6; ++i) t[0x40 + i] = a[0x39 + i];
    t[10] = 6;
    for (int i = 0; i < 6; ++i) t[0x28 + i] = a[0x3f + i];
    for (int i = 0; i < 6; ++i) t[0x38 + i] = a[0x45 + i];
    for (int i = 0; i < 6; ++i) t[0x48 + i] = a[0x51 + i];
    t[0x0b] = 4;
    t[0x5c] = a[2];
    t[0x5d] = a[3];
    t[0x5e] = a[4];
    t[0x5f] = a[5];
    t[0x60] = a[6];
    t[0x61] = a[7];
    t[0x62] = a[8];
    t[0x0c] = 6;
    t[0x63] = a[9];
    for (int i = 0; i < 12; ++i) t[0x50 + i] = a[0x1b + i];
    t[3] = a[0x57];
    t[4] = a[0x58];
    t[5] = a[0x59];
    for (int i = 0; i < 10; ++i) t[0x64 + i] = b[i];
    for (int i = 0; i < 5; ++i) t[0x6e + i] = b[0x0d + i];
    t[0x78] = b[10];
    t[0x79] = b[0x0c];
    t[0x7a] = b[0x0b];
}

// 0x00442218 FRONTTIRE / 0x00442358 REARTIRE: three tyre blocks from the row's compound / force keys, then TIRESIZE.
void Tires(Ctx& c, const uint8_t* r, bool rear) {
    uint8_t* s = c.f.sub;   // callee frame (0x120): +0x40 compound row, +0xa0 force-volume row, +0xc0 TIRESIZE row
    uint8_t* block = c.out + (rear ? 0x408 : 0x210);
    for (int i = 0; i < 3; ++i) {
        c.db.Fetch(L64(r, 8 + 8 * i), s + 0x40);
        c.db.Fetch(L64(r, 0x20 + 8 * i), s + 0xa0);
        TyreBlock(block + 0xa8 * i, s + 0x40, s + 0xa0);
    }
    c.db.Fetch(L64(r, 0), s + 0xc0);
    const int k = rear ? 1 : 0;
    c.out[0x1ca + k] = s[0xc2];
    c.out[0x1cc + k] = s[0xc0];
    c.out[0x1c8 + k] = s[0xc3];
    c.out[0x1ce + k] = 4;
}

// 0x004427b8 LIGHTWEIGHT
void Lightweight(Ctx& c, const uint8_t* r) {
    uint8_t* o = c.out;
    W16(o, 0x20, static_cast<uint32_t>(Div(static_cast<int32_t>(S16(o, 0x20)) * static_cast<int32_t>(L16(r, 0)), 1000)));
    o[0x24] = static_cast<uint8_t>(Div(o[0x24] * r[5], 100));
}

// 0x00442800 RACINGMODIFY
void RacingModify(Ctx& c, const uint8_t* r) {
    uint8_t* o = c.out;
    W16(o, 0x20, static_cast<uint32_t>(Div(static_cast<int32_t>(S16(o, 0x20)) * static_cast<int32_t>(L16(r, 0)), 1000)));
    o[0x24] = static_cast<uint8_t>(Div(o[0x24] * r[0x17], 100));
    o[0x42] = r[0x0c];
    W16(o, 0x1c, L16(r, 2));
    W16(o, 0x1e, L16(r, 4));
    o[0x43] = c.car[0x131];
    o[0x44] = c.car[0x132];
    o[0x96] = r[0x13];
    o[0x97] = r[0x14];
    o[0x98] = r[0x15];
    o[0x99] = r[0x16];
    o[0x4c] = r[0x18];
    o[0x4d] = r[0x19];
    o[0x4e] = r[0x1a];
    o[0x4f] = r[0x1b];
    o[0x50] = r[0x1c];
}

// Power / weight scaling shared by several parts: +0x10a (u16) * a / 100, +0x10c (u8) * b / 100.
void ScalePower(uint8_t* o, uint32_t a, uint32_t b) {
    W16(o, 0x10a, static_cast<uint32_t>(Div(static_cast<int32_t>(L16(o, 0x10a) * a), 100)));
    o[0x10c] = static_cast<uint8_t>(Div(static_cast<int32_t>(o[0x10c] * b), 100));
}

// 0x004428b8 PORTPOLISH, 0x00442a58 DISPLACEMENT, 0x00442aa0 COMPUTER, 0x00442e80 MUFFLER, 0x00442ec8 INTERCOOLER,
// 0x004431f8 SUPERCHARGER
void PowerPart(Ctx& c, const uint8_t* r) { ScalePower(c.out, L16(r, 0), L16(r, 2)); }

// 0x00442900 ENGINEBALANCE (u16 0/2 power, s8 7 torque-curve offset, s8 8 +0xbd)
void EngineBalance(Ctx& c, const uint8_t* r) {
    uint8_t* o = c.out;
    for (int i = 0; i < 16; ++i) o[0xc0 + i] = static_cast<uint8_t>(o[0xc0 + i] + r[7]);
    o[0xbd] = static_cast<uint8_t>(o[0xbd] + r[8]);
    ScalePower(o, L16(r, 0), L16(r, 2));
}

// 0x00442ae8 NATUNE
void Natune(Ctx& c, const uint8_t* r) {
    uint8_t* o = c.out;
    for (int i = 0; i < 16; ++i) o[0xc0 + i] = static_cast<uint8_t>(o[0xc0 + i] + r[9]);
    o[0xbd] = static_cast<uint8_t>(o[0xbd] + r[10]);
    ScalePower(o, L16(r, 4), L16(r, 6));
}

// 0x00442c40 TURBINEKIT
void Turbine(Ctx& c, const uint8_t* r) {
    uint8_t* o = c.out;
    ScalePower(o, L16(r, 4), L16(r, 6));
    o[0x113] = r[9];
    o[0xbd] = static_cast<uint8_t>(o[0xbd] + r[0x10]);
    o[0xbe] = static_cast<uint8_t>(o[0xbe] + r[0x11]);
    o[0x118] = c.car[0x133];
    o[0x116] = c.car[0x134];
    o[0x11a] = c.car[0x135];
    o[0x119] = c.car[0x136];
    o[0x117] = c.car[0x137];
    o[0x11b] = c.car[0x138];
}

inline void ScaleU16(uint8_t* o, int off, uint32_t pct) {
    W16(o, off, static_cast<uint32_t>(Div(static_cast<int32_t>(L16(o, off) * pct), 100)));
}
inline void ScaleS16(uint8_t* o, int off, uint32_t pct) {
    W16(o, off, static_cast<uint32_t>(Div(static_cast<int32_t>(S16(o, off)) * static_cast<int32_t>(pct), 100)));
}

// 0x00442ce0 FLYWHEEL
void Flywheel(Ctx& c, const uint8_t* r) {
    uint8_t* o = c.out;
    ScaleU16(o, 0xba, r[4]);
    ScaleU16(o, 0xb8, r[3]);
    ScaleS16(o, 0x174, r[5]);
    ScaleS16(o, 0x176, r[6]);
}

// 0x00442d60 CLUTCH
void Clutch(Ctx& c, const uint8_t* r) {
    Flywheel(c, r);
    c.out[0x161] = r[7];
}

// 0x00442de8 PROPELLERSHAFT
void Propeller(Ctx& c, const uint8_t* r) {
    uint8_t* o = c.out;
    ScaleU16(o, 0xba, r[3]);
    ScaleS16(o, 0x174, r[4]);
    ScaleS16(o, 0x176, r[5]);
    ScaleS16(o, 0x164, r[6]);
    ScaleS16(o, 0x166, r[7]);
}

// 0x003f16c0 / 0x003f16d0 / 0x003f1748
void MaskAscc(uint8_t* o) { o[0x1b8] = static_cast<uint8_t>(o[0x1b8] & 0xf0); }
void MaskTcs(uint8_t* o, uint8_t flag) { o[0x1b8] = static_cast<uint8_t>(o[0x1b8] & (flag ? 0xcf : 0x0f)); }

// 0x00442f10 ASCC
void Ascc(Ctx& c, const uint8_t* r) {
    uint8_t* o = c.out;
    const uint8_t* car = c.car;
    o[0x1ba] = static_cast<uint8_t>(Interp(r[5], r[6], r[4], car[0x157] ? car[0x157] : 1));
    o[0x1bb] = r[8];
    o[0x1bc] = static_cast<uint8_t>(Interp(r[0x0b], r[0x0c], r[0x0a], car[0x158]));
    o[0x1bd] = static_cast<uint8_t>(Interp(r[0x10], r[0x11], r[0x0f], car[0x159] ? car[0x159] : 1));
    o[0x1be] = r[0x13];
    o[0x1bf] = static_cast<uint8_t>(Interp(r[0x16], r[0x17], r[0x15], car[0x15a]));
    o[0x1b8] = static_cast<uint8_t>((r[3] & 3) | (r[9] & 3) << 2 | (r[0x0e] & 3) << 4 | (r[0x14] & 3) << 6);
    if (car[0x157] == 0) MaskAscc(o);
    if (car[0x159] == 0) MaskTcs(o, c.g.flag006224a8);
}

// 0x00443040 TCSC
void Tcsc(Ctx& c, const uint8_t* r) {
    uint8_t* o = c.out;
    const uint8_t k = c.car[0x156];
    o[0x1c2] = r[5];
    o[0x1c0] = r[3];
    o[0x1c1] = r[4];
    o[0x1c3] = static_cast<uint8_t>(Interp(r[7], r[8], r[6], k ? k : 1));
    if (c.car[0x156] == 0) o[0x1c2] = 0;
}

// 0x004458d0(car, colour, out): VARIATION row of the car's colour into out; 1 when found.
uint32_t ColourRow(Ctx& c, int32_t colour, uint8_t* out) {
    const int32_t count = c.db.Count(L64(c.car, 0x10));
    if (!(colour < count)) return 0;
    const int32_t first = c.db.FirstIndex(kVariationTable, L32(c.car, 0x10));
    if (first == -1 || count <= 0) return 0;
    int32_t index = first;
    for (int32_t i = 0; i < count;) {
        ++i;
        c.db.RowAt(kVariationTable, static_cast<uint32_t>(index), out);
        if (static_cast<int32_t>(L32(out, 4)) == colour + 1) return 1;
        index = first + i;
    }
    return 0;
}

// float -> u16 field as 0x004430f0 does (values >= 2^31 converted after subtracting 2^31).
uint16_t FloatField(uint32_t f) {
    constexpr uint32_t k2p31 = 0x4f000000u;
    if (ee::FpuLe(k2p31, f)) return static_cast<uint16_t>(ee::FpuCvtWS(ee::FpuSub(f, k2p31, nullptr)));
    return static_cast<uint16_t>(ee::FpuCvtWS(f));
}

// 0x004430f0 VARIATION floats
void Variation(Ctx& c, const uint8_t* r) {
    uint8_t* o = c.out;
    W16(o, 0x16, FloatField(L32(r, 0x14)));
    W16(o, 0x18, FloatField(L32(r, 0x10)));
    W16(o, 0x12, FloatField(L32(r, 0x18)));
    W16(o, 0x14, FloatField(L32(r, 0x1c)));
    W16(o, 0x40, ee::FpuCvtWS(ee::FpuMul(L32(r, 0x20), 0x461c4000u, nullptr)));   // * 10000
}

} // namespace

void BuildCarParams(uint8_t* car, uint8_t* out, const SpecSource& db, BuildFrame& frame, const BuildGlobals& g) {
    Ctx c{car, out, db, frame, g};
    uint8_t* sp = frame.frame;
    auto fetch = [&](int slot, int buf) { db.Fetch(L64(car, slot), sp + buf); };
    auto has = [&](int slot) { return L64(car, slot) != kNone; };
    out[0x10c] = 100;
    W16(out, 0x10a, 1000);
    out[0x113] = 0;
    fetch(0x20, 0x00), Brake(c, sp);
    fetch(0x28, 0x10), BrakeController(c, sp + 0x10);
    fetch(0x70, 0x20), Steer(c, sp + 0x20);
    fetch(0x30, 0x00), Chassis(c, sp);
    fetch(0x38, 0x50), Engine(c, sp + 0x50);
    fetch(0x40, 0x00), Drivetrain(c, sp);
    fetch(0x48, 0x00), Gear(c, sp);
    fetch(0x50, 0x00), Suspension(c, sp);
    fetch(0x58, 0x00), Lsd(c, sp);
    fetch(0x60, 0x00), Tires(c, sp, false);
    fetch(0x68, 0x00), Tires(c, sp, true);
    if (has(0x78)) fetch(0x78, 0), Lightweight(c, sp);
    fetch(0x80, 0x00), RacingModify(c, sp);
    if (has(0x88)) fetch(0x88, 0), PowerPart(c, sp);
    if (has(0x90)) fetch(0x90, 0), EngineBalance(c, sp);
    if (has(0x98)) fetch(0x98, 0), PowerPart(c, sp);
    if (has(0xa0)) fetch(0xa0, 0), PowerPart(c, sp);
    if (has(0xa8)) fetch(0xa8, 0), Natune(c, sp);
    fetch(0xb0, 0x00), Turbine(c, sp);
    if (has(0xb8)) fetch(0xb8, 0), Flywheel(c, sp);
    if (has(0xc0)) fetch(0xc0, 0), Clutch(c, sp);
    if (has(0xc8)) fetch(0xc8, 0), Propeller(c, sp);
    if (has(0xd0)) fetch(0xd0, 0), PowerPart(c, sp);
    if (has(0xd8)) fetch(0xd8, 0), PowerPart(c, sp);
    if (has(0x108)) fetch(0x108, 0), PowerPart(c, sp);
    fetch(0xe0, 0x00), Ascc(c, sp);
    fetch(0xe8, 0x00), Tcsc(c, sp);
    out[5] = car[0x15b];   // 0x004430b8
    out[6] = car[0x15c];
    if (has(0x100)) fetch(0x100, 0);   // WING: 0x004430d0 is empty
    if (has(0xf8)) {                   // NOS: 0x004430d8
        fetch(0xf8, 0);
        out[0x11d] = car[0x167];
        W16(out, 0x11e, L16(sp, 0));
    }
    if (static_cast<int8_t>(car[0x18]) == -1) car[0x18] = 0;
    ColourRow(c, static_cast<int8_t>(car[0x18]), sp);
    Variation(c, sp);
    out[0x25] = out[0x24];
    out[0x26] = static_cast<uint8_t>(out[0x24] >> 1);
    out[0x120] = DriveClass(c);
    fetch(0x00, 0x00);
    out[1] = sp[0x13];
    out[4] = sp[0x14];
    for (uint32_t i = static_cast<uint32_t>(out[0x155]) + 1; i < 12; ++i) W16(out, 0x138 + 2 * static_cast<int>(i), 0xffff);
    for (uint32_t i = out[0xbf]; i < 16; ++i) {
        out[0xc0 + i] = 0xff;
        W16(out, 0xd8 + 2 * static_cast<int>(i), 0);
    }
    out[7] = car[0x163];
    out[8] = car[0x162];
    if (L16(car, 0x15e) != 0) W16(out, 0x108, L16(car, 0x15e));
}

RaceGlobals ReadRaceGlobals(const uint8_t* ram) {
    RaceGlobals g;
    g.build.flag006224a8 = ram[0x006224a8];
    g.d620e22 = ram[0x00620e22];
    g.d620e23 = ram[0x00620e23];
    g.d620e15 = ram[0x00620e15];
    g.d620e16 = ram[0x00620e16];
    g.d6224a0 = ram[0x006224a0];
    g.d6224a1 = ram[0x006224a1];
    g.d6224a2 = ram[0x006224a2];
    g.d6224a3 = ram[0x006224a3];
    g.d6224a4 = ram[0x006224a4];
    g.d6224a5 = ram[0x006224a5];
    std::memcpy(g.d6a4028, ram + 0x006a4028, sizeof g.d6a4028);
    std::memcpy(g.d622420, ram + 0x00622420, 6);
    std::memcpy(g.d622428, ram + 0x00622428, 6);
    return g;
}

void BuildRaceCarParams(uint8_t* o, uint8_t* car, int32_t mode, const SpecSource& db, BuildFrame& frame,
                        const RaceGlobals& g) {
    std::memset(o, 0, kCprmSize);
    BuildCarParams(car, o, db, frame, g.build);
    o[0] = 2;
    if (o[0x11c] == 0) o[0x11c] = g.d620e22;
    if (o[0x112] == 0) o[0x112] = g.d620e23;
    const uint8_t kind = o[1];
    if (kind == 3 || kind == 2 || kind == 4) {
        o[0x11c] = 0;
        o[0x112] = 0;
    }
    if (kind == 2 && static_cast<uint32_t>(mode - 2) < 2) {
        o[0xa8] = 3;
        W16(o, 0x66, L16(o, 0x66) + g.d6224a1);
        W16(o, 0x7e, L16(o, 0x7e) + g.d6224a1);
        o[0x92] = static_cast<uint8_t>(o[0x92] + g.d6224a2);
        o[0x93] = static_cast<uint8_t>(o[0x93] + g.d6224a2);
        W16(o, 0x9a, L16(o, 0x9a) + g.d6224a0);
        W16(o, 0x9c, L16(o, 0x9c) + g.d6224a0);
        o[0x94] = static_cast<uint8_t>(o[0x94] + g.d6224a3);
        o[0x95] = static_cast<uint8_t>(o[0x95] + g.d6224a3);
        o[0x58] = g.d6224a4;
        o[0x70] = g.d6224a5;
    }
    auto half = [](int16_t v) { return static_cast<uint32_t>((v + (static_cast<uint16_t>(v) >> 15)) >> 1); };
    if (S16(o, 0x12) == 0) W16(o, 0x12, half(S16(o, 0x10)));
    if (S16(o, 0x14) == 0) W16(o, 0x14, half(S16(o, 0x10)));
    if (g.d620e15) o[0x25] = static_cast<uint8_t>(Div(o[0x24] * g.d620e15, 100));
    if (g.d620e16) o[0x26] = static_cast<uint8_t>(Div(o[0x24] * g.d620e16, 100));
    const uint8_t* steerA = g.d6a4028;       // 6 bytes at 0x006a4028
    const uint8_t* steerB = g.d6a4028 + 8;   // 6 bytes at 0x006a4030
    if (o[0x182] == 0) {
        o[0x182] = 6;
        std::memcpy(o + 0x19c, steerA, 6);
        std::memcpy(o + 0x1a2, steerB, 6);
    }
    if (o[0x195] == 0) {
        o[0x181] = 6;
        std::memcpy(o + 0x190, steerA, 6);
        std::memcpy(o + 0x196, steerB, 6);
    }
    if (o[0x1aa] == 0) o[0x1aa] = 0x8c;
    if (o[0x1ab] == 0) o[0x1ab] = 100;
    if (o[0x1c3] == 100) o[0x1c2] = 0;
    o[0x1ff] = 1;
    o[0x207] = 2;
    o[0x1e0] = 1;
    o[0x1f2] = 1;
    o[0x1f3] = 1;
    o[0x1f4] = 1;
    o[0x1f5] = 1;
    o[0x1fd] = 1;
    o[0x1fe] = 1;
    o[0x1f6] = 2;
    o[0x204] = 2;
    o[0x205] = 2;
    o[0x206] = 2;
    for (int t = 0; t < 6; ++t) {
        uint8_t* b = o + 0x210 + 0xa8 * t;
        for (int i = 0; i < 5; ++i)
            if (b[0x6e + i] == 0) b[0x6e + i] = 100;
        if (b[0x73] == 0) b[0x73] = 0x50;
    }
    uint8_t* b0 = o + 0x210;
    if (b0[0x78] == 0) b0[0x78] = b0[0x64];
    if (b0[0x79] == 0) b0[0x79] = b0[0x65];
    if (b0[0x7a] == 0) b0[0x7a] = b0[0x6d];
    for (int t = 0; t < 6; ++t) {
        uint8_t* b = o + 0x210 + 0xa8 * t;
        if (b[2] == 0) b[2] = 0x2d;
        if (b[0x0c] == 0) {
            b[0x0c] = 6;
            for (int i = 0; i < 6; ++i) {
                b[0x50 + i] = g.d622420[i];
                b[0x56 + i] = g.d622428[i];
            }
        }
        for (int i = 0; i < 0x20; ++i)
            if (b[0x64 + i] == 0) b[0x64 + i] = b0[0x64 + i];
    }
}

} // namespace gt4::game::carparams
