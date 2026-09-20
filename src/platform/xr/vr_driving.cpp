// Adapted from MiamiVR QuestDrivingVR.cpp; see third_party/vrhands/MIAMIVR_LICENSE.txt.
#include "platform/xr/vr_driving.h"
#include <algorithm>
#include <cmath>
namespace gt2::vr {
namespace {
constexpr float pi = 3.14159265358979323846f;
float Wrap(float a) { while (a > pi) a -= 2*pi; while (a < -pi) a += 2*pi; return a; }
float Unwrap(float a, float reference) { return reference + Wrap(a-reference); }
float DeadZone(float value, float zone) {
    return std::abs(value) <= zone ? 0 : std::copysign((std::abs(value)-zone)/(1-zone), value);
}
}
void DrivingController::Reset() { *this = {}; }
float DrivingController::Update(const TrackedControllers& in, const DrivingSettings& s, bool active) {
    if (!active || s.mode == 0) { Reset(); return 0; }
    if (mode_ != s.mode || hand_ != s.motionHand) { Reset(); mode_ = s.mode; hand_ = s.motionHand; }
    if (s.mode == 2) {
        const int hand = std::clamp(s.motionHand, 0, 1);
        if (in.grip[hand] <= .30f || !in.gripValid[hand]) {
            motionReferenceValid_ = false; physicalAngle_ = 0; return 0;
        }
        const auto& q=in.gripPose[hand].orientation;
        if (!motionReferenceValid_) {
            physicalAngle_=0;
            if(in.grip[hand]<.65f)return 0;
            std::copy(q,q+4,motionReference_);
            const auto& a=in.aimValid[hand]?in.aimPose[hand].orientation:q;
            // The initial forearm direction, toward the player. Wrist roll is
            // the twist about this axis, independent of yaw, pitch and position.
            motionAxis_[0]=2*(a[0]*a[2]+a[3]*a[1]);
            motionAxis_[1]=2*(a[1]*a[2]-a[3]*a[0]);
            motionAxis_[2]=1-2*(a[0]*a[0]+a[1]*a[1]);
            motionReferenceValid_=true;
        }
        const auto& r=motionReference_;
        // World-space relative rotation: current * inverse(reference).
        float d[4]={-q[3]*r[0]+q[0]*r[3]-q[1]*r[2]+q[2]*r[1],
                    -q[3]*r[1]+q[1]*r[3]-q[2]*r[0]+q[0]*r[2],
                    -q[3]*r[2]+q[2]*r[3]-q[0]*r[1]+q[1]*r[0],
                    q[3]*r[3]+q[0]*r[0]+q[1]*r[1]+q[2]*r[2]};
        const float twist=d[0]*motionAxis_[0]+d[1]*motionAxis_[1]+d[2]*motionAxis_[2];
        if(twist*twist+d[3]*d[3]<1e-6f)return -DeadZone(physicalAngle_/maxWheelAngle,.03f);
        physicalAngle_=std::clamp(Wrap(2*std::atan2(twist,d[3])),-maxWheelAngle,maxWheelAngle);
        return -DeadZone(physicalAngle_/maxWheelAngle,.03f);
    }
    const float cy = s.wheelHeightCm*.01f, cz = -s.wheelDistanceCm*.01f, radius = s.wheelRadiusCm*.01f;
    bool just[2]{};
    for (int h = 0; h < 2; ++h) {
        const auto& p = in.gripPose[h].position;
        const float dx = p[0]-(h == 0 ? -radius : radius), dy = p[1]-cy, dz = p[2]-cz;
        if (grabbed_[h] && (!in.gripValid[h] || in.grip[h] <= .30f)) {
            grabbed_[h] = angleValid_[h] = spokeUsable_[h] = false;
        }
        if (!grabbed_[h] && in.gripValid[h] && in.grip[h] >= .65f && !gripDown_[h] && dx*dx+dy*dy+dz*dz <= .23f*.23f)
            grabbed_[h] = just[h] = true;
        if (in.grip[h] <= .30f) gripDown_[h] = false;
        else if (in.grip[h] >= .65f) gripDown_[h] = true;
        if (just[h]) {
            reference_[h] = Wrap(std::atan2(p[1]-cy,p[0])-physicalAngle_);
            continuous_[h] = physicalAngle_; angleValid_[h] = spokeUsable_[h] = true;
        }
    }
    float angle = 0;
    if (grabbed_[0] && grabbed_[1]) {
        const auto& l = in.gripPose[0].position; const auto& r = in.gripPose[1].position;
        const float chord = std::atan2(r[1]-l[1],r[0]-l[0]);
        if (!twoValid_ || just[0] || just[1]) {
            twoReference_ = Wrap(chord-physicalAngle_); twoContinuous_ = physicalAngle_; twoValid_ = true;
        }
        angle = Unwrap(Wrap(chord-twoReference_),twoContinuous_); twoContinuous_ = angle;
        for (int h = 0; h < 2; ++h) {
            const auto& p = in.gripPose[h].position;
            reference_[h] = Wrap(std::atan2(p[1]-cy,p[0])-angle);
            continuous_[h] = angle; angleValid_[h] = true;
        }
    } else {
        twoValid_ = false;
        for (int h = 0; h < 2; ++h) if (grabbed_[h]) {
            const auto& p = in.gripPose[h].position;
            const float x = p[0], y = p[1]-cy, raw = std::atan2(y,x);
            angle = continuous_[h];
            const bool usable = x*x+y*y >= .06f*.06f;
            bool reseat = !angleValid_[h];
            if (usable && angleValid_[h]) {
                const float measured = Unwrap(Wrap(raw-reference_[h]),angle);
                if (!spokeUsable_[h] || std::abs(measured-angle) > pi/4) reseat = true;
                else angle = measured;
            } else if (!usable) reseat = false;
            const float locked = std::clamp(angle,-maxWheelAngle,maxWheelAngle);
            if (reseat || locked != angle) { angle = locked; if (usable) reference_[h] = Wrap(raw-angle); }
            spokeUsable_[h] = usable; continuous_[h] = angle; angleValid_[h] = true;
            break;
        }
    }
    physicalAngle_ = std::clamp(angle,-maxWheelAngle,maxWheelAngle);
    // Wheel angle is counter-clockwise (left); the PS1 analogue axis is positive right.
    return -DeadZone(std::clamp(angle/maxWheelAngle,-1.f,1.f), .03f);
}
}
