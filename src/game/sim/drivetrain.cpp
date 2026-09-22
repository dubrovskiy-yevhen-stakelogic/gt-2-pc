#include "game/sim/drivetrain.h"

#include <cstring>
#include <algorithm>

#include "game/sim/fixed.h"

namespace gt2::sim {

namespace {

// Low word of the 32 x 32 product, as MIPS `mult` + `mflo` (wraps on overflow like the original).
inline int32_t MulLo(int32_t a, int32_t b) { return int32_t(uint32_t(a) * uint32_t(b)); }
// 32-bit sums, negations and shifts that wrap like the original's `addu` / `subu` / `negu` / `sll` instead of
// being undefined.
inline int32_t Add(int32_t a, int32_t b) { return int32_t(uint32_t(a) + uint32_t(b)); }
inline int32_t Sub(int32_t a, int32_t b) { return int32_t(uint32_t(a) - uint32_t(b)); }
inline int32_t Neg(int32_t a) { return int32_t(0u - uint32_t(a)); }
inline int32_t Shl(int32_t a, int shift) { return int32_t(uint32_t(a) << shift); }
inline int32_t Half(int32_t a) { return Add(a, int32_t(uint32_t(a) >> 31)) >> 1; } // a / 2 towards zero
// Low word of the 64-bit quotient (value << shift) / divisor, as the original's calls of 0x80086084 use it.
inline int32_t DivShifted(int32_t value, int shift, int32_t divisor) {
    return int32_t(uint64_t(Div64(int64_t(value) << shift, int64_t(divisor))));
}

// Road speed (1/4096 m/s) at which the engine reaches `rpm` + 500 in the gear with `revsPerSpeed`:
// (rpm + 500) / 60 << 12 << 12 / revsPerSpeed. Shared by the automatic upshift check and the reverse-to-forward
// selection (0x800449C8 / 0x800448C8).
int32_t GearLimitSpeed(uint16_t rpm, int32_t revsPerSpeed) {
    const int32_t revsPerSecond12 = int32_t((uint32_t(rpm) + 500u) << 12) / 60;
    const int64_t quotient = Div64(int64_t(revsPerSecond12) << 12, int64_t(revsPerSpeed));
    return int32_t(uint64_t(quotient)); // the original compares only the low word
}

// Lowest forward gear whose limit speed at the upshift rpm exceeds `speed` (original: 0x800448C8, used when
// leaving reverse under throttle in the manual gearbox): 0 when rolling backwards, 1 for cars with fewer than
// three forward gears, the gear count when no gear is low enough.
int32_t GearForSpeed(const CarBody& body, int32_t speed) {
    if (speed < 0) return 0;
    const int32_t gears = body.forwardGears;
    if (gears < 3) return 1;
    for (int32_t gear = 1; gear < gears; gear++)
        if (speed < GearLimitSpeed(body.upshiftRpm, body.revsPerSpeed[gear])) return gear;
    return gears;
}

// Which axles the automatic gearbox and the traction control watch for the drive type (0x800449C8 switch).
void DrivenAxles(uint8_t driveType, bool& front, bool& rear) {
    switch (driveType) {
    case 0: case 4: case 5: front = false; rear = true; break;
    case 1: case 2: front = true; rear = false; break;
    case 3: case 6: front = true; rear = true; break;
    default: front = false; rear = false; break;
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
int32_t TurboBoostSum(const CarBody& body, int16_t spool0, int16_t spool1) { // 0x80074F24
    const int32_t spools[2] = {spool0, spool1};
    int32_t total = 0;
    for (uint32_t i = 0; i < 2; i++) {
        const int32_t boost = body.turboBoost[i];
        if (boost == 0) continue;
        const int32_t spool = spools[i], spoolRpm = body.turboSpoolRpm[i], coefficient = body.turboCoefficient[i];
        int32_t contribution = 0;
        if (spool < spoolRpm) {
            switch (body.turboModel[i]) {
            case 0: { // boost minus a parabola in the distance to the spool speed
                const int32_t d = Sub(spool, spoolRpm);
                contribution = Sub(boost, MulLo(coefficient, MulLo(d, d) >> 12) >> 12);
                break;
            }
            case 1: // parabola in the spool speed
                contribution = MulLo(coefficient, MulLo(spool, spool) >> 12) >> 12;
                break;
            case 2: // linear
                contribution = Div(MulLo(boost, spool), spoolRpm);
                break;
            default:
                break;
            }
        } else if (spool < MulLo(3, spoolRpm)) { // above the spool speed the boost falls off with a quarter of the coefficient
            int32_t quarter = coefficient;
            if (quarter < 0) quarter = Add(quarter, 3);
            quarter >>= 2;
            const int32_t d = Sub(spool, spoolRpm);
            contribution = Sub(boost, MulLo(quarter, MulLo(d, d) >> 12) >> 12);
        }
        total = Add(total, contribution);
    }
    return Add(total, 0x1000);
}

int32_t BoostMultiplier(const CarBody& body, int16_t spool0, int16_t spool1) { // 0x80075074
    const int32_t multiplier = TurboBoostSum(body, spool0, spool1);
    const int32_t cap = Add(body.boostCap, 0x1000);
    return cap < multiplier ? cap : multiplier;
}

int32_t BaseEngineTorque(CarBody& body, int32_t throttle) { // 0x8003533C
    const int32_t rpm = body.engineRpm, limit = body.revLimitRpm;
    if (body.revLimiterActive == 0) {
        if (limit <= rpm) body.revLimiterActive = 1;
    } else if (rpm < Sub(limit, 500)) {
        body.revLimiterActive = 0;
    }
    int32_t torque = 0;
    if (body.revLimiterActive == 0) torque = Mul12Floor(throttle, Add(body.engineTorque, body.engineBrakeTorque));
    return Sub(torque, body.engineBrakeTorque);
}

int32_t EngineTorqueStep(CarBody& body, int32_t throttle, int32_t stepTime) { // 0x800353DC
    const int32_t boostCap = body.boostCap;
    int32_t demand = throttle; // throttle passed on to BaseEngineTorque
    if (boostCap == 0) {
        body.engineLoad = int16_t(throttle);
        body.exhaustFlame = 0;
    } else {
        const int32_t rpm = body.engineRpm;
        const int32_t fullBoost = BoostMultiplier(body, int16_t(rpm), int16_t(rpm)); // boost reachable at this engine speed
        // Spool target: last load times engine speed, relative to the reachable boost.
        const int32_t target = DivShifted(MulLo(body.engineLoad, rpm) >> 12, 12, fullBoost);
        for (uint32_t i = 0; i < 2; i++) {
            if (body.turboBoost[i] == 0) continue;
            int32_t delta = Mul12Floor(Sub(target, int16_t(body.turboSpool[i])), body.turboSpoolRate[i]);
            delta = Mul16Floor(delta, stepTime);
            const int32_t spool = Add(body.turboSpool[i], delta);
            body.turboSpool[i] = uint16_t(spool);
            if (int16_t(spool) < 0) body.turboSpool[i] = 0;
        }
        const int32_t boostNow = BoostMultiplier(body, int16_t(body.turboSpool[0]), int16_t(body.turboSpool[1]));
        const int32_t boostedThrottle = MulLo(throttle, boostNow) >> 12;
        // Intake value: boosted throttle plus an idle term that shrinks with the engine speed.
        int32_t quarterRpm = rpm;
        if (quarterRpm < 0) quarterRpm = Add(quarterRpm, 3);
        int32_t idle = Sub(0x800, quarterRpm >> 2);
        if (idle < 0x100) idle = 0x100;
        const int32_t intake = Add(boostedThrottle, MulLo(Sub(0x1000, throttle), idle) >> 12);
        body.intakeLoad = int16_t(Half(Add(body.intakeLoad, intake)));
        // Blow-off valve state machine: 0 = armed, > 0 = releasing, -1 = released until the throttle is applied again.
        const int32_t blowOff = body.blowOffState;
        if (blowOff != 0) {
            if (blowOff != -1) body.blowOffState = -1;
            else if (throttle >= 0xF01) body.blowOffState = 0;
        } else if (throttle < 0xE00) {
            const int32_t armed = Half(Sub(body.engineLoad, 0x1800));
            body.blowOffState = int16_t(armed);
            const int32_t armed16 = int16_t(armed);
            if (armed16 >= 0x1001) body.blowOffState = 0x1000;
            else if (armed16 <= 0) body.blowOffState = 1;
        }
        body.engineLoad = int16_t(boostedThrottle);
        demand = boostedThrottle < fullBoost ? Div(Shl(boostedThrottle, 12), fullBoost) : 0x1000;
        // Gauge value: the larger spool state of the two turbos.
        body.turboSpoolMax = int16_t(body.turboSpool[0]);
        if (int32_t(body.turboSpool[0]) < int16_t(body.turboSpool[1])) body.turboSpoolMax = int16_t(body.turboSpool[1]);
    }
    const int32_t torque = BaseEngineTorque(body, demand);
    if (boostCap != 0) {
        // Exhaust flame: grows with the load above 0.75 and the torque up to 2352.
        const int32_t excess = Sub(body.engineLoad, 0xC00);
        if (excess <= 0 || torque <= 0) {
            body.exhaustFlame = 0;
        } else {
            int32_t flame = excess;
            if (torque < 2352) flame = MulLo(excess, MulLo(torque, 0x1BDD) >> 12) >> 12;
            flame = MulLo(flame, flame) >> 17;
            if (flame >= 0x100) flame = 0xFF;
            body.exhaustFlame = uint8_t(flame);
        }
    }
    return torque;
}

// ---------------------------------------------------------------------------------------------------------------
int32_t EngineSpeedFromWheels(const CarBody& body) { // 0x8003932C
    // Axle angular speed (rad/s << 12) the engine is geared to.
    int32_t axleSpeed = 0;
    const int32_t front = body.axleSpeed[0], rear = body.axleSpeed[1];
    switch (body.driveType) {
    case 0: case 4: case 5: axleSpeed = rear; break;
    case 1: case 2: axleSpeed = front; break;
    case 3: axleSpeed = Add(front, rear) / 2; break;
    case 6: axleSpeed = Add(rear, Mul12Wide(body.centreSplit, Sub(front, rear))); break;
    default: break;
    }
    const uint32_t gear = body.gear;
    if (gear == 0) axleSpeed = Neg(axleSpeed); // reverse gear
    if (axleSpeed <= 0) return 0;
    // rad/s through the gear ratio, then / (2 pi) = 0x28C / 0x1000 to rev/s.
    return Mul12Floor(0x28C, Mul12Floor(body.gearRatio[gear], axleSpeed));
}

void UpdateEngineRpm(CarBody& body) { // 0x8003941C
    const int32_t floor = int32_t(body.idleRpm) - 1;
    int32_t rpm = Mul12Floor(body.engineSpeed, 60);
    if (rpm < floor) rpm = floor;
    body.engineRpm = int16_t(rpm);
}

int32_t SelectGear(CarBody& body, const GearRequest& request) { // 0x800449C8
    const int32_t speed = body.forwardSpeed;
    const int32_t gear = body.gear, gears = body.forwardGears;
    // Penalty / forced state: first gear, or reverse... the original returns 1 unless already in gear 1.
    if (body.penaltyFrames != 0) return gear == 1 ? -1 : 1;

    const bool wheelInput = (request.reserved[1] & 0x80) != 0;
    const bool ignoreShiftSpeed = wheelInput && (request.reserved[1] & 0x40) != 0;
    const int wheelGear = request.reserved[0] & 15;
    if (wheelInput && body.raceState == 0) {
        if (wheelGear >= 1 && wheelGear <= 9) {
            const int target = wheelGear - 2;
            if (target < 0 || target > gears || target == gear) return -1;
            if (!ignoreShiftSpeed && ((target == 0 && speed > 2048) || (target > 0 && speed < -2048))) return -1;
            return target;
        }
        if (wheelGear == 15) {
            if (body.shiftTimer) { body.shiftTimer = request.shift ? 1 : 0; return -1; }
            const int target = gear + (request.shift > 0 ? 1 : request.shift < 0 ? -1 : 0);
            if (target < 0 || target > gears || target == gear) return -1;
            if (!ignoreShiftSpeed && ((target == 0 && speed > 2048) || (gear == 0 && speed < -2048))) return -1;
            body.shiftTimer = 1;
            return target;
        }
    }

    const uint8_t mode = body.raceState == 0 && !wheelInput ? body.transmissionMode : 0;
    if (mode != 0) {
        if (mode != 1) return -1;
        // ---- manual gearbox (0x80044D98)
        if (request.reverse != 0) return gear != 0 ? 0 : -1;
        if (gear == 0 && body.throttle != 0) {
            const int32_t forward = GearForSpeed(body, speed);
            return forward != 0 ? forward : 1;
        }
        const int32_t timer = body.shiftTimer;
        if (timer != 0) {
            body.shiftTimer = uint8_t(timer - 1);
            if (timer != 1) return -1;
            if (request.shift != 0) body.shiftTimer = 1; // key still held: wait another frame
            return -1;
        }
        if (request.shift > 0) {
            if (gear < gears) {
                body.shiftTimer = 1;
                return gear + 1;
            }
            return -1;
        }
        if (request.shift < 0 && gear >= 2) {
            body.shiftTimer = 1;
            return gear - 1;
        }
        return -1;
    }

    // ---- automatic gearbox
    const int32_t timer = body.shiftTimer;
    if (timer != 0) {
        body.shiftTimer = uint8_t(timer - 1);
        return -1;
    }
    if (request.reverse != 0) return gear != 0 ? 0 : -1;
    if (speed >= 0x473 && gear == 0) return 1; // rolling forwards in reverse
    const int32_t throttle = body.effectiveThrottle;
    bool considerOverrev;
    if (throttle == 0) {
        if (speed < -0x472 && !(wheelInput && wheelGear == 10)) { // wheel AT requires an explicit reverse request
            if (gear != 0) return 0;
            considerOverrev = false;
        } else {
            considerOverrev = true;
        }
    } else {
        if (speed < 0x472) return gear != 1 ? 1 : -1; // pulling away: first gear
        considerOverrev = true;
    }
    if (considerOverrev && gear > 0 && gear < gears) {
        // Upshift before the rev limiter: the road speed at limiter + 500 rpm in this gear.
        if (GearLimitSpeed(body.revLimitRpm, body.revsPerSpeed[gear]) < speed) return gear + 1;
    }
    if (body.clutchState != 1 || gear == 0) return -1;

    // Shift thresholds in rpm: down below `downshiftRpm`, up above `upshiftRpm`.
    int32_t downshiftRpm, upshiftRpm;
    if (gear == 1) {
        downshiftRpm = -20000;
        upshiftRpm = body.upshiftRpm;
    } else {
        upshiftRpm = gear < gears ? int32_t(body.upshiftRpm) : 20000;
        downshiftRpm = body.downshiftRpm[gear - 2];
        if (throttle > 0xC00) { // heavy throttle: shift down 500 rpm later, but not below the floor
            downshiftRpm -= 500;
            const int32_t floor = int32_t((uint32_t(body.downshiftFloorRpm) + 500u) & 0xFFFFu);
            if (downshiftRpm < floor) downshiftRpm = floor;
        }
    }
    bool frontDriven, rearDriven;
    DrivenAxles(body.driveType, frontDriven, rearDriven);
    const int32_t rpm = body.engineRpm;
    if (rpm < downshiftRpm) {
        // Downshift unless a driven wheel is airborne or locking beyond the axle's anti-lock threshold.
        for (uint32_t wheel = 0; wheel < 4; wheel++) {
            if (!frontDriven && wheel < 2) continue;
            if (!rearDriven && wheel >= 2) return gear - 1;
            const Wheel& w = body.wheels[wheel];
            if (w.load == 0) return -1;
            if (body.tyres[wheel >> 1].peakSlipRatioPos < w.slipRatio) return -1;
        }
        return gear - 1;
    }
    if (upshiftRpm < rpm) {
        // Upshift unless a driven wheel is airborne or spinning beyond the axle's traction threshold scaled by
        // its slip scale.
        for (uint32_t wheel = 0; wheel < 4; wheel++) {
            if (!frontDriven && wheel < 2) continue;
            if (!rearDriven && wheel >= 2) return gear + 1;
            const Wheel& w = body.wheels[wheel];
            if (w.load == 0) return -1;
            const int32_t threshold = int16_t(-(MulLo(w.slipScale, -body.tyres[wheel >> 1].peakSlipRatioNeg) >> 12));
            if (w.slipRatio < threshold) return -1;
        }
        return gear + 1;
    }
    return -1;
}

void UpdateGear(CarBody& body, const GearRequest& request) { // 0x8003991C
    const int32_t gear = SelectGear(body, request);
    if (gear == -1) return;
    if (body.clutchState != 0) body.clutchState = uint8_t(gear != 0 && body.gear != 0 ? 3 : 2);
    body.gear = uint8_t(gear);
}

void ApplyTractionControl(CarBody& body, int32_t gain, int32_t gripFalloff, int32_t steerFactor) { // 0x8003DE68
    if (body.clutchState != 1 || body.engineTorque <= 0) return;
    const int32_t throttle = body.effectiveThrottle;
    if (throttle == 0) return;
    // Lowest slip ratio (most wheel spin) among the driven wheels on the ground, with its axle's traction threshold.
    int32_t minSlip = 0x1000, threshold = 0x1000;
    const uint8_t driveType = body.driveType;
    for (uint32_t axle = 0; axle < 2; axle++) {
        const bool watched = driveType == 1 ? axle == 0 : (driveType == 0 || driveType == 5) ? axle == 1 : true;
        if (!watched) continue;
        for (uint32_t side = 0; side < 2; side++) {
            const Wheel& w = body.wheels[axle * 2 + side];
            const int32_t slip = w.slipRatio;
            if (w.load != 0 && slip < minSlip) {
                minSlip = slip;
                threshold = body.tyres[axle].peakSlipRatioNeg;
            }
        }
    }
    const int32_t slipBelowThreshold = Sub(minSlip, MulLo(threshold, gripFalloff) >> 12);
    int32_t factor = Add(Mul12(slipBelowThreshold, MulLo(gain, Sub(0x1000, steerFactor)) >> 12), 0x1000);
    if (factor < 0) factor = 0;
    else if (factor > 0x1000) factor = 0x1000;
    body.effectiveThrottle = int16_t(MulLo(factor, throttle) >> 12);
}

void ApplyBrakeAssist(CarBody& body, const uint8_t* scratch, int32_t yawBrakeGain, int32_t yawBrakeThreshold, uint8_t slideSensitivity) { // 0x8003DBE8
    // The car's slide measures, written by the tick (0x8003E0C4) into its scratchpad work block.
    const uint8_t* work = scratch + kScratchCarBlocks + uint32_t(body.carIndex) * sizeof(CarScratch);
    int16_t slideMeasure, slideAngle;
    std::memcpy(&slideMeasure, work + offsetof(CarScratch, slideMeasure), sizeof slideMeasure);
    std::memcpy(&slideAngle, work + offsetof(CarScratch, slideAngle), sizeof slideAngle);
    // Per-wheel anti-lock thresholds from the axle blocks, reduced on the inside of a slide.
    int32_t thresholds[4];
    thresholds[0] = thresholds[1] = body.tyres[0].peakSlipRatioPos;
    thresholds[2] = thresholds[3] = body.tyres[1].peakSlipRatioPos;
    if (slideSensitivity != 0 && slideMeasure != 0) {
        int32_t factor = 0x1000 - MulLo(slideMeasure, slideSensitivity) / 100;
        if (factor < 0) factor = 0;
        const uint32_t side = body.yawRate > 0 ? 1 : 0;
        thresholds[side] = MulLo(factor, thresholds[side]) >> 12;
        thresholds[side + 2] = MulLo(factor, thresholds[side + 2]) >> 12;
        for (int32_t& t : thresholds)
            if (t > 0x1000) t = 0x1000;
    }
    // Stability pass: brake the front wheel on the side of the yaw while the slide angle exceeds the threshold.
    if (yawBrakeGain != 0) {
        const int32_t excess = Sub(slideAngle, yawBrakeThreshold);
        if (excess > 0) {
            Wheel& w = body.wheels[body.yawRate > 0 ? 1 : 0];
            int32_t brake = Add(w.brakeInput, MulLo(yawBrakeGain, excess) >> 9);
            if (brake > 0x1000) brake = 0x1000;
            w.brakeInput = int16_t(brake);
        }
    }
    // Anti-lock pass: scale each braking wheel by its grip force and how far its slip ratio is below the threshold.
    for (uint32_t wheel = 0; wheel < 4; wheel++) {
        Wheel& w = body.wheels[wheel];
        const int32_t brake = w.brakeInput;
        if (brake == 0) continue;
        const uint32_t axle = wheel >> 1;
        const int32_t slipGain = MulLo(w.slipScale, body.absSlipGain[axle]) >> 12;
        const int32_t loadTerm = MulLo(w.gripForce, body.brakeLoadFactor[axle]) >> 12;
        const int32_t slipTerm = MulLo(w.slipRatio - thresholds[wheel], slipGain) >> 12;
        int32_t factor = MulLo(loadTerm, 0x1000 - slipTerm) >> 12;
        if (factor < 0) factor = 0;
        else if (factor > 0x1000) factor = 0x1000;
        int32_t result = MulLo(factor, brake) >> 12;
        if (result == 0) result = 1;
        w.brakeInput = int16_t(result);
    }
}

void UpdateClutchRequest(CarBody& body) { // 0x8002F92C
    // A rear-primary car (primaryAxle 1) opens the clutch with the handbrake fully pulled.
    const bool driving = body.scriptedControl == 0 && body.penaltyFrames == 0 && body.raceState != 7 &&
                         (body.primaryAxle != 1 || body.handbrake != 0x1000);
    if (!driving) {
        body.clutchRequest = 0;
        return;
    }
    if (body.clutchState == 0) {
        // Clutch open: engage unless the brake is fully pressed.
        body.clutchRequest = int16_t(body.brake == 0x1000 ? 0 : 0x1000);
        return;
    }
    // Clutch engaged: keep it unless the engine would stall below idle.
    body.clutchRequest = int16_t(body.engineRpm < int32_t(body.idleRpm) ? 0 : 0x1000);
}

namespace {

// Clutch request of a single-gear car (original: 0x8002F9CC): open while braking near standstill.
void UpdateClutchRequestSingleGear(CarBody& body) {
    if (body.scriptedControl != 0 || body.penaltyFrames != 0 || body.raceState == 7) {
        body.clutchRequest = 0;
        return;
    }
    if (body.clutchState == 0) {
        body.clutchRequest = int16_t(body.brake != 0 ? 0 : 0x1000);
        return;
    }
    body.clutchRequest = 0x1000;
    if (body.brake == 0) return;
    const int32_t speed = body.forwardSpeed;
    if (speed < 0x472 && speed >= -0x471) body.clutchRequest = 0;
}

// Creep of a single-gear car (original: 0x8002FA60): adds throttle below 13656 (3.3 m/s) in the direction of
// the gear (an eighth of the shortfall fraction) and asks for reverse when rolling backwards without throttle.
void ApplyCreepSingleGear(CarBody& body, const PadRecord& pad, GearRequest& request) {
    if (body.clutchRequest == 0) return;
    int32_t speed = body.forwardSpeed;
    if (body.gear == 0) speed = Neg(speed);
    if (speed >= -0x472) {
        const int32_t shortfall = 13656 - speed;
        if (shortfall > 0) {
            // The original divides by the magic constant 0x13323D7D >> 13, i.e. by 13656 * 8 = 109248.
            int32_t throttle = body.throttle + (shortfall << 12) / 109248;
            if (throttle > 0x1000) throttle = 0x1000;
            body.throttle = int16_t(throttle);
        }
    }
    if (int16_t(pad.throttle) == 0 && pad.reverse == 0 && body.forwardSpeed < -0x472) request.reverse = 1;
}

} // namespace

void UpdatePlayerInput(CarBody& body, const PadRecord& pad, GearRequest& request, int32_t stepTime, const InputTuning& tuning,
                       const CurveS16Ref& steerLimitCurve) { // 0x8002FB18
    // ---- steering limits from the speed and, above 8.4 m/s, the front slip angles
    const int32_t frontSlip = (body.wheels[0].slipAngle + body.wheels[1].slipAngle) / 2; // mean slip angle
    int32_t speedIndex = MulLo(body.forwardSpeed < 0 ? Neg(body.forwardSpeed) : body.forwardSpeed, 0x1CD) >> 12;
    if (speedIndex > 0x7FFF) speedIndex = 0x7FFF;
    const int32_t limit = int16_t(InterpolateS16(steerLimitCurve, int16_t(speedIndex))); // max angle at this speed
    const int32_t maxAngle = body.steerRate;                                               // counter-steer cap
    const int32_t innerLimit = limit < body.peakSlipAngle ? limit : body.peakSlipAngle;    // window kept around the slip angle
    bool symmetric = true;
    if (body.forwardSpeed > 0x855B) {
        const int32_t slip = int16_t(frontSlip);
        symmetric = slip >= -innerLimit && slip <= innerLimit;
    }
    if (symmetric) {
        body.steerLimitPos = int16_t(limit);
        body.steerLimitNeg = int16_t(-limit);
        body.steerBlend = 0;
    } else if (int16_t(frontSlip) > 0) {
        // Sliding to the left: allow counter-steering up to the cap, limit the other side as usual.
        body.steerLimitPos = int16_t(frontSlip);
        if (maxAngle < int16_t(frontSlip)) body.steerLimitPos = int16_t(maxAngle);
        body.steerLimitNeg = int16_t(-limit);
        body.steerBlend = body.steerLimitPos;
    } else if (int16_t(frontSlip) < 0) {
        body.steerLimitPos = int16_t(limit);
        body.steerLimitNeg = int16_t(frontSlip);
        if (int16_t(frontSlip) < -maxAngle) body.steerLimitNeg = int16_t(-maxAngle);
        body.steerBlend = body.steerLimitNeg;
    }

    // ---- steering angle
    if ((pad.flags & 1) == 0) {
        // Digital: a damped spring towards the limit of the pressed side (or the centre), integrated by `stepTime`.
        const int32_t target = pad.steer > 0 ? body.steerLimitPos : pad.steer < 0 ? body.steerLimitNeg : 0;
        int32_t acceleration = Sub(Mul12(target - body.steerAngle, tuning.steerSpringGain), Mul12(body.steerAngleRate, tuning.steerDamping));
        if (target == 0) acceleration = Sub(acceleration, Mul12(body.steerAngle, tuning.steerCentring));
        const int32_t rawRate = Add(int32_t(uint16_t(body.steerAngleRate)), Mul16(acceleration, stepTime));
        body.steerAngleRate = int16_t(rawRate);
        const int32_t maxRate = body.steerMaxRate;
        int32_t rate = int16_t(rawRate);
        if (rate > maxRate) rate = maxRate;
        else if (rate < -maxRate) rate = -maxRate;
        const int32_t current = body.steerAngle;
        const int32_t next = Add(current, Mul16(rate, stepTime));
        // Snap to the target when the step reaches or passes it (and kill the rate).
        const bool reached = (target > 0 && target < next) || (target < 0 && next < target) || (current < target && next >= target) ||
                             (target < current && next <= target);
        if (reached) {
            body.steerAngle = int16_t(target);
            body.steerAngleRate = 0;
        } else {
            body.steerAngle = int16_t(next);
        }
    } else {
        // Analogue: stick -> fraction of the limit (squared, or through the tuning curve), then slew towards it.
        int32_t fraction;
        if (pad.flags & 8) {
            const int32_t magnitude = pad.steer < 0 ? -pad.steer : pad.steer;
            fraction = int16_t(InterpolateS16(tuning.steerCurve, int16_t(magnitude)));
            if (pad.steer < 0) fraction = -fraction;
        } else {
            fraction = MulLo(pad.steer, pad.steer) >> 12;
            if (pad.steer < 0) fraction = -fraction;
        }
        int32_t target;
        if (fraction < 0) target = int16_t(-(MulLo(body.steerLimitNeg, fraction) >> 12));
        else target = int16_t(uint32_t(MulLo(body.steerLimitPos, fraction)) >> 12);
        const int32_t slew = MulLo(stepTime, 0x5C7) >> 16; // angle units per step
        int32_t angle = body.steerAngle;
        if (angle < target) {
            angle += slew;
            if (target < angle) angle = target;
        } else if (angle > target) {
            angle -= slew;
            if (angle < target) angle = target;
        }
        body.steerAngle = int16_t(angle);
    }
    // Blend of the steering angle against the counter-steer limit (0..0x1000), 0 when no limit applies.
    if (body.steerBlend != 0) {
        const int32_t blend = int16_t(Div(int32_t(body.steerAngle) << 12, body.steerBlend));
        body.steerBlend = int16_t(blend);
        if (blend < 0) body.steerBlend = 0;
        else if (blend > 0x1000) body.steerBlend = 0x1000;
    }

    // ---- throttle and the reverse request
    if (pad.flags & 2) {
        int32_t throttle;
        if (pad.reverse != 0) {
            request.reverse = 1;
            throttle = 0x1000;
        } else {
            request.reverse = 0;
            throttle = pad.throttle;
        }
        body.throttle = int16_t(throttle);
        if (int16_t(throttle) != 0) body.throttle = int16_t((int16_t(throttle) * 7 >> 3) + 0x200); // 0x200..0x1000
    } else {
        int32_t delta = -int32_t(tuning.throttleFall);
        if (pad.reverse != 0 && body.penaltyFrames == 0) {
            request.reverse = 1;
            delta = tuning.throttleRise;
        } else {
            request.reverse = 0;
            if (int16_t(pad.throttle) != 0) delta = tuning.throttleRise;
        }
        int32_t throttle = int32_t(uint16_t(body.throttle)) + delta;
        if (int16_t(throttle) > 0x1000) throttle = 0x1000;
        else if (int16_t(throttle) < 0) throttle = 0;
        body.throttle = int16_t(throttle);
    }
    // ---- brake
    if (pad.flags & 4) {
        body.brake = int16_t(pad.brake);
        if (int16_t(pad.brake) != 0) body.brake = int16_t((int16_t(pad.brake) * 7 >> 3) + 0x200);
    } else if (pad.brake != 0) {
        const int32_t brake = int32_t(uint16_t(body.brake)) + tuning.brakeRise;
        body.brake = int16_t(brake);
        if (int16_t(brake) > 0x1000) body.brake = 0x1000;
    } else {
        const int32_t brake = int32_t(uint16_t(body.brake)) - tuning.brakeFall;
        body.brake = int16_t(brake);
        if (int16_t(brake) < 0) body.brake = 0;
    }
    // ---- gear request and the handbrake
    request.shift = body.penaltyFrames == 0 ? pad.shift : 0;
    if (pad.handbrake != 0) {
        const int32_t handbrake = int32_t(uint16_t(body.handbrake)) + tuning.handbrakeRise;
        body.handbrake = int16_t(handbrake);
        if (int16_t(handbrake) > 0x1000) body.handbrake = 0x1000;
    } else {
        const int32_t handbrake = int32_t(uint16_t(body.handbrake)) - tuning.handbrakeFall;
        body.handbrake = int16_t(handbrake);
        if (int16_t(handbrake) < 0) body.handbrake = 0;
    }
    // ---- clutch request; single- and two-gear cars creep
    const int32_t gears = body.forwardGears;
    if (gears == 1) {
        UpdateClutchRequestSingleGear(body);
        ApplyCreepSingleGear(body, pad, request);
        return;
    }
    UpdateClutchRequest(body);
    if (gears == 2) {
        int32_t speed = body.forwardSpeed;
        if (body.gear == 0) speed = Neg(speed);
        if (speed >= -0x472) {
            const int32_t shortfall = 0x2C74 - speed; // below 2.8 m/s in the direction of the gear
            if (shortfall > 0) {
                int32_t throttle = body.throttle + (shortfall << 12) / 0x58E8;
                if (throttle > 0x1000) throttle = 0x1000;
                body.throttle = int16_t(throttle);
            }
            if (int16_t(pad.throttle) == 0 && pad.reverse == 0 && body.gear == 0) request.reverse = 1;
        }
    }
}

bool WheelDirectionBlocked(const CarBody& body, const GearRequest& request) {
    if (!(request.reserved[1] & 0x80) || body.raceState != 0) return false;
    if (request.reserved[1] & 0x40) return false;
    const int gear = request.reserved[0] & 15;
    const int target = gear == 15 ? int(body.gear) + (request.shift > 0 ? 1 : request.shift < 0 ? -1 : 0) : gear - 2;
    if (gear == 15 && (!request.shift || target == body.gear)) return false;
    if ((gear != 15 && (gear < 2 || gear > 9)) || target < 0 || target > body.forwardGears) return false;
    return (target == 0 && body.forwardSpeed > 2048) || ((gear == 15 ? body.gear == 0 : target > 0) && body.forwardSpeed < -2048);
}

bool WheelNeutral(const CarBody& body, const GearRequest& request) {
    if (!(request.reserved[1] & 0x80) || body.raceState != 0) return false;
    const int gear = request.reserved[0] & 15, target = gear - 2;
    return gear == 1 || (gear >= 2 && gear <= 9 && (target > body.forwardGears ||
        (!(request.reserved[1] & 0x40) && ((target == 0 && body.forwardSpeed > 2048) || (target > 0 && body.forwardSpeed < -2048)))));
}

void ApplyWheelInput(CarBody& body, const PadRecord& pad, GearRequest& request) {
    request.reserved[0] = request.reserved[1] = 0;
    if (!(pad.flags & kWheelPad)) return;
    const int gear = pad.reserved & 15, clutch = (pad.reserved & 0xf0) | ((pad.flags >> 4) & 15);
    request.reserved[0] = uint8_t(gear | ((clutch & 15) << 4));
    request.reserved[1] = uint8_t(0x80 | ((pad.flags & kWheelIgnoreShiftSpeed) ? 0x40 : 0) | (clutch >> 4));
    request.shift = body.penaltyFrames == 0 ? pad.shift : 0;
    request.reverse = 0;
    body.steerAngle = int16_t(std::clamp(int(pad.steer), -4096, 4096) * int(body.steerLock) / 4096);
    body.steerAngleRate = 0;
    body.throttle = int16_t(std::min<uint16_t>(pad.throttle, 4096));
    body.brake = int16_t(std::min<uint16_t>(pad.brake, 4096));
    // Neutral and a fully depressed clutch disconnect the engine, including
    // one/two-gear cars whose original input routine otherwise adds creep.
    if (WheelNeutral(body, request) || clutch == 255) { body.clutchRequest = 0; body.clutchState = 0; }
    else if (clutch > 0 && body.clutchState != 0) body.clutchState = 2;
}

AiDispatch PrepareAiInput(CarBody& body, GearRequest& request, int32_t raceFrame, int32_t rate) { // 0x80038540
    request.reserved[0] = request.reserved[1] = 0;
    body.handbrake = 0;
    request.reverse = 0;
    request.shift = 0;
    if (body.forwardGears == 1) {
        UpdateClutchRequestSingleGear(body);
        if (body.clutchRequest != 0) return {AiControl::Drive, 0};
        if (body.raceState == 7) return {AiControl::Finished, 0};
        return {AiControl::None, 0};
    }
    if (body.scriptedControl != 0) return {AiControl::Scripted, raceFrame - rate};
    if (body.penaltyFrames != 0) return {AiControl::Scripted, int32_t(body.penaltyFrames)};
    if (body.raceState == 7) {
        body.clutchRequest = 0;
        return {AiControl::Finished, 0};
    }
    const int32_t rpm = body.engineRpm;
    if (body.clutchState == 0) {
        // Clutch open: engage once the engine has revved to three quarters of the upshift rpm.
        body.clutchRequest = 0;
        if (int32_t(body.upshiftRpm) * 3 / 4 < rpm) body.clutchRequest = 0x1000;
    } else {
        body.clutchRequest = 0x1000;
        if (rpm < int32_t(body.idleRpm)) body.clutchRequest = 0;
    }
    return {AiControl::Drive, 0};
}

} // namespace gt2::sim
