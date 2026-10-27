// Race-start physics initialisation 0x00361908 and callees (CORE.GT4 US 1.01), transliterated from the R5900 code:
// every float operation is the EE FPU operation of the original (src/ee/fpu), in the original order.
#include "game/physics/carparams_init.h"

#include "game/physics/carparams.h"
#include "game/physics/carparams_guest.h"

#include <cstring>

#include "ee/fpu.h"
#include "game/math/geom.h"
#include "game/math/libm.h"
#include "game/physics/drivetrain.h"
#include "game/physics/engine.h"

namespace gt4::game::carparams {

namespace {

using namespace gt4::ee;
using namespace guest;

// 0x00350868(table 0x00620320, opt): grip-like factor of the option record's two values.
uint32_t OptionFactor(M& m, const uint8_t* opt) {
    if (opt[0x14] != 0) return kOne;
    const uint32_t t = 0x00620320u;
    PhysLoadByteConstants(m.p(t), m.p(0x00620cd8u));
    auto of = [&](int o) {
        uint32_t v;
        std::memcpy(&v, opt + o, 4);
        return v;
    };
    uint32_t x = of(4);
    uint32_t a;   // f4
    const uint32_t t0 = m.f(t), t1 = m.f(t + 4), t2 = m.f(t + 8), t3 = m.f(t + 12), t4 = m.f(t + 16);
    if (Lt(x, t0)) {
        a = Sub(kOne, t1);
    } else if (Lt(x, kZero)) {
        a = Sub(kOne, Div(Mul(x, t1), t0));
    } else if (Le(x, t2)) {
        a = kOne;
    } else if (Lt(x, t3)) {
        a = Sub(kOne, Div(Mul(Sub(x, t2), t4), Sub(t3, t2)));
    } else {
        a = Sub(kOne, t4);
    }
    x = of(8);
    const uint32_t t5 = m.f(t + 20), t6 = m.f(t + 24), t7 = m.f(t + 28), t8 = m.f(t + 32);
    uint32_t b = kOne;   // f3
    if (Lt(x, t5)) {
        b = Add(t6, kOne);
    } else if (Lt(x, kZero)) {
        b = Add(Div(Mul(x, t6), t5), kOne);
    } else if (Lt(t8, x)) {
        uint32_t q = Div(Sub(x, t8), Sub(t7, t8));
        if (Lt(kOne, q)) q = kOne;
        b = Sub(kOne, Mul(t6, q));
    }
    return Mul(a, b);
}

const uint8_t* OptPtr(M& m, uint32_t opt, const Opt& def) { return opt ? m.p(opt) : def.bytes; }

// 0x0034e9a0(x, curve, opt): curve(x) * option factor
uint32_t TorqueAt(M& m, uint32_t x, uint32_t curve, const uint8_t* opt) {
    const uint32_t y = GeoCurveBinary(Curve(m, curve), x);
    return Mul(y, OptionFactor(m, opt));
}

// 0x0034ec30(t, a, b, out[2]): 1 + sum of the active piecewise shapes
uint32_t ShapeSum(M& m, uint32_t t, int16_t a, int16_t b, uint32_t* out) {
    uint32_t sum = kZero;
    out[0] = kZero;
    out[1] = kZero;
    const int16_t xs[2] = {a, b};
    for (int i = 0; i < 2; ++i) {
        if (Eq(m.f(t + 0xe4 + 4 * i), kZero)) continue;
        const uint32_t v = PhysPiecewiseShape(m.p(t), xs[i], static_cast<uint64_t>(i));
        sum = Add(sum, v);
        out[i] = v;
    }
    return Add(sum, kOne);
}

// 0x0034ecf0: min(ShapeSum, t[+0xfc] + 1)
uint32_t ShapeSumLimitedImpl(M& m, uint32_t t, int16_t a, int16_t b, uint32_t* out) {
    uint32_t s = ShapeSum(m, t, a, b, out);
    const uint32_t lim = Add(m.f(t + 0xfc), kOne);
    if (Lt(lim, s)) s = lim;
    return s;
}

// 0x0034ed48(cprm, t): turbo stage parameters
void TurboSetup(M& m, uint32_t c, uint32_t t) {
    auto clear = [&]() {
        m.w32(t + 0xf8, kZero);
        m.w16(t + 0xd6, 0);
        m.w8(t + 0xd8, 0);
        m.w32(t + 0xe4, kZero);
        m.w32(t + 0xec, kZero);
        m.w32(t + 0xf4, kZero);
        m.w16(t + 0xd4, 0);
        m.w8(t + 0xd9, 0);
        m.w32(t + 0xe8, kZero);
        m.w32(t + 0xf0, kZero);
    };
    const uint8_t boost = m.u8(c + 0x113);
    if (boost == 0) {
        m.w32(t + 0xfc, kZero);
    } else {
        m.w32(t + 0xfc, Mul(U8F(boost), kTenth));
        if (m.u8(c + 0x116) == 0 || m.u8(c + 0x118) == 0 || m.u8(c + 0x11a) == 0) m.w32(t + 0xfc, kZero);
    }
    if (Eq(m.f(t + 0xfc), kZero)) {
        clear();
        return;
    }
    uint8_t k = m.u8(c + 0x114);
    m.w8(t + 0xd8, k);
    if (k >= 3) m.w8(t + 0xd8, 0);
    const uint32_t f1 = Mul(U8F(m.u8(c + 0x118)), kTenth);
    m.w32(t + 0xe4, f1);
    m.w32(t + 0xec, Mul(U8F(m.u8(c + 0x11a)), kTenth));
    const uint32_t r = m.u8(c + 0x116) * 100u;
    m.w32(t + 0xf8, kZero);
    m.w8(t + 0xd9, 0);
    m.w32(t + 0xe8, kZero);
    m.w32(t + 0xf0, kZero);
    m.w16(t + 0xd6, 0);
    m.w16(t + 0xd4, r);
    uint32_t s = I2F(static_cast<int16_t>(r));
    s = Mul(s, s);
    m.w32(t + 0xf4, Div(f1, s));
    const uint32_t a = m.u8(c + 0x119), b = m.u8(c + 0x11b), d = m.u8(c + 0x117);
    if (a + b + d == 0 || a == 0 || d == 0 || b == 0) return;
    k = m.u8(c + 0x115);
    m.w8(t + 0xd9, k);
    if (k >= 3) m.w8(t + 0xd9, 0);
    const uint32_t g1 = Mul(U8F(m.u8(c + 0x119)), kTenth);
    m.w32(t + 0xe8, g1);
    m.w32(t + 0xf0, Mul(U8F(m.u8(c + 0x11b)), kTenth));
    const uint32_t r2 = m.u8(c + 0x117) * 100u;
    m.w16(t + 0xd6, r2);
    uint32_t s2 = I2F(static_cast<int16_t>(r2));
    s2 = Mul(s2, s2);
    m.w32(t + 0xf8, Div(g1, s2));
}

// 0x0034ef30(t = phys + 0x134, cprm, limitRpm, arg4): engine torque curve and rev limits
void EngineCurve(M& m, uint32_t t, uint32_t c, int32_t limit) {
    m.w16(t + 0xce, m.u8(c + 0xbc) * 10u);
    const uint8_t peak = m.u8(c + 0xbd);
    if (peak) m.w16(t + 0xcc, peak * 100u);
    else m.w16(t + 0xcc, m.u8(c + 0xbf + m.u8(c + 0xbf)) * 100u);
    const uint32_t minRev = m.u8(c + 0xbe) * 100u + 200;
    if (static_cast<int32_t>(m.u16(t + 0xcc)) < static_cast<int32_t>(minRev)) m.w16(t + 0xcc, minRev);
    uint32_t n = m.u8(c + 0xbf);
    const uint32_t slot = c + 8 + 2 * n + 0xce;   // torque entry n (s16 at cprm + 0xd8 + 2n)
    if (m.s16(slot) == 0) {
        m.w16(slot, m.u16(c + 8 + 2 * n + 0xcc));
        n = m.u8(c + 0xbf);
    }
    const uint32_t xs = t + 12;
    for (uint32_t i = 0; i < n; ++i) m.w32(xs + 4 * i, Mul(U8F(m.u8(c + 0xc0 + i) * 100u), kRpm));
    uint32_t f6 = kOne;
    if (m.u8(c + 0x10c)) f6 = Mul(U8F(m.u8(c + 0x10c)), kCenti);
    uint32_t f5 = kOne;
    if (m.u16(c + 0x10a)) f5 = Mul(U8F(m.u16(c + 0x10a)), kMilli);
    const uint32_t f3 = m.f(t + 12);
    const uint32_t f4 = m.f(xs + 4 * n - 4);
    uint32_t f2 = kOne;
    if (m.u16(c + 0x108)) f2 = Mul(U8F(m.u16(c + 0x108)), kMilli);
    f2 = Mul(f2, kCenti);
    const uint8_t pct = m.u8(c + 7);
    if (pct != 0 && pct != 100) f2 = Mul(f2, Div(U8F(pct), kHundred));
    if (n) {
        const uint32_t d5 = Sub(f5, f6), d4 = Sub(f4, f3);
        for (uint32_t i = 0; i < n; ++i) {
            const int32_t raw = m.s16(c + 8 + 0xd0 + 2 * i);
            const int32_t v = raw + (raw < -1 ? 0x10000 : 0);
            uint32_t q = Mul(d5, Sub(m.f(xs + 4 * i), f3));
            q = Div(q, d4);
            const uint32_t tq = Mul(I2F(v), kG);
            m.w32(t + 0x6c + 4 * i, Mul(tq, Mul(f2, Add(f6, q))));
        }
    }
    SetCurve(m, t, m.u8(c + 0xbf), xs, t + 0x6c);
    const uint32_t lim = Mul(Mul(U8F(m.u16(c + 0xba)), kTenth), kG);
    const uint32_t last = m.f(xs + 4 * m.u8(c + 0xbf) - 4);
    m.w32(t + 0xd0, m.u8(0x00620cd8u + 316) ? Mul(lim, 0x3f666666u) : Div(lim, last));
    TurboSetup(m, c, t);
    const int16_t count = m.s16(t);
    if (count > 0) {
        uint32_t tmp[2];
        for (int32_t i = 0; i < m.s16(t); ++i) {
            const int16_t rpm = static_cast<int16_t>(m.u8(c + 0xc0 + static_cast<uint32_t>(i)) * 100u);
            uint32_t a = ShapeSumLimitedImpl(m, t, rpm, rpm, tmp);
            const uint32_t b = ShapeSum(m, t, rpm, rpm, tmp);
            a = Div(a, b);
            const uint32_t y = t + 0x6c + 4 * static_cast<uint32_t>(i);
            m.w32(y, Mul(m.f(y), a));
        }
    }
    m.w8(t + 0xdb, 0);
    m.w32(t + 0xdc, kZero);
    if (limit > 0) {
        m.w8(t + 0xdb, 1);
        const uint32_t pctLoss = Mul(U8F(m.u8(0x00620cd8u + 8)), kCenti);
        uint32_t p = Mul(Mul(I2F(limit), kG), 0x44330cccu);   // * 716.19995
        m.w32(t + 0xe0, pctLoss);
        p = Mul(p, kTwoPi);
        p = Div(p, kSixty);
        m.w32(t + 0xdc, Div(p, Sub(kOne, Mul(pctLoss, kHalf))));
    }
}

// 0x0034f3f8(t, out, opt): power / torque peaks of the engine curve
void EnginePeaks(M& m, uint32_t t, uint32_t o, const uint8_t* opt) {
    uint32_t bestT = kZero, bestP = kZero;   // f23 torque peak, f22 power peak
    int32_t rpmT = 0, rpmP = 0;               // s6, s7
    int32_t n = 0, off = 0;                   // a3, t3
    const int16_t count = m.s16(t);
    if (count > 0) {
        int32_t o2 = 0;
        for (int32_t i = 0; i < m.s16(t); ++i) {
            uint32_t w = m.f(t + 12 + 4 * static_cast<uint32_t>(i));
            w = Div(Mul(w, kSixty), kTwoPi);
            const int32_t r = (F2I(w) + 50) / 100 * 100;
            if (n <= 0 || m.s16(o + 14 + static_cast<uint32_t>(o2)) < r) {
                m.w16(o + 16 + static_cast<uint32_t>(o2), static_cast<uint32_t>(r));
                o2 += 2;
                ++n;
                off = o2;
            }
        }
    }
    m.w16(o + 14, static_cast<uint32_t>(n));
    int32_t rpm = m.s16(o + 16);
    m.w16(o + 12, m.u16(t + 0xcc));
    int32_t top = m.u16(t + 0xcc);
    const int32_t lastRpm = m.s16(o + static_cast<uint32_t>(off) + 14);
    if (top < lastRpm) top = lastRpm;
    uint32_t mark = o + 16, store = o + 160;
    while (!(top < rpm)) {
        const uint32_t fr = I2F(rpm);
        const uint32_t w = Mul(fr, kRpm);
        const uint32_t tq0 = TorqueAt(m, w, t, opt);
        uint32_t tq = PhysSoftLimit(m.p(t), tq0, w);
        tq = Div(tq, kG);
        uint32_t pw = Mul(tq, fr);
        const bool better = Lt(bestT, tq);
        pw = Div(pw, 0x44330cccu);
        if (m.s16(mark) == rpm) {
            m.w32(store - 96, pw);
            mark += 2;
            m.w32(store, tq);
            store += 4;
        }
        if (!(static_cast<int32_t>(m.u16(o + 12)) < rpm)) {
            if (better) {
                bestT = tq;
                rpmT = rpm;
            }
            if (Lt(bestP, pw)) {
                bestP = pw;
                rpmP = rpm;
            }
        }
        rpm += 100;
    }
    if (o != 0) {
        m.w16(o + 10, static_cast<uint32_t>(rpmT));
        m.w32(o, bestP);
        m.w16(o + 8, static_cast<uint32_t>(rpmP));
        m.w32(o + 4, bestT);
    }
}

// 0x0034aad8: index of c in the 8-byte table 0x00620300, else c
int8_t WheelCode(M& m, int8_t c) {
    if (m.s8(0x00620300u) == c) return 0;
    for (int i = 1; i < 8; ++i)
        if (m.s8(0x00620300u + static_cast<uint32_t>(i)) == c) return static_cast<int8_t>(i);
    return c;
}

// 0x0034ac48(kind, out[2]): driven axles per drive kind
void DrivenAxles(uint32_t kind, int32_t* out) {
    out[0] = out[1] = 0;
    if (kind >= 11) return;
    if (kind == 0 || kind == 7) out[1] = 1;
    else if (kind == 1) out[0] = 1;
    else out[0] = out[1] = 1;
}

// 0x00355978: derived engine-model constants 0x008438c0.. from the bytes 0x0062045a..0x00620474
void EngineModelConstants(M& m) {
    auto b = [&](uint32_t a) { return m.u8(0x00620000u + a); };
    const uint32_t f6 = U8F(b(0x45a)), f18 = U8F(b(0x45b));
    uint32_t f5 = U8F(b(0x461));
    uint32_t f11 = Add(f6, f6);
    uint32_t f4 = U8F(b(0x468)), f3 = U8F(b(0x469));
    f11 = Add(f11, f18);
    uint32_t f8 = U8F(b(0x465));
    f5 = Mul(f5, f11);
    uint32_t f7 = U8F(b(0x466)), f10 = U8F(b(0x46a));
    f4 = Mul(f5, f4);
    uint32_t f1 = U8F(b(0x460)), f2 = U8F(b(0x464));
    f3 = Mul(f5, f3);
    f8 = Div(f8, 0x47c35000u);
    f4 = Div(f4, kHundred);
    const uint32_t f15 = U8F(b(0x462) * 5u);
    const uint32_t f12 = U8F(b(0x45c) * 10u);
    f1 = Mul(f1, kTenth);
    const uint32_t f0 = U8F(b(0x45d) * 10u);
    f7 = Div(f7, kHundred);
    const uint32_t f13 = U8F(b(0x45e) * 10u), f14 = U8F(b(0x45f) * 10u), f16 = U8F(b(0x463) * 5u);
    f2 = Mul(f2, kHundred);
    m.w32(0x008438e0u, f15);
    f3 = Div(f3, kHundred);
    m.w32(0x008438c8u, f12);
    m.w32(0x008438ccu, f0);
    m.w32(0x008438d0u, f13);
    m.w32(0x008438d4u, f14);
    m.w32(0x008438d8u, f1);
    m.w32(0x008438e4u, f16);
    f10 = Div(f10, f6);
    m.w32(0x008438e8u, f2);
    const uint32_t f17 = U8F(b(0x467));
    m.w32(0x008438c0u, f6);
    m.w32(0x008438c4u, f18);
    uint32_t g6 = U8F(b(0x46f));
    m.w32(0x008438f8u, f4);
    uint32_t g4 = U8F(b(0x46e));
    m.w32(0x008438dcu, f5);
    uint32_t g5 = U8F(b(0x472)), g1 = U8F(b(0x474));
    m.w32(0x008438f0u, f7);
    uint32_t g7 = U8F(b(0x470));
    m.w32(0x008438ecu, f8);
    uint32_t g8 = U8F(b(0x471));
    m.w32(0x008438fcu, f3);
    uint32_t g0 = U8F(b(0x473));
    const uint32_t k980 = 0x4474fffeu;   // 979.9999
    g4 = Div(g4, kHundred);
    const uint32_t g2 = Div(0x45bb8000u, f11);   // 6000 / f11
    m.w32(0x008438f4u, f17);
    m.w32(0x00843900u, f10);
    m.w32(0x00843904u, g4);
    g6 = Div(g6, k980);
    m.w32(0x00843920u, g2);
    g5 = Div(g5, k980);
    m.w32(0x00843908u, g6);
    m.w32(0x00843914u, g5);
    g7 = Div(g7, k980);
    m.w32(0x0084390cu, g7);
    g0 = Div(g0, k980);
    m.w32(0x00843918u, g0);
    g1 = Div(g1, kHundred);
    m.w32(0x0084391cu, g1);
    g8 = Div(g8, kHundred);
    m.w32(0x00843910u, g8);
}

// 0x0034bc98(cprm, *kind, *flag, *f4, *f5): drive kind from the drivetrain / LSD bytes. Returns *kind.
uint8_t DriveKind(M& m, uint32_t c, uint8_t* kind, uint8_t* flag, uint32_t* f4, uint32_t* f5) {
    *f4 = kZero;
    *f5 = kZero;
    auto tail = [&](uint32_t def, uint8_t b) { *f5 = Mul(U8F((b ? b : def) * 10u), kG); };
    switch (m.u8(c + 0x128)) {
    case 0: case 3: case 4:
        *kind = 0;
        *flag = 1;
        break;
    case 1:
        *kind = 1;
        *flag = 0;
        break;
    case 2: {
        *f4 = Mul(U8F(m.u8(c + 0x12a)), kCenti);
        *flag = 1;
        const uint8_t sub = m.u8(c + 0x129);
        switch (sub) {
        case 0: *kind = 2; *flag = 0; break;
        case 2:
            *kind = 4;
            if (m.u8(c + 0x12a) >= 50) *f4 = kOne;
            tail(8, m.u8(c + 0x12b));
            break;
        case 3: {
            *kind = 5;
            const uint32_t a = Mul(*f4, 0x3eb851edu);   // 0.36000004
            *f4 = a;
            const uint32_t p = Div(U8F(m.u8(0x00620308u)), kHundred);
            const uint32_t q = Mul(U8F(m.u8(0x00620309u) * 10u), kG);
            *f4 = Mul(a, p);
            const uint32_t r = Div(U8F(m.u8(0x00620310u)), kHundred);
            const uint32_t s = Div(U8F(m.u8(0x00620318u)), kHundred);
            m.w32(0x0062030cu, r);
            m.w32(0x00620314u, s);
            *f5 = q;
            break;
        }
        case 4:
            *kind = 6;
            if (!Eq(*f4, kZero)) *f4 = Div(kOne, Mul(Mul(*f4, kHundred), kDeg));
            tail(15, m.u8(c + 0x12b));
            break;
        case 5:
            *kind = 7;
            EngineModelConstants(m);
            break;
        case 6: *kind = 9; *flag = 0; break;
        case 7: *kind = 8; break;
        case 8: *kind = 10; break;
        default: *kind = 3; break;   // 1 and >= 9
        }
        break;
    }
    default:
        break;
    }
    return *kind;
}

// 0x0034ba30(cprm, axle, *radius, *width, *sidewall): tyre geometry in metres. Returns *radius.
uint32_t TyreGeometry(M& m, uint32_t c, uint32_t axle, uint32_t* radius, uint32_t* width, uint32_t* side) {
    const uint32_t t = c + axle + 0x1c0;
    const uint32_t w5 = m.u8(t + 8) * 5u;
    const uint32_t rim = m.u8(t + 10), aspect = m.u8(t + 12);
    *width = Mul(I2F(static_cast<int32_t>(w5 ? w5 : 10)), kMilli);
    uint32_t d, k;
    if (rim < 61) {
        d = Mul(U8F(rim), kInch);
        k = kHalf;
    } else if (rim % 10 == 0) {
        d = Mul(U8F(rim / 10), kInch);
        k = kHalf;
    } else {
        d = Mul(U8F(rim), kInch);
        k = 0x3d4ccccc;   // 0.05 (tenths of an inch, halved)
    }
    const uint32_t rimR = Mul(d, k);
    const uint32_t sw = Mul(Mul(*width, U8F(aspect)), 0x3d4ccccc);
    *side = sw;
    *radius = Add(rimR, sw);
    const int8_t inch = m.s8(c + axle + 0x1e0 + 2);
    if (inch != 0) {
        uint32_t f2 = Mul(Mul(I2F(inch), kInch), kHalf);
        if (Lt(Add(rimR, f2), kInch)) f2 = Sub(kInch, rimR);
        const uint32_t s = Sub(*side, f2);
        *side = s;
        if (Lt(s, 0x3c5013a9u)) *side = 0x3c5013a9u;   // 0.0127
    }
    const uint8_t pct = m.u8(c + axle + 0x1e0 + 4);
    if (pct != 0 && pct != 100) *width = Mul(*width, Div(U8F(pct), kHundred));
    if (Special(m, c)) {
        const uint32_t o = OverrideTable(m.u8(c + 0x23));
        if (axle != 0) {
            *width = m.f(o + 12);
            *radius = m.f(o + 8);
        } else {
            *width = m.f(o + 4);
            *radius = m.f(o);
        }
        *side = Mul(*width, kHalf);
    }
    return *radius;
}

// 0x00354ff8(cprm, speed, gears, clearFlag): automatic gear table (s16 x1000) for a top speed
void AutoGears(M& m, uint32_t c, int32_t speed, uint32_t gears, bool clear) {
    if (clear) m.w8(c + 0x156, 0);
    uint8_t kind, flag;
    uint32_t f4a, f5a;
    DriveKind(m, c, &kind, &flag, &f4a, &f5a);
    uint32_t radius, width, side;
    TyreGeometry(m, c, flag, &radius, &width, &side);
    const int32_t n = m.u8(c + 0x155);
    int32_t idx = n < 8 ? n : 7;
    if (n < 5) idx = 5;
    const uint32_t row = 0x0062042au + static_cast<uint32_t>(idx) * 6;
    const uint32_t f5 = 0x43480000u;   // 200
    uint32_t f3 = I2F(speed - 150);
    uint32_t f8 = I2F(speed);
    const uint32_t r1 = U8F(m.u8(row + 1));
    const uint32_t a0 = m.u8(c + 0xbd);
    uint32_t f9 = U8F(m.u8(row + 0));
    const uint32_t f10 = U8F(m.u8(row + 2));
    uint32_t f14 = Div(f5, r1);
    const uint32_t f13 = U8F(m.u8(row + 3));
    f9 = Div(f5, f9);
    uint32_t f2 = U8F(a0 * 100u);
    const uint32_t f11 = U8F(m.u8(row + 4));
    uint32_t f1 = I2F(m.s16(c + 0x150));
    const uint32_t f12 = U8F(m.u8(row + 5));
    f2 = Mul(f2, kRpm);
    f3 = Div(f3, 0x437a0000u);   // 250
    f1 = Mul(f1, kMilli);
    f8 = Mul(f8, kKmh);
    uint32_t f0 = Sub(f9, f14);
    f2 = Mul(f2, radius);
    f9 = Div(f5, f10);
    f0 = Mul(f3, f0);
    uint32_t f4 = Add(f14, f0);
    f14 = Div(f5, f13);
    f2 = Div(f2, f8);
    f0 = Sub(f9, f14);
    f0 = Mul(f3, f0);
    f9 = Div(f5, f11);
    f2 = Div(f2, f1);
    f1 = Add(f14, f0);
    f14 = Div(f5, f12);
    f0 = Sub(f9, f14);
    f3 = Mul(f3, f0);
    const uint32_t g5 = Add(f14, f3);
    if (n > 0) {
        uint32_t p = gears + 2 * static_cast<uint32_t>(n);
        for (int32_t i = n; i > 0; --i) {
            if (i == 1) f2 = Mul(f2, g5);
            const uint32_t v = Mul(f2, 0x447a0000u);   // * 1000
            f2 = Mul(f2, f4);
            f4 = Mul(f4, f1);
            m.w16(p, static_cast<uint32_t>(F2I(v)));
            p -= 2;
        }
    }
    int32_t v = m.s16(gears + 2) * 3 + m.s16(gears + 4);
    if (v < 0) v += 3;
    m.w16(gears, static_cast<uint32_t>(v >> 2));
}

// 0x0034abc8(cprm): automatic gear table when requested (+0x156 / +0x157)
void AutoGearsIfSet(M& m, uint32_t c) {
    if (m.u8(c + 0x156) == 0 || m.u8(c + 0x157) == 0) return;
    const uint8_t s = m.u8(c + 0x157);
    const int32_t speed = s == 255 ? 255 : s * 10;
    for (int i = 0; i < 10; ++i) m.w16(c + 0x14a - 2 * static_cast<uint32_t>(i), 0xffff);
    AutoGears(m, c, speed, c + 0x138, false);
}


// 0x00367d90(d): shift-speed class from the rev limit
void ShiftClass(M& m, uint32_t d) {
    const uint32_t v = m.u16(d + 0x2e);
    m.w8(d + 0x13, v >= 5000 ? 3 : v < 4500 ? 1 : 2);
}

// 0x0034aca8(cprm, d = phys + 0x234, t = phys + 0x134, arg4): drivetrain record
void DrivetrainSetup(M& m, uint32_t c, uint32_t d, uint32_t t) {
    uint8_t kind = m.u8(d), flag = m.u8(d + 3);
    uint32_t f4 = m.f(d + 4), f5 = m.f(d + 8);
    DriveKind(m, c, &kind, &flag, &f4, &f5);
    m.w8(d, kind);
    m.w8(d + 3, flag);
    m.w32(d + 4, f4);
    m.w32(d + 8, f5);
    for (uint32_t i = 0; i < 2; ++i) m.w8(d + 0x10 + i, static_cast<uint8_t>(WheelCode(m, m.s8(c + 0x168 + i))));
    int32_t axles[2];
    DrivenAxles(m.u8(d), axles);
    for (uint32_t i = 0; i < 2; ++i) {
        const uint32_t a = d + 0x10 + i;
        if (axles[i] != 0) {
            if (m.u8(a) == 0) m.w8(a, 2);
        } else if (m.u8(a) != 0) {
            m.w8(a, 0);
        }
    }
    const uint32_t e = KindEntry(m.u8(c + 2));
    uint32_t n = m.u8(e + 2);
    for (uint32_t i = 0; i < n; ++i) {
        if (m.u8(e + 8 + i) != 1) continue;
        const uint32_t j = i < n ? m.u8(e + 0x10 + i) : 0;
        if (m.u8(d + 0x10 + j) != 0) {
            m.w8(d + 0x10 + j, 1);
            n = m.u8(e + 2);
        }
    }
    for (uint32_t i = 0; i < 2; ++i) {
        const uint32_t s = c + 0x160 + i;
        const uint32_t a = d + 0x14 + 4 * i, b = d + 0x1c + 4 * i, k = d + 0x24 + 4 * i;
        switch (m.u8(d + 0x10 + i)) {
        case 0: case 1: case 2:
            m.w32(a, kZero);
            m.w32(b, kZero);
            m.w32(k, kZero);
            break;
        case 3: case 4: case 6:
            m.w32(a, Mul(U8F(m.u8(s + 10)), kG));
            m.w32(b, Mul(U8F(m.u8(s + 12)), kCenti));
            m.w32(k, Mul(U8F(m.u8(s + 14)), kCenti));
            break;
        case 5: {
            const uint32_t v = U8F(m.u8(s + 10));
            m.w32(a, Mul(v, v));
            m.w32(b, Mul(Mul(U8F(m.u8(s + 12)), kTenth), kG));
            m.w32(k, kZero);
            break;
        }
        case 7:
            m.w32(a, Mul(U8F(m.u8(s + 10)), kCenti));
            m.w32(b, Mul(U8F(m.u8(s + 12)), kCenti));
            m.w32(k, Mul(U8F(m.u8(s + 14)), kCenti));
            break;
        default:
            break;
        }
    }
    AutoGearsIfSet(m, c);
    m.w8(d + 2, m.u8(c + 0x155));
    m.w8(d + 0x12, m.u8(c + 0x158));
    if (m.u8(c + 0x158) == 5) m.w8(d + 0x12, 2);
    else if (m.u8(c + 0x158) == 6) m.w8(d + 0x12, 0);
    const uint8_t gears = m.u8(d + 2);
    if (gears >= 10) {
        m.w8(d + 2, 9);
    } else if (m.u8(d + 0x12) == 3 && gears != 1) {
        m.w8(d + 2, 1);
    } else if (m.u8(c + 0x158) == 1) {
        if (gears < 2) m.w8(d + 0x12, 0);
    }
    const int32_t fin = m.s16(c + 0x150);
    const uint32_t f22 = Mul(I2F(fin < 500 ? 4000 : fin), kMilli);
    // gear ratios d[+0x40 + 4i] = ratio(i) * final; missing entries (< 100) default to 3.5 (4.0 in slot 1)
    {
        int32_t i = 0;
        int32_t g = m.s16(c + 0x138);
        bool def = g < 100;
        for (;;) {
            if (def) g = i == 1 ? 4000 : 3500;
            m.w32(d + 0x40 + 4 * static_cast<uint32_t>(i), Mul(Mul(I2F(g), kMilli), f22));
            ++i;
            if (static_cast<int32_t>(m.u8(d + 2)) < i) break;
            g = m.s16(c + 0x138 + 2 * static_cast<uint32_t>(i));
            if (g >= 100) {
                def = false;
                continue;
            }
            if (i < 2) {
                def = true;
                continue;
            }
            m.w8(d + 2, static_cast<uint32_t>(i - 1));
            break;
        }
    }
    const uint32_t r0 = Neg(m.f(d + 0x40));
    m.w32(d + 0x40, r0);
    const uint8_t fmode = m.u8(c + 0x154);
    if (fmode != 0 && fmode != 4 && !(m.s16(c + 0x152) < 500)) {
        uint32_t f1 = Mul(I2F(m.s16(c + 0x152)), kMilli);
        if (fmode == 2) {
            f1 = Div(f1, f22);
            m.w32(d + 0x40, Mul(r0, f1));
            for (uint32_t i = 5; !(m.u8(d + 2) < i); ++i) m.w32(d + 0x40 + 4 * i, Mul(m.f(d + 0x40 + 4 * i), f1));
        } else if (fmode == 1) {
            for (uint32_t i = 0; !(m.u8(d + 2) < i); ++i) m.w32(d + 0x40 + 4 * i, Mul(m.f(d + 0x40 + 4 * i), f1));
        } else if (fmode == 3) {
            f1 = Div(f1, f22);
            m.w32(d + 0x40, Mul(r0, f1));
            m.w32(d + 0x4c, Mul(m.f(d + 0x4c), f1));
            m.w32(d + 0x50, Mul(m.f(d + 0x50), f1));
        }
    }
    for (uint32_t i = 0; i < 2; ++i) {
        uint32_t radius = m.f(d + 0x90 + 4 * i), width = m.f(d + 0xa0 + 4 * i), side = m.f(d + 0xa8 + 4 * i);
        TyreGeometry(m, c, i, &radius, &width, &side);
        m.w32(d + 0x90 + 4 * i, radius);
        m.w32(d + 0xa0 + 4 * i, width);
        m.w32(d + 0xa8 + 4 * i, side);
        if (!Eq(radius, kZero)) m.w32(d + 0x98 + 4 * i, Div(kOne, radius));
    }
    m.w8(d + 1, 0);
    if (m.u8(d) == 5 && !Eq(m.f(d + 0x90), m.f(d + 0x94))) m.w8(d + 1, 1);
    if (m.u8(c + 0x154) == 4) m.w8(d + 1, 1);
    uint32_t inertia[2];
    for (uint32_t i = 0; i < 2; ++i) inertia[i] = Mul(I2F(m.s16(c + 0x174 + 2 * i)), kCenti);
    for (uint32_t i = 0; i < 2; ++i) {
        const uint32_t f2 = inertia[i];
        if (Eq(f2, kZero)) continue;
        m.w32(d + 0xb4 + 4 * i, Div(m.f(d + 0x90 + 4 * i), f2));
        const uint32_t f1 = m.f(d + 0x98 + 4 * i);
        m.w32(d + 0xcc + 4 * i, Mul(Mul(f2, f1), f1));
    }
    const uint32_t fly = Mul(U8F(m.u16(c + 0xb8)), kMilli);
    m.w32(d + 0xc, fly);
    if (m.u16(c + 0xb8) != 0) m.w32(d + 0xc8, Div(kOne, fly));
    for (uint32_t i = 0; i < 2; ++i) {
        uint32_t f2 = Mul(I2F(m.s16(c + 0x164 + 2 * i)), kCenti);
        if (m.u8(d + 1) != 0 && i != m.u8(d + 3)) {
            f2 = Mul(f2, Div(m.f(d + 0x90 + 4 * m.u8(d + 3)), m.f(d + 0x90 + 4 * i)));
        }
        const uint32_t a = Mul(f2, f22);
        m.w32(d + 0xbc + 4 * i, Add(a, Add(inertia[i], inertia[i])));
    }
    m.w32(d + 0xc4, Div(kOne, Add(m.f(d + 0xbc), m.f(d + 0xc0))));
    m.w16(d + 0x2c, m.u8(c + 0x160) * 10u);
    m.w16(d + 0x2e, m.u8(c + 0xbe) * 100u);
    m.w32(d + 0xb0, Mul(U8F(m.u8(c + 0x162)), kG));
    for (uint32_t i = 0; !(m.u8(d + 2) < i); ++i)
        m.w32(d + 0x68 + 4 * i, Div(m.f(d + 0x40 + 4 * i), m.f(d + 0x90 + 4 * m.u8(d + 3))));
    if (GearKind12(m, d)) {
        m.w16(d + 0x2c, m.u16(t + 0xce));
    } else {
        const int32_t lim = m.u16(t + 0xce);
        if (static_cast<int32_t>(m.u16(d + 0x2c)) - 100 < lim) m.w16(d + 0x2c, static_cast<uint32_t>(lim + 100));
    }
    for (uint32_t i = 1; static_cast<int32_t>(i) < static_cast<int32_t>(m.u8(d + 2)); ++i) {
        uint32_t f = Mul(U8F(m.u16(d + 0x2e)), kRpm);
        f = Mul(f, m.f(d + 0x44 + 4 * i));
        f = Div(f, m.f(d + 0x40 + 4 * i));
        f = Mul(f, kSixty);
        f = Div(f, kTwoPi);
        int32_t a1 = F2I(f);
        const int32_t lim = m.u16(d + 0x2c);
        if (m.u8(0x00620deau) != 0 && i == 1) {
            const int32_t s = lim + a1;
            a1 = (s + static_cast<int32_t>(static_cast<uint32_t>(s) >> 31)) >> 1;
        }
        uint32_t g = Mul(m.f(d + 0x6c + 4 * i), 0x4031c71bu);   // 2.7777774
        g = Mul(g, kSixty);
        g = Div(g, kTwoPi);
        a1 -= F2I(g);
        if (a1 < lim + 500) a1 = lim + 500;
        m.w16(d + 0x2e + 2 * i, static_cast<uint32_t>(a1));
    }
    if (GearKind12(m, d) && !(m.u8(d + 2) < 2)) {
        for (uint32_t i = 2; !(m.u8(d + 2) < i); ++i) m.w32(d + 0x40 + 4 * i, Div(m.f(d + 0x40 + 4 * i), m.f(d + 0x44)));
    }
    ShiftClass(m, d);
    if (m.u8(c + 1) == 5) m.w8(d + 0x13, 4);
}

// 0x003f3278(power, phys, kind): tyre-wear-like factor phys[+0x10dc] from the power class table 0x00622320
void PowerClass(M& m, uint32_t power, uint32_t p, int32_t kind) {
    const uint32_t out = p + 0x10dc;
    if (kind < 0) kind = 0;
    else if (kind >= 4) kind = 3;
    const int32_t pos = Lt(kZero, m.f(p + 0x230)) ? 1 : 0;
    const uint32_t row = 0x00622320u + static_cast<uint32_t>((kind * 2 + pos) * 2);
    const uint32_t lo = U8F(m.u8(row) * 10u), hi = U8F(m.u8(row + 1) * 10u);
    if (Lt(power, lo)) {
        m.w32(out, kZero);
        return;
    }
    if (Le(hi, power)) {
        m.w32(out, 0x3f400000u);   // 0.75
        return;
    }
    uint32_t f = Mul(Sub(power, lo), kHalf);
    f = Div(f, Sub(hi, lo));
    m.w32(out, Add(f, 0x3e800000u));   // + 0.25
}

// 0x00356e08: engine-model power reference (EngineModelConstants first)
uint32_t ModelPower(M& m) {
    EngineModelConstants(m);
    uint32_t f = m.f(0x008438c0u);
    f = Add(f, f);
    f = Add(f, m.f(0x008438c4u));
    return Div(f, 0x3f3c49bau);   // 0.7355
}

// 0x00356ea8(d)
uint32_t ExtraPower(M& m, uint32_t d) {
    if (m.u8(d) == 7) return ModelPower(m);
    return m.u8(d + 0x13) == 4 ? 0x41598a02u : kZero;
}

// 0x003f86e8(phys, peaks): handling index in [0.01, 1]
uint32_t HandlingIndex(M& m, uint32_t p, const uint32_t* peaks) {
    uint32_t f20 = peaks[0];
    const uint32_t extra = ExtraPower(m, p + 0x234);
    const uint32_t f1a = m.f(p + 0x1070);
    uint32_t f2 = m.f(p + 0x106c);
    f20 = Add(f20, extra);
    f2 = Add(f2, f1a);
    const uint32_t f1b = m.f(p + 0xfd4);
    uint32_t f4 = m.f(p + 0x1068);
    uint32_t f0 = m.f(p + 0xfd0);
    f2 = Mul(f2, kHalf);
    f0 = Add(f0, f1b);
    f4 = Sub(f2, f4);
    uint32_t f5 = Mul(f0, kHalf);
    f4 = Add(f4, kOne);
    if (m.u8(p + 0x1077) == 7) {
        uint32_t q = Div(m.f(p + 0x1084), m.f(p + 0x103c));
        q = Add(q, kOne);
        f5 = Mul(f5, q);
    }
    f0 = m.f(p + 0x418);
    uint32_t f1 = m.f(p + 0x48);
    f2 = FpuSqrt(f1, nullptr);
    f0 = Mul(f0, f0);
    const uint32_t f3 = FpuSqrt(peaks[1], nullptr);
    f1 = Mul(f1, f2);
    f0 = Mul(f0, f20);
    f0 = Mul(f0, f20);
    f2 = Mul(f0, f3);
    f1 = Div(f2, f1);
    f1 = Mul(f1, f4);
    f2 = Mul(f1, f5);
    f0 = FpuSqrt(f2, nullptr);
    f0 = FpuSqrt(f0, nullptr);
    f0 = FpuSqrt(f0, nullptr);
    f2 = Div(f0, 0x40200000u);   // 2.5
    if (Lt(kOne, f2)) f2 = kOne;
    if (Lt(f2, kZero)) f2 = kCenti;
    return f2;
}

// 0x00360520(phys, opt): top speed estimate phys[+0xfd8] (m/s), stepping the speed until drag exceeds drive force
void TopSpeed(M& m, uint32_t p, const uint8_t* opt) {
    const uint32_t d = p + 0x234;
    if (GearKind3(m, d)) {
        m.w32(p + 0xfd8, 0x4247ffffu);
        return;
    }
    const int32_t vmax = F2I(Mul(m.f(p + 0xfd8), 0x40666666u));   // km/h
    const int32_t step = vmax / 500 + 1;
    if (vmax <= 0) return;
    int32_t gear = 1, kmh = 0;
    const uint32_t drag = 0x00620938u;   // 0x00620938: drag curve {s16 n, xs, ys} and +0x4c factor
    do {
        const uint32_t v = Mul(I2F(kmh), kKmh);   // f23
        const uint32_t ratioAddr = p + 0x23c + 0x60 + 4 * static_cast<uint32_t>(gear);
        kmh += step;
        uint32_t ratio = kOne;
        uint32_t w = Mul(v, m.f(ratioAddr));
        const uint32_t v2 = Mul(v, v);
        GearLimit(m, d, &w, &ratio);
        uint32_t rpmf = Mul(w, kSixty);
        rpmf = Div(rpmf, kTwoPi);
        const int32_t rpm = F2I(rpmf);
        uint32_t force = TorqueAt(m, w, p + 0x134, opt);
        force = PhysSoftLimit(m.p(p + 0x134), force, w);
        force = Mul(force, m.f(ratioAddr));
        if (GearKind1(m, d)) force = Mul(force, ratio);
        uint32_t a = m.f(p + 0x106c), b = m.f(p + 0x1070);
        uint32_t c = GeoCurveLinear(Curve(m, drag), v);
        a = Mul(a, v2);
        uint32_t f1 = m.f(p + 0x103c);
        b = Mul(b, v2);
        const uint32_t f3 = m.f(drag + 0x4c);
        uint32_t f2 = m.f(p + 0x1068);
        c = Mul(c, f3);
        f1 = Add(f1, a);
        f2 = Mul(f2, v2);
        f1 = Add(f1, b);
        c = Mul(c, f1);
        c = Add(c, f2);
        if (Lt(c, force)) m.w32(p + 0xfd8, v);
        if (!GearKind1(m, d) && gear < static_cast<int32_t>(m.u8(d + 2))) {
            gear += static_cast<int32_t>(m.u16(d + 0x2e)) < rpm ? 1 : 0;
        }
    } while (kmh < vmax);
}

// 0x00360780(phys, cprm, opt, arg4): engine peaks, top speed, power class, handling index
void PowerSetup(M& m, uint32_t p, uint32_t c, const uint8_t* opt) {
    uint32_t v = Mul(U8F(m.u16(p + 0x200)), kRpm);
    v = Div(v, m.f(p + 0x29c + 4 * m.u8(p + 0x236)));
    m.w32(p + 0xfd8, v);
    if (Lt(0x43a6aaaau, v)) m.w32(p + 0xfd8, 0x43a6aaaau);   // 333.3333
    const uint32_t mass = m.f(p + 0x48);
    const uint32_t e = SpecEntry(m, p);
    uint32_t f = Mul(m.f(p + 0x1010), m.f(p + 0x2cc));
    f = Mul(f, U8F(m.u8(e + 4)));
    m.w32(p + 0x1020, f);
    m.w32(p + 0x101c, Div(mass, Add(f, f)));
    uint32_t peaks[0x130 / 4] = {};
    m.stack = reinterpret_cast<uint8_t*>(peaks);
    EnginePeaks(m, p + 0x134, M::kStack, opt);
    m.stack = nullptr;
    if (GearKind1(m, p + 0x234)) {
        const int32_t a = static_cast<int32_t>(m.u16(p + 0x262)) - 500;
        int32_t r = static_cast<uint16_t>(peaks[2]);   // peak power rpm (u16 at +8)
        if (a < r) r = a;
        m.w16(p + 0x264, static_cast<uint32_t>(r));
    }
    TopSpeed(m, p, opt);
    PowerClass(m, peaks[0], p, m.u8(c + 0x120));
    m.w32(p + 0xfe0, HandlingIndex(m, p, peaks));
}

// 0x003608c0
uint32_t Recip100(uint32_t b) { return Div(kHundred, U8F(b & 0xff)); }

// 0x003608e8(out[4], scale, b0..b3): spring / damper pair
void SpringPair(uint32_t* o0, uint32_t* o1, uint32_t* o2, uint32_t* o3, uint32_t scale, uint32_t b0, uint32_t b1,
                uint32_t b2, uint32_t b3) {
    uint32_t f0 = U8F(b1 & 0xff), f1 = U8F(b3 & 0xff), f2 = U8F(b0 & 0xff), f3 = U8F(b2 & 0xff);
    f0 = Mul(f0, 0x40a00000u);
    f1 = Mul(f1, 0x40a00000u);
    f2 = Mul(f2, kCenti);
    f3 = Mul(f3, kCenti);
    f0 = Mul(f0, kG);
    f1 = Mul(f1, kG);
    *o0 = f2;
    *o1 = f0;
    *o2 = f3;
    *o3 = f1;
    *o0 = Mul(*o0, scale);
    *o2 = Mul(*o2, scale);
}

// 0x00360980(a, b, c, d, out0, out1, out2): damper curve fit (quadratic through two points)
void DamperFit(M& m, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t o0, uint32_t o1, uint32_t o2) {
    m.w32(o2, kZero);
    m.w32(o1, kZero);
    m.w32(o0, kZero);
    if (Eq(b, kZero) || Eq(d, kZero) || Eq(a, kZero) || Eq(c, kZero)) return;
    if (Le(c, a) || Le(d, b)) return;
    const uint32_t den = Sub(Mul(b, c), Mul(d, a));
    if (Le(den, kZero)) return;
    uint32_t q = Sub(Mul(Mul(b, b), c), Mul(Mul(d, d), a));
    if (Lt(kZero, q)) return;
    uint32_t f0 = Add(den, den);
    uint32_t f2 = Sub(d, b);
    uint32_t f1 = Mul(b, d);
    f0 = Div(q, f0);
    f1 = Mul(f1, f2);
    f1 = Div(f1, den);
    f2 = FpuSqrt(f1, nullptr);
    m.w32(o2, f2);
    f0 = Mul(f0, f0);
    f0 = Neg(f0);
    f0 = Div(f0, f1);
    m.w32(o0, f0);
    f0 = Neg(f0);
    f0 = FpuSqrt(f0, nullptr);
    m.w32(o1, Mul(Neg(m.f(o2)), f0));
}

// 0x00360a78: suspension option constants 0x00620cb0.. (once the option byte 0x00620dda is set)
void SuspensionConstants(M& m) {
    const uint32_t on = m.u8(0x00620ddau) != 0 ? 1u : 0u;
    m.w32(0x00620cb0u, on);
    if (!on) return;
    for (uint32_t i = 0; i < 2; ++i) {
        const uint32_t f3 = Div(U8F(m.u8(0x00620ddfu + i)), kHundred);
        const uint32_t f2 = Div(U8F(m.u8(0x00620de3u + i)), kHundred);
        const uint32_t f0 = Mul(U8F(m.u8(0x00620de1u + i)), kCenti);
        const uint32_t f1 = Mul(U8F(m.u8(0x00620dddu + i)), kCenti);
        m.w32(0x00620cb8u + 4 * i, f1);
        m.w32(0x00620cc8u + 4 * i, f0);
        m.w32(0x00620cc0u + 4 * i, f3);
        m.w32(0x00620cd0u + 4 * i, f2);
    }
}

// 0x00360b60(s, src, ratio, arg4): suspension axle record from the 14 SUSPENSION bytes at src
void SuspensionAxle(M& m, uint32_t s, uint32_t src, uint32_t ratio) {
    ratio &= 0xff;
    uint32_t f1 = U8F(ratio);
    uint32_t f0 = U8F(m.u8(src));
    f1 = Mul(f1, kCenti);
    f0 = Mul(f0, 0x4474fffeu);   // 979.9999
    f0 = Mul(f0, f1);
    m.w32(s + 20, f0);
    const uint32_t scale = Recip100(ratio);
    for (int k = 0; k < 2; ++k) {
        const uint32_t b = src + 4 + 4 * static_cast<uint32_t>(k);
        uint32_t t0, t8, t4, t12;
        SpringPair(&t0, &t8, &t4, &t12, scale, m.u8(b), m.u8(b + 1), m.u8(b + 2), m.u8(b + 3));
        const uint32_t base = s + 24 + 12 * static_cast<uint32_t>(k);
        if (m.u8(0x00620dd9u) != 0) {
            m.w32(base, t0);
            m.w32(base + 4, t8);
            m.w32(base + 8, t4);
            m.w32(s + 72 + 4 * static_cast<uint32_t>(k), t12);
        } else {
            DamperFit(m, t0, t8, t4, t12, base, base + 4, base + 8);
        }
    }
    const uint32_t ten = 0x41200000u;
    m.w32(s + 48, Mul(Mul(U8F(m.u8(src + 1)), ten), kG));
    m.w32(s + 52, Mul(Mul(U8F(m.u8(src + 2)), 0x461c4000u), kG));
    m.w32(s + 56, Mul(U8F(m.u8(src + 3)), 0x3e800000u));
    m.w32(s + 60, Mul(Mul(U8F(m.u8(src + 3)), 0x3e800000u), kG));
    m.w32(s + 64, Mul(Mul(U8F(m.u8(src + 13)), ten), kG));
}

// 0x00360dc8(phys, s, cprm, axle, arg4): ride height / travel of an axle record
void SuspensionTravel(M& m, uint32_t p, uint32_t s, uint32_t c, uint32_t axle) {
    const uint32_t f4 = m.f(s + 20);
    if (!Eq(f4, kZero)) {
        const uint32_t e = SpecEntry(m, p);
        uint32_t f0 = Neg(m.f(p + 0x1040 + 4 * axle));
        const uint32_t f1 = Div(kOne, U8F(m.u8(e + 4 + axle)));
        f0 = Mul(f0, f1);
        m.w32(s + 16, Div(f0, f4));
    }
    const uint32_t r = p + 0x40 + 4 * axle;
    const uint32_t f3 = m.f(s + 16);
    m.w32(s + 12, Add(Sub(m.f(p + 0x2c4 + 4 * axle), m.f(r)), f3));
    uint32_t f2 = Mul(U8F(m.u8(c + 0x92 + axle)), kMilli);
    const uint32_t len = m.f(r);
    if (Lt(len, f2)) f2 = len;
    const uint32_t f5 = Sub(f3, len);
    m.w32(s + 4, Add(f5, f2));
    const int32_t raw = m.s16(c + 0x66 + 0x18 * axle);
    uint32_t f0 = Mul(I2F(raw), kMilli);
    const uint32_t len2 = m.f(r);
    if (Lt(len2, f0)) f0 = len2;
    uint32_t out;
    if (raw == 0 || Le(f0, f2)) out = m.f(s + 4);
    else out = Add(f5, f0);
    m.w32(s, out);
    const uint32_t lim = Div(m.f(s + 60), f4);
    const uint32_t up = Add(f3, Mul(U8F(m.u8(c + 0x94 + axle)), kMilli));
    m.w32(s + 8, up);
    if (Lt(lim, up)) m.w32(s + 8, lim);
}

// 0x00361008(arg4, t, block): slip curve of a tyre (lateral)
void TyreSlipCurve(M& m, uint32_t t, uint32_t b) {
    uint32_t f2 = kZero;
    m.w8(b + 32, 0);
    m.w8(b + 64, m.u8(b + 72));
    m.w8(b + 48, 0);
    m.w8(b + 40, 0);
    m.w8(b + 56, 0);
    const uint32_t n1 = m.u8(b + 9), n2 = m.u8(b + 10);
    m.w32(t + 356, kZero);
    const uint32_t k255 = 0x437f0000u, k200 = 0x43480000u;
    if (n1) {
        uint32_t v1 = t + 4 * n1 + 256, a1 = t + 4 * n1 + 376, a0 = b + 64;
        for (uint32_t k = n1; k != 0; --k) {
            const uint32_t x = Div(Neg(U8F(m.u8(a0 - 32))), k255);
            m.w32(v1, x);
            const uint32_t y = Div(U8F(m.u8(a0 - 16)), k200);
            const bool more = Lt(f2, y);
            m.w32(v1 + 48, y);
            v1 -= 4;
            if (more) {
                m.w32(t + 356, x);
                f2 = y;
            }
            m.w32(a1, Div(U8F(m.u8(a0)), k200));
            ++a0;
            a1 -= 4;
        }
        f2 = kZero;
    }
    m.w32(t + 360, f2);
    m.w32(t + 364, kOne);
    if (n2) {
        const uint32_t v1 = t + 4 * n1;
        uint32_t a1 = b + 56, t0 = v1 + 376, a0 = v1 + 256, a2 = b + 72;
        for (uint32_t k = 0; k < n2; ++k) {
            const uint32_t x = Div(U8F(m.u8(a1 - 16)), k255);
            m.w32(a0, x);
            const uint32_t y = Div(U8F(m.u8(a1)), k200);
            ++a1;
            const bool more = Lt(f2, y);
            m.w32(a0 + 48, y);
            a0 += 4;
            if (more) {
                f2 = y;
                m.w32(t + 360, x);
                m.w32(t + 364, f2);
            }
            m.w32(t0, Div(U8F(m.u8(a2)), k200));
            ++a2;
            t0 += 4;
        }
    }
    const uint32_t n = n1 + n2 - 1;
    SetCurve(m, t + 248, n, t + 260, t + 308);
    SetCurve(m, t + 368, n, t + 260, t + 380);
}

// 0x00361220(t, block): camber / angle curves of a tyre
void TyreAngleCurves(M& m, uint32_t t, uint32_t b) {
    uint8_t deg = m.u8(b + 2);
    if (deg == 0) deg = 90;
    m.w8(b + 16, 0);
    uint32_t f1 = Mul(U8F(deg), kDeg);
    if (Lt(0x3fc90fdau, f1)) f1 = 0x3fc90fdau;
    m.w8(b + 24, 0);
    const uint32_t k255 = 0x437f0000u, k200 = 0x43480000u;
    const uint32_t n1 = m.u8(b + 8);
    for (uint32_t j = 0; j < n1; ++j) {
        m.w32(t + 120 + 4 * j, Div(Mul(U8F(m.u8(b + 16 + j)), f1), k255));
        m.w32(t + 152 + 4 * j, Div(U8F(m.u8(b + 24 + j)), k200));
    }
    SetCurve(m, t + 108, n1, t + 120, t + 152);
    m.w8(b + 80, 0);
    const uint32_t n2 = m.u8(b + 12);
    const uint32_t g1 = 0x3fc90fd9u;
    m.w8(b + 86, 0);
    for (uint32_t j = 0; j < n2; ++j) {
        m.w32(t + 200 + 4 * j, Div(Mul(U8F(m.u8(b + 80 + j)), g1), k255));
        m.w32(t + 224 + 4 * j, Div(U8F(m.u8(b + 86 + j)), k200));
    }
    SetCurve(m, t + 188, n2, t + 200, t + 224);
}

// 0x003613d8(scale, arg4, t, block): load sensitivity table and force curve of a tyre
void TyreLoadCurves(M& m, uint32_t scale, uint32_t t, uint32_t b) {
    uint8_t g = m.u8(b);
    if (g == 0) g = 100;
    const uint32_t grip = Mul(U8F(g), kCenti);
    for (uint32_t j = 0; j < 24; ++j) {
        const uint8_t v = m.u8(b + 100 + j);
        m.w32(t + 8 + 4 * j, v == 0 ? grip : Mul(Mul(U8F(v), kCenti), grip));
    }
    const uint32_t q = Div(scale, grip);
    const uint32_t f0 = Mul(U8F(m.u8(b + 1)), kCenti);
    m.w32(t + 104, q);
    m.w32(t + 4, f0);
    if (Lt(kOne, q)) m.w32(t + 104, kOne);
    const uint32_t n = m.u8(b + 11);
    m.w8(b + 92, 0);
    for (uint32_t j = 0; j < n; ++j) {
        m.w32(t + 440 + 4 * j, Mul(Mul(U8F(m.u8(b + 92 + j)), 0x41a00000u), kG));
        m.w32(t + 456 + 4 * j, Div(U8F(m.u8(b + 96 + j)), 0x43480000u));
    }
    SetCurve(m, t + 428, n, t + 440, t + 456);
    m.w32(t, Mul(GeoCurveBinary(Curve(m, t + 368), kZero), kG));
}

// 0x00353800(t): slip angle of the peak lateral force
uint32_t PeakSlipAngle(M& m, uint32_t t) {
    if (m.u8(t + 473) != 0) {
        const uint32_t c = t + 108;
        uint32_t best = kZero, at = kZero;
        const int16_t n = m.s16(c);
        for (int32_t i = 0; i < n; ++i) {
            const uint32_t y = m.f(m.u32(c + 8) + 4 * static_cast<uint32_t>(i));
            if (Lt(best, y)) {
                best = y;
                at = m.f(m.u32(c + 4) + 4 * static_cast<uint32_t>(i));
            }
        }
        return at;
    }
    uint32_t best = kZero;
    int32_t at = 90;
    for (int32_t a = 0; a < 46; ++a) {
        uint8_t sign[4];
        const uint32_t x = Sub(Mul(I2F(a), kDeg), kZero);
        const uint32_t folded = PhysFoldAngle(sign, x);
        uint32_t s;
        std::memcpy(&s, sign, 4);
        const uint32_t y = Mul(GeoCurveLinear(Curve(m, t + 108), folded), s);
        if (Lt(best, y)) {
            best = y;
            at = a;
        }
    }
    return Mul(I2F(at), kDeg);
}

// 0x00361570(phys, cprm, mode, arg4): per-wheel tyre records
void TyreSetup(M& m, uint32_t p, uint32_t c, int32_t mode) {
    int8_t count[6] = {};
    for (uint32_t i = 0; i < 24; ++i) {
        uint32_t v = m.u8(c + 0x1f0 + i);
        if (v >= 3) v = 0;
        const uint32_t a = v + 3;
        const uint8_t before = static_cast<uint8_t>(count[v]);
        m.w8(p + 0xfb0 - 24 + i, v);
        m.w8(p + 0xfb0 + i, a);
        count[v] = static_cast<int8_t>(before + 1);
        count[a] = static_cast<int8_t>(count[a] + 1);
    }
    uint32_t first = 0, last = 6;
    if (mode == 1) last = 3;
    else first = mode == 2 ? 3 : 0;
    for (uint32_t w = first; w < last; ++w) {
        if (count[w] == 0) continue;
        const uint32_t src = m.u8(c + 0x1e0) == 0 ? w % 3 : w;
        const uint32_t t = p + 0x410 + 0x1ec * w;
        const uint32_t b = c + 0x210 + 0xa8 * src;
        m.w8(t + 472, m.u8(b + 3));
        m.w8(t + 473, m.u8(b + 4));
        m.w8(t + 474, m.u8(b + 5));
        TyreSlipCurve(m, t, b);
        TyreAngleCurves(m, t, b);
        const uint32_t peak = PeakSlipAngle(m, t);
        m.w32(t + 184, peak);
        if (Lt(0x3f490fd9u, peak)) m.w32(t + 184, 0x3f490fd9u);   // pi/4
        const uint32_t scale = Mul(U8F(m.u8(0x00620d97u + src % 3)), kCenti);
        TyreLoadCurves(m, scale, t, b);
    }
}

// 0x003573a8(x, phys, axle): x / tyre radius of the axle's driven-wheel slot
uint32_t PerRadius(M& m, uint32_t x, uint32_t p, int32_t axle) {
    const uint32_t e = SpecEntry(m, p);
    uint32_t k = 0;
    if (axle < static_cast<int32_t>(m.u8(e + 1))) k = m.u8(e + 20 + static_cast<uint32_t>(axle));
    return Div(x, m.f(p + 0x2d4 + 4 * k));
}

// 0x00361798(phys, mode, arg4): load scaling of the tyre records
void TyreLoadScale(M& m, uint32_t p, int32_t mode) {
    int32_t first = 0, last = 6;
    if (mode == 1) last = 3;
    else first = mode == 2 ? 3 : 0;
    for (int32_t w = first; w < last; ++w) {
        const uint32_t t = p + 0x410 + 0x1ec * static_cast<uint32_t>(w);
        if (Eq(m.f(t), kZero)) continue;
        const int32_t axle = w / 3;
        const uint32_t e = SpecEntry(m, p);
        uint32_t f0 = Mul(m.f(p + 0x1040 + 4 * static_cast<uint32_t>(axle)), kHalf);
        const uint32_t k = m.u8(e + 12 + static_cast<uint32_t>(axle));
        const uint32_t x = Add(f0, m.f(p + 20 + 88 * static_cast<uint32_t>(axle) + 816));
        f0 = PerRadius(m, x, p, static_cast<int32_t>(k));
        f0 = GeoCurveLinear(Curve(m, t + 1468 - 0x410), f0);
        m.w32(t, Mul(m.f(t), f0));
    }
    m.w8(p + 4170, 0);
    if (!Eq(m.f(p + 1048), m.f(p + 2524))) m.w8(p + 4170, 1);
}

} // namespace

namespace guest {
uint32_t ShapeSumLimited(M& m, uint32_t t, int16_t a, int16_t b, uint32_t* out) {
    return ShapeSumLimitedImpl(m, t, a, b, out);
}
} // namespace guest

// 0x00361908
void PhysInit(Guest g, uint32_t p, uint32_t c, int32_t mode, uint32_t /*arg4*/, uint32_t table, uint32_t opt) {
    M m{g.ram};
    Opt def;
    const uint8_t* op = OptPtr(m, opt, def);
    m.w8(p + 0x10d3, 0);
    m.w8(p + 0x33, m.u8(c + 0x23));
    if (m.u8(c + 0x210) >= 150) m.w8(p + 0x10d3, 1);
    m.w8(p + 0x32, m.u8(c + 1));
    m.w8(p + 0x30, m.u8(c + 2));
    if (m.u8(c + 3) != 0) {
        m.w8(p + 0x10d2, m.u8(c + 3));
    } else {
        const uint8_t k = m.u8(c + 0x128);
        m.w8(p + 0x10d2, k == 3 ? 2 : k == 4 ? 3 : 1);
    }
    m.w8(p + 0x1076, m.u8(c + 0xa8));
    if (m.u8(c + 0x128) >= 5) m.w8(c + 0x128, 0);
    m.w16(p + 0x1048, m.u16(c + 0x20));
    if (m.s16(c + 0x20) == 0) m.w32(p + 0x48, 0x44960000u);   // 1200 kg
    else m.w32(p + 0x48, I2F(m.s16(c + 0x20)));
    const uint8_t pct = m.u8(c + 8);
    if (pct != 0 && pct != 100) {
        const int32_t w = m.s16(p + 0x1048);
        const uint32_t f1 = Div(U8F(pct), kHundred);
        const int32_t prod = static_cast<int32_t>(static_cast<uint32_t>(w) * pct);
        m.w32(p + 0x48, Mul(m.f(p + 0x48), f1));
        m.w16(p + 0x1048, static_cast<uint32_t>(prod / 100));
    }
    const uint32_t f22 = kMilli, f21c = kCenti;
    m.w32(p + 0, Mul(I2F(m.s16(c + 0x12)), f22));
    uint32_t f20 = Mul(U8F(m.u8(c + 0x22)), f21c);   // front weight fraction
    uint32_t f25 = kZero;
    m.w32(p + 4, Mul(I2F(m.s16(c + 0x14)), f22));
    m.w32(p + 0x18, Mul(I2F(m.s16(c + 0x1a)), f22));
    if (Special(m, c)) m.w32(p + 0x18, m.f(OverrideTable(m.u8(p + 0x33)) + 116));
    uint32_t f7;
    if (m.u8(c + 5) != 0) {   // ballast: mass and weight distribution
        const int32_t pos = 50 - m.s8(c + 6);
        const uint32_t ballast = U8F(m.u8(c + 5));
        const uint32_t f3 = f20;
        uint32_t f0 = I2F(pos);
        const uint32_t f2 = m.f(p + 0x48);
        f7 = m.f(p + 0x18);
        uint32_t f4 = Mul(f2, f3);
        f0 = Mul(f0, f21c);
        const uint32_t f6 = Add(f2, ballast);
        const uint32_t f5 = Mul(ballast, f0);
        m.w32(p + 0x48, f6);
        f4 = Add(f4, f5);
        m.w16(p + 0x1048, m.u16(p + 0x1048) + m.u8(c + 5));
        f20 = Div(f4, f6);
        f0 = Sub(f20, f0);
        uint32_t g3 = Sub(f3, f20);
        f0 = Mul(f7, f0);
        g3 = Mul(f7, g3);
        uint32_t a = Mul(ballast, f0);
        uint32_t b = Mul(f2, g3);
        a = Mul(a, f0);
        b = Mul(b, g3);
        f25 = Add(b, a);
    } else {
        f7 = m.f(p + 0x18);
    }
    {
        const uint32_t f0 = Mul(f7, f20);
        const uint32_t f23 = kHalf;
        const uint32_t f21 = Sub(kOne, f20);
        m.w32(p + 0x24, f20);
        const uint32_t f1 = Sub(f7, f0);
        m.w32(p + 0x14, f0);
        m.w32(p + 0x20, f21);
        uint32_t x = Sub(f1, f0);
        m.w32(p + 0x10, f1);
        x = Neg(x);
        m.w32(p + 0x50, Mul(x, f23));
        if (Special(m, c)) {
            const uint32_t o = OverrideTable(m.u8(p + 0x33));
            uint32_t a = Mul(m.f(o + 112), f20);
            uint32_t b = Mul(m.f(o + 20), f21);
            m.w32(p + 0x50, Neg(Add(b, a)));
        }
    }
    {
        const uint32_t shift = m.f(p + 0x50);
        const uint8_t kind0 = m.u8(p + 0x234);
        m.w32(p + 0, Sub(m.f(p + 0), shift));
        m.w32(p + 4, Add(m.f(p + 4), shift));
        m.w32(p + 8, Mul(I2F(m.s16(c + 0x16)), f22));
        m.w32(p + 0x28, Mul(Mul(I2F(m.s16(c + 0x1c) - 2 * m.s8(c + 0x96)), f22), kHalf));
        m.w32(p + 0x2c, Mul(Mul(I2F(m.s16(c + 0x1e) - 2 * m.s8(c + 0x97)), f22), kHalf));
        m.w32(p + 0xc, Mul(I2F(m.s16(c + 0x18)), f22));
        const uint32_t add = kind0 == 7 ? m.u8(0x00620cdbu) : m.u8(0x00620cdbu) + m.u8(0x00620cdcu);
        m.w32(p + 0x48, Add(m.f(p + 0x48), U8F(add)));
    }
    EngineCurve(m, p + 0x134, c, static_cast<int32_t>(m.u8(table + 3) * 10u));
    DrivetrainSetup(m, c, p + 0x234, p + 0x134);
    m.w32(p + 0x104c, U8F(m.u8(c + 0x112)));
    m.w8(p + 0x104b, m.u8(c + 0x11c));
    for (uint32_t i = 0; i < 2; ++i) {   // half track limited by the body width
        const uint8_t v = m.u8(c + 0x1e4 + i);
        const int16_t body = m.s16(c + 0x30 + 2 * i);
        if (v == 0 || v == 100 || body == 0) continue;
        const uint32_t f3 = m.f(p + 0x28 + 4 * i);
        const uint32_t f2 = Add(f3, Mul(m.f(p + 0x2d4 + 4 * i), kHalf));
        const uint32_t f0 = Mul(Mul(I2F(body), kMilli), kHalf);
        if (Lt(f0, f2)) m.w32(p + 0x28 + 4 * i, Sub(f3, Sub(f2, f0)));
    }
    {
        const uint32_t w = m.f(p + 0x24);
        const uint32_t a = Mul(m.f(p + 0x28), w);
        const uint32_t b = Mul(m.f(p + 0x2c), Sub(kOne, w));
        const uint32_t s = Add(a, b);
        m.w32(p + 0x1c, Add(s, s));
    }
    m.w32(p + 0x10d4, Mul(U8F(m.u8(c + 0x11d)), kCenti));
    m.w32(p + 0x10d8, I2F(m.s16(c + 0x11e) * 100));
    Redline(m, c, p);
    m.w8(p + 0x31, DriveLayout(m.u8(c + 0x128), m.u8(c + 0x129)));
    {
        uint32_t a = m.u8(0x00620df0u);
        if (a == 0) a = 50;
        m.w32(p + 0xfcc, Div(0x3fc90fd9u, Mul(U8F(a), kKmh)));
        uint32_t b = m.u8(0x00620df1u);
        if (b == 0) b = 50;
        m.w32(p + 0xfc8, Div(kOne, Mul(U8F(b), kKmh)));
    }
    TyreSetup(m, p, c, 0);
    for (uint32_t i = 0; i < 2; ++i) {   // steering-like curves from the 0x1ce counts
        const uint32_t n = m.u8(c + 0x1ce + i);
        if (n != 0) {
            uint32_t a3 = p + 1004 + 4 * (n + 4 * i);
            for (uint32_t j = 0; j < n; ++j) {
                const uint32_t b0 = m.u8(c + 464 + 4 * i + j), b1 = m.u8(c + 472 + 4 * i + j);
                uint32_t x = Mul(I2F(128 - static_cast<int32_t>(b0)), 0x3d8efa34u);   // 0.06981316
                const uint32_t y = Div(U8F(b1), 0x43480000u);
                x = Mul(x, 0x3d000000u);   // 0.03125
                m.w32(a3 - 32, x);
                m.w32(a3, y);
                a3 -= 4;
            }
        }
        SetCurve(m, p + 952 + 12 * i, n, p + 976 + 16 * i, p + 1008 + 16 * i);
    }
    {
        const uint32_t k98 = 0x42c3ffffu, k9800 = 0x46191fffu;
        for (uint32_t i = 0; i < 2; ++i) {
            const uint32_t a = Mul(U8F(m.u8(0x00620d94u)), k98);
            uint32_t b = Mul(U8F(m.u8(0x00620d95u)), k98);
            b = Sub(b, a);
            m.w32(p + 0x1088 + 4 * i, Sub(a, b));
            const uint32_t r = Mul(m.f(p + 0x2d4 + 4 * i), k9800);
            m.w32(p + 0x1090 + 4 * i, Div(b, r));
        }
    }
    int32_t toe[2] = {m.u8(c + 0x2a), m.u8(c + 0x2b)};
    for (uint32_t i = 0; i < 2; ++i) m.w32(p + 0xfd0 + 4 * i, Mul(I2F(toe[i] + 900), kMilli));
    if (mode == 2) {
        const uint32_t e = table + m.u8(p + 0x31);
        toe[0] = m.u8(e + 4);
        toe[1] = m.u8(e + 8);
        for (uint32_t i = 0; i < 2; ++i) {
            if (toe[i] == 0) toe[i] = 100;
            m.w32(p + 0xfd0 + 4 * i, Mul(m.f(p + 0xfd0 + 4 * i), Mul(I2F(toe[i]), kCenti)));
        }
    }
    uint32_t drag;   // f3
    {
        const int16_t cd = m.s16(c + 0x40);
        const uint32_t k = m.f(0x00622308u);
        if (cd == 0) {
            uint32_t f0 = Mul(m.f(p + 8), m.f(p + 0xc));
            f0 = Mul(f0, k);
            drag = Mul(f0, kHalf);
        } else {
            const uint32_t f3 = Div(I2F(cd), 0x461c4000u);
            drag = Mul(f3, Mul(k, kHalf));
        }
    }
    {
        const uint32_t f0 = Mul(U8F(m.u8(c + 0x42)), kCenti);
        m.w32(p + 0x1064, f0);
        m.w32(p + 0x1068, Mul(drag, f0));
        m.w32(p + 0x106c, Mul(drag, Mul(U8F(m.u8(c + 0x43)), kCenti)));
        m.w32(p + 0x1070, Mul(drag, Mul(U8F(m.u8(c + 0x44)), kCenti)));
    }
    const uint8_t wing = m.u8(c + 0x4c);
    m.w8(p + 0x1077, wing);
    if (wing == 0) {
        m.w32(p + 0x1078, kZero);
        m.w32(p + 0x107c, kZero);
        m.w32(p + 0x1080, kZero);
        m.w32(p + 0x1084, kZero);
    } else {
        m.w32(p + 0x1078, Mul(U8F(m.u8(c + 0x4d)), kKmh));
        m.w32(p + 0x107c, Mul(U8F(m.u8(c + 0x4e)), kKmh));
        const uint8_t t = m.u8(c + 0x4f);
        m.w32(p + 0x1080, t == 0 ? 0x43960000u : Div(0x41200000u, U8F(t)));
        m.w32(p + 0x1084, Mul(drag, Mul(U8F(m.u8(c + 0x50)), kCenti)));
        if (m.u8(p + 0x1077) == 7) m.w32(p + 0x1084, Mul(U8F(m.u8(c + 0x50)), 0x42c3ffffu));
    }
    {
        const uint32_t s = Add(m.f(p + 0x106c), m.f(p + 0x1070));
        const uint32_t k = Mul(U8F(m.u8(0x00620ce2u)), kMilli);
        m.w32(p + 0x1068, Add(m.f(p + 0x1068), Mul(s, k)));
    }
    for (uint32_t i = 0; i < 2; ++i) {   // brakes
        const uint8_t v = m.u8(c + 0x178 + i);
        const uint32_t force = Mul(U8F(v * 10u), kG);
        m.w32(p + 0x1010 + 4 * i, force);
        m.w32(p + 0x1024 + 4 * i, Mul(U8F(m.u8(c + 0x1a8 + i)), kTenth));
        m.w32(p + 0x102c + 4 * i, Mul(U8F(m.u8(c + 0x1aa + i)), kCenti));
        if (v == 0) m.w32(p + 0x1034 + 4 * i, kZero);
        else m.w32(p + 0x1034 + 4 * i, Div(m.f(p + 0x2c4 + 4 * i), m.f(p + 0x1010 + 4 * i)));
    }
    m.w32(p + 0x1018, Mul(U8F(m.u8(c + 0x17a) * 10u), kG));
    m.w8(p + 0x10d0, m.u8(c + 0x2c));
    m.w32(p + 0x10cc, Mul(U8F(m.u8(c + 0x2d)), kCenti));
    {   // ASCC / TCS
        const uint32_t a3 = p + 0x1098;
        const uint8_t flags = m.u8(c + 0x1b8);
        m.w32(a3, Mul(U8F(m.u8(c + 0x1ba)), kCenti));
        m.w32(a3 + 4, Mul(U8F(m.u8(c + 0x1bb)), kCenti));
        m.w32(a3 + 8, Mul(U8F(m.u8(c + 0x1bc)), kCenti));
        m.w8(a3 + 12, flags & 1);
        m.w8(a3 + 13, (flags >> 1) & 1);
        m.w8(a3 + 14, (flags >> 2) & 1);
        m.w8(a3 + 15, (flags >> 3) & 1);
        uint32_t w = m.u32(a3 + 12);
        m.w8(a3 + 48, (w & 0xffff) != 0 && !Eq(m.f(a3), kZero) ? 1 : 0);
        m.w8(a3 + 49, (w & 0xffff0000u) != 0 && !Eq(m.f(a3 + 8), kZero) ? 1 : 0);
        const uint32_t f2 = Mul(U8F(m.u8(c + 0x1bd)), kCenti);
        m.w32(a3 + 16, f2);
        m.w32(a3 + 20, Mul(U8F(m.u8(c + 0x1be)), kCenti));
        m.w8(a3 + 28, (flags >> 4) & 1);
        m.w8(a3 + 29, (flags >> 5) & 1);
        m.w8(a3 + 30, (flags >> 6) & 1);
        m.w8(a3 + 31, flags >> 7);
        m.w32(a3 + 24, Mul(U8F(m.u8(c + 0x1bf)), kCenti));
        w = m.u32(a3 + 28);
        m.w8(a3 + 50, (w & 0xffff) != 0 && !Eq(f2, kZero) ? 1 : 0);
        m.w8(a3 + 51, (w & 0xffff0000u) != 0 && !Eq(m.f(a3 + 24), kZero) ? 1 : 0);
        m.w32(a3 + 36, Mul(U8F(m.u8(c + 0x1c0)), kTenth));
        m.w32(a3 + 40, Mul(U8F(m.u8(c + 0x1c1)), kCenti));
        m.w32(a3 + 32, Mul(U8F(m.u8(c + 0x1c2)), kTenth));
        const uint8_t t = m.u8(c + 0x1c3);
        m.w32(a3 + 44, (t == 100 || t == 0) ? kOne : Mul(U8F(t), kCenti));
    }
    const uint32_t quarterPi = 0x3f490fd9u;
    m.w32(p + 0x54, Mul(U8F(m.u8(c + 0x28)), kDeg));
    SpeedAngleCurve(m, p + 88, p + 100, p + 124, m.u8(c + 0x180), c + 0x184, c + 0x18a);
    SpeedAngleCurve(m, p + 148, p + 160, p + 184, m.u8(c + 0x182), c + 0x19c, c + 0x1a2);
    SpeedAngleCurve(m, p + 208, p + 220, p + 244, m.u8(c + 0x181), c + 0x190, c + 0x196);
    {
        const uint32_t wb = m.f(p + 0x18), tr = m.f(p + 0x28);
        m.w32(p + 0x10c, Div(LibmAtan2(wb, Sub(wb, tr)), quarterPi));
        m.w32(p + 0x110, Div(LibmAtan2(wb, Add(wb, tr)), quarterPi));
    }
    if (m.u8(c + 0x29) == 0) {
        m.w32(p + 0x1004, kZero);
    } else {
        uint32_t a = Mul(U8F(m.u8(c + 0x29)), kDeg);
        if (Special(m, c)) a = m.f(OverrideTable(m.u8(p + 0x33)) + 48);
        m.w32(p + 0x1004, LibmSin(a));
    }
    for (uint32_t i = 0; i < 2; ++i) {   // caster / camber / toe per axle
        uint32_t x = Mul(I2F(static_cast<int32_t>(m.u8(c + 0x8e + i)) - 128), kHalf);
        x = Mul(x, kMilli);
        m.w32(p + 0x1008 + 4 * i, LibmAtan2(x, m.f(p + 0x2c4 + 4 * i)));
        m.w32(p + 0xfe4 + 4 * i, Neg(Mul(Mul(U8F(m.u8(c + 0x88 + i)), kTenth), kDeg)));
        m.w32(p + 0xfec + 4 * i, Mul(I2F(m.s8(c + 0x90 + i)), kDeg));
        m.w32(p + 0xff4 + 4 * i, GeoCurveBinary(Curve(m, p + 952 + 12 * i), m.f(p + 0xfe4 + 4 * i)));
        m.w32(p + 0xffc + 4 * i, Mul(U8F(m.u8(c + 0x98 + i)), kCenti));
    }
    {
        uint32_t f1 = 0x45bb8000u;   // 6000 rpm
        const uint8_t rl = m.u8(c + 0x24);
        if (rl != 0) f1 = Mul(U8F(rl), kHundred);
        m.w32(p + 0x114, f1);
        if (m.u8(c + 5) != 0) m.w32(p + 0x114, Add(f1, f25));
        m.w32(p + 0x118, Div(kOne, f1));
    }
    uint32_t f21 = 0x457a0000u;   // 4000
    if (m.u8(c + 0x25) != 0) f21 = Mul(U8F(m.u8(c + 0x25)), kHundred);
    m.w32(p + 0x11c, Div(m.f(p + 0x10), f21));
    m.w32(p + 0x120, Div(m.f(p + 0x14), f21));
    {
        uint32_t f2 = 0x44fa0000u;   // 2000
        if (m.u8(c + 0x26) != 0) f2 = Mul(U8F(m.u8(c + 0x26)), kHundred);
        m.w32(p + 0x124, Div(m.f(p + 0x28), f2));
        m.w32(p + 0x128, Div(m.f(p + 0x2c), f2));
    }
    SuspensionConstants(m);
    SuspensionAxle(m, p + 0x308, c + 0x58, m.u8(c + 0x8c));
    SuspensionAxle(m, p + 0x360, c + 0x70, m.u8(c + 0x8d));
    {
        const int16_t a = m.s16(c + 0x9a);
        m.w32(p + 0x40, Mul(I2F(a == 0 ? m.u8(c + 0x8a) : a), kMilli));
        const int16_t b = m.s16(c + 0x9c);
        m.w32(p + 0x44, Mul(I2F(b == 0 ? m.u8(c + 0x8b) : b), kMilli));
    }
    if (m.u8(p + 0x1077) == 7) {
        uint32_t d = Sub(m.f(p + 0x40), m.f(p + 0x44));
        if (Lt(d, kZero)) d = Neg(d);
        if (Lt(0x3cf5c28fu, d)) {   // 0.03
            m.w32(p + 0x1084, kZero);
            m.w8(p + 0x1077, 0);
        }
    }
    {
        const uint32_t f1 = Mul(m.f(p + 0x40), m.f(p + 0x24));
        const uint32_t f3 = Mul(m.f(p + 0x44), m.f(p + 0x20));
        const uint32_t weight = Mul(m.f(p + 0x48), kG);
        const uint32_t s = Add(f1, f3);
        m.w32(p + 0x3c, s);
        m.w32(p + 0x103c, weight);
        const uint32_t cg = Mul(U8F(m.u8(c + 0x27)), kMilli);
        const uint32_t h = Mul(Add(cg, s), kHalf);
        m.w32(p + 0x1060, cg);
        m.w32(p + 0x38, Div(h, m.f(p + 0x18)));
        m.w32(p + 0x34, Div(h, m.f(p + 0x1c)));
    }
    const uint32_t e = SpecEntry(m, p);
    uint32_t f5 = m.f(p + 0x48), f6;
    if (m.u8(e + 4) == 2 && m.u8(e + 5) == 2) {
        uint32_t s = Add(m.f(p + 0x340), m.f(p + 0x398));
        s = Add(s, s);
        f6 = Sub(f5, s);
    } else {
        const uint32_t a = Mul(m.f(p + 0x398), U8F(m.u8(e + 5)));
        const uint32_t b = Mul(m.f(p + 0x340), U8F(m.u8(e + 4)));
        f6 = Sub(f5, Add(b, a));
    }
    if (!Le(f6, kZero)) {   // sprung mass
        const uint32_t ratio = Div(f6, f5);
        const uint32_t w = Mul(f6, kG);
        m.w32(p + 0x4c, f6);
        m.w32(p + 0x34, Mul(f6, m.f(p + 0x34)));
        m.w32(p + 0x1044, Mul(w, m.f(p + 0x20)));
        m.w32(p + 0x38, Mul(ratio, m.f(p + 0x38)));
        m.w32(p + 0x1040, Mul(w, m.f(p + 0x24)));
        const uint32_t e2 = SpecEntry(m, p);
        m.w32(p + 0x1050, U8F(m.u8(c + 0x38)));
        m.w32(p + 0x1054, U8F(m.u8(c + 0x39)));
        uint32_t f1, f3;
        const int16_t ax = m.s16(c + 0x3a);
        if (ax != 0) {
            const uint32_t f0 = Mul(I2F(ax), kG);
            f3 = Mul(f0, m.f(p + 0x20));
            f1 = Mul(f0, m.f(p + 0x24));
        } else {
            f1 = m.f(p + 0x1040);
            f3 = m.f(p + 0x1044);
        }
        f5 = m.f(p + 0x48);
        m.w32(p + 0x1058, Div(U8F(m.u8(e2 + 4)), f1));
        m.w32(p + 0x105c, Div(U8F(m.u8(e2 + 5)), f3));
    }
    {
        const uint32_t f3 = m.f(p + 0x10), f4 = m.f(p + 0x14);
        uint32_t a = Mul(f5, f3), b = Mul(f5, f4);
        const uint32_t k = Mul(U8F(m.u8(0x00620cdfu)), kCenti);
        a = Mul(a, f3);
        b = Mul(b, f4);
        a = Add(f21, a);
        b = Add(f21, b);
        m.w32(p + 0x12c, Div(k, a));
        m.w32(p + 0x130, Div(k, b));
    }
    TyreLoadScale(m, p, 0);
    for (uint32_t i = 0; i < 2; ++i) SuspensionTravel(m, p, p + 0x308 + 0x58 * i, c, i);
    {
        const uint32_t f4 = m.f(p + 0x310), f3 = m.f(p + 0x368);
        const uint32_t f2 = Sub(m.f(p + 0x314), f4);
        const uint32_t f0 = Sub(m.f(p + 0x36c), f3);
        m.w32(p + 0x34c, f4);
        m.w32(p + 0x3a4, f3);
        if (Lt(f0, f2)) m.w32(p + 0x3a4, Sub(f3, Sub(f2, f0)));
        else if (Lt(f2, f0)) m.w32(p + 0x34c, Sub(f4, Sub(f0, f2)));
    }
    PowerSetup(m, p, c, op);
}

namespace {

// 0x003f2f78: physics constants from the settings block 0x00620cd8
void PhysicsSettingsConstants(M& m) {
    const uint32_t s = 0x00620cd8u;
    const uint8_t a = m.u8(s);
    const uint32_t f2 = U8F(a);
    m.w32(0x00622308u, Mul(f2, kTenth));
    if (a != 0) {
        const uint32_t f0 = Div(U8F(m.u8(s + 2)), f2);
        const uint32_t f1 = Div(U8F(m.u8(s + 1)), f2);
        m.w32(0x00622310u, f0);
        m.w32(0x0062230cu, f1);
    } else {
        m.w32(0x0062230cu, kZero);
        m.w32(0x00622310u, kZero);
    }
    const uint8_t b = m.u8(s + 5);
    m.w32(0x00622314u, b ? Div(0x41200000u, U8F(b)) : kOne);
    const uint8_t c = m.u8(s + 6);
    m.w32(0x00622318u, c ? Div(0x41200000u, U8F(c)) : 0x3e800000u);
}

} // namespace

// 0x0035f808(race)
void RaceSettings(Guest g, int32_t raceKind) {
    M m{g.ram};
    PhysicsSettingsConstants(m);
    const uint32_t n = m.u8(0x00620ce3u);
    for (uint32_t j = 0; j < n; ++j) {
        m.w32(0x00620944u + 4 * j, Mul(U8F(m.u8(0x00620ce4u + j) * 2u), kKmh));
        m.w32(0x00620964u + 4 * j, Mul(U8F(m.u8(0x00620cecu + j)), kMilli));
    }
    SetCurve(m, 0x00620938u, n, 0x00620944u, 0x00620964u);
    if (raceKind < 0) return;
    for (uint32_t i = 0; i < 24; ++i) {
        uint32_t k = i;
        if (raceKind == 2 && i == 5) k = 15;
        const uint32_t period = Mul(U8F(m.u8(0x00620d3cu + k)), kTenth);
        const uint32_t a = Mul(I2F(m.s16(0x00620d0cu + 2 * k)), kCenti);
        const uint32_t b = Mul(U8F(m.u8(0x00620cf4u + k)), kMilli);
        m.w32(0x00620984u + 4 * i, a);
        m.w32(0x006209e4u + 4 * i, b);
        m.w32(0x00620a44u + 4 * i, Div(kTwoPi, period));
        m.w8(0x00620aa4u + i, m.u8(0x00620d54u + k));
        m.w8(0x00620abcu + i, m.u8(0x00620d6cu + k));
    }
}

// 0x00364818(obj, opt): engine peak record at phys + 0x1100 (power + the extra power of the drive kind)
void PhysPeakRecord(Guest g, uint32_t phys, uint32_t opt) {
    M m{g.ram};
    Opt def;
    const uint8_t* op = OptPtr(m, opt, def);
    const uint32_t o = phys + 0x1100;
    EnginePeaks(m, phys + 0x134, o, op);
    const uint32_t extra = ExtraPower(m, phys + 0x234);
    m.w32(o, Add(m.f(o), extra));
}


// 0x00443240
uint32_t GarageGearRatios(Guest g, uint32_t car, uint32_t sp, const SpecSource& db) {
    M m{g.ram};
    const uint32_t F = sp - 0x670;
    uint64_t key;
    std::memcpy(&key, m.p(car + 0x48), 8);
    if (!db.Exists(key)) return 0;
    db.Fetch(key, m.p(F));
    if (m.u8(F + 37) == 0) return 1;
    // builder on the guest frame: its own frame at F - 0x100, callee buffers below
    BuildFrame frame;
    std::memcpy(frame.frame, m.p(F - 0x100), sizeof frame.frame);
    std::memcpy(frame.sub, m.p(F - 0x100 - sizeof frame.sub), sizeof frame.sub);
    BuildGlobals bg;
    bg.flag006224a8 = m.u8(0x006224a8u);
    BuildCarParams(m.p(car), m.p(F + 80), db, frame, bg);
    std::memcpy(m.p(F - 0x100), frame.frame, sizeof frame.frame);
    AutoGears(m, F + 80, static_cast<int32_t>(m.u8(car + 298) * 10u), F + 48, true);
    for (uint32_t i = 0; i < 12; ++i) m.w16(car + 272 + 2 * i, m.u16(F + 48 + 2 * i));
    return 1;
}

// 0x00364868(obj = {cprm}, car, opt)
void SetupCar(Guest g, uint32_t cprm, uint8_t* car, const SpecSource& db, BuildFrame& frame, uint32_t opt) {
    RaceSettings(g, -1);
    const RaceGlobals rg = ReadRaceGlobals(g.ram);
    BuildRaceCarParams(g.ram + cprm, car, 0, db, frame, rg);
    PhysInit(g, cprm + 0x600, cprm, 0, 0, 0x006203c8u, opt);
    PhysPeakRecord(g, cprm + 0x600, opt);
}

// 0x00361570(phys, cprm, mode, 0): per-wheel tyre records (the pit stop's tyre rebuild, pit_rebuild.h).
void PhysTyreSetup(Guest g, uint32_t phys, uint32_t cprm, int32_t mode) {
    M m{g.ram};
    TyreSetup(m, phys, cprm, mode);
}

} // namespace gt4::game::carparams
