#include "game/sim/car_setup.h"

#include <cstring>

#include "game/sim/ai_driver.h"
#include "game/sim/drivetrain.h"
#include "game/sim/fixed.h"
#include "game/sim/trig.h"

// Car setup of the US Simulation v1.2 executable, ported routine by routine from the disassembly (Ghidra's
// pseudo-C hid several register arguments; every routine below was checked against objdump). The integer
// semantics of the original are kept exactly: 32-bit products wrap, divisions by constants truncate towards
// zero (the compiler's magic-number sequences), divisions by variables are MIPS `div` (sim::Div) or the 64-bit
// runtime division of Div12Shift (Div64, which traps in the original on a zero divisor).
namespace gt2::sim {

namespace {

// Low 32 bits of a 32 x 32 product (MIPS `mult` + `mflo`).
int32_t MulLow(int32_t a, int32_t b) { return int32_t(uint32_t(a) * uint32_t(b)); }

// value * multiplier (wrapping), rounded towards zero by 2^shift (the `bgez / addiu 0xFFF` pattern).
int32_t ScaleTowardsZero(uint32_t value, uint32_t multiplier, uint32_t shift) {
    int32_t product = int32_t(value * multiplier);
    if (product < 0) product += int32_t((1u << shift) - 1u);
    return product >> shift;
}

int32_t HalfTowardsZero(int32_t v) { return (v + int32_t(uint32_t(v) >> 31)) >> 1; } // srl 31 / addu / sra 1

EngineBlock& Engine(CarBody& body) { return *reinterpret_cast<EngineBlock*>(reinterpret_cast<uint8_t*>(&body) + kEngineBlockOffset); }
DrivetrainBlock& Drivetrain(CarBody& body) { return *reinterpret_cast<DrivetrainBlock*>(reinterpret_cast<uint8_t*>(&body) + kDrivetrainBlockOffset); }
AxleTyreBlock& Tyres(CarBody& body, int axle) {
    return *reinterpret_cast<AxleTyreBlock*>(reinterpret_cast<uint8_t*>(&body) + kAxleTyreBlockOffset + uint32_t(axle) * sizeof(AxleTyreBlock));
}
const AxleTyreBlock& Tyres(const CarBody& body, int axle) {
    return *reinterpret_cast<const AxleTyreBlock*>(reinterpret_cast<const uint8_t*>(&body) + kAxleTyreBlockOffset + uint32_t(axle) * sizeof(AxleTyreBlock));
}

Wheel& WheelOf(CarBody& body, int wheel) { return body.wheels[wheel]; }

} // namespace

// ---------------------------------------------------------------- small helpers

int32_t MapWeight(int16_t weightKg) { // 0x80030BA4
    const int32_t weight = weightKg;
    if (weight > 899) return weight;
    const int32_t floored = weight < 750 ? 750 : weight;
    if (floored > 400) return int16_t(((floored - 400) * 300) / 500 + 600);
    return 600;
}

uint8_t DriveClassOf(uint8_t driveType) { // 0x80030D18 (jump table 0x80046BAC)
    switch (driveType) {
    case 1: case 2: return 1;
    case 3: case 4: case 6: return 2;
    case 5: return 3;
    default: return 0;
    }
}

int32_t WheelRadius(const CarParams& params, int axle, int16_t& total, int16_t& rimRadius, int16_t& tyreHeight) { // 0x80075FAC
    const uint32_t rimCode = params.rimCode[axle], aspectCode = params.tyreAspectCode[axle], widthCode = params.tyreWidthCode[axle];
    const int32_t rim = int32_t(rimCode * 0xA000u + 0x5000u) / 1000;     // (inch * 10 + 5) / 1000 m
    rimRadius = int16_t(rim);
    const int32_t height = MulLow(int16_t(rim), int32_t(aspectCode)) / 20; // rim * aspect / 20
    tyreHeight = int16_t(height);
    const int32_t sum = height + int32_t(widthCode * 0xFE000u) / 20000;     // + width * 0.0127 m
    total = int16_t(sum);
    return int16_t(sum);
}

uint8_t SetupDriveLayout(const CarParams& params, bool dirtCourse, uint8_t& driveType, uint8_t& primaryAxle, int16_t& centreSplit,
                         int16_t& centreLockTorque) { // 0x80076070 (jump tables 0x8008FB40, 0x8008FB58)
    centreSplit = 0;
    centreLockTorque = 0;
    switch (params.driveType) {
    case 0: case 3: case 4:
        driveType = 0;
        primaryAxle = 1;
        break;
    case 1:
        driveType = 1;
        primaryAxle = 0;
        break;
    case 2: {
        centreSplit = int16_t((uint32_t(params.centreSplitPercent) << 12) / 100);
        primaryAxle = 1;
        int32_t subtype = params.fourWheelType;
        if (dirtCourse) {
            // Every subtype becomes the coupling type 4; subtypes 0, 1 and 3 also get a fixed 50 % split.
            if (subtype != 2 && (subtype < 3 || subtype == 3)) centreSplit = 0x800;
            subtype = 4;
        }
        switch (subtype) {
        case 0: driveType = 2; primaryAxle = 0; break;
        case 1: driveType = 3; break;
        case 2:
            driveType = 4;
            if (params.centreSplitPercent >= 50) centreSplit = 0x1000;
            centreLockTorque = 0xC40;
            break;
        case 3:
            driveType = 5;
            centreSplit = int16_t(Div12Shift(centreSplit, 11380, 0));
            centreLockTorque = 5880;
            break;
        case 4: driveType = 6; break;
        default: break; // subtypes above 4 leave the type as it was
        }
        break;
    }
    default: break; // types above 4 leave everything but the split / lock untouched
    }
    return driveType;
}

// ---------------------------------------------------------------- suspension

namespace {
// Spring rate from the 10..255 spring code (0x80030F50): linear between the two range constants.
int32_t SpringRateFromCode(uint8_t code, const SetupConstants& constants) {
    const int32_t low = constants.springRateRange[0], high = constants.springRateRange[1];
    return low + ((int32_t(code) - 10) * (high - low)) / 245;
}
} // namespace

void SetupDamperCurve(int32_t& knee, int32_t& base, int32_t& gain, int16_t lowKnee, int32_t lowForce, int16_t highKnee,
                      int32_t highForce) { // 0x80030DA0
    gain = 0;
    base = 0;
    knee = 0;
    const int32_t a = lowKnee, c = highKnee, b = lowForce, d = highForce;
    if (b == 0 || d == 0 || a == 0 || c == 0 || !(a < c) || !(b < d)) return;
    const int32_t cross = int32_t(uint32_t(Mul12Wide(b, c)) - uint32_t(Mul12Wide(d, a)));
    if (cross <= 0) return;
    const int32_t curvature = int32_t(uint32_t(Mul12Wide(Mul12Wide(b, b), c)) - uint32_t(Mul12Wide(Mul12Wide(d, d), a)));
    if (curvature > 0) return;
    const int32_t gainSquared = Div12Shift(Mul12Wide(Mul12Wide(b, d), int32_t(uint32_t(d) - uint32_t(b))), cross, 0);
    const int32_t offset = Div12Shift(curvature, int32_t(uint32_t(cross) << 1), 0);
    gain = SquareRoot(gainSquared, 6);
    const int32_t kneeValue = Div12Shift(Mul12Wide(offset, offset), gainSquared, 0);
    knee = int32_t(0u - uint32_t(kneeValue));
    base = Mul12Wide(int32_t(0u - uint32_t(gain)), SquareRoot(kneeValue, 6));
}

void SetupSuspension(AxleSuspension& block, const SuspensionParams& p, uint8_t damperScaleDivisor, const SetupConstants& constants) { // 0x80030F94
    block.springRate = MulLow(SpringRateFromCode(p.springCode, constants), 0xF50);
    const int32_t velocityScale = Div(0x64000, int32_t(damperScaleDivisor)); // MIPS div: a zero divisor gives -1
    auto percent = [](uint8_t v) { return int16_t((uint32_t(v) << 12) / 100); };
    auto force = [](uint8_t v) { return ScaleTowardsZero(v, 0xC4004u, 12); }; // v * 196.0
    const int16_t bumpLow = int16_t(Mul12Wide(velocityScale, percent(p.bumpLowKnee)));
    const int16_t bumpHigh = int16_t(Mul12Wide(velocityScale, percent(p.bumpHighKnee)));
    SetupDamperCurve(block.bumpKnee, block.bumpBase, block.bumpGain, bumpLow, force(p.bumpLowForce), bumpHigh, force(p.bumpHighForce));
    const int16_t reboundLow = int16_t(Mul12Wide(velocityScale, percent(p.reboundLowKnee)));
    const int16_t reboundHigh = int16_t(Mul12Wide(velocityScale, percent(p.reboundHighKnee)));
    SetupDamperCurve(block.reboundKnee, block.reboundBase, block.reboundGain, reboundLow, force(p.reboundLowForce), reboundHigh,
                     force(p.reboundHighForce));
    block.antiRollRate = ScaleTowardsZero(p.antiRollCode, 0x188008u, 12); // v * 392.0
    block.bumpStopRate = ScaleTowardsZero(p.bumpStopCode, 0xF50050u, 12); // v * 3920.0 (wraps for codes >= 134)
    block.unsprungMass = int16_t(uint16_t(p.unsprungMass));
    block.staticLoad = int16_t(ScaleTowardsZero(p.unsprungMass, 0x27334u, 14)); // v * 9.8
}

void SetupSuspensionTravel(CarBody& body, AxleSuspension& block, const CarParams& params, int axle) { // 0x800312FC
    // The static deflection: sprung axle weight / 2 / spring rate, negative.
    if (block.springRate != 0) {
        const int32_t axleWeight = body.sprungWeightPerAxle[axle];
        block.staticDeflection = int16_t(0u - uint32_t(Div12Shift(HalfTowardsZero(axleWeight), block.springRate, 0)));
    }
    const uint32_t wheelRadius = uint16_t(Drivetrain(body).wheelRadius[axle]);
    const uint32_t rideHeight = uint16_t(body.rideHeight[axle]);
    const uint32_t deflection = uint16_t(block.staticDeflection);
    const uint32_t reference = deflection + (wheelRadius - rideHeight);
    block.rideReference = int16_t(reference);
    const int32_t floor = int32_t(reference - wheelRadius);
    block.travelFloor = int16_t(floor);
    int32_t bump = int32_t((uint32_t(params.bumpTravelMm[axle]) << 12) / 1000);
    const int32_t ride = body.rideHeight[axle];
    if (ride < bump) bump = ride;
    block.bumpStopStart = int16_t(floor + bump);
    const int32_t droop = int32_t((uint32_t(params.droopTravelMm[axle]) << 12) / 1000);
    block.droopLimit = int16_t(int32_t(uint16_t(block.travelFloor)) + int32_t(rideHeight) + droop);
    const int32_t staticTravel = Div12Shift(block.staticLoad, block.springRate, 0);
    if (staticTravel < block.droopLimit) block.droopLimit = int16_t(staticTravel);
}

// ---------------------------------------------------------------- engine

namespace {
void SetupTurbo(const CarParams& params, EngineBlock& engine) { // 0x800750C8
    bool haveTurbo = false;
    if (params.turboBoostCap10 != 0) {
        engine.boostCap = int16_t((uint32_t(params.turboBoostCap10) << 12) / 10);
        haveTurbo = params.turboSpoolRpm100 != 0 && params.turboBoost10 != 0 && params.turboSpoolRate10 != 0;
    }
    if (!haveTurbo) engine.boostCap = 0;
    if (engine.boostCap == 0) {
        engine.turboModel[0] = 0;
        engine.turboBoost[0] = 0;
        engine.turboSpoolRate[0] = 0;
        engine.turboCoefficient[0] = 0;
        engine.turboSpoolRpm[0] = 0;
        engine.turboModel[1] = 0;
        engine.turboBoost[1] = 0;
        engine.turboSpoolRate[1] = 0;
        engine.turboCoefficient[1] = 0;
        engine.turboSpoolRpm[1] = 0;
        return;
    }
    engine.turboModel[0] = params.turboModel[0] > 2 ? 0 : params.turboModel[0];
    engine.turboBoost[0] = int16_t((uint32_t(params.turboBoost10) << 12) / 10);
    engine.turboSpoolRate[0] = int32_t((uint32_t(params.turboSpoolRate10) << 12) / 10);
    const int32_t spoolRpm = int16_t(int32_t(params.turboSpoolRpm100) * 100);
    engine.turboSpoolRpm[0] = int16_t(uint16_t(params.turboSpoolRpm100) * 100);
    engine.turboCoefficient[0] = Div12Shift(engine.turboBoost[0], MulLow(spoolRpm, spoolRpm) >> 12, 0);
    engine.turboModel[1] = 0;
    engine.turboBoost[1] = 0;
    engine.turboSpoolRate[1] = 0;
    engine.turboSpoolRpm[1] = 0;
    engine.turboCoefficient[1] = 0;
    const uint32_t second = uint32_t(params.turboBoost10Second) + params.turboSpoolRate10Second + params.turboSpoolRpm100Second;
    if (second != 0 && params.turboBoost10Second != 0 && params.turboSpoolRpm100Second != 0 && params.turboSpoolRate10Second != 0) {
        engine.turboModel[1] = params.turboModel[1] > 2 ? 0 : params.turboModel[1];
        engine.turboBoost[1] = int16_t((uint32_t(params.turboBoost10Second) << 12) / 10);
        engine.turboSpoolRate[1] = int32_t((uint32_t(params.turboSpoolRate10Second) << 12) / 10);
        const int32_t spoolRpm2 = int16_t(int32_t(params.turboSpoolRpm100Second) * 100);
        engine.turboSpoolRpm[1] = int16_t(uint16_t(params.turboSpoolRpm100Second) * 100);
        engine.turboCoefficient[1] = Div12Shift(engine.turboBoost[1], MulLow(spoolRpm2, spoolRpm2) >> 12, 0);
    }
}
} // namespace

void SetupEngine(CarBody& body, CarParams& params, uint32_t bodyToken) { // 0x80075328
    EngineBlock& engine = Engine(body);
    const int32_t count = params.torquePointCount;
    engine.idleRpm = uint16_t(uint32_t(params.idleRpm10) * 10);
    uint32_t revLimit100 = params.revLimitRpm100;
    if (revLimit100 == 0) revLimit100 = params.torqueRpm100[count - 1]; // byte 0x33 + count
    engine.revLimitRpm = uint16_t(revLimit100 * 100);
    const int32_t revLimitFloor = int32_t(uint32_t(params.upshiftRpm100) * 100 + 500);
    if (int32_t(engine.revLimitRpm) < revLimitFloor) engine.revLimitRpm = uint16_t(revLimitFloor);
    if (params.torque[count - 1] == 0) params.torque[count - 1] = params.torque[count - 2]; // patches the record

    uint32_t power = params.powerPercent;
    if (power == 0) power = 100;
    uint32_t powerTop = params.powerPercentTop;
    if (powerTop == 0) powerTop = 1000;
    const int32_t powerBase = int32_t(power * 10);
    const int32_t powerSpan = int32_t(powerTop - power * 10);
    const int32_t firstRpm100 = params.torqueRpm100[0], lastRpm100 = params.torqueRpm100[count - 1];
    for (int32_t i = 0; i < count; i++) {
        uint32_t multiplier = params.torqueMultiplier1000;
        if (multiplier == 0) multiplier = 1000;
        else if (multiplier < 256) multiplier *= 10;
        const int32_t rpm100 = params.torqueRpm100[i];
        const int32_t blend = Div(MulLow(powerSpan, rpm100 - firstRpm100), lastRpm100 - firstRpm100);
        engine.xs[i] = ((rpm100 * 25) << 14) / 60; // rpm * 4096 / 60
        const int32_t powerAtRpm = MulLow(int32_t(multiplier), powerBase + blend);
        const int32_t torque = ScaleTowardsZero(params.torque[i], 0x27334u, 12); // raw * 39.2
        engine.ys[i] = int32_t(uint64_t(Div64(int64_t(torque) * int64_t(powerAtRpm), 100000000)));
    }
    engine.xsToken = bodyToken + kEngineBlockOffset + uint32_t(offsetof(EngineBlock, xs));
    engine.ysToken = bodyToken + kEngineBlockOffset + uint32_t(offsetof(EngineBlock, ys));
    engine.count = uint16_t(count);
    // Divided by the engine SPEED of the last sample (xs), not its torque.
    engine.engineBrakeCoefficient = Div12Shift(ScaleTowardsZero(params.engineBrake, 0x27334u, 12), engine.xs[count - 1], 0);
    SetupTurbo(params, engine);
    for (int32_t i = 0; i < int16_t(engine.count); i++) {
        const int16_t rpm = int16_t(int32_t(params.torqueRpm100[i]) * 100);
        const int32_t capped = BoostMultiplier(body, rpm, rpm); // 0x80075074
        const int32_t sum = TurboBoostSum(body, rpm, rpm);     // 0x80074F24
        engine.ys[i] = Mul12Wide(Div12Shift(capped, sum, 0), engine.ys[i]);
    }
}

// ---------------------------------------------------------------- gearbox

void GenerateGearRatios(CarParams& params, int32_t position, bool dirtCourse, const SetupConstants& constants) { // 0x80074B38
    uint8_t driveType = 0, primaryAxle = 0;
    int16_t split = 0, lock = 0;
    SetupDriveLayout(params, dirtCourse, driveType, primaryAxle, split, lock);
    int16_t total = 0, rim = 0, tyre = 0;
    WheelRadius(params, primaryAxle, total, rim, tyre);
    const int32_t gears = params.gearCount;
    const int32_t row = gears < 5 ? 5 : gears;
    const uint8_t* table = constants.gearAutoTable + row * 6; // 0x800923E2: 6 percentages per gear count
    // `position` is a top speed in km/h x10 (the gearAutoFinal slider): the top gear is chosen so that the rev limit
    // is reached at that speed, the lower gears follow a geometric progression whose steps come from the table row
    // for the gear count, blended by (position - 150) / 250 between the two column sets.
    const int32_t blend = ((position - 150) << 12) / 250;
    const int32_t final12 = (int32_t(params.finalDrive) << 12) / 1000;
    const int32_t circumference = MulLow(25736, total) >> 12; // 2 pi * radius
    const int32_t revLimitDistance = MulLow(int32_t(params.revLimitRpm100), circumference * 100); // rpm * circumference
    const int32_t topSpeed = MulLow(position, 1138);                                             // 1/4096 m/s per 0.1 km/h * 10
    const int32_t perMinute = Div12Shift(revLimitDistance, topSpeed, 0) / 60;
    int32_t ratio = Div12Shift(perMinute, final12, 0);
    auto inverse = [](uint8_t percent) { return Div(0xC8000, int32_t(percent)); }; // 200.0 / percent
    const int32_t t1 = inverse(table[1]);
    int32_t stepLow = t1 + Mul12Wide(blend, inverse(table[0]) - t1);
    const int32_t t3 = inverse(table[3]);
    const int32_t stepGrowth = t3 + Mul12Wide(blend, inverse(table[2]) - t3);
    const int32_t t5 = inverse(table[5]);
    const int32_t topStep = t5 + Mul12Wide(blend, inverse(table[4]) - t5);
    for (int32_t gear = gears; gear > 0; gear--) {
        if (gear == 1) ratio = Mul12Wide(ratio, topStep);
        params.gearRatio[gear] = int16_t(Mul12Wide(ratio, 1000));
        ratio = Mul12Wide(ratio, stepLow);
        stepLow = Mul12Wide(stepLow, stepGrowth);
    }
    int32_t reverse = int32_t(params.gearRatio[1]) * 3 + params.gearRatio[2];
    if (reverse < 0) reverse += 3;
    params.gearRatio[0] = int16_t(reverse >> 2);
}

namespace {
// 0x80034740: generates the gear ratios when the auto-set is on. Returns whether it did.
bool AutoSetGears(CarParams& params, bool dirtCourse, const SetupConstants& constants) {
    if (params.gearAutoSet == 0 || params.gearAutoFinal == 0) return false;
    const int32_t position = params.gearAutoFinal == 0xFF ? 0xFF : int32_t(params.gearAutoFinal) * 10;
    for (int i = 0; i < 8; i++) params.gearRatio[i] = -1;
    GenerateGearRatios(params, position, dirtCourse, constants);
    return true;
}
} // namespace

void SetupDrivetrain(CarBody& body, CarParams& params, bool dirtCourse, const SetupConstants& constants) { // 0x800347C4
    DrivetrainBlock& d = Drivetrain(body);
    EngineBlock& engine = Engine(body);
    SetupDriveLayout(params, dirtCourse, d.driveType, d.primaryAxle, d.centreSplit, d.centreLockTorque);
    for (int axle = 0; axle < 2; axle++) {
        const int8_t code = params.diffTypeCode[axle];
        uint8_t type = uint8_t(code);
        for (int i = 0; i < 8; i++)
            if (constants.diffTypeCodes[i] == code) { type = uint8_t(i); break; }
        d.axleDiffType[axle] = type;
    }
    // Default differential types by layout: the driven axle gets type 2 unless set, the other axle type 0.
    if (d.driveType == 1) {
        if (d.axleDiffType[0] == 0) d.axleDiffType[0] = 2;
        if (d.axleDiffType[1] != 0) d.axleDiffType[1] = 0;
    } else if (d.driveType == 0) {
        if (d.axleDiffType[0] != 0) d.axleDiffType[0] = 0;
        if (d.axleDiffType[1] == 0) d.axleDiffType[1] = 2;
    } else if (d.driveType <= 6) {
        if (d.axleDiffType[0] == 0) d.axleDiffType[0] = 2;
        if (d.axleDiffType[1] == 0) d.axleDiffType[1] = 2;
    }
    for (int axle = 0; axle < 2; axle++) { // jump table 0x80046BE4
        const uint32_t initial = params.diffInitialTorque[axle], accel = params.diffAccel[axle], decel = params.diffDecel[axle];
        switch (d.axleDiffType[axle]) {
        case 0: case 1: case 2:
            d.axleDiffMinTorque[axle] = 0;
            d.axleDiffAccelRatio[axle] = 0;
            d.axleDiffDecelRatio[axle] = 0;
            break;
        case 3: case 4: case 6:
            d.axleDiffMinTorque[axle] = ScaleTowardsZero(initial, 0x27334u, 12); // kgf m -> N m * 4
            d.axleDiffAccelRatio[axle] = int32_t((accel << 12) / 100);
            d.axleDiffDecelRatio[axle] = int32_t((decel << 12) / 100);
            break;
        case 5: {
            const int32_t v = int32_t(initial << 12);
            d.axleDiffMinTorque[axle] = Mul12Wide(v, v);
            d.axleDiffDecelRatio[axle] = 0;
            d.axleDiffAccelRatio[axle] = int32_t((accel * 0x27334u) / 0xA000u);
            break;
        }
        case 7:
            d.axleDiffMinTorque[axle] = int32_t((initial << 12) / 100);
            d.axleDiffAccelRatio[axle] = int32_t((accel << 12) / 100);
            d.axleDiffDecelRatio[axle] = int32_t((decel << 12) / 100);
            break;
        default: break;
        }
    }
    AutoSetGears(params, dirtCourse, constants);
    d.gearCount = params.gearCount;
    const int32_t gears = d.gearCount;
    for (int32_t gear = 0; gear <= gears; gear++) {
        const int64_t product = int64_t(params.gearRatio[gear]) * int64_t(int32_t(params.finalDrive) << 12);
        d.gearRatio[gear] = int32_t(uint64_t(Div64(product, 1000000)));
    }
    for (int axle = 0; axle < 2; axle++) WheelRadius(params, axle, d.wheelRadius[axle], d.rimRadius[axle], d.tyreHeight[axle]);
    if (d.wheelRadius[0] != 0) d.inverseWheelRadius[0] = int16_t(Div(0x1000000, d.wheelRadius[0]));
    if (d.wheelRadius[1] != 0) d.inverseWheelRadius[1] = int16_t(Div(0x1000000, d.wheelRadius[1]));
    int32_t wheelInertia[2] = {params.wheelInertia[0], params.wheelInertia[1]};
    for (int axle = 0; axle < 2; axle++) {
        if (wheelInertia[axle] == 0) continue;
        d.axleInvInertia[axle] = Div12Shift(d.wheelRadius[axle], wheelInertia[axle] << 4, 0);
        d.wheelInertiaFactor[axle] = int16_t(Mul12Wide(wheelInertia[axle], Mul12Wide(d.inverseWheelRadius[axle], d.inverseWheelRadius[axle])));
    }
    if (params.engineInertia != 0) {
        d.engineInertiaRef = int16_t((uint32_t(params.engineInertia) << 6) / 100);
        d.engineInvInertia = Div(0x0FEB0000, int32_t(uint32_t(params.engineInertia) << 2));
    }
    int32_t axleInertia[2];
    for (int axle = 0; axle < 2; axle++) {
        int32_t v = int32_t(uint32_t(params.axleInertiaCode[axle]) * 16);
        v = MulLow(v, params.finalDrive) / 1000;
        v += wheelInertia[axle] * 32;
        axleInertia[axle] = v;
        d.axleInertia[axle] = v;
    }
    d.centreDiffStiffness = Div12Shift(0x1000, axleInertia[0] + axleInertia[1], 0);
    d.downshiftFloorRpm = uint16_t(uint32_t(params.downshiftFloorRpm10) * 10);
    d.upshiftRpm = uint16_t(uint32_t(params.upshiftRpm100) * 100);
    int32_t peakTorque = 0;
    for (int32_t i = 0; i < int16_t(engine.count); i++) {
        const int32_t torque = Interpolate(engine.xs, engine.ys, engine.count, engine.xs[i]);
        if (peakTorque < torque) peakTorque = torque;
    }
    d.overrunClutchTorque = int16_t(MulLow(peakTorque, 15) / 10);
    d.lockedClutchTorque = int16_t(ScaleTowardsZero(params.clutchCode, 0x27334u, 12));
    const int32_t circumference = MulLow(d.wheelRadius[d.primaryAxle], 25736) >> 12; // 2 pi r
    for (int32_t gear = 0; gear <= gears; gear++) d.revsPerSpeed[gear] = Div12Shift(d.gearRatio[gear], circumference, 0);
    if (gears == 2) {
        d.gearRatio[2] = Div(int32_t(params.gearRatio[2]) << 12, params.gearRatio[1]);
        d.downshiftFloorRpm = engine.idleRpm;
    } else if (int32_t(d.downshiftFloorRpm) - 500 < int32_t(engine.idleRpm)) {
        d.downshiftFloorRpm = uint16_t(engine.idleRpm + 500);
    }
    if (gears == 2) {
        // Two-speed box: the single downshift point is the rpm of the largest torque sample.
        int32_t best = 0, bestIndex = int32_t(int16_t(engine.count)) - 1;
        for (int32_t i = 0; i < int16_t(engine.count); i++)
            if (best < engine.ys[i]) { best = engine.ys[i]; bestIndex = i; }
        d.downshiftRpm[0] = uint16_t(uint32_t(params.torqueRpm100[bestIndex]) * 100);
    } else {
        for (int32_t gear = 1; gear < gears; gear++) {
            const int32_t step = Div12Shift(d.gearRatio[gear + 1], d.gearRatio[gear], 0);
            int32_t rpm = MulLow(Mul12Wide(int32_t((uint32_t(d.upshiftRpm) << 12) / 60), step), 60);
            if (rpm < 0) rpm += 0xFFF;
            int16_t target;
            if (int32_t(d.downshiftFloorRpm) + 1000 < (rpm >> 12)) target = int16_t((rpm >> 12) - 500);
            else target = int16_t(d.downshiftFloorRpm + 500);
            d.downshiftRpm[gear - 1] = uint16_t(target);
        }
    }
}

// ---------------------------------------------------------------- tyre curves

void SetupSlipRatioCurves(AxleTyreBlock& block, uint8_t negCountByte, uint8_t negXs[6], uint8_t negYs[6], uint8_t negYs2[6], uint8_t posXs[6],
                          uint8_t posYs[6], uint8_t posYs2[6], uint8_t posCount, uint32_t blockToken) { // 0x800314DC
    const int32_t negCount = negCountByte;
    negXs[0] = 0;
    negYs[0] = 0;
    posXs[0] = 0;
    posYs[0] = 0;
    negYs2[0] = posYs2[0];
    block.peakSlipRatioNeg = 0;
    int32_t peak = 0;
    for (int32_t i = 0; i < negCount; i++) {
        const int32_t k = negCount - (i + 1);
        block.slipRatioXs[k] = int16_t((int32_t(0u - uint32_t(negXs[i])) << 12) / 255);
        const int32_t y = int32_t((uint32_t(negYs[i]) << 12) / 200);
        block.slipRatioYs[k] = int16_t(y);
        if (peak < int16_t(y)) {
            block.peakSlipRatioNeg = block.slipRatioXs[k];
            peak = int16_t(y);
        }
        block.slipRatioGripYs[k] = int16_t((uint32_t(negYs2[i]) << 12) / 200);
    }
    block.peakSlipRatioPos = 0;
    peak = 0;
    for (int32_t i = 0; i < int32_t(posCount); i++) {
        const int32_t k = negCount + i - 1;
        block.slipRatioXs[k] = int16_t((uint32_t(posXs[i]) << 12) / 255);
        const int32_t y = int32_t((uint32_t(posYs[i]) << 12) / 200);
        block.slipRatioYs[k] = int16_t(y);
        if (peak < int16_t(y)) {
            block.peakSlipRatioPos = block.slipRatioXs[k];
            peak = int16_t(y);
        }
        block.slipRatioGripYs[k] = int16_t((uint32_t(posYs2[i]) << 12) / 200);
    }
    const uint16_t count = uint16_t(negCount + posCount - 1);
    block.slipRatioYsToken = blockToken + uint32_t(offsetof(AxleTyreBlock, slipRatioYs));
    block.slipRatioCount = count;
    block.slipRatioXsToken = blockToken + uint32_t(offsetof(AxleTyreBlock, slipRatioXs));
    block.slipRatioGripCount = count;
    block.slipRatioGripXsToken = blockToken + uint32_t(offsetof(AxleTyreBlock, slipRatioXs));
    block.slipRatioGripYsToken = blockToken + uint32_t(offsetof(AxleTyreBlock, slipRatioGripYs));
}

void SetupSlipAngleCurve(AxleTyreBlock& block, uint8_t xs[8], uint8_t ys[8], uint8_t count, uint32_t blockToken) { // 0x800316F4
    xs[0] = 0;
    ys[0] = 0;
    for (int32_t i = 0; i < int32_t(count); i++) {
        block.slipAngleXs[i] = int16_t((uint32_t(xs[i]) * 0x400u) / 255u);
        block.slipAngleYs[i] = int16_t((uint32_t(ys[i]) << 12) / 200u);
    }
    block.slipAngleCount = count;
    block.slipAngleXsToken = blockToken + uint32_t(offsetof(AxleTyreBlock, slipAngleXs));
    block.slipAngleYsToken = blockToken + uint32_t(offsetof(AxleTyreBlock, slipAngleYs));
}

void SetupLoadAndCamberCurves(AxleTyreBlock& block, uint8_t loadXs[4], const uint8_t loadYs[4], uint8_t loadCount, uint8_t camberXs[4],
                              const uint8_t camberYs[4], uint8_t camberCount, int16_t rimRadius, int32_t gripFactor, uint32_t blockToken) { // 0x80031794
    loadXs[0] = 0;
    for (int32_t i = 0; i < int32_t(loadCount); i++) {
        const int32_t loadUnit = ScaleTowardsZero(loadXs[i], 0x310010u, 12); // v * 784.0
        const int32_t load = MulLow(rimRadius, loadUnit) >> 12;
        block.loadGripXs[i] = load;
        block.loadGripYs[i] = MulLow(Mul12Wide(gripFactor, load), loadYs[i]) / 200;
    }
    block.loadGripCount = loadCount;
    block.loadGripXsToken = blockToken + uint32_t(offsetof(AxleTyreBlock, loadGripXs));
    block.loadGripYsToken = blockToken + uint32_t(offsetof(AxleTyreBlock, loadGripYs));
    camberXs[0] = 0;
    for (int32_t i = 0; i < int32_t(camberCount); i++) {
        block.camberGripXs[i] = int16_t((uint32_t(camberXs[i]) * 0xE3u) / 255u);
        block.camberGripYs[i] = int16_t((uint32_t(camberYs[i]) << 12) / 200u);
    }
    block.camberGripCount = camberCount;
    block.camberGripXsToken = blockToken + uint32_t(offsetof(AxleTyreBlock, camberGripXs));
    block.camberGripYsToken = blockToken + uint32_t(offsetof(AxleTyreBlock, camberGripYs));
}

int32_t PeakSlipAngle(const CarBody& body) { // 0x8003B598
    const AxleTyreBlock& front = Tyres(body, 0);
    const CurveS16 curve{front.slipAngleXs, front.slipAngleYs, front.slipAngleCount};
    int32_t best = 0, bestDegrees = 90;
    for (int32_t degrees = -45; degrees < 46; degrees++) {
        const int32_t angle = (degrees * 0x1000) / 360;
        const int32_t force = SymmetricCurve(curve, angle); // 0x80039F4C
        const int32_t along = Mul12Wide(Cos(uint32_t(angle)), force);
        if (best < along) {
            best = along;
            bestDegrees = degrees;
        }
    }
    return (bestDegrees * 0x1000) / 360;
}

// ---------------------------------------------------------------- top speed

namespace {
// 0x80030950: sweeps the speed in 1 km/h steps through the gears (shifting at the upshift rpm) and keeps the
// highest speed at which the drive force still exceeds drag, rolling resistance and the downforce-induced terms.
void EstimateTopSpeed(CarBody& body, const SetupConstants& constants) {
    DrivetrainBlock& d = Drivetrain(body);
    const EngineBlock& engine = Engine(body);
    if (d.gearCount == 1) {
        body.topSpeed = 0x32028;
        return;
    }
    const int32_t limitKmh = Mul12ShiftFloor(body.topSpeed, 0x3999, 12);
    const int32_t step = limitKmh / 500 + 1;
    int32_t gear = 1, kmh = 0, accumulator = 0;
    const int32_t weight = body.weight;
    while (kmh < limitKmh) {
        const int32_t speed = int32_t((uint32_t(accumulator) * 64u - uint32_t(kmh) * 7u) * 2u); // = kmh * step * 1138 (1/4096 m/s per km/h)
        const int32_t engineSpeed = Mul12Wide(speed, d.revsPerSpeed[gear]);
        int32_t rpm = MulLow(engineSpeed, 60);
        if (rpm < 0) rpm += 0xFFF;
        rpm >>= 12;
        const int32_t torque = Interpolate(engine.xs, engine.ys, engine.count, engineSpeed);
        const int32_t driveForce = Mul12Wide(torque, Mul12Wide(0x6488, d.revsPerSpeed[gear]));
        int32_t speedSquared = (speed < 0 ? int32_t(0u - uint32_t(speed)) : speed) >> 5;
        speedSquared = MulLow(speedSquared, speedSquared) >> 12;
        const int32_t downFront = MulLow(body.downforce[0], speedSquared) >> 12;
        const int32_t downRear = MulLow(body.downforce[1], speedSquared) >> 12;
        int32_t resistance = Interpolate(constants.rollingResistance.xs, constants.rollingResistance.ys, constants.rollingResistance.count, speed);
        resistance = Mul12Wide(constants.rollingResistanceGain, resistance);
        resistance = Mul12Wide(int32_t(uint32_t(weight) + uint32_t(downFront) + uint32_t(downRear)), resistance);
        resistance = int32_t(uint32_t(resistance) + uint32_t(MulLow(body.dragCoefficient, speedSquared) >> 12));
        if (resistance < driveForce) body.topSpeed = speed;
        if (gear < int32_t(d.gearCount) && int32_t(d.upshiftRpm) < rpm) gear++;
        accumulator += step * 9;
        kmh += step;
    }
}
} // namespace

void SetupTopSpeed(CarBody& body, const SetupConstants& constants) { // 0x80030C5C
    const DrivetrainBlock& d = Drivetrain(body);
    const EngineBlock& engine = Engine(body);
    const int32_t revLimitSpeed = int32_t(uint32_t(engine.revLimitRpm) << 12) / 60;
    body.topSpeed = Div12Shift(revLimitSpeed, d.revsPerSpeed[d.gearCount], 0);
    const int32_t mass = body.mass;
    const int32_t brakeFront = Mul12Wide(d.brakeTorque[0], d.inverseWheelRadius[0]);
    const int32_t brakeRear = Mul12Wide(d.brakeTorque[1], d.inverseWheelRadius[1]);
    body.brakeBalanceSpeed = Div12Shift(mass, int32_t(uint32_t(brakeFront + brakeRear) << 2), 0);
    EstimateTopSpeed(body, constants);
}

// ---------------------------------------------------------------- the per-car setup

void SetupCar(CarBody& body, const CarSetupInputs& inputs) { // 0x800319A8
    CarParams& p = *inputs.params;
    const SetupConstants& constants = inputs.constants;
    body.byte1C = inputs.byte1C; // 0x80030D10
    if (p.driveType > 4) p.driveType = 0;

    // ---- geometry (mm -> 1/4096 m)
    const uint32_t frontPercent = p.frontWeightPercent;
    body.frontExtent = (int32_t(p.frontLength) << 12) / 1000;
    body.rearExtent = (int32_t(p.rearLength) << 12) / 1000;
    const int32_t wheelbase = (int32_t(p.wheelbase) << 12) / 1000;
    body.wheelbase = int16_t(wheelbase);
    body.frontWeightFraction = int16_t((frontPercent << 12) / 100);
    const int32_t rearAxle = MulLow(int16_t(wheelbase), int32_t(frontPercent)) / 100;
    body.axleOffset[0] = int16_t(wheelbase - rearAxle);
    body.axleOffset[1] = int16_t(rearAxle);
    body.rearWeightFraction = int16_t(0x1000 - body.frontWeightFraction);
    const int32_t cgOffset = HalfTowardsZero(body.axleOffset[1] - body.axleOffset[0]);
    body.cgOffset = int16_t(cgOffset);
    body.frontExtent -= int16_t(cgOffset);
    body.rearExtent += body.cgOffset;
    body.width = int16_t((int32_t(p.width) << 12) / 1000);
    body.halfTrack[0] = int16_t((int32_t(p.frontTrack) << 12) / 2000);
    const int32_t rearHalfTrack = (int32_t(p.rearTrack) << 12) / 2000;
    body.halfTrack[1] = int16_t(rearHalfTrack);
    body.meanTrack = int16_t(MulLow(body.halfTrack[0], int32_t(frontPercent)) / 50 + MulLow(int16_t(rearHalfTrack), int32_t(100 - frontPercent)) / 50);
    body.frontExtent -= 0x80;
    body.height = int16_t((int32_t(p.height) << 12) / 1000);
    body.rearExtent -= 0x80;
    body.width = int16_t(body.width - int16_t(int32_t(body.width) / 20)); // 95 % of the body width

    // ---- engine, drivetrain
    SetupEngine(body, p, inputs.bodyToken);
    SetupDrivetrain(body, p, inputs.dirtCourse, constants);
    DrivetrainBlock& d = Drivetrain(body);
    const uint8_t driveClass = DriveClassOf(d.driveType);
    body.driveClass = driveClass;

    // ---- tyre curves
    const uint32_t tyreToken0 = inputs.bodyToken + kAxleTyreBlockOffset, tyreToken1 = tyreToken0 + sizeof(AxleTyreBlock);
    SetupSlipRatioCurves(Tyres(body, 0), p.slipRatioNegCount, p.slipRatioNegXs, p.slipRatioNegYs, p.slipRatioNegYs2, p.slipRatioPosXs,
                         p.slipRatioPosYs, p.slipRatioPosYs2, p.slipRatioPosCount, tyreToken0);
    SetupSlipRatioCurves(Tyres(body, 1), p.slipRatioNegCountRear, p.slipRatioNegXsRear, p.slipRatioNegYsRear, p.slipRatioNegYs2Rear,
                         p.slipRatioPosXsRear, p.slipRatioPosYsRear, p.slipRatioPosYs2Rear, p.slipRatioPosCountRear, tyreToken1);
    SetupSlipAngleCurve(Tyres(body, 0), p.slipAngleXs, p.slipAngleYs, p.slipAngleCount, tyreToken0);
    SetupSlipAngleCurve(Tyres(body, 1), p.slipAngleXsRear, p.slipAngleYsRear, p.slipAngleCountRear, tyreToken1);
    const int32_t peakSlipAngle = PeakSlipAngle(body);
    body.peakSlipAngle = int16_t(peakSlipAngle);
    if (int16_t(peakSlipAngle) >= 342) body.peakSlipAngle = 0x155;

    // ---- per-surface grip
    for (uint32_t i = 0; i < 8; i++) {
        const uint32_t percent = p.surfaceGripPercent[i];
        if (percent == 0) {
            body.surfaceGrip[i] = 0x100;
            body.inverseSurfaceGrip[i] = 0x100;
        } else {
            body.surfaceGrip[i] = int16_t((percent << 8) / 100);
            body.inverseSurfaceGrip[i] = int16_t(Div(0x6400, int32_t(percent)));
        }
    }

    // ---- compound grip: load gains, then the grip factors (front, rear)
    int32_t grip[2] = {p.tyreGripPercent[0], p.tyreGripPercent[1]};
    for (int i = 0; i < 2; i++)
        if (grip[i] == 0) grip[i] = 100;
    for (uint32_t i = 0; i < 2; i++) {
        const int32_t over = grip[i] - 175;
        int32_t scaled = (over << 12) / (grip[i] < 175 ? 100 : 50) + 0x1000;
        int32_t scaled8 = scaled << 3;
        if (scaled < 0x200) {
            scaled = 0x200;
            scaled8 = 0x1000;
        }
        body.gripLoadGain[i] = int16_t(scaled8 / 100);
        body.gripLoadGain2[i] = int16_t(((scaled8 + scaled) * 4 - scaled) / 100);
    }
    for (int i = 0; i < 2; i++) grip[i] = (grip[i] << 12) / 100;
    int32_t modifier[2] = {p.tyreGripModifier[0], p.tyreGripModifier[1]};
    for (int i = 0; i < 2; i++) {
        if (modifier[i] == 0) modifier[i] = 100;
        grip[i] = MulLow(modifier[i], grip[i]) / 100;
    }
    if (inputs.controlMode == 2) { // AI cars: the class table of 0x801C98A4
        modifier[0] = constants.aiGripPercent[driveClass];
        modifier[1] = constants.aiGripPercent[driveClass + 4];
        for (int i = 0; i < 2; i++) {
            if (modifier[i] == 0) modifier[i] = 100;
            grip[i] = MulLow(modifier[i], grip[i]) / 100;
        }
    }
    const int32_t frontWeight = constants.classTuning[driveClass].frontGripWeight;
    const int32_t gripWeight = Mul12Wide(Dot2Shift12(frontWeight, grip[0], 0x1000 - frontWeight, grip[1], 0), 0x9CCD);
    body.inverseGripWeight = int16_t(Div12Shift(0x1000, gripWeight, 0));
    const AxleTyreBlock& front = Tyres(body, 0);
    const AxleTyreBlock& rear = Tyres(body, 1);
    const CurveS16 frontGrip{front.slipRatioXs, front.slipRatioGripYs, front.slipRatioGripCount};
    const CurveS16 rearGrip{rear.slipRatioXs, rear.slipRatioGripYs, rear.slipRatioGripCount};
    const int32_t zeroSlipGrip = HalfTowardsZero(int16_t(InterpolateS16(frontGrip, 0)) + int16_t(InterpolateS16(rearGrip, 0)));
    body.gripAtZeroSlip = Mul12Wide(zeroSlipGrip, gripWeight);
    body.inverseGripAtZeroSlip = int16_t(Div12Shift(0x1000, body.gripAtZeroSlip, 0));
    SetupLoadAndCamberCurves(Tyres(body, 0), p.loadGripXs, p.loadGripYs, p.loadGripCount, p.camberGripXs, p.camberGripYs, p.camberGripCount,
                             d.rimRadius[0], grip[0], tyreToken0);
    SetupLoadAndCamberCurves(Tyres(body, 1), p.loadGripXsRear, p.loadGripYsRear, p.loadGripCountRear, p.camberGripXsRear, p.camberGripYsRear,
                             p.camberGripCountRear, d.rimRadius[1], grip[1], tyreToken1);

    // ---- aerodynamics
    const int32_t frontalArea = Mul12Wide(body.width, body.height);
    const int32_t halfDrag = HalfTowardsZero(constants.dragConstant);
    auto aeroTerm = [&](uint8_t coefficient100) { return Mul12Wide(frontalArea, Mul12Wide(halfDrag, int32_t(uint32_t(coefficient100) << 12) / 100)); };
    body.dragCoefficient = aeroTerm(p.dragCoefficient100);
    body.downforce[0] = aeroTerm(p.downforce[0]);
    body.downforce[1] = aeroTerm(p.downforce[1]);
    body.dragCoefficient += (body.downforce[0] + body.downforce[1]) / 10;

    // ---- brakes, driver aids
    d.brakeTorque[0] = ScaleTowardsZero(p.brakeFront, 0x188008u, 12); // v * 392.0
    d.brakeTorque[1] = ScaleTowardsZero(p.brakeRear, 0x188008u, 12);
    d.brakeTorque[2] = ScaleTowardsZero(p.handbrake, 0x188008u, 12);
    if (p.brakeFront != 0 && p.brakeRear != 0)
        for (uint32_t i = 0; i < 2; i++) body.brakeLoadFactor[i] = int16_t(Div12Shift(d.wheelRadius[i], d.brakeTorque[i], 0));
    for (uint32_t i = 0; i < 2; i++) body.absSlipGain[i] = int32_t((uint32_t(p.absGain[i]) << 12) / 10);
    body.asmYawGain = int16_t((uint32_t(p.asmYawGain100) << 12) / 100);
    body.asmYawThreshold = int16_t((uint32_t(p.asmYawThreshold100) << 12) / 100);
    body.tcsFalloffGain = int16_t((uint32_t(p.tcsFalloffGain10) << 12) / 10);
    body.tcsSteerGain = int16_t((uint32_t(p.tcsSteerGain100) << 12) / 100);
    body.tcsGain = int32_t((uint32_t(p.tcsGain10) << 12) / 10);

    // ---- steering
    body.steerLock = int16_t((int32_t(p.steerLockDeg) << 12) / 360);
    body.steerRate = int16_t((int32_t(p.steerRateDeg) << 12) / 360);
    body.steerMaxRate = int16_t((int32_t(p.steerMaxRateCode) * 0xA000) / 360);
    if (inputs.dirtCourse) body.steerMaxRate = 0x5C7;
    const uint32_t steerCount = p.steerLimitCount;
    for (uint32_t i = 0; i < steerCount; i++) {
        body.steerLimitXs[i] = int16_t(uint32_t(p.steerLimitXs[i]) << 7);
        body.steerLimitYs[i] = int16_t((int32_t(p.steerLimitYs[i]) << 12) / 1800);
    }
    body.steerLimitCount = uint16_t(steerCount);
    body.steerLimitXsToken = inputs.bodyToken + uint32_t(offsetof(CarBody, steerLimitXs));
    body.steerLimitYsToken = inputs.bodyToken + uint32_t(offsetof(CarBody, steerLimitYs));
    const int32_t axleSpan = body.axleOffset[0] + body.axleOffset[1];
    const int32_t frontHalfTrack = body.halfTrack[0];
    body.ackermann[0] = int16_t(Atan2(axleSpan, axleSpan - frontHalfTrack) << 3);
    body.ackermann[1] = int16_t(Atan2(axleSpan, axleSpan + frontHalfTrack) << 3);

    // ---- wheel alignment
    body.camber[0] = int16_t(int32_t(0u - uint32_t((int32_t(p.camberFront10) << 12) / 360)) / 10);
    body.camber[1] = int16_t(int32_t(0u - uint32_t((int32_t(p.camberRear10) << 12) / 360)) / 10);
    for (uint32_t i = 0; i < 2; i++) body.toe[i] = int16_t(int32_t(p.toeCode[i]) - 0x80);

    // ---- inertia terms
    if (p.yawInertiaCode != 0) {
        int32_t inertia = int32_t(p.yawInertiaCode) * 400;
        if (inertia < 12000) inertia = 12000; // 0x80030C2C
        body.inverseYawInertia = Div12Shift(0x28C, inertia, 7);
    }
    if (p.pitchInertiaCode != 0) {
        const int32_t inertia = int32_t(p.pitchInertiaCode) * 400;
        body.inversePitchInertia[0] = Div12Shift(body.axleOffset[0], inertia, 4);
        body.inversePitchInertia[1] = Div12Shift(body.axleOffset[1], inertia, 4);
    }
    if (p.rollInertiaCode != 0) {
        int32_t inertia = int32_t(p.rollInertiaCode) * 400;
        if (inertia < 12000) inertia = 12000; // 0x80030C44
        body.inverseRollInertia[0] = Div12Shift(body.halfTrack[0], inertia, 4);
        body.inverseRollInertia[1] = Div12Shift(body.halfTrack[1], inertia, 4);
    }

    // ---- suspension blocks
    for (int axle = 0; axle < 2; axle++) {
        const SuspensionParams* settings = reinterpret_cast<const SuspensionParams*>(p.suspension[axle]);
        SetupSuspension(AxleSuspensionOf(body, size_t(axle)), *settings, p.damperScaleDivisor[axle], constants);
    }

    // ---- CG height and the lever terms
    body.rideHeight[0] = int16_t((uint32_t(p.rideHeightMm[0]) << 12) / 1000);
    const int32_t rearRide = int32_t((uint32_t(p.rideHeightMm[1]) << 12) / 1000);
    body.rideHeight[1] = int16_t(rearRide);
    const int32_t cgHeight = (MulLow(body.rideHeight[0], body.frontWeightFraction) >> 12) + (MulLow(int16_t(rearRide), body.rearWeightFraction) >> 12);
    body.cgHeight = int16_t(cgHeight);
    int32_t clamped = int16_t(cgHeight);
    if (clamped >= 820) clamped = 819;
    // A correction term in mm that grows with the CG height (225 mm -> 0), added when it is non-negative.
    int32_t correction = (((clamped - 225) * 230) / 594 + 50) & 0xFFFF;
    correction = ((correction - 150) << 12) / 1000;
    int32_t leverHeight = int16_t(uint32_t(correction) + uint32_t(cgHeight));
    if (int32_t(uint32_t(correction) << 16) < 0) leverHeight = int16_t(cgHeight);
    body.rollLever = int16_t(HalfTowardsZero(Div12Shift(leverHeight, body.meanTrack, 0)));
    body.pitchLever = int16_t(HalfTowardsZero(Div12Shift(leverHeight, body.wheelbase, 0)));

    // ---- mass
    const int32_t mappedWeight = int16_t(MapWeight(p.weightKg));
    body.mass = int16_t(mappedWeight << 2);
    body.inverseMass = int16_t(Div12Shift(0x1000, (mappedWeight << 18) >> 16, 0));
    body.weight = Mul12Wide(body.mass, 0x9CCD);
    const int32_t unsprung = (int32_t(AxleSuspensionOf(body, 0).unsprungMass) + AxleSuspensionOf(body, 1).unsprungMass) << 1;
    const int32_t sprungMass = body.mass - unsprung;
    if (sprungMass > 0) {
        body.inverseSprungMass = int16_t(Div12Shift(0x1000, sprungMass, 0));
        body.sprungWeightPerAxle[0] = Mul12Wide(Mul12Wide(sprungMass, 0x9CCD), body.frontWeightFraction);
        body.sprungWeightPerAxle[1] = Mul12Wide(Mul12Wide(sprungMass, 0x9CCD), body.rearWeightFraction);
        const int32_t sprungFraction = Div12Shift(sprungMass, body.mass, 0);
        // The original scales the roll lever by the sprung MASS (not the fraction); kept as is.
        body.rollLever = int16_t(MulLow(sprungMass, body.rollLever) >> 12);
        body.pitchLever = int16_t(MulLow(sprungFraction, body.pitchLever) >> 12);
    }
    for (int axle = 0; axle < 2; axle++) SetupSuspensionTravel(body, AxleSuspensionOf(body, size_t(axle)), p, axle);
    SetupTopSpeed(body, constants);
}

// ---------------------------------------------------------------- race start

void PlaceCarOnCourse(CarBody& body, CoursePlacementQueries& course, int32_t chunkHint, int32_t x, int32_t y, int32_t headingSin,
                      int32_t headingCos, uint8_t raceMode) { // 0x80032B0C
    body.chunkIndex = chunkHint;
    const int32_t groundHeight = course.GroundHeight(x, y, body.chunkIndex);
    body.position[0] = x;
    body.position[1] = y;
    body.pitch = 0;
    body.roll = 0;
    body.position[2] = groundHeight + body.cgHeight;
    body.heading = int16_t(Atan2(int32_t(0u - uint32_t(headingSin)), headingCos));
    body.courseDistance = course.CourseDistance(body.chunkIndex, x << 4, groundHeight << 4, y << 4);
    auto rows = [&](int32_t pitch) {
        BuildAttitudeMatrix(body.basis[kRowForward], body.basis[kRowLateral], body.basis[kRowUp], pitch, body.roll, body.heading);
    };
    rows(body.pitch);
    UpdateWheelGeometry(body);
    int32_t offRoadCount = 0;
    for (int wheel = 0; wheel < 4; wheel++) {
        Wheel& w = WheelOf(body, wheel);
        ContactQuery query{};
        query.x = (body.position[0] + w.offsetX) << 4;
        query.height = (body.position[2] + w.offsetHeight) << 4;
        query.y = (body.position[1] + w.offsetY) << 4;
        query.chunkIndex = uint16_t(body.chunkIndex);
        course.Contact(query);
        if (query.surfaceHeight == kNoSurfaceHeight) {
            w.surface = 0;
            w.surfaceAttribute = 0;
            const int32_t height = groundHeight + w.offsetHeight;
            w.surfaceHeightAvg = height;
            w.surfaceHeight = height << 4;
        } else {
            w.surfaceHeightAvg = query.surfaceHeight >> 4;
            w.surfaceHeight = query.surfaceHeight;
            w.surface = query.surface;
            w.surfaceAttribute = query.attribute1;
            offRoadCount += query.attribute2 & 2;
        }
    }
    const bool raceWithReset = raceMode != 6 && raceMode != 3; // 0x80041AB8
    uint8_t flags = body.flags78D;
    flags = (offRoadCount != 0 && raceWithReset) ? uint8_t(flags | 0x10) : uint8_t(flags & 0xEF);
    body.flags78D = flags;
    for (int wheel = 0; wheel < 4; wheel++) {
        Wheel& w = WheelOf(body, wheel);
        const int axle = wheel >> 1;
        const AxleSuspension& suspension = AxleSuspensionOf(body, size_t(axle));
        const int32_t load = HalfTowardsZero(body.sprungWeightPerAxle[axle]) + suspension.staticLoad;
        w.load = load;
        w.gripForce = load;
        w.travel = suspension.staticDeflection;
    }
    body.footprintSet = 1;
    UpdateFootprint(body);
    // 0x80041C78: the other corner set becomes a copy of the current one.
    const int current = body.footprintSet;
    for (int corner = 0; corner < 4; corner++) {
        body.footprint[1 - current][corner][0] = body.footprint[current][corner][0];
        body.footprint[1 - current][corner][1] = body.footprint[current][corner][1];
    }
    UpdateVisualPose(body); // 0x8003E7EC
    body.pitch = body.visualPitch;
    body.roll = body.visualRoll;
    const int32_t rake = Atan2(body.rideHeight[0] - body.rideHeight[1], body.wheelbase);
    const int32_t pitch = int32_t(uint16_t(body.pitch)) + rake;
    body.pitch = int16_t(pitch);
    rows(int16_t(pitch));
    UpdateWheelGeometry(body);
}

void ResetViewState(CarBody& body) { // 0x80032E44
    body.viewYawAccel = 0;
    body.viewYawRate = 0;
    body.viewYawOffset = 0;
    body.viewPitchRate = 0;
    body.viewPitchOffset = 0;
    body.viewYaw = body.heading;
    body.viewPitch = body.pitch;
}

void ResetWheelState(CarBody& body) { // 0x80032AAC
    for (int wheel = 0; wheel < 4; wheel++) {
        Wheel& w = WheelOf(body, wheel);
        w.skidLevel = 0;
        w.skidDelay = 0xFF;
        w.smokeLevel = 0;
        w.rotation = 0;
        w.speedFactor = 0;
        w.slipRatio = 0;
        w.slipRatioGrip = 0;
        w.roughness = 0;
        w.travelVelocity = 0;
        w.springForce = 0;
        w.damperForce = 0;
        w.dustLevel = 0;
        w.steerAngle = 0;
    }
}

void ResetDynamicState(CarBody& body, int32_t rate) { // 0x80032E6C
    for (int wheel = 0; wheel < 4; wheel++) {
        Wheel& w = WheelOf(body, wheel);
        w.slipAngle = 0;
        w.slipBlend = 0;
        w.lateralForce = 0;
        w.driveForce = 0;
        w.brakeForce = 0;
        w.contactForwardSpeed = 0;
        w.contactLateralSpeed = 0;
        w.rimSpeed = 0;
        w.accel = 0;
        w.contactFlags = 0;
        w.slipScale = 0x1000;
    }
    for (uint32_t axle = 0; axle < 2; axle++) {
        body.axleDiffLocked[axle] = 0;
        const uint32_t diffType = body.axleDiffType[axle];
        if (diffType - 3 < 2) body.axleDiffLocked[axle] = 1; // torque-sensing differentials start locked
        body.axleSpeed[axle] = 0;
    }
    body.centreLocked = 0;
    if (uint32_t(body.driveType) - 4 < 2) body.centreLocked = 1; // coupling types 4 / 5 start locked
    body.timeScale = 0x1000;
    body.steerAngle = 0;
    body.steerLimitNeg = 0;
    body.steerLimitPos = 0;
    body.yawRate = 0;
    body.steerAngleRate = 0;
    body.neighbourClass = 0;
    body.draftInput = 0;
    body.draftBlend = 0;
    for (uint32_t i = 0; i < 8; i++) body.neighbourFlags[i] = 0;
    body.velocity[0] = body.velocity[1] = body.velocity[2] = 0;
    body.steerInput = 0;
    body.throttle = 0;
    body.effectiveThrottle = 0;
    body.brake = 0;
    for (int wheel = 0; wheel < 4; wheel++) WheelOf(body, wheel).brakeInput = 0;
    body.blowOffState = -1;
    body.gear = 1;
    body.inputFlag6FD = 0;
    body.aiSlowZone = 0;
    body.aiScriptThrottleOn = 0;
    body.intakeLoad = 0;
    body.engineLoad = 0;
    body.turboSpool[1] = 0;
    body.turboSpool[0] = 0;
    body.turboSpoolMax = 0;
    body.engineTorque = 0;
    body.engineBrakeTorque = 0;
    body.revLimiterActive = 0;
    body.resetState = 0;
    body.resetCounter = 0;
    body.speedReadout = 0;
    body.maxSpeedReadout = 0;
    body.engineRpm = 0;
    body.wallHitMask = 0;
    body.impactTimer = 0;
    body.wallImpact = 0;
    body.impactHold = 0;
    body.airborne = 0;
    body.loadSoundLevel = 0;
    body.impactSoundFlag = 0;
    body.word760 = 0;
    body.engineVisual0 = 0x80;
    body.engineVisual1 = 0x80;
    body.exhaustFlame = 0;
    ResetWheelState(body);
    body.forwardSpeed = 0;
    body.lateralSpeed = 0;
    body.clutchState = 0;
    body.clutchRequest = 0;
    body.engineSpeed = 0;
    body.pitchRate = 0;
    body.rollRate = 0;
    body.contactMask = 0;
    body.contactFlags = 0;
    body.shiftTimer = 0;
    if (body.raceState != 0 || body.transmissionMode == 0) body.shiftTimer = uint8_t(rate / 5);
    body.downforceForce[0] = 0;
    body.downforceForce[1] = 0;
    body.forwardAccel = 0;
    body.lateralAccel = 0;
    body.yawAccel = 0;
    body.steerBlend = 0;
    body.externalLongForce = 0;
    body.externalLatForce = 0;
    body.netLongitudinalForce = 0;
    body.hitNormal0 = 0;
    body.hitNormal1 = 0;
    body.hitCorner = 0;
    body.scrapeDirection = 0;
    body.wallScrape = 0;
    body.pushForce[0] = 0;
    body.pushForce[1] = 0;
}

void ResetTyreWear(CarBody& body, const TyreWearConstants& wear) { // 0x80032A1C
    for (int wheel = 0; wheel < 4; wheel++) {
        Wheel& w = WheelOf(body, wheel);
        w.slipRatioAbs = 0;
        w.lateralFactorAbs = 0;
        if (wear.wearLimit == 0) {
            w.wear = 0;
            w.wearStage = 0;
            w.wearGrip = 0x1000;
        } else {
            w.wear = wear.coldLimit;
            w.wearStage = int8_t(wear.coldLimit < 0 ? 0xC1 : 0);
            w.wearGrip = int16_t(0x1000 - wear.coldGripLoss);
        }
        w.wearRate = 0;
        w.damage = 0;
    }
}

// 0x80035948: target speed of a cornering section from its curvature, banking and the tyre grip.
int32_t SectionTargetSpeed(const CarBody& body, const RaceSectionList& list, int32_t section, const DriveClassTuning* classTuning,
                           const TyreWearConstants& wear) {
    const RaceSection& s = list.sections[section];
    const int16_t surfaceGrip = SurfaceGripAt(body, s.surface); // s8 index
    int32_t grip = MulLow(surfaceGrip, body.gripAtZeroSlip) >> 8;
    if (wear.wearLimit != 0 && (body.flags78D & 0x10) == 0) {
        uint32_t wearSum = 0;
        for (int wheel = 0; wheel < 4; wheel++) wearSum += uint16_t(body.wheels[wheel].wearGrip);
        grip = MulLow(int32_t(wearSum << 16) >> 18, grip) >> 12;
    }
    int32_t lateral;
    bool divide = false;
    int32_t bankTerm = 0;
    if (s.curvature < 0) {
        lateral = Mul12Floor(int32_t(0u - uint32_t(s.curvature)), grip);
        if (s.bank >= 1) {
            divide = true;
            bankTerm = int32_t(0u - uint32_t(Sin(uint32_t(s.bank))));
        }
    } else {
        lateral = Mul12Floor(s.curvature, grip);
        if (s.bank < 0) {
            divide = true;
            bankTerm = Sin(uint32_t(s.bank));
        }
    }
    if (divide) lateral = int32_t(uint64_t(Div64(int64_t(lateral) << 12, int64_t(bankTerm + 0x1000))));
    const uint8_t driveClass = body.driveClass;
    const int32_t root = SquareRoot(lateral, 6);
    const int32_t speed = Mul12Floor(classTuning[driveClass].cornerSpeedGain, root) + classTuning[driveClass].cornerSpeedBase;
    const int32_t topSpeed = body.topSpeed;
    if (speed > topSpeed) return topSpeed;
    return speed < 0x58E8 ? 0x58E8 : speed;
}

// 0x80035B68: the next section's type and target speed.
void AdvanceSection(CarBody& body, const RaceSectionList& list, const DriveClassTuning* classTuning, const TyreWearConstants& wear) {
    const int32_t count = list.count;
    body.aiPreviousType = body.aiSectionType;
    body.aiPreviousTargetSpeed = body.aiTargetSpeed;
    const int32_t index = body.aiSection;
    body.aiSectionType = uint8_t(list.sections[index == count ? count - 1 : index].type);
    const uint8_t type = body.aiSectionType;
    if (type == 0) {
        body.aiTargetSpeed = body.topSpeed;
    } else if (type < 3) {
        int32_t section = index;
        if (section == count) section--;
        body.aiTargetSpeed = SectionTargetSpeed(body, list, section, classTuning, wear);
    }
}

void InitRaceProgress(CarBody& body, uint8_t raceState, const RaceSectionList lists[7], const DriveClassTuning* classTuning,
                      const TyreWearConstants& wear) { // 0x800367AC -> 0x80036340
    body.aiLine = raceState;
    const RaceSectionList& list = lists[raceState];
    const int32_t count = list.count;
    const int32_t distance = body.courseDistance;
    body.aiSection = uint8_t(count);
    for (int32_t i = 0; i < count; i++)
        if (distance < list.sections[i].distance) {
            body.aiSection = uint8_t(i);
            break;
        }
    int32_t previous = int32_t(body.aiSection) + count - 1;
    if (previous >= count) {
        previous -= count;
        while (previous >= count) previous -= count;
    }
    const uint8_t type = uint8_t(list.sections[previous].type);
    body.aiSectionType = type;
    if (type == 0) body.aiTargetSpeed = body.topSpeed;
    else body.aiTargetSpeed = SectionTargetSpeed(body, list, previous, classTuning, wear);
    AdvanceSection(body, list, classTuning, wear);
}

namespace {
// 0x800358E0: the first of the requested state's five candidates that has a section list, or -1.
int32_t PickRaceState(uint32_t requested, const RaceSectionList lists[7], const int8_t* table) {
    if (requested > 6) requested = 6;
    for (uint32_t i = 0; i < 5; i++) {
        const int8_t candidate = table[requested * 5 + i];
        if (candidate < 0 || candidate > 6) continue; // a -1 entry makes the original test a word of the grid object, never for state 6
        if (lists[candidate].sections != nullptr) return candidate;
    }
    return -1;
}
} // namespace

uint32_t StartCar(CarBody& body, const RaceStartInputs& inputs) { // 0x80033384
    body.carIndex = inputs.carIndex;
    body.contactType = inputs.contactType;
    uint8_t controlClass = inputs.controlClass;
    if (((inputs.word801C98A0 >> 16) & 0xFF) == 1 && inputs.carIndex == 0) controlClass = 1;
    body.controlClass = controlClass; // 0x80030D64
    body.raceState = 0;
    CarSetupInputs setup = inputs.setup;
    setup.controlMode = body.controlClass;
    setup.byte1C = inputs.byte1C;
    SetupCar(body, setup);
    const uint32_t result = 0;
    if (int8_t(body.controlClass) == 0) body.transmissionMode = inputs.transmission; // 0x80030D70
    else if (int8_t(body.controlClass) == 2) body.transmissionMode = 0;
    // raceMode 6 with contact type 2 would consult 0x8003EF40 here (not ported; see the header).
    body.flags78D = 0;
    // 0x80039040 with a grid offset: the start position on the main line (the Handicap Start); behind the line = sector 0, lap 0.
    int32_t chunk = inputs.chunkHint, x = inputs.x, y = inputs.y, headingSin = inputs.headingSin, headingCos = inputs.headingCos;
    bool behind = false;
    if (inputs.gridOffset != 0) {
        AiCourseData course;
        for (size_t i = 0; i < 7; i++) course.lines[i] = inputs.sectionLists[i];
        course.courseLength = inputs.courseLength;
        course.raceStateTable = inputs.raceStateTable;
        behind = LineStartPlacement(course, inputs.pointToPoint, inputs.startLineDistances, inputs.startLineCount, inputs.gridOffset, chunk, x, y, headingSin,
                                    headingCos) < 0;
    }
    PlaceCarOnCourse(body, *inputs.course, chunk, x, y, headingSin, headingCos, inputs.raceMode);
    ResetViewState(body);
    ResetDynamicState(body, inputs.step.rate);
    ResetTyreWear(body, inputs.wear);
    SetStepTime(body, inputs.step);
    const int32_t startLines = inputs.startLineCount;
    body.sector = uint8_t(startLines);
    if (!behind)
        for (int32_t i = 0; i < startLines; i++)
            if (body.courseDistance < inputs.startLineDistances[i]) {
                body.sector = uint8_t(i);
                break;
            }
    if (!behind && int32_t(body.sector) != startLines) {
        body.lap = 1;
    } else {
        body.sector = 0;
        body.lap = 0;
    }
    body.lastLineTime = 0;
    body.aiRecovery = 0;
    body.scriptedControl = 0;
    body.penaltyFrames = 0;
    body.finishFlag = 0;
    body.dirtiness = 0;
    body.messageCode = 0;
    body.messageFrames = 0;
    body.aiPreviousTargetSpeed = body.topSpeed;
    body.aiTargetSpeed = body.topSpeed;
    body.gridSlot = int8_t(inputs.hasGridList ? 8 - int32_t(inputs.carIndex) : 0); // 0x800392AC
    body.racePosition = uint8_t(inputs.carIndex + 1);
    const int32_t raceState = PickRaceState(6, inputs.sectionLists, inputs.raceStateTable);
    InitRaceProgress(body, uint8_t(raceState), inputs.sectionLists, inputs.setup.constants.classTuning, inputs.wear);
    // 0x801D5869 == 0 (no standing start) with a start speed in the low byte of 0x801C98A0: the rolling start.
    if (inputs.byte801D5869 == 0 && (inputs.word801C98A0 & 0xFF) != 0) RollingStart(body, int32_t(inputs.word801C98A0 & 0xFF));
    body.licenseState = 0; // 0x8003D22C
    body.licenseCode = 0;
    body.licenseTime = 359999999;
    return result;
}

namespace {
// 0x800448C8 (a private helper of drivetrain.cpp, repeated here for the rolling start): the lowest forward gear
// whose limit speed at the upshift rpm + 500 exceeds `speed`; 0 when rolling backwards, 1 with fewer than three
// forward gears, the gear count when no gear is low enough.
int32_t RollingStartGear(const CarBody& body, int32_t speed) {
    if (speed < 0) return 0;
    const int32_t gears = body.forwardGears;
    if (gears < 3) return 1;
    for (int32_t gear = 1; gear < gears; gear++) {
        const int32_t revsPerSecond12 = int32_t((uint32_t(body.upshiftRpm) + 500u) << 12) / 60;
        const int32_t limit = int32_t(uint64_t(Div64(int64_t(revsPerSecond12) << 12, int64_t(body.revsPerSpeed[gear]))));
        if (speed < limit) return gear;
    }
    return gears;
}
} // namespace

void RollingStart(CarBody& body, int32_t kmh) { // 0x8003311C
    const int32_t speed = int32_t(uint32_t(kmh) * 1138u);
    body.yawRate = 0;
    for (int i = 0; i < 3; i++) body.velocity[i] = Mul12Wide(body.basis[0][i], speed); // along the forward row
    // 1/100 mph: kmh * 1000000 / 16093 (magic 0x41284311 >> 44, rounded toward zero)
    const int32_t scaled = int32_t(uint32_t(kmh) * 1000000u);
    const int32_t readout = int32_t((int64_t(scaled) * 0x41284311LL) >> 32) >> 12;
    body.speedReadout = uint16_t(readout - (scaled >> 31));
    body.maxSpeedReadout = body.speedReadout;
    body.gear = uint8_t(RollingStartGear(body, speed));
    int32_t engineSpeed = Mul12Wide(body.revsPerSpeed[body.gear], speed);
    body.engineSpeed = engineSpeed;
    const int32_t idle = int32_t(uint32_t(body.idleRpm) << 12) / 60;
    if (engineSpeed < idle) body.engineSpeed = idle;
    else {
        const int32_t limit = int32_t(uint32_t(body.revLimitRpm) << 12) / 60;
        if (limit < engineSpeed) body.engineSpeed = limit;
    }
    int32_t revs = int32_t(uint32_t(body.engineSpeed) * 60u);
    if (revs < 0) revs += 4095;
    body.engineRpm = int16_t(revs >> 12);
    if (body.boostCap != 0) {
        body.turboSpool[0] = uint16_t(body.engineRpm);
        if (body.turboBoost[1] != 0) body.turboSpool[1] = uint16_t(body.engineRpm);
        body.intakeLoad = int16_t(BoostMultiplier(body, body.engineRpm, body.engineRpm)); // 0x80075074
        body.engineLoad = 0x1000;
        body.blowOffState = 0;
        body.turboSpoolMax = uint16_t(body.engineRpm);
    }
    body.clutchState = 1;
    body.forwardSpeed = speed;
    body.lateralSpeed = 0;
    body.clutchRequest = 0x1000;
    for (Wheel& w : body.wheels) {
        w.rimSpeed = speed;
        w.contactForwardSpeed = speed;
    }
    for (int axle = 0; axle < 2; axle++) body.axleSpeed[axle] = Mul12Wide(body.inverseWheelRadius[axle], speed);
}

} // namespace gt2::sim
