#pragma once
#include <cstdint>

#include "game/sim/car_body.h"
#include "game/sim/tyres.h"

// Engine, clutch, gearbox, driver aids and driver input (original: 0x80074F24, 0x80075074, 0x8003533C, 0x800353DC,
// 0x8003932C, 0x8003941C, 0x800449C8, 0x8003991C, 0x8003DE68, 0x8003DBE8, 0x8002FB18, 0x80038540 and callees).
// Ported from the US Simulation v1.2 executable and verified bit for bit by tools/gt2verify (verify_drivetrain.cpp;
// the engine routines by verify_drive_shafts.cpp).
//
// Units: speeds in 1/4096 m/s, angular speeds in rad/s << 12, engine speed in rev/s << 12 (engineSpeed) or rpm
// (engineRpm), pedals and factors with 0x1000 = 1.0, angles in 4096 units per turn. The fields are the named
// members of CarBody / Wheel / AxleTyreParams (car_body.h). The slip thresholds are the tyre blocks' peak slip
// ratios: peakSlipRatioNeg (wheel spin side) for the upshift check and the traction control, peakSlipRatioPos
// (locking side) for the downshift check and the anti-lock pass.
namespace gt2::sim {

// Piecewise-linear curve with 16-bit samples ({ u16 count; s16* xs; s16* ys }), evaluated by InterpolateS16
// (tyres.h, original 0x8003D848).
using CurveS16Ref = CurveS16;

// The 12-byte pad record filled by 0x8003C250 from the controller or the replay stream and consumed by
// MapPadInput (0x8003E020) and UpdatePlayerInput (0x8002FB18). 0x8003C250(record, car) zeroes the record and
// dispatches on the car's control source (s16 car + 0x18): 1 = live pad (0x80014074 after the pad-state checks
// of 0x80012360 / 0x8001252C), 2 = replay (0x80013EF0), 3 = 0x80014030; all end in 0x80013C90, which packs
// the pad into a 5-byte frame { flags, buttons, steer axis, throttle axis >> 4, brake axis >> 4 } for the
// replay stream (or reads one from it) and expands the frame into this record: analogue pedals through the
// 16-entry table at 0x8002F4D4 (0, 273, 546, ... 4096), digital pedals as 0 / 1 from the button bits 4 / 8,
// shift from bits 0x40 / 0x80, reverse from 0x20, handbrake from 0x10, the curve flag from bit 0x400.
struct PadRecord {
    uint16_t flags;      // +0  bit 0: `steer` is analogue (-0x1000..0x1000), else digital (sign only)
                         //     bit 1: `throttle` is analogue, bit 2: `brake` is analogue
                         //     bit 3: use the non-linear stick curve (InputTuning::steerCurve) for analogue steering
    int16_t steer;       // +2  analogue: (0x80 - stick) * 0x20; digital: +2 left, -2 right
    uint16_t brake;      // +4  analogue: 0..0x1000 from the pedal table; digital: 0 / 1
    uint16_t throttle;   // +6  analogue: 0..0x1000 from the pedal table; digital: 0 / 1
    int8_t shift;        // +8  +1 shift up, -1 shift down (both pressed: 0)
    uint8_t reverse;     // +9  reverse button (full throttle in reverse)
    uint8_t handbrake;   // +10
    uint8_t reserved;    // +11
};
static_assert(sizeof(PadRecord) == 12);
constexpr uint16_t kWheelPad = 0x100;
constexpr uint16_t kWheelIgnoreShiftSpeed = 0x200;
// Applies only to native wheel frames, after the original pad mapping.
void ApplyWheelInput(CarBody& body, const PadRecord& pad, struct GearRequest& request);
bool WheelNeutral(const CarBody& body, const struct GearRequest& request);
bool WheelDirectionBlocked(const CarBody& body, const struct GearRequest& request);

// Per-car gear request in the scratchpad (0x1F800364 + car * 4), written by the input routines and read by
// the gear selection in the same step.
struct GearRequest {
    uint8_t reverse;     // +0  select reverse (gear 0)
    int8_t shift;        // +1  manual gearbox: +1 up, -1 down
    uint8_t reserved[2];
};
static_assert(sizeof(GearRequest) == 4);
inline uint8_t WheelClutch(const GearRequest& request) {
    return (request.reserved[1] & 0x80) ? uint8_t(((request.reserved[1] & 15) << 4) | (request.reserved[0] >> 4)) : 0;
}

// Tuning constants of the input mapping (overlay data 0x80046D7C..0x80046F44). The per-car rates are indexed
// by the car index in the original (u16 tables at 0x80046DB0.. with one entry per player).
struct InputTuning {
    int32_t steerSpringGain = 409600;   // 0x80046F3C  digital steering: pull towards the target per unit of error
    int32_t steerDamping = 20480;       // 0x80046F40  ... minus this times the current steering rate
    int32_t steerCentring = 204800;     // 0x80046F44  extra return-to-centre pull when the target is 0
    CurveS16Ref steerCurve;             // 0x80046DA4  |stick| 0..0x1000 -> steering fraction (pad flag bit 3)
    uint16_t throttleRise = 1365;       // 0x80046DB0 + car * 2  digital throttle: pedal change per step
    uint16_t throttleFall = 1365;       // 0x80046DB4 + car * 2
    uint16_t brakeRise = 1365;          // 0x80046DB8 + car * 2
    uint16_t brakeFall = 1365;          // 0x80046DBC + car * 2
    uint16_t handbrakeRise = 4096;      // 0x80046DC0 + car * 2
    uint16_t handbrakeFall = 4096;      // 0x80046DC4 + car * 2
};

// Engine speed the driven wheels impose through the current gear, rev/s << 12 (original: 0x8003932C).
// Takes the axle speed selected by the drive layout (body + 0x370), reversed in gear 0, and returns 0 when the
// wheels turn backwards relative to the gear.
int32_t EngineSpeedFromWheels(const CarBody& body);

// body + 0x6AC = engine rpm from the engine speed (rev/s << 12 * 60), at least idle - 1 (original: 0x8003941C).
void UpdateEngineRpm(CarBody& body);

// Gear the gearbox should be in this step, or -1 for no change (original: 0x800449C8). Automatic shifting by
// rpm and wheel slip on the driven axle(s), manual shifting from `request.shift` when body + 0x642 == 1, reverse
// from `request.reverse`. Maintains the shift timer (body + 0x61C).
int32_t SelectGear(CarBody& body, const GearRequest& request);

// Applies SelectGear: writes the gear and, unless the clutch is open, marks it as engaging (body + 0x619 = 3
// for a change between forward gears, 2 when reverse is involved); original: 0x8003991C.
void UpdateGear(CarBody& body, const GearRequest& request);

// ---- engine ----
// Boost factor of the turbo model, 0x1000 + the contributions of both turbos of the engine block (body + 0x7C)
// at the two inputs (engine rpm, or the turbos' current spool states): each turbo contributes its boost shaped
// by its model below its spool rpm and a quadratic droop above it, nothing at three times the spool rpm and
// beyond (original: 0x80074F24).
int32_t TurboBoostSum(const CarBody& body, int16_t spool0, int16_t spool1);
// TurboBoostSum limited by the engine's boost cap + 0x1000 (original: 0x80075074).
int32_t BoostMultiplier(const CarBody& body, int16_t spool0, int16_t spool1);
// Torque of the naturally aspirated engine at `throttle` (0..0x1000) minus the engine braking torque; negative
// values are engine braking. Toggles the rev limiter flag (revLimiterActive) with a hysteresis of 500 rpm
// (original: 0x8003533C).
int32_t BaseEngineTorque(CarBody& body, int32_t throttle);
// Engine torque of this step including the turbo model (original: 0x800353DC). Cars without a turbo (boostCap
// == 0) pass the throttle straight to BaseEngineTorque; turbo cars integrate the spool states with `stepTime`
// (16.16 s, the scratchpad's word 0), scale the throttle by the boost and update the intake / blow-off /
// exhaust flame values.
int32_t EngineTorqueStep(CarBody& body, int32_t throttle, int32_t stepTime);

// Traction control: scales the effective throttle (body + 0x708) down when the least-gripping driven wheel is
// below the axle's slip threshold (original: 0x8003DE68). `gain` (12.12, body + 0x450 for the player) is the
// throttle cut per unit of slip; `gripFalloff` (0..0x1000) scales the threshold; `steerFactor` (0..0x1000)
// reduces the gain. Only acts with the clutch engaged, positive engine torque and a non-zero throttle.
void ApplyTractionControl(CarBody& body, int32_t gain, int32_t gripFalloff, int32_t steerFactor);

// Brake assists: an anti-lock pass over the four wheel brake inputs (wheel + 0x60) and a stability pass that
// adds brake to one front wheel while the car slides (original: 0x8003DBE8). Reads the car's slide measures
// from the scratchpad (0x1F800090 / 0x1F800092 + car * 0x90 where car = body + 0x45C). `slideSensitivity` is
// the overlay constant 0x80046EE8 (percent, 100 in the attract race); `yawBrakeGain` / `yawBrakeThreshold`
// come from body + 0x458 / 0x45A for the player or from the AI table.
void ApplyBrakeAssist(CarBody& body, const uint8_t* scratch, int32_t yawBrakeGain, int32_t yawBrakeThreshold, uint8_t slideSensitivity);

// Clutch engagement request from the pedals and the engine state (original: 0x8002F92C, called for cars with
// more than one forward gear).
void UpdateClutchRequest(CarBody& body);

// Driver input of the human (or replayed) car: steering limits from the speed and the front slip angles,
// steering angle from the pad, pedals with digital ramp rates, the gear request and the clutch request
// (original: 0x8002FB18). `stepTime` is the step in 16.16 s (scratchpad word 0), `steerLimitCurve` the body's
// speed -> steering limit table (body + 0x3C, a table object in the car data).
void UpdatePlayerInput(CarBody& body, const PadRecord& pad, GearRequest& request, int32_t stepTime, const InputTuning& tuning,
                       const CurveS16Ref& steerLimitCurve);

// What the AI input routine hands over to after preparing the body (original: 0x80038540).
enum class AiControl : uint8_t {
    None,      // nothing else (single-gear car with the clutch open outside the finished state)
    Drive,     // 0x80037834: the racing line follower (inputs from the course's AI data)
    Finished,  // 0x800377E8: the car has finished (body + 0x786 == 7), coast to a stop
    Scripted,  // 0x800372EC(body, car, argument): timed input script (start countdown / penalty)
};
struct AiDispatch {
    AiControl control = AiControl::None;
    int32_t argument = 0; // Scripted: frames left (body + 0x78E, or the race frame counter minus the rate)
};

// AI car input: clears the handbrake and the gear request, sets the clutch request from the rpm (open until
// three quarters of the upshift rpm while the clutch is open, closed above idle otherwise) and returns which
// AI routine the original calls next (original: 0x80038540, the part that acts on the body). `raceFrame` is
// the u16 at 0x800A9520 and `rate` the simulation rate (0x801C8570), only used for the scripted argument.
AiDispatch PrepareAiInput(CarBody& body, GearRequest& request, int32_t raceFrame, int32_t rate);

} // namespace gt2::sim
