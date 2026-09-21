// The 2 player Battle's split-screen race (split_race.h).
#include "split_race.h"

#include "platform/os/keys.h"
#include "platform/os/paths.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>

#include "frame_interp.h"
#include "game/audio/music_player.h"
#include "game/audio/race_audio.h"
#include "game/audio/race_music.h"
#include "game/camera/game_camera.h"
#include "game/career/career_state.h"
#include "game/menu/menu_car.h"
#include "game/shell/title_options.h"
#include "game_window.h"
#include "gt2export/car_mesh.h"
#include "gt2formats/car_model.h"
#include "gt2formats/course_data.h"
#include "gt2formats/exe_profile.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/race_capture.h"
#include "gt2formats/replay.h"
#include "gt2formats/sponsor_boards.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "gt2view/hud.h"
#include "gt2view/particles.h"
#include "gt2view/scene_assets.h"
#include "graphics_options.h"
#include "pc_overlay.h"
#include "panel.h"
#include "platform/input/ps1_pad.h"

using namespace gt2;
using namespace gt2view;

namespace gt2game {

namespace {

// The rectangles of 0x8002975C (race overlay 0x8002F1FC: {x, y, w, h} per player) as fractions of the frame: player 1 the
// top half, player 2 the bottom half; 0x8002972C: player 1's camera over the whole frame.
constexpr float kViewRect[2][4] = {{0.0f, 0.0f, 1.0f, 0.5f}, {0.0f, 0.5f, 1.0f, 1.0f}};
constexpr float kFullRect[4] = {0.0f, 0.0f, 1.0f, 1.0f};

// The tyre smoke inputs of one car (race_view.cpp SmokeInput: 0x800133F0's reads).
SmokePool::CarInput SmokeInputOf(const sim::RaceSim& race, size_t car, const std::array<int16_t, 4>& wheelZ) {
    SmokePool::CarInput in;
    const sim::CarPose pose = race.Pose(car);
    for (size_t i = 0; i < 3; i++) {
        for (size_t j = 0; j < 3; j++) in.rotation[i][j] = pose.rotation[i][j];
        in.translation[i] = pose.worldPosition[i];
    }
    const std::array<sim::WheelVisual, 4> wheels = race.Wheels(car);
    const sim::CarBody& body = race.CarAt(car).body;
    for (size_t w = 0; w < 4; w++) {
        const int16_t halfTrack = body.halfTrack[w >> 1];
        in.wheels[w].x = int16_t((w & 1) ? halfTrack : -halfTrack);
        in.wheels[w].y = wheels[w].verticalOffset;
        in.wheels[w].z = wheelZ[w];
        in.wheels[w].skidLevel = body.wheels[w].skidLevel;
    }
    return in;
}

// The pads of the two players inside the physics core: 0x8003C250 -> 0x80013EF0 (pad slot 2, player 1, stream 0x801D5F84) and
// 0x80014030 (pad slot 3, player 2, stream 0x801DA49C), both -> 0x80013C90: live, the logical pad of the step recorded into the
// player's stream (it stops 300 fields after the finish, 0x800A9522) and turned into the physics' pad record; in a replay
// (0x800A951C set) the stream's next frame, and a stream that ran out sets 0x800A8D68 (the replay is over).
struct SplitPads {
    std::array<LogicalPad, 2> pad{};
    std::array<ReplayStream, 2> stream{ReplayStream(), ReplayStream()};
    std::array<uint16_t, 16> pedalTable{};
    bool playback = false;
    bool ended = false; // 0x800A8D68
    static void Source(void* user, sim::RaceSim& race, sim::PadRecord* pads) {
        SplitPads& self = *static_cast<SplitPads*>(user);
        for (size_t car = 0; car < race.CarCount(); car++) {
            const int16_t slot = race.CarAt(car).padSlot;
            if (slot != 2 && slot != 3) continue;
            const size_t p = size_t(slot - 2);
            ReplayFrame frame{};
            if (self.playback) {
                self.stream[p].Read(frame); // left zero once the stream has ended
                if (self.stream[p].Ended()) self.ended = true;
            } else {
                frame = FrameOfPad(self.pad[p]);
                if (race.Shell().State().sinceFinish < 300) self.stream[p].Record(frame);
                else self.stream[p].End(false);
            }
            pads[car] = PadOfFrame(frame, self.pedalTable);
        }
    }
};

// A player's controller as the race overlay reads it (race_view.cpp): the pad object polled every field, its logical pad built
// every race frame through the key tables of the player's pad block.
struct PlayerPad {
    std::array<uint8_t, 0x200> object{};
    std::array<uint8_t, 0x200> scratch{};
    std::vector<uint8_t> block;
    bool triggerPedals = true;
    std::array<uint8_t, 4 * input::pad_block::kTableSize> TablesFor(const input::Ps1PadFrame& f) const {
        std::array<uint8_t, 4 * input::pad_block::kTableSize> t{};
        std::copy(block.begin(), block.begin() + std::ptrdiff_t(t.size()), t.begin());
        if (triggerPedals && f.pressure) {
            const std::array<uint8_t, 11> analog = input::TriggerPedalTable(t.data() + input::pad_block::kTableSize);
            std::copy(analog.begin(), analog.end(), t.begin() + input::pad_block::kTableSize);
        }
        return t;
    }
    uint16_t U16(uint32_t o) const { return uint16_t(object[o] | object[o + 1] << 8); }
    uint32_t U32(uint32_t o) const { return uint32_t(U16(o) | uint32_t(U16(o + 2)) << 16); }
};

// What a frame draws of the two views (the renderer 0x800292A0's calls): 0x800297F4 with its mask (bit 0: player 1's view
// not drawn, bit 1: player 2's) and / or 0x8002972C (player 1's camera over the whole frame).
struct ViewDraw {
    bool splitPlayer1 = false, splitPlayer2 = false, full = false;
};

// Sim 0x800292A0 (game mode 0; the Arcade build's copy is the same code): the view's split flag (view + 0x2EA) against the two
// camera objects' + 0x103. Off and either camera split: both split (+ 0x103 = 1), camera 1 follows car 0 and camera 2 car 1
// (+ 0x10C), both replay modes 0 (+ 0x105), the flag on. On and not both split: both unsplit with replay mode 2 (the trackside
// camera following the leader), the flag off. Then: the flag was on -> 0x800297F4(view, flag now off ? 2 : 0); the flag is off
// now -> 0x8002972C (so the frame that turns the split on draws neither).
ViewDraw SplitViewFrame(camera::RaceCamera& a, camera::RaceCamera& b, uint8_t& viewSplit) {
    const bool was = viewSplit != 0;
    if (!was) {
        if (a.split != 0 || b.split != 0) {
            a.target = 0, a.split = 1, a.replayMode = 0;
            b.target = 1, b.split = 1, b.replayMode = 0;
            viewSplit = 1;
        }
    } else if (!(a.split != 0 && b.split != 0)) {
        a.split = 0, a.replayMode = 2;
        b.split = 0, b.replayMode = 2;
        viewSplit = 0;
    }
    ViewDraw d;
    if (was) {
        d.splitPlayer1 = true;
        d.splitPlayer2 = viewSplit != 0;
    }
    d.full = viewSplit == 0;
    return d;
}

// What the renderer reads of one step for both views (frame_interp.h RenderSnapshot, per camera).
struct SplitSnapshot {
    bool valid = false;
    std::array<sim::CarPose, 2> poses{};
    std::array<sim::CarPose, 2> groundPoses{};
    std::array<std::array<sim::WheelVisual, 4>, 2> wheels{};
    std::array<camera::RaceCamera, 2> cameras{};
    SmokePool smoke;
    std::array<int32_t, 2> rpm{}, speedReadout{}, boost{};
    uint32_t clock = 0;
};

// The listener block of a camera object (camera + 0xB8 / 0xD8 / 0xEC / 0xF4 / 0x10B / 0x10A / 0x10C).
audio::Listener ListenerOf(const camera::RaceCamera& cam) {
    audio::Listener l;
    const camera::CameraProjection p = camera::ProjectionOf(cam);
    for (int k = 0; k < 3; k++) {
        l.position[k] = p.eye[k];
        l.velocity[k] = float(cam.velocity[k] / 65536.0);
        l.axisSide[k] = float(cam.backAxis[k]) / 4096.0f;
        l.axisRight[k] = float(cam.rightAxis[k]) / 4096.0f;
    }
    l.cameraMotion = true;
    l.cameraSpeed = cam.speed;
    for (int k = 0; k < 3; k++) {
        l.cameraDirection[k] = cam.direction[k];
        l.cameraPosition[k] = cam.view.t[k];
        l.cameraAxisSide[k] = cam.backAxis[k];
        l.cameraAxisRight[k] = cam.rightAxis[k];
    }
    l.inCarView = cam.external == 0;
    l.inCarViewAlt = cam.inCarSound != 0;
    l.focusedCar = cam.target;
    return l;
}

// The HUD's inputs of a split race frame (0x800293D4 in game mode 0), from the race and both camera objects.
struct SplitHudContext {
    uint8_t laps = 2;           // 0x801D586B
    bool replaying = false;     // 0x800A951C
    bool metric = false;
    bool courseMap = true;      // career + 0xB1
    bool tyrePanel = false;     // 0x8002DE8C's condition
    uint8_t counter = 0;        // 0x8002F864 (SplitHudCounter)
};
struct SplitHud {
    bool split = true;          // view + 0x2EA: 0x8002E908(car 0, camera 1), 0x8002E908(car 1, camera 2); else 0x8002E63C(car followed, camera 1)
    size_t followed = 0;        // camera 1 + 0x10C
    std::array<HudFrame, 2> frames; // car 0 / car 1 (each with its camera's + 0x107); the full view draws frames[followed] with camera 1's
};
// 0x8002E550 (the HUD tick of every race frame, after the step): 0x8002F864 += 1 while the race clock 0x80046F64 runs; 0 at the
// race's HUD setup (0x8002E390). The running times add (counter & 15) * 1000 / 900 ms.
void SplitHudCounter(const sim::RaceSim& race, uint8_t& counter) {
    if (race.RaceClock() != 0) counter = uint8_t(counter + 1);
}
SplitHud SplitHudOf(const sim::RaceSim& race, const std::array<const camera::RaceCamera*, 2>& cams, bool viewSplit, const SplitHudContext& c,
                    const gt2::HudStrings& strings, const SplitSnapshot* previous, float at) {
    SplitHud h;
    h.split = viewSplit;
    h.followed = std::min<size_t>(cams[0]->target, 1);
    const sim::RaceShellState& shell = race.Shell().State();
    const uint32_t clock = previous ? uint32_t(LerpInt(int32_t(previous->clock), int32_t(race.RaceClock()), at)) : race.RaceClock();
    for (size_t car = 0; car < 2; car++) {
        const sim::CarBody& body = race.CarAt(car).body;
        const sim::PlayerResults& results = shell.results[car];
        HudFrame& f = h.frames[car];
        f.gameMode = 0;
        f.faceSlot = int(car);                                     // car + 0x880
        f.replay = c.replaying;
        f.replayView = (viewSplit ? cams[car] : cams[0])->replayInfo; // camera + 0x107
        f.lapCount = c.laps;
        f.lap = body.lap;
        f.position = int(int8_t(body.racePosition));
        f.finished = body.finishFlag != 0;
        f.totalMs = f.finished ? results.finishTime : int32_t(clock / 3);
        f.lapMs = int32_t(clock / 3) - body.lastLineTime;
        f.subFrame = c.counter & 0xF;
        for (int i = 0; i < results.count && i < 10; i++) f.laps.push_back(results.laps[i].time);
        f.resultsLapNumber = results.lapNumber;
        f.bestLapMs = results.count > 0 ? results.best.time : -1;
        f.bestLapSpeed = results.count > 0 ? results.best.maxSpeed : 0;
        f.rpm = previous ? LerpInt(previous->rpm[car], body.engineRpm, at) : body.engineRpm;
        f.revLimitRpm = body.revLimitRpm;
        f.redlineRpm = body.upshiftRpm;
        f.speedReadout = previous ? LerpInt(previous->speedReadout[car], body.speedReadout, at) : body.speedReadout;
        f.gear = body.gear;
        f.clutchEngaged = body.clutchState == 1;
        f.turbo = body.boostCap;
        f.boost = previous ? LerpInt(previous->boost[car], body.intakeLoad, at) : body.intakeLoad;
        f.metric = c.metric;
        f.courseMap = c.courseMap;
        f.mapHighlight = 0; // 0x80029064's fifth argument
        for (size_t i = 0; i < 2 && i < race.CarCount(); i++) { // car + 0x832 / + 0x83A
            const sim::CarPose pose = previous ? InterpolatedPoseRounded(previous->poses[i], race.Pose(i), at) : race.Pose(i);
            f.mapCars.push_back({int16_t(pose.worldPosition[0] >> 16), int16_t(pose.worldPosition[2] >> 16), true});
        }
        f.startTimer = shell.startTimer;
        f.messageCode = body.messageCode;
        f.tyrePanel = c.tyrePanel;
        for (size_t w = 0; w < 4; w++) {
            f.wheelDamage[w] = body.wheels[w].damage;
            f.wheelWearStage[w] = body.wheels[w].wearStage;
        }
        f.timeInvalid = body.hudInvalid != 0;
        f.crashKind = body.hudMessageKind;
        f.captionTimer = body.hudTimer;
        f.splitTimer = body.hudTimer2;
        f.crashTimer = body.hudTimer3;
        f.captionMs = body.hudValue;
        f.caption = strings.At(body.hudLabel);
        f.splitA = uint32_t(body.hudCompare);
        f.splitB = uint32_t(body.hudGap);
    }
    return h;
}

// The fields of two HUD inputs that differ ("name ours/original"), empty when equal.
std::string DiffHud(const HudFrame& a, const HudFrame& b) {
    std::string d;
    auto field = [&](const char* name, long long x, long long y) {
        if (x == y) return;
        char t[96];
        std::snprintf(t, sizeof t, " %s %lld/%lld", name, x, y);
        d += t;
    };
    field("mode", a.gameMode, b.gameMode), field("face", a.faceSlot, b.faceSlot), field("replay", a.replay, b.replay), field("replayView", a.replayView, b.replayView);
    field("laps", a.lapCount, b.lapCount), field("lap", a.lap, b.lap), field("position", a.position, b.position), field("finished", a.finished, b.finished);
    field("total", a.totalMs, b.totalMs), field("lapMs", a.lapMs, b.lapMs), field("subFrame", a.subFrame, b.subFrame);
    field("lapCount", (long long)a.laps.size(), (long long)b.laps.size());
    for (size_t i = 0; i < a.laps.size() && i < b.laps.size(); i++) field("lapTime", a.laps[i], b.laps[i]);
    field("resultsLap", a.resultsLapNumber, b.resultsLapNumber);
    if (a.bestLapMs >= 0 || (b.bestLapMs >= 0 && b.bestLapMs != 0)) field("best", a.bestLapMs, b.bestLapMs), field("bestSpeed", a.bestLapSpeed, b.bestLapSpeed);
    field("rpm", a.rpm, b.rpm), field("limit", a.revLimitRpm, b.revLimitRpm), field("redline", a.redlineRpm, b.redlineRpm), field("speed", a.speedReadout, b.speedReadout);
    field("gear", a.gear, b.gear), field("clutch", a.clutchEngaged, b.clutchEngaged), field("turbo", a.turbo, b.turbo), field("boost", a.boost, b.boost);
    field("courseMap", a.courseMap, b.courseMap), field("mapCars", (long long)a.mapCars.size(), (long long)b.mapCars.size());
    for (size_t i = 0; i < a.mapCars.size() && i < b.mapCars.size(); i++) field("mapX", a.mapCars[i].x, b.mapCars[i].x), field("mapZ", a.mapCars[i].z, b.mapCars[i].z);
    field("start", a.startTimer, b.startTimer), field("message", a.messageCode, b.messageCode), field("tyres", a.tyrePanel, b.tyrePanel);
    for (size_t w = 0; w < 4; w++) field("damage", a.wheelDamage[w], b.wheelDamage[w]), field("wear", a.wheelWearStage[w], b.wheelWearStage[w]);
    field("invalid", a.timeInvalid, b.timeInvalid), field("crash", a.crashKind, b.crashKind), field("captionTimer", a.captionTimer, b.captionTimer);
    field("splitTimer", a.splitTimer, b.splitTimer), field("crashTimer", a.crashTimer, b.crashTimer);
    if (a.captionTimer > 0 || a.splitTimer > 0) {
        field("captionMs", a.captionMs, b.captionMs), field("splitA", a.splitA, b.splitA), field("splitB", a.splitB, b.splitB);
        if (a.caption != b.caption) d += " caption '" + a.caption + "'/'" + b.caption + "'";
    }
    return d;
}

} // namespace

SplitRaceResult RunSplitRace(GameWindow& window, Panels* panels, const DiscImage& disc, const GtfsVolume& vol, RaceData& data, const RaceOptions& raceOptions,
                             const SplitRaceConfig& splitConfig) {
    const RaceViewConfig& config = splitConfig.view;
    const ReplayFile* replay = splitConfig.replay;
    const bool replaying = replay != nullptr;
    VkSceneRenderer& renderer = window.Renderer();
    const std::string& trackName = data.trackName;
    SplitRaceResult result;
    if (data.params.size() < 2) throw std::runtime_error("2 player race: two cars are needed");
    if (replaying && (replay->stream.empty() || replay->stream2.empty())) throw std::runtime_error("2 player replay: both players' streams are needed");
    const size_t carCount = 2;
    if (replaying) data.constants.flag800A951C = 1; // the replay (arcade loop state 12 / the title's argument 1): 0x800A951C

    // Race music as the single race (0x800299D8): the intro raises the start hold.
    const std::vector<MusicTrack> musicTracks = ReadMusicTable(LoadExeImage(disc));
    uint32_t musicRandom = config.deterministic ? 0x1234u : uint32_t(std::chrono::steady_clock::now().time_since_epoch().count());
    audio::RaceMusicInputs musicInputs;
    musicInputs.gameMode = data.constants.gameMode;
    musicInputs.demoFlag = data.constants.flag800A951C;
    auto initRaceMusic = [&](sim::RaceShellState* state) {
        audio::RaceMusicBytes m;
        uint16_t holdInitial = 0, hold = raceOptions.countdown ? 1 : 0;
        audio::InitRaceMusic(m, musicRandom, musicInputs, musicTracks, holdInitial, hold);
        if (hold > 1) data.shell.introFields = hold;
        if (config.musicTrack >= 0) m.raceTrack = uint8_t(config.musicTrack);
        if (state) {
            state->musicRequestFlag = m.loop;
            state->musicRequest = m.request;
            state->musicRaceTrack = m.raceTrack;
            state->music2F0 = m.next;
            state->music2F1 = m.kind;
        }
        return m;
    };
    initRaceMusic(nullptr);
    const shell::PcSettings* settings = config.pcSettings;
    const bool haveOptions = settings && settings->haveCareerOptions;
    if (haveOptions) data.constants.viewMode = settings->options.chaseView;

    sim::RaceSim race;
    SetupRace(race, data, carCount, raceOptions);
    SplitPads splitPads;
    splitPads.pedalTable = PedalTable(disc);
    if (replaying) { // 0x800163B8(stream, 1, 0x4400) for both players at the race load
        splitPads.playback = true;
        splitPads.stream[0] = ReplayStream::FromBytes(replay->stream);
        splitPads.stream[0].Init(true, ReplayStream::kPlayerCapacity);
        splitPads.stream[1] = ReplayStream::FromBytes(replay->stream2);
        splitPads.stream[1].Init(true, ReplayStream::kPlayerCapacity);
    }
    race.SetPadSource(&SplitPads::Source, &splitPads);
    ShellLog shellLog;
    shellLog.Attach(race);
    std::printf("2 player %s: %s, %u lap(s), hold %u fields, cars %s / %s\n", replaying ? "replay" : "race", trackName.c_str(), raceOptions.laps, race.HoldFrames(),
                data.carIds[0].c_str(), data.carIds[1].c_str());
    if (replaying)
        std::printf("2 player replay: streams of %d / %d frames\n", ReplayStream::FromBytes(replay->stream).Frames(), ReplayStream::FromBytes(replay->stream2).Frames());

    // The controllers: pad objects of ports 1 and 2 with the career's pad blocks (the new game's without a career).
    const input::PadTables padTables = input::PadTables::Load(LoadExeImage(disc));
    std::array<PlayerPad, 2> players;
    {
        const career::CareerState fresh = career::NewCareer(career::ReadNewGameDefaults(disc));
        const uint8_t* c = reinterpret_cast<const uint8_t*>(&fresh);
        for (size_t p = 0; p < 2; p++) {
            const std::vector<uint8_t>& given = p == 0 ? splitConfig.padBlock1 : splitConfig.padBlock2;
            if (given.size() == input::pad_block::kSize) players[p].block = given;
            else players[p].block.assign(c + (p == 0 ? 0x0A : 0x5C), c + (p == 0 ? 0x0A : 0x5C) + input::pad_block::kSize);
            players[p].triggerPedals = config.triggerPedals;
            input::InitTracker(players[p].object.data() + input::pad_object::kTracker, padTables);
        }
    }

    // The scene: the course, both cars (and their copies for the second view: the reflection pass is written per camera).
    renderer.clearColor[0] = 0.45f; renderer.clearColor[1] = 0.58f; renderer.clearColor[2] = 0.78f;
    SceneAssets assets(renderer, vol);
    if (!assets.ReserveSecondView()) throw std::runtime_error("2 player race: the scene's second view ranges are taken");
    SponsorTable sponsorTable;
    std::vector<SponsorUpload> sponsors;
    try {
        const uint32_t seed = config.sponsorSeedGiven ? config.sponsorSeed : config.deterministic ? 0x14D57u : uint32_t(gt2::os::TickCountMs() / 17u);
        const CourseInfoTable info = ParseCourseInfo(vol.Read(".crsinfo"));
        const uint16_t flags = data.courseIndex >= 0 ? info.entries[size_t(data.courseIndex)].flags : uint16_t(0x40);
        sponsorTable = ParseSponsorTable(vol.Read(".crstims.tsd"));
        sponsors = PlaceSponsorBoards(sponsorTable, LoadSponsorSlots(LoadOverlayImage(disc, kRaceOverlayIndex)), config.sponsorCategory, seed, flags);
    } catch (const std::exception& e) {
        std::printf("sponsor boards: none (%s)\n", e.what());
        sponsors.clear();
    }
    assets.UseTrack(trackName, data.track, -1, &sponsors);
    std::array<std::array<int, 2>, 2> slots{}; // [view][car]
    {
        WheelTemplates wheelTemplates = GeneratedWheelTemplates();
        std::array<int16_t, 4> dishes = kWheelDishDepths;
        try {
            const GuestImage exe = LoadExeImage(disc);
            wheelTemplates = LoadWheelTemplates(exe);
            dishes = LoadWheelDishDepths(exe);
        } catch (const std::exception& e) {
            std::printf("wheels: the executable's templates are not available (%s)\n", e.what());
        }
        std::optional<menu::MenuWheelFiles> wheelFiles;
        try {
            wheelFiles = menu::LoadMenuWheelFiles(vol, LoadExeImage(disc));
        } catch (const std::exception& e) {
            std::printf("wheels: fitted wheels not loaded (%s)\n", e.what());
        }
        for (size_t car = 0; car < carCount; car++) {
            const sim::CarBody& body = race.CarAt(car).body;
            const int16_t dish = WheelDishDepth(car < data.configs.size() ? data.configs[car].word00 : 0, dishes);
            std::array<WheelAxleDims, 2> dims;
            for (size_t axle = 0; axle < 2; axle++) dims[axle] = RaceWheelDims(body.wheelRadius[axle], body.rimRadius[axle], body.tyreHeight[axle], dish);
            const WheelArea area = GenerateWheelArea(dims, wheelTemplates);
            for (int view = 0; view < 2; view++) {
                // a car of each player, and its copy per view (the same model as the other player's: its own slot for its own entry)
                const std::string alias = "2p:" + std::to_string(view) + ":" + std::to_string(car);
                slots[size_t(view)][car] = assets.UseCar(data.carIds[car], alias, &area);
                if (slots[size_t(view)][car] < 0) throw std::runtime_error("2 player race: no free car slot in the scene");
                if (wheelFiles && car < data.configs.size()) {
                    const int file = menu::MenuWheelFile(*wheelFiles, data.configs[car].word00);
                    if (file >= 0) {
                        const menu::MenuWheelTexture wheel = menu::LoadMenuWheelTexture(vol, wheelFiles->paths[size_t(file)]);
                        assets.SetCarWheelTexture(slots[size_t(view)][car], wheel.image, wheel.words, wheel.rows, wheel.clut);
                    }
                }
            }
        }
    }
    // The wheels' z in car space (the .cdo wheel x of 0x800133F0's record): the smoke's spawn points and the wheel transforms.
    std::vector<std::array<int16_t, 4>> wheelZ(carCount, std::array<int16_t, 4>{});
    for (size_t car = 0; car < carCount; car++)
        for (size_t w = 0; w < 4; w++) wheelZ[car][w] = assets.SlotModel(slots[0][car]).wheels[w].x;
    bool particles = config.particles;
    SmokePool smoke;
    if (particles) {
        try {
            smoke.LoadFadeScale(LoadOverlayImage(disc, kRaceOverlayIndex));
        } catch (const std::exception& e) {
            std::printf("tyre smoke: disabled (%s)\n", e.what());
            particles = false;
        }
    }
    std::unique_ptr<Hud> hud;
    if (config.hud) {
        try {
            hud = std::make_unique<Hud>(renderer, vol, LoadExeImage(disc), LoadOverlayImage(disc, kRaceOverlayIndex));
            hud->UseCourse(trackName);
        } catch (const std::exception& e) {
            std::printf("hud: disabled (%s)\n", e.what());
        }
    }
    if (panels) panels->Upload();

    // Sound: both players' cars as player cars.
    std::unique_ptr<audio::MusicPlayer> musicPlayer;
    std::unique_ptr<audio::RaceAudio> raceAudio;
    if (config.sound && config.shotPath.empty()) {
        try {
            std::vector<audio::CarSoundSetup> setups(data.sound.begin(), data.sound.begin() + std::ptrdiff_t(carCount));
            for (size_t i = 0; i < setups.size(); i++) setups[i].controlClass = raceOptions.aiPlayer ? 0 : 2;
            raceAudio = std::make_unique<audio::RaceAudio>();
            raceAudio->Load(vol, LoadExeImage(disc), LoadOverlayImage(disc, kRaceOverlayIndex), setups);
            std::string error;
            if (!raceAudio->OpenDevice(error)) { std::printf("sound: no output device (%s); running silently\n", error.c_str()); raceAudio.reset(); }
        } catch (const std::exception& e) {
            std::printf("sound: disabled (%s)\n", e.what());
            raceAudio.reset();
        }
    }
    if (raceAudio && !config.reverb) raceAudio->SetReverbEnabled(false);
    if (raceAudio && settings && settings->haveCareerOptions) raceAudio->SetMasterVolume(settings->options.sfxVolume);
    if (raceAudio && config.music) {
        try {
            musicPlayer = std::make_unique<audio::MusicPlayer>();
            musicPlayer->Open(config.discPath, LoadExeImage(disc));
            musicPlayer->SetVolume(settings && settings->haveCareerOptions ? settings->options.musicVolume : uint8_t(0xF0));
            raceAudio->AttachMusic(musicPlayer.get());
        } catch (const std::exception& e) {
            std::printf("music: disabled (%s)\n", e.what());
            musicPlayer.reset();
        }
    }
    audio::RaceMusicBytes raceMusic = initRaceMusic(&race.Shell().State());
    auto stepMusic = [&] {
        sim::RaceShellState& s = race.Shell().State();
        raceMusic.loop = s.musicRequestFlag;
        raceMusic.request = s.musicRequest;
        raceMusic.raceTrack = s.musicRaceTrack;
        raceMusic.next = s.music2F0;
        raceMusic.kind = s.music2F1;
        audio::UpdateRaceMusic(raceMusic, musicPlayer.get());
        s.musicRequestFlag = raceMusic.loop;
        s.musicRequest = raceMusic.request;
        s.music2F0 = raceMusic.next;
    };
    shellLog.audio = raceAudio.get();

    // The two cameras (0x80010000(camera, player) with + 0x103 = 1 at the race load of mode 0); a replay: the replay cameras.
    const camera::CameraConstants cameraConstants = camera::LoadCameraConstants(LoadOverlayImage(disc, kRaceOverlayIndex));
    const std::vector<uint8_t> cameraTro = vol.Read("crsobj/" + trackName + ".tro");
    const camera::ReplayCameraData replayCameras = camera::ReplayCamerasOfTro(cameraTro);
    std::array<camera::GameCamera, 2> cameras;
    {
        std::vector<camera::CarShape> shapes;
        for (size_t i = 0; i < carCount; i++) shapes.push_back(camera::CarShapeOf(ParseCarModel(vol.Read("carobj/" + data.carIds[i] + ".cdo"))));
        for (size_t p = 0; p < 2; p++) {
            camera::GameCamera::Options o;
            if (haveOptions) {
                o.cameraPosition = settings->options.cameraPosition;
                o.viewAngle = settings->options.viewAngle;
                o.replayInfo = settings->options.replayInfo;
            }
            if (config.cameraPosition >= 0) o.cameraPosition = uint8_t(config.cameraPosition);
            if (config.viewAngle >= 0) o.viewAngle = uint8_t(config.viewAngle);
            if (o.cameraPosition > 2) o.cameraPosition = 0;
            if (o.viewAngle > 2) o.viewAngle = 1;
            o.gameMode = data.constants.gameMode;
            o.replay = replaying ? 1 : 0;
            o.player = uint8_t(p);
            o.split = true;
            cameras[p].Setup(cameraConstants, data.track, replayCameras, shapes, o);
            cameras[p].Start(race);
        }
    }
    uint8_t viewSplit = 0; // view + 0x2EA (the race task's view object; 0x800292A0 turns it on at the first frame)
    uint8_t hudCounter = 0; // 0x8002F864 (SplitHudCounter)
    ViewDraw viewDraw = SplitViewFrame(cameras[0].Camera(), cameras[1].Camera(), viewSplit);
    if (raceAudio) raceAudio->PrimeTwoPlayer(race, viewSplit != 0);

    shell::GraphicsSettings graphics = CurrentGraphics();
    renderer.SetOptions(RenderOptionsOf(graphics));
    const bool deterministicRun = config.deterministic || !config.shotPath.empty();
    bool highRate = graphics.frameRate == shell::GraphicsSettings::kFrameRateDisplay && !deterministicRun && window.Pacing();
    bool interpolating = highRate || config.interpAlpha >= 0;
    std::printf("graphics: %s; 2 player presentation %s\n", DescribeGraphics(graphics).c_str(), highRate ? "at the display rate, interpolated" : "frame-locked to the fields");
    SplitSnapshot previous; // the render state before the last step
    FrameLog frameLog(config.frameLog);
    gt2::raceui::PauseMenu pauseMenu;
    bool paused = false;
    std::array<bool, 2> cameraPressed{};
    std::array<uint32_t, 2> replayButtons{}; // 0x800109FC's buttons per camera since the last step (camera + 0x6C)
    uint32_t enterForShell = 0;
    int steps = 0, shiftRequest = 0;
    std::vector<sim::PadRecord> pads(race.CarCount());
    bool finishLogged = false, replayEndLogged = false;
    using Clock = std::chrono::steady_clock;
    const double stepSeconds = data.constants.step.frameTime / 65536.0; // 1/30 s
    auto stepAnchor = Clock::now();
    auto nextPresent = stepAnchor;
    double buildEstimate = 0.003;
    auto seconds = [](double s) { return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s)); };
    window.ResetPacing();
    for (int frame = 1;; frame++) {
        const int fieldBeforeOverlay = window.Field();
        const uint64_t clockBeforePump = window.ClockRevision();
        if (!window.BeginFrame()) {
            result.exit = RaceExit::kClosed;
            break;
        }
        if (window.Field() != fieldBeforeOverlay || window.ClockRevision() != clockBeforePump) nextPresent = stepAnchor = Clock::now();
        if (!(graphics == CurrentGraphics())) {
            graphics = CurrentGraphics();
            renderer.SetOptions(RenderOptionsOf(graphics));
            highRate = graphics.frameRate == shell::GraphicsSettings::kFrameRateDisplay && !deterministicRun && window.Pacing();
            interpolating = highRate || config.interpAlpha >= 0;
        }
        const auto telemetry = race.Telemetry(0);
        const int strength = AdaptivePedalStrength();
        const bool effects = !paused && !replaying && config.triggerPedals && strength > 0;
        window.Input().SetPedalResistance(uint8_t(effects ? std::max(1, 3 * strength / 100) : 0),
            uint8_t(effects ? std::max(1, (3 + std::clamp(int(telemetry.brake) * 3 / 4096, 0, 3)) * strength / 100) : 0));
        const auto fieldTime = window.NextField(); // this field's scheduled start (the display frame rate's time base)
        // 0x8007FC30 for both ports.
        for (size_t p = 0; p < 2; p++) {
            const input::Ps1PadFrame& f = p == 0 ? window.Pad() : window.Pad2();
            const input::Actuators act = input::PollPad(players[p].object.data(), f, padTables, players[p].block.data() + input::pad_block::kCalibration,
                                                        p == 0 && window.Input().Port1HasMotors() ? 2 : 0);
            if (p == 0) window.Input().SetActuators(paused ? input::Actuators{} : act);
        }
        bool leave = false;
        const bool enter = window.Pressed(gt2::keys::kReturn) || window.Pad2Pressed(input::ps1::kCross);
        const bool startPressed = window.KeyPressed(gt2::keys::kEscape) || window.PadPressed(input::ps1::kStart);
        if (replaying && splitPads.ended) { // the stream ran out (0x800A8D68): the replay is over
            if (!replayEndLogged) std::printf("2 player replay: end of a stream after %d steps\n", steps);
            replayEndLogged = true;
            if (splitConfig.replayEndLeaves) {
                result.exit = RaceExit::kFinished;
                break;
            }
        }
        if (paused) {
            const int chosen = startPressed ? 0 : pauseMenu.Update(window.Pressed(gt2::keys::kUp), window.Pressed(gt2::keys::kDown), enter);
            if (chosen == 0) paused = false;
            else if (chosen == 1) {
                result.exit = RaceExit::kExited;
                std::printf("2 player %s: exit from the pause menu\n", replaying ? "replay" : "race");
                leave = true;
            }
        } else {
            // Player 1's Start pauses (the Arcade build's race frame 0x80015B64 reads pad 1 only); a title replay (argument 1)
            // leaves at once (0x80015C90).
            if (startPressed) {
                if (panels && !splitConfig.startLeaves) {
                    paused = true;
                    pauseMenu = gt2::raceui::PauseMenu{};
                    pauseMenu.Open();
                } else {
                    result.exit = RaceExit::kQuit;
                    leave = true;
                }
            }
            if (window.KeyPressed('C')) cameraPressed[0] = true;
            if (window.KeyPressed('Q')) shiftRequest = 1;
            if (window.KeyPressed('A')) shiftRequest = -1;
            if (enter && !replaying) enterForShell = 0x200; // X: the shell's results wait (0x8002A700)
            if (replaying) { // the replay controls of camera 1 as keys (race_view.cpp): O onboard, I Replay Info, Up / Down, T = Triangle (the split)
                if (window.Pressed('O')) replayButtons[0] |= 0x400;
                if (window.Pressed('I')) replayButtons[0] |= 0x200;
                if (window.Pressed(gt2::keys::kUp)) replayButtons[0] |= 0x1;
                if (window.Pressed(gt2::keys::kDown)) replayButtons[0] |= 0x2;
                if (window.Pressed('T')) replayButtons[0] |= 0x100;
            }
        }
        if (leave) break;
        if (raceAudio) raceAudio->SetPaused(paused);
        if (paused) for (PlayerPad& pl : players) input::SnapshotTracker(pl.object.data() + input::pad_object::kTracker, pl.scratch.data());

        // One step on every second field (frame-locked, reproducible; with the display frame rate the paced field clock).
        const bool stepping = !paused && frame % 2 == 0 && !(replaying && splitPads.ended);
        if (stepping) {
            std::array<uint32_t, 2> held{};
            for (size_t p = 0; p < 2; p++) {
                PlayerPad& pl = players[p];
                const input::Ps1PadFrame& f = p == 0 ? window.Pad() : window.Pad2();
                const auto tables = pl.TablesFor(f);
                input::BuildRaceLogical(pl.object.data(), tables.data());
                held[p] = pl.U32(input::pad_object::kLogical);
                const uint32_t pressed = pl.U32(input::pad_object::kLogical + 4), genericPressed = pl.U32(input::pad_object::kSnapshot + 4);
                LogicalPad merged;
                merged.buttons = held[p] & 0x4FFu; // driving functions 0..7 and the stick curve (0x400)
                merged.analog = uint16_t(pl.U16(input::pad_object::kAnalogFlags) & 0xD);
                merged.steerAxis = pl.U16(input::pad_object::kValues + 0);
                merged.throttle = pl.U16(input::pad_object::kValues + 4);
                merged.brake = pl.U16(input::pad_object::kValues + 6);
                if (p == 0) { // player 1's keyboard (the single race's keys) merged at the logical level
                    LogicalPad k;
                    if (window.KeyHeld(gt2::keys::kLeft)) k.buttons |= kPadLeft;
                    if (window.KeyHeld(gt2::keys::kRight)) k.buttons |= kPadRight;
                    if (window.KeyHeld(gt2::keys::kUp) || (config.drive && !config.shotPath.empty())) k.buttons |= kPadThrottle;
                    if (window.KeyHeld(gt2::keys::kDown)) k.buttons |= kPadBrake;
                    if (window.KeyHeld(gt2::keys::kSpace)) k.buttons |= kPadHandbrake;
                    if (window.KeyHeld('R')) k.buttons |= kPadReverse;
                    if (shiftRequest > 0) k.buttons |= kPadShiftUp;
                    if (shiftRequest < 0) k.buttons |= kPadShiftDown;
                    merged.buttons |= k.buttons;
                    if (k.buttons & (kPadLeft | kPadRight)) merged.analog &= 0xFFFEu;
                    if (k.buttons & kPadThrottle) merged.analog &= 0xFFFBu;
                    if (k.buttons & kPadBrake) merged.analog &= 0xFFF7u;
                }
                splitPads.pad[p] = merged;
                if (!replaying && (pressed & camera::kButtonView)) cameraPressed[p] = true; // the view button (not read in replays)
                // the replay controls read each camera's generic pad (0x800109FC with pad + 0x6C): Circle / Square / Cross,
                // Up / Down, Triangle (game mode 0: the split), L1 / R1
                if (replaying) replayButtons[p] |= genericPressed & (0xE00u | 0x100u | 0x1010u | 0x3u);
            }
            if (interpolating) { // what the renderer reads of the state before this step (copies; frame_interp.h)
                previous.valid = true;
                for (size_t i = 0; i < 2; i++) {
                    previous.poses[i] = race.Pose(i);
                    previous.groundPoses[i] = race.VisualPose(i);
                    previous.wheels[i] = race.Wheels(i);
                    previous.cameras[i] = cameras[i].Camera();
                    const sim::CarBody& b = race.CarAt(i).body;
                    previous.rpm[i] = b.engineRpm;
                    previous.speedReadout[i] = b.speedReadout;
                    previous.boost[i] = b.intakeLoad;
                }
                previous.smoke = smoke;
                previous.clock = race.RaceClock();
            }
            if (particles) smoke.Update();
            const uint32_t buttons[2] = {enterForShell, enterForShell};
            race.Step(pads.data(), buttons);
            SplitHudCounter(race, hudCounter);
            enterForShell = 0;
            shiftRequest = 0;
            for (size_t p = 0; p < 2; p++) { // 0x800100F4 for player 1 and (mode 0) player 2
                camera::CameraPad cameraPad;
                if (cameraPressed[p]) {
                    cameraPad.pressed |= camera::kButtonView;
                    if (replaying && p == 0) cameraPad.replayPressed |= 0x800; // C in a replay: the replay camera mode
                }
                cameraPad.replayPressed |= replayButtons[p];
                replayButtons[p] = 0;
                if (!replaying && ((held[p] & camera::kButtonLookBack) || (p == 0 && (window.KeyHeld('V') || config.lookBack)))) cameraPad.held |= camera::kButtonLookBack;
                cameras[p].Update(race, cameraPad);
                cameraPressed[p] = false;
            }
            // the frame's renderer (0x800292A0): the split flag after the cameras' update
            viewDraw = SplitViewFrame(cameras[0].Camera(), cameras[1].Camera(), viewSplit);
            if (particles)
                for (size_t i = 0; i < carCount; i++) smoke.SpawnFromCar(SmokeInputOf(race, i, wheelZ[i]));
            {
                const sim::Car& car = race.CarAt(0); // the motors of player 1's controller (port 1)
                input::VibrationSource v;
                v.padSlot = car.padSlot;
                v.vibrationOff = players[0].block[input::pad_block::kVibrationOff];
                v.finishTime = car.finishTime;
                v.word760 = car.body.word760;
                v.loadLevel = car.body.loadSoundLevel;
                v.impactFlag = car.body.impactSoundFlag;
                if (!replaying) input::FeedVibration(players[0].object.data(), v);
            }
            steps++;
            stepAnchor = fieldTime;
            // the frame's sound (0x80015DF4, before the frame's draw: the previous draw's view values): split - each player's car against its
            // camera (0x80014ED0), else every car against player 1's camera (0x80014E6C)
            if (raceAudio)
                raceAudio->StepTwoPlayer(race, {ListenerOf(cameras[0].Camera()), ListenerOf(cameras[1].Camera())}, viewSplit != 0,
                                         data.constants.flag800A951C != 0);
            stepMusic();
            if (!finishLogged && !replaying && (race.CarAt(0).body.finishFlag != 0 || race.CarAt(1).body.finishFlag != 0)) {
                PrintStandings(race, shellLog, data.carIds);
                finishLogged = true;
            }
            if (!replaying && race.RaceTaskOver()) { // 0x8002A700 after a finish and the results wait
                const sim::RaceShellState& s = race.Shell().State();
                result.finished = true;
                result.exit = RaceExit::kFinished;
                for (size_t p = 0; p < 2; p++) {
                    result.results[p] = s.results[p];
                    result.places[p] = s.results[p].position;
                }
                std::printf("2 player race: over after %d steps, places %d / %d\n", steps, result.places[0], result.places[1]);
                break;
            }
        }

        // ---- the frame: the views, the HUD, the overlay screens; `alpha` = how far between the state before the last step and
        // the last step's it shows (-1: the last step as it is, the original's presentation)
        std::vector<DrawItem> items;
        size_t sceneCount = 0;
        auto buildFrame = [&](double alpha) {
            items.clear();
            const bool interp = alpha >= 0 && previous.valid;
            const float at = float(std::clamp(alpha, 0.0, 1.0));
            auto appendView = [&](int view, const float* rect) {
                const camera::RaceCamera& cam = cameras[size_t(view)].Camera();
                const size_t first = items.size();
                assets.SelectView(view);
                const bool cameraInterp = interp && !CameraCut(previous.cameras[size_t(view)], cam);
                const camera::CameraProjection p = cameraInterp ? InterpolatedProjection(previous.cameras[size_t(view)], cam, at) : camera::ProjectionOf(cam);
                const float viewAspect = renderer.AspectRatio() * (rect[2] - rect[0]) / (rect[3] - rect[1]);
                float vp[16];
                if (cameraInterp) ClipMatrixOf(p, viewAspect, 0.1f, vp);
                else camera::ClipMatrix(cam, viewAspect, 0.1f, vp);
                const float sy = rect[3] - rect[1], oy = rect[1] + rect[3] - 1.0f; // NDC of the view -> its rectangle of the window
                for (int col = 0; col < 4; col++) vp[col * 4 + 1] = sy * vp[col * 4 + 1] + oy * vp[col * 4 + 3];
                const std::array<std::array<float, 3>, 3> cameraAxes = {{{p.right[0], p.right[1], p.right[2]}, {-p.up[0], -p.up[1], -p.up[2]}, {p.forward[0], p.forward[1], p.forward[2]}}};
                float backdropModel[16], backdropMvp[16];
                SceneAssets::BackdropModel(p.eye, backdropModel);
                Multiply(vp, backdropModel, backdropMvp);
                std::vector<DrawItem> blended;
                assets.AppendBackdropItems(items, blended, backdropMvp);
                SceneAssets::TrackView tv;
                tv.eye = {p.eye[0], p.eye[1], p.eye[2]};
                tv.right = {p.right[0], p.right[1], p.right[2]};
                tv.forward = {p.forward[0], p.forward[1], p.forward[2]};
                tv.projectionDistance = p.H;
                tv.fullDetail = !config.raceDetail;
                tv.maxDetail = config.maxDetail || graphics.maxDetail;
                tv.cameraChunk = cam.chunk;
                tv.extendedDistance = float(graphics.drawDistance);
                assets.AppendTrackItems(items, blended, vp, tv);
                for (size_t car = 0; car < carCount; car++) {
                    if (cam.hideTarget && car == cam.target) continue; // the driver view (0x800140A4: camera + 0x108)
                    const int slot = slots[size_t(view)][car];
                    const bool blend = interp && !PoseJump(previous.poses[car], race.Pose(car));
                    float model[16], mvp[16];
                    if (blend) InterpolatedModelMatrix(previous.poses[car], race.Pose(car), at, model);
                    else ModelMatrix(race.Pose(car), model);
                    Multiply(vp, model, mvp);
                    assets.UpdateCarReflection(slot, model, cameraAxes);
                    // the wheels (0x800140A4 / 0x800670F0: race_view.cpp's rule): steer / travel / rolling angle between the steps
                    std::array<std::array<float, 16>, 4> wheelModels{};
                    const sim::CarBody& body = race.CarAt(car).body;
                    const std::array<sim::WheelVisual, 4> now = race.Wheels(car);
                    for (size_t w = 0; w < 4; w++) {
                        sim::WheelVisual v = now[w];
                        if (blend) {
                            const sim::WheelVisual& pw = previous.wheels[car][w];
                            v.steerAngle = int16_t(LerpInt(pw.steerAngle, now[w].steerAngle, at));
                            v.verticalOffset = int16_t(LerpInt(pw.verticalOffset, now[w].verticalOffset, at));
                            const int32_t turn = int32_t(int16_t(uint16_t((now[w].rotation - pw.rotation) << 4))) >> 4; // wrapped to -2048..2047
                            v.rotation = uint16_t(int32_t(pw.rotation) + LerpInt(0, turn, at));
                        }
                        const int16_t halfTrack = body.halfTrack[w >> 1];
                        const float centre[3] = {float((w & 1) ? halfTrack : -halfTrack) / 4096.0f, float(v.verticalOffset) / 4096.0f, float(wheelZ[car][w]) / 4096.0f};
                        WheelModelMatrix(int(w), centre, v.steerAngle, body.camber[w >> 1], v.rotation, wheelModels[w].data());
                    }
                    float ground[16], shadowMvp[16];
                    const auto groundPose = race.VisualPose(car);
                    if (interp && !PoseJump(previous.groundPoses[car], groundPose))
                        InterpolatedModelMatrix(previous.groundPoses[car], groundPose, at, ground);
                    else ModelMatrix(groundPose, ground);
                    GroundShadowMatrix(ground, assets.SlotShadowHeight(slot));
                    Multiply(vp, ground, shadowMvp);
                    assets.AppendCarItems(items, slot, mvp, car < data.paints.size() ? data.paints[car] : 0u, 0, true, &wheelModels, shadowMvp);
                }
                if (particles) {
                    const float right[3] = {p.right[0], p.right[1], p.right[2]}, up[3] = {p.up[0], p.up[1], p.up[2]}, fwd[3] = {p.forward[0], p.forward[1], p.forward[2]};
                    if (interp) assets.AppendSmokeItems(items, blended, InterpolatedSmoke(previous.smoke, smoke, at), vp, p.eye, right, up, fwd, p.H);
                    else assets.AppendSmokeItems(items, blended, smoke, vp, p.eye, right, up, fwd, p.H);
                }
                items.insert(items.end(), blended.begin(), blended.end());
                for (size_t k = first; k < items.size(); k++) std::copy(rect, rect + 4, items[k].scissor);
            };
            if (viewDraw.splitPlayer1) appendView(0, kViewRect[0]);
            if (viewDraw.splitPlayer2) appendView(1, kViewRect[1]);
            if (viewDraw.full) appendView(0, kFullRect);
            assets.SelectView(0);
            std::copy(assets.SkyColor().begin(), assets.SkyColor().end(), renderer.clearColor);
            sceneCount = items.size();
            if (hud) { // 0x800293D4 in game mode 0: split, 0x8002E908 for car 0 and car 1 (Hud::Build2P); else 0x8002E63C for the car
                       // camera 1 follows (Hud::Build2PFull; captures work/play/arcade_2p/replay1 c_16420 / c_16440 / c_16520)
                SplitHudContext hc;
                hc.laps = raceOptions.laps;
                hc.replaying = replaying;
                hc.metric = OverlayMetricUnits(config.metric);
                hc.courseMap = !haveOptions || settings->options.courseMap;
                hc.tyrePanel = data.constants.wear.wearLimit != 0 || data.constants.shellControlClass == 2;
                hc.counter = hudCounter;
                const SplitHud h = SplitHudOf(race, {&cameras[0].Camera(), &cameras[1].Camera()}, viewSplit != 0, hc, hud->Strings(), interp ? &previous : nullptr, at);
                if (h.split) hud->Build2P(h.frames[0], h.frames[1], renderer.AspectRatio(), items);
                else hud->Build2PFull(h.frames[h.followed], race.CarAt(0).body.revLimitRpm, race.CarAt(1).body.revLimitRpm, renderer.AspectRatio(), items);
            }
            if (panels) {
                panels->Clear();
                if (paused) {
                    panels->Overlay(gt2::raceui::BuildPauseFrame(panels->OverlayAssets(), pauseMenu)); // 0x80029E80
                } else if (!replaying && race.Shell().State().endTimer >= 0) { // 0x8002B170 of mode 0 over both views
                    gt2::raceui::RaceEndState s;
                    const sim::RaceShellState& shell = race.Shell().State();
                    s.subMode = 0;
                    s.timer = shell.endTimer;
                    s.auxTimer = shell.endTimerAux;
                    s.lapCount = int(raceOptions.laps);
                    for (size_t k = 0; k < race.CarCount(); k++) {
                        const int car = race.RaceOrder()[k];
                        if (car < 0 || size_t(car) >= race.CarCount()) continue;
                        gt2::raceui::RaceEndRow r;
                        r.name = size_t(car) < 2 && !splitConfig.names[size_t(car)].empty() ? splitConfig.names[size_t(car)] : data.carIds[size_t(car)];
                        r.car = car;
                        r.player = car == 0;
                        r.finishTime = race.CarAt(size_t(car)).finishTime;
                        r.finished = race.CarAt(size_t(car)).body.finishFlag != 0;
                        r.laps = race.CarAt(size_t(car)).body.lap;
                        s.rows.push_back(r);
                    }
                    panels->Overlay(gt2::raceui::BuildRaceEndFrame(panels->OverlayAssets(), s));
                }
                panels->Append(renderer.AspectRatio(), items);
            }
        };
        if (panels) panels->SoundFrame(); // the panels' sound tick: once per field
        auto alphaAt = [&](Clock::time_point t) { return std::clamp(std::chrono::duration<double>(t - stepAnchor).count() / stepSeconds, 0.0, 1.0); };
        double fieldAlpha = -1;
        const bool moving = !paused && !(replaying && splitPads.ended);
        if (config.interpAlpha >= 0) fieldAlpha = config.interpAlpha;
        else if (highRate && moving) fieldAlpha = alphaAt(Clock::now() + seconds(buildEstimate));
        buildFrame(fieldAlpha);
        const bool capture = !config.shotPath.empty() && frame == config.shotFrames;
        if (!highRate) {
            window.EndFrame(items, capture ? config.shotPath : std::string(), std::chrono::nanoseconds(16'666'667), sceneCount);
            if (!config.frameLog.empty()) frameLog.Presented(false, fieldAlpha, steps);
        } else {
            // The display frame rate (race_view.cpp's scheme, docs/formats/modern_graphics.md 2): the field only advances; the
            // frames are presented at the display's blanks (vsync) or the cap's period, each drawing its present time's state.
            window.SkipFrame(std::chrono::nanoseconds(16'666'667), std::chrono::nanoseconds(66'666'667));
            const auto capPeriod = graphics.frameCap > 0 ? std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / graphics.frameCap))
                                                         : Clock::duration::zero();
            const int presentMode = int(renderer.PresentMode()); // VkPresentModeKHR: 2 FIFO, 3 FIFO_RELAXED
            Clock::time_point vblank;
            Clock::duration refresh{};
            const bool vsyncPaced = (presentMode == 2 || presentMode == 3) && window.VBlankTiming(vblank, refresh);
            bool fresh = true;
            for (;;) {
                const auto t = Clock::now();
                Clock::time_point slot = std::max(t, nextPresent);
                if (vsyncPaced) {
                    const auto earliest = std::max(t + seconds(buildEstimate) + std::chrono::microseconds(500), nextPresent);
                    const auto blanks = (earliest - vblank + refresh - Clock::duration(1)) / refresh;
                    slot = vblank + refresh * std::max<long long>(blanks, 0);
                }
                const bool rebuild = moving && (!fresh || slot > t);
                if (window.NextField() <= slot - (vsyncPaced ? seconds(buildEstimate) : Clock::duration::zero())) break;
                double a = fieldAlpha;
                const auto buildStart = rebuild ? std::max(Clock::now(), slot - seconds(buildEstimate)) : Clock::now();
                window.SleepUntil(buildStart);
                if (rebuild) {
                    a = alphaAt(std::max(slot, buildStart + seconds(buildEstimate)));
                    buildFrame(a);
                    const double built = std::min(std::chrono::duration<double>(Clock::now() - buildStart).count(), 0.0055);
                    buildEstimate = built > buildEstimate ? built : buildEstimate * 0.9 + built * 0.1;
                    if (!vsyncPaced) window.SleepUntil(slot);
                }
                const auto presentStart = Clock::now();
                window.Present(items, sceneCount);
                const auto after = Clock::now();
                frameLog.Presented(!fresh, a, steps, std::chrono::duration<double, std::milli>(presentStart - buildStart).count(),
                                   std::chrono::duration<double, std::milli>(after - presentStart).count());
                if (vsyncPaced) nextPresent = slot + std::max(refresh / 2, capPeriod - refresh / 2);
                else nextPresent = capPeriod > Clock::duration::zero() ? std::max(slot + capPeriod, after - capPeriod / 2) : after;
                fresh = false;
                if (!moving) break;
            }
            window.SleepUntil(window.NextField());
        }
        if (capture) {
            std::printf("wrote %s after %d steps\n", config.shotPath.c_str(), steps);
            result.exit = RaceExit::kShot;
            break;
        }
        if (frame % 60 == 0 && !highRate) {
            char title[200];
            std::snprintf(title, sizeof title, "gt2game - 2 player %s - %s - P1 lap %d P%u %.0f km/h | P2 lap %d P%u %.0f km/h", replaying ? "replay" : "Battle",
                          trackName.c_str(), race.CarAt(0).body.lap, race.CarAt(0).body.racePosition, race.Telemetry(0).forwardSpeed / 4096.0 * 3.6,
                          race.CarAt(1).body.lap, race.CarAt(1).body.racePosition, race.Telemetry(1).forwardSpeed / 4096.0 * 3.6);
            window.SetTitle(title);
        }
    }
    result.steps = steps;
    if (!replaying)
        for (size_t p = 0; p < 2; p++) { // 0x800167D0: the pending run flushed (what a replay / Save Replay takes)
            ReplayStream s = splitPads.stream[p];
            s.End(false);
            result.streams[p] = s.Bytes();
        }
    return result;
}

