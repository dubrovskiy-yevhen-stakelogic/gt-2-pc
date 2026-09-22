#include "game/sim/physics_core.h"

#include <cstring>

#include "game/sim/fixed.h"
#include "game/sim/trig.h"

namespace gt2::sim {

namespace {

// ---- 32-bit wrapping arithmetic (MIPS addu / subu / negu / mult+mflo: no overflow traps) ----
int32_t Add(int32_t a, int32_t b) { return int32_t(uint32_t(a) + uint32_t(b)); }
int32_t Sub(int32_t a, int32_t b) { return int32_t(uint32_t(a) - uint32_t(b)); }
int32_t Neg(int32_t v) { return int32_t(0u - uint32_t(v)); }
int32_t Abs(int32_t v) { return v < 0 ? Neg(v) : v; }
int32_t MulLo(int32_t a, int32_t b) { return int32_t(uint32_t(a) * uint32_t(b)); }

constexpr int32_t kRollingSpeed = 11381;   // 2.78 m/s: the clutch engages while rolling from here on
constexpr int32_t kCreepRimSpeed = 5690;   // rim speed below which the standstill creep correction applies
constexpr int32_t kCreepGain = 0x3996;     // creep force per unit of contact patch velocity
constexpr int32_t kCreepLimit = 0x2000;    // ... saturating at
constexpr int32_t kYawRateLimit = 0x80000;

CarBody& BodyOf(Car* cars, int car) { return cars[car].body; }
DriveCarWork& BlockOf(DriveStepWork& work, int car) { return work.cars[size_t(car)]; }
CarScratch& TyreBlockOf(DriveStepWork& work, int car) { return *reinterpret_cast<CarScratch*>(&work.cars[size_t(car)]); }
uint32_t AngleIndex(int32_t angle) { return uint32_t(angle) & 0xFFFu; }

// rpm -> engine speed (rev/s << 12): (rpm << 12) / 60, signed division as the original's magic-number idiom.
int32_t RpmToEngineSpeed(uint32_t rpm) { return Div(int32_t(rpm << 12), 60); }

// Whether the input routine of a player-flagged body is the AI's (finished race or penalty over): 0x8003E0C4.
bool PlayerHandedToAi(const CarBody& body) { return body.raceState != 0 && body.penaltyFrames == 0; }

void RaiseAiInput(const PhysicsContext& context, CarBody& body, int car, GearRequest& request) {
    const AiDispatch dispatch = PrepareAiInput(body, request, context.holdFrames, context.move.globals.rate);
    if (dispatch.control != AiControl::None && context.aiInput) context.aiInput(context.user, body, car, dispatch);
}

// ---- 0x80039FC8, pass A: clutch state machine and the brake torques of one car ----
void ClutchAndBrakes(CarBody& body, DriveCarWork& block) {
    block.clutchOutputSpeed = 0;
    block.clutchInputSpeed = 0;
    if (body.forwardGears == 1) {
        SingleGearClutch(body);
    } else {
        const uint32_t clutch = body.clutchState;
        if (clutch == 0) {
            if (body.clutchRequest != 0) {
                const int32_t speed = body.forwardSpeed;
                const bool reverseGear = body.gear == 0;
                const bool rolling = (speed >= kRollingSpeed && !reverseGear) || (speed < -(kRollingSpeed - 1) && reverseGear);
                if (rolling) {
                    if (body.effectiveThrottle > 0x400) body.clutchState = 3;
                } else if (int32_t(body.downshiftFloorRpm) < body.engineRpm) { // the clutch engage rpm of the clutch start
                    body.clutchState = 2;
                } else {
                    body.clutchRequest = 0;
                }
            }
        } else if (clutch == 1) {
            if (body.clutchRequest == 0) body.clutchState = 0;
            else if (body.forwardGears == 2) block.clutchOutputSpeed = EngineSpeedFromWheels(body);
        }
        if (uint32_t(body.clutchState) - 2u < 2u) {
            block.clutchInputSpeed = body.engineSpeed;
            const int32_t wheelSpeed = EngineSpeedFromWheels(body);
            block.clutchOutputSpeed = wheelSpeed;
            if (body.clutchState == 3) body.effectiveThrottle = int16_t(wheelSpeed < block.clutchInputSpeed ? 0 : 0x1000);
        }
    }
    for (uint32_t w = 0; w < 4; w++) block.wheels[w].brakeTorque = MulLo(body.wheels[w].brakeInput, body.brakeTorque[w >> 1]) >> 12;
    const int32_t handbrakeTorque = int32_t(uint32_t(MulLo(body.handbrake, body.brakeTorque[2])) << 4) >> 16;
    for (uint32_t w = 2; w < 4; w++) block.wheels[w].brakeTorque = Add(block.wheels[w].brakeTorque, handbrakeTorque);
    for (uint32_t w = 0; w < 4; w++) {
        uint32_t flags = (block.wheels[w].brakeTorque != 0) ? 2u : 0u;
        if (body.wheels[w].load == 0) flags |= 1u;
        body.wheels[w].contactFlags = uint8_t(flags);
    }
}

// ---- pass B: rolling resistance of one car ----
void RollingResistance(const PhysicsContext& context, CarBody& body, CarScratch& block) {
    body.externalLongForce = 0;
    body.netLongitudinalForce = 0;
    body.externalLatForce = 0;
    body.forwardAccel = 0;
    int32_t loadSum = 0;
    for (uint32_t w = 0; w < 4; w++) loadSum = Add(loadSum, Mul12Floor(body.wheels[w].load, context.surfaceRolling[body.wheels[w].surface]));
    const int32_t speed = body.forwardSpeed;
    const CurveS32& curve = context.rollingResistance;
    int32_t resistance = 0;
    if (speed > 0) resistance = Neg(Mul12Floor(Interpolate(curve.xs, curve.ys, curve.count, speed), loadSum));
    else if (speed < 0) resistance = Mul12Floor(Interpolate(curve.xs, curve.ys, curve.count, Neg(speed)), loadSum);
    block.rollingResistance = resistance;
}

// ---- pass C: longitudinal tyre forces of one car from the slip-ratio curve ----
void LongitudinalForces(CarBody& body, CarScratch& block) {
    for (uint32_t w = 0; w < 4; w++) {
        Wheel& wheel = body.wheels[w];
        WheelScratch& record = block.wheels[w];
        int32_t force = Mul12Floor(record.slipForce, wheel.gripForce);
        if (record.slipSign < 0) force = Neg(force);
        const int32_t scaled = Mul12Wide(wheel.slipScale, force);
        if (record.slipMode >= 0) { // the rim drives the road
            record.driveForce = scaled;
            record.brakeForce = 0;
            wheel.driveForce = force;
            wheel.brakeForce = 0;
        } else {
            record.driveForce = 0;
            record.brakeForce = scaled;
            wheel.driveForce = 0;
            wheel.brakeForce = force;
        }
    }
}

// ---- pass D: lateral tyre forces of one car from the slip-angle curve, plus the standstill creep ----
void LateralForces(CarBody& body, const AxleTyreCurves curves[2]) {
    for (uint32_t w = 0; w < 4; w++) {
        Wheel& wheel = body.wheels[w];
        const int32_t blend = wheel.slipBlend;
        if (blend == 0) {
            wheel.lateralForce = 0;
            wheel.lateralFactorAbs = 0;
        } else {
            const bool rollingForward = wheel.rimSpeed > 0;
            const int32_t steer = wheel.steerAngle;
            const int32_t angle = AngleDifference(rollingForward ? steer : steer + 0x800, wheel.slipAngle);
            int32_t factor = Mul12Wide(blend, SymmetricCurve(curves[w >> 1].slipAngleForce, angle));
            int32_t limit = wheel.slipRatioGrip;
            if (limit < factor) {
                factor = limit;
            } else {
                limit = Neg(limit);
                if (factor < limit) factor = limit;
            }
            wheel.lateralFactorAbs = int16_t(Abs(factor));
            const int32_t lateral = Mul12(wheel.gripForce, factor);
            wheel.lateralForce = rollingForward ? lateral : Neg(lateral);
        }
        // Creep: a nearly stopped wheel whose rim speed matches the car's pushes back against the contact
        // patch velocity so that the car settles instead of drifting.
        const int32_t rim = wheel.rimSpeed;
        const int32_t difference = Sub(body.forwardSpeed, rim);
        if (uint32_t(rim) + uint32_t(kCreepRimSpeed - 1) < uint32_t(2 * kCreepRimSpeed - 1) && difference >= -1137 && difference < 1138) {
            const uint32_t index = AngleIndex(wheel.steerAngle);
            const int32_t creep = Add(Mul12Wide(Cos(index), wheel.contactLateralSpeed), Mul12Wide(Sin(index), wheel.contactForwardSpeed));
            int32_t correction = 0;
            if (creep != 0) {
                int32_t response;
                if (creep > 0) response = creep < 2276 ? Mul12(kCreepGain, creep) : kCreepLimit;
                else response = creep < -2275 ? -kCreepLimit : Mul12(kCreepGain, creep);
                const int32_t weight = MulLo(Sub(kCreepRimSpeed, Abs(rim)), 0xB84) >> 12; // 1.0 at rest, 0 at kCreepRimSpeed
                correction = int16_t(Mul12(weight, response));
            }
            wheel.lateralForce = Add(wheel.lateralForce, correction);
        }
    }
}

// ---- pass E: longitudinal integration of one car ----
void IntegrateLongitudinal(CarBody& body, DriveStepWork& work, int car) {
    CarScratch& block = TyreBlockOf(work, car);
    work.stepTime = body.stepTime;
    int32_t driveSum = 0;
    for (uint32_t w = 0; w < 4; w++) driveSum = Add(driveSum, Mul12(block.wheels[w].steerCos, body.wheels[w].driveForce));
    body.netLongitudinalForce = Add(body.netLongitudinalForce, driveSum);
    body.netLongitudinalForce = Add(body.netLongitudinalForce, block.rollingResistance);
    // Effective mass: the body plus the rotating mass of every wheel rolling on the ground (wheelInertiaFactor
    // is read as an unsigned 16-bit value by the original).
    int32_t rolling = 0;
    for (uint32_t w = 0; w < 4; w++)
        if ((body.wheels[w].contactFlags & 0xD) == 0) rolling = Add(rolling, int32_t(uint16_t(body.wheelInertiaFactor[w >> 1])));
    rolling = int16_t(rolling);
    block.inverseEffectiveMass = rolling != 0 ? Div(0x1000000, Add(rolling, body.mass)) : body.inverseMass;
    const int32_t inverseMass = block.inverseEffectiveMass;
    const int32_t netForce = Add(Add(body.netLongitudinalForce, body.externalLongForce), body.dragForce);
    body.forwardAccel = Add(body.forwardAccel, Mul12Wide(netForce, inverseMass));
    body.forwardSpeed = Add(body.forwardSpeed, Mul16Wide(body.forwardAccel, work.stepTime));
    if (body.forwardSpeed == 0) return;
    // Braking forces may only stop the car, never reverse it. (The original reads wheel 0's lateral force
    // for every wheel here.)
    int32_t brakeSum = 0;
    const int32_t wheel0Lateral = body.wheels[0].lateralForce;
    for (uint32_t w = 0; w < 4; w++) {
        brakeSum = Add(brakeSum, Mul12(block.wheels[w].steerCos, body.wheels[w].brakeForce));
        brakeSum = Sub(brakeSum, Mul12(block.wheels[w].steerSin, wheel0Lateral));
    }
    const int32_t brakeAccel = Mul12(inverseMass, brakeSum);
    const int32_t speedBefore = body.forwardSpeed;
    const int32_t speedAfter = Add(speedBefore, Mul16Wide(brakeAccel, work.stepTime));
    if ((speedBefore ^ speedAfter) < 0) {
        body.forwardAccel = Add(body.forwardAccel, Mul12Wide(body.stepTime, Neg(speedBefore)));
        body.forwardSpeed = 0;
    } else {
        body.netLongitudinalForce = Add(body.netLongitudinalForce, brakeSum);
        body.forwardAccel = Add(body.forwardAccel, brakeAccel);
        body.forwardSpeed = speedAfter;
    }
}

// ---- pass G: engine speed of one car with a clutch (body + 0x372 != 1) ----
void EngineStep(const PhysicsContext& context, CarBody& body, DriveCarWork& block, int32_t stepTime, bool partialClutch) {
    const int32_t idleSpeed = RpmToEngineSpeed(body.idleRpm);
    if (body.clutchState == 0) {
        // Clutch open: the engine spins up freely against its own inertia, never below idle.
        const int32_t torque = EngineTorqueStep(body, body.effectiveThrottle, stepTime);
        const int32_t accel = Mul12Wide(torque, body.engineInvInertia);
        body.engineSpeed = Add(body.engineSpeed, Mul16Wide(accel, stepTime));
        if (body.engineSpeed < idleSpeed) body.engineSpeed = idleSpeed;
        return;
    }
    const int32_t wheelSpeed = EngineSpeedFromWheels(body);
    const uint32_t clutch = body.clutchState;
    if (clutch == 1) {
        // Engaged: locked to the wheels (through the clutch engagement in the clutch-start mode).
        if (body.forwardGears == 2) body.engineSpeed = MulLo(block.clutchEngagement, wheelSpeed) >> 12;
        else body.engineSpeed = wheelSpeed;
    } else if (clutch - 2u < 2u) {
        // Engaging: locks once the engine speed and the wheel speed have crossed since the engagement began.
        const int32_t output = block.clutchOutputSpeed, input = block.clutchInputSpeed;
        const int32_t engine = body.engineSpeed;
        const bool crossed = (input <= output && wheelSpeed <= engine) || (output <= input && engine <= wheelSpeed);
        if (crossed && !partialClutch) {
            body.clutchState = 1;
            body.engineSpeed = wheelSpeed;
            body.shiftTimer = uint8_t(Div(context.move.globals.rate, 5));
        }
        if (body.clutchState == 3 && output <= idleSpeed) body.clutchState = 0;
        if (body.clutchState == 2 && body.brake == 0x1000 && body.engineSpeed < RpmToEngineSpeed(uint32_t(body.idleRpm) + 500u))
            body.clutchState = 0;
    }
    const int32_t engine = body.engineSpeed;
    if (engine < 0) {
        body.engineSpeed = 0;
    } else {
        const int32_t limit = RpmToEngineSpeed(uint32_t(body.revLimitRpm) + 10u);
        if (engine >= limit) body.engineSpeed = limit;
    }
}

// ---- pass H: lateral / yaw integration of one car ----
void IntegrateLateral(CarBody& body, DriveStepWork& work, int car) {
    work.stepTime = body.stepTime;
    if (int8_t(body.airborne) != 0) { // read as a signed byte by the original
        body.lateralAccel = 0;
        body.yawAccel = 0;
        return;
    }
    const CarScratch& block = TyreBlockOf(work, car);
    const int32_t frontTrack = body.halfTrack[0], rearTrack = body.halfTrack[1];
    const int32_t frontOffset = body.axleOffset[0], rearOffset = body.axleOffset[1];
    const int16_t lever[4] = {int16_t(Neg(frontTrack)), int16_t(frontTrack), int16_t(Neg(rearTrack)), int16_t(rearTrack)};
    const int16_t arm[4] = {int16_t(frontOffset), int16_t(frontOffset), int16_t(Neg(rearOffset)), int16_t(Neg(rearOffset))};
    int32_t yawTorque = 0, lateralSum = 0;
    for (uint32_t w = 0; w < 4; w++) {
        const Wheel& wheel = body.wheels[w];
        const int32_t longitudinal = Add(wheel.driveForce, wheel.brakeForce);
        const int32_t lateral = wheel.lateralForce;
        const int32_t s = block.wheels[w].steerSin, c = block.wheels[w].steerCos;
        const int32_t lateralTerm = Sub(Neg(Mul12Wide(c, lateral)), Mul12Wide(s, longitudinal));
        lateralSum = Add(lateralSum, lateralTerm);
        const int32_t longitudinalTerm = Sub(Mul12Wide(c, longitudinal), Mul12Wide(s, lateral));
        yawTorque = Add(yawTorque, Sub(Mul12Wide(lever[w], longitudinalTerm), Mul12Wide(arm[w], lateralTerm)));
    }
    int32_t yawRate = Add(body.yawRate, Mul16Wide(work.stepTime, Mul12Wide(body.inverseYawInertia, yawTorque)));
    if (yawRate > kYawRateLimit) yawRate = kYawRateLimit;
    else if (yawRate < -kYawRateLimit) yawRate = -kYawRateLimit;
    body.yawRate = yawRate;
    body.lateralAccel = Mul12Wide(Sub(lateralSum, body.externalLatForce), body.inverseMass);
    body.yawAccel = int16_t(Mul12Wide(body.lateralAccel, body.inverseGripAtZeroSlip));
}

// ---- pass I: velocity integration of one car (plane components only) ----
void IntegrateVelocity(CarBody& body, DriveStepWork& work) {
    work.stepTime = body.stepTime;
    const bool held = body.scriptedControl != 0 || body.penaltyFrames != 0 || body.raceState == 7;
    if (held) {
        body.netLongitudinalForce = 0;
        body.yawRate = 0;
        body.velocity[0] = 0;
        body.velocity[1] = 0;
        if (body.penaltyFrames == 0) {
            body.pitchRate = 0;
            body.rollRate = 0;
            body.velocity[2] = 0;
        }
        return;
    }
    if (int8_t(body.airborne) == 0) {
        bool resting = uint32_t(body.forwardSpeed) + 1137u < 2275u;
        for (uint32_t w = 0; w < 4 && resting; w++) resting = body.wheels[w].slipBlend == 0;
        bool loaded = false;
        for (uint32_t w = 0; w < 4; w++) loaded = loaded || (body.wheels[w].contactFlags & 2) != 0;
        if (resting && loaded) {
            // Standing still with load on the wheels: pin everything to zero.
            body.yawRate = 0;
            body.velocity[0] = 0;
            body.velocity[1] = 0;
            body.netLongitudinalForce = 0;
            body.forwardAccel = 0;
            body.lateralAccel = 0;
            body.yawAccel = 0;
            body.forwardSpeed = 0;
            body.lateralSpeed = 0;
        } else {
            const int32_t forwardDelta = Mul16Wide(body.forwardAccel, work.stepTime);
            const int32_t lateralDelta = Mul16Wide(body.lateralAccel, work.stepTime);
            for (uint32_t k = 0; k < 2; k++) {
                const int32_t delta = Add(Mul12(forwardDelta, body.basis[kRowForward][k]), Mul12(lateralDelta, body.basis[kRowLateral][k]));
                body.velocity[k] = Add(body.velocity[k], delta);
            }
        }
    }
    if (body.pushForce[0] != 0 || body.pushForce[1] != 0)
        for (uint32_t k = 0; k < 2; k++)
            body.velocity[k] = Add(body.velocity[k], Mul16Wide(Mul12Wide(body.pushForce[k], body.inverseMass), work.stepTime));
}

// ---- pass K: tyre wear of one car (only with the wear limit set) ----
void AccumulateWear(CarBody& body, DriveStepWork& work) {
    if ((body.flags78D & 0x10) != 0) return;
    work.stepTime = body.stepTime;
    for (uint32_t w = 0; w < 4; w++) {
        Wheel& wheel = body.wheels[w];
        wheel.wearRate = 0;
        if (wheel.surface != 0 || wheel.load == 0) continue;
        const uint32_t axle = w >> 1;
        const int32_t loadTerm = Div(Add(wheel.load, body.sprungWeightPerAxle[axle]), 3);
        const int32_t slipTerm = Add(MulLo(wheel.slipRatioAbs, body.gripLoadGain2[axle]) >> 12, MulLo(wheel.lateralFactorAbs, body.gripLoadGain[axle]) >> 12);
        const int32_t rate = MulLo(MulLo(slipTerm, loadTerm) >> 12, work.stepTime) >> 16;
        wheel.wear = Add(wheel.wear, rate);
        wheel.wearRate = uint8_t(rate < 256 ? rate : 255);
    }
}

// ---- pass L: pit request flags of one car ----
void UpdatePitFlags(const PhysicsContext& context, CarBody& body) {
    // The original tests bits 0xC00 of the 32-bit word at body + 0x78C: bits 2 / 3 of flags78D (the pit request).
    if ((body.flags78D & 0x0C) != 0) return;
    bool needsPit = false;
    if (context.wear.wearLimit != 0)
        for (uint32_t w = 0; w < 4 && !needsPit; w++)
            needsPit = body.wheels[w].wear >= 0 && body.wheels[w].wearGrip < context.wear.pitGripFactor;
    if (!needsPit && ControlClass(context, body) == 2)
        for (uint32_t w = 0; w < 4 && !needsPit; w++) needsPit = body.wheels[w].damage != 0;
    if (!needsPit) return;
    const uint32_t flags = body.flags78D;
    body.flags78D = uint8_t(flags | 4u);
    if (int8_t(body.controlClass) != 0) body.flags78D = uint8_t(flags | 0xCu);
}

// ---- 0x8003E0C4 helpers ----
void SteerWheels(const PhysicsContext& context, CarBody& body, CarScratch& block) {
    const int32_t steer = body.steerAngle;
    const int32_t outer = body.ackermann[0], inner = body.ackermann[1];
    int32_t left, right;
    if (steer >= 0) {
        left = MulLo(steer, outer) >> 12;
        right = MulLo(steer, inner) >> 12;
    } else {
        left = Neg(MulLo(Neg(steer), inner) >> 12);
        right = Neg(MulLo(Neg(steer), outer) >> 12);
    }
    body.wheels[0].steerAngle = int16_t(left);
    body.wheels[1].steerAngle = int16_t(right);
    body.wheels[2].steerAngle = 0;
    body.wheels[3].steerAngle = 0;
    for (uint32_t axle = 0; axle < 2; axle++) {
        const uint32_t toe = uint16_t(body.toe[axle]); // read unsigned by the original
        body.wheels[axle * 2].steerAngle = int16_t(uint16_t(body.wheels[axle * 2].steerAngle) - toe);
        body.wheels[axle * 2 + 1].steerAngle = int16_t(uint16_t(body.wheels[axle * 2 + 1].steerAngle) + toe);
    }
    if (ControlClass(context, body) == 2) { // corner damage bends the wheels
        body.wheels[0].steerAngle = int16_t(uint16_t(body.wheels[0].steerAngle) - body.wheels[0].damage);
        body.wheels[1].steerAngle = int16_t(uint16_t(body.wheels[1].steerAngle) + body.wheels[1].damage);
        body.wheels[2].steerAngle = int16_t(uint16_t(body.wheels[2].steerAngle) + body.wheels[2].damage);
        body.wheels[3].steerAngle = int16_t(uint16_t(body.wheels[3].steerAngle) - body.wheels[3].damage);
    }
    for (uint32_t w = 0; w < 4; w++) {
        block.wheels[w].steerCos = int16_t(Cos(AngleIndex(body.wheels[w].steerAngle)));
        block.wheels[w].steerSin = int16_t(Sin(AngleIndex(body.wheels[w].steerAngle)));
    }
}

void SlideMeasures(CarBody& body, CarScratch& block) {
    block.slideMeasure = 0;
    block.slideAngle = 0;
    if (body.forwardSpeed <= 0) return;
    int32_t steer = body.steerAngle, yawRate = body.yawRate;
    if (MulLo(steer, yawRate) >= 0) { // steering into the turn: how far the yaw lags the steady-state yaw
        if (steer < 0) {
            steer = Neg(steer);
            yawRate = Neg(yawRate);
        }
        const int32_t lateralRate = Mul12Floor(body.forwardSpeed, Sin(AngleIndex(steer)));
        const int32_t wheelbase = MulLo(body.wheelbase, 0x6488) >> 12;
        const int32_t steadyYaw = int32_t(uint64_t(Div64(int64_t(lateralRate) << 12, wheelbase)));
        const int32_t reference = int32_t(uint32_t(steadyYaw) << 7);
        if (steadyYaw != 0 && yawRate < reference) {
            const int32_t ratio = int32_t(uint64_t(Div64(int64_t(yawRate) << 12, reference)));
            block.slideMeasure = int16_t(Sub(0x1000, ratio));
        }
    }
    int32_t angle = Atan2(Neg(body.lateralSpeed), body.forwardSpeed);
    if (angle == 0) return;
    bool counterSteering;
    if (angle > 0) {
        counterSteering = body.yawRate <= 0;
    } else {
        counterSteering = body.yawRate >= 0;
        angle = Neg(angle);
    }
    if (!counterSteering || body.forwardSpeed < 0) return;
    // (The original evaluates ApproxLength(forwardSpeed, lateralSpeed) here and discards the result.)
    int32_t measure = Mul12ShiftFloor(angle, body.forwardSpeed, 2);
    if (measure < 0) return;
    if (measure > 0x1000) measure = 0x1000;
    block.slideAngle = int16_t(measure);
}

void DriverAids(const PhysicsContext& context, CarBody& body, const uint8_t* scratch) {
    const int32_t control = int8_t(body.controlClass); // read as a signed byte by the original
    const bool ai = control == 2 || (control == 0 && PlayerHandedToAi(body));
    if (ai) {
        const AiAidTuning& aid = context.aiAids[body.driveClass];
        const int32_t falloff = Falloff(body.yawAccel, aid.steerFalloffGain, aid.steerFalloffThreshold);
        ApplyTractionControl(body, aid.tractionGain, falloff, 0);
        ApplyBrakeAssist(body, scratch, aid.yawBrakeGain, aid.yawBrakeThreshold, context.slideSensitivity);
    } else if (control == 0) {
        const int32_t gain = body.tcsFalloffGain;
        if (gain != 0) {
            const int32_t falloff = Falloff(body.yawAccel, gain, 0x400);
            const int32_t steerFactor = body.gear != 0 ? MulLo(body.steerBlend, body.tcsSteerGain) >> 12 : 0;
            ApplyTractionControl(body, body.tcsGain, falloff, steerFactor);
        }
        ApplyBrakeAssist(body, scratch, body.asmYawGain, body.asmYawThreshold, context.slideSensitivity);
    }
}

// ---- 0x80040F30 / 0x800412D4 helpers ----
// -0x2000 < v <= 0x2000 (or -0x10000 < v <= 0xFFFF), written as the original's sign test on the two differences.
bool WithinAsymmetric(int32_t v, uint32_t below, uint32_t above) { return int32_t((0u - uint32_t(v) - below) ^ (above - uint32_t(v))) < 1; }
int32_t HalfWidth(int16_t width) { const int32_t w = width; return (w - (w >> 31)) >> 1; }
// v * 15 / 16 (or / 32) towards zero, as the original computes the shrunken extents.
int32_t Shrink(int32_t v, int shift) {
    int32_t scaled = Sub(int32_t(uint32_t(v) << 4), v);
    if (scaled < 0) scaled = Add(scaled, (1 << shift) - 1);
    return scaled >> shift;
}
} // namespace

int32_t ControlClass(const PhysicsContext& context, const CarBody& body) {
    if (body.controlClass != 0 || body.raceState != 0) return 0;
    return context.move.controlClass;
}

void ClearNeighbourFields(CarBody& body) { // 0x8003360C
    body.draftInput = 0;
    body.neighbourClass = 0;
    for (uint32_t i = 0; i < 8; i++) body.neighbourFlags[i] = 0;
}

void SingleGearClutch(CarBody& body) { // 0x80039470
    body.clutchState = uint8_t(body.clutchRequest != 0 ? 1 : 0);
}

void SingleGearRpm(CarBody& body) { // 0x80039994
    body.engineRpm = int16_t(Mul12Floor(body.engineSpeed, 60));
}

void SingleGearSelect(CarBody& body, const GearRequest& request) { // 0x800399C4
    const uint32_t gear = body.gear;
    int32_t target = -1;
    if (request.reverse != 0) {
        if (gear != 0) target = 0;
    } else if (body.forwardSpeed >= 1139 || body.effectiveThrottle != 0) {
        if (gear == 0) target = 1;
    } else if (body.forwardSpeed < -1138) {
        if (gear != 0) target = 0;
    }
    if (target == -1) return;
    body.gear = uint8_t(target);
    if (body.forwardGears == 2) body.clutchState = 2;
}

int32_t ApproxLength3(int32_t a, int32_t b, int32_t c) { // 0x8003C398
    int32_t x = Abs(a), y = Abs(b), z = Abs(c);
    int32_t largest = x;
    if (x < y) {
        largest = y;
        y = x;
    }
    int32_t result = largest;
    if (largest < z) {
        result = z;
        z = largest;
    }
    return Add(result, Add(y, z) >> 2);
}

void ClassifyNeighbour(CarBody& body, int32_t along, int32_t across, int32_t frontSum, int32_t rearSum, int32_t widthSum) { // 0x80033634
    int8_t* flags = body.neighbourFlags;                                          // [0] ahead, [1] ahead-left, [2] ahead-right,
    const int32_t halfLeft = Div(Neg(widthSum), 2), halfRight = Div(widthSum, 2); // [3] left, [4] right, [5] behind-left,
    if (along > frontSum) {                                                       // [6] behind-right, [7] behind
        if (across < halfLeft) flags[1] = 2;
        else if (across > halfRight) flags[2] = 2;
        else {
            flags[0] = 2;
            flags[across > 0 ? 2 : 1] = 1;
        }
        return;
    }
    if (along < Neg(rearSum)) {
        if (across < halfLeft) flags[5] = 2;
        else if (across > halfRight) flags[6] = 2;
        else {
            flags[7] = 2;
            flags[across > 0 ? 6 : 5] = 1;
        }
        return;
    }
    if (across >= 0) {
        flags[4] = 2;
        flags[along > 0 ? 2 : 6] = 1;
    } else {
        flags[3] = 2;
        flags[along > 0 ? 1 : 5] = 1;
    }
}

void UpdateCarPairAwareness(CarBody& a, CarBody& b) { // 0x8003373C
    if (a.contactType != 0 || b.contactType != 0 || a.aiLine == 4 || b.aiLine == 4) return;
    if ((a.flags78D & 0x10) != (b.flags78D & 0x10)) return;
    const int32_t distance = ApproxLength3(Sub(a.position[0], b.position[0]), Sub(a.position[1], b.position[1]), Sub(a.position[2], b.position[2]));
    if (distance > 0x64000) return; // 100 m
    int32_t offset[3];
    for (uint32_t k = 0; k < 3; k++) offset[k] = Sub(b.position[k], a.position[k]);
    const int32_t heightA = a.height, heightB = b.height;
    if (offset[2] < Neg(heightB + 0x5000) || offset[2] > heightA + 0x5000) return;
    // Position of each car in the other's frame (along its forward basis, across its lateral basis).
    int32_t alongA = 0, acrossA = 0, alongB = 0, acrossB = 0;
    for (uint32_t k = 0; k < 3; k++) {
        alongA = Add(alongA, Mul12(a.basis[kRowForward][k], offset[k]));
        acrossA = Add(acrossA, Mul12(a.basis[kRowLateral][k], offset[k]));
        alongB = Sub(alongB, Mul12(b.basis[kRowForward][k], offset[k]));
        acrossB = Sub(acrossB, Mul12(b.basis[kRowLateral][k], offset[k]));
    }
    const int32_t frontA = a.frontExtent, rearA = a.rearExtent, widthA = a.width;
    const int32_t frontB = b.frontExtent, rearB = b.rearExtent, widthB = b.width;
    if (distance <= 0x14000) { // 20 m: neighbour sectors and the follow class
        ClassifyNeighbour(a, alongA, acrossA, Add(frontA, rearB), Add(rearA, frontB), Add(widthA, widthB));
        ClassifyNeighbour(b, alongB, acrossB, Add(rearA, frontB), Add(frontA, rearB), Add(widthA, widthB));
        int32_t relativeVelocity[3];
        for (uint32_t k = 0; k < 3; k++) relativeVelocity[k] = Sub(b.velocity[k], a.velocity[k]);
        const int32_t widthSum = Add(widthA, widthB);
        // The car behind classifies the gap to the car ahead by the closing speed (only for AI-driven cars).
        auto classify = [&](CarBody& follower, int32_t gap, int32_t across, int32_t closing) {
            if (gap <= 0 || !(Neg(widthSum) < across && across < widthSum)) return;
            if (closing < -0xA008) follower.neighbourClass = 0x1000;
            else if (closing < -0x472 && Div(int32_t(uint32_t(gap) << 12), Neg(closing)) < 0x3000) follower.neighbourClass = 0x800;
            if (follower.neighbourClass == 0 && gap < 0x2000) follower.neighbourClass = 0x400;
        };
        if (int8_t(a.controlClass) != 0 || a.raceState != 0) {
            int32_t closing = 0;
            for (uint32_t k = 0; k < 3; k++) closing = Add(closing, Mul12Wide(a.basis[kRowForward][k], relativeVelocity[k]));
            classify(a, Sub(Sub(alongA, rearB), frontA), acrossA, closing);
        }
        if (int8_t(b.controlClass) != 0 || b.raceState != 0) {
            int32_t closing = 0;
            for (uint32_t k = 0; k < 3; k++) closing = Sub(closing, Mul12Wide(b.basis[kRowForward][k], relativeVelocity[k]));
            classify(b, Sub(Sub(alongB, rearA), frontB), acrossB, closing);
        }
    }
    // Slipstream: a car directly behind a fast one (> 25 m/s) within its own length asks for draft.
    auto draft = [](CarBody& follower, const CarBody& leader, int32_t along, int32_t across, int32_t width) {
        if (follower.draftInput == 0x1000) return;
        const int32_t speed = leader.forwardSpeed;
        if (speed <= 0x1BC88 || along >= 0 || Neg(speed) >= along || Neg(width) >= across || across >= width) return;
        const int32_t strength = Add(Div12Shift(Add(speed, along), speed), int32_t(uint16_t(follower.draftInput)));
        follower.draftInput = int16_t(strength);
        if (int16_t(strength) > 0x1000) follower.draftInput = 0x1000;
    };
    draft(a, b, alongB, acrossB, widthB);
    draft(b, a, alongA, acrossA, widthA);
}

void RefreshContactCorners(CarContactState& state, Car* cars, int count) { // 0x80040F30
    const int buffer = state.buffer;
    for (int i = 0; i < count; i++) {
        CarBody& self = cars[i].body;
        if ((self.contactFlags & 2) == 0) continue;
        UpdateFootprint(self);
        if (!TakesPartInContact(self)) continue;
        const int32_t s = Sin(AngleIndex(self.heading)), c = Cos(AngleIndex(self.heading));
        int slot = 0;
        for (int j = 0; j < count; j++) {
            if (j == i) continue;
            const CarBody& other = cars[j].body;
            if (TakesPartInContact(other) && WithinAsymmetric(Sub(other.position[2], self.position[2]), 0x2000u, 0x2000u)) {
                for (int k = 0; k < 4; k++) {
                    ContactCorner& record = state.corners[i][slot][k];
                    const int32_t relX = Sub(other.footprint[other.footprintSet][k][0], self.position[0]);
                    const int32_t relY = Sub(other.footprint[other.footprintSet][k][1], self.position[1]);
                    if (!WithinAsymmetric(relY, 0x10000u, 0xFFFFu) || !WithinAsymmetric(relX, 0x10000u, 0xFFFFu)) {
                        record.edgeFlags[buffer] = 0xF;
                        continue;
                    }
                    const int32_t across = Dot2Shift12(c, relX, s, relY, 0);
                    const int32_t along = Dot2Shift12(Neg(s), relX, c, relY, 0);
                    record.x[buffer] = across;
                    record.y[buffer] = along;
                    const int32_t half = HalfWidth(self.width);
                    uint8_t flags = uint8_t((self.frontExtent < along) << 3);
                    if (along < Neg(self.rearExtent)) flags |= 4;
                    if (across < Neg(half)) flags |= 2;
                    if (half < across) flags |= 1;
                    record.edgeFlags[buffer] = flags;
                }
            }
            slot++;
        }
    }
}

void ComputeContactPush(const CarContactState& state, Car* cars, int count) { // 0x800412D4
    for (int i = 0; i < count; i++) {
        cars[i].body.pushForce[1] = 0;
        cars[i].body.pushForce[0] = 0;
    }
    const int buffer = state.buffer;
    for (int i = 0; i < count; i++) {
        CarBody& self = cars[i].body;
        // The car's rectangle shrunk to 15/16 (front, rear) and 15/32 of the width (half width).
        const int32_t front = Shrink(self.frontExtent, 4), rear = Shrink(self.rearExtent, 4), half = Shrink(self.width, 5);
        if (!TakesPartInContact(self)) continue;
        int slot = 0;
        for (int j = 0; j < count; j++) {
            if (j == i) continue;
            CarBody& other = cars[j].body;
            if (TakesPartInContact(other)) {
                // Corners of the other car inside the shrunken rectangle.
                bool inside[4] = {false, false, false, false};
                int32_t cornerX[4] = {0, 0, 0, 0}, cornerY[4] = {0, 0, 0, 0};
                bool any = false;
                for (int k = 0; k < 4; k++) {
                    const ContactCorner& record = state.corners[i][slot][k];
                    if (record.edgeFlags[buffer] == 0xF) continue;
                    const int32_t x = record.x[buffer];
                    if (!(x < half && Neg(half) < x)) continue;
                    const int32_t y = record.y[buffer];
                    if (!(y < front && Neg(rear) < y)) continue;
                    any = true;
                    cornerX[k] = x;
                    cornerY[k] = y;
                    inside[k] = true;
                }
                if (any) {
                    const int32_t dx = Sub(other.position[0], self.position[0]), dy = Sub(other.position[1], self.position[1]);
                    const int32_t c = Cos(AngleIndex(self.heading)), s = Sin(AngleIndex(self.heading));
                    const int32_t across = Dot2Shift12(c, dx, s, dy, 0);
                    const int32_t along = Dot2Shift12(Neg(s), dx, c, dy, 0);
                    // Which side the other car sits on: the deeper relative penetration wins.
                    const int32_t lateralDepth = Div(int32_t(uint32_t(Abs(across)) << 12), half);
                    const int32_t longitudinalDepth = Div(int32_t(uint32_t(Abs(along)) << 12), along < 0 ? rear : front);
                    int32_t best = 0, side = -1;
                    if (longitudinalDepth < lateralDepth) {
                        for (int k = 0; k < 4; k++) {
                            if (!inside[k]) continue;
                            const int32_t depth = Div12Shift(Add(half, across > 0 ? Neg(cornerX[k]) : cornerX[k]), int32_t(uint32_t(half) << 1));
                            if (best < depth) {
                                best = depth;
                                side = across > 0 ? 3 : 2;
                            }
                        }
                    } else {
                        for (int k = 0; k < 4; k++) {
                            if (!inside[k]) continue;
                            const int32_t depth = Div12Shift(along > 0 ? Sub(front, cornerY[k]) : Add(rear, cornerY[k]), Add(front, rear));
                            if (best < depth) {
                                best = depth;
                                side = along > 0 ? 0 : 1;
                            }
                        }
                    }
                    if (best != 0) {
                        int32_t normal[2] = {0, 0};
                        switch (side) {
                        case 0: normal[0] = int16_t(Neg(s)); normal[1] = int16_t(c); break;  // front
                        case 1: normal[0] = int16_t(s); normal[1] = int16_t(Neg(c)); break;  // rear
                        case 2: normal[0] = int16_t(Neg(c)); normal[1] = int16_t(Neg(s)); break; // left
                        case 3: normal[0] = int16_t(c); normal[1] = int16_t(s); break;       // right
                        default: break;
                        }
                        int32_t strength = Add(best, 0x800);
                        if (strength > 0x1000) strength = 0x1000;
                        strength = Mul12Floor(strength, 0x4C901);
                        for (uint32_t k = 0; k < 2; k++) {
                            const int32_t push = Mul12Wide(normal[k], strength);
                            other.pushForce[k] = Add(other.pushForce[k], push);
                            self.pushForce[k] = Add(self.pushForce[k], Neg(push));
                        }
                    }
                }
            }
            slot++;
        }
    }
}

void TyreForces(const PhysicsContext& context, Car* cars, int count, DriveStepWork& work, const GearRequest* requests) { // 0x80039FC8
    for (int car = 0; car < count; car++) {
        work.stepTime = BodyOf(cars, car).stepTime;
        ClutchAndBrakes(BodyOf(cars, car), BlockOf(work, car));
    }
    for (int car = 0; car < count; car++) RollingResistance(context, BodyOf(cars, car), TyreBlockOf(work, car));
    for (int car = 0; car < count; car++) EvaluateSlipCurves(BodyOf(cars, car), TyreBlockOf(work, car), context.curves[car].axles); // 0x800397D0
    for (int car = 0; car < count; car++) LongitudinalForces(BodyOf(cars, car), TyreBlockOf(work, car));
    for (int car = 0; car < count; car++) LateralForces(BodyOf(cars, car), context.curves[car].axles);
    for (int car = 0; car < count; car++) IntegrateLongitudinal(BodyOf(cars, car), work, car);
    {
        CarBody* bodies[kMaxCars];
        for (int car = 0; car < count; car++) bodies[car] = &BodyOf(cars, car);
        auto drivetrain = context.drivetrain;
        for (int car = 0; car < count; ++car) drivetrain.wheelClutch[car] = WheelClutch(requests[car]);
        UpdateDrivetrain(bodies, size_t(count), work, drivetrain); // 0x80046B58
    }
    for (int car = 0; car < count; car++) {
        CarBody& body = BodyOf(cars, car);
        work.stepTime = body.stepTime;
        const uint32_t gears = body.forwardGears;
        if (gears == 1) {
            SingleGearRpm(body);
            if (requests[car].reserved[1] & 0x80) UpdateGear(body, requests[car]);
            else SingleGearSelect(body, requests[car]);
            continue;
        }
        EngineStep(context, body, BlockOf(work, car), work.stepTime, WheelClutch(requests[car]) != 0);
        UpdateEngineRpm(body); // 0x8003941C
        if (gears == 2 && !(requests[car].reserved[1] & 0x80)) SingleGearSelect(body, requests[car]);
        else UpdateGear(body, requests[car]); // 0x8003991C
    }
    for (int car = 0; car < count; car++) IntegrateLateral(BodyOf(cars, car), work, car);
    for (int car = 0; car < count; car++) IntegrateVelocity(BodyOf(cars, car), work);
    if (context.wear.wearLimit != 0)
        for (int car = 0; car < count; car++) AccumulateWear(BodyOf(cars, car), work);
    if (context.gameMode != 3 && context.gameMode != 6) // 0x80041AB8
        for (int car = 0; car < count; car++) UpdatePitFlags(context, BodyOf(cars, car));
}

void PhysicsCore(const PhysicsContext& context, Car* cars, int count, const PadRecord* pads, DriveStepWork& work, GearRequest* requests,
                 int32_t (*deltas)[4]) { // 0x8003E0C4
    const bool held = context.holdFrames != 0;
    for (int car = 0; car < count; car++) {
        CarBody& body = BodyOf(cars, car);
        body.scriptedControl = uint8_t(held ? 1 : 0);
        if (context.gameMode == 6 && body.contactType == 2) body.scriptedControl = uint8_t(body.scriptedControl | context.scriptedHold);
        if (body.impactTimer != 0) body.impactTimer = int16_t(body.impactTimer - 1);
        if (body.penaltyFrames != 0) body.penaltyFrames = uint16_t(body.penaltyFrames - 1);
        const uint32_t timer = body.messageFrames;
        if (timer != 0 && timer != 0xFF) body.messageFrames = uint8_t(timer - 1);
        SetStepTime(body, context.move.globals); // 0x8004232C
    }
    for (int car = 0; car < count; car++) {
        work.stepTime = BodyOf(cars, car).stepTime;
        UpdateAero(BodyOf(cars, car), context.move.globals); // 0x8003DAA8
    }
    for (int car = 0; car < count; car++) {
        CarBody& body = BodyOf(cars, car);
        const CurveS32& torque = context.curves[car].engineTorque;
        body.engineTorque = Interpolate(torque.xs, torque.ys, torque.count, body.engineSpeed);
        body.engineBrakeTorque = Mul12Floor(body.engineBrakeCoefficient, body.engineSpeed);
    }
    for (int car = 0; car < count; car++) UpdateTyreWearAndGrip(BodyOf(cars, car), context.curves[car].axles, context.wear); // 0x80039A4C
    for (int car = 0; car < count; car++) UpdateSlipAngles(BodyOf(cars, car));                                              // 0x80039DE8
    for (int car = 0; car < count; car++) {
        CarBody& body = BodyOf(cars, car);
        work.stepTime = body.stepTime;
        body.inputFlag6FD = 1;
        const int32_t control = int8_t(body.controlClass); // read as a signed byte by the original
        if (control == 0) {
            uint16_t padWords[4];
            std::memcpy(padWords, &pads[car], sizeof(padWords));
            MapPadInput(body, padWords); // 0x8003E020
            if (PlayerHandedToAi(body)) {
                RaiseAiInput(context, body, car, requests[car]);
            } else {
                UpdatePlayerInput(body, pads[car], requests[car], work.stepTime, context.input[car], context.curves[car].steerLimit); // 0x8002FB18
                ApplyWheelInput(body, pads[car], requests[car]);
                body.effectiveThrottle = body.throttle;
                for (uint32_t w = 0; w < 4; w++) body.wheels[w].brakeInput = body.brake;
            }
        } else if (control == 2) {
            RaiseAiInput(context, body, car, requests[car]);
        }
    }
    for (int car = 0; car < count; car++) SteerWheels(context, BodyOf(cars, car), TyreBlockOf(work, car));
    {
        CarBody* bodies[kMaxCars];
        for (int car = 0; car < count; car++) bodies[car] = &BodyOf(cars, car);
        UpdateSlipRatios(bodies, uint32_t(count), &TyreBlockOf(work, 0)); // 0x80039778
    }
    for (int car = 0; car < count; car++) SlideMeasures(BodyOf(cars, car), TyreBlockOf(work, car));
    for (int car = 0; car < count; car++) DriverAids(context, BodyOf(cars, car), reinterpret_cast<const uint8_t*>(&work));
    TyreForces(context, cars, count, work, requests);
    for (int car = 0; car < count; car++) { // 0x80030330 (it mirrors the car's step time into the work area's word 0 as well)
        work.stepTime = BodyOf(cars, car).stepTime;
        ComputeDisplacement(BodyOf(cars, car), deltas[car]);
    }
}

void SimulateCars(const PhysicsContext& context, Car* cars, int count, int32_t (*deltas)[4]) { // 0x80034480
    for (int car = 0; car < count; car++) {
        BodyOf(cars, car).contactFlags = 0;
        ClearNeighbourFields(BodyOf(cars, car));
    }
    for (int car = 0; car < count; car++) { // 0x80034320 queries the control class per body
        MoveContext move = context.move;
        move.controlClass = ControlClass(context, BodyOf(cars, car));
        MovePass(move, &cars[car], 1, &deltas[car]);
    }
    UpdateCarProximity(*context.contact, cars, count);              // 0x800400CC
    SweepCarPairs(*context.contact, cars, count);                   // 0x800407A0
    ResolveCarContacts(context.move, *context.contact, cars, count); // 0x80040924
    RefreshContactCorners(*context.contact, cars, count);           // 0x80040F30
    ComputeContactPush(*context.contact, cars, count);              // 0x800412D4
    for (int i = 0; i < count; i++)
        for (int j = i + 1; j < count; j++) UpdateCarPairAwareness(BodyOf(cars, i), BodyOf(cars, j)); // 0x8003373C
    const int32_t rate = context.move.globals.rate;
    for (int car = 0; car < count; car++) {
        CarBody& body = BodyOf(cars, car);
        if (body.wallImpact == 0) continue;
        if (body.wallImpact > 0x1000) body.wallImpact = 0x1000;
        // The step driver uses body + 0x6FA (resetState) as "the impact sound of this contact was raised".
        if (ControlClass(context, body) == 1 && body.resetState == 0 && body.wallImpact > 0x2AA) {
            if (context.soundEvent) context.soundEvent(context.user, int(body.carIndex), 1); // 0x800156B8
            body.resetState = 1;
        }
        if (body.impactTimer < Div(MulLo(rate, 3), 5)) body.impactTimer = int16_t(rate);
    }
}

} // namespace gt2::sim
