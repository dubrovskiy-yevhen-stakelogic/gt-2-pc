#include "game/sim/drive_shafts.h"

#include "game/sim/drivetrain.h"
#include "game/sim/fixed.h"
#include "game/sim/trig.h"

// Every routine here reproduces the integer arithmetic of the original exactly: 32-bit sums and products wrap,
// `>>` is arithmetic, "/ 2" is the compiler's round-towards-zero halving. The body fields are the named members
// of CarBody / Wheel (car_body.h, which lists the original offsets); the drivetrain parameter block starts at
// body + 0x370 (car + 0x39C in the original), the engine block at body + 0x7C.
namespace gt2::sim {

namespace {

// ---- wrapping 32-bit arithmetic ----
inline int32_t Add(int32_t a, int32_t b) { return int32_t(uint32_t(a) + uint32_t(b)); }
inline int32_t Sub(int32_t a, int32_t b) { return int32_t(uint32_t(a) - uint32_t(b)); }
inline int32_t Neg(int32_t a) { return int32_t(0u - uint32_t(a)); }
inline int32_t Shl(int32_t a, int shift) { return int32_t(uint32_t(a) << shift); }
inline int32_t MulLo(int32_t a, int32_t b) { return int32_t(uint32_t(a) * uint32_t(b)); }        // mult + mflo
inline int32_t MulHi(int32_t a, int32_t b) { return int32_t(uint64_t(int64_t(a) * int64_t(b)) >> 32); } // mult + mfhi
inline int32_t Half(int32_t a) { return Add(a, int32_t(uint32_t(a) >> 31)) >> 1; }               // a / 2 towards zero
inline int32_t Abs(int32_t a) { return a < 0 ? Neg(a) : a; }
// Low word of the 64-bit quotient (value << shift) / divisor, as the original's calls of 0x80086084 use it.
inline int32_t DivShifted(int32_t value, int shift, int32_t divisor) {
    return int32_t(uint64_t(Div64(int64_t(value) << shift, int64_t(divisor))));
}
// Signed division by 60 as the compiler emits it: multiply by 0x88888889, add, shift by 5, correct the sign.
inline int32_t DivBy60(int32_t value) {
    const int32_t high = MulHi(value, int32_t(0x88888889u));
    return Sub(Add(high, value) >> 5, value >> 31);
}

// The original reads the gear (u8 at body + 0x618) and the clutch state (u8 at 0x619) as one little-endian u16:
// 0x100 = reverse engaged, 0x101 = gear 1 engaged.
inline uint32_t GearAndClutch(const CarBody& body) { return (uint32_t(body.clutchState) << 8) | body.gear; }

// Wheel::contactFlags bits: bit 3 = driven by the primary axle, bit 2 = by the secondary axle.
inline void OrWheelFlags(CarBody& body, size_t wheel, uint8_t bits) {
    body.wheels[wheel].contactFlags = uint8_t(body.wheels[wheel].contactFlags | bits);
}

// Sum of the brake torques of both wheels of an axle / of all wheels: non-zero means the brakes are applied, and
// a shaft that reverses its direction (or starts from rest) while braking is held at zero.
inline int32_t AxleBrake(const DriveCarWork& work, size_t axle) {
    return Add(work.wheels[axle * 2].brakeTorque, work.wheels[axle * 2 + 1].brakeTorque);
}
inline int32_t AnyBrake(const DriveCarWork& work) { return Add(AxleBrake(work, 0), AxleBrake(work, 1)); }

} // namespace

// ---------------------------------------------------------------------------------------------------------------
int32_t WrapAngle(int32_t angle) { // 0x800450A0
    if (angle < -0x800) {
        do angle = Add(angle, 0x1000); while (angle < -0x800);
        return angle;
    }
    while (angle >= 0x800) angle = Sub(angle, 0x1000);
    return angle;
}

int32_t AngleDifference(int32_t a, int32_t b) { // 0x800450E0
    int32_t d = Sub(WrapAngle(a), WrapAngle(b));
    if (d >= 0x800) d = Sub(d, 0x1000);
    else if (d < -0x800) d = Add(d, 0x1000);
    return d;
}

// ---------------------------------------------------------------------------------------------------------------
// The engine routines the drive shafts call (TurboBoostSum, BoostMultiplier, BaseEngineTorque, EngineTorqueStep)
// live in drivetrain.cpp.

int32_t GovernedEngineStep(CarBody& body, int32_t stepTime) { // 0x8004530C
    const uint32_t primary = body.primaryAxle, gear = body.gear;
    const int32_t ratio = body.gearRatio[gear];
    // Engine-side speed of the primary axle, scaled by 652/4096 of the ratio.
    int32_t engineSide = Mul12Wide(body.axleSpeed[primary], MulLo(ratio, 652) >> 12);
    if (gear == 0) engineSide = Neg(engineSide);
    int32_t torque = Mul12Floor(body.effectiveThrottle, 1176);
    // Loss proportional to the engine-side speed: 784 * engineSide / 341280 (the compiler's multiply-high division).
    const int32_t scaled = MulLo(engineSide, 784);
    const int32_t loss = Sub(MulHi(scaled, 0x624DD93D) >> 17, scaled >> 31);
    const int32_t throttleInGear1 = gear == 1 ? body.effectiveThrottle : 0;
    // Target engine speed from the forward speed: 0 below 0.83 m/s, 16.67 below 2.78 m/s, then a ramp up to
    // 36.1 m/s, then the power speed (upshiftRpm doubles as the governed engine's power rpm).
    const int32_t speed = body.forwardSpeed;
    int32_t targetSpeed = 0;
    if (speed >= 3414) {
        if (speed < 11380) {
            targetSpeed = 0x10AAA;
        } else if (speed > 0x241E4) {
            targetSpeed = DivBy60(Shl(body.upshiftRpm, 12));
        } else {
            const int32_t slope = DivBy60(Shl(Sub(body.upshiftRpm, 1000), 12));
            const int64_t product = int64_t(Sub(speed, 11380)) * int64_t(slope);
            targetSpeed = Add(int32_t(uint64_t(Div64(product, 0x21570))), 0x10AAA);
        }
    }
    targetSpeed = MulLo(throttleInGear1, targetSpeed) >> 12;
    if (targetSpeed <= 0x10A66) targetSpeed = 0;
    int32_t demand = Mul12Floor(Sub(targetSpeed, body.engineSpeed), 245);
    if (demand >= 0x1001) demand = 0x1000;
    else if (demand < 0) demand = 0;
    const int32_t baseTorque = BaseEngineTorque(body, demand);
    body.engineLoad = int16_t(demand);
    const int32_t engineSpeed = Add(body.engineSpeed, Mul16Wide(stepTime, Mul12Wide(baseTorque, body.engineInvInertia)));
    body.engineSpeed = engineSpeed;
    if (targetSpeed == 0 && engineSpeed <= 0x8554) body.engineSpeed = Half(engineSpeed);
    if (body.engineSpeed > 0x10AAA && engineSide > 0x8555) {
        const int32_t surplus = Mul12Floor(throttleInGear1, Sub(body.engineTorque, baseTorque));
        torque = Add(torque, Div(MulLo(surplus, body.engineSpeed), engineSide));
    }
    int32_t effect757 = 0, effect758 = 0;
    if (body.engineSpeed >= 6827) {
        const int32_t scaledSpeed = Shl(body.engineSpeed, 6);
        effect758 = Add(MulLo(3, body.engineLoad), 0x1000) >> 7;
        effect757 = Sub(MulHi(scaledSpeed, 0x0F5C2B6B) >> 14, scaledSpeed >> 31);
    }
    body.engineVisual1 = uint8_t(effect758);
    body.engineVisual0 = uint8_t(effect757);
    body.exhaustFlame = 0;
    body.turboSpoolMax = 0;
    if (body.clutchRequest != 0 && body.brake != 0) {
        body.exhaustFlame = 64;
        body.turboSpoolMax = int16_t(Mul12Floor(Abs(engineSide), 60));
    }
    return Sub(torque, loss);
}

int32_t ClutchStartEngineStep(CarBody& body, DriveStepWork& work, int32_t stepTime) { // 0x80045138
    DriveCarWork& block = work.cars[body.carIndex];
    int32_t engagement = 0x1000;
    if (GearAndClutch(body) == 0x101u) {
        engagement = DivShifted(body.engineSpeed, 12, block.clutchOutputSpeed);
        if (engagement >= 0x1001) engagement = 0x1000;
        else if (engagement < body.gearRatio[2]) engagement = body.gearRatio[2]; // gearRatio[2] is the lower bound here
        // Target: launch speed (downshiftRpm[0] doubles as the launch rpm) relative to the clutch output speed.
        const int32_t launchSpeed = DivBy60(Shl(body.downshiftRpm[0], 12));
        int32_t target = 0x1000;
        if (launchSpeed < block.clutchOutputSpeed) {
            target = DivShifted(launchSpeed, 12, block.clutchOutputSpeed);
            if (target < body.gearRatio[2]) target = body.gearRatio[2];
        }
        if (target != engagement) {
            const int32_t rate = MulLo(Shl(stepTime, 9) >> 16, body.effectiveThrottle) >> 12;
            if (engagement < target) {
                engagement = Add(engagement, rate);
                if (target < engagement) engagement = target;
            } else {
                engagement = Sub(engagement, rate);
                if (engagement < target) engagement = target;
            }
        }
    }
    block.clutchEngagement = engagement;
    const int32_t throttle = body.effectiveThrottle;
    const int32_t torque = BaseEngineTorque(body, throttle);
    body.engineLoad = int16_t(throttle);
    return Mul12Wide(engagement, torque);
}

int32_t ClutchSlipFactor(int32_t speedDifference) { // 0x800392D8
    int32_t d = Add(Abs(speedDifference), -0x8555);
    if (d < 0) d = 0;
    const int32_t factor = Add(Mul12Floor(d, 30), 0x1000);
    return factor >= 0x1200 ? 0x1200 : factor;
}

int32_t CentreCouplingFraction(const CarBody& body, int32_t shaftTorque, int32_t rearTorque) { // 0x800459A8
    const int32_t steer = Abs(body.yawAccel); // relaxes the coupling above 1024 units
    int32_t steerFactor = 0x1000;
    if (steer >= 0x401) steerFactor = steer < 0x1000 ? Sub(0x1400, steer) : 0x400;
    const int32_t lockTorque = body.centreLockTorque;
    if (lockTorque != 0) {
        if (GearAndClutch(body) == 0x100u || body.clutchState == 0) return kCouplingLocked;
        if (steerFactor == 0x1000) {
            if (body.clutchState == 2 || body.brake >= 0xC01) return kCouplingLocked;
            const int32_t sum = Add(shaftTorque, rearTorque);
            if (shaftTorque > 0 && sum > 0) return kCouplingLocked;
            if (shaftTorque < 0 && sum < 0) return kCouplingLocked;
        }
    }
    const int32_t speedDifference = Abs(Sub(body.axleSpeed[1], body.axleSpeed[0]));
    const int32_t fraction = Mul12Floor(steerFactor, Mul12Floor(body.centreSplit, speedDifference));
    if (fraction < 0x1000) return fraction;
    return lockTorque == 0 ? 0x1000 : kCouplingLocked;
}

// ---------------------------------------------------------------------------------------------------------------
void ComputeWheelTorques(CarBody& body, DriveStepWork& work, size_t car) { // 0x80045688
    work.stepTime = body.stepTime;
    DriveCarWork& block = work.cars[car];
    for (size_t wheel = 0; wheel < 4; wheel++) {
        DriveWheelWork& record = block.wheels[wheel];
        const int32_t radius = body.wheelRadius[wheel >> 1];
        const int32_t roadTorque = Neg(Mul12Wide(radius, Add(record.roadForce[0], record.roadForce[1])));
        record.torque = roadTorque;
        const int32_t spin = body.wheels[wheel].rimSpeed;
        if (spin != 0) record.torque = spin > 0 ? Sub(roadTorque, record.brakeTorque) : Add(roadTorque, record.brakeTorque);
    }
}

void ApplyActiveDifferentialBias(CarBody& body, DriveStepWork& work, size_t car) { // 0x800457B0
    work.stepTime = body.stepTime;
    DriveCarWork& block = work.cars[car];
    for (size_t axle = 0; axle < 2; axle++) {
        const uint32_t type = body.axleDiffType[axle];
        if (type < 6 || type >= 8 || body.forwardSpeed <= 0x2C73) continue; // active types only, above 10 km/h
        const int32_t bias = MulLo(block.activeDiffFactor, body.axleDiffMinTorque[axle]) >> 12;
        if (bias == 0) continue;
        DriveWheelWork& left = block.wheels[axle * 2];
        DriveWheelWork& right = block.wheels[axle * 2 + 1];
        // Target for the right wheel: the axle's mean speed, shifted by an eighth towards the steering direction.
        int32_t target = Half(Add(body.wheels[axle * 2].rimSpeed, body.wheels[axle * 2 + 1].rimSpeed));
        const int32_t eighth = (target < 0 ? Add(target, 7) : target) >> 3;
        const int32_t steer = body.steerAngle;
        if (steer > 0) target = Add(target, eighth);
        else if (steer < 0) target = Sub(target, eighth);
        int32_t sevenSixteenths = Sub(Shl(bias, 3), bias);
        if (sevenSixteenths < 0) sevenSixteenths = Add(sevenSixteenths, 15);
        sevenSixteenths >>= 4;
        int32_t nineSixteenths = Add(Shl(bias, 3), bias);
        if (nineSixteenths < 0) nineSixteenths = Add(nineSixteenths, 15);
        nineSixteenths >>= 4;
        const int32_t rightSpeed = body.wheels[axle * 2 + 1].rimSpeed;
        if (rightSpeed < target) {
            right.torque = Add(right.torque, sevenSixteenths);
            left.torque = Sub(left.torque, nineSixteenths);
        } else if (rightSpeed > target) {
            right.torque = Sub(right.torque, nineSixteenths);
            left.torque = Add(left.torque, sevenSixteenths);
        }
    }
}

void UpdateDriveShafts(CarBody& body, DriveStepWork& work, size_t car, const DrivetrainGlobals& globals) { // 0x80045AE8
    const int32_t stepTime = body.stepTime;
    work.stepTime = stepTime;
    DriveCarWork& block = work.cars[car];
    const uint32_t driveType = body.driveType, clutchState = body.clutchState;

    // Driven-wheel flags (bit 3 primary axle, bit 2 secondary axle).
    if (clutchState != 0 && driveType < 7) {
        switch (driveType) {
        case 0: case 4: case 5: OrWheelFlags(body, 2, 8); OrWheelFlags(body, 3, 8); break;
        case 1: OrWheelFlags(body, 0, 8); OrWheelFlags(body, 1, 8); break;
        case 2: OrWheelFlags(body, 0, 8); OrWheelFlags(body, 1, 8); OrWheelFlags(body, 2, 4); OrWheelFlags(body, 3, 4); break;
        default: OrWheelFlags(body, 0, 4); OrWheelFlags(body, 1, 4); OrWheelFlags(body, 2, 8); OrWheelFlags(body, 3, 8); break; // 3, 6
        }
    }

    // Engine and clutch: torque entering the gearbox; the engine speed integrates the torque the clutch does
    // not pass on. The original reads the byte at body + 0x372 (the forward gear count) as the engine control
    // mode here: 1 = governed engine (0x8004530C), 2 = clutch start (0x80045138), otherwise the throttle.
    int32_t shaftTorque = 0;
    const uint32_t engineMode = body.forwardGears;
    if (engineMode == 1) {
        shaftTorque = GovernedEngineStep(body, stepTime);
        if (clutchState == 0) shaftTorque = 0;
    } else if (clutchState != 0) {
        const int32_t engineTorque = engineMode == 2 ? ClutchStartEngineStep(body, work, stepTime) : EngineTorqueStep(body, body.effectiveThrottle, stepTime);
        if (clutchState == 1) {
            shaftTorque = engineTorque;
        } else {
            int32_t coupled;
            if (clutchState == 3) coupled = body.lockedClutchTorque;
            else if (engineTorque > 0) coupled = Mul12Floor(ClutchSlipFactor(Sub(block.clutchInputSpeed, block.clutchOutputSpeed)), engineTorque);
            else coupled = body.overrunClutchTorque;
            if (block.clutchInputSpeed < block.clutchOutputSpeed) coupled = Neg(coupled);
            const int32_t engineAccel = Mul16Wide(stepTime, Mul12Wide(Sub(engineTorque, coupled), body.engineInvInertia));
            body.engineSpeed = Add(body.engineSpeed, engineAccel);
            shaftTorque = coupled;
        }
    }

    // Gearbox.
    const uint32_t gear = body.gear;
    if (gear == 0) shaftTorque = Neg(shaftTorque);
    const int32_t ratio = body.gearRatio[gear];
    shaftTorque = Mul12Wide(shaftTorque, ratio);
    const int32_t frontTorque = Add(block.wheels[0].torque, block.wheels[1].torque);
    const int32_t rearTorque = Add(block.wheels[2].torque, block.wheels[3].torque);

    // Centre coupling: mode 1 = a torque split between the axles, 2 = centre differential, 3 = locked.
    int32_t couplingMode = 0, couplingTorque = 0; // couplingTorque: torque sent to the front axle by types 4..6
    if (driveType == 2) {
        couplingMode = 1;
    } else if (driveType == 3) {
        couplingMode = 2;
    } else if (driveType >= 4 && driveType < 7) {
        int32_t requested;
        if (driveType == 6) {
            requested = Mul12Wide(body.centreSplit, shaftTorque);
        } else if (driveType == 4) {
            const int32_t split = body.centreSplit;
            if (split == 0x1000 || shaftTorque <= 0 || GearAndClutch(body) == 0x100u) requested = kCouplingLocked;
            else requested = Mul12Floor(split, shaftTorque);
        } else {
            requested = CentreCouplingFraction(body, shaftTorque, rearTorque);
            if (requested != kCouplingLocked) requested = Half(Mul12Floor(requested, shaftTorque));
        }
        if (body.centreLocked != 0) {
            if (requested == kCouplingLocked) {
                couplingMode = 3;
            } else if (body.centreLockTorque < Abs(Sub(Sub(frontTorque, rearTorque), shaftTorque))) {
                body.centreLocked = 0;
            } else {
                couplingMode = 3;
            }
        }
        if (body.centreLocked == 0) {
            couplingTorque = requested;
            if (requested == kCouplingLocked) {
                couplingTorque = body.centreLockTorque;
                if (Sub(body.axleSpeed[1], body.axleSpeed[0]) < 0) couplingTorque = Neg(couplingTorque);
            }
            if (couplingTorque != 0) couplingMode = 1;
        }
        if (couplingMode != 0) { OrWheelFlags(body, 0, 4); OrWheelFlags(body, 1, 4); }
    }

    // Engine inertia seen by the axles (through the gear ratio, scaled by the clutch engagement in clutch-start mode).
    int32_t reflectedInertia = 0;
    if (engineMode != 1 && clutchState == 1) {
        reflectedInertia = MulLo(body.engineInertiaRef, ratio) >> 12;
        if (engineMode == 2) reflectedInertia = MulLo(block.clutchEngagement, reflectedInertia) >> 12;
    }

    if (couplingMode == 3) { // both axles turn as one
        const int32_t total = Add(Add(shaftTorque, frontTorque), rearTorque);
        const int32_t inertia = Add(Add(body.axleInertia[0], body.axleInertia[1]), reflectedInertia);
        int32_t accel = Mul16Wide(stepTime, DivShifted(total, 16, inertia));
        const int32_t previous = body.axleSpeed[0];
        if (globals.raceModeByte == 3 && driveType == 5 && body.brake > 0xC00) body.axleSpeed[0] = Add(previous, accel);
        else body.axleSpeed[0] = Add(previous, Half(Add(body.wheels[1].accel, accel)));
        if (AnyBrake(block) != 0 && (Mul12Floor(body.axleSpeed[0], previous) < 0 || previous == 0)) {
            body.axleSpeed[0] = 0;
            accel = 0;
        }
        body.wheels[3].accel = int16_t(accel);
        body.wheels[1].accel = int16_t(accel);
        body.axleSpeed[1] = body.axleSpeed[0];
        block.axleTorque[0] = block.axleTorque[1] = Half(shaftTorque);
        return;
    }
    if (couplingMode == 2) { // centre differential: common speed plus a difference driven by the torque imbalance
        const int32_t previousMean = Half(Add(body.axleSpeed[0], body.axleSpeed[1]));
        const int32_t total = Add(Add(shaftTorque, frontTorque), rearTorque);
        const int32_t inertia = Add(Add(body.axleInertia[0], body.axleInertia[1]), reflectedInertia);
        int32_t accel = Mul16Wide(DivShifted(total, 16, inertia), stepTime);
        int32_t mean = Add(previousMean, Half(Add(body.wheels[1].accel, accel)));
        if (AnyBrake(block) != 0 && (Mul12Floor(mean, previousMean) < 0 || previousMean == 0)) {
            mean = 0;
            accel = 0;
        }
        body.wheels[1].accel = int16_t(accel);
        const int32_t difference = Sub(body.axleSpeed[0], body.axleSpeed[1]);
        const int32_t imbalance = Sub(Sub(frontTorque, rearTorque), Mul12Wide(body.centreSplit, difference));
        int32_t differenceAccel = Mul16Wide(Mul12ShiftWide(imbalance, body.centreDiffStiffness, uint32_t(-4)), stepTime);
        const int32_t halfDifference = Half(difference);
        int32_t newHalf = Add(halfDifference, Half(Add(body.wheels[3].accel, differenceAccel)));
        if (MulLo(halfDifference, newHalf) < 0) { // the difference changed sign: stop at zero
            newHalf = 0;
            differenceAccel = Neg(halfDifference);
        }
        body.axleSpeed[0] = Add(mean, newHalf);
        body.axleSpeed[1] = Sub(mean, newHalf);
        body.wheels[3].accel = int16_t(differenceAccel);
        block.axleTorque[0] = block.axleTorque[1] = Half(shaftTorque);
        return;
    }

    // Independent axles: torque and inertia per axle by drive type.
    int32_t axleInertia[2] = {0, 0};
    switch (driveType) {
    case 0: // rear drive
        axleInertia[1] = Add(body.axleInertia[1], reflectedInertia);
        block.axleTorque[0] = 0;
        block.axleTorque[1] = shaftTorque;
        break;
    case 1: // front drive
        axleInertia[0] = Add(body.axleInertia[0], reflectedInertia);
        block.axleTorque[0] = shaftTorque;
        block.axleTorque[1] = 0;
        break;
    case 2: { // 4WD, front primary: the rear gets a share of the speed difference
        axleInertia[0] = Add(body.axleInertia[0], reflectedInertia);
        axleInertia[1] = body.axleInertia[1];
        const int32_t transfer = Mul12Wide(body.centreSplit, Sub(body.axleSpeed[0], body.axleSpeed[1]));
        block.axleTorque[0] = Sub(shaftTorque, transfer);
        block.axleTorque[1] = transfer;
        break;
    }
    case 4: case 5: case 6: // 4WD, rear primary, coupling torque to the front
        axleInertia[0] = body.axleInertia[0];
        axleInertia[1] = Add(body.axleInertia[1], reflectedInertia);
        block.axleTorque[0] = couplingTorque;
        block.axleTorque[1] = Sub(shaftTorque, couplingTorque);
        break;
    default: // 3 never gets here (centre differential); 7+ leave the locals uninitialised in the original
        break;
    }
    const int32_t previousDifference = couplingMode == 1 ? Sub(body.axleSpeed[0], body.axleSpeed[1]) : 0;
    const int32_t axleRoadTorque[2] = {frontTorque, rearTorque};
    for (size_t axle = 0; axle < 2; axle++) {
        if (body.axleDiffType[axle] == 0) continue;
        int32_t accel = 0;
        const int32_t previous = body.axleSpeed[axle];
        if (axleInertia[axle] != 0)
            accel = Mul16Wide(DivShifted(Add(block.axleTorque[axle], axleRoadTorque[axle]), 16, axleInertia[axle]), stepTime);
        const size_t rightWheel = axle * 2 + 1;
        body.axleSpeed[axle] = Add(previous, Half(Add(body.wheels[rightWheel].accel, accel)));
        if (AxleBrake(block, axle) != 0 && (Mul12Floor(body.axleSpeed[axle], previous) < 0 || previous == 0)) {
            body.axleSpeed[axle] = 0;
            accel = 0;
        }
        body.wheels[rightWheel].accel = int16_t(accel);
    }
    if (couplingMode == 1) { // the coupling reversed: the secondary axle takes the primary's speed, types 4/5 lock
        const int32_t difference = Sub(body.axleSpeed[0], body.axleSpeed[1]);
        if ((previousDifference > 0 && difference <= 0) || (previousDifference < 0 && difference >= 0)) {
            const uint32_t primary = body.primaryAxle;
            body.axleSpeed[1u - primary] = body.axleSpeed[primary];
            if (driveType - 4 < 2) body.centreLocked = 1;
        }
    }
}

void UpdateAxleDifferentials(CarBody& body, DriveStepWork& work, size_t car) { // 0x800465E0
    const int32_t stepTime = body.stepTime;
    work.stepTime = stepTime;
    DriveCarWork& block = work.cars[car];
    for (size_t axle = 0; axle < 2; axle++) {
        const size_t leftWheel = axle * 2, rightWheel = axle * 2 + 1;
        DriveWheelWork& left = block.wheels[leftWheel];
        DriveWheelWork& right = block.wheels[rightWheel];
        const int32_t speedDifference = Sub(body.wheels[leftWheel].rimSpeed, body.wheels[rightWheel].rimSpeed);
        uint32_t type = body.axleDiffType[axle];
        bool locked = false;
        if (type == 1) {
            locked = true;
        } else if (type == 3 || type == 4) { // torque sensing: locking torque from the drive torque, with a lock latch
            const int32_t driveTorque = block.axleTorque[axle];
            int32_t lockTorque = driveTorque >= 0 ? Mul12Floor(body.axleDiffAccelRatio[axle], driveTorque)
                                                   : Mul12Floor(body.axleDiffDecelRatio[axle], Neg(driveTorque));
            if (lockTorque < body.axleDiffMinTorque[axle]) lockTorque = body.axleDiffMinTorque[axle];
            bool transfer = true;
            if (body.axleDiffLocked[axle] != 0) {
                const int32_t torqueDifference = Sub(left.torque, right.torque);
                if (torqueDifference <= Shl(lockTorque, 1) && torqueDifference >= Neg(Shl(lockTorque, 1))) {
                    locked = true;
                    transfer = false;
                } else {
                    body.axleDiffLocked[axle] = 0;
                }
            }
            if (transfer) {
                if (speedDifference > 0) { left.torque = Sub(left.torque, lockTorque); right.torque = Add(right.torque, lockTorque); }
                else if (speedDifference < 0) { left.torque = Add(left.torque, lockTorque); right.torque = Sub(right.torque, lockTorque); }
            }
        } else if (type == 5) { // speed sensing: locking torque from the squared speed difference
            int32_t squared = Mul12Floor(speedDifference, speedDifference);
            if (body.axleDiffMinTorque[axle] < squared) squared = body.axleDiffMinTorque[axle];
            const int32_t lockTorque = Mul12Floor(body.axleDiffAccelRatio[axle], squared);
            if (speedDifference > 0) { left.torque = Sub(left.torque, lockTorque); right.torque = Add(right.torque, lockTorque); }
            else if (speedDifference < 0) { left.torque = Add(left.torque, lockTorque); right.torque = Sub(right.torque, lockTorque); }
        }
        if (locked) type = 1;
        const int32_t radius = body.wheelRadius[axle];
        const int32_t axleSpeed = body.axleSpeed[axle];
        if (type == 0) { // open: each wheel integrates its own torque
            for (size_t i = 0; i < 2; i++) {
                const size_t wheel = leftWheel + i;
                const DriveWheelWork& record = block.wheels[wheel];
                int32_t accel = Mul16Wide(stepTime, Mul12ShiftWide(record.torque, body.axleInvInertia[axle], uint32_t(-4)));
                const int32_t previous = body.wheels[wheel].rimSpeed;
                int32_t speed = Add(previous, Half(Add(accel, body.wheels[wheel].accel)));
                if (record.brakeTorque != 0 && (previous == 0 || (previous > 0 && speed < 0) || (previous < 0 && speed > 0))) {
                    speed = 0;
                    accel = 0;
                }
                body.wheels[wheel].accel = int16_t(accel);
                body.wheels[wheel].rimSpeed = speed;
            }
        } else if (type == 1) { // locked: both wheels at the axle speed
            const int32_t rimSpeed = Mul12Wide(radius, axleSpeed);
            body.wheels[rightWheel].rimSpeed = rimSpeed;
            body.wheels[leftWheel].rimSpeed = rimSpeed;
            body.wheels[leftWheel].accel = 0;
        } else { // differential: the speed difference integrates the torque difference, stops when it reverses
            // The shift is 12 - 3 here (a2 = -3 is loaded in a delay slot at 0x800468CC, which Ghidra hides), 12 - 4 above.
            int32_t differenceAccel = Mul16Wide(Mul12ShiftWide(Sub(left.torque, right.torque), body.axleInvInertia[axle], uint32_t(-3)), stepTime);
            const int32_t halfDifference = Half(speedDifference);
            int32_t newHalf = Add(halfDifference, Half(Add(body.wheels[leftWheel].accel, differenceAccel)));
            if (MulLo(speedDifference, newHalf) < 0) {
                newHalf = 0;
                differenceAccel = 0;
                if (type - 3 < 2) body.axleDiffLocked[axle] = 1;
            }
            body.wheels[leftWheel].accel = int16_t(differenceAccel);
            const int32_t rimSpeed = Mul12Wide(radius, axleSpeed);
            body.wheels[leftWheel].rimSpeed = Add(rimSpeed, newHalf);
            body.wheels[rightWheel].rimSpeed = Sub(rimSpeed, newHalf);
        }
    }
}

void UpdateDrivetrain(CarBody* const* bodies, size_t count, DriveStepWork& work, const DrivetrainGlobals& globals) { // 0x80046B58
    for (size_t car = 0; car < count; car++) ComputeWheelTorques(*bodies[car], work, car);
    for (size_t car = 0; car < count; car++) ApplyActiveDifferentialBias(*bodies[car], work, car);
    for (size_t car = 0; car < count; car++) UpdateDriveShafts(*bodies[car], work, car, globals);
    for (size_t car = 0; car < count; car++) UpdateAxleDifferentials(*bodies[car], work, car);
}

} // namespace gt2::sim
