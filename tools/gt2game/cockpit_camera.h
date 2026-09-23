#pragma once
#include "game/camera/race_camera.h"
#include "gt2view/cockpit_fit.h"
#include <cmath>

namespace gt2game {
struct CockpitSettings {
    static constexpr int kMinHeightCm = -20, kMaxHeightCm = 20;
    static constexpr int kMinBackCm = -20, kMaxBackCm = 40;
    static constexpr int kMinMirrorScalePercent = 25, kMaxMirrorScalePercent = 100;
    bool enabled = true;
    bool steeringWheel = true;
    bool mirror = true;
    int mirrorScalePercent = 100;
    int seatHeightCm = 0;
    int seatBackCm = 0;
};

inline float CockpitSeatBack(const CockpitSettings& settings, bool desktop = false) {
    return .15f + settings.seatBackCm * .01f + (desktop ? .18f : 0.f);
}

// Tracking remains in seated metres. Only its presentation parent follows the
// car's suspension/impact transform instead of the horizon-stabilised camera.
inline std::array<float,16> CockpitControlsLocal(const gt2view::CockpitFit& fit, const CockpitSettings& settings,
                                               float scale, const float rigSeat[3]) {
    return {scale,0,0,0, 0,scale,0,0, 0,0,scale,0,
            fit.eye[0]+rigSeat[0],fit.eye[1]+settings.seatHeightCm*.01f+rigSeat[1],
            fit.eye[2]+CockpitSeatBack(settings)+rigSeat[2],1};
}

inline bool CockpitShown(const gt2::camera::RaceCamera& camera, bool enabled, bool replay, bool oldCamera, uint32_t hold) {
    // StartIntro returns to the selected race camera for its final 60 fields.
    // The cabin must be present from that camera cut, before the cars are released.
    return enabled && !replay && !oldCamera && hold < 60 && camera.position == 0 &&
        camera.target == 0 && camera.hideTarget && !camera.external;
}

inline void PlaceCockpitEye(gt2::camera::CameraProjection& camera, const float model[16],
                            const gt2view::CockpitFit& fit, const CockpitSettings& settings, bool desktop = false) {
    const float local[] = {fit.eye[0], fit.eye[1] + settings.seatHeightCm * .01f,
                          fit.eye[2] + CockpitSeatBack(settings, desktop)};
    for (int row = 0; row < 3; ++row)
        camera.eye[row] = model[12 + row] + model[row] * local[0] + model[4 + row] * local[1] + model[8 + row] * local[2];
    for (int row = 0; row < 3; ++row)
        camera.rows[row][3] = -(camera.rows[row][0] * camera.eye[0] + camera.rows[row][1] * camera.eye[1] + camera.rows[row][2] * camera.eye[2]);
}

inline void FrameDesktopCockpit(gt2::camera::CameraProjection& camera) {
    // A monitor frames the dashboard; headset orientation and FOV stay runtime-owned.
    constexpr float angle = .1396263402f;
    const float cosine = std::cos(angle), sine = std::sin(angle);
    float up[3], forward[3];
    for (int axis = 0; axis < 3; ++axis) {
        up[axis] = camera.up[axis] * cosine + camera.forward[axis] * sine;
        forward[axis] = camera.forward[axis] * cosine - camera.up[axis] * sine;
    }
    for (int row = 0; row < 3; ++row) {
        float upWeight = 0, forwardWeight = 0;
        for (int axis = 0; axis < 3; ++axis) {
            upWeight += camera.rows[row][axis] * camera.up[axis];
            forwardWeight += camera.rows[row][axis] * camera.forward[axis];
        }
        camera.rows[row][3] = 0;
        for (int axis = 0; axis < 3; ++axis) {
            camera.rows[row][axis] += upWeight * (up[axis] - camera.up[axis]) + forwardWeight * (forward[axis] - camera.forward[axis]);
            camera.rows[row][3] -= camera.rows[row][axis] * camera.eye[axis];
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        camera.up[axis] = up[axis];
        camera.forward[axis] = forward[axis];
    }
    camera.H *= 5.f / 6.f;
}
}
