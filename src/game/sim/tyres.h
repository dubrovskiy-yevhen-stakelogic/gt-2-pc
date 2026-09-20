#pragma once
#include <cstddef>
#include <cstdint>

#include "game/sim/car_body.h"

// Tyre forces and slip (original: 0x80039FC8, 0x80039DE8, 0x80039A4C, 0x80039778 and callees).
// Ported from the US Simulation v1.2 executable and verified bit for bit by tools/gt2verify (verify_tyres.cpp).
//
// Units used throughout: velocities in 1/4096 m/s, angles in 4096 units per turn, ratios and factors with
// 0x1000 = 1.0 unless stated otherwise. Offsets in comments are relative to the body (car + 0x2C), the wheel
// record (body + 0x460 + wheel * 0x68) or the per-axle tyre block (body + 0x194 + axle * 0xD8).
namespace gt2::sim {

// Piecewise-linear curve with 16-bit samples: the original's table object is { u16 count; s16* xs; s16* ys }.
struct CurveS16 {
    const int16_t* xs = nullptr;
    const int16_t* ys = nullptr;
    uint32_t count = 0;
};

// Same with 32-bit samples ({ u16 count; s32* xs; s32* ys }), evaluated by sim::Interpolate (fixed.h).
struct CurveS32 {
    const int32_t* xs = nullptr;
    const int32_t* ys = nullptr;
    uint32_t count = 0;
};

// Integer square root with `extraBits` additional result bits: floor(sqrt(value << (2 * extraBits))).
// A negative `value` is treated as unsigned (original: 0x80081288, the PsyQ-style GTE-normalised SquareRoot).
int32_t SquareRoot(int32_t value, uint32_t extraBits);

// Lookup in a 16-bit curve with 32-bit arithmetic and MIPS division; clamps outside the range, linear scan
// from the left. `x` is sign-extended from 16 bits by the original (0x8003D848).
int32_t InterpolateS16(const CurveS16& curve, int16_t x);

// Lookup of two curves sharing `curve.xs`: `y` from `curve.ys`, `secondY` from `secondYs` (0x8003D940; the
// original takes a second table object of which only the ys pointer is read).
void InterpolateS16Pair(const CurveS16& curve, const int16_t* secondYs, int16_t x, int16_t& y, int16_t& secondY);

// Odd-symmetric evaluation of a curve defined on 0..0x400 for an angle-like input: the input is folded into
// 0..0x400 (x -> -x, 0x800 - x, x + 0x800) and the result negated for negative inputs (0x80039F4C).
int32_t SymmetricCurve(const CurveS16& curve, int32_t x);

// The tyre curves of one axle (body + 0x194 + axle * 0xD8). Resolved from the table objects by the caller;
// in the original the objects hold guest pointers into the car data.
struct AxleTyreCurves {
    CurveS16 slipAngleForce;          // +0x00  xs = slip angle 0..0x400, ys = lateral force factor (0x80039F4C)
    CurveS16 slipRatioForce;          // +0x2C  xs = slip ratio -0x1000..0x1000, ys = longitudinal force factor
    const int16_t* slipRatioGrip = nullptr; // +0x6C  ys of the second slip-ratio curve (same xs): lateral grip factor
    CurveS32 loadGrip;                // +0x90  xs = vertical load, ys = grip force capacity
    CurveS16 camberGrip;              // +0xBC  xs = |camber| in angle units, ys = grip factor
};

// Tyre wear tuning constants (the overlay's block 0x80046F48..0x80046F60; all zero in the attract race, which
// disables wear). Wear is a per-wheel s32 (wheel + 0x64) that grows with slip * load (0x80039FC8, last pass).
struct TyreWearConstants {
    int32_t wearLimit = 0;      // 0x80046F48  wear at which the tyre is fully worn; 0 disables wear entirely
    int32_t wornGripLoss = 0;   // 0x80046F4C  grip loss (of 0x1000) at the wear limit
    int32_t pitGripFactor = 0;  // 0x80046F50  grip factor below which 0x80039FC8 raises the pit flag (car + 0x7B9 bit 2)
    int32_t coldLimit = 0;      // 0x80046F54  negative wear below which the factor is pinned at 1.0 (stage -128)
    int32_t coldGripLoss = 0;   // 0x80046F58  grip loss at zero from coldLimit (the factor is 1 - loss * wear / coldLimit)
    int32_t wearKnee = 0;       // 0x80046F5C  wear at which the loss reaches kneeGripLoss (first, steeper segment)
    int32_t kneeGripLoss = 0;   // 0x80046F60  grip loss at the knee
};

// The physics step's per-wheel work record in the scratchpad (0x1F800004 + car * 0x90 + wheel * 0x1C).
#pragma pack(push, 1)
struct WheelScratch {
    int32_t load;            // +0x00  vertical load scaled by the axle factor (written by 0x80039FC8)
    int32_t driveForce;      // +0x04  longitudinal force when the wheel drives the road (slipMode >= 0)
    int32_t brakeForce;      // +0x08  ... when the road drives the wheel (slipMode < 0)
    int32_t reserved0C;
    int8_t slipMode;         // +0x10  +1: rim faster than the road (traction), -1: road faster (braking / locked)
    int8_t reserved11;
    int8_t slipSign;         // +0x12  sign of the slip ratio before clamping
    int8_t reserved13;
    int16_t slipRatio;       // +0x14  -0x1000..0x1000, faded in below 5.6 m/s (0x80039490)
    int16_t slipForce;       // +0x16  longitudinal force factor from the slip-ratio curve (0x800397D0)
    int16_t steerSin;        // +0x18  sin of the wheel's steer angle (written by the tick, 0x8003E0C4)
    int16_t steerCos;        // +0x1A  cos of the wheel's steer angle
};

// The per-car work block of the scratchpad (0x1F800004 + car * 0x90), as the tyre code sees it. The drivetrain
// pass reads the same block as DriveCarWork (drive_shafts.h): its axleTorque[2] are the two words at +0x70.
struct CarScratch {
    WheelScratch wheels[4];      // +0x00
    int32_t axleTorque[2];       // +0x70  drive torque delivered to the front / rear axle (drive shafts)
    int32_t clutchInputSpeed;    // +0x78  engine speed at the start of the clutch engagement (0x80039FC8 pass H)
    int32_t clutchOutputSpeed;   // +0x7C  engine speed the wheels impose through the gear
    int32_t inverseEffectiveMass; // +0x80  inverse of the effective mass of this step (0x80039FC8)
    int32_t rollingResistance;   // +0x84  rolling resistance force along forwardSpeed (0x80039FC8)
    int32_t clutchEngagement;    // +0x88  0..0x1000 of the clutch-start mode (drive shafts)
    int16_t slideMeasure;        // +0x8C  yaw deficit against the steady-state yaw (0..0x1000), written by the tick
    int16_t slideAngle;          // +0x8E  drift angle weighted by the speed (0..0x1000)
};
#pragma pack(pop)
static_assert(sizeof(WheelScratch) == 0x1C);
static_assert(sizeof(CarScratch) == 0x90);
static_assert(offsetof(CarScratch, clutchInputSpeed) == 0x78);
static_assert(offsetof(CarScratch, inverseEffectiveMass) == 0x80);
static_assert(offsetof(CarScratch, rollingResistance) == 0x84);
static_assert(offsetof(CarScratch, clutchEngagement) == 0x88);
static_assert(offsetof(CarScratch, slideMeasure) == 0x8C);

constexpr uint32_t kScratchCarBlocks = 0x4; // offset of CarScratch[0] inside the 1 KB scratchpad

// Slip ratio of every wheel of one car from the rim speed (wheel + 0x18) and the contact patch velocity
// (wheel + 0x2C forward, + 0x30 lateral) projected on the steered rolling direction. Writes the scratch
// record's slipMode / slipSign / slipRatio and wheel + 0x44 (original: 0x80039490, called with (body, car)).
void UpdateWheelSlipRatios(CarBody& body, CarScratch& scratch);

// The same for `count` cars (original: 0x80039778; `scratch` is the array of per-car blocks).
void UpdateSlipRatios(CarBody* const bodies[], uint32_t count, CarScratch scratch[]);

// Evaluates the slip-ratio curves of every wheel: scratch slipForce and wheel + 0x46 from the two curves,
// wheel + 0x3A = |slip ratio| (original: 0x800397D0, per car).
void EvaluateSlipCurves(CarBody& body, CarScratch& scratch, const AxleTyreCurves curves[2]);

// Slip angle (wheel + 0x50) and its blend factor (wheel + 0x52: 0 below 0.28 m/s, 1.0 above 2.78 m/s) from
// the contact patch velocity (original: 0x80039DE8, per car).
void UpdateSlipAngles(CarBody& body);

// Tyre wear grip factor (wheel + 0x38) and wear stage (wheel + 0x3F) from the wear (wheel + 0x64), then the
// grip force capacity (wheel + 0x34) from the load, camber and the surface under the wheel (wheel + 0x14
// indexes the per-surface grip at body + 0x348; original: 0x80039A4C, per car).
void UpdateTyreWearAndGrip(CarBody& body, const AxleTyreCurves curves[2], const TyreWearConstants& constants);

} // namespace gt2::sim
