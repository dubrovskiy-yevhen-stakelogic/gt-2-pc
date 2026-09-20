// Differential checks of src/game/sim/drivetrain.* against the original (see guest.h for the harness).
#include <cstring>
#include <string>

#include "game/sim/drivetrain.h"
#include "game/sim/field.h"
#include "guest.h"

namespace gt2::verify {

namespace {

sim::CarBody& BodyAt(uint8_t* ram, uint32_t object) { return *reinterpret_cast<sim::CarBody*>(ram + (object & 0x1FFFFF)); }
uint32_t CarIndexOf(uint32_t object) { return (object - kCarBase - kBodyOffset) / kCarStride; }

template <typename T>
T RamAt(const uint8_t* ram, uint32_t address) {
    T v;
    std::memcpy(&v, ram + (address & 0x1FFFFF), sizeof(T));
    return v;
}
template <typename T>
void RamPut(uint8_t* ram, uint32_t address, T v) { std::memcpy(ram + (address & 0x1FFFFF), &v, sizeof(T)); }

// Uniform integer in [low, high].
int32_t Between(std::mt19937& rng, int32_t low, int32_t high) { return low + int32_t(rng() % uint32_t(high - low + 1)); }
// Signed value of up to `limit` magnitude; `limit` picked from small / medium / large ranges by `variant`.
int32_t Signed(std::mt19937& rng, uint32_t limit) { return int32_t(rng() % limit) * ((rng() & 1) ? 1 : -1); }
uint32_t Magnitude(size_t variant) { return variant % 3 == 0 ? 0x1000u : variant % 3 == 1 ? 0x100000u : 0x7FFFFFFFu; }

struct CallArgs { uint32_t a1 = 0, a2 = 0, a3 = 0; };

// Description of the inputs of the case being prepared, printed with a mismatch (filled by `prepare`).
std::string g_caseDescription;

// Stateful comparison like VerifyStateful (guest.h), but `prepare` returns the arguments of the call, `native`
// returns a value that is compared with the original's v0 when `checkReturn` is set, and `native` may use
// the guest itself (the AI row runs the original's AI routine on our copy).
template <typename Prepare, typename Native>
StatefulResult RunStateful(Guest& guest, const std::vector<uint8_t>& pristine, uint32_t function, const std::vector<uint32_t>& objects,
                           size_t variants, bool checkReturn, Prepare prepare, Native native) {
    StatefulResult result;
    std::vector<uint8_t> ours(Bus::kRamSize), ourScratch(kScratchSize);
    for (uint32_t object : objects)
        for (size_t variant = 0; variant < variants; variant++) {
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            std::memset(guest.Scratch(), 0, kScratchSize);
            g_caseDescription.clear();
            const CallArgs args = prepare(guest.Ram(), guest.Scratch(), object, variant);
            std::memcpy(ours.data(), guest.Ram(), Bus::kRamSize);
            std::memcpy(ourScratch.data(), guest.Scratch(), kScratchSize);
            const uint32_t original = guest.Call(function, object, args.a1, args.a2, args.a3);
            const uint32_t returned = native(ours.data(), ourScratch.data(), object, args);
            result.cases++;
            const uint32_t stackLow = (kStack & 0x1FFFFF) - 0x2000, stackHigh = (kStack & 0x1FFFFF) + 0x100;
            const bool equal = (!checkReturn || original == returned) && std::memcmp(ours.data(), guest.Ram(), stackLow) == 0 &&
                               std::memcmp(ours.data() + stackHigh, guest.Ram() + stackHigh, Bus::kRamSize - stackHigh) == 0 &&
                               std::memcmp(ourScratch.data(), guest.Scratch(), kScratchSize) == 0;
            if (!equal && result.mismatches++ < 3) {
                if (!g_caseDescription.empty()) std::printf("    case: %s\n", g_caseDescription.c_str());
                if (checkReturn && original != returned)
                    std::printf("    MISMATCH object %08X variant %zu: return original %d ours %d\n", object, variant, int32_t(original), int32_t(returned));
                bool shown = false;
                for (uint32_t i = 0; i < Bus::kRamSize && !shown; i++)
                    if ((i < stackLow || i >= stackHigh) && ours[i] != guest.Ram()[i]) {
                        std::printf("    MISMATCH object %08X variant %zu: first differing byte at 0x%08X (object + 0x%X): original %02X ours %02X\n", object,
                                    variant, 0x80000000u + i, i - (object & 0x1FFFFF), guest.Ram()[i], ours[i]);
                        shown = true;
                    }
                for (uint32_t i = 0; i < kScratchSize && !shown; i++)
                    if (ourScratch[i] != guest.Scratch()[i]) {
                        std::printf("    MISMATCH object %08X variant %zu: first differing scratchpad byte at 0x1F800000 + 0x%X: original %02X ours %02X\n",
                                    object, variant, i, guest.Scratch()[i], ourScratch[i]);
                        shown = true;
                    }
            }
        }
    return result;
}

// Field helpers on a body inside a RAM image.
void Put8(sim::CarBody& b, uint32_t off, int32_t v) { sim::SetField(&b, off, uint8_t(v)); }
void Put16(sim::CarBody& b, uint32_t off, int32_t v) { sim::SetField(&b, off, int16_t(v)); }
void Put32(sim::CarBody& b, uint32_t off, int32_t v) { sim::SetField(&b, off, v); }
constexpr uint32_t Wheel(uint32_t wheel, uint32_t field) { return 0x460 + wheel * 0x68 + field; }
constexpr uint32_t Axle(uint32_t axle, uint32_t field) { return 0x194 + axle * 0xD8 + field; }

// A 16-bit table object { u16 count; s16* xs; s16* ys } in guest memory, resolved on a RAM image.
sim::CurveS16Ref CurveAt(uint8_t* ram, uint32_t object) {
    sim::CurveS16Ref curve;
    curve.count = RamAt<uint16_t>(ram, object);
    curve.xs = reinterpret_cast<const int16_t*>(ram + (RamAt<uint32_t>(ram, object + 4) & 0x1FFFFF));
    curve.ys = reinterpret_cast<const int16_t*>(ram + (RamAt<uint32_t>(ram, object + 8) & 0x1FFFFF));
    return curve;
}

// Randomises the gearbox data a gear selection reads: gear count, ratios (non-zero, the original divides by
// them), shift rpm tables.
void RandomGearbox(std::mt19937& rng, sim::CarBody& b) {
    Put8(b, 0x372, Between(rng, 1, 7));
    for (uint32_t g = 0; g < 8; g++) {
        Put32(b, 0x3A4 + g * 4, Between(rng, 1000, 80000));
        Put32(b, 0x3C4 + g * 4, rng() % 8 == 0 ? Signed(rng, 0x7FFFFFFF) | 1 : Between(rng, 1, 60000));
    }
    Put16(b, 0x108, Between(rng, 0, 12000));
    Put16(b, 0x10A, Between(rng, 0, 3000));
    Put16(b, 0x394, Between(rng, 0, 9000));
    Put16(b, 0x396, rng() % 8 == 0 ? Between(rng, 0, 0xFFFF) : Between(rng, 0, 12000));
    for (uint32_t g = 0; g < 6; g++) Put16(b, 0x398 + g * 2, Between(rng, 0, 12000));
}

// Randomises the wheel and axle fields the driver aids read (contact, grip, load, thresholds).
void RandomWheels(std::mt19937& rng, sim::CarBody& b, size_t variant) {
    for (uint32_t w = 0; w < 4; w++) {
        Put32(b, Wheel(w, 0x08), rng() % 4 == 0 ? 0 : Between(rng, 1, 40000));
        Put16(b, Wheel(w, 0x44), variant % 2 ? Between(rng, -0x1000, 0x1000) : Signed(rng, 0x8000));
        Put16(b, Wheel(w, 0x2A), Between(rng, 0, 0x1000));
        Put32(b, Wheel(w, 0x34), Signed(rng, variant % 3 == 2 ? 0x7FFFFFFFu : 0x20000u));
        Put16(b, Wheel(w, 0x60), rng() % 4 == 0 ? 0 : Between(rng, 0, 0x1000));
    }
    for (uint32_t a = 0; a < 2; a++) {
        Put16(b, Axle(a, 0x68), Signed(rng, 0x1000));
        Put16(b, Axle(a, 0x6A), Signed(rng, 0x1000));
    }
}

} // namespace

int VerifyDrivetrain(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng) {
    int failures = 0;
    std::vector<uint32_t> bodies;
    for (uint32_t car = 0; car < kCarCount; car++) bodies.push_back(kCarBase + car * kCarStride + kBodyOffset);
    constexpr uint32_t kFree = 0x801E8000u; // free guest RAM for records the original reads through a pointer

    // 0x8003D848 (16-bit curve lookup, sim::InterpolateS16) is verified by verify_tyres.cpp.

    // ---- 0x8003932C: engine speed from the driven wheels (pure)
    {
        StatefulResult r = RunStateful(
            guest, pristine, 0x8003932C, bodies, 400, true,
            [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) {
                sim::CarBody& b = BodyAt(ram, object);
                if (variant % 4 != 0) {
                    Put8(b, 0x370, Between(rng, 0, 8));
                    Put8(b, 0x618, Between(rng, 0, 7));
                    Put16(b, 0x374, Signed(rng, 0x2000));
                    Put32(b, 0x634, Signed(rng, Magnitude(variant)));
                    Put32(b, 0x638, Signed(rng, Magnitude(variant)));
                    for (uint32_t g = 0; g < 8; g++) Put32(b, 0x3A4 + g * 4, Signed(rng, variant % 5 == 4 ? 0x7FFFFFFFu : 0x20000u));
                }
                return CallArgs{};
            },
            [](uint8_t* ram, uint8_t*, uint32_t object, const CallArgs&) { return uint32_t(sim::EngineSpeedFromWheels(BodyAt(ram, object))); });
        Report("EngineSpd", 0x8003932Cu, r.cases, r.mismatches, failures);
    }

    // ---- 0x8003941C: rpm from the engine speed
    {
        StatefulResult r = RunStateful(
            guest, pristine, 0x8003941C, bodies, 300, false,
            [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) {
                sim::CarBody& b = BodyAt(ram, object);
                if (variant % 4 != 0) {
                    Put32(b, 0x624, Signed(rng, Magnitude(variant)));
                    Put16(b, 0x10A, Between(rng, 0, 0xFFFF));
                }
                return CallArgs{};
            },
            [](uint8_t* ram, uint8_t*, uint32_t object, const CallArgs&) { sim::UpdateEngineRpm(BodyAt(ram, object)); return 0u; });
        Report("EngineRpm", 0x8003941Cu, r.cases, r.mismatches, failures);
    }

    // 0x80074F24 / 0x80075074 / 0x8003533C / 0x800353DC (the engine: sim::TurboBoostSum, BoostMultiplier,
    // BaseEngineTorque, EngineTorqueStep) are verified by verify_drive_shafts.cpp.

    // ---- 0x800449C8: gear selection (return value + shift timer)
    constexpr uint32_t kRequest = 0x1F800364u;
    auto randomShiftState = [&](sim::CarBody& b, size_t variant) {
        RandomGearbox(rng, b);
        RandomWheels(rng, b, variant);
        Put16(b, 0x78E, rng() % 6 == 0 ? Between(rng, 1, 300) : 0);
        Put8(b, 0x786, rng() % 5 == 0 ? Between(rng, 0, 8) : 0);
        Put8(b, 0x642, Between(rng, 0, 2));
        Put8(b, 0x618, Between(rng, 0, 7));
        Put8(b, 0x619, Between(rng, 0, 3));
        Put8(b, 0x61C, rng() % 3 == 0 ? Between(rng, 0, 3) : 0);
        Put8(b, 0x370, Between(rng, 0, 7));
        Put16(b, 0x610, rng() % 3 == 0 ? 0 : Between(rng, 0, 0x1000));
        Put16(b, 0x708, rng() % 3 == 0 ? 0 : Between(rng, 0, 0x1000));
        Put16(b, 0x6AC, variant % 5 == 1 ? Signed(rng, 0x8000) : Between(rng, 0, 9000));
        b.forwardSpeed = variant % 3 == 0 ? Signed(rng, 0x2000) : Signed(rng, Magnitude(variant));
    };
    {
        StatefulResult r = RunStateful(
            guest, pristine, 0x800449C8, bodies, 800, true,
            [&](uint8_t* ram, uint8_t* scratch, uint32_t object, size_t variant) {
                sim::CarBody& b = BodyAt(ram, object);
                if (variant % 4 != 0) randomShiftState(b, variant);
                const uint32_t car = CarIndexOf(object);
                scratch[0x364 + car * 4] = uint8_t(rng() % 3 == 0);
                scratch[0x365 + car * 4] = uint8_t(int8_t(Between(rng, -1, 1)));
                return CallArgs{kRequest + car * 4, 0, 0};
            },
            [](uint8_t* ram, uint8_t* scratch, uint32_t object, const CallArgs&) {
                const auto* request = reinterpret_cast<const sim::GearRequest*>(scratch + 0x364 + CarIndexOf(object) * 4);
                return uint32_t(sim::SelectGear(BodyAt(ram, object), *request));
            });
        Report("SelectGear", 0x800449C8u, r.cases, r.mismatches, failures);
    }
    // ---- 0x8003991C: gear + clutch state update
    {
        StatefulResult r = RunStateful(
            guest, pristine, 0x8003991C, bodies, 400, false,
            [&](uint8_t* ram, uint8_t* scratch, uint32_t object, size_t variant) {
                sim::CarBody& b = BodyAt(ram, object);
                if (variant % 4 != 0) randomShiftState(b, variant);
                const uint32_t car = CarIndexOf(object);
                scratch[0x364 + car * 4] = uint8_t(rng() % 3 == 0);
                scratch[0x365 + car * 4] = uint8_t(int8_t(Between(rng, -1, 1)));
                return CallArgs{car, 0, 0};
            },
            [](uint8_t* ram, uint8_t* scratch, uint32_t object, const CallArgs& a) {
                const auto* request = reinterpret_cast<const sim::GearRequest*>(scratch + 0x364 + a.a1 * 4);
                sim::UpdateGear(BodyAt(ram, object), *request);
                return 0u;
            });
        Report("UpdateGear", 0x8003991Cu, r.cases, r.mismatches, failures);
    }

    // ---- 0x8003DE68: traction control
    {
        StatefulResult r = RunStateful(
            guest, pristine, 0x8003DE68, bodies, 600, false,
            [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) {
                sim::CarBody& b = BodyAt(ram, object);
                CallArgs args{uint32_t(Between(rng, 0, 0x4000)), uint32_t(Between(rng, 0, 0x1000)), uint32_t(Between(rng, 0, 0x1000))};
                if (variant % 4 != 0) {
                    RandomWheels(rng, b, variant);
                    Put8(b, 0x619, rng() % 3 == 0 ? Between(rng, 0, 3) : 1);
                    Put32(b, 0x710, rng() % 4 == 0 ? Signed(rng, 0x1000) : Between(rng, 1, 0x4000));
                    Put16(b, 0x708, rng() % 4 == 0 ? 0 : Between(rng, 0, 0x1000));
                    Put8(b, 0x370, Between(rng, 0, 7));
                    if (variant % 5 == 4) args = CallArgs{uint32_t(Signed(rng, 0x100000)), uint32_t(Signed(rng, 0x10000)), uint32_t(Signed(rng, 0x10000))};
                }
                return args;
            },
            [](uint8_t* ram, uint8_t*, uint32_t object, const CallArgs& a) {
                sim::ApplyTractionControl(BodyAt(ram, object), int32_t(a.a1), int32_t(a.a2), int32_t(a.a3));
                return 0u;
            });
        Report("Traction", 0x8003DE68u, r.cases, r.mismatches, failures);
    }

    // ---- 0x8003DBE8: brake assists (reads the slide measures from the scratchpad and the constant 0x80046EE8)
    {
        StatefulResult r = RunStateful(
            guest, pristine, 0x8003DBE8, bodies, 600, false,
            [&](uint8_t* ram, uint8_t* scratch, uint32_t object, size_t variant) {
                sim::CarBody& b = BodyAt(ram, object);
                CallArgs args{uint32_t(int32_t(int16_t(Between(rng, -0x100, 0x1000)))), uint32_t(int32_t(int16_t(Between(rng, -0x100, 0x1000)))), 0};
                const uint32_t car = sim::Field<uint8_t>(&b, 0x45C);
                if (variant % 4 != 0) {
                    RandomWheels(rng, b, variant);
                    b.yawRate = rng() % 5 == 0 ? 0 : Signed(rng, 0x100000);
                    Put32(b, 0x424, Between(rng, 0, 0x20000));
                    Put32(b, 0x428, Between(rng, 0, 0x20000));
                    Put16(b, 0x42C, Between(rng, 0, 0x1000));
                    Put16(b, 0x42E, Between(rng, 0, 0x1000));
                    RamPut<uint8_t>(ram, D(0x80046EE8u), uint8_t(rng() % 4 == 0 ? 0 : Between(rng, 0, 200)));
                    RamPut<int16_t>(scratch + 0x90 + car * 0x90, 0, int16_t(rng() % 4 == 0 ? 0 : Between(rng, -0x100, 0x1000)));
                    RamPut<int16_t>(scratch + 0x92 + car * 0x90, 0, int16_t(Between(rng, -0x100, 0x1000)));
                    if (variant % 7 == 6) {
                        args = CallArgs{uint32_t(Signed(rng, 0x10000)), uint32_t(Signed(rng, 0x10000)), 0};
                        RamPut<int16_t>(scratch + 0x90 + car * 0x90, 0, int16_t(Signed(rng, 0x8000)));
                        RamPut<int16_t>(scratch + 0x92 + car * 0x90, 0, int16_t(Signed(rng, 0x8000)));
                    }
                }
                return args;
            },
            [](uint8_t* ram, uint8_t* scratch, uint32_t object, const CallArgs& a) {
                sim::ApplyBrakeAssist(BodyAt(ram, object), scratch, int32_t(a.a1), int32_t(a.a2), RamAt<uint8_t>(ram, D(0x80046EE8u)));
                return 0u;
            });
        Report("BrakeAssist", 0x8003DBE8u, r.cases, r.mismatches, failures);
    }

    // ---- 0x8002FB18: player input (pad record in free RAM, gear request in the scratchpad, rates by car index)
    {
        constexpr uint32_t kPad = kFree;
        StatefulResult r = RunStateful(
            guest, pristine, 0x8002FB18, bodies, 1000, false,
            [&](uint8_t* ram, uint8_t* scratch, uint32_t object, size_t variant) {
                sim::CarBody& b = BodyAt(ram, object);
                const uint32_t car = CarIndexOf(object);
                sim::PadRecord pad{};
                pad.flags = uint16_t(rng() % 16);
                pad.steer = int16_t((pad.flags & 1) ? Between(rng, -0x1200, 0x1200) : Between(rng, -2, 2));
                pad.brake = uint16_t((pad.flags & 4) ? Between(rng, 0, 0x1000) : rng() % 2);
                pad.throttle = uint16_t((pad.flags & 2) ? Between(rng, 0, 0x1000) : rng() % 2);
                pad.shift = int8_t(Between(rng, -1, 1));
                pad.reverse = uint8_t(rng() % 4 == 0);
                pad.handbrake = uint8_t(rng() % 4 == 0);
                if (variant % 9 == 8) { // extremes
                    pad.steer = int16_t(Signed(rng, 0x8000));
                    pad.brake = uint16_t(rng() % 0x10000);
                    pad.throttle = uint16_t(rng() % 0x10000);
                }
                int32_t stepTime = b.stepTime;
                if (variant % 4 != 0) {
                    stepTime = Between(rng, 0, 0x4000);
                    b.forwardSpeed = variant % 3 == 0 ? Signed(rng, 0x2000) : variant % 3 == 1 ? Signed(rng, 0x40000) : Signed(rng, 0x7FFFFFFF);
                    Put16(b, Wheel(0, 0x50), Signed(rng, variant % 5 == 4 ? 0x8000u : 0x400u));
                    Put16(b, Wheel(1, 0x50), Signed(rng, variant % 5 == 4 ? 0x8000u : 0x400u));
                    Put16(b, 0x3A, variant % 5 == 4 ? Signed(rng, 0x8000) : Between(rng, 0, 0x800));
                    Put16(b, 0x60, variant % 5 == 4 ? Signed(rng, 0x8000) : Between(rng, 0, 0x800));
                    Put16(b, 0x62, variant % 5 == 4 ? Signed(rng, 0x8000) : Between(rng, 0, 0x2000));
                    Put16(b, 0x60C, Signed(rng, 0x800));
                    Put16(b, 0x60E, Signed(rng, 0x2000));
                    Put16(b, 0x610, Between(rng, 0, 0x1000));
                    Put16(b, 0x612, Between(rng, 0, 0x1000));
                    Put16(b, 0x614, Between(rng, 0, 0x1000));
                    Put16(b, 0x700, Signed(rng, 0x800));
                    Put16(b, 0x70A, Signed(rng, 0x800));
                    Put16(b, 0x70C, Signed(rng, 0x800));
                    Put16(b, 0x78E, rng() % 6 == 0 ? Between(rng, 1, 300) : 0);
                    Put8(b, 0x786, rng() % 5 == 0 ? Between(rng, 0, 8) : 0);
                    Put8(b, 0x642, Between(rng, 0, 2));
                    Put8(b, 0x372, rng() % 3 == 0 ? Between(rng, 1, 2) : Between(rng, 3, 7));
                    Put8(b, 0x373, Between(rng, 0, 2));
                    Put8(b, 0x718, rng() % 5 == 0);
                    Put8(b, 0x618, Between(rng, 0, 5));
                    Put8(b, 0x619, Between(rng, 0, 3));
                    Put16(b, 0x6AC, Between(rng, 0, 9000));
                    Put16(b, 0x10A, Between(rng, 0, 3000));
                    // Steering limit curve of the car: random samples on the dump's table object.
                    const uint32_t count = RamAt<uint16_t>(ram, object + 0x3C);
                    const uint32_t xs = RamAt<uint32_t>(ram, object + 0x40), ys = RamAt<uint32_t>(ram, object + 0x44);
                    int32_t x = Between(rng, -100, 100);
                    for (uint32_t i = 0; i < count; i++) {
                        x += Between(rng, 0, 4000);
                        RamPut<int16_t>(ram, xs + i * 2, int16_t(x > 0x7FFF ? 0x7FFF : x));
                        RamPut<int16_t>(ram, ys + i * 2, int16_t(variant % 5 == 4 ? Signed(rng, 0x8000) : Between(rng, 0, 0x800)));
                    }
                }
                {
                    char text[400];
                    std::snprintf(text, sizeof(text),
                                  "flags %u steer %d brake %u throttle %u shift %d reverse %u hb %u | gears %u gear %u speed %d thr %d clutchReq %d clutch %u 718 %u 78E %u 786 %u 373 %u brake %d hb %d step %d",
                                  pad.flags, pad.steer, pad.brake, pad.throttle, pad.shift, pad.reverse, pad.handbrake, sim::Field<uint8_t>(&b, 0x372),
                                  sim::Field<uint8_t>(&b, 0x618), b.forwardSpeed, sim::Field<int16_t>(&b, 0x610), sim::Field<int16_t>(&b, 0x60A),
                                  sim::Field<uint8_t>(&b, 0x619), sim::Field<uint8_t>(&b, 0x718), sim::Field<uint16_t>(&b, 0x78E), sim::Field<uint8_t>(&b, 0x786),
                                  sim::Field<uint8_t>(&b, 0x373), sim::Field<int16_t>(&b, 0x612), sim::Field<int16_t>(&b, 0x614), stepTime);
                    g_caseDescription = text;
                }
                std::memcpy(ram + (kPad & 0x1FFFFF), &pad, sizeof(pad));
                std::memcpy(scratch, &stepTime, 4);
                scratch[0x364 + car * 4] = uint8_t(rng() % 2);
                scratch[0x365 + car * 4] = uint8_t(int8_t(Between(rng, -1, 1)));
                return CallArgs{kPad, kRequest + car * 4, car};
            },
            [](uint8_t* ram, uint8_t* scratch, uint32_t object, const CallArgs& a) {
                sim::CarBody& b = BodyAt(ram, object);
                const uint32_t car = a.a3;
                sim::PadRecord pad;
                std::memcpy(&pad, ram + (a.a1 & 0x1FFFFF), sizeof(pad));
                auto* request = reinterpret_cast<sim::GearRequest*>(scratch + (a.a2 & 0x3FF));
                int32_t stepTime;
                std::memcpy(&stepTime, scratch, 4);
                sim::InputTuning tuning;
                tuning.steerSpringGain = RamAt<int32_t>(ram, D(0x80046F3Cu));
                tuning.steerDamping = RamAt<int32_t>(ram, D(0x80046F40u));
                tuning.steerCentring = RamAt<int32_t>(ram, D(0x80046F44u));
                tuning.steerCurve = CurveAt(ram, D(0x80046DA4u));
                tuning.throttleRise = RamAt<uint16_t>(ram, D(0x80046DB0u) + car * 2);
                tuning.throttleFall = RamAt<uint16_t>(ram, D(0x80046DB4u) + car * 2);
                tuning.brakeRise = RamAt<uint16_t>(ram, D(0x80046DB8u) + car * 2);
                tuning.brakeFall = RamAt<uint16_t>(ram, D(0x80046DBCu) + car * 2);
                tuning.handbrakeRise = RamAt<uint16_t>(ram, D(0x80046DC0u) + car * 2);
                tuning.handbrakeFall = RamAt<uint16_t>(ram, D(0x80046DC4u) + car * 2);
                sim::UpdatePlayerInput(b, pad, *request, stepTime, tuning, CurveAt(ram, object + 0x3C));
                return 0u;
            });
        Report("PlayerInput", 0x8002FB18u, r.cases, r.mismatches, failures);
    }

    // ---- 0x80038540: AI input. Our port prepares the body and reports which AI routine follows; the check
    // then runs that original routine (out of scope) on our copy through the guest, so that the whole effect
    // is compared.
    {
        const int32_t rate = RamAt<int32_t>(pristine.data(), D(0x801C8570u));
        std::vector<uint8_t> savedRam(Bus::kRamSize), savedScratch(kScratchSize);
        StatefulResult r = RunStateful(
            guest, pristine, 0x80038540, bodies, 300, false,
            [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) {
                sim::CarBody& b = BodyAt(ram, object);
                if (variant % 4 != 0) {
                    Put8(b, 0x372, rng() % 4 == 0 ? 1 : sim::Field<uint8_t>(&b, 0x372));
                    Put8(b, 0x718, rng() % 6 == 0);
                    Put16(b, 0x78E, rng() % 6 == 0 ? Between(rng, 1, 300) : 0);
                    Put8(b, 0x786, rng() % 4 == 0 ? 7 : sim::Field<uint8_t>(&b, 0x786));
                    Put8(b, 0x619, Between(rng, 0, 3));
                    Put16(b, 0x6AC, Between(rng, 0, 9000));
                    Put16(b, 0x396, Between(rng, 0, 12000));
                    Put16(b, 0x10A, Between(rng, 0, 3000));
                    Put16(b, 0x612, rng() % 2 ? 0 : Between(rng, 0, 0x1000));
                    Put16(b, 0x60A, rng() % 2 ? 0 : 0x1000);
                    b.forwardSpeed = Signed(rng, 0x4000);
                }
                return CallArgs{0, CarIndexOf(object), 0};
            },
            [&](uint8_t* ram, uint8_t* scratch, uint32_t object, const CallArgs& a) {
                sim::CarBody& b = BodyAt(ram, object);
                auto* request = reinterpret_cast<sim::GearRequest*>(scratch + 0x364 + a.a2 * 4);
                const sim::AiDispatch next = sim::PrepareAiInput(b, *request, RamAt<uint16_t>(ram, D(0x800A9520u)), rate);
                uint32_t function = 0;
                switch (next.control) {
                case sim::AiControl::Drive: function = 0x80037834u; break;
                case sim::AiControl::Finished: function = 0x800377E8u; break;
                case sim::AiControl::Scripted: function = 0x800372ECu; break;
                case sim::AiControl::None: break;
                }
                if (function == 0) return 0u;
                // Run the original's AI routine on our image: swap it into the guest and back.
                std::memcpy(savedRam.data(), guest.Ram(), Bus::kRamSize);
                std::memcpy(savedScratch.data(), guest.Scratch(), kScratchSize);
                std::memcpy(guest.Ram(), ram, Bus::kRamSize);
                std::memcpy(guest.Scratch(), scratch, kScratchSize);
                guest.Call(function, object, a.a2, uint32_t(next.argument));
                std::memcpy(ram, guest.Ram(), Bus::kRamSize);
                std::memcpy(scratch, guest.Scratch(), kScratchSize);
                std::memcpy(guest.Ram(), savedRam.data(), Bus::kRamSize);
                std::memcpy(guest.Scratch(), savedScratch.data(), kScratchSize);
                return 0u;
            });
        Report("AiInput*", 0x80038540u, r.cases, r.mismatches, failures); // * = the AI routines themselves run in the guest
    }
    return failures;
}

} // namespace gt2::verify
