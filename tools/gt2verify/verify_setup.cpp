// Differential checks of src/game/sim/car_setup.* against the original (see guest.h for the harness).
// The car parameter records of the dump (0x801DE8BA + slot * 0x1C0) are copied into free guest RAM, randomised
// over realistic and extreme ranges (a quarter of the variants keep the dump's own record) and handed to the
// original and to the port with the same body; afterwards the whole RAM must match.
#include <cstring>
#include <stdexcept>
#include <string>

#include "game/sim/car_setup.h"
#include "game/sim/fixed.h"
#include "game/sim/race_shell.h"
#include "game/sim/trig.h"
#include "guest.h"

namespace gt2::verify {

namespace {

constexpr uint32_t kFree = 0x801E8000u;          // free guest RAM for the record copy and output scratch
constexpr uint32_t kRecordCopy = kFree;           // CarParams copy (0x1C0 bytes)
constexpr uint32_t kOutputs = kFree + 0x400;      // small output buffers of the helper rows
constexpr uint32_t kDumpRecords = 0x801DE8BAu;    // the six records of the attract race (0x8001523C)
constexpr uint32_t kTrackObject = 0x800A9500u;
constexpr uint32_t kCourseTable = 0x801E18E8u;    // 0x80060E94: 24-byte entries, +8 = flags (bit 2 = dirt)
constexpr uint32_t kCourseIndex = 0x800AF230u;
constexpr uint32_t kQueryScratch = kStack - 0x80; // guest stack area (excluded from the comparison) for callback records

sim::CarBody& BodyAt(uint8_t* ram, uint32_t object) { return *reinterpret_cast<sim::CarBody*>(ram + (object & 0x1FFFFF)); }
sim::CarParams& ParamsAt(uint8_t* ram, uint32_t address) { return *reinterpret_cast<sim::CarParams*>(ram + (address & 0x1FFFFF)); }

template <typename T>
T RamAt(const uint8_t* ram, uint32_t address) {
    T v;
    std::memcpy(&v, ram + (address & 0x1FFFFF), sizeof(T));
    return v;
}
template <typename T>
void RamPut(uint8_t* ram, uint32_t address, T v) { std::memcpy(ram + (address & 0x1FFFFF), &v, sizeof(T)); }

int32_t Between(std::mt19937& rng, int32_t low, int32_t high) { return low + int32_t(rng() % uint32_t(high - low + 1)); }
uint8_t Byte(std::mt19937& rng, int32_t low, int32_t high) { return uint8_t(Between(rng, low, high)); }
bool Chance(std::mt19937& rng, uint32_t oneIn) { return rng() % oneIn == 0; }

std::string g_caseDescription;
size_t g_skipped = 0; // cases of the current row in which the original trapped

struct CallArgs {
    uint32_t a1 = 0, a2 = 0, a3 = 0;
    uint32_t stack[10] = {};  // 5th.. arguments at sp + 0x10 ..
    uint32_t stackCount = 0;
};

// Stateful comparison like VerifyStateful (guest.h) with stack arguments and a native side that may call the guest.
template <typename Prepare, typename Native>
StatefulResult RunStateful(Guest& guest, const std::vector<uint8_t>& pristine, uint32_t function, const std::vector<uint32_t>& objects,
                           size_t variants, bool checkReturn, Prepare prepare, Native native) {
    StatefulResult result;
    g_skipped = 0;
    std::vector<uint8_t> ours(Bus::kRamSize), ourScratch(kScratchSize);
    for (uint32_t object : objects)
        for (size_t variant = 0; variant < variants; variant++) {
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            std::memset(guest.Scratch(), 0, kScratchSize);
            g_caseDescription.clear();
            const CallArgs args = prepare(guest.Ram(), guest.Scratch(), object, variant);
            for (uint32_t i = 0; i < args.stackCount; i++) RamPut(guest.Ram(), kStack + 0x10 + i * 4, args.stack[i]);
            std::memcpy(ours.data(), guest.Ram(), Bus::kRamSize);
            std::memcpy(ourScratch.data(), guest.Scratch(), kScratchSize);
            uint32_t original = 0;
            try {
                original = guest.Call(function, object, args.a1, args.a2, args.a3);
            } catch (const std::runtime_error&) { // the original trapped (its 64-bit division breaks on a zero divisor)
                if (g_skipped++ < 2 && !g_caseDescription.empty()) std::printf("    skipped (original trapped): %s\n", g_caseDescription.c_str());
                continue;
            }
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
                int shown = 0;
                for (uint32_t i = 0; i < Bus::kRamSize && shown < 4; i++)
                    if ((i < stackLow || i >= stackHigh) && ours[i] != guest.Ram()[i]) {
                        std::printf("    MISMATCH object %08X variant %zu: differing byte at 0x%08X (object + 0x%X): original %02X ours %02X\n", object,
                                    variant, 0x80000000u + i, i - (object & 0x1FFFFF), guest.Ram()[i], ours[i]);
                        shown++;
                    }
                for (uint32_t i = 0; i < kScratchSize && shown == 0; i++)
                    if (ourScratch[i] != guest.Scratch()[i]) {
                        std::printf("    MISMATCH object %08X variant %zu: first differing scratchpad byte at 0x1F800000 + 0x%X: original %02X ours %02X\n",
                                    object, variant, i, guest.Scratch()[i], ourScratch[i]);
                        shown++;
                    }
            }
        }
    return result;
}

// ---------------------------------------------------------------- inputs from the image

bool DirtCourse(const uint8_t* ram) {
    const uint32_t course = RamAt<uint8_t>(ram, D(kCourseIndex));
    return (RamAt<uint16_t>(ram, D(kCourseTable) + course * 24 + 8) & 4) != 0;
}
void SetDirtCourse(uint8_t* ram, bool dirt) {
    const uint32_t course = RamAt<uint8_t>(ram, D(kCourseIndex));
    uint16_t flags = RamAt<uint16_t>(ram, D(kCourseTable) + course * 24 + 8);
    flags = uint16_t(dirt ? flags | 4 : flags & ~4u);
    RamPut(ram, D(kCourseTable) + course * 24 + 8, flags);
}

// The constants the port reads, resolved on the given image (pointers into it).
sim::SetupConstants ConstantsOf(uint8_t* ram) {
    sim::SetupConstants c;
    c.dragConstant = RamAt<int32_t>(ram, D(0x80046EF0));
    c.rollingResistanceGain = RamAt<int32_t>(ram, D(0x80046E00));
    c.springRateRange[0] = RamAt<uint8_t>(ram, D(0x80046DC8));
    c.springRateRange[1] = RamAt<uint8_t>(ram, D(0x80046DC9));
    c.diffTypeCodes = reinterpret_cast<const int8_t*>(ram + (D(0x80046DCCu) & 0x1FFFFF));
    c.gearAutoTable = ram + (D(0x800923E2u) & 0x1FFFFF);
    c.rollingResistance.count = RamAt<uint16_t>(ram, D(0x801C8730));
    c.rollingResistance.xs = reinterpret_cast<const int32_t*>(ram + (RamAt<uint32_t>(ram, D(0x801C8734)) & 0x1FFFFF));
    c.rollingResistance.ys = reinterpret_cast<const int32_t*>(ram + (RamAt<uint32_t>(ram, D(0x801C8738)) & 0x1FFFFF));
    c.aiGripPercent = ram + (D(0x801C98A4u) & 0x1FFFFF);
    c.classTuning = reinterpret_cast<const sim::DriveClassTuning*>(ram + (D(0x801C8690u) & 0x1FFFFF));
    return c;
}

sim::TyreWearConstants WearOf(const uint8_t* ram) {
    sim::TyreWearConstants w;
    w.wearLimit = RamAt<int32_t>(ram, D(0x80046F48));
    w.wornGripLoss = RamAt<int32_t>(ram, D(0x80046F4C));
    w.pitGripFactor = RamAt<int32_t>(ram, D(0x80046F50));
    w.coldLimit = RamAt<int32_t>(ram, D(0x80046F54));
    w.coldGripLoss = RamAt<int32_t>(ram, D(0x80046F58));
    w.wearKnee = RamAt<int32_t>(ram, D(0x80046F5C));
    w.kneeGripLoss = RamAt<int32_t>(ram, D(0x80046F60));
    return w;
}

// Randomises the globals the setup reads (both images see the same values: they are written before the copy).
void RandomGlobals(std::mt19937& rng, uint8_t* ram, size_t variant) {
    if (variant % 4 == 0) return; // the dump's own tables
    SetDirtCourse(ram, Chance(rng, 3));
    if (Chance(rng, 2)) RamPut<int32_t>(ram, D(0x80046EF0), Between(rng, 0, 20000));
    if (Chance(rng, 2)) RamPut<int32_t>(ram, D(0x80046E00), Between(rng, 0, 8000));
    if (Chance(rng, 3)) {
        RamPut<uint8_t>(ram, D(0x80046DC8), Byte(rng, 1, 120));
        RamPut<uint8_t>(ram, D(0x80046DC9), Byte(rng, 1, 200));
    }
    for (uint32_t i = 0; i < 8; i++)
        if (Chance(rng, 3)) RamPut<uint8_t>(ram, D(0x801C98A4) + i, Byte(rng, 0, 255));
    for (uint32_t cls = 0; cls < 4; cls++) {
        const uint32_t record = D(0x801C8690) + cls * 40;
        if (Chance(rng, 2)) RamPut<int16_t>(ram, record + 0x24, int16_t(Between(rng, -0x2000, 0x2000)));
        if (Chance(rng, 2)) RamPut<int16_t>(ram, record + 0x02, int16_t(Between(rng, 0, 0x4000)));
        if (Chance(rng, 2)) RamPut<int32_t>(ram, record + 0x04, Between(rng, -0x100000, 0x100000));
    }
}

// Would 0x80030DA0 divide by zero for these damper points? (The original traps in the 64-bit division; the
// randomiser re-rolls such points.) Mirrors the arithmetic of sim::SetupDamperCurve up to its divisor.
bool DamperTraps(int16_t a, int32_t b, int16_t c, int32_t d) {
    if (b == 0 || d == 0 || a == 0 || c == 0 || !(a < c) || !(b < d)) return false;
    const int32_t cross = int32_t(uint32_t(sim::Mul12Wide(b, c)) - uint32_t(sim::Mul12Wide(d, a)));
    if (cross <= 0) return false;
    const int32_t curvature = int32_t(uint32_t(sim::Mul12Wide(sim::Mul12Wide(b, b), c)) - uint32_t(sim::Mul12Wide(sim::Mul12Wide(d, d), a)));
    if (curvature > 0) return false;
    const int64_t numerator = int64_t(sim::Mul12Wide(sim::Mul12Wide(b, d), int32_t(uint32_t(d) - uint32_t(b)))) << 12;
    return sim::Div64(numerator, cross) == 0;
}

// The damper inputs of one axle as 0x80030F94 derives them, for the trap check.
bool SuspensionTraps(const uint8_t settings[12], uint8_t divisor) {
    const int32_t scale = sim::Div(0x64000, int32_t(divisor));
    auto knee = [&](uint8_t v) { return int16_t(sim::Mul12Wide(scale, int16_t((uint32_t(v) << 12) / 100))); };
    auto force = [](uint8_t v) { return int32_t(uint32_t(v) * 0xC4004u) >> 12; };
    return DamperTraps(knee(settings[4]), force(settings[5]), knee(settings[6]), force(settings[7])) ||
           DamperTraps(knee(settings[8]), force(settings[9]), knee(settings[10]), force(settings[11]));
}

void RandomSuspension(std::mt19937& rng, uint8_t settings[12], uint8_t& divisor, bool extreme) {
    for (int attempt = 0; attempt < 100; attempt++) {
        settings[0] = extreme ? Byte(rng, 0, 255) : Byte(rng, 10, 255);
        settings[1] = Byte(rng, 0, 255);
        settings[2] = Byte(rng, 0, 255);
        settings[3] = extreme ? Byte(rng, 0, 255) : Byte(rng, 5, 120);
        for (int i = 4; i < 12; i++) settings[i] = extreme ? Byte(rng, 0, 255) : Byte(rng, 5, 255);
        divisor = extreme && Chance(rng, 8) ? 0 : Byte(rng, 1, 255);
        if (!SuspensionTraps(settings, divisor)) return;
    }
}

// Randomises a record in place (starting from a copy of a dump record). `extreme` widens the ranges.
void RandomParams(std::mt19937& rng, sim::CarParams& p, bool extreme, bool touchReserved) {
    auto b = [&](int32_t low, int32_t high) { return Byte(rng, low, high); };
    p.frontLength = int16_t(extreme ? Between(rng, -32768, 32767) : Between(rng, 500, 4000));
    p.rearLength = int16_t(extreme ? Between(rng, -32768, 32767) : Between(rng, 500, 4000));
    p.width = int16_t(extreme ? Between(rng, 1, 32767) : Between(rng, 1200, 2200));
    p.height = int16_t(extreme ? Between(rng, 1, 32767) : Between(rng, 900, 2000));
    p.wheelbase = int16_t(extreme ? Between(rng, 1, 32767) : Between(rng, 1800, 3500));
    p.frontWeightPercent = extreme ? b(0, 100) : b(30, 70);
    p.idleRpm10 = b(0, 255);
    p.frontTrack = int16_t(extreme ? Between(rng, 1, 32767) : Between(rng, 1000, 2000));
    p.rearTrack = int16_t(extreme ? Between(rng, 1, 32767) : Between(rng, 1000, 2000));
    p.gearCount = b(1, 7);
    for (int g = 0; g < 8; g++) p.gearRatio[g] = int16_t(g <= p.gearCount ? Between(rng, 100, 5000) : (Chance(rng, 2) ? -1 : Between(rng, -32768, 32767)));
    p.finalDrive = int16_t(Between(rng, 1000, 6000));
    p.engineInertia = Chance(rng, 6) ? 0 : b(1, 255);
    p.engineBrake = b(0, 255);
    p.turboBoostCap10 = Chance(rng, 2) ? 0 : b(1, 60);
    p.absGain[0] = b(0, 255);
    p.absGain[1] = b(0, 255);
    p.upshiftRpm100 = b(0, 255);
    p.revLimitRpm100 = Chance(rng, 3) ? 0 : b(1, 255);
    const int32_t count = extreme ? Between(rng, 2, 16) : Between(rng, 5, 16);
    p.torquePointCount = uint8_t(count);
    int32_t rpm = Between(rng, 3, 20);
    for (int i = 0; i < 16; i++) {
        if (i < count) {
            rpm += Between(rng, 1, 12);
            if (rpm > 255) rpm = 255 - (count - i); // keep it strictly increasing
            p.torqueRpm100[i] = uint8_t(rpm);
            p.torque[i] = uint16_t(extreme ? Between(rng, 1, 0xFFFF) : Between(rng, 100, 4000));
        } else {
            p.torqueRpm100[i] = b(0, 255);
            p.torque[i] = uint16_t(Between(rng, 0, 0xFFFF));
        }
    }
    if (Chance(rng, 4)) p.torque[count - 1] = 0; // exercises the fill from the previous sample
    p.steerLimitCount = b(0, 6);
    for (int i = 0; i < 6; i++) {
        p.steerLimitXs[i] = b(0, 255);
        p.steerLimitYs[i] = b(0, 255);
    }
    p.fourWheelType = extreme ? b(0, 255) : b(0, 4);
    p.yawInertiaCode = b(0, 255);
    p.pitchInertiaCode = b(0, 255);
    p.rollInertiaCode = b(0, 255);
    p.weightKg = int16_t(extreme ? Between(rng, -1000, 32767) : Between(rng, 400, 3000));
    p.downshiftFloorRpm10 = b(0, 255);
    p.steerLockDeg = b(0, 255);
    p.steerRateDeg = b(0, 255);
    p.brakeFront = b(1, 255);
    p.brakeRear = b(1, 255);
    p.dragCoefficient100 = b(0, 255);
    p.handbrake = b(0, 255);
    for (int i = 0; i < 2; i++) {
        p.wheelInertia[i] = b(1, 255);
        p.tyreWidthCode[i] = extreme ? b(0, 255) : b(5, 40);
        p.rimCode[i] = extreme ? b(0, 255) : b(8, 30);
        p.tyreAspectCode[i] = extreme ? b(0, 255) : b(2, 80);
    }
    p.camberFront10 = b(0, 255);
    p.camberRear10 = b(0, 255);
    for (int axle = 0; axle < 2; axle++) RandomSuspension(rng, p.suspension[axle], p.damperScaleDivisor[axle], extreme);
    for (int i = 0; i < 2; i++) {
        p.tyreGripPercent[i] = Chance(rng, 5) ? 0 : b(1, 255);
        p.downforce[i] = b(0, 255);
        p.tyreGripModifier[i] = Chance(rng, 5) ? 0 : b(1, 255);
    }
    p.driveType = extreme ? b(0, 8) : b(0, 4);
    p.clutchCode = b(0, 255);
    // Tyre curves: the sample at zero slip ratio must give a non-zero grip (the setup divides by it).
    p.slipAngleCount = b(1, 8);
    p.slipAngleCountRear = b(1, 8);
    for (int i = 0; i < 8; i++) {
        p.slipAngleXs[i] = b(0, 255); p.slipAngleYs[i] = b(0, 255);
        p.slipAngleXsRear[i] = b(0, 255); p.slipAngleYsRear[i] = b(0, 255);
    }
    p.slipRatioNegCount = b(1, 6);
    p.slipRatioPosCount = b(1, 6);
    p.slipRatioNegCountRear = b(1, 6);
    p.slipRatioPosCountRear = b(1, 6);
    for (int i = 0; i < 6; i++) {
        p.slipRatioNegXs[i] = b(0, 255); p.slipRatioNegYs[i] = b(0, 255); p.slipRatioNegYs2[i] = b(0, 255);
        p.slipRatioPosXs[i] = b(0, 255); p.slipRatioPosYs[i] = b(0, 255); p.slipRatioPosYs2[i] = b(0, 255);
        p.slipRatioNegXsRear[i] = b(0, 255); p.slipRatioNegYsRear[i] = b(0, 255); p.slipRatioNegYs2Rear[i] = b(0, 255);
        p.slipRatioPosXsRear[i] = b(0, 255); p.slipRatioPosYsRear[i] = b(0, 255); p.slipRatioPosYs2Rear[i] = b(0, 255);
    }
    p.slipRatioPosYs2[0] = b(10, 255);
    p.slipRatioPosYs2Rear[0] = b(10, 255);
    for (int i = 0; i < 2; i++) p.rideHeightMm[i] = b(0, 255);
    p.steerMaxRateCode = b(0, 255);
    p.centreSplitPercent = b(0, 255);
    p.loadGripCount = b(1, 4);
    p.loadGripCountRear = b(1, 4);
    p.camberGripCount = b(1, 4);
    p.camberGripCountRear = b(1, 4);
    for (int i = 0; i < 4; i++) {
        p.loadGripXs[i] = b(0, 255); p.loadGripYs[i] = b(0, 255); p.loadGripXsRear[i] = b(0, 255); p.loadGripYsRear[i] = b(0, 255);
        p.camberGripXs[i] = b(0, 255); p.camberGripYs[i] = b(0, 255); p.camberGripXsRear[i] = b(0, 255); p.camberGripYsRear[i] = b(0, 255);
    }
    p.turboSpoolRpm100 = Chance(rng, 4) ? 0 : b(1, 100);
    p.turboBoost10 = Chance(rng, 4) ? 0 : b(1, 40);
    p.turboSpoolRate10 = Chance(rng, 4) ? 0 : b(1, 60);
    p.turboSpoolRpm100Second = Chance(rng, 3) ? 0 : b(1, 100);
    p.turboBoost10Second = Chance(rng, 3) ? 0 : b(1, 40);
    p.turboSpoolRate10Second = Chance(rng, 3) ? 0 : b(1, 60);
    p.turboModel[0] = b(0, 3);
    p.turboModel[1] = b(0, 3);
    for (int i = 0; i < 2; i++) {
        p.axleInertiaCode[i] = b(0, 255);
        static const char kCodes[] = "frnmhvyt";
        p.diffTypeCode[i] = Chance(rng, 4) ? int8_t(Between(rng, -128, 127)) : int8_t(kCodes[rng() % 8]);
        p.diffInitialTorque[i] = b(0, 255);
        p.diffAccel[i] = b(0, 255);
        p.diffDecel[i] = b(0, 255);
        p.toeCode[i] = b(0, 255);
        p.bumpTravelMm[i] = b(0, 255);
        p.droopTravelMm[i] = b(0, 255);
    }
    for (int i = 0; i < 8; i++) p.surfaceGripPercent[i] = Chance(rng, 4) ? 0 : b(1, 255);
    p.tcsFalloffGain10 = b(0, 255);
    p.tcsSteerGain100 = b(0, 255);
    p.tcsGain10 = b(0, 255);
    p.asmYawGain100 = b(0, 255);
    p.asmYawThreshold100 = b(0, 255);
    p.gearAutoSet = Chance(rng, 3) ? 1 : 0;
    p.gearAutoFinal = Chance(rng, 4) ? 0 : (Chance(rng, 4) ? 0xFF : b(1, 254));
    p.powerPercent = Chance(rng, 4) ? 0 : b(1, 255);
    p.torqueMultiplier1000 = uint16_t(Chance(rng, 4) ? 0 : (Chance(rng, 2) ? Between(rng, 1, 255) : Between(rng, 256, 3000)));
    p.powerPercentTop = uint16_t(Chance(rng, 4) ? 0 : Between(rng, 1, 3000));
    if (touchReserved) {
        // Bytes the setup is believed not to read: a mismatch confined to these variants would expose an unknown read.
        for (uint8_t& v : p.reserved000) v = b(0, 255);
        p.reserved030 = b(0, 255);
        p.reserved033 = b(0, 255);
        for (uint8_t& v : p.reserved052) v = b(0, 255);
        p.reserved05C = b(0, 255);
        for (uint8_t& v : p.reserved08C) v = b(0, 255);
        for (uint8_t& v : p.reserved0FC) v = b(0, 255);
        for (uint8_t& v : p.reserved15E) v = b(0, 255);
        for (uint8_t& v : p.reserved180) v = b(0, 255);
        for (uint8_t& v : p.reserved190) v = b(0, 255);
        for (uint8_t& v : p.reserved19A) v = b(0, 255);
        for (uint8_t& v : p.reserved1A7) v = b(0, 255);
        p.reserved1AD = b(0, 255);
        for (uint8_t& v : p.reserved1B2) v = b(0, 255);
    }
}

// Places a record copy at kRecordCopy: the dump's record of the car for variant % 4 == 0, else a randomised one.
void PrepareRecord(std::mt19937& rng, uint8_t* ram, uint32_t object, size_t variant) {
    const uint32_t car = (object - kCarBase - kBodyOffset) / kCarStride;
    const uint32_t source = D(kDumpRecords) + car * sizeof(sim::CarParams);
    std::memcpy(ram + (kRecordCopy & 0x1FFFFF), ram + (source & 0x1FFFFF), sizeof(sim::CarParams));
    if (variant % 4 == 0) return;
    RandomParams(rng, ParamsAt(ram, kRecordCopy), variant % 4 == 3, variant % 8 == 7);
}

void RandomBytes(std::mt19937& rng, uint8_t* ram, uint32_t address, uint32_t size, size_t variant) {
    for (uint32_t i = 0; i < size; i++) ram[(address + i) & 0x1FFFFF] = variant % 3 == 0 ? 0 : uint8_t(rng());
}

// Puts a fully set-up body (the original's code) with a random record on the guest before a sub-routine test,
// so that the inputs the sub-routine reads from the body are consistent. A record on which the original traps
// (zero divisor in its 64-bit division, e.g. a generated gear ratio of 0) is re-rolled from the pristine image.
void PrepareSetupBody(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, uint32_t object, size_t variant) {
    for (int attempt = 0; attempt < 100; attempt++) {
        PrepareRecord(rng, guest.Ram(), object, variant);
        RandomGlobals(rng, guest.Ram(), variant);
        RandomBytes(rng, guest.Ram(), object, 0x460, variant);
        RamPut<uint32_t>(guest.Ram(), kStack + 0x10, 0);
        try {
            guest.Call(0x800319A8, object, kRecordCopy, uint32_t(Between(rng, 0, 2)), 0);
            return;
        } catch (const std::runtime_error&) {
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            std::memset(guest.Scratch(), 0, kScratchSize);
        }
    }
}

// Course queries answered by the original on the guest (the record lives in the guest stack area, which the
// comparison excludes). The queries only read the course object, so running them after the original's own
// placement is harmless.
class GuestCourse : public sim::CoursePlacementQueries {
public:
    explicit GuestCourse(Guest& guest) : guest_(guest) {}
    int32_t GroundHeight(int32_t x, int32_t y, int32_t& chunk) override {
        RamPut<int32_t>(guest_.Ram(), kQueryScratch, chunk);
        RamPut<uint32_t>(guest_.Ram(), kStack + 0x10, kQueryScratch); // 5th argument: the chunk in/out pointer
        const int32_t height = int32_t(guest_.Call(0x80028900, D(kTrackObject), uint32_t(x), uint32_t(y), 0x64000));
        chunk = RamAt<int32_t>(guest_.Ram(), kQueryScratch);
        return height;
    }
    int32_t CourseDistance(int32_t chunk, int32_t x16, int32_t height16, int32_t y16) override {
        RamPut<int32_t>(guest_.Ram(), kQueryScratch + 0x10, x16);
        RamPut<int32_t>(guest_.Ram(), kQueryScratch + 0x14, height16);
        RamPut<int32_t>(guest_.Ram(), kQueryScratch + 0x18, y16);
        return int32_t(guest_.Call(0x80028C6C, D(kTrackObject), uint32_t(chunk), kQueryScratch + 0x10));
    }
    void Contact(sim::ContactQuery& query) override {
        std::memcpy(guest_.Ram() + ((kQueryScratch + 0x20) & 0x1FFFFF), &query, sizeof query);
        guest_.Call(0x80028830, D(kTrackObject), kQueryScratch + 0x20);
        std::memcpy(&query, guest_.Ram() + ((kQueryScratch + 0x20) & 0x1FFFFF), sizeof query);
    }

private:
    Guest& guest_;
};

// Race-progress lists resolved on an image.
void ResolveSectionLists(uint8_t* ram, sim::RaceSectionList lists[7]) {
    const uint32_t grid = RamAt<uint32_t>(ram, D(0x801C8568));
    for (uint32_t i = 0; i < 7; i++) {
        const uint32_t list = RamAt<uint32_t>(ram, grid + 8 + i * 4);
        lists[i] = {};
        if (list == 0) continue;
        lists[i].count = RamAt<int32_t>(ram, list);
        lists[i].sections = reinterpret_cast<const sim::RaceSection*>(ram + ((list + 4) & 0x1FFFFF));
    }
}

// A random grid position near one of the dump's cars (so that the course queries find road most of the time).
void RandomPlacement(std::mt19937& rng, const uint8_t* ram, size_t variant, int32_t& chunk, int32_t& x, int32_t& y, int32_t& sinH, int32_t& cosH) {
    const uint32_t car = rng() % kCarCount;
    const uint32_t body = kCarBase + car * kCarStride + kBodyOffset;
    chunk = RamAt<int32_t>(ram, body + 0x600);
    x = RamAt<int32_t>(ram, body + 0x65C);
    y = RamAt<int32_t>(ram, body + 0x660);
    if (variant % 4 != 0) {
        x += Between(rng, -0x8000, 0x8000); // +- 8 m
        y += Between(rng, -0x8000, 0x8000);
        if (variant % 8 == 3) chunk = Between(rng, 0, chunk + 8);
    }
    const uint32_t heading = variant % 4 == 0 ? uint32_t(RamAt<int16_t>(ram, body + 0x648)) : rng();
    sinH = sim::Sin(heading);
    cosH = sim::Cos(heading);
}

} // namespace

