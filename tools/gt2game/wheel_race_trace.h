#pragma once
#include "platform/input/input_diagnostics.h"
#include "platform/input/wheel.h"
#include "game/sim/drivetrain.h"
#include "wheel_feedback.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace gt2game {
class WheelRaceTrace {
public:
    void Sample(const gt2::input::wheel::Rig& rig, const gt2::sim::PadRecord& pad, const gt2::sim::CarBody& car,
                int step, bool directionBlocked) {
        static const bool modelProbe = std::getenv("GT2_FFB_TRACE") != nullptr;
        if (!(pad.flags & gt2::sim::kWheelPad) && !modelProbe) { valid_ = false; return; }
        const bool event = !valid_ || step <= lastStep_ || pad.shift != lastShift_ || car.gear != lastGear_ ||
            std::abs(int(pad.steer) - lastSteer_) >= 1024 || std::abs(int64_t(car.yawRate) - lastYaw_) >= 8192 ||
            car.contactFlags != lastContact_ || directionBlocked != lastBlocked_;
        lastStep_ = step; lastShift_ = pad.shift; lastGear_ = car.gear; lastSteer_ = pad.steer;
        lastYaw_ = car.yawRate; lastContact_ = car.contactFlags; lastBlocked_ = directionBlocked; valid_ = true;
        if (!event && step % 30 != 0) return;
        const auto& binding = gt2::input::wheel::Config().axes[gt2::input::wheel::Steering];
        int raw = 0; bool online = false;
        for (const auto& device : rig.Devices()) if (device.id == binding.device && binding.axis >= 0 && binding.axis < gt2::input::wheel::kAxes) {
            raw = device.axes[binding.axis]; online = device.online; break;
        }
        char line[1024];
        std::snprintf(line, sizeof(line),
            "wheel race step=%d ready=%d online=%d raw=%d mapped=%.5f padSteer=%d bodySteer=%d lock=%d "
            "shift=%d gear=%d mode=%d blocked=%d throttle=%u brake=%u speed=%ld lateral=%ld yaw=%ld "
            "contacts=%u wall=%u push=%ld,%ld frontForce=%ld,%ld load=%ld,%ld,%ld,%ld",
            step, int(rig.Current().ready), int(online), raw, double(rig.Current().steering), int(pad.steer), int(car.steerAngle), int(car.steerLock),
            int(pad.shift), int(car.gear), int(pad.reserved & 15), int(directionBlocked), unsigned(pad.throttle), unsigned(pad.brake),
            long(car.forwardSpeed), long(car.lateralSpeed), long(car.yawRate), unsigned(car.contactFlags), unsigned(car.wallHitMask),
            long(car.pushForce[0]), long(car.pushForce[1]), long(car.wheels[0].lateralForce), long(car.wheels[1].lateralForce),
            long(car.wheels[0].load), long(car.wheels[1].load), long(car.wheels[2].load), long(car.wheels[3].load));
        gt2::input::InputDiagnostic(line);
        const auto frame = WheelFeedbackOf(car);
        const auto moments = gt2::input::wheel::CalculateSteeringMoments(frame, gt2::input::wheel::Config().steeringGeometry);
        std::snprintf(line, sizeof(line),
            "steering model step=%d valid=%d ratio=%.2f loadN=%.1f,%.1f FyN=%.1f,%.1f slip=%.4f,%.4f "
            "trailMm=%.2f,%.2f mechanicalNm=%.3f,%.3f pneumaticNm=%.3f,%.3f handwheelNm=%.3f output=%.4f",
            step, int(moments.valid), double(moments.steeringRatio), double(frame.front[0].loadN), double(frame.front[1].loadN),
            double(frame.front[0].lateralForceN), double(frame.front[1].lateralForceN), double(frame.front[0].slipRadians), double(frame.front[1].slipRadians),
            double(moments.trailM[0] * 1000), double(moments.trailM[1] * 1000), double(moments.mechanicalNm[0]), double(moments.mechanicalNm[1]),
            double(moments.pneumaticNm[0]), double(moments.pneumaticNm[1]), double(moments.handwheelNm), double(rig.OutputForce()));
        gt2::input::InputDiagnostic(line);
    }
private:
    bool valid_ = false, lastBlocked_ = false;
    int lastStep_ = 0, lastShift_ = 0, lastGear_ = 0, lastSteer_ = 0;
    int32_t lastYaw_ = 0;
    uint8_t lastContact_ = 0;
};
}
