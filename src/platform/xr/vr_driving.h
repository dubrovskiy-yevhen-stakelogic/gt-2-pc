#pragma once
#include "platform/xr/vr_rig.h"
#include <array>
#include <optional>

namespace gt2::vr {
struct TrackedControllers {
    Pose gripPose[2], aimPose[2];
    bool gripValid[2]{}, aimValid[2]{};
    float grip[2]{}, trigger[2]{};
};
struct DrivingSettings {
    int mode = 0; // 0 stick, 1 virtual wheel, 2 motion
    int motionHand = 1;
    int sensitivity = 100; // Below 100: softer centre and smoothed virtual-wheel response at every turn size; full lock remains.
    int deadZonePercent = 3;
    bool toggleGrip = false; // Virtual wheel only; each hand latches independently.
    int wheelHeightCm = -28, wheelDistanceCm = 38, wheelRadiusCm = 18;
    // Runtime cockpit anchor in recentered tracking metres; never persisted.
    std::optional<std::array<float,3>> cockpitWheelCenter;
};
inline std::array<float,3> DrivingWheelCenter(const DrivingSettings& s) {
    return s.cockpitWheelCenter.value_or(std::array<float,3>{0,s.wheelHeightCm*.01f,-s.wheelDistanceCm*.01f});
}
// Vice City Quest's car steering behavior, independent of its vehicle engine.
class DrivingController {
public:
    static constexpr float maxWheelAngle = 1.3962634f; // 80 degrees
    void Reset();
    float Update(const TrackedControllers& input, const DrivingSettings& settings, bool active, float elapsedSeconds = 1.f/90.f);
    float Angle() const { return physicalAngle_; }
    bool Grabbed(int hand) const { return grabbed_[hand]; }
    bool MotionCalibrated() const { return motionReferenceValid_; }
private:
    float steering_ = 0;
    int mode_ = -1, hand_ = -1;
    bool toggleGrip_ = false;
    bool grabbed_[2]{}, gripDown_[2]{}, angleValid_[2]{}, spokeUsable_[2]{};
    float reference_[2]{}, continuous_[2]{};
    bool twoValid_ = false, motionReferenceValid_ = false;
    float twoReference_ = 0, twoContinuous_ = 0, physicalAngle_ = 0;
    float motionReference_[4]{0,0,0,1}, motionAxis_[3]{0,0,1};
};
// Trigger hysteresis: one menu step per squeeze, rearmed by release.
class MenuTriggers {
public:
    void Begin(float left, float right) { down_ = {left > .2f, right > .2f}; }
    int Update(float left, float right) {
        const float values[2] = {left, right}; int result = 0;
        for (int h = 0; h < 2; ++h) {
            if (values[h] <= .2f) down_[h] = false;
            else if (values[h] >= .65f && !down_[h]) { down_[h] = true; result += h == 0 ? -1 : 1; }
        }
        return result;
    }
private:
    std::array<bool, 2> down_{};
};
}
