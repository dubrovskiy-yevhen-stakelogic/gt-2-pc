#pragma once
#include "platform/input/wheel.h"
#include "game/sim/car_body.h"
#include <algorithm>
#include <cmath>

namespace gt2game {
// Optional driver inputs, evaluated once per live physics step before recording.
// They never change tyre grip, yaw, position, or a recorded replay frame.
class WheelDrivingAids {
public:
    void Reset() { throttleLimit_ = 1; correction_ = 0; }
    void Apply(gt2::LogicalPad& pad, const gt2::sim::CarBody& car, const gt2::input::wheel::Settings& s) {
        if (!pad.wheel || car.raceState != 0 || car.gear == 0 || pad.wheelGear == 2 ||
            pad.wheelGear == 1 || pad.clutch > 127 || (pad.buttons & gt2::kPadHandbrake)) { Reset(); return; }
        const int level = std::clamp(s.tractionControl, 0, 5);
        float limit = 1;
        if (level && car.forwardSpeed >= 0) {
            for (int i = 0; i < 4; ++i) {
                const bool driven = car.driveType == 1 ? i < 2 : (car.driveType == 0 || car.driveType == 5) ? i >= 2 : true;
                const auto& wheel = car.wheels[i];
                if (!driven || wheel.load <= 0 || (wheel.contactFlags & 1)) continue;
                const float peak = std::max(.05f, -float(car.tyres[i / 2].peakSlipRatioNeg) / 4096.f);
                const float threshold = peak * (1.75f - .2f * float(level));
                const float excess = std::max(0.f, -float(wheel.slipRatio) / 4096.f - threshold);
                limit = std::min(limit, 1.f / (1.f + excess * float(2 + 2 * level)));
            }
            // Cut promptly; restore over roughly one second to avoid repeated wheelspin.
            throttleLimit_ = std::min(limit, throttleLimit_ + .04f);
            pad.throttle = uint16_t(std::lround(float(pad.throttle) * throttleLimit_));
        } else throttleLimit_ = 1;

        const int steeringHelp = std::clamp(s.countersteer, 0, 2);
        const bool frontContact = (car.wheels[0].load > 0 && !(car.wheels[0].contactFlags & 1)) ||
                                  (car.wheels[1].load > 0 && !(car.wheels[1].contactFlags & 1));
        if (!steeringHelp || car.forwardSpeed < 8192 || car.steerLock <= 0 || !frontContact) { correction_ = 0; return; }
        constexpr float radiansPerUnit = 6.28318530718f / 4096.f;
        const float slip = std::atan2(float(car.lateralSpeed), float(car.forwardSpeed));
        const float input = std::clamp((2048.f - float(pad.steerAxis)) / 2048.f, -1.f, 1.f);
        float desired = 0;
        // A body slipping toward its yaw is oversteering. Ordinary steady turns
        // and understeer must not make the helper unwind the player's steering.
        if (int64_t(car.lateralSpeed) * car.yawRate > 0) {
            const float activity = std::clamp((std::abs(slip) - .05236f) / .12217f, 0.f, 1.f);
            const float target = std::clamp(-slip / (float(car.steerLock) * radiansPerUnit), -1.f, 1.f);
            // Do not reduce countersteer the player has already applied.
            if (input * target <= 0 || std::abs(input) < std::abs(target))
                desired = std::clamp((target - input) * activity * (steeringHelp == 1 ? .35f : .7f), -.35f, .35f);
        }
        correction_ += std::clamp(desired - correction_, -.04f, .04f);
        const float output = std::clamp(input + correction_, -1.f, 1.f);
        pad.steerAxis = uint16_t(std::clamp(std::lround(2048.f - output * 2048.f), 0l, 4095l));
    }
    float ThrottleLimit() const { return throttleLimit_; }
    float SteeringCorrection() const { return correction_; }
private:
    float throttleLimit_ = 1, correction_ = 0;
};
}
