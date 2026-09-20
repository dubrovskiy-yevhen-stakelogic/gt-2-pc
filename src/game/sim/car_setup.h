#pragma once
#include <cstddef>
#include <cstdint>

#include "game/sim/car_body.h"
#include "game/sim/ground.h"
#include "game/sim/tyres.h"

// Car setup: fills the physics body from the car's parameter record at race start (original: 0x80033384 ->
// 0x800319A8 per car, gear ratios in 0x800347C4). This is the boundary between the game's data files and the
// simulation, and therefore the place where an editable car-parameter layer plugs in.
// Ported from the US Simulation v1.2 executable (SHA-1 3030aa27...) and verified bit for bit by tools/gt2verify
// (verify_setup.cpp). Offsets in comments are relative to the body (car + 0x2C) unless stated otherwise.
//
// Units: lengths 1/4096 m, masses 4 units per kg (weight = mass * 2.45 = kg * 9.8), factors 0x1000 = 1.0,
// angles 4096 per turn, engine speed rev/s << 12, rpm as plain integers.
namespace gt2::sim {

// ---------------------------------------------------------------- the input record

// The 0x1C0-byte car parameter record the game shell assembles for each race slot from the car's data file
// and the parts / settings chosen in the menus. In the attract race of the dump the six records live at
// 0x801C98E0 + 0x14FDA + slot * 0x1C0 (0x8001523C passes them to 0x80012CD4, which hands one to 0x80033384).
// Every field below is named from how the setup reads it; "raw" means the unit was not established.
// The setup MUTATES a few fields (marked "patched"): a zero last torque point is filled from the previous one,
// the first sample of every tyre curve is forced to 0, the negative-side second slip-ratio sample is copied
// from the positive side, and the gear ratios are rewritten when gearAutoSet is on.
#pragma pack(push, 1)
struct CarParams {
    uint8_t reserved000[8];          // 0x000  zero in the dump
    int16_t frontLength;             // 0x008  mm, reference point -> front edge (frontExtent)
    int16_t rearLength;              // 0x00A  mm, reference point -> rear edge (rearExtent)
    int16_t width;                   // 0x00C  mm
    int16_t height;                  // 0x00E  mm
    int16_t wheelbase;               // 0x010  mm
    uint8_t frontWeightPercent;      // 0x012  static weight on the front axle, %
    uint8_t idleRpm10;               // 0x013  idle rpm / 10
    int16_t frontTrack;              // 0x014  mm
    int16_t rearTrack;               // 0x016  mm
    int16_t gearRatio[8];            // 0x018  x1000; [0] reverse, [1..gearCount] forward, unused = -1 (patched by the auto-set)
    int16_t finalDrive;              // 0x028  x1000
    uint8_t gearCount;               // 0x02A  number of forward gears (top gear index)
    uint8_t engineInertia;           // 0x02B  raw: engineInertiaRef = v * 64 / 100, engineInvInertia = 0x0FEB0000 / (v * 4)
    uint8_t engineBrake;             // 0x02C  raw: engineBrakeCoefficient = (v * 39.2) / torque[last]
    uint8_t turboBoostCap10;         // 0x02D  boost cap x10; 0 = no turbo
    uint8_t absGain[2];              // 0x02E  front, rear: absSlipGain = v / 10
    uint8_t reserved030;             // 0x030
    uint8_t upshiftRpm100;           // 0x031  rpm / 100: automatic upshift point; rev limit floor = v * 100 + 500
    uint8_t revLimitRpm100;          // 0x032  rpm / 100; 0 = last torque point
    uint8_t reserved033;             // 0x033
    uint8_t torqueRpm100[16];        // 0x034  rpm / 100 of the torque curve samples (torquePointCount used)
    uint8_t steerLimitCount;         // 0x044  samples of the speed -> steering limit table (0..6)
    uint8_t steerLimitXs[6];         // 0x045  speed = v << 7 (1/4096 m/s)
    uint8_t steerLimitYs[6];         // 0x04B  steering fraction = v * 4096 / 1800
    uint8_t fourWheelType;           // 0x051  4WD subtype 0..4 (driveType 2): 0 centre diff, 1 type 3, 2 type 4, 3 type 5, 4 type 6
    uint8_t reserved052[4];          // 0x052
    uint8_t torquePointCount;        // 0x056  samples in torqueRpm100 / torque (2..16)
    uint8_t yawInertiaCode;          // 0x057  raw: inverse yaw inertia = 0x28C / max(v * 400, 12000) << 19; 0 = keep
    uint8_t pitchInertiaCode;        // 0x058  raw: axle / (v * 400) << 16 per axle; 0 = keep
    uint8_t rollInertiaCode;         // 0x059  raw: half track / max(v * 400, 12000) << 16; 0 = keep
    int16_t weightKg;                // 0x05A  kg; below 900 it is mapped up (0x80030BA4: 750..899 -> 810..899, floor 810)
    uint8_t reserved05C;             // 0x05C
    uint8_t downshiftFloorRpm10;     // 0x05D  rpm / 10: lowest rpm an automatic downshift may target
    uint8_t steerLockDeg;            // 0x05E  degrees -> steerLock (angle units)
    uint8_t steerRateDeg;            // 0x05F  degrees -> body + 0x3A
    uint8_t brakeFront;              // 0x060  raw: brake torque = v * 392.0; also enables brakeLoadFactor with brakeRear
    uint8_t brakeRear;               // 0x061
    uint8_t dragCoefficient100;      // 0x062  Cd x100
    uint8_t handbrake;               // 0x063  raw: handbrake torque = v * 392.0
    uint8_t wheelInertia[2];         // 0x064  front, rear: axleInvInertia = radius / (v * 16), axleInertia += v * 32
    uint8_t tyreWidthCode[2];        // 0x066  front, rear: adds v * 254 / 20000 m to the wheel radius
    uint8_t rimCode[2];              // 0x068  front, rear: rim radius = (v * 10 + 5) / 1000 m
    uint8_t tyreAspectCode[2];       // 0x06A  front, rear: tyre height = rim radius * v / 20
    uint8_t camberFront10;           // 0x06C  degrees x10 -> body + 0x368 = -angle / 10
    uint8_t camberRear10;            // 0x06D  -> body + 0x36A
    uint8_t suspension[2][12];       // 0x06E  front, rear: see SuspensionParams
    uint8_t tyreGripPercent[2];      // 0x086  front, rear compound grip, % (0 = 100)
    uint8_t downforce[2];            // 0x088  front, rear: downforce coefficient x100
    uint8_t driveType;               // 0x08A  0 rear, 1 front, 2 four-wheel (subtype in fourWheelType), 3 / 4 like 0; > 4 -> 0
    uint8_t clutchCode;              // 0x08B  raw: lockedClutchTorque = v * 39.2
    uint8_t reserved08C[2];          // 0x08C
    uint8_t slipAngleCount;          // 0x08E  front: samples of the slip-angle curve (1..8)
    uint8_t slipAngleXs[8];          // 0x08F  angle / 255 of 0x400 (patched: [0] = 0)
    uint8_t slipAngleYs[8];          // 0x097  force factor / 200 (patched: [0] = 0)
    uint8_t slipAngleCountRear;      // 0x09F
    uint8_t slipAngleXsRear[8];      // 0x0A0
    uint8_t slipAngleYsRear[8];      // 0x0A8
    // Slip-ratio curves: a negative side and a positive side per axle, 6 samples each, sharing one xs table.
    uint8_t slipRatioNegCount;       // 0x0B0  front, negative side (braking); 1..6
    uint8_t slipRatioNegXs[6];       // 0x0B1  ratio / 255 (negated; patched: [0] = 0)
    uint8_t slipRatioNegYs[6];       // 0x0B7  longitudinal force factor / 200 (patched: [0] = 0)
    uint8_t slipRatioNegYs2[6];      // 0x0BD  lateral grip factor / 200 (patched: [0] = slipRatioPosYs2[0])
    uint8_t slipRatioPosCount;       // 0x0C3  front, positive side (traction)
    uint8_t slipRatioPosXs[6];       // 0x0C4
    uint8_t slipRatioPosYs[6];       // 0x0CA
    uint8_t slipRatioPosYs2[6];      // 0x0D0
    uint8_t slipRatioNegCountRear;   // 0x0D6
    uint8_t slipRatioNegXsRear[6];   // 0x0D7
    uint8_t slipRatioNegYsRear[6];   // 0x0DD
    uint8_t slipRatioNegYs2Rear[6];  // 0x0E3
    uint8_t slipRatioPosCountRear;   // 0x0E9
    uint8_t slipRatioPosXsRear[6];   // 0x0EA
    uint8_t slipRatioPosYsRear[6];   // 0x0F0
    uint8_t slipRatioPosYs2Rear[6];  // 0x0F6
    uint8_t reserved0FC[0x116 - 0x0FC]; // 0x0FC  zero in the dump
    uint8_t rideHeightMm[2];         // 0x116  front, rear: body + 0x24 / + 0x26 (m), the height of the CG above the ground
    uint8_t steerMaxRateCode;        // 0x118  steerMaxRate = v * 10 * 4096 / 360 (dirt courses: fixed 0x5C7)
    uint8_t centreSplitPercent;      // 0x119  4WD: torque to the front, % (fourWheelType 2: >= 50 forces 1.0)
    uint8_t loadGripCount;           // 0x11A  front: samples of the load -> grip curve (1..4)
    uint8_t loadGripXs[4];           // 0x11B  load = v * 784 * rimRadius (patched: [0] = 0)
    uint8_t loadGripYs[4];           // 0x11F  grip = gripFactor * load * v / 200
    uint8_t loadGripCountRear;       // 0x123
    uint8_t loadGripXsRear[4];       // 0x124
    uint8_t loadGripYsRear[4];       // 0x128
    uint8_t camberGripCount;         // 0x12C  front: samples of the camber -> grip curve (1..4)
    uint8_t camberGripXs[4];         // 0x12D  |camber| = v * 227 / 255 angle units (patched: [0] = 0)
    uint8_t camberGripYs[4];         // 0x131  factor / 200
    uint8_t camberGripCountRear;     // 0x135
    uint8_t camberGripXsRear[4];     // 0x136
    uint8_t camberGripYsRear[4];     // 0x13A
    uint16_t torque[16];             // 0x13E  torque curve samples, raw (x39.2 then scaled by the power settings); (patched: a zero last sample)
    uint8_t reserved15E[0x178 - 0x15E]; // 0x15E  zero in the dump
    uint8_t turboSpoolRpm100;        // 0x178  first turbo: spool rpm / 100
    uint8_t turboBoost10;            // 0x179  first turbo boost x10 (index 1 of the triple)
    uint8_t turboSpoolRate10;        // 0x17A  first turbo spool rate x10
    uint8_t turboSpoolRpm100Second;  // 0x17B
    uint8_t turboBoost10Second;      // 0x17C
    uint8_t turboSpoolRate10Second;  // 0x17D
    uint8_t tyreGripModifier[2];     // 0x17E  front, rear: extra grip %, (0 = 100)
    uint8_t reserved180[2];          // 0x180
    uint8_t axleInertiaCode[2];      // 0x182  front, rear: axleInertia = v * 16 * finalDrive / 1000 + wheelInertia * 32
    int8_t diffTypeCode[2];          // 0x184  front, rear: letter code looked up in SetupConstants::diffTypeCodes
    uint8_t diffInitialTorque[2];    // 0x186  front, rear (unit depends on the diff type)
    uint8_t diffAccel[2];            // 0x188
    uint8_t diffDecel[2];            // 0x18A
    uint8_t damperScaleDivisor[2];   // 0x18C  front, rear: damper velocities scaled by 0x64000 / v (75..90 in the dump)
    uint8_t toeCode[2];              // 0x18E  front, rear: body + 0x36C = v - 128
    uint8_t reserved190[2];          // 0x190
    uint8_t surfaceGripPercent[8];   // 0x192  per surface type (wheel + 0x14); 0 = 100
    uint8_t reserved19A[2];          // 0x19A
    uint8_t turboModel[2];           // 0x19C  first, second turbo: 0..2 (else 0)
    uint8_t bumpTravelMm[2];         // 0x19E  front, rear: bump-stop start above the floor (capped by rideHeight)
    uint8_t droopTravelMm[2];        // 0x1A0  front, rear: droop limit above the floor + ride height
    uint8_t tcsFalloffGain10;        // 0x1A2  tcsFalloffGain = v / 10
    uint8_t tcsSteerGain100;         // 0x1A3  tcsSteerGain = v / 100
    uint8_t tcsGain10;               // 0x1A4  tcsGain = v / 10
    uint8_t asmYawGain100;           // 0x1A5  asmYawGain = v / 100
    uint8_t asmYawThreshold100;      // 0x1A6  asmYawThreshold = v / 100
    uint8_t reserved1A7[3];          // 0x1A7
    uint8_t gearAutoSet;             // 0x1AA  non-zero: gear ratios are generated (0x80074B38) from gearAutoFinal
    uint8_t gearAutoFinal;           // 0x1AB  auto-set top speed, km/h (0 = off; 255 is passed as 25.5 km/h, else v * 10 in 0.1 km/h)
    uint8_t powerPercent;            // 0x1AC  engine power at the first rpm sample, % (0 = 100)
    uint8_t reserved1AD;             // 0x1AD
    uint16_t torqueMultiplier1000;   // 0x1AE  x1000 (0 = 1000; < 256 is x100 and multiplied by 10)
    uint16_t powerPercentTop;        // 0x1B0  power at the last rpm sample, x10 % (0 = 1000); blended linearly over the samples
    uint8_t reserved1B2[0x1C0 - 0x1B2]; // 0x1B2  zero in the dump
};
#pragma pack(pop)
static_assert(sizeof(CarParams) == 0x1C0);
static_assert(offsetof(CarParams, gearRatio) == 0x18);
static_assert(offsetof(CarParams, torqueRpm100) == 0x34);
static_assert(offsetof(CarParams, torquePointCount) == 0x56);
static_assert(offsetof(CarParams, weightKg) == 0x5A);
static_assert(offsetof(CarParams, suspension) == 0x6E);
static_assert(offsetof(CarParams, tyreGripPercent) == 0x86);
static_assert(offsetof(CarParams, driveType) == 0x8A);
static_assert(offsetof(CarParams, slipAngleCount) == 0x8E);
static_assert(offsetof(CarParams, slipAngleCountRear) == 0x9F);
static_assert(offsetof(CarParams, slipRatioNegCount) == 0xB0);
static_assert(offsetof(CarParams, slipRatioPosCount) == 0xC3);
static_assert(offsetof(CarParams, slipRatioNegCountRear) == 0xD6);
static_assert(offsetof(CarParams, slipRatioPosCountRear) == 0xE9);
static_assert(offsetof(CarParams, rideHeightMm) == 0x116);
static_assert(offsetof(CarParams, loadGripCount) == 0x11A);
static_assert(offsetof(CarParams, loadGripCountRear) == 0x123);
static_assert(offsetof(CarParams, camberGripCount) == 0x12C);
static_assert(offsetof(CarParams, camberGripCountRear) == 0x135);
static_assert(offsetof(CarParams, torque) == 0x13E);
static_assert(offsetof(CarParams, turboSpoolRpm100) == 0x178);
static_assert(offsetof(CarParams, tyreGripModifier) == 0x17E);
static_assert(offsetof(CarParams, axleInertiaCode) == 0x182);
static_assert(offsetof(CarParams, damperScaleDivisor) == 0x18C);
static_assert(offsetof(CarParams, surfaceGripPercent) == 0x192);
static_assert(offsetof(CarParams, bumpTravelMm) == 0x19E);
static_assert(offsetof(CarParams, gearAutoSet) == 0x1AA);
static_assert(offsetof(CarParams, torqueMultiplier1000) == 0x1AE);
static_assert(offsetof(CarParams, powerPercentTop) == 0x1B0);

// The 12 bytes of one axle's suspension settings (CarParams::suspension[axle]), read by 0x80030F94.
struct SuspensionParams {
    uint8_t springCode;      // +0  10..255 -> spring rate = (16 + (v - 10) * 69 / 245) * 0xF50 (SetupConstants::springRateRange)
    uint8_t antiRollCode;    // +1  antiRollRate = v * 392.0
    uint8_t bumpStopCode;    // +2  bumpStopRate = v * 3920.0
    uint8_t unsprungMass;    // +3  kg per wheel: block + 0x30, staticLoad (+0x32) = v * 9.8
    uint8_t bumpLowKnee;     // +4  damper curve (bump): low-speed knee %, scaled by 0x64000 / damperScaleDivisor
    uint8_t bumpLowForce;    // +5  ... force at the knee, x196.0
    uint8_t bumpHighKnee;    // +6  high-speed knee %
    uint8_t bumpHighForce;   // +7  force at the high knee
    uint8_t reboundLowKnee;  // +8  the same four for rebound
    uint8_t reboundLowForce; // +9
    uint8_t reboundHighKnee; // +10
    uint8_t reboundHighForce;// +11
};
static_assert(sizeof(SuspensionParams) == 12);

// ---------------------------------------------------------------- body blocks written by the setup

// Engine block (body + 0x7C .. 0x12C). The two tokens are the original's guest pointers to xs / ys.
#pragma pack(push, 1)
struct EngineBlock {
    uint16_t count;              // +0x00  torque curve samples
    uint16_t reserved02;
    uint32_t xsToken;            // +0x04  -> +0x0C (guest pointer; a 32-bit token here)
    uint32_t ysToken;            // +0x08  -> +0x4C
    int32_t xs[16];              // +0x0C  engine speed rev/s << 12 (= rpm * 4096 / 60)
    int32_t ys[16];              // +0x4C  torque = raw * 39.2 * power(rpm) * torqueMultiplier / 1e8, scaled by the turbo model
    uint16_t revLimitRpm;        // +0x8C  (body + 0x108)
    uint16_t idleRpm;            // +0x8E  (body + 0x10A)
    int32_t engineBrakeCoefficient; // +0x90  (body + 0x10C)
    int16_t turboSpoolRpm[2];    // +0x94  (body + 0x110)
    int16_t turboBoost[2];       // +0x98  (body + 0x114)
    int32_t turboSpoolRate[2];   // +0x9C  (body + 0x118)
    int32_t turboCoefficient[2]; // +0xA4  (body + 0x120)  boost / (spool rpm^2 >> 12)
    int16_t boostCap;            // +0xAC  (body + 0x128)  0 = no turbo
    uint8_t turboModel[2];       // +0xAE  (body + 0x12A)
};
static_assert(sizeof(EngineBlock) == 0xB0);
constexpr uint32_t kEngineBlockOffset = 0x7C;

// Per-axle tyre block (body + 0x194 + axle * 0xD8). Curve objects are { u16 count; xs token; ys token } with the
// samples stored right behind them inside the block.
struct AxleTyreBlock {
    uint16_t slipAngleCount;     // +0x00  slip angle -> lateral force factor (0x80039F4C evaluates it)
    uint16_t reserved02;
    uint32_t slipAngleXsToken;   // +0x04  -> +0x0C
    uint32_t slipAngleYsToken;   // +0x08  -> +0x1C
    int16_t slipAngleXs[8];      // +0x0C  angle units 0..0x400
    int16_t slipAngleYs[8];      // +0x1C  0x1000 = 1.0
    uint16_t slipRatioCount;     // +0x2C  slip ratio -> longitudinal force factor
    uint16_t reserved2E;
    uint32_t slipRatioXsToken;   // +0x30  -> +0x38
    uint32_t slipRatioYsToken;   // +0x34  -> +0x50
    int16_t slipRatioXs[12];     // +0x38  -0x1000..0x1000, negative side first
    int16_t slipRatioYs[12];     // +0x50
    int16_t peakSlipRatioNeg;    // +0x68  xs of the largest ys on the negative side (brake slip threshold)
    int16_t peakSlipRatioPos;    // +0x6A  ... positive side (traction slip threshold)
    uint16_t slipRatioGripCount; // +0x6C  second curve on the same xs: lateral grip factor
    uint16_t reserved6E;
    uint32_t slipRatioGripXsToken; // +0x70  -> +0x38
    uint32_t slipRatioGripYsToken; // +0x74  -> +0x78
    int16_t slipRatioGripYs[12]; // +0x78
    uint16_t loadGripCount;      // +0x90  vertical load -> grip capacity (s32 samples)
    uint16_t reserved92;
    uint32_t loadGripXsToken;    // +0x94  -> +0x9C
    uint32_t loadGripYsToken;    // +0x98  -> +0xAC
    int32_t loadGripXs[4];       // +0x9C
    int32_t loadGripYs[4];       // +0xAC
    uint16_t camberGripCount;    // +0xBC  |camber| -> grip factor
    uint16_t reservedBE;
    uint32_t camberGripXsToken;  // +0xC0  -> +0xC8
    uint32_t camberGripYsToken;  // +0xC4  -> +0xD0
    int16_t camberGripXs[4];     // +0xC8
    int16_t camberGripYs[4];     // +0xD0
};
static_assert(sizeof(AxleTyreBlock) == 0xD8);
static_assert(offsetof(AxleTyreBlock, slipRatioGripYs) == 0x78);
static_assert(offsetof(AxleTyreBlock, camberGripXs) == 0xC8);
constexpr uint32_t kAxleTyreBlockOffset = 0x194;

// Drivetrain block (body + 0x370 .. 0x420); names as in re/structs/car.yaml.
struct DrivetrainBlock {
    uint8_t driveType;           // +0x00  (0x370) 0 rear, 1 front, 2/3 4WD centre diff, 4/5/6 4WD coupling
    uint8_t reserved01;
    uint8_t gearCount;           // +0x02  (0x372) forward gears
    uint8_t primaryAxle;         // +0x03  (0x373) 0 front, 1 rear
    int16_t centreSplit;         // +0x04  (0x374) 0x1000 = 1.0
    int16_t centreLockTorque;    // +0x06  (0x376)
    int16_t engineInertiaRef;    // +0x08  (0x378)
    uint8_t axleDiffType[2];     // +0x0A  (0x37A) 0 open, 1 locked, 3/4 torque sensing, 5 speed sensing, 6/7 active
    int32_t axleDiffMinTorque[2]; // +0x0C  (0x37C)
    int32_t axleDiffAccelRatio[2]; // +0x14 (0x384)
    int32_t axleDiffDecelRatio[2]; // +0x1C (0x38C)
    uint16_t downshiftFloorRpm;  // +0x24  (0x394)
    uint16_t upshiftRpm;         // +0x26  (0x396)
    uint16_t downshiftRpm[6];    // +0x28  (0x398) per gear from 2
    int32_t gearRatio[8];        // +0x34  (0x3A4) ratio * final drive, 12.12; [0] = reverse
    int32_t revsPerSpeed[8];     // +0x54  (0x3C4) gearRatio / wheel circumference, 12.12
    int16_t wheelRadius[2];      // +0x74  (0x3E4) 1/4096 m: rim radius + tyre height + width term
    int16_t inverseWheelRadius[2]; // +0x78 (0x3E8) 0x1000000 / wheelRadius
    int16_t rimRadius[2];        // +0x7C  (0x3EC)
    int16_t tyreHeight[2];       // +0x80  (0x3F0)
    int16_t lockedClutchTorque;  // +0x84  (0x3F4)
    int16_t overrunClutchTorque; // +0x86  (0x3F6) 1.5 x the largest torque of the curve
    int32_t axleInvInertia[2];   // +0x88  (0x3F8)
    int32_t axleInertia[2];      // +0x90  (0x400)
    int32_t centreDiffStiffness; // +0x98  (0x408) 1 / (axleInertia[0] + axleInertia[1])
    int32_t engineInvInertia;    // +0x9C  (0x40C)
    int16_t wheelInertiaFactor[2]; // +0xA0 (0x410) wheelInertia * inverseWheelRadius^2
    int32_t brakeTorque[3];      // +0xA4  (0x414) front, rear, handbrake (written by 0x800319A8)
};
static_assert(sizeof(DrivetrainBlock) == 0xB0);
static_assert(offsetof(DrivetrainBlock, gearRatio) == 0x34);
static_assert(offsetof(DrivetrainBlock, wheelRadius) == 0x74);
static_assert(offsetof(DrivetrainBlock, brakeTorque) == 0xA4);
constexpr uint32_t kDrivetrainBlockOffset = 0x370;
#pragma pack(pop)

// The block views alias the named CarBody members.
static_assert(kEngineBlockOffset == offsetof(CarBody, engineTorqueCount));
static_assert(kEngineBlockOffset + offsetof(EngineBlock, turboModel) == offsetof(CarBody, turboModel));
static_assert(kAxleTyreBlockOffset == offsetof(CarBody, tyres));
static_assert(kDrivetrainBlockOffset == offsetof(CarBody, driveType));
static_assert(kDrivetrainBlockOffset + offsetof(DrivetrainBlock, gearCount) == offsetof(CarBody, forwardGears));
static_assert(kDrivetrainBlockOffset + offsetof(DrivetrainBlock, brakeTorque) == offsetof(CarBody, brakeTorque));

// ---------------------------------------------------------------- setup inputs

// One of the four 40-byte drive-class tuning records at 0x801C8690 (indexed by body + 0x1D): the setup, the
// race-progress initialisation, the physics core's driver aids (physics_core.h AiAidTuning mirrors +0x18..+0x23)
// and the AI driver (ai_driver.h) read it.
#pragma pack(push, 1)
struct DriveClassTuning {
    int16_t brakeMarginFactor;   // +0x00  0x801C8690: > 0x1000 -> the AI eases off (factor - 1) * braking distance early (0x8003643C)
    int16_t cornerSpeedGain;     // +0x02  0x801C8692: section speed = gain * sqrt(grip term) + base (0x80035948)
    int32_t cornerSpeedBase;     // +0x04  0x801C8694
    int16_t steerGain;           // +0x08  0x801C8698: AI steering per unit of yaw demand beyond the yaw rate (0x80037834)
    int16_t overshootThrottleCut;// +0x0A  0x801C869A: AI throttle cut per unit of steering demand beyond the steering window
    int16_t overshootBrake;      // +0x0C  0x801C869C: AI brake per unit of steering demand beyond the window
    int16_t lookaheadSpeedGain;  // +0x0E  0x801C869E: speed * gain (12.12) blends the AI's lookahead distance (0x80035874)
    int32_t lookaheadMin;        // +0x10  0x801C86A0: lookahead distance at rest, 16.16 m
    int32_t lookaheadMax;        // +0x14  0x801C86A4: lookahead distance at speed
    int16_t yawBrakeGain;        // +0x18  0x801C86A8: brake assist (physics_core.h AiAidTuning)
    int16_t yawBrakeThreshold;   // +0x1A
    int16_t steerFalloffGain;    // +0x1C
    int16_t steerFalloffThreshold; // +0x1E
    int32_t tractionGain;        // +0x20  0x801C86B0
    int16_t frontGripWeight;     // +0x24  0x801C86B4: weight of the front tyre grip in the grip mean (rear = 1 - it)
    int16_t throttleScale;       // +0x26  0x801C86B6: AI throttle factor (0x1000 = 1.0) on every line but line 3 (0x8003643C)
};
#pragma pack(pop)
static_assert(sizeof(DriveClassTuning) == 40);
static_assert(offsetof(DriveClassTuning, yawBrakeGain) == 0x18 && offsetof(DriveClassTuning, throttleScale) == 0x26);

// Constants of the executable and the race overlay read by the setup. The tables are game data and are passed
// in by the caller (disc_data.h builds them from the disc); the scalars default to the attract race's values.
struct SetupConstants {
    int32_t dragConstant = 3276;         // 0x80046EF0  air density term: drag = width * height * (v / 2) * Cd
    int32_t rollingResistanceGain = 409; // 0x80046E00  scales the rolling-resistance curve in the top-speed estimate
    uint8_t springRateRange[2] = {16, 85}; // 0x80046DC8, 0x80046DC9  spring code 10 -> 16, 255 -> 85 (times 0xF50)
    const int8_t* diffTypeCodes = nullptr; // 0x80046DCC  8 letter codes of the differential types (index = type)
    const uint8_t* gearAutoTable = nullptr; // 0x800923E2  rows of 6 bytes per gear count (rows 5, 6, 7 are used)
    CurveS32 rollingResistance;          // 0x801C8730  speed -> rolling resistance factor (xs 0x80046EF8, ys 0x80046F18)
    const uint8_t* aiGripPercent = nullptr; // 0x801C98A4  AI cars: [class] front, [class + 4] rear grip %
    const DriveClassTuning* classTuning = nullptr; // 0x801C8690  4 records
};

// Everything 0x800319A8 takes for one car.
struct CarSetupInputs {
    CarParams* params = nullptr;   // the car's record (mutated, see CarParams)
    uint32_t bodyToken = 0;        // the 32-bit address the original stores in the body's table objects (pointers to
                                   // the samples inside the body); the native game passes 0 and resolves by offset
    uint8_t controlMode = 0;       // car + 0x45D: 0 player, 2 AI (AI cars take the class grip table)
    uint8_t byte1C = 0;            // body + 0x1C (the driver's 11th argument, 0 in the dump)
    bool dirtCourse = false;       // course table entry (0x80060E94(0x800AF230)) + 8 & 4
    SetupConstants constants;
};

// ---------------------------------------------------------------- pieces of the setup (all verified separately)

// Mass mapping of CarParams::weightKg (0x80030BA4): values >= 900 pass, otherwise max(v, 750) is mapped to
// (v - 400) * 3 / 5 + 600 (750 -> 810).
int32_t MapWeight(int16_t weightKg);

// Drive class of a drivetrain type (0x80030D18): 1/2 -> 1, 3/4/6 -> 2, 5 -> 3, else 0.
uint8_t DriveClassOf(uint8_t driveType);

// Wheel radius of one axle (0x80075FAC): rim radius (out), tyre height (out) and their sum plus the width term
// (returned and written to `total`).
int32_t WheelRadius(const CarParams& params, int axle, int16_t& total, int16_t& rimRadius, int16_t& tyreHeight);

// Drive layout from CarParams::driveType / fourWheelType (0x80076070): writes driveType, primaryAxle, centreSplit
// and centreLockTorque of the drivetrain block; returns the drive type. The four-wheel subtype (CarParams +
// 0x51, 0..4) selects the centre coupling model; dirt courses force type 4 (and a 0.5 split for subtypes 0, 1, 3).
uint8_t SetupDriveLayout(const CarParams& params, bool dirtCourse, uint8_t& driveType, uint8_t& primaryAxle,
                         int16_t& centreSplit, int16_t& centreLockTorque);

// Damper curve parameters from two (knee, force) points (0x80030DA0): out[0] = knee, out[1] = base, out[2] = gain of
// force = base + gain * sqrt(velocity - knee). All zero unless the points describe a rising curve.
void SetupDamperCurve(int32_t& knee, int32_t& base, int32_t& gain, int16_t lowKnee, int32_t lowForce, int16_t highKnee, int32_t highForce);

// Suspension block of one axle from its 12 settings (0x80030F94).
void SetupSuspension(AxleSuspension& block, const SuspensionParams& params, uint8_t damperScaleDivisor, const SetupConstants& constants);

// Travel limits of one axle from the sprung weight and the ride height (0x800312FC).
void SetupSuspensionTravel(CarBody& body, AxleSuspension& block, const CarParams& params, int axle);

// Engine block from the torque curve and the power settings (0x80075328 with the turbo block 0x800750C8).
void SetupEngine(CarBody& body, CarParams& params, uint32_t bodyToken);

// Gear ratios generated from the auto-set top speed (0x80074B38, called by 0x80034740 when gearAutoSet is on): the top
// gear reaches the rev limit at `position` (0.1 km/h units), the lower gears follow a progression from the table row.
void GenerateGearRatios(CarParams& params, int32_t position, bool dirtCourse, const SetupConstants& constants);

// Drivetrain block (0x800347C4). Needs the engine block.
void SetupDrivetrain(CarBody& body, CarParams& params, bool dirtCourse, const SetupConstants& constants);

// Tyre curves of one axle (0x800314DC slip ratio, 0x800316F4 slip angle, 0x80031794 load and camber).
void SetupSlipRatioCurves(AxleTyreBlock& block, uint8_t negCount, uint8_t negXs[6], uint8_t negYs[6], uint8_t negYs2[6],
                          uint8_t posXs[6], uint8_t posYs[6], uint8_t posYs2[6], uint8_t posCount, uint32_t blockToken);
void SetupSlipAngleCurve(AxleTyreBlock& block, uint8_t xs[8], uint8_t ys[8], uint8_t count, uint32_t blockToken);
void SetupLoadAndCamberCurves(AxleTyreBlock& block, uint8_t loadXs[4], const uint8_t loadYs[4], uint8_t loadCount, uint8_t camberXs[4],
                              const uint8_t camberYs[4], uint8_t camberCount, int16_t rimRadius, int32_t gripFactor, uint32_t blockToken);

// Slip angle with the largest cos(angle) * lateral force factor of the front curve, -45..45 degrees (0x8003B598).
int32_t PeakSlipAngle(const CarBody& body);

// Top speed estimate and the brake-balance speed (0x80030C5C with 0x80030950).
void SetupTopSpeed(CarBody& body, const SetupConstants& constants);

// The whole per-car setup (0x800319A8). Returns nothing; the original returns 0.
void SetupCar(CarBody& body, const CarSetupInputs& inputs);

// ---------------------------------------------------------------- race start (the per-race driver 0x80033384)

// Course queries the placement needs (0x80028900, 0x80028C6C, 0x80028830 of the original). The game implements
// them with the native course code (ground.h); the verifier routes them to the original.
class CoursePlacementQueries {
public:
    virtual ~CoursePlacementQueries() = default;
    // Ground height (1/4096 m) under (x, y) starting the chunk search at `chunk` (in/out) - 0x80028900.
    virtual int32_t GroundHeight(int32_t x, int32_t y, int32_t& chunk) = 0;
    // Distance along the course (16.16 m) of the world point (x, height, y in 16.16) in `chunk` - 0x80028C6C.
    virtual int32_t CourseDistance(int32_t chunk, int32_t x16, int32_t height16, int32_t y16) = 0;
    // Road under one wheel - 0x80028830.
    virtual void Contact(ContactQuery& query) = 0;
};

// Race-progress data of the game shell: 0x801C8568 -> +8 + line * 4 -> section list { s32 count; Section[count] }.
// Seven lists: 0 (unused in the dump), 1 / 2 (the two overtaking lines), 3 (the pit / formation line), 4 (the grid
// slots, also 0x801C8568 -> +0x18), 5 (the zone flags of the line change: type != 0 swaps lines 1 / 2), 6 (the
// main racing line). One record per section (a straight or an arc), 0x28 bytes; the AI driver (ai_driver.h)
// follows it by interpolating between consecutive records.
#pragma pack(push, 1)
struct RaceSection {
    int8_t type;             // +0x00  0 = straight (target speed = top speed), 1 / 2 = corner (0x80035948 target); the AI
                             //        reads the whole word (0x80035D68, 0x800360C8): bytes 1..3 are 0 in the game's lists
    uint8_t reserved01[3];
    int32_t x;               // +0x04  start point of the section, simulation plane 1/4096 m (0x80035D68)
    int32_t y;               // +0x08
    int32_t reserved0C;      // +0x0C  small signed values (not read by the ported routines)
    uint8_t reserved10[2];   // +0x10  chunk index in the dump's lists (not read)
    int8_t surface;          // +0x12  indexes the body's surface grip tables (0x348 setup, 0x358 AI)
    uint8_t reserved13;      // +0x13  2 on some records of the dump (not read)
    int32_t distance;        // +0x14  start of the section along the course, 16.16 m
    int32_t curvature;       // +0x18  signed radius of an arc section, 1/4096 m (0 for a straight); magnitude scales the lateral grip term
    int32_t gradient;        // +0x1C  angle units; 1 - sin(gradient) scales the AI's braking distance (0x8003643C)
    int32_t bank;            // +0x20  angle units; a bank against the curve reduces the target speed
    int32_t heading;         // +0x24  angle units: the tangent direction at the start point (an arc's centre is at -radius along its normal)
};
#pragma pack(pop)
static_assert(offsetof(RaceSection, x) == 0x04 && offsetof(RaceSection, surface) == 0x12 && offsetof(RaceSection, heading) == 0x24);
static_assert(sizeof(RaceSection) == 0x28);

struct RaceSectionList {
    int32_t count = 0;
    const RaceSection* sections = nullptr;
};

// Inputs of the per-race driver besides the per-car setup.
struct RaceStartInputs {
    CarSetupInputs setup;
    StepGlobals step;               // 0x801C856C / 0x801C8570 (SetStepTime, the shift timer of 0x80032E6C)
    CoursePlacementQueries* course = nullptr;
    int32_t chunkHint = 0;          // a2: nearest chunk to the grid position (0x80028288)
    int32_t x = 0, y = 0;           // a3, 5th: grid position in the simulation plane (1/4096 m)
    int32_t headingSin = 0, headingCos = 0; // 6th, 7th: sin / cos of the grid heading (4096 = 1.0)
    uint8_t controlClass = 0;       // 8th: 0 player, 2 AI -> body + 0x45D (forced to 1 for car 0 when 0x801C98A2 == 1)
    uint8_t transmission = 0;       // 9th: -> body + 0x642 for player cars (0 automatic, 1 manual)
    uint8_t contactType = 0;        // 10th: -> body + 0x45E
    uint8_t byte1C = 0;             // 11th: -> body + 0x1C
    uint8_t carIndex = 0;           // 12th: -> body + 0x45C
    int32_t gridOffset = 0;         // 13th: non-zero places the car `gridOffset` metres along the main line instead (0x80039040,
                                    // ai_driver.h LineStartPlacement: the 2 player Battle's Handicap Start); a negative one
                                    // (behind the line) starts the car in sector 0 of lap 0
    int32_t courseLength = 0;       // *(*(0x800B4A44)) (LineStartPlacement)
    bool pointToPoint = false;      // 0x80060E94(0x800AF230) + 8 & 0x20 (LineStartPlacement)
    // Globals.
    uint8_t raceMode = 2;           // 0x801D5866: 6 = ...; 3 / 6 disable the off-road flag; 6 with contactType 2 runs 0x8003EF40 (not ported)
    uint32_t word801C98A0 = 0;      // 0x801C98A0: byte 2 == 1 forces controlClass 1 for car 0; byte 0 != 0 (with 0x801D5869 == 0) runs 0x8003311C (not ported)
    uint8_t byte801D5869 = 0;
    const int32_t* startLineDistances = nullptr; // 0x800B4A58: { s32 count; s32 distance[count] } (16.16 m)
    int32_t startLineCount = 0;
    RaceSectionList sectionLists[7]; // 0x801C8568 -> +8 .. +0x20, indexed by the race state
    bool hasGridList = false;       // 0x801C8568 -> +0x18 != 0 (0x800392AC: body + 0x6B2 = 8 - car index)
    const int8_t* raceStateTable = nullptr; // 0x80046DD4: rows of 5 candidate states per requested state (0x800358E0)
    TyreWearConstants wear;         // 0x80046F48.. (0x80032A1C initialises the wheels from them)
};

// Places the body on the course at the grid position with the wheels on the road (0x80032B0C).
void PlaceCarOnCourse(CarBody& body, CoursePlacementQueries& course, int32_t chunkHint, int32_t x, int32_t y, int32_t headingSin, int32_t headingCos,
                      uint8_t raceMode);

// Resets of the dynamic state (0x80032E44 view state, 0x80032E6C body / wheel state with 0x80032AAC, 0x80032A1C tyre wear).
void ResetViewState(CarBody& body);
void ResetWheelState(CarBody& body);
void ResetDynamicState(CarBody& body, int32_t rate); // rate = 0x801C8570
void ResetTyreWear(CarBody& body, const TyreWearConstants& wear);

// 0x80035948: target speed of a cornering section of `list` (index `section`) from its curvature, banking and the
// car's tyre grip (surface grip table body + 0x348, wear-scaled unless body + 0x78D bit 4), at most the top speed
// and at least 0x58E8.
int32_t SectionTargetSpeed(const CarBody& body, const RaceSectionList& list, int32_t section, const DriveClassTuning* classTuning,
                           const TyreWearConstants& wear);
// 0x80035B68: the current section (body + 0x78A) becomes the previous one (type -> 0x78B, target speed -> 0x790)
// and its type / target speed are derived (0x78C, 0x794; a straight targets the top speed, a corner
// SectionTargetSpeed, types >= 3 keep the speed). An index equal to the count reads the last section.
void AdvanceSection(CarBody& body, const RaceSectionList& list, const DriveClassTuning* classTuning, const TyreWearConstants& wear);
// Race-progress initialisation (0x800367AC -> 0x80036340 with 0x80035948 / 0x80035B68): the line (body + 0x789),
// the section index (0x78A = first section starting beyond the car's course distance) and the target speeds of
// the current and next section. The AI driver calls it for every line change.
void InitRaceProgress(CarBody& body, uint8_t raceState, const RaceSectionList lists[7], const DriveClassTuning* classTuning,
                      const TyreWearConstants& wear);

// The per-race driver (0x80033384). Returns the value of 0x800319A8 (0). With 0x801D5869 == 0 and a start speed in
// the low byte of 0x801C98A0 it ends with RollingStart (0x8003311C).
uint32_t StartCar(CarBody& body, const RaceStartInputs& inputs);

// 0x8003311C(body, kmh): a rolling start at `kmh` km/h (race settings block +0, licence tests and events that start
// moving): forward speed kmh x 1138 (1/4096 m/s) along the body's heading (velocity, forwardSpeed, the wheels' rim
// and contact speeds, the axle speeds), the speed readouts (1/100 mph), the gear 0x800448C8 picks for the speed, the
// engine speed of that gear clamped to idle..rev limit and its rpm, the turbo spool at that rpm, clutch engaged.
void RollingStart(CarBody& body, int32_t kmh);

} // namespace gt2::sim
