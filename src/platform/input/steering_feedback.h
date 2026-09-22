#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace gt2::input::wheel {
// These are explicit model assumptions, not recovered vehicle specifications.
struct SteeringGeometry {
    int wheelDegrees = 900;
    int mechanicalTrailMm = 30;
    int patchHalfLengthMm = 80;
    int referenceTorqueNm = 40;
};
struct ContactFeedback {
    bool grounded = false;
    float loadN = 0, nominalLoadN = 0;
    float lateralForceN = 0, lateralCapacityN = 0;
    float corneringStiffness = 0; // N/rad, native tyre curve's initial slope
    float slipRadians = 0, rollingSpeed = 0; // signed, in the wheel's plane
};
struct FeedbackFrame {
    std::array<ContactFeedback, 2> front{};
    float roadWheelLockRadians = 0;
};
struct SteeringMoments {
    std::array<float, 2> mechanicalNm{}, pneumaticNm{}, trailM{};
    float handwheelNm = 0, steeringRatio = 0;
    bool valid = true;
};

// Uniform-pressure brush: integrate bristle shear from the leading to trailing
// edge, capped by the available lateral friction. With an adhesive fraction u,
// trail = a*(u/2-u*u/3)/(1-u/2). It is a/3 without sliding and tends to zero as
// the patch slides. Native GT2 Fy remains authoritative; this supplies its arm.
inline float PneumaticTrail(float halfLength, float stiffness, float capacity, float slip) {
    if (!(halfLength > 0 && stiffness > 0 && capacity > 0)) return 0;
    const float demand = stiffness * std::abs(std::tan(std::clamp(slip, -1.56f, 1.56f)));
    const float u = demand > 0 ? std::min(1.f, capacity / (2.f * demand)) : 1.f;
    return halfLength * (u * .5f - u * u / 3.f) / (1.f - u * .5f);
}

inline SteeringMoments CalculateSteeringMoments(const FeedbackFrame& frame, const SteeringGeometry& geometry) {
    SteeringMoments result;
    constexpr float pi = 3.14159265358979323846f;
    if (!std::isfinite(frame.roadWheelLockRadians) || frame.roadWheelLockRadians <= 0) { result.valid = false; return result; }
    const float wheelHalfTravel = std::clamp(geometry.wheelDegrees, 180, 2520) * pi / 360.f;
    result.steeringRatio = wheelHalfTravel / frame.roadWheelLockRadians;
    const float mechanicalTrail = std::clamp(geometry.mechanicalTrailMm, 0, 100) * .001f;
    const float referenceHalfLength = std::clamp(geometry.patchHalfLengthMm, 20, 150) * .001f;
    for (size_t i = 0; i < frame.front.size(); ++i) {
        const auto& w = frame.front[i];
        if (!w.grounded) continue;
        for (float value : {w.loadN, w.nominalLoadN, w.lateralForceN, w.lateralCapacityN,
                           w.corneringStiffness, w.slipRadians, w.rollingSpeed})
            if (!std::isfinite(value)) { result = {}; result.valid = false; return result; }
        if (w.loadN <= 0 || w.nominalLoadN <= 0 || w.lateralCapacityN <= 0) continue;
        // Constant vertical tyre stiffness gives contact length proportional to
        // sqrt(load). The reference patch size is separately configurable.
        const float halfLength = referenceHalfLength * std::sqrt(std::min(w.loadN / w.nominalLoadN, 4.f));
        const float trail = PneumaticTrail(halfLength, w.corneringStiffness, w.lateralCapacityN, w.slipRadians);
        // Reversing swaps the contact patch's leading/trailing edges; geometric
        // trail stays behind the steering axis. Never use speed magnitude here.
        const float travel = w.rollingSpeed > 0 ? 1.f : w.rollingSpeed < 0 ? -1.f : 0.f;
        const float fy = std::clamp(w.lateralForceN, -w.lateralCapacityN, w.lateralCapacityN);
        result.trailM[i] = trail * travel;
        result.mechanicalNm[i] = fy * mechanicalTrail;
        result.pneumaticNm[i] = fy * result.trailM[i];
        result.handwheelNm += (result.mechanicalNm[i] + result.pneumaticNm[i]) / result.steeringRatio;
    }
    if (!std::isfinite(result.handwheelNm)) { result = {}; result.valid = false; }
    return result;
}
}
