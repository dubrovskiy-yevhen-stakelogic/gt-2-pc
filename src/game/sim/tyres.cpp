#include "game/sim/tyres.h"

#include <bit>

#include "game/sim/fixed.h"
#include "game/sim/trig.h"

namespace gt2::sim {

namespace {

constexpr int32_t kStandstillSpeed = 70;       // |speed| <= 70 (0.017 m/s) counts as standing still
constexpr int32_t kSlipFadeSpeed = 22760;      // below 5.56 m/s the slip ratio is faded in linearly
constexpr int32_t kSlipFadeGain = 0x2E1;       // 737 / 4096 per unit of speed: 1.0 at kSlipFadeSpeed

// 32-bit wrapping subtraction / negation / magnitude (MIPS subu / negu, no overflow trap).
int32_t Sub(int32_t a, int32_t b) { return int32_t(uint32_t(a) - uint32_t(b)); }
int32_t Neg(int32_t v) { return int32_t(0u - uint32_t(v)); }
int32_t Abs(int32_t v) { return v < 0 ? Neg(v) : v; }
// |v| <= limit, tested like the original's unsigned range check (v + limit < 2 * limit + 1).
bool Within(int32_t v, int32_t limit) { return uint32_t(v) + uint32_t(limit) < uint32_t(2 * limit + 1); }

// Raw slip ratio: slipScale * (roadSpeed - rimSpeed) / reference, truncated to 16 bits like the original's
// `sh` of the 64-bit division's low word (0x80039490).
int16_t RawSlipRatio(int32_t slipScale, int32_t roadSpeed, int32_t rimSpeed, int32_t reference) {
    const int32_t scaled = Mul12Wide(slipScale, Sub(roadSpeed, rimSpeed));
    return int16_t(uint64_t(Div64(int64_t(scaled) << 12, reference)));
}

} // namespace

int32_t SquareRoot(int32_t value, uint32_t extraBits) {
    if (value == 0) return 0;
    // Number of bit pairs to process: 16 for a negative (unsigned) value, otherwise just enough to cover the
    // leading one (the original counts leading zeros with the GTE's LZCS/LZCR).
    uint32_t pairs = 16;
    if (value >= 0) {
        const uint32_t leadingZeros = uint32_t(std::countl_zero(uint32_t(value)));
        pairs = (((31u - leadingZeros) & ~1u) + 2u) >> 1;
    }
    uint32_t bits = uint32_t(value) << ((32u - 2u * pairs) & 31u); // MIPS sllv uses the low 5 bits
    uint32_t root = 0, rootTwice = 0, remainder = 0;
    for (uint32_t n = pairs; n; n--) {
        root <<= 1;
        rootTwice <<= 1;
        remainder = ((remainder << 2) | (bits >> 30)) - (rootTwice + 1u);
        bits <<= 2;
        if (int32_t(remainder) < 0) {
            remainder += rootTwice + 1u;
        } else {
            root++;
            rootTwice += 2;
        }
    }
    for (; extraBits; extraBits--) {
        rootTwice <<= 1;
        remainder = (remainder << 2) - (rootTwice + 1u);
        root <<= 1;
        if (int32_t(remainder) < 0) {
            remainder += rootTwice + 1u;
        } else {
            root++;
            rootTwice += 2;
        }
    }
    return int32_t(root);
}

namespace {

// Index k of the first sample with xs[k] > x, for x strictly inside the range (shared by 0x8003D848 and
// 0x8003D940: linear scan from 1, indices kept to 16 bits like the original).
uint32_t SegmentAfter(const CurveS16& curve, int16_t x) {
    const uint32_t last = (curve.count - 1u) & 0xFFFFu;
    uint32_t k = 1;
    if (1 < last) {
        do {
            if (x < curve.xs[k & 0xFFFFu]) break;
            k++;
        } while ((k & 0xFFFFu) < last);
    }
    return k & 0xFFFFu;
}

// (x - x0) * (y1 - y0) / (x1 - x0) with the original's 32-bit product and MIPS division.
int32_t SegmentStep(int32_t dx, int32_t dy, int32_t width) { return Div(int32_t(uint32_t(dx) * uint32_t(dy)), width); }

} // namespace

int32_t InterpolateS16(const CurveS16& curve, int16_t x) {
    const uint32_t last = (curve.count - 1u) & 0xFFFFu;
    if (!(curve.xs[0] < x)) return curve.ys[0];
    if (!(x < curve.xs[last])) return curve.ys[last];
    const uint32_t k = SegmentAfter(curve, x);
    const int32_t x0 = curve.xs[k - 1], y0 = curve.ys[k - 1];
    return y0 + SegmentStep(x - x0, curve.ys[k] - y0, curve.xs[k] - x0);
}

void InterpolateS16Pair(const CurveS16& curve, const int16_t* secondYs, int16_t x, int16_t& y, int16_t& secondY) {
    const uint32_t last = (curve.count - 1u) & 0xFFFFu;
    if (!(curve.xs[0] < x)) {
        y = curve.ys[0];
        secondY = secondYs[0];
        return;
    }
    if (!(x < curve.xs[last])) {
        y = curve.ys[last];
        secondY = secondYs[last];
        return;
    }
    const uint32_t k = SegmentAfter(curve, x);
    const int32_t x0 = curve.xs[k - 1], dx = x - x0, width = curve.xs[k] - x0;
    y = int16_t(curve.ys[k - 1] + SegmentStep(dx, curve.ys[k] - curve.ys[k - 1], width));
    secondY = int16_t(secondYs[k - 1] + SegmentStep(dx, secondYs[k] - secondYs[k - 1], width));
}

int32_t SymmetricCurve(const CurveS16& curve, int32_t x) {
    int32_t folded, sign;
    if (x < -0x400) {
        folded = Sub(x, -0x800);
        sign = -1;
    } else if (x < 0) {
        folded = Neg(x);
        sign = -1;
    } else {
        folded = x > 0x3FF ? Sub(0x800, x) : x;
        sign = 1;
    }
    const int32_t y = int16_t(InterpolateS16(curve, int16_t(folded))); // the original keeps 16 bits of both
    return sign * y;
}

void UpdateWheelSlipRatios(CarBody& body, CarScratch& scratch) {
    for (uint32_t i = 0; i < 4; i++) {
        Wheel& wheel = body.wheels[i];
        WheelScratch& record = scratch.wheels[i];
        // Velocity of the contact patch along the steered wheel's rolling direction.
        const int32_t roadSpeed = Mul12Wide(record.steerCos, wheel.contactForwardSpeed) - Mul12Wide(record.steerSin, wheel.contactLateralSpeed);
        const int32_t rimSpeed = wheel.rimSpeed;
        const int32_t slipScale = wheel.slipScale;
        const bool rimStill = Within(rimSpeed, kStandstillSpeed);
        const bool roadStill = Within(roadSpeed, kStandstillSpeed);
        int16_t ratio;
        int8_t sign, mode;
        if (rimStill) {
            // A stopped wheel on a moving road: full slip in the direction of the road.
            if (rimSpeed < roadSpeed) {
                ratio = 0x1000; sign = -1;
            } else if (roadSpeed < rimSpeed) {
                ratio = 0x1000; sign = 1;
            } else {
                ratio = 0; sign = 0;
            }
            mode = -1;
        } else if (rimSpeed <= 0) {
            if (roadSpeed < rimSpeed) { // the road runs backwards faster than the rim: it drives the wheel
                ratio = roadStill ? int16_t(0) : RawSlipRatio(slipScale, roadSpeed, rimSpeed, roadSpeed);
                sign = 1; mode = -1;
            } else {
                ratio = RawSlipRatio(slipScale, roadSpeed, rimSpeed, rimSpeed);
                sign = -1; mode = 1;
            }
        } else {
            if (rimSpeed < roadSpeed) { // the road is faster than the rim: braking, the road drives the wheel
                ratio = roadStill ? int16_t(0) : RawSlipRatio(slipScale, roadSpeed, rimSpeed, roadSpeed);
                sign = -1; mode = -1;
            } else { // the rim is faster: traction, the wheel drives the road
                ratio = RawSlipRatio(slipScale, roadSpeed, rimSpeed, rimSpeed);
                sign = 1; mode = 1;
            }
        }
        if (ratio > 0x1000) ratio = 0x1000;
        else if (ratio < -0x1000) ratio = -0x1000;
        if (mode == 0) {
            ratio = 0;
        } else {
            // Fade the ratio in with the speed of whichever side drives (rim in traction, road in braking).
            const int32_t reference = Abs(mode > 0 ? rimSpeed : roadSpeed);
            if (reference < kSlipFadeSpeed) ratio = int16_t(Mul12((reference * kSlipFadeGain) >> 12, ratio));
        }
        record.slipMode = mode;
        record.slipSign = sign;
        record.slipRatio = ratio;
        wheel.slipRatio = ratio;
    }
}

void UpdateSlipRatios(CarBody* const bodies[], uint32_t count, CarScratch scratch[]) {
    for (uint32_t car = 0; car < count; car++) UpdateWheelSlipRatios(*bodies[car], scratch[car]);
}

void EvaluateSlipCurves(CarBody& body, CarScratch& scratch, const AxleTyreCurves curves[2]) {
    for (uint32_t i = 0; i < 4; i++) {
        Wheel& wheel = body.wheels[i];
        WheelScratch& record = scratch.wheels[i];
        const AxleTyreCurves& axle = curves[i >> 1];
        int16_t force, grip;
        InterpolateS16Pair(axle.slipRatioForce, axle.slipRatioGrip, record.slipRatio, force, grip);
        record.slipForce = force;
        wheel.slipRatioGrip = grip;
        wheel.slipRatioAbs = int16_t(Abs(record.slipRatio));
    }
}

void UpdateSlipAngles(CarBody& body) {
    constexpr int32_t kFullSpeed = 0x2C74;   // 2.78 m/s: full weight of the slip angle from here on
    constexpr int32_t kZeroSpeed = 0x472;    // 0.28 m/s: no weight below
    constexpr int32_t kBlendGain = 0x666;    // 0.4 per unit of speed: (kFullSpeed - kZeroSpeed) * 0.4 = 1.0
    for (uint32_t i = 0; i < 4; i++) {
        Wheel& wheel = body.wheels[i];
        const int32_t forward = wheel.contactForwardSpeed;
        const int32_t lateral = wheel.contactLateralSpeed;
        int32_t blend = 0x1000;
        if (Within(forward, kFullSpeed) && lateral < kFullSpeed + 1 && lateral > -(kFullSpeed + 1)) {
            // Both components are small: weight by the speed of the contact patch.
            const int32_t speed = SquareRoot((lateral * lateral >> 12) + (forward * forward >> 12), 6);
            if (speed <= kFullSpeed - 1) blend = speed > kZeroSpeed ? ((speed - kZeroSpeed) * kBlendGain) >> 12 : 0;
        }
        wheel.slipBlend = int16_t(blend);
        wheel.slipAngle = int16_t(blend == 0 ? 0 : Atan2(-lateral, forward));
    }
}

void UpdateTyreWearAndGrip(CarBody& body, const AxleTyreCurves curves[2], const TyreWearConstants& k) {
    const bool wearActive = k.wearLimit != 0 && (body.flags78D & 0x10) == 0; // bit 4: off the road, wear suspended
    if (wearActive) {
        for (uint32_t i = 0; i < 4; i++) {
            Wheel& wheel = body.wheels[i];
            const int32_t wear = wheel.wear;
            int16_t gripFactor;
            int8_t stage;
            if (wear >= k.wearLimit) { // fully worn: pin the wear and apply the full loss
                wheel.wear = k.wearLimit;
                gripFactor = int16_t(Sub(0x1000, k.wornGripLoss));
                stage = 0x7F;
            } else if (wear >= k.wearKnee) { // second segment: knee .. limit
                const int32_t progress = int32_t(uint64_t(Div64(int64_t(Sub(wear, k.wearKnee)) << 12, Sub(k.wearLimit, k.wearKnee))));
                gripFactor = int16_t(Sub(Sub(0x1000, Mul12Floor(progress, Sub(k.wornGripLoss, k.kneeGripLoss))), k.kneeGripLoss));
                stage = progress > 0xFFF ? int8_t(0x7F) : int8_t((progress >> 6) + 0x40);
            } else if (wear >= 0) { // first segment: 0 .. knee
                const int32_t progress = int32_t(uint64_t(Div64(int64_t(wear) << 12, k.wearKnee)));
                gripFactor = int16_t(Sub(0x1000, Mul12Floor(k.kneeGripLoss, progress)));
                stage = progress > 0xFFF ? int8_t(0x3F) : int8_t(progress >> 6);
            } else if (wear >= k.coldLimit) { // negative wear: cold tyre, coldLimit .. 0
                const int32_t progress = int32_t(uint64_t(Div64(int64_t(Neg(wear)) << 12, Neg(k.coldLimit))));
                gripFactor = int16_t(Sub(0x1000, Mul12Floor(k.coldGripLoss, progress)));
                stage = progress > 0xFFF ? int8_t(-0x3F) : int8_t(-(progress >> 6));
            } else {
                gripFactor = 0x1000;
                stage = int8_t(-0x80);
            }
            wheel.wearGrip = gripFactor;
            wheel.wearStage = stage;
        }
    }
    // Camber from the body roll (minus the visual roll): the left wheels lean one way, the right wheels the other.
    const int32_t roll = body.roll - body.visualRoll;
    const int32_t rollCamber[4] = {-roll, roll, -roll, roll};
    for (uint32_t i = 0; i < 4; i++) {
        Wheel& wheel = body.wheels[i];
        const uint32_t axle = i >> 1;
        int32_t load = wheel.load;
        if (wearActive) load = int32_t(uint32_t(wheel.wearGrip) * uint32_t(load)) >> 12;
        const int32_t loadGrip = Interpolate(curves[axle].loadGrip.xs, curves[axle].loadGrip.ys, curves[axle].loadGrip.count, load);
        const int32_t camber = Abs(rollCamber[i] + body.camber[axle]);
        const int32_t camberGrip = int16_t(InterpolateS16(curves[axle].camberGrip, int16_t(camber)));
        const int32_t surfaceGrip = SurfaceGripAt(body, wheel.surface);
        const int32_t grip = int32_t(uint32_t(camberGrip) * uint32_t(loadGrip)) >> 12;
        wheel.gripForce = int32_t(uint32_t(surfaceGrip) * uint32_t(grip)) >> 8;
    }
}

} // namespace gt2::sim
