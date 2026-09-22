#pragma once
// High frame rates for the race (docs/formats/modern_graphics.md): the simulation keeps the original's 30 Hz step
// (sim::RaceSim, bit-exact, frame-locked replays); the renderer draws every display refresh a state between the last
// two steps. Everything here works on COPIES of what the renderer reads after a step (the car render transforms, the
// camera object's output, the smoke pool, the HUD's gauge inputs) and produces float render data only - the simulation
// and the camera object are never written.
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "game/camera/race_camera.h"
#include "game/sim/race_sim.h"
#include "gt2view/particles.h"

namespace gt2game {

// What the race view renders of one simulation step (captured before the next step changes it).
struct RenderSnapshot {
    bool valid = false;
    std::vector<gt2::sim::CarPose> poses;   // car + 0x81C / + 0x830 of every car
    std::vector<gt2::sim::CarPose> groundPoses; // ground-following transform for shadows
    gt2::camera::RaceCamera camera{};       // the camera object after the step's camera update
    gt2view::SmokePool smoke;               // the sprite pool after the step's spawns
    int32_t rpm = 0, speedReadout = 0, boost = 0; // HUD gauges: body + 0x6D8 / + 0x6DA / + 0x76E of the player
    uint32_t clock = 0;                     // RaceSim::RaceClock
    std::vector<std::array<gt2::sim::WheelVisual, 4>> wheels; // steer / rolling angle / suspension of every car's wheels
};

// Column-major model matrix between two render transforms: translation lerped (from the 16.16 metres), rotation
// lerped and re-orthonormalised (Gram-Schmidt on the columns, which keeps the handedness).
void InterpolatedModelMatrix(const gt2::sim::CarPose& a, const gt2::sim::CarPose& b, float t, float* m);
// A car that moved more than 20 m in one step (reset to the grid, a replay restart): drawn at its new pose.
bool PoseJump(const gt2::sim::CarPose& a, const gt2::sim::CarPose& b);
gt2::sim::CarPose InterpolatedPoseRounded(const gt2::sim::CarPose& a, const gt2::sim::CarPose& b, float t); // positions only (map dots)

// A camera cut between two steps (position / look-back / followed car / replay camera changed, or a jump of the eye
// or the view direction): the new camera is drawn as it is.
bool CameraCut(const gt2::camera::RaceCamera& a, const gt2::camera::RaceCamera& b);
// ProjectionOf between two camera objects: eye lerped, view axes lerped and re-orthonormalised, H lerped; the window
// (rectangle, span, centre) of `b`.
gt2::camera::CameraProjection InterpolatedProjection(const gt2::camera::RaceCamera& a, const gt2::camera::RaceCamera& b, float t);
// camera::ClipMatrix on a projection (the same formula).
void ClipMatrixOf(const gt2::camera::CameraProjection& p, float aspect, float zNear, float out[16]);

// The smoke pool between two steps: records alive in both (same slot, kind and spawn angle) lerped in position, size and
// intensity; the others as in `b`.
gt2view::SmokePool InterpolatedSmoke(const gt2view::SmokePool& a, const gt2view::SmokePool& b, float t);

inline int32_t LerpInt(int32_t a, int32_t b, float t) { return int32_t(double(a) + (double(b) - double(a)) * double(t)); }

// The render-rate log: presented frames per second, frame times, the simulation steps in the same interval; printed every
// `interval` seconds, and one CSV line per presented frame with --frame-log <file>.
class FrameLog {
public:
    using Clock = std::chrono::steady_clock;
    explicit FrameLog(const std::string& csvPath = {}, double interval = 5.0);
    ~FrameLog();
    FrameLog(const FrameLog&) = delete;
    FrameLog& operator=(const FrameLog&) = delete;
    // One presented frame: `extra` = an in-between frame (not a 60 Hz field), `alpha` = the interpolation weight (-1 = none),
    // `buildMs` / `presentMs` = the time spent building its draw items / in the renderer's Draw (-1 = not measured).
    void Presented(bool extra, double alpha, int simSteps, double buildMs = -1, double presentMs = -1, double gpuMs = -1);
    void Reset();
    void Field(int simSteps);

private:
    std::FILE* csv_ = nullptr;
    double interval_;
    Clock::time_point start_, windowStart_, last_;
    bool haveLast_ = false;
    int frames_ = 0, extras_ = 0, timed_ = 0, stepsAtWindow_ = 0;
    double maxFrame_ = 0, sumFrame_ = 0;
    double sumBuild_ = 0, sumPresent_ = 0;
    int workSamples_ = 0, gpuSamples_ = 0; double sumGpu_ = 0, maxGpu_ = 0;
};

} // namespace gt2game
