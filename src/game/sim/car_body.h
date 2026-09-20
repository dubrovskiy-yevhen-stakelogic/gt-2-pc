#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "gt2formats/track.h"

// Simulation state of one car ("body"), laid out exactly like the original so that ported routines can be
// verified on snapshots of the original's memory (tools/gt2verify) and later evolve freely.
// Every field a ported routine reads or writes is named; bytes nobody touches are reserved space. Offsets are
// relative to the body, which sits at car + 0x2C in the original's car object (re/structs/car.yaml, which also
// records the evidence for each name). Where two subsystems saw the same field under different names, the name
// of the code that WRITES it won and the alias is noted in the comment.
//
// Units unless stated otherwise: lengths 1/4096 m, speeds 1/4096 m/s, angles 4096 units per turn, factors and
// pedals with 0x1000 = 1.0, engine speed rev/s << 12, angular axle speed rad/s << 12, times 16.16 s.
namespace gt2::sim {

#pragma pack(push, 1)
struct Wheel { // 0x68 bytes, four of them at body + 0x460: front-left, front-right, rear-left, rear-right
    int32_t surfaceHeightAvg;        // 0x000  mean of the last two surface heights (ground.cpp ApplyContacts)
    int32_t surfaceHeight;           // 0x004  surface height under the wheel, 16.16 m
    int32_t load;                    // 0x008  vertical load; 0 = wheel in the air (also the "contact" test)
    int16_t steerAngle;              // 0x00C  steer angle of the wheel
    int16_t reserved00E;
    int16_t travel;                  // 0x010  suspension travel, negative = compressed
    int16_t travelVelocity;          // 0x012
    uint8_t surface;                 // 0x014  surface type under the wheel (0 tarmac, 1 kerb, 2.. loose); indexes surfaceGrip
    uint8_t surfaceAttribute;        // 0x015  polygon attribute copied from the contact query
    int16_t accel;                   // 0x016  wheel speed change of the last drivetrain step (halved into the next one)
    int32_t rimSpeed;                // 0x018  circumferential speed of the tyre
    uint8_t skidLevel;               // 0x01C  effect levels written by the ground pass (sound / particles)
    uint8_t smokeLevel;              // 0x01D
    uint8_t skidDelay;               // 0x01E  counts up to the skid delay before the skid is reported
    uint8_t dustLevel;               // 0x01F
    uint16_t rotation;               // 0x020  rolling angle for the render
    uint8_t damage;                  // 0x022  accumulated wall-impact damage of this corner, 0..255
    uint8_t reserved023;
    int16_t offsetX;                 // 0x024  planar offset of the wheel from the body reference
    int16_t offsetY;                 // 0x026
    int16_t offsetHeight;            // 0x028
    int16_t slipScale;               // 0x02A  multiplier of the raw slip ratio (low-speed scale, 0x800..0x1000)
    int32_t contactForwardSpeed;     // 0x02C  contact patch velocity along the car
    int32_t contactLateralSpeed;     // 0x030  ... across the car
    int32_t gripForce;               // 0x034  grip force capacity of the tyre (load, camber, surface, wear)
    int16_t wearGrip;                // 0x038  grip factor left after wear (0x1000 = fresh)
    int16_t slipRatioAbs;            // 0x03A  |slip ratio|
    int16_t lateralFactorAbs;        // 0x03C  |lateral force factor| (tyre forces)
    uint8_t wearRate;                // 0x03E
    int8_t wearStage;                // 0x03F  wear stage for the display, -128..0x7F
    int16_t speedFactor;             // 0x040  min(1.0, |contact velocity| * 0x52 / 0x1000)
    int16_t roughness;               // 0x042  surface bump offset added to the travel
    int16_t slipRatio;               // 0x044  -0x1000..0x1000 (copy of the step's scratch record)
    int16_t slipRatioGrip;           // 0x046  lateral grip factor from the slip-ratio curve
    int32_t springForce;             // 0x048
    int32_t damperForce;             // 0x04C
    int16_t slipAngle;               // 0x050  angle of the contact patch velocity, 0 = straight ahead
    int16_t slipBlend;               // 0x052  0..0x1000 weight of the slip angle by speed
    int32_t lateralForce;            // 0x054
    int32_t driveForce;              // 0x058  longitudinal force while the wheel drives the road
    int32_t brakeForce;              // 0x05C  ... while the road drives the wheel
    int16_t brakeInput;              // 0x060  0..0x1000 brake of this wheel after the assists (0x8003DBE8)
    uint8_t reserved062;
    uint8_t contactFlags;            // 0x063  bit 0: in the air, bit 1: braking, bit 2 / 3: driven by the secondary / primary axle
    int32_t wear;                    // 0x064  accumulated wear (negative = cold)
};

// Per-axle suspension block (body + 0x12C + axle * 0x34). Heights are wheel travel in 1/4096 m, negative =
// compressed (established from 0x800438F0; the names describe how the values are used).
struct AxleSuspension {
    int16_t bumpStopStart;      // +0x00  travel below which the bump stop adds (start - travel)^2 * bumpStopRate
    int16_t travelFloor;        // +0x02  travel is clamped up to this; also the "wheel firmly on the ground" test
    int16_t droopLimit;         // +0x04  travel above this = wheel in the air (no load)
    int16_t reserved06;
    int16_t rideReference;      // +0x08  travel origin: reference minus the wheel radius (wheelRadius[axle])
    int16_t staticDeflection;   // +0x0A  -(axle weight / 2) / spring rate, written by the setup (0x80030F94)
    int32_t springRate;         // +0x0C  force = -travel * rate
    int32_t bumpKnee;           // +0x10  bump damping: base + gain * sqrt(-knee - velocity)
    int32_t bumpBase;           // +0x14
    int32_t bumpGain;           // +0x18
    int32_t reboundKnee;        // +0x1C  rebound damping: -(base + gain * sqrt(velocity - knee))
    int32_t reboundBase;        // +0x20
    int32_t reboundGain;        // +0x24
    int32_t antiRollRate;       // +0x28  anti-roll bar: sign(d) * d^2 * rate for the travel difference d
    int32_t bumpStopRate;       // +0x2C
    int16_t unsprungMass;       // +0x30  kg per wheel (setup)
    int16_t staticLoad;         // +0x32  added to the spring + damper force to give the wheel load
};

// Per-axle tyre parameter block (body + 0x194 + axle * 0xD8): five curve objects { u16 count; xs token; ys token;
// samples } (car_setup.h AxleTyreBlock has the full layout; the tyre code receives them resolved as
// tyres.h AxleTyreCurves). Only the two peak slip ratios are read directly by the ported code.
struct AxleTyreParams {
    uint8_t curves[0x68];       // +0x00  slip angle curve, slip ratio curve
    // Slip ratio = (road - rim) / reference (tyres.cpp): negative while the wheel spins, positive while it locks.
    int16_t peakSlipRatioNeg;   // +0x68  slip ratio with the largest force on the negative side: the traction threshold
                                //        (upshift check 0x800449C8, traction control 0x8003DE68)
    int16_t peakSlipRatioPos;   // +0x6A  ... positive side: the locking threshold (downshift check, anti-lock 0x8003DBE8)
    uint8_t curves6C[0xD8 - 0x6C]; // +0x6C  slip-ratio grip curve, load grip curve, camber grip curve
};

// One car of the original's array (stride 0xB40): 0x2C bytes of the shell's bookkeeping, then the physics body.
struct Car;

struct CarBody {
    // ---- geometry and mass (written by the setup, 0x800319A8 / 0x80030D10) ----
    int32_t frontExtent;             // 0x000  distance from the reference point to the front edge
    int32_t rearExtent;              // 0x004  ... to the rear edge
    int16_t width;                   // 0x008  full width (halved where used)
    int16_t height;                  // 0x00A  vertical extent (pair awareness)
    int16_t axleOffset[2];           // 0x00C  distance from the CG to the front / rear axle (both positive; their sum = wheelbase)
    int16_t wheelbase;               // 0x010  also the lever arm of the pitch (ground.cpp)
    int16_t meanTrack;               // 0x012  weight-averaged full track; lever arm of the roll
    int16_t rearWeightFraction;      // 0x014  0x1000 = all weight on the rear; also the front/rear blend of the ground pass
    int16_t frontWeightFraction;     // 0x016
    int16_t halfTrack[2];            // 0x018  front, rear
    uint8_t byte1C;                  // 0x01C  a1 of 0x80033384's 11th argument (0 in the attract race)
    uint8_t driveClass;              // 0x01D  0 rear, 1 front, 2 four-wheel, 3 4WD type 5; indexes the class / AI aid tables
    int16_t rollLever;               // 0x01E  (CG height term / mean track) / 2 x sprung mass = lateral load transfer per lateral acceleration
    int16_t pitchLever;              // 0x020  (CG height term / wheelbase) / 2 x sprung / total mass = longitudinal load transfer per net force
    int16_t cgHeight;                // 0x022  weighted ride height; added to the ground height at placement
    int16_t rideHeight[2];           // 0x024  front, rear
    int16_t mass;                    // 0x028  4 units per kg
    int16_t inverseMass;             // 0x02A  0x1000000 / mass
    int16_t inverseSprungMass;       // 0x02C  0x1000000 / (mass - 2 x unsprung per axle): vertical velocity change per force
    int16_t cgOffset;                // 0x02E  (rearAxle - frontAxle) / 2, moves the extents
    int32_t gripAtZeroSlip;          // 0x030  mean lateral grip factor at zero slip ratio x grip weight
    int16_t inverseGripAtZeroSlip;   // 0x034  lateral acceleration -> yaw acceleration gain (physics_core)
    int16_t inverseGripWeight;       // 0x036  0x1000 / (class-weighted tyre grip x 2.45)
    // ---- steering (setup) ----
    int16_t steerLock;               // 0x038  angle units, from CarParams::steerLockDeg
    int16_t steerRate;               // 0x03A  angle units from CarParams::steerRateDeg (setup); read only as the counter-steer cap
                                     //        of the player input (0x8002FB18)
    uint16_t steerLimitCount;        // 0x03C  table object { count; xs token -> 0x48; ys token -> 0x54 }: speed -> steering limit
                                     //        (the player input 0x8002FB18); not the yaw inertia, which is 0x68
    uint16_t reserved03E;
    uint32_t steerLimitXsToken;      // 0x040  guest pointer to steerLimitXs (a 32-bit token here)
    uint32_t steerLimitYsToken;      // 0x044  guest pointer to steerLimitYs
    int16_t steerLimitXs[6];         // 0x048  speed = v << 7
    int16_t steerLimitYs[6];         // 0x054  fraction = v * 4096 / 1800
    int16_t peakSlipAngle;           // 0x060  angle with the largest cos * force (0x8003B598), at most 0x155; the AI's steering window
    int16_t steerMaxRate;            // 0x062  steering angle change per second
    int16_t ackermann[2];            // 0x064  [0] inner, [1] outer: atan2(wheelbase, wheelbase -/+ half track) << 3
    int32_t inverseYawInertia;       // 0x068  (0x3C was once assumed; 0x80039FC8 pass H reads 0x68)
    int32_t inversePitchInertia[2];  // 0x06C  front, rear axle terms: pitch rate change per force
    int32_t inverseRollInertia[2];   // 0x074  front, rear: roll rate change per left-right difference
    // ---- engine block 0x07C..0x12C (car_setup.h EngineBlock) ----
    uint16_t engineTorqueCount;      // 0x07C  torque curve object { count; xs token -> 0x88; ys token -> 0xC8 }
    uint16_t reserved07E;
    uint32_t engineTorqueXsToken;    // 0x080
    uint32_t engineTorqueYsToken;    // 0x084
    int32_t engineTorqueXs[16];      // 0x088  engine speed rev/s << 12
    int32_t engineTorqueYs[16];      // 0x0C8  torque
    uint16_t revLimitRpm;            // 0x108  rev limiter, hysteresis of 500 rpm
    uint16_t idleRpm;                // 0x10A
    int32_t engineBrakeCoefficient;  // 0x10C  x engine speed = engine braking torque
    int16_t turboSpoolRpm[2];        // 0x110  engine speed where each turbo reaches its boost
    int16_t turboBoost[2];           // 0x114  boost (of 0x1000) of each turbo, 0 = none
    int32_t turboSpoolRate[2];       // 0x118  spool state change per step
    int32_t turboCoefficient[2];     // 0x120  shape of the boost curve
    int16_t boostCap;                // 0x128  maximum boost (of 0x1000), 0 = no turbo
    uint8_t turboModel[2];           // 0x12A  curve type below the spool speed (0, 1, 2)
    // ---- suspension and tyre blocks ----
    AxleSuspension suspension[2];    // 0x12C  front, rear (0x34 bytes each)
    AxleTyreParams tyres[2];         // 0x194  front, rear (0xD8 bytes each)
    int32_t topSpeed;                // 0x344  estimated by the setup (0x80030950)
    int16_t surfaceGrip[8];          // 0x348  grip per surface type, 0x100 = 1.0
    int16_t inverseSurfaceGrip[8];   // 0x358  0x6400 / percent
    int16_t camber[2];               // 0x368  front, rear (negative angle / 10)
    int16_t toe[2];                  // 0x36C  front, rear: code - 128
    // ---- drivetrain block 0x370..0x420 (car_setup.h DrivetrainBlock) ----
    uint8_t driveType;               // 0x370  0 rear, 1 front, 2/3 4WD centre diff (front/rear primary), 4/5/6 4WD coupling
    uint8_t reserved371;
    uint8_t forwardGears;            // 0x372  number of forward gears (top gear index), written by the setup (0x800347C4).
                                     //        Alias "engineControlMode": the drive shafts (0x80045AE8) dispatch on it, 1 gear =
                                     //        governed engine (0x8004530C), 2 gears = clutch start (0x80045138), else the plain engine
    uint8_t primaryAxle;             // 0x373  0 front, 1 rear: axle the other one copies when the coupling reverses; with 1 the
                                     //        fully pulled handbrake opens the clutch (0x8002F92C)
    int16_t centreSplit;             // 0x374  share (of 0x1000) of the centre coupling / blend of the axle speeds
    int16_t centreLockTorque;        // 0x376  torque window that keeps the centre coupling locked
    int16_t engineInertiaRef;        // 0x378  engine inertia referred to the gearbox (x the gear ratio)
    uint8_t axleDiffType[2];         // 0x37A  per axle: 0 open, 1 locked, 3/4 torque sensing, 5 speed sensing, 6/7 active
    int32_t axleDiffMinTorque[2];    // 0x37C
    int32_t axleDiffAccelRatio[2];   // 0x384  locking torque per drive torque (accelerating)
    int32_t axleDiffDecelRatio[2];   // 0x38C  ... (decelerating)
    uint16_t downshiftFloorRpm;      // 0x394  auto gearbox: lowest rpm a downshift may target (+500); clutch engage rpm of single-gear cars
    uint16_t upshiftRpm;             // 0x396  auto gearbox: upshift rpm; the governed engine's power rpm
    uint16_t downshiftRpm[6];        // 0x398  auto gearbox: downshift rpm of gear 2.. (index gear - 2); [0] is the launch rpm of the clutch start
    int32_t gearRatio[8];            // 0x3A4  overall ratio (x4096) per gear index, [0] = reverse
    int32_t revsPerSpeed[8];         // 0x3C4  engine revolutions per unit of road speed per gear (12.12)
    int16_t wheelRadius[2];          // 0x3E4  per axle
    int16_t inverseWheelRadius[2];   // 0x3E8  0x1000000 / wheelRadius
    int16_t rimRadius[2];            // 0x3EC
    int16_t tyreHeight[2];           // 0x3F0
    int16_t lockedClutchTorque;      // 0x3F4  torque through the clutch in clutch state 3
    int16_t overrunClutchTorque;     // 0x3F6  torque through the slipping clutch when the engine does not drive
    int32_t axleInvInertia[2];       // 0x3F8  wheel acceleration per torque (x 2^-8) per axle
    int32_t axleInertia[2];          // 0x400  inertia of the axle (with wheels) per axle
    int32_t centreDiffStiffness;     // 0x408  centre differential torque per speed difference
    int32_t engineInvInertia;        // 0x40C  engine acceleration per torque
    int16_t wheelInertiaFactor[2];   // 0x410  wheelInertia x inverseWheelRadius^2: mass of the rotating parts while the wheels roll
    int32_t brakeTorque[3];          // 0x414  front, rear per unit of brake input, [2] handbrake (rear wheels)
    // ---- setup constants after the drivetrain block ----
    int32_t brakeBalanceSpeed;       // 0x420  mass / (4 x sum of brake torque x inverse radius): lower bound of the AI's braking factor
    int32_t absSlipGain[2];          // 0x424  per axle
    int16_t brakeLoadFactor[2];      // 0x42C  wheelRadius / brake torque
    int32_t weight;                  // 0x430  mass x 2.45
    int32_t sprungWeightPerAxle[2];  // 0x434  sprung weight x front / rear fraction; the load offset of the wear rate
    int32_t dragCoefficient;         // 0x43C  air drag (force = coefficient * speed^2 term)
    int32_t downforce[2];            // 0x440  front, rear aerodynamic downforce coefficients
    int16_t gripLoadGain[2];         // 0x448  front, rear: from the compound grip %; wear per unit of lateral slip factor
    int16_t gripLoadGain2[2];        // 0x44C  wear per unit of slip ratio
    int32_t tcsGain;                 // 0x450  player's traction control gain (12.12)
    int16_t tcsFalloffGain;          // 0x454  steer falloff gain of the yaw acceleration
    int16_t tcsSteerGain;            // 0x456  ... threshold
    int16_t asmYawGain;              // 0x458  brake assist yaw gain
    int16_t asmYawThreshold;         // 0x45A
    uint8_t carIndex;                // 0x45C  index of the car in the array (selects the scratchpad work block)
    uint8_t controlClass;            // 0x45D  0 player, 2 AI (0x80030D64); non-zero disables the player-state query (0x800419E8)
    uint8_t contactType;             // 0x45E  non-zero: the car takes no part in car-to-car contact (0x800400CC); 2 = effects cleared
    uint8_t reserved45F;
    Wheel wheels[4];                 // 0x460  front-left, front-right, rear-left, rear-right
    // ---- race progress and driver state ----
    int32_t chunkIndex;              // 0x600  course chunk the car is on
    int32_t courseDistance;          // 0x604  16.16 m along the course
    int16_t lap;                     // 0x608
    int16_t clutchRequest;           // 0x60A  0 or 0x1000
    int16_t steerAngle;              // 0x60C  steering angle of the driver
    int16_t steerAngleRate;          // 0x60E  its rate (digital steering integrator)
    int16_t throttle;                // 0x610  throttle pedal 0..0x1000
    int16_t brake;                   // 0x612  brake pedal
    int16_t handbrake;               // 0x614
    int16_t reserved616;
    uint8_t gear;                    // 0x618  gear index into gearRatio, 0 = reverse
    uint8_t clutchState;             // 0x619  0 open, 1 engaged, 2 engaging from rest / slipping, 3 engaging while rolling / locked torque
    uint8_t axleDiffLocked[2];       // 0x61A  torque-sensing axle differential is locked
    uint8_t shiftTimer;              // 0x61C  frames
    uint8_t revLimiterActive;        // 0x61D  the limiter is cutting
    int16_t engineLoad;              // 0x61E  throttle / torque demand of the last step (turbo model), 0..0x1000+
    uint16_t turboSpool[2];          // 0x620  spool state (boost) of each turbo
    int32_t engineSpeed;             // 0x624  rev/s << 12
    int32_t velocity[3];             // 0x628  1/4096 m/s in the simulation plane (x, y, height)
    int32_t axleSpeed[2];            // 0x634  angular speed of the front / rear axle, rad/s << 12
    uint8_t centreLocked;            // 0x63C  centre coupling of drive types 4 / 5 is locked
    uint8_t contactFlags;            // 0x63D  bit 0: hit a wall in this step (cleared by the step driver), bit 1: car-car contact
    int8_t scrapeDirection;          // 0x63E  signed steering-like value while sliding along a wall (-64..64)
    uint8_t hitCorner;               // 0x63F  footprint corner that hit the wall last
    int16_t wallScrape;              // 0x640  strength of the current wall contact (>= 0x1000 while pushing), decays
    uint8_t transmissionMode;        // 0x642  0 automatic, 1 manual, other = no shifting
    uint8_t reserved643;
    int16_t pitch;                   // 0x644  body attitude
    int16_t roll;                    // 0x646
    int16_t heading;                 // 0x648  kept in -0xFFF..0xFFF
    int16_t yawAccel;                // 0x64A  yaw acceleration of the step (physics_core); relaxes the type-5 coupling above 1024
    int32_t yawRate;                 // 0x64C  heading change per second, angle units << 7
    int16_t pitchRate;               // 0x650  << 4
    int16_t rollRate;                // 0x652  << 4
    int16_t hitNormal0, hitNormal1;  // 0x654  wall normal of the last hit
    uint32_t dirtiness;              // 0x658  capped at 480000
    int32_t position[3];             // 0x65C  simulation plane x, y and height
    int16_t basis[3][4];             // 0x668  attitude rows (forward, lateral, up) x (x, y, height, padding), 0x1000 = 1.0
    int32_t visualPosition[3];       // 0x680  ground-following pose: x, y, mean surface height
    int16_t visualBasis[3][4];       // 0x68C  rows of the visual pose
    int32_t forwardSpeed;            // 0x6A4  velocity in the car's frame
    int32_t lateralSpeed;            // 0x6A8
    int16_t engineRpm;               // 0x6AC
    uint16_t speedReadout;           // 0x6AE  |forward speed| scaled for the display
    uint8_t sector;                  // 0x6B0  start-line / split sector of the shell
    uint8_t airborne;                // 0x6B1  set while no wheel carries load (no force integration)
    int8_t gridSlot;                 // 0x6B2  start-grid slot (indexes RaceGridInfo)
    uint8_t footprintSet;            // 0x6B3  which of the two corner sets is current (flips every step)
    int32_t footprint[2][4][2];      // 0x6B4  [set][corner][x, y]: front-left, front-right, rear-left, rear-right
    int16_t visualPitch;             // 0x6F4
    int16_t visualRoll;              // 0x6F6  subtracted from the roll for the camber (tyres)
    uint16_t maxSpeedReadout;        // 0x6F8
    uint8_t resetState;              // 0x6FA  2 = reset requested (stuck car); cleared by the shell at the lap line
    uint8_t resetCounter;            // 0x6FB  frames with all wheels off the road
    uint8_t finishFlag;              // 0x6FC  1 finished, 2 after the race order pass
    uint8_t inputFlag6FD;            // 0x6FD  cleared by the pad mapping unless the pad's word 3 is held at 0x1000
    int16_t stepTime;                // 0x6FE  simulation step in 16.16 s (2184 = 1/30 s), scaled by timeScale
    int16_t steerBlend;              // 0x700  blend of the steering angle against the counter-steer limit
    int16_t reserved702;
    int32_t stepRate12;              // 0x704  steps per second << 12, scaled by timeScale
    int16_t effectiveThrottle;       // 0x708  throttle after the traction control (the tyre code's throttle)
    int16_t steerLimitNeg;           // 0x70A  steering limits of this step
    int16_t steerLimitPos;           // 0x70C
    int16_t reserved70E;
    int32_t engineTorque;            // 0x710  torque at the current engine speed (curve) above the friction torque
    int32_t engineBrakeTorque;       // 0x714  engine braking / friction torque
    uint8_t scriptedControl;         // 0x718  scripted control (held before the start); also disables the wheel effects
    uint8_t contactMask;             // 0x719  bits 0-3: wheel below the travel floor; bits 4-7: the same before the lift
    int16_t dragForce;               // 0x71A  signed against forwardSpeed
    int32_t downforceForce[2];       // 0x71C  front, rear
    int32_t forwardAccel;            // 0x724
    int32_t lateralAccel;            // 0x728
    int32_t externalLongForce;       // 0x72C  cleared every step; subtracted from the net force for the load transfer
    int32_t externalLatForce;        // 0x730  cleared every step
    int32_t netLongitudinalForce;    // 0x734
    uint16_t impactHold;             // 0x738  held wall impact strength for the sound level
    int16_t viewYawOffset;           // 0x73A  heading - view yaw
    int16_t viewPitchOffset;         // 0x73C  pitch - view pitch
    int16_t impactTimer;             // 0x73E  frames since the wall impact
    int16_t wallImpact;              // 0x740  strength of the wall impact of this step (sound / damage), 0 if none
    int16_t intakeLoad;              // 0x742  eased throttle-dependent value (turbo sound / effects)
    int16_t blowOffState;            // 0x744  0 idle, 1..0x1000 armed level, -1 released
    int16_t turboSpoolMax;           // 0x746  larger spool state of the two turbos (gauge / sound)
    int16_t viewYaw;                 // 0x748
    int16_t viewYawRate;             // 0x74A
    int16_t viewPitch;               // 0x74C
    int16_t viewPitchRate;           // 0x74E
    uint8_t racePosition;            // 0x750  1-based
    uint8_t licenseState;            // 0x751  licence test: 0 running, 1 passed, 2 failed (0x8003D244 / 0x8003D2A0); cleared by 0x8003D22C
    int16_t viewYawAccel;            // 0x752
    uint8_t reserved754[2];
    uint8_t licenseCode;             // 0x756  licence test outcome: 1 pass, 3 overshot, 4 off course, 5 wall (0x8003D5F8)
    uint8_t engineVisual0;           // 0x757  effects: engine load deviation visual (exhaust); governed mode: speed term;
                                     //        the car sound's engine volume (0x800146D8)
    uint8_t engineVisual1;           // 0x758  effects: engine load visual; governed mode: load term; the sound's exhaust volume
    uint8_t exhaustFlame;            // 0x759  backfire intensity 0..255; the sound's turbo volume
    uint8_t reserved75A[2];
    int32_t licenseTime;             // 0x75C  licence test time (1/1000 s) of the outcome; 359999999 (99:59:59.999) at the start
    int16_t word760;                 // 0x760  cleared at the start (meaning not established)
    uint8_t loadSoundLevel;          // 0x762
    uint8_t impactSoundFlag;         // 0x763
    uint8_t messageCode;             // 0x764  shell message request
    uint8_t messageFrames;           // 0x765  counts down unless 0 / 0xFF
    int16_t timeScale;               // 0x766  0x1000 = real time
    int8_t steerInput;               // 0x768  -100..100 from the pad
    uint8_t aiScriptThrottleOn;      // 0x769  throttle toggle while waiting for the start (0x800372EC)
    int16_t neighbourClass;          // 0x76A  0x1000 / 0x800 / 0x400: how close the car ahead is
    int8_t neighbourFlags[8];        // 0x76C  sectors around the car occupied by neighbours: [0] ahead, [1] ahead-left, [2] ahead-right, [3] left, [4] right
    int16_t draftInput;              // 0x774  slipstream strength requested this step (0 = none)
    int16_t draftBlend;              // 0x776  0..0x1000, eases towards the request
    int32_t pushForce[2];            // 0x778  car-to-car push of the last step
    int32_t lastLineTime;            // 0x780  race time at the last start / split line (shell)
    uint8_t aiSlowZone;              // 0x784  1 on the grid / formation line
    uint8_t wallHitMask;             // 0x785  corner mask of the last move step's wall hits
    uint8_t raceState;               // 0x786  0 racing, 3 / 4 fixed AI lookaheads, 5 / 6 stop the car, 7 finished
    uint8_t reserved787;
    uint8_t aiRecovery;              // 0x788  0 normal, 3 reversing away from a wall, 4 turning back to the line
    uint8_t aiLine;                  // 0x789  racing line followed: 6 main, 1 / 2 overtaking, 3 pit / formation, 4 grid slots (no contact)
    uint8_t aiSection;               // 0x78A  section index in the line
    uint8_t aiPreviousType;          // 0x78B  type of the previous section, or 3 / 4 while braking for the current one
    uint8_t aiSectionType;           // 0x78C  0 straight, 1 / 2 arc
    uint8_t flags78D;                // 0x78D  bit 1: lap not yet counted, bits 2 / 3: pit request, bit 4: off the road (tyre wear suspended)
    uint16_t penaltyFrames;          // 0x78E  penalty / reset animation frames left (> 1 = animating)
    int32_t aiPreviousTargetSpeed;   // 0x790
    int32_t aiTargetSpeed;           // 0x794
    uint8_t reserved798[0xA60 - 0x798];
    // ---- the shell's HUD request fields of the car record (car + 0xA8C..) ----
    int8_t hudInvalid;               // 0xA60  (car + 0xA8C) the displayed lap / split is flagged
    uint8_t hudMessageKind;          // 0xA61  (car + 0xA8D) 2 = reset request (0x800156B8)
    int16_t hudTimer;                // 0xA62  (car + 0xA8E) frames left of the lap / split display
    int16_t hudTimer2;               // 0xA64  (car + 0xA90) frames left of the gap / record display
    int16_t hudTimer3;               // 0xA66  (car + 0xA92) counted down like the others; 120 with hudMessageKind 2 for the reset
    int32_t hudValue;                // 0xA68  (car + 0xA94) displayed lap / split time
    uint32_t hudLabel;               // 0xA6C  (car + 0xA98) caption token
    int32_t hudCompare;              // 0xA70  (car + 0xA9C) record / gap to compare with
    int32_t hudGap;                  // 0xA74  (car + 0xAA0)
    uint8_t reservedA78[0xB14 - 0xA78];
};

struct Car {
    uint8_t reserved000[0x18];
    int16_t padSlot;                 // 0x018  control source: 1 live pad, 2 replay, 3 = 0x80014030; 2 / 3 keep the results
    uint8_t reserved01A[0x24 - 0x1A];
    int32_t finishTime;              // 0x024  total time at the finish
    uint8_t reserved028[4];
    CarBody body;                    // 0x02C
};
#pragma pack(pop)

// Rows of CarBody::basis / visualBasis.
constexpr size_t kRowForward = 0, kRowLateral = 1, kRowUp = 2;

// surfaceGrip[index] read like the original: the index is not range-checked (surface types 8..15 read
// inverseSurfaceGrip, the AI's signed section surface may read the bytes before the table).
inline int16_t SurfaceGripAt(const CarBody& body, int32_t index) {
    int16_t grip;
    std::memcpy(&grip, reinterpret_cast<const uint8_t*>(&body) + offsetof(CarBody, surfaceGrip) + index * 2, sizeof grip);
    return grip;
}

static_assert(sizeof(Wheel) == 0x68);
static_assert(offsetof(Wheel, load) == 0x08);
static_assert(offsetof(Wheel, steerAngle) == 0x0C);
static_assert(offsetof(Wheel, travel) == 0x10);
static_assert(offsetof(Wheel, surface) == 0x14);
static_assert(offsetof(Wheel, accel) == 0x16);
static_assert(offsetof(Wheel, rimSpeed) == 0x18);
static_assert(offsetof(Wheel, skidLevel) == 0x1C);
static_assert(offsetof(Wheel, rotation) == 0x20);
static_assert(offsetof(Wheel, damage) == 0x22);
static_assert(offsetof(Wheel, offsetX) == 0x24);
static_assert(offsetof(Wheel, slipScale) == 0x2A);
static_assert(offsetof(Wheel, contactForwardSpeed) == 0x2C);
static_assert(offsetof(Wheel, gripForce) == 0x34);
static_assert(offsetof(Wheel, wearGrip) == 0x38);
static_assert(offsetof(Wheel, lateralFactorAbs) == 0x3C);
static_assert(offsetof(Wheel, wearStage) == 0x3F);
static_assert(offsetof(Wheel, speedFactor) == 0x40);
static_assert(offsetof(Wheel, slipRatio) == 0x44);
static_assert(offsetof(Wheel, springForce) == 0x48);
static_assert(offsetof(Wheel, slipAngle) == 0x50);
static_assert(offsetof(Wheel, lateralForce) == 0x54);
static_assert(offsetof(Wheel, brakeForce) == 0x5C);
static_assert(offsetof(Wheel, brakeInput) == 0x60);
static_assert(offsetof(Wheel, contactFlags) == 0x63);
static_assert(offsetof(Wheel, wear) == 0x64);

static_assert(sizeof(AxleSuspension) == 0x34);
static_assert(offsetof(AxleSuspension, staticDeflection) == 0x0A);
static_assert(offsetof(AxleSuspension, springRate) == 0x0C);
static_assert(offsetof(AxleSuspension, staticLoad) == 0x32);
static_assert(sizeof(AxleTyreParams) == 0xD8);
static_assert(offsetof(AxleTyreParams, peakSlipRatioNeg) == 0x68);

static_assert(offsetof(CarBody, height) == 0x00A);
static_assert(offsetof(CarBody, axleOffset) == 0x00C);
static_assert(offsetof(CarBody, wheelbase) == 0x010);
static_assert(offsetof(CarBody, halfTrack) == 0x018);
static_assert(offsetof(CarBody, driveClass) == 0x01D);
static_assert(offsetof(CarBody, rollLever) == 0x01E);
static_assert(offsetof(CarBody, cgHeight) == 0x022);
static_assert(offsetof(CarBody, mass) == 0x028);
static_assert(offsetof(CarBody, inverseSprungMass) == 0x02C);
static_assert(offsetof(CarBody, gripAtZeroSlip) == 0x030);
static_assert(offsetof(CarBody, inverseGripWeight) == 0x036);
static_assert(offsetof(CarBody, steerLock) == 0x038);
static_assert(offsetof(CarBody, steerLimitCount) == 0x03C);
static_assert(offsetof(CarBody, steerLimitXs) == 0x048);
static_assert(offsetof(CarBody, steerLimitYs) == 0x054);
static_assert(offsetof(CarBody, peakSlipAngle) == 0x060);
static_assert(offsetof(CarBody, ackermann) == 0x064);
static_assert(offsetof(CarBody, inverseYawInertia) == 0x068);
static_assert(offsetof(CarBody, inverseRollInertia) == 0x074);
static_assert(offsetof(CarBody, engineTorqueCount) == 0x07C);
static_assert(offsetof(CarBody, engineTorqueXs) == 0x088);
static_assert(offsetof(CarBody, engineTorqueYs) == 0x0C8);
static_assert(offsetof(CarBody, revLimitRpm) == 0x108);
static_assert(offsetof(CarBody, engineBrakeCoefficient) == 0x10C);
static_assert(offsetof(CarBody, turboSpoolRpm) == 0x110);
static_assert(offsetof(CarBody, turboSpoolRate) == 0x118);
static_assert(offsetof(CarBody, boostCap) == 0x128);
static_assert(offsetof(CarBody, suspension) == 0x12C);
static_assert(offsetof(CarBody, tyres) == 0x194);
static_assert(offsetof(CarBody, topSpeed) == 0x344);
static_assert(offsetof(CarBody, surfaceGrip) == 0x348);
static_assert(offsetof(CarBody, camber) == 0x368);
static_assert(offsetof(CarBody, driveType) == 0x370);
static_assert(offsetof(CarBody, forwardGears) == 0x372);
static_assert(offsetof(CarBody, centreSplit) == 0x374);
static_assert(offsetof(CarBody, axleDiffType) == 0x37A);
static_assert(offsetof(CarBody, axleDiffDecelRatio) == 0x38C);
static_assert(offsetof(CarBody, downshiftFloorRpm) == 0x394);
static_assert(offsetof(CarBody, downshiftRpm) == 0x398);
static_assert(offsetof(CarBody, gearRatio) == 0x3A4);
static_assert(offsetof(CarBody, revsPerSpeed) == 0x3C4);
static_assert(offsetof(CarBody, wheelRadius) == 0x3E4);
static_assert(offsetof(CarBody, lockedClutchTorque) == 0x3F4);
static_assert(offsetof(CarBody, axleInvInertia) == 0x3F8);
static_assert(offsetof(CarBody, engineInvInertia) == 0x40C);
static_assert(offsetof(CarBody, brakeTorque) == 0x414);
static_assert(offsetof(CarBody, brakeBalanceSpeed) == 0x420);
static_assert(offsetof(CarBody, brakeLoadFactor) == 0x42C);
static_assert(offsetof(CarBody, sprungWeightPerAxle) == 0x434);
static_assert(offsetof(CarBody, dragCoefficient) == 0x43C);
static_assert(offsetof(CarBody, gripLoadGain) == 0x448);
static_assert(offsetof(CarBody, tcsGain) == 0x450);
static_assert(offsetof(CarBody, asmYawThreshold) == 0x45A);
static_assert(offsetof(CarBody, carIndex) == 0x45C);
static_assert(offsetof(CarBody, controlClass) == 0x45D);
static_assert(offsetof(CarBody, contactType) == 0x45E);
static_assert(offsetof(CarBody, wheels) == 0x460);
static_assert(offsetof(CarBody, chunkIndex) == 0x600);
static_assert(offsetof(CarBody, courseDistance) == 0x604);
static_assert(offsetof(CarBody, clutchRequest) == 0x60A);
static_assert(offsetof(CarBody, throttle) == 0x610);
static_assert(offsetof(CarBody, gear) == 0x618);
static_assert(offsetof(CarBody, shiftTimer) == 0x61C);
static_assert(offsetof(CarBody, engineLoad) == 0x61E);
static_assert(offsetof(CarBody, turboSpool) == 0x620);
static_assert(offsetof(CarBody, engineSpeed) == 0x624);
static_assert(offsetof(CarBody, velocity) == 0x628);
static_assert(offsetof(CarBody, axleSpeed) == 0x634);
static_assert(offsetof(CarBody, centreLocked) == 0x63C);
static_assert(offsetof(CarBody, contactFlags) == 0x63D);
static_assert(offsetof(CarBody, scrapeDirection) == 0x63E);
static_assert(offsetof(CarBody, wallScrape) == 0x640);
static_assert(offsetof(CarBody, transmissionMode) == 0x642);
static_assert(offsetof(CarBody, pitch) == 0x644);
static_assert(offsetof(CarBody, heading) == 0x648);
static_assert(offsetof(CarBody, yawAccel) == 0x64A);
static_assert(offsetof(CarBody, yawRate) == 0x64C);
static_assert(offsetof(CarBody, pitchRate) == 0x650);
static_assert(offsetof(CarBody, hitNormal0) == 0x654);
static_assert(offsetof(CarBody, dirtiness) == 0x658);
static_assert(offsetof(CarBody, position) == 0x65C);
static_assert(offsetof(CarBody, basis) == 0x668);
static_assert(offsetof(CarBody, visualPosition) == 0x680);
static_assert(offsetof(CarBody, visualBasis) == 0x68C);
static_assert(offsetof(CarBody, forwardSpeed) == 0x6A4);
static_assert(offsetof(CarBody, engineRpm) == 0x6AC);
static_assert(offsetof(CarBody, speedReadout) == 0x6AE);
static_assert(offsetof(CarBody, sector) == 0x6B0);
static_assert(offsetof(CarBody, airborne) == 0x6B1);
static_assert(offsetof(CarBody, gridSlot) == 0x6B2);
static_assert(offsetof(CarBody, footprintSet) == 0x6B3);
static_assert(offsetof(CarBody, footprint) == 0x6B4);
static_assert(offsetof(CarBody, visualPitch) == 0x6F4);
static_assert(offsetof(CarBody, maxSpeedReadout) == 0x6F8);
static_assert(offsetof(CarBody, resetState) == 0x6FA);
static_assert(offsetof(CarBody, finishFlag) == 0x6FC);
static_assert(offsetof(CarBody, inputFlag6FD) == 0x6FD);
static_assert(offsetof(CarBody, stepTime) == 0x6FE);
static_assert(offsetof(CarBody, steerBlend) == 0x700);
static_assert(offsetof(CarBody, stepRate12) == 0x704);
static_assert(offsetof(CarBody, effectiveThrottle) == 0x708);
static_assert(offsetof(CarBody, steerLimitNeg) == 0x70A);
static_assert(offsetof(CarBody, engineTorque) == 0x710);
static_assert(offsetof(CarBody, engineBrakeTorque) == 0x714);
static_assert(offsetof(CarBody, scriptedControl) == 0x718);
static_assert(offsetof(CarBody, contactMask) == 0x719);
static_assert(offsetof(CarBody, dragForce) == 0x71A);
static_assert(offsetof(CarBody, downforceForce) == 0x71C);
static_assert(offsetof(CarBody, forwardAccel) == 0x724);
static_assert(offsetof(CarBody, externalLongForce) == 0x72C);
static_assert(offsetof(CarBody, netLongitudinalForce) == 0x734);
static_assert(offsetof(CarBody, impactHold) == 0x738);
static_assert(offsetof(CarBody, impactTimer) == 0x73E);
static_assert(offsetof(CarBody, wallImpact) == 0x740);
static_assert(offsetof(CarBody, intakeLoad) == 0x742);
static_assert(offsetof(CarBody, turboSpoolMax) == 0x746);
static_assert(offsetof(CarBody, viewYaw) == 0x748);
static_assert(offsetof(CarBody, racePosition) == 0x750);
static_assert(offsetof(CarBody, viewYawAccel) == 0x752);
static_assert(offsetof(CarBody, licenseState) == 0x751);
static_assert(offsetof(CarBody, licenseCode) == 0x756);
static_assert(offsetof(CarBody, engineVisual0) == 0x757);
static_assert(offsetof(CarBody, engineVisual1) == 0x758);
static_assert(offsetof(CarBody, exhaustFlame) == 0x759);
static_assert(offsetof(CarBody, licenseTime) == 0x75C);
static_assert(offsetof(CarBody, word760) == 0x760);
static_assert(offsetof(CarBody, loadSoundLevel) == 0x762);
static_assert(offsetof(CarBody, messageCode) == 0x764);
static_assert(offsetof(CarBody, timeScale) == 0x766);
static_assert(offsetof(CarBody, steerInput) == 0x768);
static_assert(offsetof(CarBody, aiScriptThrottleOn) == 0x769);
static_assert(offsetof(CarBody, neighbourClass) == 0x76A);
static_assert(offsetof(CarBody, neighbourFlags) == 0x76C);
static_assert(offsetof(CarBody, draftInput) == 0x774);
static_assert(offsetof(CarBody, pushForce) == 0x778);
static_assert(offsetof(CarBody, lastLineTime) == 0x780);
static_assert(offsetof(CarBody, aiSlowZone) == 0x784);
static_assert(offsetof(CarBody, wallHitMask) == 0x785);
static_assert(offsetof(CarBody, raceState) == 0x786);
static_assert(offsetof(CarBody, aiRecovery) == 0x788);
static_assert(offsetof(CarBody, aiLine) == 0x789);
static_assert(offsetof(CarBody, aiSectionType) == 0x78C);
static_assert(offsetof(CarBody, flags78D) == 0x78D);
static_assert(offsetof(CarBody, penaltyFrames) == 0x78E);
static_assert(offsetof(CarBody, aiTargetSpeed) == 0x794);
static_assert(offsetof(CarBody, hudInvalid) == 0xA8C - 0x2C);
static_assert(offsetof(CarBody, hudMessageKind) == 0xA8D - 0x2C);
static_assert(offsetof(CarBody, hudTimer) == 0xA8E - 0x2C);
static_assert(offsetof(CarBody, hudTimer3) == 0xA92 - 0x2C);
static_assert(offsetof(CarBody, hudValue) == 0xA94 - 0x2C);
static_assert(offsetof(CarBody, hudGap) == 0xAA0 - 0x2C);
static_assert(sizeof(CarBody) == 0xB40 - 0x2C);
static_assert(offsetof(Car, padSlot) == 0x18);
static_assert(offsetof(Car, finishTime) == 0x24);
static_assert(offsetof(Car, body) == 0x2C);
static_assert(sizeof(Car) == 0xB40);

// Global simulation constants of the original (re/structs/car.yaml, `globals`).
struct StepGlobals {
    int32_t frameTime = 2184;   // 0x801C856C  one simulation frame in 16.16 s (1/30 s)
    int32_t rate = 30;          // 0x801C8570  simulation frames per second
    int32_t draftDragFloor = 1024; // 0x80046EF4  drag factor (of 0x1000) kept at full slipstream
};

// Corners of the car's rectangle in the simulation plane for the current heading (original: 0x80041AE8).
void UpdateFootprint(CarBody& body);

// Sweeps the current footprint from the previous corner set against the course walls. Returns a bit mask of
// the corners that hit; `fraction` = earliest hit (0x1000 = none), `corner` = its index. Records the wall
// normal in the body (original: 0x80041CCC).
uint32_t CheckWalls(const Track& track, CarBody& body, int32_t& fraction, uint32_t& corner);

// Steering bias for sliding along the wall just hit (original: 0x80033D34). `rate` is the simulation step
// scale the original keeps in a global (0x801C8570).
void UpdateScrapeDirection(CarBody& body, int32_t rate);

// One movement step: applies the displacement (x, y, height, heading), checks the walls and moves back by
// the fraction that penetrated. Returns the corner hit mask (original: 0x80033E6C).
//   mode 0: normal step (scrape direction reset, then updated); 1: keep the corner set; 2: like 0 plus a
//   footprint refresh at the end.
//   collisionDisabled: global debug flag of the original (0x800A9520).
uint32_t MoveBody(const Track& track, CarBody& body, const int32_t delta[4], int32_t& remaining, int mode, int32_t rate,
                  bool collisionDisabled);

// Per-step time values from the body's time scale (original: 0x8004232C).
void SetStepTime(CarBody& body, const StepGlobals& globals);

// Air drag and downforce for this step from the forward speed and the slipstream blend (original: 0x8003DAA8).
void UpdateAero(CarBody& body, const StepGlobals& globals);

// Displacement of one step from the velocity and the yaw rate: x, y, height (1/4096 m) and heading (angle
// units). Original: 0x80030330, which writes these into a scratch area for the move pass.
void ComputeDisplacement(const CarBody& body, int32_t delta[4]);

// Maps a pad record { u16 flags; s16 steer; u16; u16 word3 } to the body's inputs (original: 0x8003E020).
// flags bit 0: analogue steering (steer is -0x1000..0x1000), else digital (sign only); bit 1: word3 is analogue.
void MapPadInput(CarBody& body, const uint16_t pad[4]);

// |a| + |b| - min(|a|, |b|) / 2: cheap vector length (original: 0x8003C360).
int32_t ApproxLength(int32_t a, int32_t b);

// Velocity response to the wall just hit (original: 0x800340A4): keeps only the component along the recorded
// wall normal, damps the vertical speed accordingly and, unless `mode` is 1, rates the impact (wallImpact,
// wallScrape), damps all velocity by the impact strength and moves the body along the remaining fraction of
// the step with a second, corner-set-preserving move. The two wall-response constant sets of the original
// (0x80046E20) are selected by the course flag "dirt" (course table entry bit 2).
struct MoveContext {
    const Track* track = nullptr;
    StepGlobals globals;
    bool collisionDisabled = false;  // 0x800A9520
    bool dirtCourse = false;         // course table flag (0x80060E94(course)->+8 & 4)
    int controlClass = 0;            // 0x800419E8 result of the game shell: 2 = player-controlled race car
};
// `delta` is overwritten with the displacement of the remaining fraction (the original reuses the caller's array).
void RespondToWall(const MoveContext& context, CarBody& body, int32_t delta[4], int32_t remaining, int mode);

// Displacement for the remaining fraction of the step from the (already responded) velocity (original: 0x80030424).
void ScaleDisplacement(const CarBody& body, int32_t delta[4], int32_t remaining);

// Move pass of the step for every car (original: 0x80034320): move + wall check, wall response, scrape decay
// and corner damage. `deltas` are the per-car displacements from ComputeDisplacement.
// The displacement of a car that hit a wall is replaced by the one of the remaining fraction.
void MovePass(const MoveContext& context, Car* cars, int count, int32_t (*deltas)[4]);

// 0x1000 at |value| <= threshold, decreasing linearly by `gain` (1.0 = 0x1000) per unit beyond it, 0 at the far
// end (original: 0x8003DFDC).
int32_t Falloff(int32_t value, int32_t gain, int32_t threshold);

} // namespace gt2::sim