int FramesCompareSplitReplay(const RaceData& data, const RaceOptions& options, const std::string& capturePath, const ReplayFile& replay,
                             const std::array<uint16_t, 16>& pedalTable, int maxFrames) {
    const std::vector<RaceCaptureFrame> all = ReadRaceCapture(capturePath);
    if (all.empty()) throw std::runtime_error(capturePath + ": no frames");
    if (replay.stream.empty() || replay.stream2.empty()) throw std::runtime_error("frames: the replay has no second player's stream (game mode 0)");
    std::vector<const RaceCaptureFrame*> frames;
    for (const RaceCaptureFrame& f : all)
        if (f.race == all[0].race) frames.push_back(&f);
    RaceData d = data;
    d.constants.flag800A951C = 1;
    sim::RaceSim race;
    const size_t carCount = std::min<size_t>(2, d.params.size());
    SetupRace(race, d, carCount, options);
    SplitPads pads;
    pads.pedalTable = pedalTable;
    pads.playback = true;
    pads.stream[0] = ReplayStream::FromBytes(replay.stream);
    pads.stream[0].Init(true, ReplayStream::kPlayerCapacity);
    pads.stream[1] = ReplayStream::FromBytes(replay.stream2);
    pads.stream[1].Init(true, ReplayStream::kPlayerCapacity);
    race.SetPadSource(&SplitPads::Source, &pads);
    const RaceCaptureFrame& first = *frames[0];
    std::printf("frames (2 player replay): %zu captured race frames (fields %u..%u, game mode %u, demo %u); hold at the start: original %u, ours %u\n", frames.size(),
                first.field, frames.back()->field, first.gameMode, first.demo, first.hold, race.HoldFrames());
    constexpr size_t kBody = offsetof(sim::Car, body);
    int differing = 0, shown = 0;
    size_t compared = 0;
    const size_t count = maxFrames > 0 ? std::min(frames.size(), size_t(maxFrames)) : frames.size();
    auto word = [](const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; };
    for (size_t k = 0; k < count; k++) {
        const RaceCaptureFrame& f = *frames[k];
        std::string where;
        for (size_t car = 0; car < carCount && car < kRaceCaptureCars; car++) {
            const uint8_t* theirs = f.cars.data() + car * kRaceCaptureCarSize;
            const uint8_t* ours = reinterpret_cast<const uint8_t*>(&race.CarAt(car));
            const uint32_t bodyAddress = RaceAddress(0x800A9688u) + uint32_t(car) * uint32_t(kRaceCaptureCarSize) + uint32_t(kBody);
            size_t diffs = 0, firstDiff = SIZE_MAX;
            for (size_t i = kBody; i < kRaceCaptureCarSize; i++) { // the simulated part of the record (race_common.cpp FramesCompare)
                const size_t b = i - kBody;
                if (!(b < 0x798 || (b >= 0xA60 && b < 0xA78)) || theirs[i] == ours[i]) continue;
                const size_t w = i & ~size_t(3);
                if (word(theirs + w) == word(ours + w) + bodyAddress && word(ours + w) < 0xB40) continue; // a relocated pointer
                diffs++;
                if (firstDiff == SIZE_MAX) firstDiff = i;
            }
            if (diffs) {
                char text[96];
                std::snprintf(text, sizeof text, " car %zu: %zu byte(s), first body + 0x%zX;", car, diffs, firstDiff - kBody);
                where += text;
            }
            if (diffs && std::getenv("GT2_FRAMES_RANGES") && k == size_t(std::atoi(std::getenv("GT2_FRAMES_RANGES")))) { // dev: the differing ranges
                for (size_t i = kBody; i < kRaceCaptureCarSize;) {
                    const size_t bo = i - kBody;
                    if (theirs[i] == ours[i] || !(bo < 0x798 || (bo >= 0xA60 && bo < 0xA78))) { i++; continue; }
                    size_t j = i;
                    while (j < kRaceCaptureCarSize && theirs[j] != ours[j]) j++;
                    std::printf("    car %zu body + 0x%03zX..0x%03zX: original", car, i - kBody, j - 1 - kBody);
                    for (size_t q = i; q < j && q < i + 8; q++) std::printf(" %02X", theirs[q]);
                    std::printf(" ours");
                    for (size_t q = i; q < j && q < i + 8; q++) std::printf(" %02X", ours[q]);
                    std::printf("\n");
                    i = j;
                }
            }
        }
        // player 1's stream object as the playback left it (frames left, position, the current run)
        const std::vector<uint8_t>& ours = pads.stream[0].Bytes();
        const size_t n = std::min({f.stream.size(), ours.size(), ReplayStream::kHeaderSize + size_t(pads.stream[0].Used())});
        if (!std::equal(ours.begin(), ours.begin() + std::ptrdiff_t(n), f.stream.begin())) where += " stream 1 differs;";
        compared++;
        if (!where.empty()) differing++;
        if ((!where.empty() && shown++ < 12) || k % 300 == 0 || k + 1 == count)
            std::printf("  frame %5zu (field %6u) hold %3u  %s%s\n", k, f.field, f.hold, where.empty() ? "= original" : "DIFFERS:", where.c_str());
        std::vector<sim::PadRecord> none(race.CarCount());
        race.Step(none.data());
    }
    std::printf("frames: %zu compared, %d differ (2 player replay: both cars' bodies byte for byte + player 1's stream; streams ended: %s)\n", compared, differing,
                pads.ended ? "yes" : "no");
    return differing;
}

