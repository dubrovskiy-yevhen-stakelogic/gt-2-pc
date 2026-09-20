// Differential checks of src/game/sim/drive_shafts.* (the wheel-rotation drivetrain pass 0x80046B58 and its
// callees, including the engine routines of drivetrain.h it calls) against the original (see guest.h for the harness).
#include <cstring>

#include "game/sim/drive_shafts.h"
#include "game/sim/drivetrain.h"
#include "guest.h"

namespace gt2::verify {

namespace {

// ---- random inputs: tier 1 realistic, 2 wide, 3 extreme (full 32-bit range with edge values) ----
struct Rng {
    std::mt19937& g;
    uint32_t Next() { return g(); }
    bool Chance(uint32_t oneIn) { return Next() % oneIn == 0; }
    int32_t Range(int32_t low, int32_t high) { return low + int32_t(Next() % uint32_t(int64_t(high) - int64_t(low) + 1)); }
    int32_t Spread(int tier, int32_t realistic, int32_t wide) {
        if (tier >= 3) {
            if (Chance(4)) {
                static constexpr int32_t kEdges[] = {0, 1, -1, 0x7FFFFFFF, int32_t(0x80000000), 0x1000, -0x1000, 0xFFFF, -0x10000};
                return kEdges[Next() % (sizeof(kEdges) / sizeof(kEdges[0]))];
            }
            return int32_t(Next());
        }
        const int32_t limit = tier == 1 ? realistic : wide;
        return Range(-limit, limit);
    }
    int32_t Positive(int tier, int32_t realistic, int32_t wide) {
        if (tier >= 3) return Chance(4) ? 0x7FFFFFFF : int32_t(Next() & 0x7FFFFFFF);
        return Range(0, tier == 1 ? realistic : wide);
    }
};

uint32_t BodyAddress(uint32_t car) { return kCarBase + car * kCarStride + kBodyOffset; }
uint8_t* At(uint8_t* ram, uint32_t address) { return ram + (address & 0x1FFFFF); }
sim::CarBody& BodyAt(uint8_t* ram, uint32_t address) { return *reinterpret_cast<sim::CarBody*>(At(ram, address)); }
sim::DriveStepWork& WorkAt(uint8_t* scratch) { return *reinterpret_cast<sim::DriveStepWork*>(scratch); }

template <typename T>
void Put(uint8_t* body, uint32_t offset, T value) { std::memcpy(body + offset, &value, sizeof(T)); }
void Put32(uint8_t* body, uint32_t offset, int32_t v) { Put<int32_t>(body, offset, v); }
void Put16(uint8_t* body, uint32_t offset, int32_t v) { Put<int16_t>(body, offset, int16_t(v)); }
void Put8(uint8_t* body, uint32_t offset, int32_t v) { Put<uint8_t>(body, offset, uint8_t(v)); }

// Engine block fields read by 0x8003533C / 0x800353DC / 0x80074F24 (offsets relative to the body).
void RandomiseEngine(Rng& rng, uint8_t* body, int tier) {
    Put16(body, 0x108, rng.Range(3000, 9000));                                   // rev limit rpm
    for (uint32_t i = 0; i < 2; i++) {
        Put16(body, 0x110 + i * 2, rng.Range(1, 8000));                          // turbo spool rpm (non-zero: divisor)
        Put16(body, 0x114 + i * 2, rng.Chance(2) ? 0 : rng.Range(0, 3000));      // turbo boost
        Put32(body, 0x118 + i * 4, rng.Positive(tier, 0x2000, 0x20000));         // spool rate
        Put32(body, 0x120 + i * 4, rng.Spread(tier, 20000, 200000));             // curve coefficient
        Put8(body, 0x12A + i, rng.Range(0, 3));                                  // curve model
        Put16(body, 0x620 + i * 2, tier >= 3 ? int32_t(rng.Next() & 0xFFFF) : rng.Range(0, 3000)); // spool state
    }
    Put16(body, 0x128, rng.Chance(2) ? 0 : rng.Range(0, 2000));                  // boost cap
    Put8(body, 0x61D, rng.Range(0, 1));                                          // rev limiter active
    Put16(body, 0x61E, rng.Range(-0x800, 0x1800));                               // engine load
    Put32(body, 0x624, rng.Chance(4) ? rng.Spread(tier, 2000000, 20000000) : rng.Positive(tier, 2000000, 20000000)); // engine speed
    Put16(body, 0x6AC, rng.Range(0, 9000));                                      // rpm
    Put16(body, 0x708, rng.Chance(4) ? 0 : rng.Chance(4) ? 0x1000 : rng.Range(0, 0x1000)); // throttle
    Put32(body, 0x710, rng.Positive(tier, 5000, 100000));                        // torque range
    Put32(body, 0x714, rng.Positive(tier, 5000, 100000));                        // friction torque
    Put16(body, 0x742, rng.Range(-0x2000, 0x2000));
    static constexpr int32_t kBlowOff[] = {0, -1, 1, 0x1000, 0x800, 0x2000};
    Put16(body, 0x744, rng.Chance(2) ? kBlowOff[rng.Next() % 6] : rng.Range(-0x2000, 0x2000));
    Put16(body, 0x746, rng.Range(-0x2000, 0x2000));
    Put16(body, 0x6FE, rng.Chance(2) ? 2184 : rng.Range(0, 0x2000));             // step time
}

// Drivetrain parameter block (body + 0x370) and the drivetrain state.
void RandomiseDrivetrain(Rng& rng, uint8_t* body, int tier) {
    Put8(body, 0x370, rng.Range(0, 6));                                          // drive type
    Put8(body, 0x372, rng.Range(0, 2));                                          // engine control mode
    Put8(body, 0x373, rng.Range(0, 1));                                          // primary axle
    Put16(body, 0x374, rng.Chance(3) ? 0x1000 : rng.Range(-0x1000, 0x2000));     // centre split
    Put16(body, 0x376, rng.Chance(3) ? 0 : rng.Range(0, 0x4000));                // centre lock torque
    Put16(body, 0x378, rng.Range(0, 200));                                       // engine inertia (referred)
    for (uint32_t axle = 0; axle < 2; axle++) {
        Put8(body, 0x37A + axle, rng.Range(0, 7));                               // axle diff type
        Put32(body, 0x37C + axle * 4, rng.Positive(tier, 20000, 2000000));       // diff minimum torque
        Put32(body, 0x384 + axle * 4, rng.Positive(tier, 0x2000, 0x20000));      // diff accel ratio
        Put32(body, 0x38C + axle * 4, rng.Positive(tier, 0x2000, 0x20000));      // diff decel ratio
        Put16(body, 0x3E4 + axle * 2, rng.Range(800, 2000));                     // wheel radius
        Put32(body, 0x3F8 + axle * 4, rng.Positive(tier, 20000, 200000));        // axle inverse inertia
        Put32(body, 0x400 + axle * 4, rng.Range(1, tier == 1 ? 5000 : 200000));  // axle inertia (non-zero: divisor)
        Put8(body, 0x61A + axle, rng.Range(0, 1));                               // axle diff locked
        Put32(body, 0x634 + axle * 4, rng.Chance(8) ? 0 : rng.Spread(tier, 500000, 5000000)); // axle speed
    }
    if (rng.Chance(4)) Put32(body, 0x638, *reinterpret_cast<int32_t*>(body + 0x634)); // equal axle speeds
    Put16(body, 0x396, rng.Range(1000, 9000));                                   // power rpm
    Put16(body, 0x398, rng.Range(500, 8000));                                    // launch rpm
    for (uint32_t gear = 0; gear < 8; gear++) Put32(body, 0x3A4 + gear * 4, rng.Chance(4) ? 0 : rng.Range(0, tier >= 2 ? 200000 : 70000)); // ratios
    Put16(body, 0x3F4, rng.Range(-0x4000, 0x4000));                              // locked clutch torque
    Put16(body, 0x3F6, rng.Range(-0x4000, 0x4000));                              // overrun clutch torque
    Put32(body, 0x408, rng.Positive(tier, 20000, 200000));                       // centre diff stiffness
    Put32(body, 0x40C, rng.Positive(tier, 4000000, 40000000));                   // engine inverse inertia
    Put16(body, 0x60A, rng.Chance(2) ? 0 : rng.Range(-0x1000, 0x1000));
    Put16(body, 0x60C, rng.Range(-200, 200));                                    // steer sign
    Put16(body, 0x612, rng.Chance(2) ? 0 : rng.Range(0, 0x1000));                // brake input
    Put8(body, 0x618, rng.Range(0, 7));                                          // gear
    Put8(body, 0x619, rng.Range(0, 3));                                          // clutch state
    Put8(body, 0x63C, rng.Range(0, 1));                                          // centre locked
    Put16(body, 0x64A, rng.Range(-5000, 5000));
    static constexpr int32_t kSpeeds[] = {0, 3413, 3414, 11379, 11380, 0x2C73, 0x2C74, 0x241E4, 0x241E5};
    Put32(body, 0x6A4, rng.Chance(4) ? kSpeeds[rng.Next() % 9] : rng.Spread(tier, 200000, 2000000)); // forward speed
    for (uint32_t wheel = 0; wheel < 4; wheel++) {
        uint8_t* w = body + 0x460 + wheel * 0x68;
        Put16(w, 0x16, tier == 1 ? rng.Range(-5000, 5000) : rng.Range(-0x8000, 0x7FFF)); // last acceleration
        Put32(w, 0x18, rng.Chance(8) ? 0 : rng.Spread(tier, 200000, 2000000));  // rim speed
        Put8(w, 0x63, int32_t(rng.Next() & 0xFF));                               // flags
    }
}

void RandomiseBody(Rng& rng, uint8_t* body, int tier) {
    RandomiseEngine(rng, body, tier);
    RandomiseDrivetrain(rng, body, tier);
}

// The car's work block in the scratchpad (there are no dump values for it: the harness zeroes the scratchpad).
void RandomiseWork(Rng& rng, sim::DriveCarWork& work, int tier) {
    for (sim::DriveWheelWork& wheel : work.wheels) {
        wheel.brakeTorque = rng.Chance(2) ? 0 : rng.Positive(tier, 200000, 2000000);
        wheel.roadForce[0] = rng.Spread(tier, 500000, 5000000);
        wheel.roadForce[1] = rng.Spread(tier, 500000, 5000000);
        wheel.torque = rng.Spread(tier, 2000000, 20000000);
        for (uint8_t& b : wheel.reserved10) b = uint8_t(rng.Next());
        wheel.steerCos = int16_t(rng.Next());
        wheel.steerSin = int16_t(rng.Next());
    }
    work.axleTorque[0] = rng.Spread(tier, 2000000, 20000000);
    work.axleTorque[1] = rng.Spread(tier, 2000000, 20000000);
    work.clutchInputSpeed = rng.Spread(tier, 500000, 5000000);
    work.clutchOutputSpeed = rng.Spread(tier, 500000, 5000000);
    if (work.clutchOutputSpeed == 0) work.clutchOutputSpeed = 1;
    if (rng.Chance(4)) work.clutchInputSpeed = work.clutchOutputSpeed;
    work.reserved80 = int32_t(rng.Next());
    work.reserved84 = int32_t(rng.Next());
    work.clutchEngagement = rng.Range(0, 0x1000);
    work.activeDiffFactor = int16_t(rng.Range(-0x1000, 0x1000));
    work.reserved8E = int16_t(rng.Next());
}

// Like VerifyStateful (guest.h) with three additions: `prepare` chooses the arguments, the return value is
// compared as well when `compareReturn` is set, and cases in which the original raised an exception (its 64-bit
// division traps on a zero divisor) are skipped and counted.
//   prepare(ram, scratch, variant, args[4]); native(ram, scratch, args) -> int32_t
template <typename Prepare, typename Native>
void RunChecked(Guest& guest, const std::vector<uint8_t>& pristine, const char* name, uint32_t function, size_t variants, Prepare prepare,
                Native native, bool compareReturn, int& failures) {
    std::vector<uint8_t> ours(Bus::kRamSize), ourScratch(kScratchSize);
    size_t cases = 0, mismatches = 0, skipped = 0;
    const uint32_t stackLow = (kStack & 0x1FFFFF) - 0x2000, stackHigh = (kStack & 0x1FFFFF) + 0x100;
    for (size_t variant = 0; variant < variants; variant++) {
        std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
        std::memset(guest.Scratch(), 0, kScratchSize);
        uint32_t args[4] = {0, 0, 0, 0};
        prepare(guest.Ram(), guest.Scratch(), variant, args);
        std::memcpy(ours.data(), guest.Ram(), Bus::kRamSize);
        std::memcpy(ourScratch.data(), guest.Scratch(), kScratchSize);
        int32_t original = 0;
        try {
            original = int32_t(guest.Call(function, args[0], args[1], args[2], args[3]));
        } catch (const std::runtime_error&) {
            skipped++;
            continue;
        }
        const int32_t result = native(ours.data(), ourScratch.data(), args);
        cases++;
        const bool ramEqual = std::memcmp(ours.data(), guest.Ram(), stackLow) == 0 &&
                              std::memcmp(ours.data() + stackHigh, guest.Ram() + stackHigh, Bus::kRamSize - stackHigh) == 0;
        const bool scratchEqual = std::memcmp(ourScratch.data(), guest.Scratch(), kScratchSize) == 0;
        const bool returnEqual = !compareReturn || original == result;
        if (ramEqual && scratchEqual && returnEqual) continue;
        if (mismatches++ >= 3) continue;
        if (!returnEqual) std::printf("    MISMATCH %s variant %zu: return original %d ours %d\n", name, variant, original, result);
        for (uint32_t i = 0; i < Bus::kRamSize && !ramEqual; i++)
            if ((i < stackLow || i >= stackHigh) && ours[i] != guest.Ram()[i]) {
                std::printf("    MISMATCH %s variant %zu: first differing byte at 0x%08X (a0 + 0x%X): original %02X ours %02X\n", name, variant,
                            0x80000000u + i, i - (args[0] & 0x1FFFFF), guest.Ram()[i], ours[i]);
                break;
            }
        for (uint32_t i = 0; i < kScratchSize && !scratchEqual; i++)
            if (ourScratch[i] != guest.Scratch()[i]) {
                std::printf("    MISMATCH %s variant %zu: first differing scratchpad byte at 0x1F800000 + 0x%X: original %02X ours %02X\n", name, variant, i,
                            guest.Scratch()[i], ourScratch[i]);
                break;
            }
    }
    std::printf("%-10s 0x%08X  %zu cases, %zu mismatches, %zu skipped (original trapped)  %s\n", name, function, cases, mismatches, skipped,
                mismatches ? "FAIL" : "ok");
    failures += mismatches ? 1 : 0;
}

// Pure functions of their register arguments.
template <typename Native>
void RunPure(Guest& guest, const char* name, uint32_t function, size_t cases, std::mt19937& rng, int32_t realistic, Native native, int& failures) {
    Rng r{rng};
    size_t bad = 0, done = 0;
    for (size_t i = 0; i < cases; i++) {
        const int tier = int(i % 4); // 0..3: small, realistic, wide, extreme
        const int32_t a = tier == 0 ? r.Range(-0x1000, 0x1000) : r.Spread(tier, realistic, 0x1000000);
        const int32_t b = tier == 0 ? r.Range(-0x1000, 0x1000) : r.Spread(tier, realistic, 0x1000000);
        done++;
        const int32_t original = int32_t(guest.Call(function, uint32_t(a), uint32_t(b)));
        const int32_t ours = native(a, b);
        if (original != ours && bad++ < 3) std::printf("    MISMATCH %s(%d, %d): original %d, ours %d\n", name, a, b, original, ours);
    }
    Report(name, function, done, bad, failures);
}

} // namespace

int VerifyDriveShafts(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& mt) {
    int failures = 0;
    Rng rng{mt};
    sim::DrivetrainGlobals globals;
    globals.raceModeByte = pristine[D(0x801D5866u) & 0x1FFFFF];

    // ---- angle helpers and the clutch slip factor: pure ----
    RunPure(guest, "WrapAngle", 0x800450A0u, 20000, mt, 0x100000, [](int32_t a, int32_t) { return sim::WrapAngle(a); }, failures);
    RunPure(guest, "AngleDiff", 0x800450E0u, 20000, mt, 0x100000, sim::AngleDifference, failures);
    RunPure(guest, "ClutchSlip", 0x800392D8u, 20000, mt, 0x100000, [](int32_t a, int32_t) { return sim::ClutchSlipFactor(a); }, failures);

    // Prepares one body (car = variant % 6) with the tier of the variant; tier 0 keeps the dump's values.
    auto prepareBody = [&](uint8_t* ram, size_t variant, uint32_t& bodyAddress) {
        const uint32_t car = uint32_t(variant % kCarCount);
        bodyAddress = BodyAddress(car);
        const int tier = int((variant / kCarCount) % 4);
        if (tier != 0) RandomiseBody(rng, At(ram, bodyAddress), tier);
    };

    // ---- turbo boost: pure in the engine block (a0 = body + 0x7C) and two spool speeds ----
    for (int pass = 0; pass < 2; pass++) {
        const bool capped = pass == 1;
        RunChecked(
            guest, pristine, capped ? "BoostMul" : "BoostSum", capped ? 0x80075074u : 0x80074F24u, 2400,
            [&](uint8_t* ram, uint8_t*, size_t variant, uint32_t* args) {
                uint32_t body;
                prepareBody(ram, variant, body);
                args[0] = body + 0x7C;
                args[1] = uint32_t(int32_t(int16_t(rng.Chance(3) ? rng.Range(0, 9000) : int32_t(rng.Next() & 0xFFFF))));
                args[2] = uint32_t(int32_t(int16_t(rng.Chance(3) ? rng.Range(0, 9000) : int32_t(rng.Next() & 0xFFFF))));
            },
            [&](uint8_t* ram, uint8_t*, const uint32_t* args) {
                const sim::CarBody& body = BodyAt(ram, args[0] - 0x7C);
                return capped ? sim::BoostMultiplier(body, int16_t(args[1]), int16_t(args[2])) : sim::TurboBoostSum(body, int16_t(args[1]), int16_t(args[2]));
            },
            true, failures);
    }

    // ---- engine torque (writes the rev limiter flag), engine step with turbo model ----
    RunChecked(
        guest, pristine, "BaseTorque", 0x8003533Cu, 2400,
        [&](uint8_t* ram, uint8_t*, size_t variant, uint32_t* args) {
            prepareBody(ram, variant, args[0]);
            args[1] = uint32_t(rng.Chance(4) ? rng.Range(-0x1000, 0x2000) : rng.Range(0, 0x1000));
        },
        [&](uint8_t* ram, uint8_t*, const uint32_t* args) { return sim::BaseEngineTorque(BodyAt(ram, args[0]), int32_t(args[1])); }, true, failures);

    RunChecked(
        guest, pristine, "EngineStep", 0x800353DCu, 4800,
        [&](uint8_t* ram, uint8_t* scratch, size_t variant, uint32_t* args) {
            prepareBody(ram, variant, args[0]);
            args[1] = uint32_t(int32_t(int16_t(rng.Chance(4) ? rng.Range(-0x1000, 0x2000) : rng.Range(0, 0x1000))));
            const int32_t stepTime = BodyAt(ram, args[0]).stepTime;
            std::memcpy(scratch, &stepTime, 4);
        },
        [&](uint8_t* ram, uint8_t* scratch, const uint32_t* args) {
            return sim::EngineTorqueStep(BodyAt(ram, args[0]), int32_t(args[1]), WorkAt(scratch).stepTime);
        },
        true, failures);

    RunChecked(
        guest, pristine, "Governed", 0x8004530Cu, 4800,
        [&](uint8_t* ram, uint8_t* scratch, size_t variant, uint32_t* args) {
            prepareBody(ram, variant, args[0]);
            const int32_t stepTime = BodyAt(ram, args[0]).stepTime;
            std::memcpy(scratch, &stepTime, 4);
        },
        [&](uint8_t* ram, uint8_t* scratch, const uint32_t* args) { return sim::GovernedEngineStep(BodyAt(ram, args[0]), WorkAt(scratch).stepTime); }, true,
        failures);

    RunChecked(
        guest, pristine, "ClutchSt", 0x80045138u, 4800,
        [&](uint8_t* ram, uint8_t* scratch, size_t variant, uint32_t* args) {
            prepareBody(ram, variant, args[0]);
            sim::CarBody& body = BodyAt(ram, args[0]);
            if (rng.Chance(2)) Put16(reinterpret_cast<uint8_t*>(&body), 0x618, 0x101); // the clutch-start case
            const int32_t stepTime = body.stepTime;
            std::memcpy(scratch, &stepTime, 4);
            for (uint32_t car = 0; car < kCarCount; car++) RandomiseWork(rng, WorkAt(scratch).cars[car], int(variant % 3) + 1);
        },
        [&](uint8_t* ram, uint8_t* scratch, const uint32_t* args) {
            return sim::ClutchStartEngineStep(BodyAt(ram, args[0]), WorkAt(scratch), WorkAt(scratch).stepTime);
        },
        true, failures);

    // ---- centre coupling fraction: pure in the body and the torques ----
    RunChecked(
        guest, pristine, "Coupling", 0x800459A8u, 4800,
        [&](uint8_t* ram, uint8_t*, size_t variant, uint32_t* args) {
            prepareBody(ram, variant, args[0]);
            const int tier = int(variant % 3) + 1;
            args[1] = uint32_t(rng.Chance(6) ? 0 : rng.Spread(tier, 2000000, 20000000));
            args[2] = uint32_t(rng.Spread(tier, 2000000, 20000000));
            args[3] = uint32_t(rng.Spread(tier, 2000000, 20000000));
        },
        [&](uint8_t* ram, uint8_t*, const uint32_t* args) { return sim::CentreCouplingFraction(BodyAt(ram, args[0]), int32_t(args[1]), int32_t(args[3])); },
        true, failures);

    // ---- the four passes and the whole step over the six cars of the dump: (cars, count) ----
    auto prepareCars = [&](uint8_t* ram, uint8_t* scratch, size_t variant, uint32_t* args) {
        args[0] = kCarBase;
        args[1] = kCarCount;
        const int tier = int(variant % 4); // 0: dump values in the bodies (the work blocks are random in every case)
        for (uint32_t car = 0; car < kCarCount; car++) {
            if (tier != 0) RandomiseBody(rng, At(ram, BodyAddress(car)), tier);
            RandomiseWork(rng, WorkAt(scratch).cars[car], tier == 0 ? 1 : tier);
        }
        int32_t stepTime = int32_t(rng.Next());
        std::memcpy(scratch, &stepTime, 4); // the routines overwrite it per car; a random value shows if one relies on it
    };
    auto bodiesOf = [&](uint8_t* ram, sim::CarBody** bodies) {
        for (uint32_t car = 0; car < kCarCount; car++) bodies[car] = &BodyAt(ram, BodyAddress(car));
    };

    RunChecked(
        guest, pristine, "WheelTorq", 0x80045688u, 800, prepareCars,
        [&](uint8_t* ram, uint8_t* scratch, const uint32_t*) {
            sim::CarBody* bodies[kMaxCars];
            bodiesOf(ram, bodies);
            for (size_t car = 0; car < kCarCount; car++) sim::ComputeWheelTorques(*bodies[car], WorkAt(scratch), car);
            return 0;
        },
        false, failures);

    RunChecked(
        guest, pristine, "DiffBias", 0x800457B0u, 800, prepareCars,
        [&](uint8_t* ram, uint8_t* scratch, const uint32_t*) {
            sim::CarBody* bodies[kMaxCars];
            bodiesOf(ram, bodies);
            for (size_t car = 0; car < kCarCount; car++) sim::ApplyActiveDifferentialBias(*bodies[car], WorkAt(scratch), car);
            return 0;
        },
        false, failures);

    RunChecked(
        guest, pristine, "Shafts", 0x80045AE8u, 1600, prepareCars,
        [&](uint8_t* ram, uint8_t* scratch, const uint32_t*) {
            sim::CarBody* bodies[kMaxCars];
            bodiesOf(ram, bodies);
            for (size_t car = 0; car < kCarCount; car++) sim::UpdateDriveShafts(*bodies[car], WorkAt(scratch), car, globals);
            return 0;
        },
        false, failures);

    RunChecked(
        guest, pristine, "AxleDiffs", 0x800465E0u, 1600, prepareCars,
        [&](uint8_t* ram, uint8_t* scratch, const uint32_t*) {
            sim::CarBody* bodies[kMaxCars];
            bodiesOf(ram, bodies);
            for (size_t car = 0; car < kCarCount; car++) sim::UpdateAxleDifferentials(*bodies[car], WorkAt(scratch), car);
            return 0;
        },
        false, failures);

    RunChecked(
        guest, pristine, "Drivetrain", 0x80046B58u, 1600, prepareCars,
        [&](uint8_t* ram, uint8_t* scratch, const uint32_t*) {
            sim::CarBody* bodies[kMaxCars];
            bodiesOf(ram, bodies);
            sim::UpdateDrivetrain(bodies, kCarCount, WorkAt(scratch), globals);
            return 0;
        },
        false, failures);

    return failures;
}

} // namespace gt2::verify
