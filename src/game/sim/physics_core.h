#pragma once
#include <cstddef>
#include <cstdint>

#include "game/sim/car_body.h"
#include "game/sim/car_contact.h"
#include "game/sim/drive_shafts.h"
#include "game/sim/drivetrain.h"
#include "game/sim/tyres.h"

// The physics core of one simulation step (original: 0x8003E0C4 with the tyre-force routine 0x80039FC8) and
// the step driver 0x80034480 (move pass, contact passes, after-move fix-ups).
// Ported from the US Simulation v1.2 executable and verified bit for bit by tools/gt2verify (verify_core.cpp).
//
// The scratchpad (0x1F800000) is the step's work area. The tick writes the current car's step time at +0 before
// every per-car routine, the per-car work blocks follow at +4 (DriveStepWork, drive_shafts.h; the tyre code sees
// the same blocks as CarScratch, tyres.h), the gear requests sit at +0x364 and the displacements at +0xB4
// (aliasing the work blocks, which are dead by the time they are written). The port keeps the three as
// separate objects; the verification tool maps them onto the guest's scratchpad.
//
// Offsets in the comments are relative to the body (car + 0x2C), the wheel record (body + 0x460 + wheel *
// 0x68) or the car's scratchpad block (0x1F800004 + car * 0x90).
namespace gt2::sim {

constexpr size_t kMaxCars = 6;

// Curve objects of one car's data, resolved from the table objects (guest pointers) by the caller.
struct CarCurves {
    AxleTyreCurves axles[2];   // body + 0x194 + axle * 0xD8 (tyres.h)
    CurveS32 engineTorque;     // body + 0x7C: engine speed (rev/s << 12) -> torque (0x8003E0C4 writes body + 0x710)
    CurveS16Ref steerLimit;    // body + 0x3C: speed -> steering limit (UpdatePlayerInput)
};

// One driving-aid record of the AI parameter table (resident data 0x801C8690 + index * 0x28, index = body +
// 0x1D). Only the fields at +0x18..+0x23 are read by the physics core.
struct AiAidTuning {
    int16_t yawBrakeGain = 0;          // +0x18  ApplyBrakeAssist gain
    int16_t yawBrakeThreshold = 0;     // +0x1A  ... threshold
    int16_t steerFalloffGain = 0;      // +0x1C  Falloff of the yaw acceleration (body + 0x64A): gain
    int16_t steerFalloffThreshold = 0; // +0x1E  ... threshold
    int32_t tractionGain = 0;          // +0x20  ApplyTractionControl gain (12.12)
};
constexpr size_t kAiAidEntries = 4;    // 0x801C8690 .. 0x801C8730 (the rolling-resistance curve object follows)

// Everything the original reads from outside the car array: globals, overlay constants, the game shell's
// answers, and the two hooks into other subsystems (AI driver, sound).
struct PhysicsContext {
    // Track, step globals, collision flag (0x800A9520), course type and the shell's control class
    // (0x800418E8: depends on game-mode globals only). The per-body query 0x800419E8 is ControlClass() below.
    MoveContext move;
    CarContactState* contact = nullptr;      // 0x801C8608 (car_contact.h)
    DrivetrainGlobals drivetrain;            // 0x801D5866 (drive_shafts.h)
    TyreWearConstants wear;                  // 0x80046F48.. (tyres.h)
    const CarCurves* curves = nullptr;       // one per car
    const InputTuning* input = nullptr;      // one per car (the pad rates are indexed by the car in the original)
    CurveS32 rollingResistance;              // 0x801C8730: |forward speed| -> rolling-resistance factor (0x1000 = 1.0)
    const int32_t* surfaceRolling = nullptr; // 0x80046E00: s32[8] rolling-resistance coefficient per surface type
    AiAidTuning aiAids[kAiAidEntries];       // 0x801C8690 + index * 0x28
    uint8_t slideSensitivity = 100;          // 0x80046EE8 (ApplyBrakeAssist)
    uint8_t gameMode = 2;                    // 0x801D5866: 6 = the mode in which 0x8003FAEC can hold a car, 3/6 = no pit flags
    uint16_t holdFrames = 0;                 // 0x800A9520 (u16): non-zero while the cars are held (start countdown)
    uint8_t scriptedHold = 0;                // 0x8003FAEC(): extra hold flag of game mode 6 (not exercised by the dump)

