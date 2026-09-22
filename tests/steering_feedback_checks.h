#pragma once
#include "../tools/gt2game/wheel_feedback.h"
#include <fstream>
#include <cstdlib>

inline void CheckSteeringFeedback() {
    namespace w = gt2::input::wheel;
    w::Settings settings; settings.enabled = settings.feedback = true;
    w::FeedbackFrame frame; frame.roadWheelLockRadians = .7853981634f;
    for (auto& tyre : frame.front) tyre = {true, 3500, 3500, 500, 3500, 40000, .0125f, 20};
    const auto baseline = w::CalculateSteeringMoments(frame, settings.steeringGeometry);
    Check(baseline.valid && Near(baseline.steeringRatio, 10) && baseline.handwheelNm > 0, "loaded tyre force produces steering torque through steering ratio");
    Check(Near(baseline.mechanicalNm[0], 15) && Near(baseline.trailM[0], .08f / 3), "mechanical arm and small-slip brush moment have physical dimensions");

    // Independent quadrature of the uniform-pressure brush, not the closed form.
    for (float slip : {.005f, .04f, .1f, .3f, .8f}) {
        double force = 0, moment = 0;
        constexpr int samples = 20000;
        constexpr double a = .08, stiffness = 40000, capacity = 3500;
        for (int i = 0; i < samples; ++i) {
            const double x = 2 * a * (i + .5) / samples;
            const double shear = std::min(stiffness / (2 * a * a) * x * std::tan(double(slip)), capacity / (2 * a));
            force += shear; moment += shear * (x - a);
        }
        Check(std::abs(w::PneumaticTrail(float(a), float(stiffness), float(capacity), slip) - moment / force) < .00001,
              "brush trail agrees with integrated shear distribution");
    }
    auto changed = frame;
    for (auto& tyre : changed.front) tyre.lateralForceN = -tyre.lateralForceN;
    Check(Near(w::CalculateSteeringMoments(changed, settings.steeringGeometry).handwheelNm, -baseline.handwheelNm), "left/right steering torque is symmetric");
    changed = frame; changed.front[0].grounded = false;
    Check(Near(w::CalculateSteeringMoments(changed, settings.steeringGeometry).handwheelNm, baseline.handwheelNm / 2), "lifting one front wheel removes exactly its contribution");
    changed.front[1].loadN = 0;
    Check(w::CalculateSteeringMoments(changed, settings.steeringGeometry).handwheelNm == 0, "no tyre aligning torque with both front wheels unloaded");
    changed = frame;
    for (auto& tyre : changed.front) { tyre.loadN *= .25f; tyre.lateralForceN *= .25f; tyre.lateralCapacityN *= .25f; tyre.corneringStiffness *= .25f; }
    Check(w::CalculateSteeringMoments(changed, settings.steeringGeometry).handwheelNm < baseline.handwheelNm * .25f,
          "unloading reduces force and contact length instead of normalizing it away");
    auto pneumaticOnly = settings.steeringGeometry; pneumaticOnly.mechanicalTrailMm = 0;
    changed = frame; for (auto& tyre : changed.front) { tyre.slipRadians = .6f; tyre.lateralForceN = 3500; }
    auto atPeak = frame; for (auto& tyre : atPeak.front) { tyre.slipRadians = .05f; tyre.lateralForceN = 2000; }
    Check(w::CalculateSteeringMoments(changed, pneumaticOnly).handwheelNm < w::CalculateSteeringMoments(atPeak, pneumaticOnly).handwheelNm,
          "pneumatic aligning moment falls after tyre saturation despite high lateral force");
    changed = frame; for (auto& tyre : changed.front) tyre.lateralCapacityN = 0;
    Check(w::CalculateSteeringMoments(changed, settings.steeringGeometry).handwheelNm == 0, "no grip cannot produce aligning torque");
    changed = frame; for (auto& tyre : changed.front) { tyre.rollingSpeed = -20; tyre.lateralForceN *= -1; }
    const auto reverse = w::CalculateSteeringMoments(changed, settings.steeringGeometry);
    Check(Near(reverse.mechanicalNm[0], -baseline.mechanicalNm[0]) && Near(reverse.pneumaticNm[0], baseline.pneumaticNm[0]),
          "reverse swaps pneumatic leading edge but preserves fixed mechanical geometry");
    changed = frame; for (auto& tyre : changed.front) { tyre.lateralForceN = 0; tyre.rollingSpeed = 0; tyre.slipRadians = .5f; }
    Check(w::CalculateSteeringMoments(changed, settings.steeringGeometry).handwheelNm == 0, "rotated stationary wheel is not pulled to an invented centre");
    auto twiceTravel = settings.steeringGeometry; twiceTravel.wheelDegrees *= 2;
    Check(Near(w::CalculateSteeringMoments(frame, twiceTravel).handwheelNm, baseline.handwheelNm / 2), "steering ratio scales moment consistently with physical wheel travel");
    changed = frame; changed.front[0].lateralForceN = std::numeric_limits<float>::quiet_NaN();
    Check(!w::CalculateSteeringMoments(changed, settings.steeringGeometry).valid, "non-finite contact data invalidates output");
    w::SteeringMoments excessive; excessive.handwheelNm = 1000;
    Check(Near(w::Force(excessive, 0, settings), .25f), "FFB strength remains an absolute output cap");
    settings.invertForce = true;
    Check(w::Force(baseline, 0, settings) < 0 && w::Force({}, 3, settings) < 0, "inversion affects aligning moment while damping stays passive");
    settings.invertForce = false; settings.axes[w::Steering].right = -32768;
    Check(w::Force(baseline, 0, settings) < 0, "inverted physical axis calibration preserves torque direction");
    settings.feedback = false; Check(w::Force(baseline, 0, settings) == 0, "disabled FFB is zero");
    settings.steeringGeometry = {1080, 45, 65, 17};
    const auto roundTrip = w::Settings::Parse(settings.Serialize()).steeringGeometry;
    Check(roundTrip.wheelDegrees == 1080 && roundTrip.mechanicalTrailMm == 45 && roundTrip.patchHalfLengthMm == 65 && roundTrip.referenceTorqueNm == 17,
          "steering model settings survive save and reload");

    gt2::sim::CarBody car{}; car.steerLock = 512; car.sprungWeightPerAxle[0] = 24000; car.suspension[0].staticLoad = 2000;
    gt2::sim::AxleTyreBlock tyre{}; tyre.slipAngleCount = 3;
    tyre.slipAngleXs[1] = 64; tyre.slipAngleXs[2] = 512;
    tyre.slipAngleYs[1] = 4096; tyre.slipAngleYs[2] = 3000;
    std::memcpy(&car.tyres[0], &tyre, sizeof(tyre));
    for (auto& wheel : car.wheels) {
        wheel.load = wheel.gripForce = 14000; wheel.slipRatioGrip = 4096; wheel.contactForwardSpeed = 20 * 4096; wheel.rimSpeed = 20 * 4096;
    }
    auto nativeMoment = [&](int steer, int sideways) {
        for (auto& wheel : car.wheels) { wheel.steerAngle = int16_t(steer); wheel.contactLateralSpeed = sideways; }
        gt2::sim::UpdateSlipAngles(car);
        for (auto& wheel : car.wheels) {
            const int factor = gt2::sim::SymmetricCurve({tyre.slipAngleXs, tyre.slipAngleYs, 3}, wheel.steerAngle - wheel.slipAngle);
            wheel.lateralForce = int32_t(int64_t(wheel.gripForce) * factor / 4096);
        }
        return w::CalculateSteeringMoments(gt2game::WheelFeedbackOf(car), {});
    };
    Check(nativeMoment(40, 0).handwheelNm > 0 && nativeMoment(-40, 0).handwheelNm < 0,
          "native GT2 force signs oppose left and right steering in forward motion");
    Check(nativeMoment(0, 2 * 4096).handwheelNm > 0, "sideslip creates countersteering torque with physical wheel centred");
    gt2::sim::UpdateSlipAngles(car);
    const int aligned = car.wheels[0].slipAngle;
    Check(std::abs(nativeMoment(aligned, 2 * 4096).handwheelNm) < .001f,
          "wheel aligned with contact velocity is an off-centre torque equilibrium");
    const auto native = gt2game::WheelFeedbackOf(car);
    Check(Near(native.front[0].loadN, 3500) && Near(native.front[0].nominalLoadN, 3500), "native load and static reference convert consistently to newtons");

    if (const char* path = std::getenv("GT2_FFB_CURVE_LOG")) {
        std::ofstream out(path);
        out << "slip_deg,load_fraction,lateral_force_n,pneumatic_trail_mm,mechanical_nm,pneumatic_nm,handwheel_nm\n";
        for (float fraction : {1.f, .5f, .1f, 0.f}) for (int degrees = -40; degrees <= 40; ++degrees) {
            auto sweep = frame; const float angle = degrees * .01745329252f;
            const int units = int(std::lround(degrees * 4096.f / 360));
            const float factor = gt2::sim::SymmetricCurve({tyre.slipAngleXs, tyre.slipAngleYs, 3}, units) / 4096.f;
            for (auto& contact : sweep.front) {
                contact.loadN = 3500 * fraction; contact.lateralForceN = 3500 * fraction * factor;
                contact.lateralCapacityN = 3500 * fraction; contact.corneringStiffness = 3500 * fraction / (64 * .001533980788f);
                contact.slipRadians = angle;
            }
            const auto m = w::CalculateSteeringMoments(sweep, {});
            out << degrees << ',' << fraction << ',' << sweep.front[0].lateralForceN << ',' << m.trailM[0] * 1000 << ','
                << m.mechanicalNm[0] << ',' << m.pneumaticNm[0] << ',' << m.handwheelNm << '\n';
        }
        Check(bool(out), "write steering moment sweep report");
    }
}