int VerifySetup(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng) {
    int failures = 0;
    std::vector<uint32_t> bodies;
    for (uint32_t car = 0; car < kCarCount; car++) bodies.push_back(kCarBase + car * kCarStride + kBodyOffset);

    // ---- 0x80030BA4: weight mapping (pure)
    {
        size_t cases = 0, bad = 0;
        for (int i = 0; i < 3000; i++) {
            const int16_t w = int16_t(i < 2000 ? Between(rng, -1000, 2000) : Between(rng, -32768, 32767));
            cases++;
            const int32_t original = int32_t(guest.Call(0x80030BA4, uint32_t(uint16_t(w))));
            const int32_t ours = sim::MapWeight(w);
            if (original != ours && bad++ < 3) std::printf("    MISMATCH MapWeight(%d): original %d ours %d\n", w, original, ours);
        }
        Report("MapWeight", 0x80030BA4u, cases, bad, failures);
    }

    // ---- 0x80030D18: drive class (pure)
    {
        size_t cases = 0, bad = 0;
        for (uint32_t v = 0; v < 256; v++) {
            cases++;
            const uint32_t original = guest.Call(0x80030D18, v);
            const uint32_t ours = sim::DriveClassOf(uint8_t(v));
            if (original != ours && bad++ < 3) std::printf("    MISMATCH DriveClassOf(%u): original %u ours %u\n", v, original, ours);
        }
        Report("DriveClass", 0x80030D18u, cases, bad, failures);
    }

    // ---- 0x80075FAC: wheel radius (outputs through pointers)
    {
        StatefulResult r = RunStateful(
            guest, pristine, 0x80075FAC, {kRecordCopy}, 1200, true,
            [&](uint8_t* ram, uint8_t*, uint32_t, size_t variant) {
                PrepareRecord(rng, ram, kCarBase + kBodyOffset + (variant % kCarCount) * kCarStride, variant);
                CallArgs a;
                a.a1 = rng() & 1;
                a.a2 = kOutputs;
                a.a3 = kOutputs + 4;
                a.stack[0] = kOutputs + 8;
                a.stackCount = 1;
                return a;
            },
            [](uint8_t* ram, uint8_t*, uint32_t, const CallArgs& a) {
                int16_t total, rim, tyre;
                const int32_t result = sim::WheelRadius(ParamsAt(ram, kRecordCopy), int(a.a1), total, rim, tyre);
                RamPut(ram, a.a2, total);
                RamPut(ram, a.a3, rim);
                RamPut(ram, a.stack[0], tyre);
                return uint32_t(result);
            });
        Report("WheelRad", 0x80075FACu, r.cases, r.mismatches, failures);
    }

    // ---- 0x80076070: drive layout
    {
        StatefulResult r = RunStateful(
            guest, pristine, 0x80076070, {kRecordCopy}, 1500, true,
            [&](uint8_t* ram, uint8_t*, uint32_t, size_t variant) {
                PrepareRecord(rng, ram, kCarBase + kBodyOffset + (variant % kCarCount) * kCarStride, variant);
                if (variant % 4 != 0) ParamsAt(ram, kRecordCopy).driveType = Byte(rng, 0, 6);
                SetDirtCourse(ram, Chance(rng, 2));
                RamPut<uint8_t>(ram, kOutputs, uint8_t(rng()));
                RamPut<uint8_t>(ram, kOutputs + 1, uint8_t(rng()));
                CallArgs a;
                a.a1 = kOutputs;
                a.a2 = kOutputs + 1;
                a.a3 = kOutputs + 2;
                a.stack[0] = kOutputs + 4;
                a.stackCount = 1;
                return a;
            },
            [](uint8_t* ram, uint8_t*, uint32_t, const CallArgs& a) {
                uint8_t type = RamAt<uint8_t>(ram, a.a1), primary = RamAt<uint8_t>(ram, a.a2);
                int16_t split, lock;
                const uint8_t result = sim::SetupDriveLayout(ParamsAt(ram, kRecordCopy), DirtCourse(ram), type, primary, split, lock);
                RamPut(ram, a.a1, type);
                RamPut(ram, a.a2, primary);
                RamPut(ram, a.a3, split);
                RamPut(ram, a.stack[0], lock);
                return uint32_t(result);
            });
        Report("DriveLayout", 0x80076070u, r.cases, r.mismatches, failures);
    }

    // ---- 0x80030DA0: damper curve from two points
    {
        StatefulResult r = RunStateful(
            guest, pristine, 0x80030DA0, {kOutputs}, 3000, false,
            [&](uint8_t*, uint8_t*, uint32_t, size_t variant) {
                CallArgs a;
                a.a1 = kOutputs + 4;
                a.a2 = kOutputs + 8;
                int16_t lowKnee, highKnee;
                int32_t lowForce, highForce;
                do {
                    lowKnee = int16_t(variant % 3 == 0 ? Between(rng, -0x8000, 0x7FFF) : Between(rng, 0, 6000));
                    highKnee = int16_t(variant % 3 == 0 ? Between(rng, -0x8000, 0x7FFF) : Between(rng, 0, 6000));
                    lowForce = variant % 3 == 0 ? Between(rng, -0x100000, 0x100000) : Between(rng, 0, 50000);
                    highForce = variant % 3 == 0 ? Between(rng, -0x100000, 0x100000) : Between(rng, 0, 50000);
                    if (variant % 5 == 1) { lowKnee = 0; }
                } while (DamperTraps(lowKnee, lowForce, highKnee, highForce));
                a.a3 = uint32_t(int32_t(lowKnee));
                a.stack[0] = uint32_t(lowForce);
                a.stack[1] = uint32_t(int32_t(highKnee));
                a.stack[2] = uint32_t(highForce);
                a.stack[3] = 0;
                a.stack[4] = D(0x80046BC4); // the name string of the original (unused)
                a.stackCount = 5;
                g_caseDescription = "damper a=" + std::to_string(lowKnee) + " b=" + std::to_string(lowForce) + " c=" + std::to_string(highKnee) +
                                    " d=" + std::to_string(highForce);
                return a;
            },
            [](uint8_t* ram, uint8_t*, uint32_t object, const CallArgs& a) {
                int32_t knee, base, gain;
                sim::SetupDamperCurve(knee, base, gain, int16_t(a.a3), int32_t(a.stack[0]), int16_t(a.stack[1]), int32_t(a.stack[2]));
                RamPut(ram, object, knee);
                RamPut(ram, a.a1, base);
                RamPut(ram, a.a2, gain);
                return 0u;
            });
        Report("Damper", 0x80030DA0u, r.cases, r.mismatches, failures);
    }

    // ---- 0x80030F94: suspension block of one axle
    {
        std::vector<uint32_t> blocks;
        for (uint32_t body : bodies) blocks.push_back(body + 0x12C);
        StatefulResult r = RunStateful(
            guest, pristine, 0x80030F94, blocks, 300, false,
            [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) {
                PrepareRecord(rng, ram, object - 0x12C, variant);
                RandomGlobals(rng, ram, variant);
                RandomBytes(rng, ram, object, 0x34, variant);
                CallArgs a;
                const uint32_t axle = rng() & 1;
                a.a1 = kRecordCopy + 0x6E + axle * 12;
                a.a2 = uint32_t(ParamsAt(ram, kRecordCopy).damperScaleDivisor[axle]) | (variant % 5 == 4 ? (rng() << 8) : 0u); // only the low byte counts
                a.a3 = 0;
                return a;
            },
            [](uint8_t* ram, uint8_t*, uint32_t object, const CallArgs& a) {
                const sim::SuspensionParams& settings = *reinterpret_cast<const sim::SuspensionParams*>(ram + (a.a1 & 0x1FFFFF));
                sim::SetupSuspension(*reinterpret_cast<sim::AxleSuspension*>(ram + (object & 0x1FFFFF)), settings, uint8_t(a.a2), ConstantsOf(ram));
                return 0u;
            });
        Report("Suspension", 0x80030F94u, r.cases, r.mismatches, failures);
    }

    // ---- 0x80075328: engine block (the record is patched)
    {
        StatefulResult r = RunStateful(
            guest, pristine, 0x80075328, {kRecordCopy}, 1200, false,
            [&](uint8_t* ram, uint8_t*, uint32_t, size_t variant) {
                const uint32_t body = kCarBase + kBodyOffset + (variant % kCarCount) * kCarStride;
                PrepareRecord(rng, ram, body, variant);
                RandomBytes(rng, ram, body + 0x7C, 0xB0, variant);
                CallArgs a;
                a.a1 = body + 0x7C;
                a.a2 = 0;
                return a;
            },
            [](uint8_t* ram, uint8_t*, uint32_t, const CallArgs& a) {
                sim::SetupEngine(BodyAt(ram, a.a1 - 0x7C), ParamsAt(ram, kRecordCopy), a.a1 - 0x7C);
                return 0u;
            });
        Report("Engine", 0x80075328u, r.cases, r.mismatches, failures);
    }

    // ---- 0x80074B38: generated gear ratios
    {
        StatefulResult r = RunStateful(
            guest, pristine, 0x80074B38, {kRecordCopy}, 1500, false,
            [&](uint8_t* ram, uint8_t*, uint32_t, size_t variant) {
                PrepareRecord(rng, ram, kCarBase + kBodyOffset + (variant % kCarCount) * kCarStride, variant);
                sim::CarParams& p = ParamsAt(ram, kRecordCopy);
                if (variant % 4 != 0 && p.driveType > 4) p.driveType = 0; // the setup clamps it before this is reached
                SetDirtCourse(ram, Chance(rng, 2));
                CallArgs a;
                a.a1 = uint32_t(Chance(rng, 5) ? 0xFF : Between(rng, 10, 2550));
                a.a2 = kRecordCopy + 0x18;
                g_caseDescription = "gears " + std::to_string(p.gearCount) + " position " + std::to_string(a.a1) + " final " + std::to_string(p.finalDrive);
                return a;
            },
            [](uint8_t* ram, uint8_t*, uint32_t, const CallArgs& a) {
                sim::GenerateGearRatios(ParamsAt(ram, kRecordCopy), int32_t(a.a1), DirtCourse(ram), ConstantsOf(ram));
                return 0u;
            });
        Report("GearAuto", 0x80074B38u, r.cases, r.mismatches, failures);
    }

    // ---- 0x800347C4: drivetrain block (needs a consistent engine block: the original's own, prepared first)
    {
        StatefulResult r = RunStateful(
            guest, pristine, 0x800347C4, {kRecordCopy}, 1200, false,
            [&](uint8_t* ram, uint8_t*, uint32_t, size_t variant) {
                const uint32_t body = kCarBase + kBodyOffset + (variant % kCarCount) * kCarStride;
                PrepareRecord(rng, ram, body, variant);
                sim::CarParams& p = ParamsAt(ram, kRecordCopy);
                if (p.driveType > 4) p.driveType = 0;
                RandomGlobals(rng, ram, variant);
                RandomBytes(rng, ram, body + 0x370, 0xB0, variant);
                guest.Call(0x80075328, kRecordCopy, body + 0x7C, 0); // engine block of the original for this record
                CallArgs a;
                a.a1 = body + 0x370;
                a.a2 = body + 0x7C;
                a.a3 = 0;
                g_caseDescription = "gears " + std::to_string(p.gearCount) + " drive " + std::to_string(p.driveType) + " auto " + std::to_string(p.gearAutoSet) +
                                    "/" + std::to_string(p.gearAutoFinal);
                return a;
            },
            [](uint8_t* ram, uint8_t*, uint32_t, const CallArgs& a) {
                sim::SetupDrivetrain(BodyAt(ram, a.a1 - 0x370), ParamsAt(ram, kRecordCopy), DirtCourse(ram), ConstantsOf(ram));
                return 0u;
            });
        Report("Drivetrain", 0x800347C4u, r.cases, r.mismatches, failures);
    }

    // ---- 0x800314DC: slip-ratio curves of one axle. a0 = negXs (the "object"), the rest in a1..a3 and the stack.
    {
        const std::vector<uint32_t> negXs = {kRecordCopy + 0xB1, kRecordCopy + 0xD7};
        StatefulResult r = RunStateful(
            guest, pristine, 0x800314DC, negXs, 1200, false,
            [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) {
                const uint32_t body = kCarBase + kBodyOffset + (variant % kCarCount) * kCarStride;
                const bool rear = object == kRecordCopy + 0xD7;
                const uint32_t block = body + (rear ? 0x26C : 0x194);
                PrepareRecord(rng, ram, body, variant);
                RandomBytes(rng, ram, block, 0xD8, variant);
                const uint32_t base = object - 1;
                CallArgs a;
                a.a1 = base + 7;
                a.a2 = base + 13;
                a.a3 = RamAt<uint8_t>(ram, base);
                a.stack[0] = base + 20;
                a.stack[1] = base + 26;
                a.stack[2] = base + 32;
                a.stack[3] = RamAt<uint8_t>(ram, base + 19);
                a.stack[4] = block;
                a.stackCount = 5;
                return a;
            },
            [](uint8_t* ram, uint8_t*, uint32_t object, const CallArgs& a) {
                uint8_t* base = ram + ((object - 1) & 0x1FFFFF);
                sim::SetupSlipRatioCurves(*reinterpret_cast<sim::AxleTyreBlock*>(ram + (a.stack[4] & 0x1FFFFF)), uint8_t(a.a3), base + 1, base + 7, base + 13,
                                          base + 20, base + 26, base + 32, uint8_t(a.stack[3]), a.stack[4]);
                return 0u;
            });
        Report("SlipRatio", 0x800314DCu, r.cases, r.mismatches, failures);
    }

    // ---- 0x800316F4: slip-angle curve. a0 = xs, a1 = ys, a2 = count, a3 = block.
    {
        const std::vector<uint32_t> xs = {kRecordCopy + 0x8F, kRecordCopy + 0xA0};
        StatefulResult r = RunStateful(
            guest, pristine, 0x800316F4, xs, 1200, false,
            [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) {
                const uint32_t body = kCarBase + kBodyOffset + (variant % kCarCount) * kCarStride;
                const bool rear = object == kRecordCopy + 0xA0;
                const uint32_t block = body + (rear ? 0x26C : 0x194);
                PrepareRecord(rng, ram, body, variant);
                RandomBytes(rng, ram, block, 0xD8, variant);
                CallArgs a;
                a.a1 = object + 8;
                a.a2 = RamAt<uint8_t>(ram, object - 1) | (variant % 5 == 4 ? (rng() << 8) : 0u); // only the low byte counts
                a.a3 = block;
                return a;
            },
            [](uint8_t* ram, uint8_t*, uint32_t object, const CallArgs& a) {
                sim::SetupSlipAngleCurve(*reinterpret_cast<sim::AxleTyreBlock*>(ram + (a.a3 & 0x1FFFFF)), ram + (object & 0x1FFFFF), ram + (a.a1 & 0x1FFFFF),
                                         uint8_t(a.a2), a.a3);
                return 0u;
            });
        Report("SlipAngle", 0x800316F4u, r.cases, r.mismatches, failures);
    }

    // ---- 0x80031794: load and camber curves. a0 = loadXs, a1 = loadYs, a2 = loadCount, a3 = camberXs, stack: camberYs, camberCount, radius, grip, block, param_4.
    {
        const std::vector<uint32_t> xs = {kRecordCopy + 0x11B, kRecordCopy + 0x124};
        StatefulResult r = RunStateful(
            guest, pristine, 0x80031794, xs, 1200, false,
            [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) {
                const uint32_t body = kCarBase + kBodyOffset + (variant % kCarCount) * kCarStride;
                const bool rear = object == kRecordCopy + 0x124;
                const uint32_t block = body + (rear ? 0x26C : 0x194);
                PrepareRecord(rng, ram, body, variant);
                RandomBytes(rng, ram, block, 0xD8, variant);
                CallArgs a;
                a.a1 = object + 4;
                a.a2 = RamAt<uint8_t>(ram, object - 1);
                a.a3 = object + 0x12;
                a.stack[0] = object + 0x16;
                a.stack[1] = RamAt<uint8_t>(ram, object + 0x11);
                a.stack[2] = uint32_t(int32_t(int16_t(variant % 3 == 0 ? Between(rng, -0x8000, 0x7FFF) : Between(rng, 100, 3000))));
                a.stack[3] = uint32_t(variant % 3 == 0 ? Between(rng, -0x100000, 0x100000) : Between(rng, 0, 0x3000));
                a.stack[4] = block;
                a.stack[5] = 0;
                a.stackCount = 6;
                return a;
            },
            [](uint8_t* ram, uint8_t*, uint32_t object, const CallArgs& a) {
                sim::SetupLoadAndCamberCurves(*reinterpret_cast<sim::AxleTyreBlock*>(ram + (a.stack[4] & 0x1FFFFF)), ram + (object & 0x1FFFFF),
                                              ram + (a.a1 & 0x1FFFFF), uint8_t(a.a2), ram + (a.a3 & 0x1FFFFF), ram + (a.stack[0] & 0x1FFFFF),
                                              uint8_t(a.stack[1]), int16_t(a.stack[2]), int32_t(a.stack[3]), a.stack[4]);
                return 0u;
            });
        Report("LoadCamber", 0x80031794u, r.cases, r.mismatches, failures);
    }

    // ---- 0x8003B598: peak slip angle of the front curve (pure on the body)
    {
        StatefulResult r = RunStateful(
            guest, pristine, 0x8003B598, bodies, 200, true,
            [&](uint8_t*, uint8_t*, uint32_t object, size_t variant) {
                PrepareSetupBody(guest, pristine, rng, object, variant);
                return CallArgs{};
            },
            [](uint8_t* ram, uint8_t*, uint32_t object, const CallArgs&) { return uint32_t(sim::PeakSlipAngle(BodyAt(ram, object))); });
        Report("PeakSlip", 0x8003B598u, r.cases, r.mismatches, failures);
    }

    // ---- 0x80030C5C: top speed estimate on a set-up body
    {
        StatefulResult r = RunStateful(
            guest, pristine, 0x80030C5C, bodies, 200, false,
            [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) {
                PrepareSetupBody(guest, pristine, rng, object, variant);
                if (variant % 3 == 2) RamPut<int32_t>(ram, object + 0x344, Between(rng, 0, 0x100000));
                return CallArgs{};
            },
            [](uint8_t* ram, uint8_t*, uint32_t object, const CallArgs&) {
                sim::SetupTopSpeed(BodyAt(ram, object), ConstantsOf(ram));
                return 0u;
            });
        Report("TopSpeed", 0x80030C5Cu, r.cases, r.mismatches, failures);
    }

    // ---- 0x800312FC: suspension travel of one axle on a set-up body
    {
        StatefulResult r = RunStateful(
            guest, pristine, 0x800312FC, bodies, 200, false,
            [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) {
                PrepareSetupBody(guest, pristine, rng, object, variant);
                CallArgs a;
                const uint32_t axle = rng() & 1;
                a.a1 = object + 0x12C + axle * 0x34;
                a.a2 = kRecordCopy;
                a.a3 = axle;
                if (variant % 3 == 2) RamPut<int32_t>(ram, a.a1 + 0x0C, Between(rng, 1, 0x1000000));
                return a;
            },
            [](uint8_t* ram, uint8_t*, uint32_t object, const CallArgs& a) {
                sim::SetupSuspensionTravel(BodyAt(ram, object), *reinterpret_cast<sim::AxleSuspension*>(ram + (a.a1 & 0x1FFFFF)), ParamsAt(ram, kRecordCopy),
                                           int(a.a3));
                return 0u;
            });
        Report("SuspTravel", 0x800312FCu, r.cases, r.mismatches, failures);
    }

    // ---- 0x800319A8: the whole per-car setup
    {
        StatefulResult r = RunStateful(
            guest, pristine, 0x800319A8, bodies, 200, true,
            [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) {
                PrepareRecord(rng, ram, object, variant);
                RandomGlobals(rng, ram, variant);
                RandomBytes(rng, ram, object, 0x460, variant);
                CallArgs a;
                a.a1 = kRecordCopy;
                a.a2 = uint32_t(int32_t(int8_t(variant % 3 == 0 ? 2 : Between(rng, -128, 127))));
                a.a3 = 0;
                a.stack[0] = uint8_t(rng());
                a.stackCount = 1;
                const sim::CarParams& p = ParamsAt(ram, kRecordCopy);
                g_caseDescription = "variant " + std::to_string(variant) + " drive " + std::to_string(p.driveType) + "/" + std::to_string(p.fourWheelType) +
                                    " gears " + std::to_string(p.gearCount) + " points " + std::to_string(p.torquePointCount) + " dirt " + std::to_string(DirtCourse(ram));
                return a;
            },
            [](uint8_t* ram, uint8_t*, uint32_t object, const CallArgs& a) {
                sim::CarSetupInputs in;
                in.params = &ParamsAt(ram, kRecordCopy);
                in.bodyToken = object;
                in.controlMode = uint8_t(a.a2);
                in.byte1C = uint8_t(a.stack[0]);
                in.dirtCourse = DirtCourse(ram);
                in.constants = ConstantsOf(ram);
                sim::SetupCar(BodyAt(ram, object), in);
                return 0u;
            });
        Report("SetupCar", 0x800319A8u, r.cases, r.mismatches, failures);
    }

    // ---- 0x80032E44 / 0x80032E6C / 0x80032A1C: state resets
    {
        StatefulResult r = RunStateful(
            guest, pristine, 0x80032E44, bodies, 100, false,
            [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) {
                RandomBytes(rng, ram, object + 0x600, 0x1A0, variant);
                return CallArgs{};
            },
            [](uint8_t* ram, uint8_t*, uint32_t object, const CallArgs&) { sim::ResetViewState(BodyAt(ram, object)); return 0u; });
        Report("ResetView", 0x80032E44u, r.cases, r.mismatches, failures);
        r = RunStateful(
            guest, pristine, 0x80032E6C, bodies, 200, false,
            [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) {
                RandomBytes(rng, ram, object + 0x370, 0x430, variant);
                if (Chance(rng, 2)) RamPut<int32_t>(ram, D(0x801C8570), Between(rng, -100, 100));
                return CallArgs{};
            },
            [](uint8_t* ram, uint8_t*, uint32_t object, const CallArgs&) { sim::ResetDynamicState(BodyAt(ram, object), RamAt<int32_t>(ram, D(0x801C8570))); return 0u; });
        Report("ResetDyn", 0x80032E6Cu, r.cases, r.mismatches, failures);
        r = RunStateful(
            guest, pristine, 0x80032A1C, bodies, 200, false,
            [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) {
                RandomBytes(rng, ram, object + 0x460, 0x1A0, variant);
                if (variant % 2) {
                    RamPut<int32_t>(ram, D(0x80046F48), Between(rng, 0, 100000));
                    RamPut<int32_t>(ram, D(0x80046F54), Between(rng, -100000, 100000));
                    RamPut<int32_t>(ram, D(0x80046F58), Between(rng, -0x8000, 0x8000));
                }
                return CallArgs{};
            },
            [](uint8_t* ram, uint8_t*, uint32_t object, const CallArgs&) { sim::ResetTyreWear(BodyAt(ram, object), WearOf(ram)); return 0u; });
        Report("ResetWear", 0x80032A1Cu, r.cases, r.mismatches, failures);
    }

    // ---- 0x80032B0C: placement on the course (course queries answered by the original)
    {
        GuestCourse course(guest);
        StatefulResult r = RunStateful(
            guest, pristine, 0x80032B0C, bodies, 150, true,
            [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) {
                PrepareSetupBody(guest, pristine, rng, object, variant);
                RandomBytes(rng, ram, object + 0x460, 0x340, variant);
                if (Chance(rng, 2)) RamPut<uint8_t>(ram, D(0x801D5866), Byte(rng, 0, 7));
                int32_t chunk, x, y, sinH, cosH;
                RandomPlacement(rng, pristine.data(), variant, chunk, x, y, sinH, cosH); // from the untouched bodies
                CallArgs a;
                a.a1 = uint32_t(chunk);
                a.a2 = uint32_t(x);
                a.a3 = uint32_t(y);
                a.stack[0] = uint32_t(sinH);
                a.stack[1] = uint32_t(cosH);
                a.stackCount = 2;
                g_caseDescription = "chunk " + std::to_string(chunk) + " x " + std::to_string(x) + " y " + std::to_string(y);
                return a;
            },
            [&](uint8_t* ram, uint8_t*, uint32_t object, const CallArgs& a) {
                sim::PlaceCarOnCourse(BodyAt(ram, object), course, int32_t(a.a1), int32_t(a.a2), int32_t(a.a3), int32_t(a.stack[0]), int32_t(a.stack[1]),
                                      RamAt<uint8_t>(ram, D(0x801D5866)));
                return 0u;
            });
        Report("Place", 0x80032B0Cu, r.cases, r.mismatches, failures);
    }

    // ---- 0x80033384: the per-race driver
    {
        GuestCourse course(guest);
        StatefulResult r = RunStateful(
            guest, pristine, 0x80033384, bodies, 120, true,
            [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) {
                PrepareRecord(rng, ram, object, variant);
                RandomGlobals(rng, ram, variant);
                RandomBytes(rng, ram, object, 0x460, variant);
                RandomBytes(rng, ram, object + 0x600, 0x1A0, variant);
                const uint32_t carIndex = (object - kCarBase - kBodyOffset) / kCarStride;
                int32_t chunk, x, y, sinH, cosH;
                RandomPlacement(rng, pristine.data(), variant, chunk, x, y, sinH, cosH); // from the untouched bodies
                CallArgs a;
                a.a1 = kRecordCopy;
                a.a2 = uint32_t(chunk);
                a.a3 = uint32_t(x);
                a.stack[0] = uint32_t(y);
                a.stack[1] = uint32_t(sinH);
                a.stack[2] = uint32_t(cosH);
                a.stack[3] = variant % 3 == 0 ? 2u : uint32_t(Byte(rng, 0, 2)); // control class
                a.stack[4] = Byte(rng, 0, 1);                                    // transmission
                a.stack[5] = Byte(rng, 0, 2);                                    // contact type
                a.stack[6] = Byte(rng, 0, 255);                                  // body + 0x1C
                a.stack[7] = variant % 2 ? carIndex : uint32_t(Byte(rng, 0, 7)); // car index
                a.stack[8] = 0;                                                  // grid offset (0x80039040 not ported)
                a.stackCount = 9;
                if (variant % 4 != 0) {
                    uint8_t mode = Byte(rng, 0, 7);
                    // 0x8003EF40 (mode 6, contact type 2) initialises the ghost car's stream *(car 1 + 0x1C): only a mode 6
                    // dump has that stream in place
                    if (mode == 6 && a.stack[5] == 2 && RamAt<uint8_t>(pristine.data(), D(0x801D5866)) != 6) mode = 2;
                    RamPut<uint8_t>(ram, D(0x801D5866), mode);
                    RamPut<uint8_t>(ram, D(0x801C98A1), Byte(rng, 0, 255));
                    RamPut<uint8_t>(ram, D(0x801C98A2), Byte(rng, 0, 2));
                    if (Chance(rng, 2)) {
                        RamPut<int32_t>(ram, D(0x800B4A58), Between(rng, 0, 8));
                        for (uint32_t i = 0; i < 8; i++) RamPut<int32_t>(ram, D(0x800B4A5C) + i * 4, Between(rng, 0, 0x2000000));
                    }
                    if (Chance(rng, 2)) RamPut<int32_t>(ram, D(0x801C8570), Between(rng, 1, 100));
                }
                g_caseDescription = "variant " + std::to_string(variant) + " mode " + std::to_string(RamAt<uint8_t>(ram, D(0x801D5866))) + " class " +
                                    std::to_string(a.stack[3]) + " type " + std::to_string(a.stack[5]);
                return a;
            },
            [&](uint8_t* ram, uint8_t*, uint32_t object, const CallArgs& a) {
                sim::RaceStartInputs in;
                in.setup.params = &ParamsAt(ram, kRecordCopy);
                in.setup.bodyToken = object;
                in.setup.dirtCourse = DirtCourse(ram);
                in.setup.constants = ConstantsOf(ram);
                in.step.frameTime = RamAt<int32_t>(ram, D(0x801C856C));
                in.step.rate = RamAt<int32_t>(ram, D(0x801C8570));
                in.course = &course;
                in.chunkHint = int32_t(a.a2);
                in.x = int32_t(a.a3);
                in.y = int32_t(a.stack[0]);
                in.headingSin = int32_t(a.stack[1]);
                in.headingCos = int32_t(a.stack[2]);
                in.controlClass = uint8_t(a.stack[3]);
                in.transmission = uint8_t(a.stack[4]);
                in.contactType = uint8_t(a.stack[5]);
                in.byte1C = uint8_t(a.stack[6]);
                in.carIndex = uint8_t(a.stack[7]);
                in.gridOffset = int32_t(a.stack[8]);
                in.raceMode = RamAt<uint8_t>(ram, D(0x801D5866));
                in.word801C98A0 = RamAt<uint32_t>(ram, D(0x801C98A0));
                in.byte801D5869 = RamAt<uint8_t>(ram, D(0x801D5869));
                in.startLineCount = RamAt<int32_t>(ram, D(0x800B4A58));
                in.startLineDistances = reinterpret_cast<const int32_t*>(ram + (D(0x800B4A5Cu) & 0x1FFFFF));
                ResolveSectionLists(ram, in.sectionLists);
                const uint32_t grid = RamAt<uint32_t>(ram, D(0x801C8568));
                in.hasGridList = RamAt<uint32_t>(ram, grid + 0x18) != 0;
                in.raceStateTable = reinterpret_cast<const int8_t*>(ram + (D(0x80046DD4u) & 0x1FFFFF));
                in.wear = WearOf(ram);
                const uint32_t result = sim::StartCar(BodyAt(ram, object), in);
                if (in.raceMode == 6 && in.contactType == 2) { // 0x8003EF40 (race_sim.cpp runs it after the car start: the same body)
                    sim::GhostSession ghost, loaded;
                    LoadGhostSession(ram, ghost);
                    loaded = ghost;
                    sim::GhostStartCar(ghost, BodyAt(ram, object), in.hasGridList);
                    StoreGhostSession(ram, ghost, loaded);
                }
                return result;
            });
        Report("StartCar", 0x80033384u, r.cases, r.mismatches, failures);
    }

    return failures;
}

} // namespace gt2::verify