    // Hooks. aiInput is raised where the original calls the AI driver (0x80037834 / 0x800377E8 / 0x800372EC after
    // PrepareAiInput); soundEvent where it calls 0x800156B8(car index, 1) for a hard wall impact.
    void* user = nullptr;
    void (*aiInput)(void* user, CarBody& body, int car, const AiDispatch& dispatch) = nullptr;
    void (*soundEvent)(void* user, int car, int event) = nullptr;
};

// Control class of one body (original: 0x800419E8): 0 while the body's flags 0x45D / 0x786 are set, else the
// shell's class (context.move.controlClass). 2 = player-controlled race car (damage, pit flags).
int32_t ControlClass(const PhysicsContext& context, const CarBody& body);

// ---- small callees ported here ----
// Clears the per-step neighbour / slipstream fields of a body: draftInput (0x774), the neighbour class (0x76A)
// and the eight neighbour flags (0x76C..0x773). Original: 0x8003360C.
void ClearNeighbourFields(CarBody& body);
// Single-gear cars (body + 0x372 == 1): clutch state from the clutch request (0x80039470), engine rpm from the
// engine speed (0x80039994), forward / reverse selection from the speed, throttle and the reverse request
// (0x800399C4; also used by the clutch-start mode 2, where it marks the clutch as engaging).
void SingleGearClutch(CarBody& body);
void SingleGearRpm(CarBody& body);
void SingleGearSelect(CarBody& body, const GearRequest& request);
// |a| + ... cheap length of a 3-vector: largest + (sum of the other two) / 4 (0x8003C398).
int32_t ApproxLength3(int32_t a, int32_t b, int32_t c);
// Marks where a neighbour is (body + 0x76C..0x773: 2 = the sector it is in, 1 = the adjacent sector) from its
// position in this car's frame and the summed extents of the two cars (0x80033634).
void ClassifyNeighbour(CarBody& body, int32_t along, int32_t across, int32_t frontSum, int32_t rearSum, int32_t widthSum);
// One car pair (a before b in the array): neighbour sectors, the AI's follow class (body + 0x76A) and the
// slipstream request (body + 0x774) for both cars (0x8003373C).
void UpdateCarPairAwareness(CarBody& a, CarBody& b);
// After the contact resolution: refreshes the footprint and the corner tables of every car that touched another
// car in this step (0x80040F30; contactFlags bit 1).
void RefreshContactCorners(CarContactState& state, Car* cars, int count);
// Push forces between overlapping cars into body + 0x778 / 0x77C (consumed by the next step's tyre forces);
// original 0x800412D4.
void ComputeContactPush(const CarContactState& state, Car* cars, int count);

// ---- the three routines of the step ----
// Tyre forces, longitudinal / lateral / yaw integration, drivetrain, engine and gearbox, tyre wear and pit
// flags for `count` cars (original: 0x80039FC8). `work` is the step's work area (see the header comment),
// `requests` the gear requests written by the input routines.
void TyreForces(const PhysicsContext& context, Car* cars, int count, DriveStepWork& work, const GearRequest* requests);

// The physics core of the step (original: 0x8003E0C4): timers, step time, aero, engine curves, tyre grip and
// slip, driver input (player from `pads[car]`, AI through the hook), wheel steer angles, slide measures,
// driver aids, TyreForces and the displacements (`deltas[car]` = x, y, height, heading) for the move pass.
void PhysicsCore(const PhysicsContext& context, Car* cars, int count, const PadRecord* pads, DriveStepWork& work, GearRequest* requests,
                 int32_t (*deltas)[4]);

// The step driver after the physics core (original: 0x80034480): move pass with wall collision, car-to-car
// contact (proximity, sweep, resolution, corner refresh, push forces), pairwise awareness, impact sound and
// timers. Needs context.move.track and context.contact.
void SimulateCars(const PhysicsContext& context, Car* cars, int count, int32_t (*deltas)[4]);

} // namespace gt2::sim
