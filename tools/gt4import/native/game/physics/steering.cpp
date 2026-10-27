#include "game/physics/steering.h"

#include "ee/fpu.h"
#include "game/math/geom.h"
#include "game/math/libm.h"
#include "game/math/raw.h"

namespace gt4::game {

namespace {
using namespace gt4::ee;
using raw::Ld;
using raw::St;

inline uint32_t Mul(uint32_t a, uint32_t b) { return FpuMul(a, b, nullptr); }
inline uint32_t Add(uint32_t a, uint32_t b) { return FpuAdd(a, b, nullptr); }
inline uint32_t Sub(uint32_t a, uint32_t b) { return FpuSub(a, b, nullptr); }
inline uint32_t Div(uint32_t a, uint32_t b) { return FpuDiv(a, b, nullptr); }

constexpr uint32_t kHalf = 0x3f000000u, kOne = 0x3f800000u, kPi = 0x40490fdau, kTwoPi = 0x40c90fdau;
constexpr uint32_t kEntries = 0x00620478u, kAssist = 0x00845bf0u, kServo = 0x00620b00u;

GeoCurve CurveAt(const GuestBytes& mem, uint32_t header) {
    const int16_t n = static_cast<int16_t>(mem.U16(header));
    const uint32_t span = 4u * static_cast<uint32_t>(n > 0 ? n : 1) + 8u;
    const uint8_t* xs = mem.At(mem.U32(header + 4) - 4, span) + 4;
    const uint8_t* ys = mem.At(mem.U32(header + 8) - 4, span) + 4;
    return GeoCurve{n, xs, ys};
}

inline int8_t S8(uint8_t v) { return static_cast<int8_t>(v); }
} // namespace

uint32_t PhysWheelSteerAngle(const uint8_t* spec, const uint8_t* entry, int64_t i, uint32_t x) {
    const int n = entry[1];
    const bool inRange = i < n;
    uint32_t a = 0;
    if (inRange && S8(entry[60 + i]) > 0) {
        a = x;
        const uint8_t side = entry[28 + i];
        if (side < 2) {
            const uint32_t idx = FpuLe(0, x) ? side : 1u - side;
            a = Mul(a, Ld(spec, 268 + 4 * idx));
        }
    } else if (inRange && S8(entry[60 + i]) < 0 && spec[4304] == 1) {
        a = Mul(FpuNeg(x), Ld(spec, 4300));
        const uint8_t side = entry[28 + i];
        if (side < 2) {
            const uint32_t idx = FpuLe(0, x) ? side : 1u - side;
            a = Mul(a, Ld(spec, 268 + 4 * idx));
        }
    }
    const uint8_t toeSide = entry[28 + i];
    const uint32_t k = inRange ? entry[20 + i] : 0u;
    const uint32_t toe = Ld(spec, 4104 + 4 * k);
    if (toeSide == 0) a = Sub(a, toe);
    else if (toeSide == 1) a = Add(a, toe);
    return a;
}

uint32_t PhysSteerGeometry(uint32_t a, uint32_t b) {
    const uint32_t ca = LibmCos(a);
    const uint32_t cb = LibmCos(b);
    return Mul(ca, Sub(0x40400000u, Add(cb, cb)));
}

uint32_t PhysWheelSteerLayout1(const GuestBytes& mem, uint32_t car, int64_t i) {
    const uint8_t* c = mem.At(car, 0x10a0);
    const uint8_t* spec = mem.At(Ld(c, 0x10), 0x10e0);
    const uint8_t* entry = mem.At(kEntries + 84u * spec[0x30], 84);
    if (!(i < entry[1] && S8(entry[60 + i]) > 0)) return 0;
    uint32_t k = spec[51];
    if (k >= 3) k = 0;
    const uint32_t table = mem.U32(0x006205c8u + 120u * k + 48);
    return Mul(Ld(c, 0x56c), PhysSteerGeometry(table, Sub(Ld(c, 0x670), Ld(c, 0x67c))));
}

void PhysSteeringWheel(uint8_t* c) {
    constexpr uint32_t kStep = 0x3c888888u;
    uint32_t angle = Ld(c, 0x768), rate = Ld(c, 0x76c);
    const uint32_t acc = Sub(Mul(Sub(Mul(Ld(c, 0x56c), 0x41340000u), angle), 0x43480000u), Mul(rate, 0x41a00000u));
    St(c, 0x770, acc);
    rate = Add(rate, Mul(acc, kStep));
    St(c, 0x76c, rate);
    angle = Add(angle, Mul(rate, kStep));
    St(c, 0x768, angle);
}

uint32_t GeoAngleBlend(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t wb = GeoWrapRadians(b), wd = GeoWrapRadians(d);
    const uint32_t diff = Sub(wb, wd);
    if (FpuLe(kPi, diff)) wd = Add(wd, kTwoPi);
    else if (FpuLt(diff, FpuNeg(kPi))) wb = Add(wb, kTwoPi);
    return GeoWrapRadians(Add(Mul(a, wb), Mul(c, wd)));
}

void PhysSteering(const GuestBytes& mem, uint32_t car) {
    uint8_t* c = mem.At(car, 0x10a0);
    const uint32_t specAddress = Ld(c, 0x10);
    const uint8_t* spec = mem.At(specAddress, 0x10e0);
    const uint8_t* entry = mem.At(kEntries + 84u * spec[0x30], 84);
    for (int i = 0; i < entry[1]; ++i) {
        uint8_t* w = c + 356 + 236 * i;
        const uint32_t a = entry[0] == 1 ? PhysWheelSteerLayout1(mem, car, i) : PhysWheelSteerAngle(spec, entry, i, Ld(c, 0x56c));
        St(w, 0, a);
    }
    PhysSteeringWheel(c);
    const uint8_t* g = mem.At(kAssist, 8);
    if (raw::Ld(g, 0) != 0) {
        for (int i = 0; i < entry[1]; ++i) {
            uint8_t* w = c + 356 + 236 * i;
            const bool locked = mem.U8(0x00620288u + w[137]) == 1;
            const uint32_t dt = Ld(c, 0x54c);
            const uint32_t axle = entry[44 + i];
            const uint32_t lim0 = mem.U32(kAssist + 8 + 4 * axle);
            const bool mode2 = mem.U32(kAssist) == 2;
            uint32_t target;
            if (!mode2) {
                if (locked) {
                    target = 0;
                } else if (FpuEq(Ld(w, 96), 0)) {
                    target = Sub(Ld(w, 28), Mul(Sub(Ld(w, 0), Ld(c, 0x8e0 + 4 * i)), kHalf));
                    if (FpuLt(target, FpuNeg(lim0))) target = FpuNeg(lim0);
                    else if (FpuLt(lim0, target)) target = lim0;
                } else if (FpuLt(Ld(w, 40), 0)) {
                    target = Mul(GeoAngleDiffRadians(Sub(Ld(w, 48), kPi), Ld(w, 0)), kHalf);
                } else {
                    target = Mul(Sub(Ld(w, 48), Ld(w, 0)), kHalf);
                }
            } else {
                if (locked) target = Ld(w, 0);
                else if (FpuEq(Ld(w, 96), 0)) target = Ld(w, 28);
                else if (FpuLt(Ld(w, 40), 0)) target = GeoAngleBlend(kHalf, Sub(Ld(w, 48), kPi), kHalf, Ld(w, 0));
                else target = Mul(Add(Ld(w, 48), Ld(w, 0)), kHalf);
            }
            const uint32_t pos = Ld(w, 28), vel = Ld(w, 24);
            const uint32_t err = Sub(target, pos);
            const uint32_t kp = mem.U32(kAssist + 16 + 4 * axle), kd = mem.U32(kAssist + 24 + 4 * axle);
            const uint32_t flag = mem.U32(kAssist + 4);
            uint32_t lim = lim0;
            uint32_t force = Sub(Mul(kp, err), Mul(kd, vel));
            uint32_t scale = kOne;
            if (flag != 0) {
                const uint32_t n = static_cast<uint32_t>(entry[8 + axle]);
                scale = Div(Mul(Ld(w, 76), FpuCvtSW(n)), Ld(spec, 4160 + 4 * axle));
            }
            if (flag == 1 || flag == 3) lim = Mul(lim, scale);
            if (flag == 2 || flag == 3) force = Mul(force, scale);
            const uint32_t v = Add(vel, Mul(force, dt));
            St(w, 24, v);
            St(w, 28, Add(pos, Mul(v, dt)));
            if (mem.U32(kAssist) == 2) {
                const uint32_t base = Ld(w, 0);
                const uint32_t off = Sub(Ld(w, 28), base);
                if (FpuLt(lim, off)) {
                    St(w, 24, 0);
                    St(w, 28, Add(base, lim));
                } else if (FpuLt(off, FpuNeg(lim))) {
                    St(w, 24, 0);
                    St(w, 28, Sub(base, lim));
                }
            } else {
                const uint32_t p = Ld(w, 28);
                if (FpuLt(lim, p)) {
                    St(w, 28, lim);
                    St(w, 24, 0);
                } else if (FpuLt(p, FpuNeg(lim))) {
                    St(w, 28, FpuNeg(lim));
                    St(w, 24, 0);
                }
            }
        }
    }
    if (mem.U8(0x00620af0u) == 0) return;
    for (int i = 0; i < entry[1]; ++i) {
        uint8_t* w = c + 356 + 236 * i;
        const uint32_t axle = entry[44 + i];
        const uint32_t load = Mul(Ld(w, 104), Ld(spec, 4184 + 4 * axle));
        const uint32_t speed = Add(Ld(spec, 4176 + 4 * axle), GeoCurveBinary(CurveAt(mem, 0x00620b24u), Ld(c, 0x6ec)));
        const uint32_t dead = GeoCurveBinary(CurveAt(mem, 0x00620b40u), speed);
        uint32_t t;
        if (FpuLe(0, load)) {
            t = Sub(load, dead);
            if (FpuLt(t, 0)) t = 0;
        } else {
            t = Add(load, dead);
            if (FpuLt(0, t)) t = 0;
        }
        if (!FpuEq(t, 0)) {
            t = Mul(t, GeoCurveBinary(CurveAt(mem, 0x00620b54u), speed));
            const uint32_t cap = mem.U32(kServo + 4);
            if (FpuLt(cap, t)) t = cap;
            else if (FpuLt(t, FpuNeg(cap))) t = FpuNeg(cap);
            if (axle == 1) t = FpuNeg(t);
        }
        const uint32_t dt = Ld(c, 0x54c);
        const uint32_t pos = Ld(w, 204), rate = Ld(w, 208);
        const uint32_t acc = Sub(Mul(mem.U32(kServo + 8), Sub(t, pos)), Mul(mem.U32(kServo + 12), rate));
        const uint32_t r = Add(rate, Mul(acc, dt));
        St(w, 208, r);
        const uint32_t p = Add(pos, Mul(r, dt));
        St(w, 204, p);
        St(w, 0, Add(Ld(w, 0), p));
    }
}

} // namespace gt4::game
