#pragma once
#include "platform/input/steering_feedback.h"
#include "game/sim/car_body.h"
#include "game/sim/car_setup.h"

namespace gt2game {
inline gt2::input::wheel::FeedbackFrame WheelFeedbackOf(const gt2::sim::CarBody& body) {
    using namespace gt2::input::wheel;
    FeedbackFrame frame;
    constexpr float radians = 6.28318530717958647692f / 4096.f;
    frame.roadWheelLockRadians = std::abs(float(body.steerLock)) * radians;
    gt2::sim::AxleTyreBlock tyre;
    static_assert(sizeof(tyre) == sizeof(body.tyres[0]));
    std::memcpy(&tyre, &body.tyres[0], sizeof(tyre));
    float slope = 0, peak = 0;
    const size_t count = std::min<size_t>(tyre.slipAngleCount, 8);
    for (size_t i = 0; i < count; ++i) {
        const float factor = std::max(0.f, tyre.slipAngleYs[i] / 4096.f);
        peak = std::max(peak, factor);
        if (slope == 0 && tyre.slipAngleXs[i] > 0 && factor > 0)
            slope = factor / (tyre.slipAngleXs[i] * radians);
    }
    for (size_t i = 0; i < frame.front.size(); ++i) {
        const auto& source = body.wheels[i]; auto& target = frame.front[i];
        target.grounded = source.load > 0 && !(source.contactFlags & 1);
        // Setup uses 4 mass units/kg and weight = mass*9.8: forces are 4 units/N.
        target.loadN = source.load * .25f;
        target.nominalLoadN = (body.sprungWeightPerAxle[0] * .5f + body.suspension[0].staticLoad) * .25f;
        target.lateralForceN = source.lateralForce * .25f;
        const float gripN = std::max(0.f, source.gripForce * .25f);
        target.lateralCapacityN = gripN * std::min(peak, std::max(0.f, source.slipRatioGrip / 4096.f));
        target.corneringStiffness = gripN * slope;
        const float steer = source.steerAngle * radians;
        const float forward = source.contactForwardSpeed / 4096.f, lateral = source.contactLateralSpeed / 4096.f;
        target.rollingSpeed = std::cos(steer) * forward - std::sin(steer) * lateral;
        target.slipRadians = std::remainder(steer - std::atan2(-lateral, forward), 3.14159265358979323846f);
    }
    return frame;
}
}
