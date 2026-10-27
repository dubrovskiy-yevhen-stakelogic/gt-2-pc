#include "game/physics/engine.h"

#include "ee/fpu.h"
#include "game/math/raw.h"
#include "game/physics/drivetrain.h"

namespace gt4::game {

namespace {
using namespace gt4::ee;
using raw::Ld;
using raw::St;

inline uint32_t Mul(uint32_t a, uint32_t b) { return FpuMul(a, b, nullptr); }
inline uint32_t Add(uint32_t a, uint32_t b) { return FpuAdd(a, b, nullptr); }
inline uint32_t Sub(uint32_t a, uint32_t b) { return FpuSub(a, b, nullptr); }
inline uint32_t Div(uint32_t a, uint32_t b) { return FpuDiv(a, b, nullptr); }
inline uint32_t I2F(int64_t v) { return FpuCvtSW(static_cast<uint32_t>(v)); }

constexpr uint32_t kOne = 0x3f800000u, kSixtieth = 0x3c888888u;
} // namespace

uint32_t PhysPiecewiseShape(const uint8_t* t, int64_t xIn, uint64_t index) {
    const uint32_t i = static_cast<uint32_t>(index);
    const int64_t x = static_cast<int16_t>(static_cast<uint16_t>(xIn));
    const int64_t knee = static_cast<int16_t>(raw::Ld16(t, static_cast<int>(4 + 2 * i + 208)));
    const uint32_t c224 = Ld(t, static_cast<int>(4 + 4 * i + 224)), c240off = 4 + 4 * i + 240;
    if (x < knee) {
        const uint8_t mode = t[i + 216];
        if (mode == 1) {
            const uint32_t fx = I2F(x);
            return Mul(Mul(Ld(t, static_cast<int>(c240off)), fx), fx);
        }
        if (mode == 0) {
            const uint32_t d = I2F(static_cast<int32_t>(static_cast<uint32_t>(x) - static_cast<uint32_t>(knee)));
            return Sub(c224, Mul(Mul(Ld(t, static_cast<int>(c240off)), d), d));
        }
        if (mode == 2) return Div(Mul(c224, I2F(x)), I2F(knee));
        return 0;
    }
    const int64_t limit = static_cast<int32_t>(static_cast<uint32_t>(knee) * 3u);
    if (!(x < limit)) return 0;
    const uint32_t d = I2F(static_cast<int32_t>(static_cast<uint32_t>(x) - static_cast<uint32_t>(knee)));
    return Sub(c224, Mul(Mul(Mul(Ld(t, static_cast<int>(c240off)), d), d), 0x3e800000u));
}

uint32_t PhysDistanceStep(const uint8_t* car) { return Mul(Ld(car, 0x6c8), Ld(car, 0x54c)); }

uint64_t PhysUpdateStandstillLatch(uint8_t* car) {
    car[0x522] = FpuEq(Ld(car, 0x514), 0) ? 0 : 1;
    return 1;
}

uint32_t PhysScaledByGlobal(const uint8_t* car, uint32_t divisor) { return Div(Ld(car, 0x788), divisor); }

uint32_t PhysPowerRatio(const uint8_t* car, uint32_t g) {
    uint32_t power = Add(Ld(car, 0x5ec), Ld(car, 0x5f0));
    power = Mul(power, I2F(static_cast<int16_t>(raw::Ld16(car, 0x5fe))));
    power = Mul(power, 0x38dbc307u);
    const uint32_t den = Add(Add(power, g), g);
    const uint32_t r = Div(Ld(car, 0x598), Div(power, den));
    return FpuLt(kOne, r) ? kOne : r;
}

uint32_t PhysInverseScale(int64_t n, uint32_t x) {
    uint32_t f = 0x42440000u;
    if (!(n < 101)) {
        f = Div(0x47ba6233u, I2F(n));
        if (FpuLt(0x42440000u, f)) f = 0x42440000u;
    }
    return Mul(f, x);
}

uint64_t PhysCurvePair(const PhysCurve& a, const uint8_t* ys2, uint8_t* out1, uint8_t* out2, uint32_t x) {
    const uint32_t last = static_cast<uint32_t>(static_cast<int32_t>(a.count) - 1);
    auto xs = [&](uint32_t k) { return Ld(a.xs, static_cast<int>(4 * k)); };
    if (FpuLe(x, xs(0))) {
        St(out1, 0, Ld(a.ys, 0));
        St(out2, 0, Ld(ys2, 0));
        return 1;
    }
    if (FpuLe(xs(last), x)) {
        St(out1, 0, Ld(a.ys, static_cast<int>(4 * last)));
        St(out2, 0, Ld(ys2, static_cast<int>(4 * last)));
        return 1;
    }
    uint32_t i = 1;
    while (i < last && !FpuLt(x, xs(i))) ++i;
    const int o = static_cast<int>(4 * i);
    const uint32_t y0 = Ld(a.ys, o - 4), x0 = Ld(a.xs, o - 4);
    const uint32_t dx = Sub(x, x0);
    const uint32_t span = Sub(Ld(a.xs, o), x0);
    uint32_t r = Div(Mul(dx, Sub(Ld(a.ys, o), y0)), span);
    St(out1, 0, Add(y0, r));
    const uint32_t z0 = Ld(ys2, o - 4);
    r = Div(Mul(dx, Sub(Ld(ys2, o), z0)), span);
    St(out2, 0, Add(z0, r));
    return 1;
}

void PhysSpeedFilters(uint8_t* car) {
    const uint32_t shown = Ld(car, 0x6d0), speed = Ld(car, 0x6c8);
    const uint32_t step = Mul(Ld(car, 0x54c), 0x42de38e2u);
    uint32_t v;
    bool clamp;
    if (FpuLt(shown, speed)) {
        v = Add(shown, step);
        clamp = FpuLt(speed, v);
    } else {
        v = Sub(shown, step);
        clamp = FpuLt(v, speed);
    }
    St(car, 0x6d0, v);
    if (clamp) St(car, 0x6d0, speed);
    const uint32_t x = Ld(car, 0x664);
    uint32_t rate = Ld(car, 0x660);
    uint32_t a = Mul(Sub(Ld(car, 0x658), x), 0x43480000u);
    a = Sub(a, Mul(rate, 0x41a00000u));
    a = Mul(a, kSixtieth);
    rate = Add(rate, a);
    St(car, 0x660, rate);
    St(car, 0x664, Add(x, Mul(rate, kSixtieth)));
}

uint32_t PhysLowSpeedBlend(const uint8_t* p, uint32_t x, uint32_t v) {
    if (p[0x520] == 0) v = FpuNeg(v);
    const uint32_t k = 0x40855554u;
    if (FpuLe(v, k)) return x;
    uint32_t t = Div(Sub(k, v), k);
    if (FpuLt(0, t)) t = 0;
    if (FpuLt(t, 0xbf800000u)) t = 0xbf800000u;
    t = Mul(t, 0x3f4ccccc);
    return Add(t, Mul(Sub(kOne, t), x));
}

void PhysRpmFromAngularSpeed(uint8_t* car) {
    const uint32_t rpm = Div(Mul(Ld(car, 0x600), 0x42700000u), 0x40c90fdau);
    raw::St16(car, 0x5fe, static_cast<uint16_t>(FpuCvtWS(rpm)));
}

uint32_t PhysGearScaledDrive(const uint8_t* car, const uint8_t* spec) {
    uint32_t f = PhysDriveSelect(car, spec);
    int64_t gear = car[0x624];
    const uint8_t* drive = spec + 0x234;
    if (DynKindIs(drive, 2) && !(gear < 2)) gear = 1;
    f = Mul(f, Ld(drive, static_cast<int>(64 + 4 * gear)));
    if (FpuLe(f, 0)) f = 0;
    return f;
}

void PhysWheelSplit(uint8_t* car, const uint8_t* entry, const uint8_t* g) {
    if (entry[1] == 0) return;
    uint8_t* w = car + 356;
    for (uint32_t k = 0; k < entry[1]; ++k, w += 236) {
        uint32_t f1 = Mul(Ld(w, 84), Ld(w, 76));
        if (static_cast<int8_t>(w[69]) < 0) f1 = FpuNeg(f1);
        uint32_t f0 = Mul(Ld(w, 60), f1);
        uint32_t f2, f3;
        if (static_cast<int8_t>(w[68]) > 0) {
            f3 = 0;
            f2 = f1;
            f1 = 0;
        } else {
            f2 = 0;
            f3 = f1;
            f1 = f0;
            f0 = 0;
        }
        St(w, 124, f0);
        St(w, 128, f1);
        St(w, 116, g[0xb6] != 2 ? f3 : f1);
        St(w, 112, g[0xb7] != 2 ? f2 : f0);
    }
}

uint32_t DynWheelRecordAddress(uint32_t objAddress, const uint8_t* obj, const uint8_t* entry, int64_t i, uint64_t sel) {
    uint32_t k = 0;
    if (i < static_cast<int64_t>(entry[1])) k = entry[20 + i];
    const uint8_t slot = obj[3992 + static_cast<uint32_t>(sel) + 24 * k];
    return objAddress + 492u * slot + 1040u;
}

uint64_t DynKindIs(const uint8_t* b, unsigned kind) { return b[18] == kind ? 1 : 0; }
uint64_t DynKindIs1or2(const uint8_t* b) { return static_cast<uint8_t>(b[18] + 255) < 2 ? 1 : 0; }

void DynSetSlotByte(uint8_t* car, int64_t i, uint64_t v) {
    if (i < 4) car[0x774 + static_cast<int32_t>(i)] = static_cast<uint8_t>(static_cast<int8_t>(v));
}

} // namespace gt4::game
