#pragma once
// The race camera of the native game: the ported camera object (race_camera.h) fed from sim::RaceSim once per race
// frame, like the original's frame driver 0x80015B64 does after the physics tick (0x8003EBF0 -> 0x800100F4), plus the
// state the original keeps outside the object (the trackside camera progress 0x800A8D6C, the live onboard table
// 0x8002F378). The renderer reads Camera() (ProjectionOf / ClipMatrix).
#include <cstdint>
#include <span>
#include <vector>

#include "game/camera/race_camera.h"

namespace gt2 {
struct CarModel;
}
namespace gt2::sim {
class RaceSim;
}

namespace gt2::camera {

// car + 0x87C and the LOD 0 scale / size of a car model (0x80017E74; the trackside zoom 0x80011568, side views).
struct CarShape {
    int32_t halfWidth = 0x10000;
    uint16_t lodScale = 16, lodSize = 1;
};
CarShape CarShapeOf(const CarModel& model);

class GameCamera {
public:
    struct Options {
        uint8_t cameraPosition = 0; // Camera Position option (career + 0xAF): 0 Driver, 1 Chase 1, 2 Chase 2
        uint8_t viewAngle = 1;      // View Angle option (career + 0xB2): 0 Narrow, 1 Standard, 2 Wide
        uint8_t replayInfo = 0;     // Replay Info option (career + 0xAE)
        uint8_t gameMode = 2;       // 0x801D5866
        uint8_t replay = 0;         // 0x800A951C (attract race / replay: the trackside cameras)
        // The 2 player Battle (game mode 0): the race load runs 0x80010000(camera, player) for both camera objects (view + 0xC4 +
        // player * 0x110; the camera follows car `player`) and sets + 0x103 = 1 (half-height views) before the first update.
        uint8_t player = 0;
        bool split = false;
    };
    // `constants` / `track` / `replayCameras` must outlive the object (replayCameras.bytes too). `shapes`: one per car.
    void Setup(const CameraConstants& constants, const Track& track, const ReplayCameraData& replayCameras, std::vector<CarShape> shapes,
               const Options& options);
    // 0x80010000 for player 1 at the race load (and at a restart), then the first update (0x80015AB4).
    void Start(const sim::RaceSim& race);
    // 0x800100F4 after a physics step. `pad`: the logical buttons (kButtonView pressed, kButtonLookBack held; in a
    // replay the replay controls).
    void Update(const sim::RaceSim& race, const CameraPad& pad);

    const RaceCamera& Camera() const { return camera_; }
    RaceCamera& Camera() { return camera_; }
    const Options& GetOptions() const { return options_; }
    // A replay of the race (0x800A951C set: 0x800109FC's replay cameras from the next Start) or the race again.
    void SetReplay(bool replay) { options_.replay = replay ? 1 : 0; }
    // The lap step the replay controls of a game mode 6 replay wrote to car + 0x21 of the followed car in the updates since the
    // last call (0x800109FC: L1 = -1, R1 = +1; 0 = none), cleared by the call - what 0x80013EF0 reads at the next tick
    // (race_shell.h GhostSession::replayLapStep).
    int8_t TakeLapStep(size_t car);

private:
    CameraWorld World(const sim::RaceSim& race);

    RaceCamera camera_{};
    const CameraConstants* constants_ = nullptr;
    const Track* track_ = nullptr;
    ReplayCameraData replay_;
    std::vector<CarShape> shapes_;
    std::vector<CameraCar> cars_;
    std::vector<uint8_t> lapStep_; // car + 0x21 per car (CameraCar::ghostByte)
    std::array<OnboardView, 13> onboard_{};
    int32_t replayProgress_ = 0;
    Options options_;
};

} // namespace gt2::camera
