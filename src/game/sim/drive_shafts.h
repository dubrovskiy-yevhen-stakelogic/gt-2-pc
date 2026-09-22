#pragma once
#include <cstddef>
#include <cstdint>

#include "game/sim/car_body.h"

// Wheel-rotation drivetrain pass of the physics step (original: 0x80046B58 and callees): road torques on the
// wheels, the engine and clutch, the drive shafts (axle speeds) and the axle differentials (wheel speeds). (The
// pass was first labelled "suspension" when the call tree was split up; springs and dampers are in ground.*.)
// Ported from the US Simulation v1.2 executable and verified bit for bit by tools/gt2verify (verify_drive_shafts.cpp).
//
// The scratchpad (0x1F800000) is the step's work area. The callers write the current car's step time at +0,
// per-car blocks of 0x90 bytes follow at +4. DriveStepWork mirrors that layout so the port can run on a copy
// of the original's scratchpad.
namespace gt2::sim {

#pragma pack(push, 1)
struct DriveWheelWork {              // 0x1C bytes, one per wheel (FL, FR, RL, RR)
    int32_t brakeTorque;             // +0x00 brake torque magnitude (non-zero = brakes applied), set by the tyre code
    int32_t roadForce[2];            // +0x04 two longitudinal road-force terms (1/4096 N); their sum times the wheel
                                     //       radius is the road torque on the wheel (0x80045688)
    int32_t torque;                  // +0x0C net torque on the wheel this step, written by 0x80045688 and biased by
                                     //       0x800457B0 / 0x800465E0, integrated into the wheel speed by 0x800465E0
    uint8_t reserved10[8];
    int16_t steerCos;                // +0x18 cos of the wheel steer angle (tyre code)
    int16_t steerSin;                // +0x1A sin of the wheel steer angle (tyre code)
};

struct DriveCarWork {                // 0x90 bytes at 0x1F800004 + car * 0x90
    DriveWheelWork wheels[4];        // +0x00
    int32_t axleTorque[2];           // +0x70 drive torque delivered to the front / rear axle (0x80045AE8 -> 0x800465E0)
    int32_t clutchInputSpeed;        // +0x78 speed on the engine side of the clutch; read only (0x80045AE8)
    int32_t clutchOutputSpeed;       // +0x7C speed on the gearbox side of the clutch; read only (0x80045AE8, 0x80045138)
    int32_t reserved80;
    int32_t reserved84;
    int32_t clutchEngagement;        // +0x88 0..0x1000 clutch engagement of the clutch-start mode (0x80045138 writes,
                                     //       0x80045AE8 scales the reflected engine inertia by it)
    int16_t activeDiffFactor;        // +0x8C factor (of 0x1000) of the active differential torque bias (0x800457B0 reads)
    int16_t reserved8E;
};

struct DriveStepWork {
    int32_t stepTime;                // +0x00 step time (16.16 s) of the car being processed
    DriveCarWork cars[7];            // +0x04 (7 blocks fit in the 1 KB scratchpad)
};
#pragma pack(pop)

static_assert(sizeof(DriveWheelWork) == 0x1C);
static_assert(sizeof(DriveCarWork) == 0x90);
static_assert(offsetof(DriveStepWork, cars) == 4);
static_assert(sizeof(DriveStepWork) <= 0x400);

// Globals of the original read by this pass.
struct DrivetrainGlobals {
    uint8_t raceModeByte = 2;        // u8 at 0x801D5866: the locked 4WD coupling integrates without the previous
                                     // acceleration when this equals 3 and the car is drive type 5 (0x80045AE8)
    uint8_t wheelClutch[7] = {};    // Native input only: pedal depression 0..255 per car.
};

// Marker returned by CentreCouplingFraction: "fully locked" (INT32_MAX in the original).
constexpr int32_t kCouplingLocked = 0x7FFFFFFF;

// ---- angle helpers (4096 units per turn) ----
// Wraps an angle into [-0x800, 0x800) (original: 0x800450A0).
int32_t WrapAngle(int32_t angle);
// Shortest signed difference a - b of two angles, in [-0x800, 0x800) (original: 0x800450E0).
int32_t AngleDifference(int32_t a, int32_t b);

// ---- engine modes of the drive shafts (the plain engine routines are in drivetrain.h) ----
// Engine step of single-gear cars (forwardGears == 1): the throttle is chosen to hold a target engine speed
// derived from the forward speed; returns the drive torque minus a speed-dependent loss (original: 0x8004530C).
int32_t GovernedEngineStep(CarBody& body, int32_t stepTime);
// Engine step of two-gear cars (forwardGears == 2, clutch start): eases the clutch engagement of the car's work
// block (indexed by carIndex like the original) and returns the engaged torque (original: 0x80045138).
int32_t ClutchStartEngineStep(CarBody& body, DriveStepWork& work, int32_t stepTime);
// Clutch slip factor 0x1000..0x1200 from the speed difference across the clutch (original: 0x800392D8).
int32_t ClutchSlipFactor(int32_t speedDifference);
// Fraction (of 0x1000) of the shaft torque that a drive-type-5 centre coupling passes to the secondary axle,
// or kCouplingLocked (original: 0x800459A8; the original also receives the front torque but never reads it).
int32_t CentreCouplingFraction(const CarBody& body, int32_t shaftTorque, int32_t rearTorque);

// ---- the four passes of 0x80046B58, each for one car (the originals loop over the car array) ----
// Road torque on each wheel: -(radius * road forces) minus the brake torque against the spin (0x80045688).
void ComputeWheelTorques(CarBody& body, DriveStepWork& work, size_t car);
// Active differential (axle diff types 6, 7): shifts torque between the wheels of an axle towards the target
// speed above 10 km/h (0x800457B0).
void ApplyActiveDifferentialBias(CarBody& body, DriveStepWork& work, size_t car);
// Engine/clutch -> gearbox -> centre coupling -> axle speeds; leaves the axle drive torques in the work block
// (0x80045AE8).
void UpdateDriveShafts(CarBody& body, DriveStepWork& work, size_t car, const DrivetrainGlobals& globals);
// Axle differentials: wheel speeds from the axle speed and the wheel torques (0x800465E0).
void UpdateAxleDifferentials(CarBody& body, DriveStepWork& work, size_t car);

// The whole pass over `count` bodies (original: 0x80046B58 on the car array, count = a1). Runs each of the four
// passes over all cars before the next one, like the original.
void UpdateDrivetrain(CarBody* const* bodies, size_t count, DriveStepWork& work, const DrivetrainGlobals& globals);

} // namespace gt2::sim
