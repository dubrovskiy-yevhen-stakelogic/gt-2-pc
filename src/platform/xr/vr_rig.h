#pragma once
// The VR rig of the race (docs/research/vr_port_plan.md, section 2 and M2): where the player's two eyes are in the
// game's world, and the matrices the draw list is rendered with.
//
//   world_from_eye[v] = C . H . S . local_from_eye[v]
//
//   C  the original's race camera of this frame (game/camera; interpolated between the last two 30 Hz steps by
//      tools/gt2game/frame_interp.h). Its columns are right, up, back and it looks along -z - the OpenXR convention
//      with 1 unit = 1 m, so no axis conversion is needed anywhere.
//   H  the horizon lock: the camera's pitch and roll stripped by a percentage, its yaw kept (a rotation in the
//      camera's own frame, so that C . H is the levelled camera).
//   S  the seat offset in the levelled camera's axes (right, up, back). The driver view's 0.8 m above the car's
//      matrix is already part of C (the original does it), so the default is zero.
//   local_from_eye  what xrLocateViews reported, moved into the recentred origin (Recenter below). The head pose is
//      never interpolated: it is the runtime's prediction for this compositor frame.
//
// The draw list is built ONCE for both eyes in "reference space" R = world - refEye (refEye = the mid eye), so the
// per-eye matrices this produces map R to clip space:
//   kWorld items:  worldVP[v] . mvp      (mvp = T(-refEye) . model)
//   kSky items:    skyVP[v] . mvp        (the same without the eye translation: the backdrop sits at infinity and
//                                         has no parallax between the eyes)
//   kScreen items: HudProjection maps the item's clip coordinates to a shared plane ahead of the head.
//
// Pure arithmetic: no OpenXR and no Vulkan types, so it compiles on the PC and on Android and can be tested on its
// own. Reversed Z with an infinite far plane (z_ndc = nearZ / depth), Vulkan clip space (y down), exactly what the
// desktop renderer uses.
#include <cstdint>

namespace gt2::vr {

// A rigid pose in the runtime's convention: metres, quaternion (x, y, z, w).
struct Pose {
    float position[3] = {0, 0, 0};
    float orientation[4] = {0, 0, 0, 1};
};

// A projection as OpenXR states it: the tangent angles of the four frustum sides in radians (left and down are
// negative). Asymmetric per eye.
struct Fov {
    float left = -0.785398f, right = 0.785398f, up = 0.785398f, down = -0.785398f;
};

struct EyeView {
    Pose pose;
    Fov fov;
};

struct Settings {
    float horizonLock = 0.6f; // 0 = the camera as it is, 1 = pitch and roll removed entirely (yaw always kept)
    float worldScale = 1.0f;  // game metres per real metre (head translation and eye separation are scaled by it)
    float seat[3] = {0, 0, 0}; // metres along the levelled camera's right / up / back axes
    float nearZ = 0.05f;
    float ipd = -1.0f;        // >= 0: the eye separation forced to this (the checks use 0); < 0 = the runtime's
    bool originalFov = false; // the original camera's frustum instead of the runtime's (the checks' (a))
};

// C, as the renderer already has it: the camera's world frame and the clip matrix camera::ClipMatrix /
// frame_interp.h ClipMatrixOf built for the eye image's aspect (only `originalFov` reads it).
struct Camera {
    float eye[3] = {0, 0, 0};
    float right[3] = {1, 0, 0};
    float up[3] = {0, 1, 0};
    float forward[3] = {0, 0, -1};
    float clip[16] = {};
};

// What the rig produced for one compositor frame.
struct View {
    bool valid = false;
    float refEye[3] = {0, 0, 0};                    // the mid eye in world metres: the origin the draw list uses
    float right[3] = {1, 0, 0};                     // the mid eye's world axes - the CPU work's camera (billboards,
    float up[3] = {0, 1, 0};                        // the car's reflection axes, the scenery LOD, the backdrop)
    float forward[3] = {0, 0, -1};
    float worldVP[2][16] = {};                      // R -> clip, per eye
    float skyVP[2][16] = {};                        // the same with the eye translation removed
    float eyeWorld[2][3] = {};                      // each eye in world metres (the log / the checks)
    float eyeQuat[2][4] = {};                       // each eye's world orientation (x, y, z, w)
    Fov fov[2];                                     // the frustum each eye was rendered with
    float levelled[9] = {};                         // C . H as a column-major 3x3 (right, up, back)
    float ipd = 0;                                  // the eye separation used, in real metres
};

// The recentred origin is the head's position and yaw at the last recentre. The game's camera already
// supplies the driver's eye height; subsequent leaning/crouching remains relative to this seated origin.
// Recentre on XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING (and once at the start).
class Recenter {
public:
    void Request() { pending_ = true; }
    bool Pending() const { return pending_; }
    // Latches `head` (the VIEW space located in LOCAL) as the new origin.
    void Latch(const Pose& head);
    // A pose the runtime reported in LOCAL -> the recentred space.
    Pose Apply(const Pose& local) const;
    float Yaw() const { return yaw_; }

private:
    bool pending_ = true;
    float offset_[3] = {0, 0, 0};
    float yaw_ = 0;
};

// world_from_eye[v] = C . H . S . eyes[v], and the matrices the draw list is rendered with.
View Build(const Camera& camera, const EyeView eyes[2], const Settings& settings);
// Preserve the original fly-through and orientation; lower only elevated camera positions.
void LowerIntroCamera(Camera& camera, float groundY, float lowering = 2.0f);

// The frustum of the original camera (`camera.clip`), as an OpenXR field of view. Exposed for the checks and for the
// log; Build uses it when Settings::originalFov is set.
Fov OriginalFov(const Camera& camera);

// Column-major projection matrix of `fov` (Vulkan clip space, y down, reversed Z with an infinite far plane).
void Projection(const Fov& fov, float nearZ, float out[16]);
// Project a common 2.56 x 1.92 m head-relative HUD plane 2 m ahead through each eye.
void HudProjection(const Pose& head, const EyeView eyes[2], float out[2][16]);

} // namespace gt2::vr