int SoundCompareSplit(const DiscImage& disc, const GtfsVolume& vol, const RaceData& data, const RaceOptions& options, const std::string& capturePath, int maxFrames) {
    const std::vector<RaceCaptureFrame> all = ReadRaceCapture(capturePath);
    if (all.empty()) throw std::runtime_error(capturePath + ": no frames");
    std::vector<const RaceCaptureFrame*> frames;
    for (const RaceCaptureFrame& f : all)
        if (f.race == all[0].race) frames.push_back(&f);
    const size_t carCount = 2;
    sim::RaceSim race;
    SetupRace(race, data, carCount, options);
    // The capture's pads: player 1's logical pad of every frame (the dev aid drives both cars by the AI: the pads are idle).
    SplitPads pads;
    pads.pedalTable = PedalTable(disc);
    race.SetPadSource(&SplitPads::Source, &pads);
    // Both cameras as the race load sets them (0x80010000(camera, player), + 0x103 = 1).
    const camera::CameraConstants cameraConstants = camera::LoadCameraConstants(LoadOverlayImage(disc, kRaceOverlayIndex));
    const std::vector<uint8_t> cameraTro = vol.Read("crsobj/" + data.trackName + ".tro");
    const camera::ReplayCameraData replayCameras = camera::ReplayCamerasOfTro(cameraTro);
    std::array<camera::GameCamera, 2> cameras;
    std::vector<camera::CarShape> shapes;
    for (size_t i = 0; i < carCount; i++) shapes.push_back(camera::CarShapeOf(ParseCarModel(vol.Read("carobj/" + data.carIds[i] + ".cdo"))));
    for (size_t p = 0; p < 2; p++) {
        camera::GameCamera::Options o;
        o.gameMode = data.constants.gameMode;
        o.player = uint8_t(p);
        o.split = true;
        cameras[p].Setup(cameraConstants, data.track, replayCameras, shapes, o);
        cameras[p].Start(race);
    }
    uint8_t viewSplit = 0;
    SplitViewFrame(cameras[0].Camera(), cameras[1].Camera(), viewSplit);
    // The cars' sound as the original sets it up: the capture aid's player cars have control class 2 (player banks).
    audio::RaceAudio sound;
    std::vector<audio::CarSoundSetup> setups(data.sound.begin(), data.sound.begin() + std::ptrdiff_t(carCount));
    for (audio::CarSoundSetup& c : setups) c.controlClass = 2;
    sound.Load(vol, LoadExeImage(disc), LoadOverlayImage(disc, kRaceOverlayIndex), setups);
    sound.PrimeTwoPlayer(race, viewSplit != 0);
    // The compared bytes of the sound object (car + 0xAA4): the logic's fields, not the driver's voice handles / sample tokens
    // and addresses (the original's SPU allocation and memory layout are not ours).
    auto logical = [](const uint8_t* o) {
        std::vector<uint8_t> v(o, o + 0x12);                    // mode, index, doppler, rpm, pitches, master, panSide, pan
        v.insert(v.end(), o + 0x16, o + 0x24);                  // the bank / effect volumes, blow-off
        for (size_t bank = 0; bank < 2; bank++) {
            const uint8_t* e = o + 0x24 + bank * 0x28;
            v.insert(v.end(), e + 4, e + 6);                    // layer count, priority
            v.insert(v.end(), e + 8, e + 0x10);                 // rpm, pitch scale, volumes
            for (size_t k = 0; k < 2; k++) {
                const uint8_t* sl = e + 0x10 + k * 0xC;
                v.insert(v.end(), sl + 1, sl + 3);              // layer, previous
                v.insert(v.end(), sl + 4, sl + 0xA);            // volumes, pitch
            }
        }
        return v;
    };
    std::printf("sound compare (2 player): %zu captured race frames (fields %u..%u)\n", frames.size(), frames[0]->field, frames.back()->field);
    // The HUD's inputs too (GT2_HUD2P_RAMS="<ram.bin>;<ram.bin>..": gt2play --prims snapshots of the same run): at the capture frame
    // whose two car bodies equal a snapshot's, our inputs (SplitHudOf, the game's) against the original's (TwoPlayerHudFromRam).
    struct HudProbe { std::string path; std::vector<uint8_t> ram; size_t frame = SIZE_MAX; };
    std::vector<HudProbe> probes;
    const gt2::ExeProfile& profile = gt2::ProfileOf(disc);
    const GuestImage raceOverlay = LoadOverlayImage(disc, kRaceOverlayIndex), exeImage = LoadExeImage(disc);
    gt2::HudStrings hudStrings = gt2::LoadHudStrings(vol);
    hudStrings.base = exeImage.Sim(gt2::HudStrings::kBase); // as Hud's
    if (const char* list = std::getenv("GT2_HUD2P_RAMS")) {
        for (std::string rest = list; !rest.empty();) {
            const size_t semi = rest.find(';');
            HudProbe probe;
            probe.path = rest.substr(0, semi);
            rest = semi == std::string::npos ? std::string() : rest.substr(semi + 1);
            if (probe.path.empty()) continue;
            if (std::FILE* f = std::fopen(probe.path.c_str(), "rb")) {
                probe.ram.resize(0x200000);
                probe.ram.resize(std::fread(probe.ram.data(), 1, probe.ram.size(), f));
                std::fclose(f);
            }
            if (probe.ram.size() != 0x200000) throw std::runtime_error(probe.path + ": not a 2 MB RAM image");
            const uint32_t cars = profile.Race(0x800A9688u) & 0x1FFFFF;
            constexpr size_t kBody = offsetof(sim::Car, body);
            for (size_t k = 0; k < frames.size() && probe.frame == SIZE_MAX; k++) {
                bool equal = true;
                for (size_t car = 0; car < 2 && equal; car++)
                    equal = std::equal(frames[k]->cars.begin() + std::ptrdiff_t(car * kRaceCaptureCarSize + kBody),
                                       frames[k]->cars.begin() + std::ptrdiff_t(car * kRaceCaptureCarSize + kBody + 0x798),
                                       probe.ram.begin() + std::ptrdiff_t(cars + car * kRaceCaptureCarSize + kBody));
                if (equal) probe.frame = k;
            }
            std::printf("hud check: %s = capture frame %s\n", probe.path.c_str(), probe.frame == SIZE_MAX ? "none" : std::to_string(probe.frame).c_str());
            probes.push_back(std::move(probe));
        }
    }
    int hudChecked = 0, hudDiffering = 0;
    uint8_t hudCounter = 0;
    const size_t count = maxFrames > 0 ? std::min(frames.size(), size_t(maxFrames)) : frames.size();
    int differing = 0, shown = 0;
    std::array<int, 2> carDiffering{};
    for (size_t k = 0; k < count; k++) {
        const RaceCaptureFrame& f = *frames[k];
        std::string where;
        for (size_t car = 0; car < carCount; car++) {
            const std::vector<uint8_t> theirs = logical(f.cars.data() + car * kRaceCaptureCarSize + 0xAA4);
            const std::vector<uint8_t> ours = logical(reinterpret_cast<const uint8_t*>(&sound.Sound(car)));
            if (theirs != ours) {
                carDiffering[car]++;
                size_t first = 0;
                while (first < theirs.size() && theirs[first] == ours[first]) first++;
                char text[96];
                std::snprintf(text, sizeof text, " car %zu (compared byte %zu: original %02X%02X ours %02X%02X);", car, first, theirs[first],
                              first + 1 < theirs.size() ? theirs[first + 1] : 0, ours[first], first + 1 < ours.size() ? ours[first + 1] : 0);
                where += text;
            }
        }
        if (!where.empty()) differing++;
        if ((!where.empty() && shown++ < (std::getenv("GT2_SOUND_SHOW") ? std::atoi(std::getenv("GT2_SOUND_SHOW")) : 12)) || k % 600 == 0 || k + 1 == count)
            std::printf("  frame %5zu (field %6u) split %u  %s%s\n", k, f.field, unsigned(viewSplit), where.empty() ? "= original" : "DIFFERS:", where.c_str());
        for (const HudProbe& probe : probes) {
            if (probe.frame != k) continue;
            const TwoPlayerHudRam theirs = TwoPlayerHudFromRam(probe.ram.data(), profile, raceOverlay, hudStrings, false);
            SplitHudContext hc;
            hc.laps = options.laps;
            hc.replaying = data.constants.flag800A951C != 0;
            hc.courseMap = theirs.frames[0].courseMap; // career + 0xB1: the options, not the race's state
            hc.tyrePanel = data.constants.wear.wearLimit != 0 || data.constants.shellControlClass == 2;
            hc.counter = hudCounter;
            const SplitHud ours = SplitHudOf(race, {&cameras[0].Camera(), &cameras[1].Camera()}, viewSplit != 0, hc, hudStrings, nullptr, 0);
            std::string d;
            if (!theirs.valid) d = " the snapshot is not a 2 player race frame";
            else {
                if (ours.split != theirs.split) d += " split";
                if (ours.followed != theirs.followed) d += " followed";
                for (size_t car = 0; car < 2; car++) {
                    const std::string c = DiffHud(ours.frames[car], theirs.frames[car]);
                    if (!c.empty()) d += " car " + std::to_string(car) + ":" + c + ";";
                }
            }
            hudChecked++;
            if (!d.empty()) hudDiffering++;
            std::printf("  hud check frame %zu (%s): %s%s\n", k, probe.path.c_str(), d.empty() ? "inputs = original" : "DIFFERS:", d.c_str());
        }
        std::vector<sim::PadRecord> none(race.CarCount());
        race.Step(none.data());
        SplitHudCounter(race, hudCounter);
        for (size_t p = 0; p < 2; p++) cameras[p].Update(race, camera::CameraPad{});
        SplitViewFrame(cameras[0].Camera(), cameras[1].Camera(), viewSplit);
        sound.StepTwoPlayer(race, {ListenerOf(cameras[0].Camera()), ListenerOf(cameras[1].Camera())}, viewSplit != 0, data.constants.flag800A951C != 0);
    }
    std::printf("sound compare: %zu frames, %d differ (car 0: %d, car 1: %d; the sound objects' logic fields)\n", count, differing, carDiffering[0], carDiffering[1]);
    if (!probes.empty()) std::printf("hud check: %d snapshot(s) compared, %d differ (the HUD's inputs of both cars)\n", hudChecked, hudDiffering);
    return differing + hudDiffering;
}

} // namespace gt2game
