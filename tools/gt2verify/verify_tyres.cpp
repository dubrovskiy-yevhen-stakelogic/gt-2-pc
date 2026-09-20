// Differential checks of src/game/sim/tyres.* against the original (see guest.h for the harness).
#include <cstring>

#include "game/sim/field.h"
#include "game/sim/fixed.h"
#include "game/sim/trig.h"
#include "game/sim/tyres.h"
#include "guest.h"

namespace gt2::verify {

namespace {

// ---- access to the original's objects inside a RAM image
uint32_t ReadU32(const uint8_t* ram, uint32_t address) {
    uint32_t v;
    std::memcpy(&v, ram + (address & 0x1FFFFF), 4);
    return v;
}
uint16_t ReadU16(const uint8_t* ram, uint32_t address) {
    uint16_t v;
    std::memcpy(&v, ram + (address & 0x1FFFFF), 2);
    return v;
}
void WriteU32(uint8_t* ram, uint32_t address, uint32_t v) { std::memcpy(ram + (address & 0x1FFFFF), &v, 4); }
void WriteU16(uint8_t* ram, uint32_t address, uint16_t v) { std::memcpy(ram + (address & 0x1FFFFF), &v, 2); }

sim::CarBody& BodyAt(uint8_t* ram, uint32_t body) { return *reinterpret_cast<sim::CarBody*>(ram + (body & 0x1FFFFF)); }
uint32_t BodyAddress(uint32_t car) { return kCarBase + car * kCarStride + kBodyOffset; }
sim::CarScratch& ScratchAt(uint8_t* scratch, uint32_t car) { return *reinterpret_cast<sim::CarScratch*>(scratch + sim::kScratchCarBlocks + car * sizeof(sim::CarScratch)); }

// Table objects { u16 count; s16*/s32* xs; ys } hold guest pointers; resolve them into the image.
sim::CurveS16 CurveS16At(const uint8_t* ram, uint32_t table) {
    sim::CurveS16 c;
    c.count = ReadU16(ram, table);
    c.xs = reinterpret_cast<const int16_t*>(ram + (ReadU32(ram, table + 4) & 0x1FFFFF));
    c.ys = reinterpret_cast<const int16_t*>(ram + (ReadU32(ram, table + 8) & 0x1FFFFF));
    return c;
}
sim::CurveS32 CurveS32At(const uint8_t* ram, uint32_t table) {
    sim::CurveS32 c;
    c.count = ReadU16(ram, table);
    c.xs = reinterpret_cast<const int32_t*>(ram + (ReadU32(ram, table + 4) & 0x1FFFFF));
    c.ys = reinterpret_cast<const int32_t*>(ram + (ReadU32(ram, table + 8) & 0x1FFFFF));
    return c;
}
void ResolveAxleCurves(const uint8_t* ram, uint32_t body, sim::AxleTyreCurves curves[2]) {
    for (uint32_t axle = 0; axle < 2; axle++) {
        const uint32_t block = body + 0x194 + axle * 0xD8;
        curves[axle].slipAngleForce = CurveS16At(ram, block + 0x00);
        curves[axle].slipRatioForce = CurveS16At(ram, block + 0x2C);
        curves[axle].slipRatioGrip = CurveS16At(ram, block + 0x6C).ys;
        curves[axle].loadGrip = CurveS32At(ram, block + 0x90);
        curves[axle].camberGrip = CurveS16At(ram, block + 0xBC);
    }
}
constexpr uint32_t kWearConstants = 0x80046F48u; // seven s32: limit, wornLoss, pitFactor, coldLimit, coldLoss, knee, kneeLoss
sim::TyreWearConstants WearConstantsAt(const uint8_t* ram) {
    sim::TyreWearConstants k;
    k.wearLimit = int32_t(ReadU32(ram, D(kWearConstants) + 0x00));
    k.wornGripLoss = int32_t(ReadU32(ram, D(kWearConstants) + 0x04));
    k.pitGripFactor = int32_t(ReadU32(ram, D(kWearConstants) + 0x08));
    k.coldLimit = int32_t(ReadU32(ram, D(kWearConstants) + 0x0C));
    k.coldGripLoss = int32_t(ReadU32(ram, D(kWearConstants) + 0x10));
    k.wearKnee = int32_t(ReadU32(ram, D(kWearConstants) + 0x14));
    k.kneeGripLoss = int32_t(ReadU32(ram, D(kWearConstants) + 0x18));
    return k;
}

// Signed random value with |v| < limit.
int32_t Signed(std::mt19937& rng, uint32_t limit) { return int32_t(rng() % limit) * ((rng() & 1) ? 1 : -1); }
// Velocity-like value in one of four magnitude classes: standstill, slow, racing speed, extreme.
int32_t Speed(std::mt19937& rng, size_t variant) {
    const uint32_t limit = variant % 4 == 1 ? 100u : variant % 4 == 2 ? 0x8000u : variant % 4 == 3 ? 0x60000u : 0x7FFFFFFFu;
    return Signed(rng, limit);
}

// Synthetic 16-bit curve placed in the guest's free RAM: table object at `table`, samples at `xs` / `ys`.
struct SyntheticCurve {
    std::vector<int16_t> xs, ys;
    void Fill(std::mt19937& rng, uint32_t count, int32_t firstMin, int32_t firstMax, int32_t maxStep, bool fullRangeYs) {
        xs.resize(count);
        ys.resize(count);
        int32_t x = firstMin + int32_t(rng() % uint32_t(firstMax - firstMin + 1));
        for (uint32_t i = 0; i < count; i++) {
            if (i) x += int32_t(rng() % uint32_t(maxStep)) + (rng() % 5 == 0 ? 0 : 1); // ascending, occasionally repeated
            if (x > 0x7FFF) x = 0x7FFF;
            xs[i] = int16_t(x);
            ys[i] = fullRangeYs ? int16_t(rng() & 0xFFFF) : int16_t(rng() % 0x2000);
        }
    }
    void Place(uint8_t* ram, uint32_t table, uint32_t xsAddress, uint32_t ysAddress) const {
        WriteU16(ram, table, uint16_t(xs.size()));
        WriteU32(ram, table + 4, xsAddress);
        WriteU32(ram, table + 8, ysAddress);
        std::memcpy(ram + (xsAddress & 0x1FFFFF), xs.data(), xs.size() * 2);
        std::memcpy(ram + (ysAddress & 0x1FFFFF), ys.data(), ys.size() * 2);
    }
    sim::CurveS16 Curve() const { return {xs.data(), ys.data(), uint32_t(xs.size())}; }
};

} // namespace

int VerifyTyres(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng) {
    int failures = 0;
    std::vector<uint32_t> bodies;
    for (uint32_t car = 0; car < kCarCount; car++) bodies.push_back(BodyAddress(car));

    // ---- integer square root (pure)
    {
        size_t cases = 0, bad = 0;
        auto check = [&](int32_t value, uint32_t extra) {
            cases++;
            const int32_t original = int32_t(guest.Call(0x80081288, uint32_t(value), extra)), ours = sim::SquareRoot(value, extra);
            if (original != ours && bad++ < 3) std::printf("    MISMATCH SquareRoot(%d, %u): original %d, ours %d\n", value, extra, original, ours);
        };
        const int32_t edges[] = {0, 1, 2, 3, 4, 15, 16, 17, 4095, 4096, 4097, 0xFFFF, 0x10000, 0x7FFFFFFF, int32_t(0x80000000), -1, -2, -4096, 0x40000000, 0x3FFFFFFF};
        for (int32_t v : edges) for (uint32_t extra = 0; extra <= 8; extra++) check(v, extra);
        for (int i = 0; i < 30000; i++) {
            const uint32_t limit = i % 4 == 0 ? 0x1000u : i % 4 == 1 ? 0x100000u : i % 4 == 2 ? 0x7FFFFFFFu : 0xFFFFFFFFu;
            check(int32_t(rng() % limit), uint32_t(rng() % 11));
        }
        Report("SquareRoot", 0x80081288u, cases, bad, failures);
    }

    // ---- 16-bit curve lookups on synthetic tables in free guest RAM
    constexpr uint32_t kTable = 0x801E8000u, kXs = 0x801E8010u, kYs = 0x801E8810u;
    constexpr uint32_t kTable2 = 0x801E9000u, kXs2 = 0x801E9010u, kYs2 = 0x801E9810u, kOut1 = 0x801EA000u, kOut2 = 0x801EA004u;
    {
        size_t cases = 0, bad = 0;
        SyntheticCurve curve;
        for (int variant = 0; variant < 2000; variant++) {
            const bool wide = variant % 3 == 0;
            curve.Fill(rng, 1u + uint32_t(rng() % 24), wide ? -0x7000 : -0x800, wide ? 0 : 0x800, wide ? 0x1800 : 0x300, wide);
            curve.Place(guest.Ram(), kTable, kXs, kYs);
            for (int q = 0; q < 20; q++) {
                const int32_t x = q < 4 ? curve.xs[rng() % curve.xs.size()] + q - 2 : curve.xs.front() - 300 + int32_t(rng() % uint32_t(curve.xs.back() - curve.xs.front() + 600));
                const int16_t x16 = int16_t(x);
                cases++;
                const int32_t original = int32_t(guest.Call(0x8003D848, kTable, uint32_t(x16))), ours = sim::InterpolateS16(curve.Curve(), x16);
                if (original != ours && bad++ < 3) std::printf("    MISMATCH InterpolateS16(count %zu, %d): original %d, ours %d\n", curve.xs.size(), x16, original, ours);
            }
        }
        Report("InterpS16", 0x8003D848u, cases, bad, failures);
    }
    {
        size_t cases = 0, bad = 0;
        SyntheticCurve curve, second;
        for (int variant = 0; variant < 2000; variant++) {
            const bool wide = variant % 3 == 0;
            const uint32_t count = 1u + uint32_t(rng() % 24);
            curve.Fill(rng, count, wide ? -0x7000 : -0x1000, wide ? 0 : 0x1000, wide ? 0x1800 : 0x300, wide);
            second.Fill(rng, count, 0, 0, 1, wide); // only its ys are used
            curve.Place(guest.Ram(), kTable, kXs, kYs);
            second.Place(guest.Ram(), kTable2, kXs2, kYs2); // its xs / count are ignored by the original
            for (int q = 0; q < 20; q++) {
                const int32_t x = q < 4 ? curve.xs[rng() % count] + q - 2 : curve.xs.front() - 300 + int32_t(rng() % uint32_t(curve.xs.back() - curve.xs.front() + 600));
                const int16_t x16 = int16_t(x);
                WriteU32(guest.Ram(), kStack + 16, kOut2); // fifth argument on the stack
                WriteU16(guest.Ram(), kOut1, 0x5A5A);
                WriteU16(guest.Ram(), kOut2, 0x5A5A);
                cases++;
                guest.Call(0x8003D940, kTable, kTable2, uint32_t(x16), kOut1);
                int16_t y = 0, secondY = 0;
                sim::InterpolateS16Pair(curve.Curve(), second.ys.data(), x16, y, secondY);
                const int16_t originalY = int16_t(ReadU16(guest.Ram(), kOut1)), originalSecond = int16_t(ReadU16(guest.Ram(), kOut2));
                if ((originalY != y || originalSecond != secondY) && bad++ < 3)
                    std::printf("    MISMATCH InterpolateS16Pair(count %u, %d): original %d %d, ours %d %d\n", count, x16, originalY, originalSecond, y, secondY);
            }
        }
        Report("InterpPair", 0x8003D940u, cases, bad, failures);
    }
    {
        size_t cases = 0, bad = 0;
        SyntheticCurve curve;
        for (int variant = 0; variant < 1500; variant++) {
            curve.Fill(rng, 1u + uint32_t(rng() % 12), 0, 0x40, 0x100, variant % 5 == 0);
            curve.Place(guest.Ram(), kTable, kXs, kYs);
            for (int q = 0; q < 20; q++) {
                const uint32_t limit = q % 4 == 0 ? 0x400u : q % 4 == 1 ? 0x1000u : q % 4 == 2 ? 0x20000u : 0x7FFFFFFFu;
                const int32_t x = q == 0 ? int32_t(rng() % 5) - 2 : Signed(rng, limit);
                cases++;
                const int32_t original = int32_t(guest.Call(0x80039F4C, kTable, uint32_t(x))), ours = sim::SymmetricCurve(curve.Curve(), x);
                if (original != ours && bad++ < 3) std::printf("    MISMATCH SymmetricCurve(count %zu, %d): original %d, ours %d\n", curve.xs.size(), x, original, ours);
            }
        }
        Report("SymCurve", 0x80039F4Cu, cases, bad, failures);
    }

    // ---- per-wheel slip ratio: (body, car) per car, then the loop over all cars
    auto prepareSlipInputs = [&](uint8_t* ram, uint8_t* scratch, uint32_t car, size_t variant) {
        sim::CarBody& b = BodyAt(ram, BodyAddress(car));
        for (uint32_t w = 0; w < 4; w++) {
            sim::Wheel& wheel = b.wheels[w];
            if (variant % 4 != 0) { // keep the dump's values for a quarter of the cases
                wheel.steerAngle = int16_t(Signed(rng, variant % 8 < 4 ? 0x200u : 0x1000u));
                sim::SetField<int32_t>(&wheel, 0x18, Speed(rng, variant));
                sim::SetField<int32_t>(&wheel, 0x2C, Speed(rng, variant + uint32_t(rng() % 4)));
                sim::SetField<int32_t>(&wheel, 0x30, Speed(rng, variant + uint32_t(rng() % 4)));
                sim::SetField<int16_t>(&wheel, 0x2A, int16_t(variant % 3 == 0 ? 0x1000 : Signed(rng, 0x8000u)));
                if (rng() % 8 == 0) // rim speed close to the road speed
                    sim::SetField<int32_t>(&wheel, 0x18, int32_t(uint32_t(sim::Field<int32_t>(&wheel, 0x2C)) + rng() % 141 - 70u));
            }
            // The tick writes sin / cos of the steer angle into the record before the tyre code runs.
            sim::WheelScratch& record = ScratchAt(scratch, car).wheels[w];
            record.steerSin = int16_t(sim::Sin(uint32_t(wheel.steerAngle)));
            record.steerCos = int16_t(sim::Cos(uint32_t(wheel.steerAngle)));
        }
    };
    {
        size_t cases = 0, mismatches = 0;
        for (uint32_t car = 0; car < kCarCount; car++) {
            const StatefulResult r = VerifyStateful(
                guest, pristine, 0x80039490, {BodyAddress(car)}, 120,
                [&](uint8_t* ram, uint8_t* scratch, uint32_t, size_t variant) { prepareSlipInputs(ram, scratch, car, variant); },
                [&](uint8_t* ram, uint8_t* scratch, uint32_t body) { sim::UpdateWheelSlipRatios(BodyAt(ram, body), ScratchAt(scratch, car)); }, car);
            cases += r.cases;
            mismatches += r.mismatches;
        }
        Report("WheelSlip", 0x80039490u, cases, mismatches, failures);
    }
    {
        const StatefulResult r = VerifyStateful(
            guest, pristine, 0x80039778, {kCarBase}, 200,
            [&](uint8_t* ram, uint8_t* scratch, uint32_t, size_t variant) {
                for (uint32_t car = 0; car < kCarCount; car++) prepareSlipInputs(ram, scratch, car, variant + car);
            },
            [&](uint8_t* ram, uint8_t* scratch, uint32_t) {
                sim::CarBody* cars[kMaxCars];
                for (uint32_t car = 0; car < kCarCount; car++) cars[car] = &BodyAt(ram, BodyAddress(car));
                sim::UpdateSlipRatios(cars, kCarCount, &ScratchAt(scratch, 0));
            },
            kCarCount);
        Report("SlipRatios", 0x80039778u, r.cases, r.mismatches, failures);
    }

    // ---- slip-ratio curves into the scratch record and the wheel
    {
        const StatefulResult r = VerifyStateful(
            guest, pristine, 0x800397D0, {kCarBase}, 300,
            [&](uint8_t* ram, uint8_t* scratch, uint32_t, size_t variant) {
                for (uint32_t car = 0; car < kCarCount; car++)
                    for (uint32_t w = 0; w < 4; w++) {
                        sim::Wheel& wheel = BodyAt(ram, BodyAddress(car)).wheels[w];
                        // The record's ratio is what 0x80039490 left there: the dump's copy at wheel + 0x44, or random.
                        int16_t ratio = sim::Field<int16_t>(&wheel, 0x44);
                        if (variant % 4 != 0) ratio = int16_t(variant % 4 == 1 ? Signed(rng, 0x1001u) : Signed(rng, 0x8000u));
                        ScratchAt(scratch, car).wheels[w].slipRatio = ratio;
                    }
            },
            [&](uint8_t* ram, uint8_t* scratch, uint32_t) {
                for (uint32_t car = 0; car < kCarCount; car++) {
                    sim::AxleTyreCurves curves[2];
                    ResolveAxleCurves(ram, BodyAddress(car), curves);
                    sim::EvaluateSlipCurves(BodyAt(ram, BodyAddress(car)), ScratchAt(scratch, car), curves);
                }
            },
            kCarCount);
        Report("SlipCurves", 0x800397D0u, r.cases, r.mismatches, failures);
    }

    // ---- slip angle and its blend from the contact patch velocity
    {
        const StatefulResult r = VerifyStateful(
            guest, pristine, 0x80039DE8, {kCarBase}, 300,
            [&](uint8_t* ram, uint8_t*, uint32_t, size_t variant) {
                if (variant % 4 == 0) return;
                for (uint32_t car = 0; car < kCarCount; car++)
                    for (uint32_t w = 0; w < 4; w++) {
                        sim::Wheel& wheel = BodyAt(ram, BodyAddress(car)).wheels[w];
                        const uint32_t limit = variant % 4 == 1 ? 0x600u : variant % 4 == 2 ? 0x3000u : 0x7FFFFFFFu;
                        sim::SetField<int32_t>(&wheel, 0x2C, Signed(rng, rng() % 3 ? limit : 0x3000u));
                        sim::SetField<int32_t>(&wheel, 0x30, Signed(rng, rng() % 3 ? limit : 0x3000u));
                    }
            },
            [&](uint8_t* ram, uint8_t*, uint32_t) {
                for (uint32_t car = 0; car < kCarCount; car++) sim::UpdateSlipAngles(BodyAt(ram, BodyAddress(car)));
            },
            kCarCount);
        Report("SlipAngle", 0x80039DE8u, r.cases, r.mismatches, failures);
    }

    // ---- tyre wear and grip capacity; the wear constants are poked into the overlay's tuning block
    {
        const StatefulResult r = VerifyStateful(
            guest, pristine, 0x80039A4C, {kCarBase}, 400,
            [&](uint8_t* ram, uint8_t*, uint32_t, size_t variant) {
                sim::TyreWearConstants k; // zero = the dump's values (wear disabled)
                if (variant % 2 == 1) {
                    k.wearLimit = 1000 + int32_t(rng() % 2000000);
                    k.wearKnee = 1 + int32_t(rng() % uint32_t(k.wearLimit - 1));
                    k.coldLimit = -1 - int32_t(rng() % 1000000);
                    k.wornGripLoss = int32_t(rng() % 0x1800) - 0x400;
                    k.kneeGripLoss = int32_t(rng() % 0x1800) - 0x400;
                    k.coldGripLoss = int32_t(rng() % 0x1800) - 0x400;
                    k.pitGripFactor = int32_t(rng() % 0x1000);
                }
                const int32_t values[7] = {k.wearLimit, k.wornGripLoss, k.pitGripFactor, k.coldLimit, k.coldGripLoss, k.wearKnee, k.kneeGripLoss};
                for (uint32_t i = 0; i < 7; i++) WriteU32(ram, D(kWearConstants) + i * 4, uint32_t(values[i]));
                if (variant % 4 == 0) return;
                for (uint32_t car = 0; car < kCarCount; car++) {
                    sim::CarBody& b = BodyAt(ram, BodyAddress(car));
                    const bool extreme = variant % 4 == 3;
                    sim::SetField<uint8_t>(&b, 0x78D, uint8_t(rng() % 4 == 0 ? 0x10 : 0));
                    sim::SetField<int16_t>(&b, 0x646, int16_t(Signed(rng, extreme ? 0x8000u : 0x200u)));
                    sim::SetField<int16_t>(&b, 0x6F6, int16_t(Signed(rng, extreme ? 0x8000u : 0x200u)));
                    for (uint32_t axle = 0; axle < 2; axle++) sim::SetField<int16_t>(&b, 0x368 + axle * 2, int16_t(Signed(rng, extreme ? 0x8000u : 0x100u)));
                    for (uint32_t c = 0; c < 8; c++) sim::SetField<int16_t>(&b, 0x348 + c * 2, int16_t(extreme ? Signed(rng, 0x8000u) : int32_t(rng() % 0x200)));
                    for (uint32_t w = 0; w < 4; w++) {
                        sim::Wheel& wheel = b.wheels[w];
                        sim::SetField<int32_t>(&wheel, 0x08, extreme ? Signed(rng, 0x7FFFFFFFu) : int32_t(rng() % 60000));
                        sim::SetField<uint8_t>(&wheel, 0x14, uint8_t(rng() % 8));
                        sim::SetField<int16_t>(&wheel, 0x38, int16_t(rng() % 0x1001));
                        int32_t wear;
                        switch (rng() % 6) { // every wear stage, including beyond both limits
                        case 0: wear = k.coldLimit - 1 - int32_t(rng() % 1000); break;
                        case 1: wear = k.coldLimit + int32_t(rng() % uint32_t(-k.coldLimit + 1)); break;
                        case 2: wear = int32_t(rng() % uint32_t(k.wearKnee + 1)); break;
                        case 3: wear = k.wearKnee + int32_t(rng() % uint32_t(k.wearLimit - k.wearKnee + 1)); break;
                        case 4: wear = k.wearLimit + int32_t(rng() % 1000); break;
                        default: wear = Signed(rng, 0x7FFFFFFFu); break;
                        }
                        sim::SetField<int32_t>(&wheel, 0x64, wear);
                    }
                }
            },
            [&](uint8_t* ram, uint8_t*, uint32_t) {
                const sim::TyreWearConstants k = WearConstantsAt(ram);
                for (uint32_t car = 0; car < kCarCount; car++) {
                    sim::AxleTyreCurves curves[2];
                    ResolveAxleCurves(ram, BodyAddress(car), curves);
                    sim::UpdateTyreWearAndGrip(BodyAt(ram, BodyAddress(car)), curves, k);
                }
            },
            kCarCount);
        Report("TyreWear", 0x80039A4Cu, r.cases, r.mismatches, failures);
    }
    return failures;
}

} // namespace gt2::verify
