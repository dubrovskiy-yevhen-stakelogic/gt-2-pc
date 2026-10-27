// Garage car from a car code and the ShowRoomCar metric - see garage_car.h. Transcribed from the R5900 code of
// CORE.GT4 US 1.01 (addresses per function). Row offsets are the byte offsets of the decoded SpecDB rows.
#include "game/menu/garage_car.h"

#include <array>
#include <cstring>
#include <vector>

namespace gt4::game::menu {
namespace {

using carparams::Guest;
using carparams::SpecSource;

struct Car {
    uint8_t* p;
    uint64_t K(uint32_t o) const {
        uint64_t v;
        std::memcpy(&v, p + o, 8);
        return v;
    }
    void SetK(uint32_t o, uint64_t v) { std::memcpy(p + o, &v, 8); }
    void B(uint32_t o, uint32_t v) { p[o] = static_cast<uint8_t>(v); }
    void H(uint32_t o, uint32_t v) {
        p[o] = static_cast<uint8_t>(v);
        p[o + 1] = static_cast<uint8_t>(v >> 8);
    }
};

// Decoded row buffer; bytes beyond the fetched row keep their previous contents like the original's stack buffer.
struct Row {
    std::array<uint8_t, 0x400> b{};
    uint32_t u8(uint32_t o) const { return b[o]; }
    uint32_t u16(uint32_t o) const { return static_cast<uint32_t>(b[o] | b[o + 1] << 8); }
    uint64_t u64(uint32_t o) const {
        uint64_t v;
        std::memcpy(&v, &b[o], 8);
        return v;
    }
};

// 0x004452a8(car, lo, value, hi): value < lo -> lo; hi < value -> hi (signed compares of the zero-extended args)
uint32_t Clamp(int32_t lo, int32_t value, int32_t hi) {
    int32_t v = value;
    if (v < lo) v = lo;
    if (hi < v) v = hi;
    return static_cast<uint32_t>(v);
}

// The settings routines: exists(key) then fetch into the frame buffer (fresh per call: bytes not covered by the row
// are never read for existing rows of these tables).
bool FetchPart(const SpecSource& db, uint64_t key, Row* r) {
    if (!db.Exists(key)) return false;
    db.Fetch(key, r->b.data());
    return true;
}

// 0x00444a88: slot +0x48 (GEAR)
bool SetGear(Car c, const SpecSource& db) {
    Row r;
    if (!FetchPart(db, c.K(0x48), &r)) return false;
    c.H(0x112, r.u16(0));
    c.H(0x11a, r.u16(8));
    c.H(0x11c, r.u16(10));
    c.H(0x11e, r.u16(12));
    c.H(0x120, r.u16(14));
    c.H(0x122, r.u16(16));
    c.H(0x124, r.u16(18));
    c.H(0x126, r.u16(20));
    c.H(0x110, r.u16(22));
    c.H(0x114, r.u16(2));
    c.H(0x116, r.u16(4));
    c.H(0x118, r.u16(6));
    c.H(0x128, Clamp(static_cast<int32_t>(r.u16(24)), static_cast<int32_t>(r.u16(28)), static_cast<int32_t>(r.u16(26))));
    c.B(0x12a, Clamp(static_cast<int32_t>(r.u8(38)), static_cast<int32_t>(r.u8(40)), static_cast<int32_t>(r.u8(39))));
    c.H(0x12c, static_cast<uint32_t>(c.p[0x128] | c.p[0x129] << 8));
    return true;
}

uint32_t C3(const Row& r, uint32_t lo, uint32_t val, uint32_t hi) {   // byte operands
    return Clamp(static_cast<int32_t>(r.u8(lo)), static_cast<int32_t>(r.u8(val)), static_cast<int32_t>(r.u8(hi)));
}
uint32_t C1(const Row& r, uint32_t val, uint32_t hi) {   // lo = 1
    return Clamp(1, static_cast<int32_t>(r.u8(val)), static_cast<int32_t>(r.u8(hi)));
}

// 0x00444b88: slot +0x40
bool SetB88(Car c, const SpecSource& db) {
    Row r;
    if (!FetchPart(db, c.K(0x40), &r)) return false;
    c.B(0x12e, C3(r, 18, 20, 19));
    return true;
}
// 0x00444c08: slot +0x28
bool SetC08(Car c, const SpecSource& db) {
    Row r;
    if (!FetchPart(db, c.K(0x28), &r)) return false;
    c.B(0x12f, C1(r, 6, 3));
    c.B(0x130, C1(r, 10, 7));
    return true;
}
// 0x00444ca0: slot +0x80
bool SetCa0(Car c, const SpecSource& db) {
    Row r;
    if (!FetchPart(db, c.K(0x80), &r)) return false;
    c.B(0x131, C3(r, 13, 15, 14));
    c.B(0x132, C3(r, 16, 18, 17));
    return true;
}
// 0x00444d38: slot +0xb0
bool SetD38(Car c, const SpecSource& db) {
    Row r;
    if (!FetchPart(db, c.K(0xb0), &r)) return false;
    c.B(0x133, r.u8(10));
    c.B(0x138, r.u8(15));
    c.B(0x134, r.u8(11));
    c.B(0x135, r.u8(12));
    c.B(0x136, r.u8(13));
    c.B(0x137, r.u8(14));
    return true;
}
// 0x00444dd0: slot +0x50
bool SetDd0(Car c, const SpecSource& db) {
    Row r;
    if (!FetchPart(db, c.K(0x50), &r)) return false;
    c.B(0x141, r.u8(44));
    c.B(0x140, r.u8(41));
    c.B(0x139, C3(r, 15, 17, 16));
    c.B(0x13a, C3(r, 18, 20, 19));
    c.H(0x13c, Clamp(static_cast<int32_t>(r.u16(0)), static_cast<int32_t>(r.u16(4)), static_cast<int32_t>(r.u16(2))));
    c.H(0x13e, Clamp(static_cast<int32_t>(r.u16(6)), static_cast<int32_t>(r.u16(10)), static_cast<int32_t>(r.u16(8))));
    c.B(0x142, C3(r, 52, 54, 53));
    const uint32_t v143 = C3(r, 55, 57, 56);
    c.B(0x144, r.u8(58));
    c.B(0x143, v143);
    c.B(0x145, r.u8(59));
    c.B(0x146, C1(r, 75, 72));
    c.B(0x147, C1(r, 78, 72));
    c.B(0x148, C1(r, 82, 79));
    c.B(0x149, C1(r, 85, 79));
    c.B(0x14a, C1(r, 89, 86));
    c.B(0x14b, C1(r, 92, 86));
    c.B(0x14c, C1(r, 96, 93));
    c.B(0x14d, C1(r, 99, 93));
    c.B(0x14e, C1(r, 105, 102));
    c.B(0x14f, C1(r, 109, 106));
    return true;
}
// 0x00444fe0: slot +0x58
bool SetFe0(Car c, const SpecSource& db) {
    Row r;
    if (!FetchPart(db, c.K(0x58), &r)) return false;
    c.B(0x150, C3(r, 4, 6, 5));
    c.B(0x151, C3(r, 16, 18, 17));
    c.B(0x152, C3(r, 8, 10, 9));
    c.B(0x153, C3(r, 20, 22, 21));
    c.B(0x154, C3(r, 12, 14, 13));
    c.B(0x155, C3(r, 24, 26, 25));
    return true;
}
// 0x004450d8: slot +0xe0
bool Set0d8(Car c, const SpecSource& db) {
    Row r;
    if (!FetchPart(db, c.K(0xe0), &r)) return false;
    c.B(0x157, C1(r, 7, 4));
    c.B(0x158, C1(r, 13, 10));
    c.B(0x159, C1(r, 18, 15));
    c.B(0x15a, C1(r, 24, 21));
    return true;
}
// 0x004451a0: slot +0xe8
bool Set1a0(Car c, const SpecSource& db) {
    Row r;
    if (!FetchPart(db, c.K(0xe8), &r)) return false;
    c.B(0x156, C1(r, 9, 6));
    return true;
}
// 0x00445230: slot +0xf8 (absent key = success)
bool Set230(Car c, const SpecSource& db) {
    const uint64_t key = c.K(0xf8);
    if (key == ~0ull) return true;
    Row r;
    if (!FetchPart(db, key, &r)) return false;
    c.B(0x167, r.u8(5));
    return true;
}

// 0x004449c0: the chain stops at the first failure
bool SettingsDefaults(Car c, const SpecSource& db) {
    if (!SetGear(c, db) || !SetB88(c, db) || !SetC08(c, db) || !SetCa0(c, db) || !SetD38(c, db) || !SetDd0(c, db) ||
        !SetFe0(c, db) || !Set0d8(c, db) || !Set1a0(c, db))
        return false;
    c.B(0x15c, 0);   // 0x00445220
    c.B(0x15b, 0);
    return Set230(c, db);
}

// 0x004457b0: CAR_VARIATION (table 41) row of the generic id (u32 of car+0) -> its first 8 bytes, -1 when absent
uint64_t VariationKey(Car c, const SpecSource& db) {
    const uint32_t id = static_cast<uint32_t>(c.K(0));
    Row r;
    if (!db.Fetch(static_cast<uint64_t>(41) << 32 | id, r.b.data())) return ~0ull;
    return r.u64(0);
}

// 0x00444860(car, key): extra settings of an ENEMY_CARS entry
uint32_t EnemyExtra(Car c, uint64_t key, const SpecSource& db) {
    Row r;
    if (!db.Fetch(key, r.b.data())) return 0;
    c.H(0x128, r.u16(2));
    c.B(0x12a, r.u8(20));
    c.B(0x150, r.u8(14));
    c.B(0x152, r.u8(15));
    c.B(0x154, r.u8(16));
    c.B(0x151, r.u8(17));
    c.B(0x153, r.u8(18));
    c.B(0x155, r.u8(19));
    c.B(0x131, r.u8(12));
    c.B(0x132, r.u8(13));
    c.B(0x146, r.u8(25));
    c.B(0x147, r.u8(26));
    c.B(0x148, r.u8(27));
    c.B(0x149, r.u8(28));
    c.B(0x14a, r.u8(29));
    c.B(0x14b, r.u8(30));
    c.B(0x14c, r.u8(31));
    c.B(0x14d, r.u8(32));
    c.B(0x139, r.u8(21));
    c.B(0x13a, r.u8(22));
    c.B(0x140, r.u8(23));
    c.B(0x141, r.u8(24));
    c.B(0x14e, r.u8(33));
    c.B(0x14f, r.u8(34));
    c.H(0x13c, r.u16(4));
    c.H(0x13e, r.u16(6));
    c.B(0x142, r.u8(8));
    c.B(0x143, r.u8(10));
    c.B(0x156, r.u8(36));
    c.B(0x157, r.u8(35));
    if (c.K(8) != ~0ull) c.H(0x15e, r.u16(0));   // 0x00445b78: car+8 != -1
    return 1;
}

// DEFAULT_PARTS row offset -> car slot (0x00444480 / 0x00444690 copy the same layout)
struct PartCopy {
    uint16_t slot, off;
};
constexpr PartCopy kDefaultParts[28] = {
    {0x20, 0x00}, {0x28, 0x08}, {0x30, 0x28}, {0x38, 0x58}, {0x40, 0x48}, {0x48, 0x50}, {0x50, 0x10},
    {0x58, 0xb8}, {0x60, 0xc0}, {0x68, 0xc8}, {0x70, 0x40}, {0x78, 0x38}, {0x80, 0x30}, {0x88, 0x70},
    {0x90, 0x78}, {0x98, 0x80}, {0xa0, 0x88}, {0xa8, 0x60}, {0xb0, 0x68}, {0xb8, 0xa8}, {0xc0, 0xa0},
    {0xc8, 0xb0}, {0xd0, 0x98}, {0xd8, 0x90}, {0xe0, 0x18}, {0xe8, 0x20}, {0xf0, 0xffff}, {0x08, 0xffff},
};

// 0x00444480 (generic) / 0x00444690 (enemy)
uint32_t FromRow(Guest g, uint32_t carAddr, uint64_t key, bool enemy, uint32_t sp, const SpecSource& db) {
    if (!db.Exists(key)) return 0;
    Row first;
    db.Fetch(key, first.b.data());
    const uint64_t partsKey = first.u64(enemy ? 8 : 0);
    if (partsKey == ~0ull || !db.Exists(partsKey)) return 0;
    Row parts;
    db.Fetch(partsKey, parts.b.data());
    GarageCarConstruct(g, carAddr);
    Car c{g.ram + carAddr};
    if (enemy) {
        c.SetK(8, key);
        c.SetK(0, first.u64(0));
    } else {
        c.SetK(0, key);
    }
    for (const PartCopy& pc : kDefaultParts) {
        const uint32_t slot = pc.slot, off = pc.off;
        if (off == 0xffff) {
            if (slot == 0xf0 || !enemy) c.SetK(slot, ~0ull);
            continue;
        }
        c.SetK(slot, parts.u64(off));
    }
    SettingsDefaults(c, db);   // result ignored by both callers
    const uint64_t var = VariationKey(c, db);
    c.SetK(0x10, var);
    if (var == ~0ull) return 0;
    c.B(0x18, 0);
    if (enemy && first.u64(0x10) != ~0ull) EnemyExtra(c, first.u64(0x10), db);
    return carparams::GarageGearRatios(g, carAddr, sp, db) != 0 ? 1 : 0;
}

}  // namespace

void GarageCarConstruct(Guest g, uint32_t car) {
    uint8_t* p = g.ram + car;
    std::memset(p, 0xff, 0x18);           // +0x00 / +0x08 / +0x10 keys -1
    p[0x18] = 0xff;                       // colour byte (+0x19..+0x1f untouched)
    std::memset(p + 0x20, 0xff, 0xf0);    // +0x20 .. +0x10f part keys -1
    p[0x166] = 1;
    p[0x161] = 0xff;
    p[0x164] = 100;
    p[0x15e] = 0;
    p[0x15f] = 0;
    p[0x160] = 0xff;
    p[0x162] = 100;
    p[0x163] = 100;
    p[0x165] = 0;
    p[0x167] = 0;
    p[0x168] = 0;
    for (uint32_t a = 0x170; a >= 0x169; --a) p[a] = 0;   // 8 bytes downwards from +0x170
}

uint32_t GarageCarFromKey(Guest g, uint32_t car, uint64_t key, uint32_t sp, const SpecSource& db) {
    // 0x00444440: dsra32 key == 38 -> 0x00444690
    const bool enemy = static_cast<int64_t>(key) >> 32 == 0x26;
    return FromRow(g, car, key, enemy, enemy ? sp - 0x10 - 0x130 : sp - 0x10 - 0x120, db);
}

uint32_t ShowRoomCarMetric(Guest g, uint32_t work, uint32_t car, const SpecSource& db, carparams::BuildFrame& frame) {
    carparams::SetupCar(g, work, g.ram + car, db, frame, 0);
    uint32_t v;
    std::memcpy(&v, g.ram + work + 0x15e0, 4);
    return v;
}

}  // namespace gt4::game::menu
