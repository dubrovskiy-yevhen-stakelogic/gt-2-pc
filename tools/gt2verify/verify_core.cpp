// Differential checks of src/game/sim/physics_core.* against the original (see guest.h for the harness):
// the tyre-force routine 0x80039FC8, the physics core 0x8003E0C4, the step driver 0x80034480 and the small
// callees ported with them.
#include <cstdio>
#include <cstring>

#include "game/sim/physics_core.h"
#include "game/sim/trig.h"
#include <optional>

#include "guest.h"

namespace gt2::verify {

namespace {

// ---- access to the original's objects inside a RAM image ----
template <typename T>
T RamAt(const uint8_t* ram, uint32_t address) {
    T v;
    std::memcpy(&v, ram + (address & 0x1FFFFF), sizeof(T));
    return v;
}
template <typename T>
void RamPut(uint8_t* ram, uint32_t address, T v) { std::memcpy(ram + (address & 0x1FFFFF), &v, sizeof(T)); }

uint32_t BodyAddress(uint32_t car) { return kCarBase + car * kCarStride + kBodyOffset; }
uint8_t* At(uint8_t* ram, uint32_t address) { return ram + (address & 0x1FFFFF); }
sim::CarBody& BodyAt(uint8_t* ram, uint32_t body) { return *reinterpret_cast<sim::CarBody*>(At(ram, body)); }
sim::Car* CarsOf(uint8_t* ram) { return reinterpret_cast<sim::Car*>(At(ram, kCarBase)); }
constexpr uint32_t kContactState = 0x801C8608u;
sim::CarContactState& StateOf(uint8_t* ram) { return *reinterpret_cast<sim::CarContactState*>(At(ram, ContactBase())); }
sim::DriveStepWork& WorkOf(uint8_t* scratch) { return *reinterpret_cast<sim::DriveStepWork*>(scratch); }
constexpr uint32_t kGearRequests = 0x364, kDeltas = 0xB4; // scratchpad offsets

template <typename T>
void Put(uint8_t* object, uint32_t offset, T value) { std::memcpy(object + offset, &value, sizeof(T)); }
void Put32(uint8_t* o, uint32_t off, int32_t v) { Put<int32_t>(o, off, v); }
void Put16(uint8_t* o, uint32_t off, int32_t v) { Put<int16_t>(o, off, int16_t(v)); }
void Put8(uint8_t* o, uint32_t off, int32_t v) { Put<uint8_t>(o, off, uint8_t(v)); }
uint8_t* WheelOf(uint8_t* body, uint32_t wheel) { return body + 0x460 + wheel * 0x68; }

// ---- random inputs ----
struct Rng {
    std::mt19937& g;
    uint32_t Next() { return g(); }
    bool Chance(uint32_t oneIn) { return Next() % oneIn == 0; }
    int32_t Range(int32_t low, int32_t high) { return low + int32_t(Next() % uint32_t(int64_t(high) - int64_t(low) + 1)); }
    // tier 1: realistic magnitude, tier 2 and up: wide
    int32_t Spread(int tier, int32_t realistic, int32_t wide) { const int32_t limit = tier == 1 ? realistic : wide; return Range(-limit, limit); }
    int32_t Positive(int tier, int32_t realistic, int32_t wide) { return Range(0, tier == 1 ? realistic : wide); }
    int32_t Pick(std::initializer_list<int32_t> values) { return *(values.begin() + Next() % values.size()); }
};

// ---- the context of the original's globals, rebuilt on every image (curve objects hold guest pointers) ----
sim::CurveS16 CurveS16At(const uint8_t* ram, uint32_t table) {
    sim::CurveS16 c;
    c.count = RamAt<uint16_t>(ram, table);
    c.xs = reinterpret_cast<const int16_t*>(ram + (RamAt<uint32_t>(ram, table + 4) & 0x1FFFFF));
    c.ys = reinterpret_cast<const int16_t*>(ram + (RamAt<uint32_t>(ram, table + 8) & 0x1FFFFF));
    return c;
}
sim::CurveS16Ref CurveRefAt(const uint8_t* ram, uint32_t table) {
    const sim::CurveS16 c = CurveS16At(ram, table);
    return {c.xs, c.ys, c.count};
}
sim::CurveS32 CurveS32At(const uint8_t* ram, uint32_t table) {
    sim::CurveS32 c;
    c.count = RamAt<uint16_t>(ram, table);
    c.xs = reinterpret_cast<const int32_t*>(ram + (RamAt<uint32_t>(ram, table + 4) & 0x1FFFFF));
    c.ys = reinterpret_cast<const int32_t*>(ram + (RamAt<uint32_t>(ram, table + 8) & 0x1FFFFF));
    return c;
}
constexpr uint32_t kWearConstants = 0x80046F48u;
sim::TyreWearConstants WearConstantsAt(const uint8_t* ram) {
    sim::TyreWearConstants k;
    k.wearLimit = RamAt<int32_t>(ram, D(kWearConstants) + 0x00);
    k.wornGripLoss = RamAt<int32_t>(ram, D(kWearConstants) + 0x04);
    k.pitGripFactor = RamAt<int32_t>(ram, D(kWearConstants) + 0x08);
    k.coldLimit = RamAt<int32_t>(ram, D(kWearConstants) + 0x0C);
    k.coldGripLoss = RamAt<int32_t>(ram, D(kWearConstants) + 0x10);
    k.wearKnee = RamAt<int32_t>(ram, D(kWearConstants) + 0x14);
    k.kneeGripLoss = RamAt<int32_t>(ram, D(kWearConstants) + 0x18);
    return k;
}

struct Context {
    sim::CarCurves curves[kMaxCars];
    sim::InputTuning input[kMaxCars];
    sim::PhysicsContext physics;
};

// Everything except the shell's control class (a guest call) and the hooks.
void BuildContext(Context& c, uint8_t* ram, const Track* track, int shellClass) {
    for (uint32_t car = 0; car < kCarCount; car++) {
        const uint32_t body = BodyAddress(car);
        for (uint32_t axle = 0; axle < 2; axle++) {
            const uint32_t block = body + 0x194 + axle * 0xD8;
            sim::AxleTyreCurves& a = c.curves[car].axles[axle];
            a.slipAngleForce = CurveS16At(ram, block + 0x00);
            a.slipRatioForce = CurveS16At(ram, block + 0x2C);
            a.slipRatioGrip = CurveS16At(ram, block + 0x6C).ys;
            a.loadGrip = CurveS32At(ram, block + 0x90);
            a.camberGrip = CurveS16At(ram, block + 0xBC);
        }
        c.curves[car].engineTorque = CurveS32At(ram, body + 0x7C);
        c.curves[car].steerLimit = CurveRefAt(ram, body + 0x3C);
        sim::InputTuning& t = c.input[car];
        t.steerSpringGain = RamAt<int32_t>(ram, D(0x80046F3Cu));
        t.steerDamping = RamAt<int32_t>(ram, D(0x80046F40u));
        t.steerCentring = RamAt<int32_t>(ram, D(0x80046F44u));
        t.steerCurve = CurveRefAt(ram, D(0x80046DA4u));
        t.throttleRise = RamAt<uint16_t>(ram, D(0x80046DB0u) + car * 2);
        t.throttleFall = RamAt<uint16_t>(ram, D(0x80046DB4u) + car * 2);
        t.brakeRise = RamAt<uint16_t>(ram, D(0x80046DB8u) + car * 2);
        t.brakeFall = RamAt<uint16_t>(ram, D(0x80046DBCu) + car * 2);
        t.handbrakeRise = RamAt<uint16_t>(ram, D(0x80046DC0u) + car * 2);
        t.handbrakeFall = RamAt<uint16_t>(ram, D(0x80046DC4u) + car * 2);
    }
    sim::PhysicsContext& p = c.physics;
    p.move.track = track;
    p.move.globals.frameTime = RamAt<int32_t>(ram, D(0x801C856Cu));
    p.move.globals.rate = RamAt<int32_t>(ram, D(0x801C8570u));
    p.move.globals.draftDragFloor = RamAt<int32_t>(ram, D(0x80046EF4u));
    p.holdFrames = RamAt<uint16_t>(ram, D(0x800A9520u));
    p.move.collisionDisabled = p.holdFrames != 0;
    const uint32_t course = RamAt<uint8_t>(ram, D(0x800AF230u));
    p.move.dirtCourse = (RamAt<uint16_t>(ram, D(0x801E18E8u) + course * 0x18u + 8u) & 4) != 0;
    p.move.controlClass = shellClass;
    p.contact = &StateOf(ram);
    p.drivetrain.raceModeByte = RamAt<uint8_t>(ram, D(0x801D5866u));
    p.gameMode = p.drivetrain.raceModeByte;
    p.wear = WearConstantsAt(ram);
    p.curves = c.curves;
    p.input = c.input;
    p.rollingResistance = CurveS32At(ram, D(0x801C8730u));
    p.surfaceRolling = reinterpret_cast<const int32_t*>(At(ram, D(0x80046E00u)));
    for (uint32_t i = 0; i < sim::kAiAidEntries; i++) {
        const uint32_t entry = D(0x801C8690u) + i * 0x28u;
        p.aiAids[i].yawBrakeGain = RamAt<int16_t>(ram, entry + 0x18);
        p.aiAids[i].yawBrakeThreshold = RamAt<int16_t>(ram, entry + 0x1A);
        p.aiAids[i].steerFalloffGain = RamAt<int16_t>(ram, entry + 0x1C);
        p.aiAids[i].steerFalloffThreshold = RamAt<int16_t>(ram, entry + 0x1E);
        p.aiAids[i].tractionGain = RamAt<int32_t>(ram, entry + 0x20);
    }
    p.slideSensitivity = RamAt<uint8_t>(ram, D(0x80046EE8u));
}

// The hooks of the port run the original's routine (out of scope of the port) on our image through the guest,
// so that the whole effect of a step can still be compared: AI drivers (0x80037834 / 0x800377E8 / 0x800372EC)
// and the impact sound (0x800156B8).
struct GuestBridge {
    Guest* guest = nullptr;
    uint8_t* ram = nullptr;
    uint8_t* scratch = nullptr;
    std::vector<uint8_t> savedRam = std::vector<uint8_t>(Bus::kRamSize), savedScratch = std::vector<uint8_t>(kScratchSize);
    size_t aiCalls = 0, soundCalls = 0;
    bool nativeAi = false; // run the ported AI (ai_driver.*) on our image instead of the original through the guest
    void Call(uint32_t function, uint32_t a0, uint32_t a1, uint32_t a2) {
        std::memcpy(savedRam.data(), guest->Ram(), Bus::kRamSize);
        std::memcpy(savedScratch.data(), guest->Scratch(), kScratchSize);
        std::memcpy(guest->Ram(), ram, Bus::kRamSize);
        std::memcpy(guest->Scratch(), scratch, kScratchSize);
        guest->Call(function, a0, a1, a2);
        std::memcpy(ram, guest->Ram(), Bus::kRamSize);
        std::memcpy(scratch, guest->Scratch(), kScratchSize);
        std::memcpy(guest->Ram(), savedRam.data(), Bus::kRamSize);
        std::memcpy(guest->Scratch(), savedScratch.data(), kScratchSize);
    }
};
void AiHook(void* user, sim::CarBody& body, int car, const sim::AiDispatch& dispatch) {
    GuestBridge& bridge = *static_cast<GuestBridge*>(user);
    if (bridge.nativeAi) {
        if (dispatch.control == sim::AiControl::None) return;
        bridge.aiCalls++;
        const sim::AiContext context = AiContextFromRam(bridge.ram);
        sim::RunAiDriver(context, body, *reinterpret_cast<sim::GearRequest*>(bridge.scratch + kGearRequests + uint32_t(car) * 4), dispatch,
                         RamAt<int32_t>(bridge.scratch, 0));
        return;
    }
    uint32_t function = 0;
    switch (dispatch.control) {
    case sim::AiControl::Drive: function = 0x80037834u; break;
    case sim::AiControl::Finished: function = 0x800377E8u; break;
    case sim::AiControl::Scripted: function = 0x800372ECu; break;
    case sim::AiControl::None: return;
    }
    bridge.aiCalls++;
    bridge.Call(function, BodyAddress(uint32_t(car)), uint32_t(car), uint32_t(dispatch.argument));
}
void SoundHook(void* user, int car, int event) {
    GuestBridge& bridge = *static_cast<GuestBridge*>(user);
    bridge.soundCalls++;
    bridge.Call(0x800156B8u, uint32_t(car), uint32_t(event), 0);
}

// ---- stateful comparison with per-case arguments (like VerifyStateful in guest.h) ----
struct CallArgs { uint32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0; };
size_t g_skipped = 0; // cases of the current row in which the original trapped

template <typename Prepare, typename Native>
StatefulResult RunStateful(Guest& guest, const std::vector<uint8_t>& pristine, uint32_t function, size_t variants, Prepare prepare, Native native,
                           ScratchIgnore ignoreScratch = {}) {
    StatefulResult result;
    std::vector<uint8_t> ours(Bus::kRamSize), ourScratch(kScratchSize);
    const uint32_t stackLow = (kStack & 0x1FFFFF) - 0x2000, stackHigh = (kStack & 0x1FFFFF) + 0x100;
    for (size_t variant = 0; variant < variants; variant++) {
        std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
        std::memset(guest.Scratch(), 0, kScratchSize);
        CallArgs args;
        {
            SimContactLayout layout(guest.Ram()); // the native view of the contact state (guest.h)
            args = prepare(guest.Ram(), guest.Scratch(), variant);
        }
        std::memcpy(ours.data(), guest.Ram(), Bus::kRamSize);
        std::memcpy(ourScratch.data(), guest.Scratch(), kScratchSize);
        try {
            guest.Call(function, args.a0, args.a1, args.a2, args.a3);
        } catch (const std::runtime_error&) { // the original trapped (its 64-bit division breaks on a zero divisor)
            g_skipped++;
            continue;
        }
        {
            SimContactLayout layout(ours.data());
            native(ours.data(), ourScratch.data(), args);
        }
        result.cases++;
        if (ignoreScratch.end > ignoreScratch.begin)
            std::memcpy(ourScratch.data() + ignoreScratch.begin, guest.Scratch() + ignoreScratch.begin, ignoreScratch.end - ignoreScratch.begin);
        const bool equal = std::memcmp(ours.data(), guest.Ram(), stackLow) == 0 &&
                           std::memcmp(ours.data() + stackHigh, guest.Ram() + stackHigh, Bus::kRamSize - stackHigh) == 0 &&
                           std::memcmp(ourScratch.data(), guest.Scratch(), kScratchSize) == 0;
        if (!equal && result.mismatches++ < 3) { // show the first few differing bytes (RAM, then the scratchpad)
            int shown = 0;
            for (uint32_t i = 0; i < Bus::kRamSize && shown < 6; i++)
                if ((i < stackLow || i >= stackHigh) && ours[i] != guest.Ram()[i]) {
                    const uint32_t address = 0x80000000u + i;
                    const bool inCars = address >= kCarBase && address < kCarBase + kCarCount * kCarStride;
                    std::printf("    MISMATCH variant %zu: %s byte at 0x%08X", variant, shown ? "     also" : "first differing", address);
                    if (inCars) std::printf(" (car %u body + 0x%X)", (address - kCarBase) / kCarStride, (address - kCarBase) % kCarStride - kBodyOffset);
                    std::printf(": original %02X ours %02X\n", guest.Ram()[i], ours[i]);
                    shown++;
                }
            for (uint32_t i = 0; i < kScratchSize && shown < 6; i++)
                if (ourScratch[i] != guest.Scratch()[i]) {
                    std::printf("    MISMATCH variant %zu: %s scratchpad byte at 0x1F800000 + 0x%X: original %02X ours %02X\n", variant, shown ? "     also" : "first differing", i,
                                guest.Scratch()[i], ourScratch[i]);
                    shown++;
                }
        }
    }
    return result;
}

// ---- randomisation of the step state ----
// Dynamic state of one body (parameters of the car stay as in the dump). tier 0 keeps the dump's values.
void RandomiseDynamics(Rng& rng, uint8_t* body, int tier) {
    if (tier == 0) return;
    static constexpr int32_t kSpeedEdges[] = {0, 1, -1, 1137, 1138, -1137, -1138, 11380, 11381, -11380, -11381, 0x1BC88, 0x1BC89, 5689, 5690, -5689};
    auto speed = [&](int32_t realistic, int32_t wide) { return rng.Chance(6) ? kSpeedEdges[rng.Next() % 16] : rng.Spread(tier, realistic, wide); };
    for (uint32_t k = 0; k < 3; k++) Put32(body, 0x628 + k * 4, speed(0x20000, 0x100000));
    Put32(body, 0x6A4, speed(0x20000, 0x100000));                       // forward speed
    Put32(body, 0x6A8, speed(0x8000, 0x40000));                         // lateral speed
    Put32(body, 0x64C, rng.Chance(5) ? 0 : rng.Spread(tier, 0x20000, 0x90000)); // yaw rate
    Put32(body, 0x624, rng.Positive(tier, 0x100000, 0x400000));         // engine speed
    Put16(body, 0x6AC, rng.Range(0, 9000));                             // engine rpm
    Put8(body, 0x618, rng.Range(0, 6));                                 // gear
    Put8(body, 0x619, rng.Range(0, 3));                                 // clutch state
    Put8(body, 0x372, rng.Pick({1, 2, 3, 4, 4, 5, 6}));                 // engine / gearbox mode
    Put16(body, 0x60A, rng.Chance(2) ? 0 : 0x1000);                     // clutch request
    Put16(body, 0x60C, rng.Range(-0x800, 0x800));                       // steering angle
    Put16(body, 0x60E, rng.Range(-0x2000, 0x2000));                     // steering rate
    Put16(body, 0x610, rng.Chance(3) ? 0 : rng.Range(0, 0x1000));       // pedals
    Put16(body, 0x612, rng.Chance(3) ? 0 : rng.Pick({0x1000, rng.Range(0, 0x1000)}));
    Put16(body, 0x614, rng.Chance(2) ? 0 : rng.Range(0, 0x1000));
    Put16(body, 0x708, rng.Chance(3) ? 0 : rng.Pick({0x1000, 0x400, 0x401, rng.Range(0, 0x1000)})); // effective throttle
    Put8(body, 0x642, rng.Range(0, 2));                                 // transmission mode
    Put16(body, 0x700, rng.Range(-0x800, 0x800));                       // steering assist blend
    Put16(body, 0x70A, rng.Range(-0x800, 0));
    Put16(body, 0x70C, rng.Range(0, 0x800));
    Put16(body, 0x64A, rng.Range(-0x2000, 0x2000));                     // yaw acceleration
    for (uint32_t off : {0x724u, 0x728u, 0x72Cu, 0x730u, 0x734u}) Put32(body, off, rng.Spread(tier, 0x100000, 0x1000000));
    for (uint32_t off : {0x778u, 0x77Cu}) Put32(body, off, rng.Chance(2) ? 0 : rng.Spread(tier, 0x100000, 0x1000000)); // contact push
    Put8(body, 0x6B1, rng.Chance(8));                                   // physics hold
    Put8(body, 0x718, rng.Chance(10));                                  // scripted control
    Put16(body, 0x78E, rng.Chance(8) ? rng.Range(1, 300) : 0);          // penalty frames
    Put8(body, 0x786, rng.Chance(6) ? 7 : rng.Chance(4) ? rng.Range(1, 6) : 0); // race state
    Put8(body, 0x78D, (rng.Chance(4) ? 0x10 : 0) | (rng.Chance(5) ? 0x4 : 0) | (rng.Chance(8) ? 0x8 : 0));
    Put8(body, 0x1D, rng.Range(0, 3));                                  // AI aid index
    Put16(body, 0x766, rng.Chance(3) ? rng.Range(0x800, 0x1800) : 0x1000); // time scale
    Put16(body, 0x6FE, rng.Chance(2) ? 2184 : rng.Range(0, 0x2000));    // step time
    Put16(body, 0x73E, rng.Range(0, 40));                               // impact timer
    Put8(body, 0x765, rng.Chance(3) ? 0xFF : rng.Range(0, 10));
    Put8(body, 0x6FA, rng.Chance(2));                                   // impact sound raised
    Put16(body, 0x774, rng.Chance(2) ? 0 : rng.Pick({0x1000, rng.Range(0, 0x1000)})); // draft input
    Put16(body, 0x776, rng.Range(0, 0x1000));                           // draft blend
    Put16(body, 0x76A, rng.Chance(2) ? 0 : rng.Pick({0x400, 0x800, 0x1000}));
    for (uint32_t i = 0; i < 8; i++) Put8(body, 0x76C + i, rng.Range(0, 2));
    Put32(body, 0x634, rng.Chance(8) ? 0 : rng.Spread(tier, 200000, 2000000)); // axle speeds
    Put32(body, 0x638, rng.Chance(4) ? RamAt<int32_t>(body, 0x634) : rng.Spread(tier, 200000, 2000000));
    Put8(body, 0x61A, rng.Range(0, 1));
    Put8(body, 0x61B, rng.Range(0, 1));
    Put8(body, 0x61C, rng.Range(0, 20));                                // shift timer
    Put8(body, 0x61D, rng.Range(0, 1));                                 // rev limiter
    Put16(body, 0x620, rng.Range(0, 3000));
    Put16(body, 0x622, rng.Range(0, 3000));
    Put16(body, 0x646, rng.Range(-0x200, 0x200));                       // roll angle
    Put16(body, 0x740, rng.Chance(2) ? 0 : rng.Range(0, 0x1200));       // wall impact
    Put16(body, 0x640, rng.Range(0, 0x3000));                           // wall scrape
    for (uint32_t w = 0; w < 4; w++) {
        uint8_t* wheel = WheelOf(body, w);
        Put32(wheel, 0x08, rng.Chance(5) ? 0 : rng.Positive(tier, 60000, 400000)); // load
        Put16(wheel, 0x0C, rng.Range(-0x300, 0x300));                              // steer angle
        Put8(wheel, 0x14, rng.Range(0, 7));                                        // surface
        Put32(wheel, 0x18, rng.Chance(3) ? RamAt<int32_t>(body, 0x6A4) + rng.Range(-1200, 1200) : speed(0x20000, 0x100000)); // rim speed
        Put8(wheel, 0x22, rng.Chance(3) ? rng.Range(0, 255) : 0);                  // damage
        Put16(wheel, 0x2A, rng.Chance(2) ? 0x1000 : rng.Range(0, 0x1400));         // slip scale
        Put32(wheel, 0x2C, rng.Chance(3) ? rng.Range(-3000, 3000) : speed(0x20000, 0x100000)); // contact patch velocity
        Put32(wheel, 0x30, rng.Chance(3) ? rng.Range(-3000, 3000) : speed(0x8000, 0x40000));
        Put32(wheel, 0x34, rng.Positive(tier, 60000, 400000));                     // grip force
        Put16(wheel, 0x38, rng.Range(0, 0x1000));                                  // wear grip factor
        Put16(wheel, 0x3A, rng.Range(0, 0x1000));
        Put16(wheel, 0x3C, rng.Range(0, 0x1000));
        Put8(wheel, 0x3E, rng.Range(0, 255));
        Put8(wheel, 0x3F, rng.Range(-128, 127));
        Put16(wheel, 0x44, rng.Range(-0x1000, 0x1000));                            // slip ratio
        Put16(wheel, 0x46, rng.Range(-0x100, 0x1000));                             // slip-ratio grip
        Put16(wheel, 0x50, rng.Range(-0x400, 0x400));                              // slip angle
        Put16(wheel, 0x52, rng.Chance(4) ? 0 : rng.Range(0, 0x1000));              // slip blend
        for (uint32_t off : {0x54u, 0x58u, 0x5Cu}) Put32(wheel, off, rng.Spread(tier, 0x100000, 0x1000000));
        Put16(wheel, 0x60, rng.Chance(2) ? 0 : rng.Range(0, 0x1000));              // brake input
        Put8(wheel, 0x63, rng.Range(0, 15));
        Put32(wheel, 0x64, rng.Chance(2) ? 0 : rng.Spread(tier, 100000, 3000000)); // wear
    }
}

// The car's scratchpad work block: what the tick leaves for the tyre code, and don't-care fields random.
void RandomiseBlock(Rng& rng, uint8_t* body, sim::DriveCarWork& block, int tier, bool consistentAngles) {
    for (uint32_t w = 0; w < 4; w++) {
        sim::DriveWheelWork& wheel = block.wheels[w];
        wheel.brakeTorque = rng.Chance(2) ? 0 : rng.Positive(tier, 200000, 2000000);
        wheel.roadForce[0] = rng.Spread(tier, 500000, 5000000);
        wheel.roadForce[1] = rng.Spread(tier, 500000, 5000000);
        wheel.torque = rng.Spread(tier, 2000000, 20000000);
        wheel.reserved10[0] = uint8_t(rng.Range(-1, 1));                // slip mode
        wheel.reserved10[1] = uint8_t(rng.Next());
        wheel.reserved10[2] = uint8_t(rng.Range(-1, 1));                // slip sign
        wheel.reserved10[3] = uint8_t(rng.Next());
        Put16(wheel.reserved10, 4, rng.Range(-0x1000, 0x1000));         // slip ratio
        Put16(wheel.reserved10, 6, rng.Range(-0x1000, 0x1000));         // slip force
        // The tick writes sin of the wheel's steer angle at +0x18 and cos at +0x1A (WheelScratch in tyres.h;
        // DriveWheelWork names the pair the other way round).
        const int32_t steer = RamAt<int16_t>(WheelOf(body, w), 0x0C);
        Put16(reinterpret_cast<uint8_t*>(&wheel), 0x18, consistentAngles ? sim::Sin(uint32_t(steer)) : rng.Range(-0x1000, 0x1000));
        Put16(reinterpret_cast<uint8_t*>(&wheel), 0x1A, consistentAngles ? sim::Cos(uint32_t(steer)) : rng.Range(-0x1000, 0x1000));
    }
    block.axleTorque[0] = rng.Spread(tier, 2000000, 20000000);
    block.axleTorque[1] = rng.Spread(tier, 2000000, 20000000);
    block.clutchInputSpeed = rng.Spread(tier, 500000, 5000000);
    block.clutchOutputSpeed = rng.Spread(tier, 500000, 5000000);
    if (block.clutchOutputSpeed == 0) block.clutchOutputSpeed = 1; // a divisor of the drivetrain pass
    block.reserved80 = int32_t(rng.Next());
    block.reserved84 = int32_t(rng.Next());
    block.clutchEngagement = rng.Range(0, 0x1000);
    block.activeDiffFactor = int16_t(rng.Range(-0x1000, 0x1000));
    block.reserved8E = int16_t(rng.Range(0, 0x1000));
}

void RandomiseWearConstants(Rng& rng, uint8_t* ram, bool enabled) {
    sim::TyreWearConstants k;
    if (enabled) {
        k.wearLimit = 1000 + rng.Range(0, 2000000);
        k.wearKnee = 1 + rng.Range(0, k.wearLimit - 2);
        k.coldLimit = -1 - rng.Range(0, 1000000);
        k.wornGripLoss = rng.Range(-0x400, 0x1400);
        k.kneeGripLoss = rng.Range(-0x400, 0x1400);
        k.coldGripLoss = rng.Range(-0x400, 0x1400);
        k.pitGripFactor = rng.Range(0, 0x1000);
    }
    const int32_t values[7] = {k.wearLimit, k.wornGripLoss, k.pitGripFactor, k.coldLimit, k.coldGripLoss, k.wearKnee, k.kneeGripLoss};
    for (uint32_t i = 0; i < 7; i++) RamPut<int32_t>(ram, D(kWearConstants) + i * 4, values[i]);
}

// Puts the cars close to each other with random headings (positions near the dump's, so the course code works).
void ScatterCars(Rng& rng, uint8_t* ram, int tier) {
    sim::Car* cars = CarsOf(ram);
    if (tier == 0) return;
    const int32_t anchorX = cars[0].body.position[0], anchorY = cars[0].body.position[1], anchorZ = cars[0].body.position[2];
    const int32_t spread = tier == 1 ? 0x6000 : tier == 2 ? 0x10000 : 0x40000; // 6 m, 16 m, 64 m
    for (uint32_t car = 0; car < kCarCount; car++) {
        sim::CarBody& b = cars[car].body;
        b.position[0] = anchorX + rng.Range(-spread, spread);
        b.position[1] = anchorY + rng.Range(-spread, spread);
        b.position[2] = anchorZ + rng.Range(-0x2800, 0x2800);
        b.heading = int16_t(rng.Range(-0xFFF, 0xFFF));
        b.contactType = rng.Chance(8);
        b.aiLine = rng.Chance(8) ? 4 : uint8_t(rng.Range(0, 3));
        for (int set = 0; set < 2; set++) {
            b.footprintSet = uint8_t(set);
            sim::UpdateFootprint(b);
        }
        b.footprintSet = uint8_t(rng.Next() & 1);
        // The course-plane basis of the car follows the heading (it is what the ground code derives from it).
        const int32_t s = sim::Sin(uint32_t(uint16_t(b.heading))), c = sim::Cos(uint32_t(uint16_t(b.heading)));
        uint8_t* raw = reinterpret_cast<uint8_t*>(&b);
        Put16(raw, 0x668, -s); Put16(raw, 0x66A, c); Put16(raw, 0x66C, rng.Range(-0x200, 0x200));
        Put16(raw, 0x670, c); Put16(raw, 0x672, s); Put16(raw, 0x674, rng.Range(-0x200, 0x200));
    }
}

void RandomiseContactTables(Rng& rng, uint8_t* ram, int32_t xRange, int32_t yRange) {
    sim::CarContactState& state = StateOf(ram);
    state.buffer = int32_t(rng.Next() & 1);
    for (auto& perCar : state.corners)
        for (auto& perOther : perCar)
            for (sim::ContactCorner& corner : perOther)
                for (int buffer = 0; buffer < 2; buffer++) {
                    corner.edgeFlags[buffer] = uint8_t(rng.Chance(5) ? 0xF : rng.Range(0, 15));
                    corner.x[buffer] = rng.Range(-xRange, xRange);
                    corner.y[buffer] = rng.Range(-yRange, yRange);
                }
}

// Modes of the input routine of the physics core (body + 0x45D, car + 0x18): what the port can reproduce.
enum class InputMode { NoInput, ZeroPad, Dump, Mixed };

void SetInputMode(Rng& rng, uint8_t* ram, InputMode mode) {
    for (uint32_t car = 0; car < kCarCount; car++) {
        uint8_t* body = At(ram, BodyAddress(car));
        uint8_t* carObject = At(ram, kCarBase + car * kCarStride);
        switch (mode) {
        case InputMode::NoInput: Put8(body, 0x45D, rng.Chance(2) ? 1 : 3); Put16(carObject, 0x18, 0); break; // 0x8003C250 runs for every car
        case InputMode::ZeroPad: Put8(body, 0x45D, 0); Put16(carObject, 0x18, 0); break;
        case InputMode::Dump: Put16(carObject, 0x18, 0); break; // the replay stream of car 0 is out of scope
        case InputMode::Mixed: Put8(body, 0x45D, rng.Pick({0, 1, 3})); Put16(carObject, 0x18, 0); break;
        }
    }
}

} // namespace

int VerifyCore(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& mt, const Track* track) {
    int failures = 0;
    Rng rng{mt};
    std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
    const int shellClass = int(guest.Call(0x800418E8u)); // the shell's control class: game-mode globals only
    std::printf("           (physics core: shell control class %d, game mode %u, hold %u)\n", shellClass, RamAt<uint8_t>(pristine.data(), D(0x801D5866u)),
                RamAt<uint16_t>(pristine.data(), D(0x800A9520u)));
    GuestBridge bridge;
    bridge.guest = &guest;
    Context context;
    auto contextFor = [&](uint8_t* ram, uint8_t* scratch) -> const sim::PhysicsContext& {
        BuildContext(context, ram, track, shellClass);
        bridge.ram = ram;
        bridge.scratch = scratch;
        context.physics.user = &bridge;
        context.physics.aiInput = AiHook;
        context.physics.soundEvent = SoundHook;
        return context.physics;
    };

    // ---- 0x8003360C: clears the neighbour fields of a body
    {
        const StatefulResult r = RunStateful(
            guest, pristine, 0x8003360Cu, 60,
            [&](uint8_t* ram, uint8_t*, size_t variant) {
                const uint32_t body = BodyAddress(uint32_t(variant % kCarCount));
                for (uint32_t off = 0x768; off < 0x778; off++) Put8(At(ram, body), off, int32_t(rng.Next() & 0xFF));
                return CallArgs{body};
            },
            [](uint8_t* ram, uint8_t*, const CallArgs& a) { sim::ClearNeighbourFields(BodyAt(ram, a.a0)); });
        Report("ClearNbr", 0x8003360Cu, r.cases, r.mismatches, failures);
    }
    // ---- 0x80039470 / 0x80039994 / 0x800399C4: single-gear helpers (body, car)
    {
        const StatefulResult r = RunStateful(
            guest, pristine, 0x80039470u, 120,
            [&](uint8_t* ram, uint8_t*, size_t variant) {
                const uint32_t car = uint32_t(variant % kCarCount);
                Put16(At(ram, BodyAddress(car)), 0x60A, rng.Pick({0, 0x1000, rng.Range(-0x1000, 0x1000)}));
                Put8(At(ram, BodyAddress(car)), 0x619, rng.Range(0, 3));
                return CallArgs{BodyAddress(car), car};
            },
            [](uint8_t* ram, uint8_t*, const CallArgs& a) { sim::SingleGearClutch(BodyAt(ram, a.a0)); });
        Report("SgClutch", 0x80039470u, r.cases, r.mismatches, failures);
    }
    {
        const StatefulResult r = RunStateful(
            guest, pristine, 0x80039994u, 120,
            [&](uint8_t* ram, uint8_t*, size_t variant) {
                const uint32_t car = uint32_t(variant % kCarCount);
                Put32(At(ram, BodyAddress(car)), 0x624, rng.Chance(4) ? int32_t(rng.Next()) : rng.Range(0, 0x400000));
                return CallArgs{BodyAddress(car), car};
            },
            [](uint8_t* ram, uint8_t*, const CallArgs& a) { sim::SingleGearRpm(BodyAt(ram, a.a0)); });
        Report("SgRpm", 0x80039994u, r.cases, r.mismatches, failures);
    }
    {
        const StatefulResult r = RunStateful(
            guest, pristine, 0x800399C4u, 600,
            [&](uint8_t* ram, uint8_t* scratch, size_t variant) {
                const uint32_t car = uint32_t(variant % kCarCount);
                uint8_t* body = At(ram, BodyAddress(car));
                Put8(body, 0x618, rng.Range(0, 2));
                Put8(body, 0x372, rng.Pick({1, 2, 4}));
                Put8(body, 0x619, rng.Range(0, 3));
                Put32(body, 0x6A4, rng.Pick({0, 1138, 1139, -1138, -1139, rng.Range(-0x8000, 0x8000)}));
                Put16(body, 0x708, rng.Chance(2) ? 0 : rng.Range(1, 0x1000));
                scratch[kGearRequests + car * 4] = uint8_t(rng.Range(0, 1));
                scratch[kGearRequests + car * 4 + 1] = uint8_t(rng.Range(-1, 1));
                return CallArgs{BodyAddress(car), car};
            },
            [](uint8_t* ram, uint8_t* scratch, const CallArgs& a) {
                sim::SingleGearSelect(BodyAt(ram, a.a0), *reinterpret_cast<const sim::GearRequest*>(scratch + kGearRequests + a.a1 * 4));
            });
        Report("SgSelect", 0x800399C4u, r.cases, r.mismatches, failures);
    }
    // ---- 0x8003C398: cheap 3-vector length (pure)
    {
        size_t cases = 0, bad = 0;
        for (int i = 0; i < 20000; i++) {
            const int tier = 1 + i % 2;
            int32_t v[3];
            for (int32_t& x : v) x = rng.Chance(8) ? rng.Pick({0, 0x7FFFFFFF, int32_t(0x80000000), -1}) : rng.Spread(tier, 0x100000, 0x7FFFFFFF);
            cases++;
            const int32_t original = int32_t(guest.Call(0x8003C398u, uint32_t(v[0]), uint32_t(v[1]), uint32_t(v[2]))), ours = sim::ApproxLength3(v[0], v[1], v[2]);
            if (original != ours && bad++ < 3) std::printf("    MISMATCH ApproxLength3(%d, %d, %d): original %d, ours %d\n", v[0], v[1], v[2], original, ours);
        }
        Report("Length3", 0x8003C398u, cases, bad, failures);
    }
    // ---- 0x80033634: neighbour sector flags (body, along, across, frontSum; rearSum, widthSum on the stack)
    {
        const StatefulResult r = RunStateful(
            guest, pristine, 0x80033634u, 1500,
            [&](uint8_t* ram, uint8_t*, size_t variant) {
                const uint32_t body = BodyAddress(uint32_t(variant % kCarCount));
                for (uint32_t i = 0; i < 8; i++) Put8(At(ram, body), 0x76C + i, rng.Range(0, 2));
                const int32_t frontSum = rng.Range(0, 0x8000), rearSum = rng.Range(0, 0x8000), widthSum = rng.Range(-0x100, 0x4000);
                const int32_t along = rng.Chance(4) ? rng.Pick({frontSum, frontSum + 1, -rearSum, -rearSum - 1, 0}) : rng.Range(-0x10000, 0x10000);
                const int32_t across = rng.Chance(4) ? rng.Pick({widthSum / 2, widthSum / 2 + 1, -widthSum / 2, -widthSum / 2 - 1, 0}) : rng.Range(-0x8000, 0x8000);
                RamPut<int32_t>(ram, kStack + 16, rearSum);
                RamPut<int32_t>(ram, kStack + 20, widthSum);
                return CallArgs{body, uint32_t(along), uint32_t(across), uint32_t(frontSum)};
            },
            [](uint8_t* ram, uint8_t*, const CallArgs& a) {
                sim::ClassifyNeighbour(BodyAt(ram, a.a0), int32_t(a.a1), int32_t(a.a2), int32_t(a.a3), RamAt<int32_t>(ram, kStack + 16), RamAt<int32_t>(ram, kStack + 20));
            });
        Report("Neighbour", 0x80033634u, r.cases, r.mismatches, failures);
    }
    // ---- 0x8003373C: one car pair (bodyA, bodyB); needs two cars (a license test has one)
    if (kCarCount < 2) {
        std::puts("CarPair    0x8003373C  skipped (the dump has one car)");
    } else {
        const StatefulResult r = RunStateful(
            guest, pristine, 0x8003373Cu, 2400,
            [&](uint8_t* ram, uint8_t*, size_t variant) {
                const int tier = 1 + int(variant % 2);
                const uint32_t carA = uint32_t(rng.Range(0, int32_t(kCarCount) - 1));
                uint32_t carB = uint32_t(rng.Range(0, int32_t(kCarCount) - 2));
                if (carB >= carA) carB++;
                ScatterCars(rng, ram, tier);
                for (uint32_t car = 0; car < kCarCount; car++) {
                    uint8_t* body = At(ram, BodyAddress(car));
                    RandomiseDynamics(rng, body, tier);
                    if (rng.Chance(2)) Put32(body, 0x6A4, rng.Range(0x1BC00, 0x60000)); // fast enough to give draft
                    Put8(body, 0x45D, rng.Range(0, 2));
                    Put8(body, 0x45E, rng.Chance(6));
                    Put8(body, 0x789, rng.Chance(6) ? 4 : rng.Range(0, 3));
                }
                // B close to A most of the time (in A's frame: behind, beside, ahead)
                if (!rng.Chance(4)) {
                    sim::CarBody& a = BodyAt(ram, BodyAddress(carA));
                    sim::CarBody& b = BodyAt(ram, BodyAddress(carB));
                    const int32_t s = sim::Sin(uint32_t(uint16_t(a.heading))), c = sim::Cos(uint32_t(uint16_t(a.heading)));
                    const int32_t along = rng.Range(-0x14000, 0x14000), across = rng.Range(-0x4000, 0x4000);
                    b.position[0] = a.position[0] + ((-s * along + c * across) >> 12);
                    b.position[1] = a.position[1] + ((c * along + s * across) >> 12);
                    b.position[2] = a.position[2] + rng.Range(-0x6000, 0x6000);
                    if (rng.Chance(2)) b.heading = a.heading;
                }
                return CallArgs{BodyAddress(carA), BodyAddress(carB)};
            },
            [](uint8_t* ram, uint8_t*, const CallArgs& a) { sim::UpdateCarPairAwareness(BodyAt(ram, a.a0), BodyAt(ram, a.a1)); });
        Report("CarPair", 0x8003373Cu, r.cases, r.mismatches, failures);
    }
    // ---- 0x80040F30: corner refresh of the cars that touched another car
    {
        const StatefulResult r = RunStateful(
            guest, pristine, 0x80040F30u, 400,
            [&](uint8_t* ram, uint8_t*, size_t variant) {
                ScatterCars(rng, ram, int(variant % 4));
                RandomiseContactTables(rng, ram, 0x8000, 0x10000);
                for (uint32_t car = 0; car < kCarCount; car++) BodyAt(ram, BodyAddress(car)).contactFlags = uint8_t(rng.Range(0, 3));
                return CallArgs{kCarBase, kCarCount};
            },
            [](uint8_t* ram, uint8_t*, const CallArgs&) { sim::RefreshContactCorners(StateOf(ram), CarsOf(ram), int(kCarCount)); });
        Report("Corners", 0x80040F30u, r.cases, r.mismatches, failures);
    }
    // ---- 0x800412D4: push forces between overlapping cars
    {
        size_t pushes = 0;
        const StatefulResult r = RunStateful(
            guest, pristine, 0x800412D4u, 600,
            [&](uint8_t* ram, uint8_t*, size_t variant) {
                ScatterCars(rng, ram, int(variant % 4));
                RandomiseContactTables(rng, ram, variant % 2 ? 0x2000 : 0x8000, variant % 2 ? 0x6000 : 0x10000);
                for (uint32_t car = 0; car < kCarCount; car++)
                    for (uint32_t off : {0x778u, 0x77Cu}) Put32(At(ram, BodyAddress(car)), off, int32_t(rng.Next()));
                return CallArgs{kCarBase, kCarCount};
            },
            [&](uint8_t* ram, uint8_t*, const CallArgs&) {
                sim::ComputeContactPush(StateOf(ram), CarsOf(ram), int(kCarCount));
                for (uint32_t car = 0; car < kCarCount; car++) pushes += RamAt<int32_t>(ram, BodyAddress(car) + 0x778) != 0;
            });
        std::printf("%-10s 0x800412D4  %zu cases (%zu pushed cars), %zu mismatches  %s\n", "Push", r.cases, pushes, r.mismatches, r.mismatches ? "FAIL" : "ok");
        failures += r.mismatches ? 1 : 0;
    }

    // ---- 0x80039FC8: tyre forces for the six cars
    {
        const StatefulResult r = RunStateful(
            guest, pristine, 0x80039FC8u, 600,
            [&](uint8_t* ram, uint8_t* scratch, size_t variant) {
                const int tier = int(variant % 4);
                RandomiseWearConstants(rng, ram, variant % 3 == 2);
                for (uint32_t car = 0; car < kCarCount; car++) {
                    uint8_t* body = At(ram, BodyAddress(car));
                    RandomiseDynamics(rng, body, tier);
                    if (tier != 0) Put8(body, 0x45D, rng.Range(0, 3));
                    RandomiseBlock(rng, body, WorkOf(scratch).cars[car], tier == 0 ? 1 : tier, !rng.Chance(4));
                    scratch[kGearRequests + car * 4] = uint8_t(rng.Range(0, 1));
                    scratch[kGearRequests + car * 4 + 1] = uint8_t(rng.Range(-1, 1));
                }
                const int32_t stepTime = BodyAt(ram, BodyAddress(0)).stepTime;
                std::memcpy(scratch, &stepTime, 4);
                return CallArgs{kCarBase, kCarCount};
            },
            [&](uint8_t* ram, uint8_t* scratch, const CallArgs&) {
                sim::TyreForces(contextFor(ram, scratch), CarsOf(ram), int(kCarCount), WorkOf(scratch), reinterpret_cast<const sim::GearRequest*>(scratch + kGearRequests));
            });
        std::printf("%-10s 0x80039FC8  %zu cases, %zu mismatches, %zu skipped (original trapped)  %s\n", "TyreForce", r.cases, r.mismatches, g_skipped,
                    r.mismatches ? "FAIL" : "ok");
        failures += r.mismatches ? 1 : 0;
        g_skipped = 0;
    }

    // ---- 0x8003E0C4: the physics core for the six cars, per input mode
    // "Core/ai*" runs the original AI routines through the guest inside our port; "Core/ai" runs the ported AI
    // (ai_driver.*) - the full-frame check of the AI drivers.
    struct CoreMode { const char* name; InputMode mode; size_t variants; bool nativeAi; };
    const CoreMode coreModes[] = {{"Core/none", InputMode::NoInput, 300, false}, {"Core/pad0", InputMode::ZeroPad, 300, false},
                                  {"Core/mixed", InputMode::Mixed, 200, false}, {"Core/ai*", InputMode::Dump, 200, false}, {"Core/ai", InputMode::Dump, 400, true}};
    for (const CoreMode& mode : coreModes) {
        bridge.aiCalls = 0;
        bridge.nativeAi = mode.nativeAi;
        const StatefulResult r = RunStateful(
            guest, pristine, 0x8003E0C4u, mode.variants,
            [&](uint8_t* ram, uint8_t* scratch, size_t variant) {
                const int tier = int(variant % 4);
                RandomiseWearConstants(rng, ram, variant % 3 == 2);
                for (uint32_t car = 0; car < kCarCount; car++) {
                    uint8_t* body = At(ram, BodyAddress(car));
                    RandomiseDynamics(rng, body, tier);
                    if (mode.mode == InputMode::Dump && tier != 0) { // the AI drivers need a race in progress
                        Put8(body, 0x718, 0);
                        Put16(body, 0x78E, rng.Chance(8) ? rng.Range(1, 300) : 0);
                        Put8(body, 0x786, rng.Chance(8) ? 7 : 0);
                    }
                    RandomiseBlock(rng, body, WorkOf(scratch).cars[car], tier == 0 ? 1 : tier, false);
                    scratch[kGearRequests + car * 4] = uint8_t(rng.Range(0, 1));
                    scratch[kGearRequests + car * 4 + 1] = uint8_t(rng.Range(-1, 1));
                }
                SetInputMode(rng, ram, mode.mode);
                return CallArgs{kCarBase, kCarCount};
            },
            [&](uint8_t* ram, uint8_t* scratch, const CallArgs&) {
                const sim::PadRecord pads[kMaxCars] = {}; // car + 0x18 == 0: the original produces zeroed records
                sim::PhysicsCore(contextFor(ram, scratch), CarsOf(ram), int(kCarCount), pads, WorkOf(scratch),
                                 reinterpret_cast<sim::GearRequest*>(scratch + kGearRequests), reinterpret_cast<int32_t(*)[4]>(scratch + kDeltas));
            });
        std::printf("%-10s 0x8003E0C4  %zu cases (%zu AI driver calls %s), %zu mismatches, %zu skipped (original trapped)  %s\n", mode.name, r.cases,
                    bridge.aiCalls, mode.nativeAi ? "of the ported AI" : "in the guest", r.mismatches, g_skipped, r.mismatches ? "FAIL" : "ok");
        failures += r.mismatches ? 1 : 0;
        g_skipped = 0;
    }
    bridge.nativeAi = false;

    // ---- 0x80034480: the step driver (needs the course for the wall sweep)
    if (!track) {
        std::puts("SimCars    0x80034480  skipped (needs the course: gt2verify <ram> <disc> <course>)");
        return failures;
    }
    auto prepareStep = [&](uint8_t* ram, uint8_t* scratch, size_t variant) {
        const int tier = int(variant % 4);
        ScatterCars(rng, ram, tier == 3 ? 1 : tier);
        // The corner tables keep the dump's (real) records plus a real proximity pass on the pre-move positions
        // as the previous step: with arbitrary records in the previous buffer the sweep yields crossing
        // fractions outside 0..0x1000, and for those the original's resolution reads an uninitialised edge
        // normal (stack locals of 0x80040924 left over from the previous pair) and wraps the remaining fraction
        // to 16 bits, which the port (car_contact.cpp) does not reproduce.
        StateOf(ram).buffer = int32_t(rng.Next() & 1);
        sim::UpdateCarProximity(StateOf(ram), CarsOf(ram), int(kCarCount));
        // Displacements of one step at up to 5 m and 2.6 degrees (the largest the game produces); larger ones
        // give spurious edge crossings in the sweep (fractions outside 0..0x1000, see above).
        static constexpr int32_t kScales[4] = {1500, 6000, 12000, 20000};
        const int32_t scale = kScales[(variant / 3) % 4];
        for (uint32_t car = 0; car < kCarCount; car++) {
            uint8_t* body = At(ram, BodyAddress(car));
            int32_t delta[4];
            for (int i = 0; i < 3; i++) delta[i] = rng.Range(-scale, scale);
            delta[3] = rng.Range(-30, 30);
            std::memcpy(scratch + kDeltas + car * 0x10, delta, sizeof(delta));
            if (tier == 0) continue;
            RandomiseDynamics(rng, body, tier);
            for (int i = 0; i < 3; i++) Put32(body, 0x628 + uint32_t(i) * 4, rng.Range(-scale * 30, scale * 30));
            Put8(body, 0x45D, rng.Range(0, 2));
            Put8(body, 0x786, rng.Chance(3) ? rng.Range(1, 7) : 0);
            Put16(body, 0x6FE, 2184);
        }
        const int32_t stepTime = 2184;
        std::memcpy(scratch, &stepTime, 4);
        return CallArgs{kCarBase, kCarCount};
    };
    // Stage by stage first, at the original's callable boundaries, so that a difference names its stage.
    {
        struct Stage { const char* name; uint32_t function; };
        static constexpr Stage kStages[] = {{"MovePass", 0x80034320u}, {"Proximity", 0x800400CCu}, {"Sweep", 0x800407A0u}, {"Resolve", 0x80040924u},
                                            {"Corners", 0x80040F30u}, {"Push", 0x800412D4u}, {"Pairs", 0x8003373Cu}};
        size_t stageMismatches[7] = {};
        size_t cases = 0, mismatches = 0, crossings = 0, oddCrossings = 0; // sweep results, and those outside 0..0x1000
        std::vector<uint8_t> ours(Bus::kRamSize), ourScratch(kScratchSize), preStage(Bus::kRamSize), preStageScratch(kScratchSize);
        std::vector<uint8_t> guestRam(Bus::kRamSize), guestScratch(kScratchSize);
        const uint32_t stackLow = (kStack & 0x1FFFFF) - 0x2000, stackHigh = (kStack & 0x1FFFFF) + 0x100;
        for (size_t variant = 0; variant < 600; variant++) {
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            std::memset(guest.Scratch(), 0, kScratchSize);
            {
                SimContactLayout layout(guest.Ram()); // the native view of the contact state (guest.h)
                prepareStep(guest.Ram(), guest.Scratch(), variant);
            }
            for (uint32_t car = 0; car < kCarCount; car++) { // the clears before the move pass have no boundary: done on both images
                BodyAt(guest.Ram(), BodyAddress(car)).contactFlags = 0;
                sim::ClearNeighbourFields(BodyAt(guest.Ram(), BodyAddress(car)));
            }
            std::memcpy(ours.data(), guest.Ram(), Bus::kRamSize);
            std::memcpy(ourScratch.data(), guest.Scratch(), kScratchSize);
            cases++;
            bool trapped = false;
            for (size_t stage = 0; stage < 7 && !trapped; stage++) {
                std::memcpy(preStage.data(), ours.data(), Bus::kRamSize); // state before this stage (both images agree here)
                std::memcpy(preStageScratch.data(), ourScratch.data(), kScratchSize);
                try {
                    if (kStages[stage].function == 0x8003373Cu) {
                        for (uint32_t i = 0; i < kCarCount; i++)
                            for (uint32_t j = i + 1; j < kCarCount; j++) guest.Call(kStages[stage].function, BodyAddress(i), BodyAddress(j));
                    } else {
                        guest.Call(kStages[stage].function, kCarBase, kCarCount);
                    }
                } catch (const std::runtime_error&) {
                    trapped = true;
                    break;
                }
                std::optional<SimContactLayout> layout(std::in_place, ours.data()); // the native view of the contact state (guest.h)
                const sim::PhysicsContext& physics = contextFor(ours.data(), ourScratch.data());
                sim::Car* cars = CarsOf(ours.data());
                auto deltas = reinterpret_cast<int32_t(*)[4]>(ourScratch.data() + kDeltas);
                switch (stage) {
                case 0:
                    for (uint32_t car = 0; car < kCarCount; car++) {
                        sim::MoveContext move = physics.move;
                        move.controlClass = sim::ControlClass(physics, cars[car].body);
                        sim::MovePass(move, &cars[car], 1, &deltas[car]);
                    }
                    break;
                case 1: sim::UpdateCarProximity(*physics.contact, cars, int(kCarCount)); break;
                case 2: sim::SweepCarPairs(*physics.contact, cars, int(kCarCount)); break;
                case 3: sim::ResolveCarContacts(physics.move, *physics.contact, cars, int(kCarCount)); break;
                case 4: sim::RefreshContactCorners(*physics.contact, cars, int(kCarCount)); break;
                case 5: sim::ComputeContactPush(*physics.contact, cars, int(kCarCount)); break;
                default:
                    for (uint32_t i = 0; i < kCarCount; i++)
                        for (uint32_t j = i + 1; j < kCarCount; j++) sim::UpdateCarPairAwareness(cars[i].body, cars[j].body);
                    break;
                }
                layout.reset();
                if (stage == 2) {
                    SimContactLayout guestView(guest.Ram()); // the guest's tables in the port's layout while they are read (restored below)
                    const sim::CarContactState& table = StateOf(guest.Ram());
                    for (uint32_t i = 0; i < kCarCount; i++)
                        for (uint32_t slot = 0; slot < 5; slot++) {
                            const int16_t f = table.fraction[i][slot];
                            crossings += f != 0x1000;
                            if (f == 0x1000 || (f >= 0 && f <= 0x1000)) continue;
                            if (oddCrossings++ < 6) { // show how the sweep arrives at a fraction outside 0..0x1000
                                const sim::ContactCorner& record = table.corners[i][slot][table.corner[i][slot]];
                                const int previous = 1 - table.buffer, current = table.buffer;
                                const sim::CarBody& self = BodyAt(guest.Ram(), BodyAddress(i));
                                std::printf("      odd crossing: car %u slot %u corner %u side %d fraction %d | flags %X -> %X, x %d -> %d, y %d -> %d | front %d rear %d width %d\n", i,
                                            slot, table.corner[i][slot], table.side[i][slot], f, record.edgeFlags[previous], record.edgeFlags[current], record.x[previous],
                                            record.x[current], record.y[previous], record.y[current], self.frontExtent, self.rearExtent, self.width);
                            }
                        }
                }
                std::memcpy(ourScratch.data(), guest.Scratch(), 0xD4); // temporaries of the original's sweeps
                const bool equal = std::memcmp(ours.data(), guest.Ram(), stackLow) == 0 &&
                                   std::memcmp(ours.data() + stackHigh, guest.Ram() + stackHigh, Bus::kRamSize - stackHigh) == 0 &&
                                   std::memcmp(ourScratch.data(), guest.Scratch(), kScratchSize) == 0;
                if (equal) continue;
                stageMismatches[stage]++;
                if (stage == 3 && mismatches < 3) { // localise the pair: replay the resolution with every other pair masked off
                    std::memcpy(guestRam.data(), guest.Ram(), Bus::kRamSize); // keep the original's result for the report below
                    std::memcpy(guestScratch.data(), guest.Scratch(), kScratchSize);
                    std::vector<uint8_t> pre(Bus::kRamSize), preScratch(kScratchSize), lone(Bus::kRamSize), loneScratch(kScratchSize);
                    std::memcpy(pre.data(), preStage.data(), Bus::kRamSize);
                    std::memcpy(preScratch.data(), preStageScratch.data(), kScratchSize);
                    SimContactLayout preView(pre.data()); // `pre` is read (and copied) in the port's layout
                    for (uint32_t i = 1; i < kCarCount; i++)
                        for (uint32_t j = 0; j < i; j++) {
                            const sim::CarContactState& table = StateOf(pre.data());
                            if (table.fraction[i][j] == 0x1000 && table.fraction[j][i - 1] == 0x1000) continue;
                            std::memcpy(lone.data(), pre.data(), Bus::kRamSize);
                            std::memcpy(loneScratch.data(), preScratch.data(), kScratchSize);
                            sim::CarContactState& masked = StateOf(lone.data());
                            for (uint32_t p = 0; p < kCarCount; p++)
                                for (uint32_t q = 0; q < 5; q++)
                                    if (!((p == i && q == j) || (p == j && q == i - 1))) masked.fraction[p][q] = 0x1000;
                            { // the guest gets `lone` in its own build's layout; the native side keeps the port's layout
                                std::vector<uint8_t> build(lone);
                                preView.UndoOn(build.data());
                                std::memcpy(guest.Ram(), build.data(), Bus::kRamSize);
                            }
                            std::memcpy(guest.Scratch(), loneScratch.data(), kScratchSize);
                            guest.Call(0x80040924u, kCarBase, kCarCount);
                            const sim::PhysicsContext& lonePhysics = contextFor(lone.data(), loneScratch.data());
                            sim::ResolveCarContacts(lonePhysics.move, *lonePhysics.contact, CarsOf(lone.data()), int(kCarCount));
                            preView.UndoOn(lone.data());
                            const bool same = std::memcmp(lone.data(), guest.Ram(), stackLow) == 0 &&
                                              std::memcmp(lone.data() + stackHigh, guest.Ram() + stackHigh, Bus::kRamSize - stackHigh) == 0;
                            const sim::CarBody& bi = BodyAt(pre.data(), BodyAddress(i));
                            const sim::CarBody& bj = BodyAt(pre.data(), BodyAddress(j));
                            std::printf("      pair (%u,%u) alone: %s  fraction ij %d ji %d side ij %d ji %d corner ij %u ji %u | i: heading %d scale %d vel %d %d %d pos %d %d %d | j: heading %d scale %d vel %d %d %d pos %d %d %d\n",
                                        i, j, same ? "same" : "DIFFERS", table.fraction[i][j], table.fraction[j][i - 1], table.side[i][j], table.side[j][i - 1],
                                        table.corner[i][j], table.corner[j][i - 1], bi.heading, bi.timeScale, bi.velocity[0], bi.velocity[1], bi.velocity[2],
                                        bi.position[0], bi.position[1], bi.position[2], bj.heading, bj.timeScale, bj.velocity[0], bj.velocity[1], bj.velocity[2],
                                        bj.position[0], bj.position[1], bj.position[2]);
                            if (!same) {
                                for (uint32_t car : {i, j}) {
                                    const sim::CarBody& o = BodyAt(guest.Ram(), BodyAddress(car));
                                    const sim::CarBody& u = BodyAt(lone.data(), BodyAddress(car));
                                    std::printf("        car %u after: original vel %d %d %d pos %d %d %d impact %d hit %02X scrape %d | ours vel %d %d %d pos %d %d %d impact %d hit %02X scrape %d\n", car,
                                                o.velocity[0], o.velocity[1], o.velocity[2], o.position[0], o.position[1], o.position[2], o.wallImpact, o.wallHitMask,
                                                o.scrapeDirection, u.velocity[0], u.velocity[1], u.velocity[2], u.position[0], u.position[1], u.position[2], u.wallImpact,
                                                u.wallHitMask, u.scrapeDirection);
                                }
                            }
                        }
                    std::memcpy(guest.Ram(), guestRam.data(), Bus::kRamSize);
                    std::memcpy(guest.Scratch(), guestScratch.data(), kScratchSize);
                }
                if (mismatches++ < 3) {
                    int shown = 0;
                    for (uint32_t i = 0; i < Bus::kRamSize && shown < 6; i++)
                        if ((i < stackLow || i >= stackHigh) && ours[i] != guest.Ram()[i]) {
                            const uint32_t address = 0x80000000u + i;
                            std::printf("    MISMATCH variant %zu at stage %s: byte 0x%08X", variant, kStages[stage].name, address);
                            if (address >= kCarBase && address < kCarBase + kCarCount * kCarStride)
                                std::printf(" (car %u body + 0x%X)", (address - kCarBase) / kCarStride, (address - kCarBase) % kCarStride - kBodyOffset);
                            std::printf(": original %02X ours %02X\n", guest.Ram()[i], ours[i]);
                            shown++;
                        }
                    for (uint32_t car = 0; car < kCarCount; car++) {
                        const sim::CarBody& b = BodyAt(guest.Ram(), BodyAddress(car));
                        std::printf("      car %u: hit %02X contact %02X flags45D %d 786 %u pos %d %d %d vel %d %d %d\n", car, b.wallHitMask, b.contactFlags, b.controlClass,
                                    b.raceState, b.position[0], b.position[1], b.position[2], b.velocity[0], b.velocity[1], b.velocity[2]);
                    }
                }
                break;
            }
            if (trapped) { cases--; g_skipped++; }
        }
        std::printf("%-10s 0x80034480  %zu cases (%zu crossings, %zu outside 0..0x1000), %zu mismatches (per stage:", "SimStages", cases, crossings, oddCrossings, mismatches);
        for (size_t stage = 0; stage < 7; stage++) std::printf(" %s %zu", kStages[stage].name, stageMismatches[stage]);
        std::printf("), %zu skipped  %s\n", g_skipped, mismatches ? "FAIL" : "ok");
        failures += mismatches ? 1 : 0;
        g_skipped = 0;
    }
    {
        bridge.soundCalls = 0;
        size_t wallHits = 0, carContacts = 0;
        const StatefulResult r = RunStateful(
            guest, pristine, 0x80034480u, 600, prepareStep,
            [&](uint8_t* ram, uint8_t* scratch, const CallArgs&) {
                sim::SimulateCars(contextFor(ram, scratch), CarsOf(ram), int(kCarCount), reinterpret_cast<int32_t(*)[4]>(scratch + kDeltas));
                for (uint32_t car = 0; car < kCarCount; car++) {
                    wallHits += BodyAt(ram, BodyAddress(car)).wallHitMask != 0;
                    carContacts += (BodyAt(ram, BodyAddress(car)).contactFlags & 2) != 0;
                }
            },
            ScratchIgnore{0, 0xD4}); // the original's wall sweep and contact resolution keep temporaries at 0x00..0xD4
        std::printf("%-10s 0x80034480  %zu cases (%zu wall hits, %zu car contacts, %zu sound events), %zu mismatches, %zu skipped  %s\n", "SimCars", r.cases, wallHits,
                    carContacts, bridge.soundCalls, r.mismatches, g_skipped, r.mismatches ? "FAIL" : "ok");
        failures += r.mismatches ? 1 : 0;
        g_skipped = 0;
    }
    return failures;
}

} // namespace gt2::verify
