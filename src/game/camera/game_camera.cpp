#include "game/camera/game_camera.h"

#include <cstring>
#include <stdexcept>

#include "game/sim/race_sim.h"
#include "gt2formats/car_model.h"
#include "gt2formats/track.h"

namespace gt2::camera {

CarShape CarShapeOf(const CarModel& model) {
    CarShape s;
    if (model.lods.empty()) return s;
    const CarLod& lod = model.lods[0];
    s.halfWidth = CarHalfWidth(lod.bbox, lod.scale);
    s.lodScale = uint16_t(lod.scale);
    s.lodSize = uint16_t(lod.scaleUnk);
    return s;
}

void GameCamera::Setup(const CameraConstants& constants, const Track& track, const ReplayCameraData& replayCameras, std::vector<CarShape> shapes,
                       const Options& options) {
    constants_ = &constants;
    track_ = &track;
    replay_ = replayCameras;
    shapes_ = std::move(shapes);
    options_ = options;
    onboard_ = constants.onboard;
    replayProgress_ = 0;
}

CameraWorld GameCamera::World(const sim::RaceSim& race) {
    if (!constants_ || !track_) throw std::logic_error("GameCamera: Setup first");
    const sim::RaceShellState& shell = race.Shell().State();
    cars_.resize(race.CarCount());
    lapStep_.resize(race.CarCount(), 0);
    for (size_t i = 0; i < race.CarCount(); i++) {
        const sim::CarBody& body = race.CarAt(i).body;
        const sim::CarPose pose = race.Pose(i); // car + 0x81C / + 0x830 (0x800133F0)
        CameraCar& c = cars_[i];
        std::memcpy(c.pose.m, pose.rotation.data(), sizeof c.pose.m);
        c.pose.pad = 0;
        for (size_t k = 0; k < 3; k++) c.pose.t[k] = pose.worldPosition[k];
        c.chunk = body.chunkIndex;
        c.viewYawOffset = body.viewYawOffset;
        c.viewPitchOffset = body.viewPitchOffset;
        c.courseDistance = body.courseDistance;
        c.lap = body.lap;
        c.racePosition = int8_t(body.racePosition);
        const CarShape shape = i < shapes_.size() ? shapes_[i] : CarShape{};
        c.halfWidth = shape.halfWidth;
        c.lodScale = shape.lodScale;
        c.lodSize = shape.lodSize;
        c.ghostByte = &lapStep_[i];
    }
    CameraWorld w;
    w.cars = cars_;
    w.gameMode = options_.gameMode;
    w.replay = options_.replay;
    w.hold = shell.hold;
    w.holdInitial = shell.holdInitial;
    w.viewAngle = options_.viewAngle;
    w.track = track_;
    w.courseLength = track_->courseLength;
    w.replayCameras = replay_;
    w.constants = constants_;
    w.replayProgress = &replayProgress_;
    w.onboard = onboard_.data();
    return w;
}

int8_t GameCamera::TakeLapStep(size_t car) {
    if (car >= lapStep_.size()) return 0;
    const int8_t step = int8_t(lapStep_[car]);
    lapStep_[car] = 0;
    return step;
}

void GameCamera::Start(const sim::RaceSim& race) {
    const CameraWorld w = World(race);
    InitCamera(camera_, options_.player, options_.cameraPosition, options_.replayInfo, w);
    camera_.split = options_.split ? 1 : 0; // the race load's mode 0 (docs/formats/camera.md section 1)
    UpdateCamera(camera_, CameraPad{}, w);
}

void GameCamera::Update(const sim::RaceSim& race, const CameraPad& pad) {
    const CameraWorld w = World(race);
    UpdateCamera(camera_, pad, w);
}

} // namespace gt2::camera
