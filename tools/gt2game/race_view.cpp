// The interactive race in the game window (see race_view.h). The frame loop is the former main.cpp race loop.
#include "race_view.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>

#include "frame_interp.h"
#include "xr_field_pacing.h"
#include "graphics_options.h"
#include "pc_overlay.h"
#include "game/audio/music_player.h"
#include "game/audio/race_audio.h"
#include "game/audio/race_music.h"
#include "game/camera/game_camera.h"
#include "game/career/career_state.h"
#include "game/career/events.h"
#include "game/menu/menu_car.h"
#include "game/shell/title_draw.h"
#include "game/shell/title_options.h"
#include "gt2view/machine_test_views.h"
#include "gt2view/race_result_screens.h"
#include "gt2view/race_menu_views.h"
#include "gt2view/race_record_screens.h"
#include "gt2view/race_session_screens.h"
#include "gt2formats/car_model.h"
#include "game_window.h"
#include "gt2formats/course_data.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/sponsor_boards.h"
#include "gt2formats/xa_audio.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "gt2view/hud.h"
#include "gt2view/particles.h"
#include "gt2view/scene_assets.h"
#include "mod_scene.h"
#include "mods.h"
#include "panel.h"
#include "platform/input/ps1_pad.h"
#include "settings_screen.h"
#include "title_attract.h"
#include "title_mode.h"
#include "platform/os/keys.h"
#include "platform/os/paths.h"

using namespace gt2;
using namespace gt2view;

namespace gt2game {

namespace {

// The pad block of the race overlay's views (0x80083998's copy: pressed since the last read, auto-repeat pulses) from the
// keyboard / script / the pad's menu keys: arrows = d-pad, Enter = cross, Space = circle, Backspace / Esc = triangle, Delete =
// square, Q / Page Up = L1, W / Page Down = R1, Home = start (held directions repeat after 20 fields, then every 5).
class MenuPadReader {
public:
    gt2::MenuListPad Read(const GameWindow& window) {
        namespace pb = gt2::menu_list_pad;
        static const std::pair<int, uint32_t> kKeys[] = {
            {keys::kUp, pb::kUp}, {keys::kDown, pb::kDown}, {keys::kLeft, pb::kLeft}, {keys::kRight, pb::kRight}, {keys::kReturn, pb::kCross}, {keys::kSpace, pb::kCircle},
            {keys::kBack, pb::kTriangle}, {keys::kEscape, pb::kTriangle}, {keys::kDelete, pb::kSquare}, {'Q', pb::kL1}, {keys::kPageUp, pb::kL1}, {'W', pb::kR1},
            {keys::kPageDown, pb::kR1}, {keys::kHome, pb::kStart}};
        gt2::MenuListPad pad;
        for (const auto& [key, bit] : kKeys) {
            if (window.Held(key)) pad.held |= bit;
            if (window.Pressed(key)) pad.pressed |= bit;
        }
        constexpr uint32_t kRepeating = pb::kUp | pb::kDown | pb::kLeft | pb::kRight;
        const uint32_t held = pad.held & kRepeating;
        if (held && held == (previous_ & held)) {
            if (++timer_ >= 20 && (timer_ - 20) % 5 == 0) pad.repeat = held;
        } else {
            timer_ = 0;
        }
        previous_ = pad.held;
        return pad;
    }

private:
    uint32_t previous_ = 0;
    int timer_ = 0;
};

// The tyre smoke inputs of one car as 0x800133F0 reads them (gt2view/particles.h SmokePool::CarInput): the physics
// render matrix (car + 0x81C / + 0x830), the wheel records (x = -/+ half track of the axle, y = ride reference -
// travel, z = the .cdo wheel entry's third s16 given in `wheelZ`) and the wheels' skid levels (wheel + 0x1C).
SmokePool::CarInput SmokeInput(const sim::RaceSim& race, size_t car, const std::array<int16_t, 4>& wheelZ) {
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

// The wheel mesh area of a race car as its setup 0x80017FA0 generates it (car_mesh.h): per axle the body's wheel radius,
// width (body + 0x3EC) and tyre height, the dish depth of its configuration's wheel word (CarConfig +0x00).
WheelArea WheelAreaOf(const sim::RaceSim& race, size_t car, const RaceData& data, const WheelTemplates& templates, const std::array<int16_t, 4>& dishes) {
    const sim::CarBody& body = race.CarAt(car).body;
    const int16_t dish = WheelDishDepth(car < data.configs.size() ? data.configs[car].word00 : 0, dishes);
    std::array<WheelAxleDims, 2> dims;
    for (size_t axle = 0; axle < 2; axle++) dims[axle] = RaceWheelDims(body.wheelRadius[axle], body.rimRadius[axle], body.tyreHeight[axle], dish);
    return GenerateWheelArea(dims, templates);
}

enum class Phase { kPreRace, kSettings, kRacing, kPaused, kResult };

// The inputs of the race-end display 0x8002B170 (gt2view/race_overlay_screens.h) from the race and the flow.
gt2::raceui::RaceEndState RaceEndOf(const sim::RaceSim& race, const RaceFlow& flow, const RaceOptions& options) {
    gt2::raceui::RaceEndState s;
    const sim::RaceShellState& shell = race.Shell().State();
    s.subMode = flow.raceEndMode;
    s.timer = shell.endTimer;
    s.auxTimer = shell.endTimerAux;
    s.seriesRaces = flow.seriesRaces;
    s.lapCount = int(options.laps);
    s.licenceResult = shell.licenseResult;
    s.licenceTime = shell.licenseTime;
    for (size_t k = 0; k < 4; k++) s.medalTimes[k] = flow.medalTimes[k];
    s.fourthPrizeCounts = flow.fourthPrizeCounts;
    for (size_t k = 0; k < race.CarCount(); k++) { // the race order (0x801C8578)
        const int car = race.RaceOrder()[k];
        if (car < 0 || size_t(car) >= race.CarCount()) continue;
        gt2::raceui::RaceEndRow r;
        r.name = size_t(car) < flow.carNames.size() ? flow.carNames[size_t(car)] : std::string();
        r.player = car == 0;
        r.car = car;
        r.finishTime = race.CarAt(size_t(car)).finishTime;
        r.finished = race.CarAt(size_t(car)).body.finishFlag != 0;
        r.laps = race.CarAt(size_t(car)).body.lap;
        s.rows.push_back(r);
    }
    return s;
}

} // namespace

RaceViewResult RunRaceView(GameWindow& window, Panels* panels, const DiscImage& disc, const GtfsVolume& vol, RaceData& data, size_t carCount,
                           const RaceOptions& raceOptions, const RaceViewConfig& config, RaceFlow* flow) {
    VkSceneRenderer& renderer = window.Renderer();
    const std::string& trackName = data.trackName;
    const std::string carId = data.carIds.empty() ? std::string() : data.carIds[0];
    RaceViewResult result;

    // Race music (game/audio/race_music.h, 0x800299D8): the executable's track table; the pre-race intro raises the
    // start hold to its length (the StartRace raise of the shell, RaceShellOptions::introFields), the race track is
    // drawn from the task generator (the original's state is arbitrary here: seeded from the clock) or --music N.
    const std::vector<MusicTrack> musicTracks = ReadMusicTable(LoadExeImage(disc));
    uint32_t musicRandom = uint32_t(std::chrono::steady_clock::now().time_since_epoch().count());
    if (config.deterministic) musicRandom = 0x1234u; // scripted runs: the same intro / track every time
    audio::RaceMusicInputs musicInputs;
    musicInputs.gameMode = data.constants.gameMode;
    musicInputs.demoFlag = data.constants.flag800A951C;
    auto initRaceMusic = [&](sim::RaceShellState* state) {
        audio::RaceMusicBytes m;
        uint16_t holdInitial = 0, hold = raceOptions.countdown ? 1 : 0; // the shell's hold is non-zero exactly with the countdown
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
    initRaceMusic(nullptr); // sets the intro's hold before the race is set up

    // The title's race options (settings.txt mirrors the career's option bytes): Chase View (+0xB0) = the physics' view
    // mode (0x801C9990, the chase cameras' lag), Camera Position / View Angle / Course Map below.
    const shell::PcSettings* settings = config.pcSettings;
    const bool haveOptions = settings && settings->haveCareerOptions;
    if (haveOptions) data.constants.viewMode = settings->options.chaseView; // the same byte (career + 0xB0 = 0x801C9990)

    sim::RaceSim race;
    SetupRace(race, data, carCount, raceOptions);
    // Replays (gt2formats/replay.h): player 1's input is recorded like the original's (0x80013EF0 -> 0x80013C90 ->
    // 0x800166CC) and the car is driven from the recorded frame; a replay (--replay, or P after the finish) runs the race
    // again from its start with a stream played back (the physics and the AI are deterministic).
    ReplayDriver replayDriver;
    replayDriver.pedalTable = PedalTable(disc);
    std::vector<uint8_t> replayStream = config.replay ? config.replay->stream : std::vector<uint8_t>{}; // what a replay plays
    bool replaying = config.replay != nullptr || config.ghostReplay;
    auto startInput = [&] {
        if (config.ghostReplay) { // the sim reads the ring (RaceSim::Step, pad slot 2 with 0x800A951C)
            replayDriver.mode = ReplayDriver::Mode::kOff;
            replayDriver.Attach(race);
            return;
        }
        if (replaying) replayDriver.StartPlayback(replayStream, data.constants.gameMode);
        else replayDriver.StartRecording(data.constants.gameMode);
        replayDriver.Attach(race);
    };
    startInput();
    auto replayOver = [&] { return replayDriver.ended || (config.ghostReplay && race.HasGhost() && race.Ghost().replayEnded); }; // 0x800A8D68
    bool replayWritten = false;
    // Player 1's controller as the race overlay reads it (platform/input/ps1_pad.h): the pad object 0x800A9528, polled every
    // field (0x8007FC30 with the race's handlers), its logical pad built every race frame (0x80014BB4 from BeginFrame) through
    // the key tables of the pad block, the motors fed after every step (the tail of 0x800133F0). The keyboard is merged at
    // the logical level (below). The buffer is larger than the object: the original writes a function above 8 that takes an
    // axis past it (ps1_pad.h BuildRaceLogical).
    namespace padobj = input::pad_object;
    const input::PadTables padTables = input::PadTables::Load(LoadExeImage(disc));
    std::vector<uint8_t> padBlock = config.padBlock;
    if (padBlock.size() != input::pad_block::kSize) { // the new game's pad block (0x800104A0: EXE tables, pad configuration)
        const career::CareerState fresh = career::NewCareer(career::ReadNewGameDefaults(disc));
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&fresh) + 0x0A;
        padBlock.assign(b, b + input::pad_block::kSize);
    }
    std::array<uint8_t, 0x200> padObject{};
    input::InitTracker(padObject.data() + padobj::kTracker, padTables);
    std::array<uint8_t, 0x200> padScratch{}; // the logical pad of fields without a race frame (discarded)
    auto keyTablesFor = [&](const input::Ps1PadFrame& f) { // ours: the triggers profile for pads with analogue triggers
        std::array<uint8_t, 4 * input::pad_block::kTableSize> t{};
        std::copy(padBlock.begin(), padBlock.begin() + std::ptrdiff_t(t.size()), t.begin());
        if (config.triggerPedals && f.pressure) {
            const std::array<uint8_t, 11> analog = input::TriggerPedalTable(t.data() + input::pad_block::kTableSize);
            std::copy(analog.begin(), analog.end(), t.begin() + input::pad_block::kTableSize);
        }
        return t;
    };
    uint8_t padTypeLogged = 0xFF;
    // 0x801D5867 of a licence test: its licence, 0 S .. 5 B (ovl4 0x80010078 writes 0x80010000(test name) there); the HUD
    // (0x8002E63C) draws the course map in game mode 3 only when it is 0.
    int licenceIndex = 0;
    if (raceOptions.license) {
        try {
            licenceIndex = career::LicenceOfTest(career::EventMenuData::Load(LoadOverlayImage(disc, 4)), raceOptions.license->name);
        } catch (const std::exception& e) {
            std::printf("hud: licence of %s unknown (%s)\n", raceOptions.license->name.c_str(), e.what());
        }
    }
    auto writeReplay = [&] { // --replay-out: the recorded race in the .gmr layout
        if (config.replayOut.empty() || replaying || replayWritten || replayDriver.stream.Frames() == 0) return;
        ReplayStream s = replayDriver.stream;
        s.End(false); // 0x800167D0: the last run flushed
        const ReplayFile file = BuildReplayFile(vol, data, race.CarCount(), raceOptions, s);
        // the original's replay file format (.gmr) or a memory card image (.mcd: the title's Replay Theater lists it)
        if (WriteReplayOut(config.replayOut, file, data.params.empty() ? nullptr : &data.params[0], disc)) {
            std::printf("replay: %d frames (%u bytes coded)\n", s.Frames(), unsigned(s.Used()));
            replayWritten = true;
        }
    };
    ShellLog shellLog;
    shellLog.license = raceOptions.license;
    shellLog.Attach(race);
    bool standingsPrinted = false;
    std::printf("race: %s, %zu car(s), %u lap(s), hold %u fields, player heading %d, chunk %d\n", trackName.c_str(), race.CarCount(), raceOptions.laps, race.HoldFrames(),
                race.Telemetry(0).heading, race.CarAt(0).body.chunkIndex);

    renderer.clearColor[0] = 0.45f; renderer.clearColor[1] = 0.58f; renderer.clearColor[2] = 0.78f;
    SceneAssets assets(renderer, vol);
    // Sponsor boards (sponsor_boards.h, 0x800275E8): the original seeds its generator with the VSync counter at the
    // race load; a screenshot / scripted run uses a fixed seed (the attract race's, 0x14D57) so that it is reproducible.
    SponsorTable sponsorTable; // outlives the uploads: they refer to its logo images
    std::vector<SponsorUpload> sponsors;
    try {
        const uint32_t seed = config.sponsorSeedGiven ? config.sponsorSeed : (!config.shotPath.empty() || config.deterministic) ? 0x14D57u : uint32_t(gt2::os::TickCountMs() / 17u);
        const CourseInfoTable info = ParseCourseInfo(vol.Read(".crsinfo"));
        uint16_t flags = data.courseIndex >= 0 ? info.entries[size_t(data.courseIndex)].flags : uint16_t(0x40);
        if (data.modCourse) flags = data.modCourse->info.hasEntry ? data.modCourse->info.flags : uint16_t(0x40); // a mod course's own flags (mods.h)
        sponsorTable = ParseSponsorTable(vol.Read(".crstims.tsd"));
        sponsors = PlaceSponsorBoards(sponsorTable, LoadSponsorSlots(LoadOverlayImage(disc, kRaceOverlayIndex)), config.sponsorCategory, seed, flags);
        std::printf("sponsor boards: category %s, seed 0x%X, %zu slots\n", config.sponsorCategory.c_str(), seed, sponsors.size());
    } catch (const std::exception& e) {
        std::printf("sponsor boards: none (%s)\n", e.what());
        sponsors.clear();
    }
    if (data.modCourse) { // a mod course: its texture pack, backdrop and flags (mod_scene.h)
        const SceneAssets::TrackSources sources = ModTrackSources(*data.modCourse);
        assets.UseTrack(trackName, data.track, -1, &sponsors, &sources);
    } else {
        assets.UseTrack(trackName, data.track, -1, &sponsors);
    }
    PreloadModOpponents(assets, data); // --ai-cars mod cars with glTF bodies: loaded under their race ids first (mod_scene.h)
    int carSlot;
    if (data.modCar && data.mod.externalMesh) { // a mod body: the glTF through the renderer's external texture path
        SceneAssets::ExternalCar external;
        external.mesh = data.mod.mesh.get();
        external.scale = float(data.mod.meshScale);
        external.reflection = int(data.mod.meshReflection); // the reflection pass of the glTF body (car_json.md "Reflections")
        for (size_t k = 0; k < 3; k++) { external.wheelFront[k] = float(data.mod.wheelFront[k]); external.wheelRear[k] = float(data.mod.wheelRear[k]); }
        for (size_t k = 0; k < 2; k++) { external.wheelRadius[k] = float(data.mod.wheelRadius[k]); external.wheelWidth[k] = float(data.mod.wheelWidth[k]); }
        carSlot = assets.UseExternalCar(carId, external);
    } else {
        carSlot = -1;
    }
    { // the body models with the wheels of the race's car setup (loaded here so that later look-ups find them)
        WheelTemplates wheelTemplates = GeneratedWheelTemplates();
        std::array<int16_t, 4> dishes = kWheelDishDepths;
        try {
            const GuestImage exe = LoadExeImage(disc);
            wheelTemplates = LoadWheelTemplates(exe);
            dishes = LoadWheelDishDepths(exe);
        } catch (const std::exception& e) {
            std::printf("wheels: the executable's templates are not available (%s), generated ones used\n", e.what());
        }
        for (size_t i = 0; i < race.CarCount() && i < data.carIds.size(); i++) {
            const WheelArea area = WheelAreaOf(race, i, data, wheelTemplates, dishes);
            if (i == 0 && carSlot < 0) carSlot = assets.UseCar(data.modCar ? data.mod.modelId : carId, std::string(), &area);
            else if (const ResolvedCar* m = OpponentMod(data, i); m && !m->externalMesh) assets.UseCar(m->modelId, data.carIds[i], &area); // a mod AI car on a disc body: under its id
            else if (i > 0) assets.UseCar(data.carIds[i], std::string(), &area);
        }
        if (carSlot < 0 && !(data.modCar && data.mod.externalMesh)) carSlot = assets.UseCar(data.modCar ? data.mod.modelId : carId);
        // Fitted wheels (the race load ovl0 0x80028F5C, every car: CarConfig +0x00 -> 0x800615E8 loads the carwheel/ TIM,
        // its CLUT over the chosen paint's CLUT 0 = the rims' CLUT, 0x800678E8(buffer, slot, 0) its 48 x 48 image over the
        // car texture's top-left texels; game/menu/menu_car.h MenuWheelFile / LoadMenuWheelTexture).
        try {
            const menu::MenuWheelFiles wheelFiles = menu::LoadMenuWheelFiles(vol, LoadExeImage(disc));
            for (size_t i = 0; i < race.CarCount() && i < data.carIds.size() && i < data.configs.size(); i++) {
                if ((i == 0 && data.modCar) || OpponentMod(data, i)) continue; // a mod's own wheels
                const int file = menu::MenuWheelFile(wheelFiles, data.configs[i].word00);
                if (file < 0) continue;
                const int slot = i == 0 ? carSlot : assets.UseCar(data.carIds[i]);
                if (slot < 0) continue;
                const menu::MenuWheelTexture wheel = menu::LoadMenuWheelTexture(vol, wheelFiles.paths[size_t(file)]);
                assets.SetCarWheelTexture(slot, wheel.image, wheel.words, wheel.rows, wheel.clut);
                std::printf("wheels: car %zu (%s) fitted with %s\n", i, data.carIds[i].c_str(), wheelFiles.paths[size_t(file)].c_str());
            }
        } catch (const std::exception& e) {
            std::printf("wheels: fitted wheels not loaded (%s)\n", e.what());
        }
    }
    if (carSlot < 0) throw std::runtime_error("no free car slot in the scene");
    // A mod course's objects (external glTF meshes, mod_scene.h): in the scene slots left after the cars.
    const std::vector<CourseObjectDraw> courseObjects = data.modCourse ? LoadCourseObjects(assets, *data.modCourse) : std::vector<CourseObjectDraw>{};
    // Tyre smoke (gt2view/particles.h): the original's sprite pool, updated before and fed after every step. The
    // wheel records' z comes from each car's .cdo (a mod mesh: its wheel centres).
    bool particles = config.particles;
    SmokePool smoke;
    std::vector<std::array<int16_t, 4>> smokeWheelZ(race.CarCount(), std::array<int16_t, 4>{});
    if (particles) {
        try {
            smoke.LoadFadeScale(LoadOverlayImage(disc, kRaceOverlayIndex));
            for (size_t i = 0; i < race.CarCount() && i < data.carIds.size(); i++) {
                if (i == 0 && data.modCar && data.mod.externalMesh) {
                    for (size_t w = 0; w < 4; w++) smokeWheelZ[i][w] = int16_t(std::lround((w < 2 ? data.mod.wheelFront[2] : data.mod.wheelRear[2]) * 4096.0));
                    continue;
                }
                if (const ResolvedCar* m = OpponentMod(data, i); m && m->externalMesh) {
                    for (size_t w = 0; w < 4; w++) smokeWheelZ[i][w] = int16_t(std::lround((w < 2 ? m->wheelFront[2] : m->wheelRear[2]) * 4096.0));
                    continue;
                }
                const int slot = i == 0 ? carSlot : assets.UseCar(data.carIds[i]);
                if (slot >= 0)
                    for (size_t w = 0; w < 4; w++) smokeWheelZ[i][w] = assets.SlotModel(slot).wheels[w].x;
            }
        } catch (const std::exception& e) {
            std::printf("tyre smoke: disabled (%s)\n", e.what());
            particles = false;
        }
    }
    // HUD (gt2view/hud.h): the original's race font, gauge sheet and dial faces, fed from the simulation below.
    std::unique_ptr<Hud> hud;
    if (config.hud) {
        try {
            hud = std::make_unique<Hud>(renderer, vol, LoadExeImage(disc), LoadOverlayImage(disc, kRaceOverlayIndex));
            hud->UseCourse(data.modCourse ? data.modCourse->courseMap : trackName);
        } catch (const std::exception& e) {
            std::printf("hud: disabled (%s)\n", e.what());
        }
    }
    if (panels) panels->Upload();

    // Sound (game/audio): the cars' engine banks from the VOL, the effect bank, the ported per-frame logic on the
    // simulation's bodies, the native mixer on the default device. Optional: --no-sound, --shot, or no device.
    std::unique_ptr<audio::MusicPlayer> musicPlayer; // declared first: the mixer (in raceAudio) pulls from it until destroyed
    std::unique_ptr<audio::RaceAudio> raceAudio;
    if (config.sound && config.shotPath.empty()) {
        try {
            std::vector<audio::CarSoundSetup> setups(data.sound.begin(), data.sound.begin() + std::ptrdiff_t(carCount));
            for (size_t i = 0; i < setups.size(); i++) setups[i].controlClass = (i == 0 && !raceOptions.aiPlayer) ? 2 : 0;
            raceAudio = std::make_unique<audio::RaceAudio>();
            raceAudio->Load(vol, LoadExeImage(disc), LoadOverlayImage(disc, kRaceOverlayIndex), setups);
            std::string error;
            if (!config.recordAudio.empty() && !raceAudio->RecordTo(config.recordAudio)) throw std::runtime_error("cannot write " + config.recordAudio);
            if (!raceAudio->OpenDevice(error)) { std::printf("sound: no output device (%s); running silently\n", error.c_str()); raceAudio.reset(); }
            else std::printf("sound: engine set %05u (exhaust %c%u), %zu car(s)\n", data.sound[0].soundId, data.sound[0].exhaustByte >= 4 ? 't' : 'n',
                             unsigned(data.sound[0].exhaustByte & 3), setups.size());
        } catch (const std::exception& e) {
            std::printf("sound: disabled (%s)\n", e.what());
            raceAudio.reset();
        }
    }
    const shell::PcSettings* pc = config.pcSettings;
    if (raceAudio && !config.reverb) raceAudio->SetReverbEnabled(false);
    if (raceAudio && pc && pc->haveCareerOptions) raceAudio->SetMasterVolume(pc->options.sfxVolume); // options +0xB4 (title)
    // The race music: XA streamed from MUSIC.DAT into the mixer's CD input (game/audio/music_player.h).
    if (raceAudio && config.music) {
        try {
            musicPlayer = std::make_unique<audio::MusicPlayer>();
            musicPlayer->Open(config.discPath, LoadExeImage(disc));
            musicPlayer->SetVolume(pc && pc->haveCareerOptions ? pc->options.musicVolume : uint8_t(0xF0)); // 0x801C9993 (0xF0 = new game)
            raceAudio->AttachMusic(musicPlayer.get());
            std::printf("music: MUSIC.DAT at LBA %u, %zu tracks\n", musicPlayer->MusicLba(), musicPlayer->TrackCount());
        } catch (const std::exception& e) {
            std::printf("music: disabled (%s)\n", e.what());
            musicPlayer.reset();
        }
    }
    audio::RaceMusicBytes raceMusic = initRaceMusic(&race.Shell().State()); // the shell was just set up: the load's music bytes
    std::printf("music: intro %d, race track %u, hold %u fields\n", int(int8_t(raceMusic.request)), unsigned(raceMusic.raceTrack), race.HoldFrames());
    // 0x80029B60 once per race frame: the shell's request bytes -> the stream driver.
    auto stepMusic = [&] {
        sim::RaceShellState& s = race.Shell().State();
        raceMusic.loop = s.musicRequestFlag;
        raceMusic.request = s.musicRequest;
        raceMusic.raceTrack = s.musicRaceTrack;
        raceMusic.next = s.music2F0;
        raceMusic.kind = s.music2F1;
        const uint8_t before = raceMusic.playing;
        audio::UpdateRaceMusic(raceMusic, musicPlayer.get());
        if (raceMusic.playing != before)
            std::printf("music: %s %d%s\n", int8_t(raceMusic.playing) < 0 ? "stop" : "play", int(int8_t(raceMusic.playing)), raceMusic.loop ? " (loop)" : "");
        s.musicRequestFlag = raceMusic.loop;
        s.musicRequest = raceMusic.request;
        s.music2F0 = raceMusic.next;
    };
    shellLog.audio = raceAudio.get();
    audio::Listener listener;
    Vec3 listenerEye{0, 0, 0};
    bool listenerValid = false;
    int stepsSinceListener = 0;
    std::vector<sim::PadRecord> pads(race.CarCount());
    sim::PadRecord lastPad{};
    int steps = 0;
    int shiftRequest = 0;
    int cameraMode = 0; // --old-camera only

    // The race camera (game/camera): the original's camera object, updated after every physics step like 0x800100F4.
    // Options: the title's Camera Position / View Angle (--camera / --view-angle override), the overlay's tables, the
    // course's trackside cameras (used when the shell's demo flag is set: --attract), the cars' LOD 0 shapes.
    const camera::CameraConstants cameraConstants = camera::LoadCameraConstants(LoadOverlayImage(disc, kRaceOverlayIndex));
    std::vector<uint8_t> cameraTro; // the course's trackside cameras (a mod course: its own list, mod_scene.h)
    camera::ReplayCameraData replayCameras;
    if (data.modCourse) replayCameras = ModReplayCameras(*data.modCourse, cameraTro);
    else {
        cameraTro = vol.Read("crsobj/" + trackName + ".tro");
        replayCameras = camera::ReplayCamerasOfTro(cameraTro);
    }
    camera::GameCamera raceCamera;
    {
        std::vector<camera::CarShape> shapes;
        for (size_t i = 0; i < race.CarCount(); i++) {
            const ResolvedCar* opponent = OpponentMod(data, i);
            const bool externalMesh = (i == 0 && data.modCar && data.mod.externalMesh) || (opponent && opponent->externalMesh); // no .cdo: the default shape
            const std::string id = i == 0 && data.modCar ? data.mod.modelId : opponent ? opponent->modelId : i < data.carIds.size() ? data.carIds[i] : std::string();
            shapes.push_back(externalMesh || id.empty() ? camera::CarShape{} : camera::CarShapeOf(ParseCarModel(vol.Read("carobj/" + id + ".cdo"))));
        }
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
        o.replay = uint8_t(data.constants.flag800A951C != 0 || replaying ? 1 : 0); // the attract race / a replay: the trackside cameras
        raceCamera.Setup(cameraConstants, data.track, replayCameras, std::move(shapes), o);
        raceCamera.Start(race);
        std::printf("camera: position %u, view angle %u (H %d)%s\n", unsigned(o.cameraPosition), unsigned(o.viewAngle), int(raceCamera.Camera().H),
                    o.replay ? ", trackside replay cameras" : "");
    }
    bool cameraPressed = false; // C since the last step: the camera button (logical 0x100, 0x800 in a replay)
    uint32_t replayButtons = 0; // O / I since the last step: the replay controls 0x400 / 0x200 (camera + 0x6C)
    bool replayEndLogged = false;

    // A fresh race on the grid (the standalone Backspace, the flow's Start / retry): 0x800299D8 again (+0x2EC = 0xFF
    // restarts the intro), the smoke pool cleared like 0x8001681C at the race load.
    // The graphics settings (graphics_options.h; presentation only). Display frame rate: every display refresh draws a state
    // between the last two 30 Hz steps (frame_interp.h); the steps themselves are unchanged. Scripted / shot / --fast runs
    // keep the original's frame-locked presentation (their frames must be reproducible), unless --interp-alpha asks for a
    // fixed in-between state.
    shell::GraphicsSettings graphics = CurrentGraphics();
    renderer.SetOptions(RenderOptionsOf(graphics));
    const bool deterministicRun = config.deterministic || !config.shotPath.empty();
    bool highRate = graphics.frameRate == shell::GraphicsSettings::kFrameRateDisplay && !deterministicRun && window.Pacing();
    bool interpolating = highRate || config.interpAlpha >= 0;
    std::printf("graphics: %s; presentation %s%s\n", DescribeGraphics(graphics).c_str(),
                highRate ? "at the display rate, interpolated" : "frame-locked to the fields (original)",
                config.interpAlpha >= 0 ? " (fixed in-between state --interp-alpha)" : "");
    RenderSnapshot previous; // the render state before the last step (the interpolation's start)
    FrameLog frameLog(config.frameLog);

    uint8_t ghostToggle = 1; // 0x800AF232 (game mode 6): 1 at the race start 0x8001584C, Select flips it (0x80015B64)
    // The event menu's Test Run (RaceFlow): the race block as 0x80017098 leaves it (1 car, 100 laps, countdown 0, sub-mode 1);
    // "Start Race" puts the event's back (0x8001710C). The race set up by resetRace.
    size_t activeCars = carCount;
    RaceOptions activeOptions = raceOptions;
    const uint8_t eventGameMode = data.constants.gameMode;
    bool testRun = false;
    // The TRANSMISSION bar's choice for the menus' races (race slot 0 + 0x8F; unset = RaceOptions::manual).
    std::optional<bool> manualChoice;
    if (flow && flow->transmission && panels && (flow->licenceMenu || flow->eventMenu || flow->machineTest)) manualChoice = false; // no bar yet: slot 0 + 0x8F = 0 as the menus built it (ovl4 0x80011000 / 0x80010078); every bar choice sets it
    auto selectRace = [&](bool test) {
        testRun = test;
        activeCars = test ? size_t(1) : carCount;
        activeOptions = raceOptions;
        if (manualChoice) activeOptions.manual = *manualChoice;
        if (manualChoice && !data.transmissions.empty()) data.transmissions[0] = uint8_t(*manualChoice ? 1 : 0);
        if (test) {
            activeOptions.laps = 100;
            activeOptions.countdown = false;
        }
        data.constants.gameMode = test ? uint8_t(1) : eventGameMode;
    };
    gt2::vr::BrakeReverse vrBrakeReverse;
    uint32_t vrControlsHeld = 0;
    auto resetRace = [&] {
        vrBrakeReverse.Reset(); vrControlsHeld = 0;
        previous.valid = false;
        ghostToggle = 1;
        SetupRace(race, data, activeCars, activeOptions);
        startInput();
        raceCamera.SetReplay(replaying || data.constants.flag800A951C != 0);
        raceCamera.Start(race);
        shellLog.Attach(race);
        raceMusic = initRaceMusic(&race.Shell().State());
        smoke.Reset();
        standingsPrinted = false;
        steps = 0;
        pads.assign(race.CarCount(), sim::PadRecord{});
    };

    // The flow's state.
    Phase phase = flow ? Phase::kPreRace : Phase::kRacing;
    int hudSubFrame = 0;
    int menuSelected = 0; // the pre-race menu's selection (startItem below)
    bool racedOnce = false;
    gt2::raceui::PauseMenu pauseMenu; // 0x800A94C0 / 0x800A94C1
    std::vector<std::string> resultLines;
    // The race overlay's menus (RaceFlow: licenceMenu / eventMenu with the panels): every row of the original's list.
    const bool overlayMenus = panels && flow && (flow->licenceMenu || flow->eventMenu || flow->machineTest);
    // The last race of this visit (M+0x240 "a replay exists"): its stream (ended), results record and whether it was a Test Run.
    std::vector<uint8_t> lastStream;
    sim::PlayerResults lastResults{};
    bool lastTestRun = false;
    bool menuCar = false;       // M+0x241: the event menu draws the car
    // What follows the replay state (12 -> 13 -> 6): the menu, the licence result then the menu, or the end of the view.
    enum class AfterReplay { kNone, kMenu, kLicenceResult, kLeave };
    AfterReplay afterReplay = AfterReplay::kNone;
    std::optional<RaceViewResult> pendingLicence; // the licence run's result, applied after its replay (0x8004E104)
    // The race overlay's menus as the original's views (gt2view/race_menu_views.h: their objects' open / close animations) in its
    // view manager (screens::SessionViewStack: the menu, the leave views 0x8005B428 / 0x8005B44C / 0x8005AE30 of 20 fields, the
    // slide transitions); SAVE REPLAY borrows the menu for its own transitions (RaceFlow::lentMenu).
    std::unique_ptr<gt2::screens::LicenceMenuView> licenceView;
    std::unique_ptr<gt2::screens::EventMenuView> eventView;
    std::unique_ptr<gt2::screens::MachineTestMenuView> machineView; // the machine test's menu (RaceFlow::machineTest)
    gt2::screens::PostRaceView* menuView = nullptr;
    gt2::screens::SessionViewStack menuStack;
    int pendingCode = 0; // the menu's code (M + 0x7C) while its leave view runs
    MenuPadReader menuPad;
    // The licence menu's selector starts on the race's test at every setup (0x8004ED00: block + 0x4B8 = race block + 0xC).
    const int raceTest = flow && flow->licenceMenu ? flow->licenceMenu->test : 0;
    bool lastManual = raceOptions.manual; // the transmission of the last race (its replay and Save Replay)
    auto menuAssets = [&]() -> const gt2::RaceMenuAssets& { return panels->MenuAssets(flow->licenceMenu ? Panels::Screen::kLicence : Panels::Screen::kSettings); };
    // 0x8004ED00 / 0x80057EAC(again): the menu's setup; a first one (again false) also starts the view manager's task anew.
    auto setupMenu = [&](bool again) {
        if (!overlayMenus) return;
        const bool replay = !lastStream.empty(); // M + 0x240
        if (flow->licenceMenu) {
            if (!licenceView) licenceView = std::make_unique<gt2::screens::LicenceMenuView>(menuAssets());
            if (flow->licenceMenuOfTest) flow->licenceMenu = flow->licenceMenuOfTest(raceTest); // 0x8004CCF8 of race block + 0xC
            licenceView->lastTransmission = flow->transmission ? *flow->transmission : 0;
            licenceView->rowEnabled[gt2::screens::kLicenceRecords] = bool(flow->records);
            licenceView->rowEnabled[gt2::screens::kLicenceDemo] = flow->demonstration;
            licenceView->Setup(*flow->licenceMenu, replay, again);
            menuView = licenceView.get();
        } else if (flow->machineTest) { // 0x800587BC(again)
            if (!machineView) machineView = std::make_unique<gt2::screens::MachineTestMenuView>(menuAssets());
            machineView->lastTransmission = flow->transmission ? *flow->transmission : 0;
            machineView->noTransmission = !flow->transmission || !flow->transmissionDialog;
            machineView->Setup(flow->machineTest, replay, menuCar && flow->menuCarId != 0, again);
            machineView->rowEnabled[gt2::screens::kMtSettings] = flow->settings != nullptr;
            machineView->rowEnabled[gt2::screens::kMtRecords] = bool(flow->recordsView);
            menuView = machineView.get();
        } else {
            if (!eventView) eventView = std::make_unique<gt2::screens::EventMenuView>(menuAssets());
            eventView->lastTransmission = flow->transmission ? *flow->transmission : 0;
            eventView->noTransmission = !flow->transmission || !flow->transmissionDialog;
            eventView->Setup(*flow->eventMenu, replay, menuCar && flow->menuCarId != 0, again);
            eventView->rowEnabled[gt2::screens::kEventSettings] = flow->settings != nullptr;
            menuView = eventView.get();
        }
        menuStack.Start(std::make_unique<gt2::screens::BorrowedView>(*menuView), false);
    };
    setupMenu(false);
    auto preRaceItems = [&]() { // in the order of the original's menu rows (Settings ... above Start Race)
        std::vector<std::string> items; // (our panel; the race overlay's menus are views)
        if (flow && flow->settings) items.push_back("Machine Settings");
        items.push_back(racedOnce && flow && flow->licence ? "Retry" : "Start");
        if (flow && flow->records) items.push_back("Records"); // the licence menu's row below Start
        items.push_back("Exit");
        return items;
    };
    auto itemIndex = [&](const std::string& name) {
        const std::vector<std::string> items = preRaceItems();
        for (size_t i = 0; i < items.size(); i++)
            if (items[i] == name) return int(i);
        return 0;
    };
    auto startItem = [&] { return itemIndex("Start"); }; // our panel opens on Start
    menuSelected = flow ? startItem() : 0;
    // The race just ended (the finish's X wait, or the pause's Exit): its stream is the replay (M+0x240 = 1, 0x80017964).
    auto keepRace = [&] {
        ReplayStream recorded = replayDriver.stream;
        recorded.End(false); // 0x800167D0: the last run flushed
        writeReplay();
        lastStream = recorded.Bytes();
        lastResults = race.Shell().State().results[0];
        lastTestRun = testRun;
        lastManual = activeOptions.manual;
    };
    auto playLast = [&](AfterReplay after) { // state 11 -> 12: the last race again from its stream
        selectRace(lastTestRun);
        activeOptions.manual = lastManual; // race slot 0 + 0x8F of that race
        if (!data.transmissions.empty() && manualChoice) data.transmissions[0] = uint8_t(lastManual ? 1 : 0);
        replayStream = lastStream;
        replaying = true;
        replayEndLogged = false;
        afterReplay = after;
        resetRace();
        phase = Phase::kRacing;
        std::printf("replay: %d frames\n", ReplayStream::FromBytes(lastStream).Frames());
    };
    // State 12 ended (the stream ran out: 0x800A8D68, or the replay pause's Exit) -> 13 -> 6; true = the view ends.
    auto endReplay = [&]() -> bool {
        replaying = false;
        const AfterReplay after = afterReplay;
        afterReplay = AfterReplay::kNone;
        if (after == AfterReplay::kNone) { // our P key: back to the result panel
            phase = Phase::kResult;
            return false;
        }
        if (after == AfterReplay::kLeave) { // an event race: the post-race views follow (0x8001728C, the caller)
            result.exit = RaceExit::kFinished;
            return true;
        }
        if (after == AfterReplay::kLicenceResult && pendingLicence) { // 0x8001736C -> view 0x8005B404: 0x8004E104 (record, NEW RECORD)
            resultLines = flow->finished ? flow->finished(*pendingLicence) : std::vector<std::string>{};
            pendingLicence.reset();
        }
        // 0x80017A28 -> the menu again (0x80057EAC / the licence menu's setup): the replay rows by M+0x240, the car by M+0x241
        phase = Phase::kPreRace;
        menuSelected = startItem();
        menuCar = flow->eventMenu.has_value() || flow->machineTest != 0;
        setupMenu(false);
        std::printf("race f%d: back to the %s menu after the replay\n", window.Field(), flow->licenceMenu ? "licence" : flow->machineTest ? "machine test" : "event");
        return false;
    };
    uint32_t enterPressedForShell = 0; // X for the shell's results wait (0x8002A700), latched until the next step
    Phase loggedPhase = Phase(-1);
    bool finishLogged = false;
    Phase autoPhase = phase;
    int phaseFrames = 0;

    using Clock = std::chrono::steady_clock;
    const double stepSeconds = data.constants.step.frameTime / 65536.0; // 1/30 s
    auto lastTime = Clock::now();
    auto nextPresent = lastTime; // the display frame rate's next present (cap cadence)
    // The display frame rate's time base: the steps run on the even fields of the window's paced 60 Hz clock, so the last
    // step's state belongs to its field's scheduled time (`stepAnchor`) and the next step comes exactly one step later; a
    // frame presented at time t shows alpha = (t - stepAnchor) / step (never beyond 1 before the next step exists).
    auto stepAnchor = lastTime;
    double buildEstimate = 0.003; // seconds one in-between frame takes to build (tracked; the present is predicted with it)
    bool presentModeLogged = false;
    std::string windowTitle; // the last SetWindowText
    double accumulator = 0;
    window.ResetPacing();
    std::vector<DrawItem> shownItems; // the last presented frame (the exit fade of the title's replays, config.exitFade)
    size_t shownScene = 0;
    bool exitFade = false;

    // VR (docs/research/vr_port_plan.md, M2): the race scene as a stereo projection layer. The draw list is built
    // ONCE per compositor frame, in the reference space "world - refEye" (DrawItem::space says what each item is),
    // and the renderer applies each eye's view-projection. The CPU work - the scenery LOD and render list, the smoke
    // billboards, the car's reflection axes, the backdrop - runs once from the rig's mid eye. The 2D layers (HUD,
    // panels, the rear-view mirror) stay flat and identical in both eyes until M4 moves them onto their own quads.
    const bool vrStereo = window.StereoAvailable() && !config.oldCamera;
    gt2::vr::View rigView;
    bool stereoActive = false;
    // The original camera of a frame as the rig needs it: its world frame and its clip matrix at the eye's aspect.
    auto rigCameraAt = [&](double alpha) {
        gt2::vr::Camera rc;
        const bool interp = alpha >= 0 && previous.valid && previous.poses.size() == race.CarCount();
        const float at = float(std::clamp(alpha, 0.0, 1.0));
        const camera::RaceCamera& cam = raceCamera.Camera();
        const bool cameraInterp = interp && !CameraCut(previous.camera, cam);
        const camera::CameraProjection p = cameraInterp ? InterpolatedProjection(previous.camera, cam, at) : camera::ProjectionOf(cam);
        for (int k = 0; k < 3; k++) {
            rc.eye[k] = p.eye[k];
            rc.right[k] = p.right[k];
            rc.up[k] = p.up[k];
            rc.forward[k] = p.forward[k];
        }
        if (!replaying && race.HoldFrames() != 0) {
            const auto pose = race.Pose(size_t(cam.target));
            gt2::vr::LowerIntroCamera(rc,float(double(pose.worldPosition[1])/65536.0),OverlayIntroLowering());
        }
        ClipMatrixOf(p, window.StereoAspect(), 0.1f, rc.clip);
        return rc;
    };
    // The 3D scene is not drawn at all while the race overlay's own menus are up (buildFrame clears the list).
    auto sceneShown = [&] { return !(phase == Phase::kPreRace && overlayMenus); };
    auto beginStereo = [&](double alpha) {
        stereoActive = false;
        if (!vrStereo || !sceneShown()) return false;
        stereoActive = window.BeginStereoScene(rigCameraAt(alpha), rigView);
        return stereoActive;
    };
    for (int frame = 1;; frame++) {
        const int fieldBeforeOverlay = window.Field();
        const uint64_t clockBeforePump = window.ClockRevision();
        window.SetDrivingActive(phase == Phase::kRacing && !replaying && race.HoldFrames() == 0);
        if (!window.BeginFrame()) {
            result.exit = RaceExit::kClosed;
            break;
        }
        if (window.Field() != fieldBeforeOverlay || window.ClockRevision() != clockBeforePump) {
            lastTime = nextPresent = stepAnchor = Clock::now(); accumulator = 0;
        }
        if (!(graphics == CurrentGraphics())) {
            graphics = CurrentGraphics(); renderer.SetOptions(RenderOptionsOf(graphics));
            highRate = graphics.frameRate == shell::GraphicsSettings::kFrameRateDisplay && !deterministicRun && window.Pacing();
            interpolating = highRate || config.interpAlpha >= 0;
        }
        { // 0x8007FC30 for port 1: the field's controller into the pad object, the motors of this poll
            const input::Ps1PadFrame& f = window.Pad();
            const input::Actuators act = input::PollPad(padObject.data(), f, padTables, padBlock.data() + input::pad_block::kCalibration,
                                                        window.Input().Port1HasMotors() ? 2 : 0);
            window.Input().SetActuators(phase == Phase::kRacing ? act : input::Actuators{});
            if (f.type != padTypeLogged) {
                padTypeLogged = f.type;
                if (f.type != input::kTypeNone)
                    std::printf("pad: %s, type %u (key table %d%s)\n", window.Input().Port1Name().c_str(), unsigned(f.type), input::TableOfType(f.type),
                                config.triggerPedals && f.pressure ? ", triggers as pedals" : "");
            }
        }
        if (!config.recordAudio.empty() && config.recordSeconds > 0 && steps * stepSeconds >= config.recordSeconds) {
            result.exit = RaceExit::kRecorded;
            break;
        }
        // --auto-race (dev aid for automated runs of the menus' races): Enter on the panels after 20 frames (Start,
        // then Exit once raced; Continue / Next race), X for the shell's results wait once the player finished.
        phaseFrames = phase == autoPhase ? phaseFrames + 1 : 0;
        autoPhase = phase;
        const bool autoEnter = flow && config.autoAdvance && phaseFrames > 0 && phaseFrames % 20 == 0 &&
                               (phase != Phase::kRacing || race.CarAt(0).body.finishFlag != 0);
        if (autoEnter && phase == Phase::kPreRace && !overlayMenus) menuSelected = racedOnce ? int(preRaceItems().size()) - 1 : itemIndex("Start");
        const bool enter = window.Pressed(keys::kReturn) || autoEnter;
        // ---- panels' input
        bool leave = false;
        // The race of the licence menu's Start when the selector named another test (the caller built it): no menu (0x8004E320).
        if (phase == Phase::kPreRace && flow && flow->startAtOnce && !racedOnce && steps == 0 && frame == 1) {
            if (manualChoice && flow->transmission) manualChoice = *flow->transmission != 0; // 0x8004E320: race block + 0xEB = career + 0x7C8E
            if (overlayMenus) selectRace(false);
            resetRace();
            phase = Phase::kRacing;
            std::printf("race: start (%s, from the licence menu's selector)\n", flow->title.c_str());
        }
        if (phase == Phase::kPreRace && overlayMenus) { // the view manager's update (0x800474F4) with the pad block
            gt2::MenuListPad pad = menuPad.Read(window);
            if (autoEnter) { // the dev aid: Start (Start Race) first, Exit once raced; the bars' choices as they stand ("Exit?": Yes)
                pad.pressed |= gt2::menu_list_pad::kCross;
                if (licenceView && licenceView->dialog == 0)
                    gt2::screens::MenuListSelect(licenceView->list, racedOnce ? gt2::screens::kLicenceExit : gt2::screens::kLicenceStart);
                if (eventView && eventView->dialog == 0) gt2::screens::MenuListSelect(eventView->list, racedOnce ? 5 : gt2::screens::kEventStartRace);
                if (eventView && eventView->dialog == 1) eventView->exitBar.cursor = 0;
                if (machineView && machineView->dialog == 0) gt2::screens::MenuListSelect(machineView->list, racedOnce ? gt2::screens::kMtExit : gt2::screens::kMtStart);
            }
            const int r = menuStack.Update(&pad);
            if (menuStack.Top()) panels->Sounds(menuStack.Top()->sounds);
            if (licenceView && licenceView->testChanged && flow->licenceMenuOfTest) { // 0x8004CCF8: the selector's test
                licenceView->info = flow->licenceMenuOfTest(licenceView->info.test);
                flow->licenceMenu = licenceView->info;
            }
            const bool menuOnTop = dynamic_cast<gt2::screens::BorrowedView*>(menuStack.Top()) != nullptr;
            if (r == 2 && machineView && !menuOnTop) { // the machine test's RECORD view left (0x8005918C returns 2): back to the menu, its setup(1)
                machineView->Setup(flow->machineTest, !lastStream.empty(), menuCar && flow->menuCarId != 0, true);
                menuStack.Pop();
            }
            if (r == 1 && menuOnTop) {
                const int code = licenceView ? licenceView->Action() : machineView ? machineView->Action() : eventView->Action();
                const bool start = licenceView || machineView ? code == 1 : (code == 1 || code == 4);
                const bool noBar = (eventView && eventView->noTransmission) || (machineView && machineView->noTransmission);
                if (start && flow->transmission && !noBar) {
                    const int t = licenceView ? licenceView->transmission : machineView ? machineView->transmission : eventView->transmission; // 0x801D156E = 0x801D5947
                    *flow->transmission = uint8_t(t);
                    manualChoice = t != 0;
                    std::printf("race: transmission %s (race slot 0 + 0x8F = %d, career + 0x7C8E)\n", t ? "MT" : "AT", t);
                }
                if (code == -2) { // SAVE REPLAY (view 0x8005B51C) with this race's replay; it slides the menu out and back in
                    const size_t cars = lastTestRun ? size_t(1) : carCount;
                    RaceOptions o = raceOptions;
                    o.manual = lastManual; // race slot 0 + 0x8F of that race
                    if (lastTestRun) {
                        o.laps = 100;
                        o.countdown = false;
                    }
                    const uint8_t mode = data.constants.gameMode;
                    data.constants.gameMode = lastTestRun ? uint8_t(1) : eventGameMode; // race block + 0xA of that race
                    const ReplayFile file = BuildReplayFile(vol, data, cars, o, ReplayStream::FromBytes(lastStream));
                    data.constants.gameMode = mode;
                    flow->lentMenu = menuView;
                    flow->resetupMenu = [&] {
                        if (licenceView) licenceView->Setup(flow->licenceMenuOfTest ? flow->licenceMenuOfTest(raceTest) : *flow->licenceMenu, !lastStream.empty(), true);
                        else if (machineView) machineView->Setup(flow->machineTest, !lastStream.empty(), menuCar && flow->menuCarId != 0, true);
                        else eventView->Setup(*flow->eventMenu, !lastStream.empty(), menuCar && flow->menuCarId != 0, true);
                    };
                    flow->saveReplay(file, lastResults);
                    flow->lentMenu = nullptr;
                    flow->resetupMenu = nullptr;
                    menuStack.Start(std::make_unique<gt2::screens::BorrowedView>(*menuView), false); // the menu goes on (its setup(1) ran)
                } else if (code == -4 && machineView && flow->recordsView) { // the machine test's RECORD (0x8005D3B4) pushed (0x800483A4)
                    menuStack.Push(flow->recordsView());
                } else if (code == -4 && flow->records) { // RECORD (view 0x8005B4DC), then the menu's setup(1)
                    flow->records();
                    setupMenu(true);
                } else if (code == -3 && flow->settings) { // Settings ... (0x801C90F4 = 1; view 0x8005D1C0), then the menu's setup(1)
                    flow->settings->Open();
                    phase = Phase::kSettings;
                } else { // the leave view (20 fields), then the code
                    pendingCode = code;
                    const uint32_t leaveView = eventView || machineView ? 0x8005AE30u : code == -5 ? 0x8005B44Cu : 0x8005B428u; // 0x80049D28 / 0x8004E464 / 0x8004E320
                    menuStack.Push(std::make_unique<gt2::screens::WaitView>(menuAssets(), leaveView, 20));
                }
            } else if (r == 1 && !menuOnTop) { // the leave view ran out: the code (M + 0x7C) of the loop
                const int code = pendingCode;
                if (code == -5) { // 0x8004E494: the selector's test's demonstration (the caller plays it)
                    result.exit = RaceExit::kDemonstration;
                    result.licenceTest = licenceView ? licenceView->info.test : raceTest;
                    leave = true;
                } else if (code == 0) {
                    playLast(AfterReplay::kMenu); // state 11
                } else if (code == 2) {
                    result.exit = RaceExit::kExited;
                    leave = true;
                } else if (licenceView && licenceView->info.test != raceTest) { // 0x8004E320 -> 0x8004C7A0: the selector's test is another race
                    result.exit = RaceExit::kChangeTest;
                    result.licenceTest = licenceView->info.test;
                    leave = true;
                } else { // licence 1 Start; event 1 Test Run (0x80017098) / 4 Start Race (0x8001710C)
                    selectRace(eventView && code == 1);
                    resetRace();
                    phase = Phase::kRacing;
                    std::printf("race: start (%s%s)\n", flow->title.c_str(), eventView && code == 1 ? ", Test Run" : "");
                    finishLogged = false;
                }
            }
        } else if (phase == Phase::kPreRace) { // our panel (a flow without the race overlay's menus)
            const std::vector<std::string> items = preRaceItems();
            if (window.Pressed(keys::kUp)) menuSelected = (menuSelected + int(items.size()) - 1) % int(items.size());
            if (window.Pressed(keys::kDown)) menuSelected = (menuSelected + 1) % int(items.size());
            if (window.Pressed(keys::kEscape) || window.Pressed(keys::kBack)) menuSelected = int(items.size()) - 1;
            if (enter) {
                const std::string& item = items[size_t(menuSelected)];
                if (item == "Records") {
                    flow->records(); // the RECORD view, back to this menu
                } else if (item == "Start" || item == "Retry") {
                    resetRace();
                    phase = Phase::kRacing;
                    std::printf("race: start (%s)\n", flow ? flow->title.c_str() : "");
                    finishLogged = false;
                } else if (item == "Machine Settings") {
                    flow->settings->Open();
                    phase = Phase::kSettings;
                } else {
                    result.exit = RaceExit::kExited;
                    leave = true;
                }
            }
        } else if (phase == Phase::kSettings) {
            const bool open = flow->settings->Update(window);
            if (panels) panels->Sounds(flow->settings->sounds); // 0x800574C0's effects of the page
            if (!open) {
                if (flow->settings->Changed() && flow->settingsChanged) {
                    flow->settingsChanged(); // slot 0's record rebuilt from the committed configuration
                    resetRace();
                }
                phase = Phase::kPreRace;
                setupMenu(true); // back from the settings view: the menu's setup(1) (0x80057EAC)
            }
        } else if (phase == Phase::kPaused) {
            // 0x80029D6C: up / down move (the selection flashes again), X / Enter chooses; Esc = Continue (ours).
            const int chosen = (window.Pressed(keys::kEscape) || window.PadPressed(input::ps1::kStart)) ? 0 : pauseMenu.Update(window.Pressed(keys::kUp), window.Pressed(keys::kDown), enter);
            if (chosen >= 0) {
                pauseMenu = gt2::raceui::PauseMenu{};
                if (chosen == 0) {
                    phase = Phase::kRacing;
                    lastTime = Clock::now();
                } else if (replaying) { // Exit of the replay's pause: the replay ends (0x80015DCC returns; the caller goes on)
                    std::printf("replay: exit from the pause menu after %d steps\n", steps);
                    result.steps = steps;
                    if (config.ghostReplay || !flow) { // the arcade loop's views / the theater
                        result.exit = config.ghostReplay ? RaceExit::kExited : config.replayEndLeaves ? RaceExit::kFinished : RaceExit::kQuit;
                        leave = true;
                    } else { // the replay of the race just driven: on as after its end (state 13)
                        leave = endReplay();
                    }
                } else if (flow && flow->pauseExitEnds) { // game mode 6: the race ends here (0x800153B8), the post-race views follow
                    race.EndGhostRace();
                    const sim::RaceShellState& s = race.Shell().State();
                    result.steps = steps;
                    result.finishTime = s.results[0].finishTime;
                    result.bestLap = s.results[0].count > 0 ? s.results[0].best.time : -1;
                    result.lapTimes.clear();
                    for (int i = 0; i < s.results[0].count && i < 10; i++) result.lapTimes.push_back(uint32_t(s.results[0].laps[i].time));
                    result.lapNumber = s.results[0].lapNumber;
                    result.playerResults = s.results[0];
                    result.newRecord = s.newRecord;
                    result.exit = RaceExit::kExited;
                    std::printf("race: exit from the pause menu (the race ends: %d lap(s) kept, new record %u)\n", int(s.results[0].count), unsigned(s.newRecord));
                    leave = true;
                } else if (overlayMenus && flow->eventMenu && !testRun) { // 0x80017964: sub-mode 2 -> state 7, the overlay is left
                    std::printf("race: exit from the pause menu (the event race is abandoned: back to the GT-mode menus)\n");
                    if (flow->raceEnded) { // 0x80017964 runs before it returns 7: the player's dirt
                        RaceViewResult ended;
                        ended.playerDirt = uint16_t(int32_t(race.CarAt(0).body.dirtiness << 12) / 600000);
                        flow->raceEnded(ended);
                    }
                    result.exit = RaceExit::kExited;
                    result.steps = steps;
                    leave = true;
                } else if (overlayMenus) { // 0x80017964: a licence test / a Test Run - M+0x240 = 1, the replay (state 11)
                    std::printf("race: exit from the pause menu (no result): the replay\n");
                    racedOnce = true;
                    keepRace();
                    if (flow->raceEnded) { // 0x80017964 (the race's end: the player's dirt), no result
                        RaceViewResult ended;
                        ended.playerDirt = uint16_t(int32_t(race.CarAt(0).body.dirtiness << 12) / 600000);
                        flow->raceEnded(ended);
                    }
                    playLast(AfterReplay::kMenu);
                } else { // the original's pause "Exit": back to the race overlay's menu, the race abandoned
                    std::printf("race: exit from the pause menu (the race is abandoned, no result)\n");
                    phase = Phase::kPreRace;
                    menuSelected = startItem();
                    resetRace();
                }
            }
        } else if (phase == Phase::kResult) {
            if (window.Pressed('P') && replayDriver.stream.Frames() > 0) { // the race's replay, back to this panel afterwards
                ReplayStream recorded = replayDriver.stream;
                recorded.End(false);
                writeReplay();
                replayStream = recorded.Bytes();
                replaying = true;
                replayEndLogged = false;
                resetRace();
                phase = Phase::kRacing;
                std::printf("replay: %d frames\n", recorded.Frames());
            } else if (enter || window.Pressed(keys::kEscape)) {
                if (flow && flow->licence) {
                    phase = Phase::kPreRace;
                    menuSelected = startItem();
                    resetRace();
                } else {
                    result.exit = RaceExit::kFinished;
                    leave = true;
                }
            }
        } else { // racing (keyboard keys alone: the pad drives through the logical pad below; Start = the pause)
            if ((window.KeyPressed(keys::kEscape) || window.PadPressed(input::ps1::kStart)) && replaying && panels) {
                // Start in a replay: the race overlay's pause menu 0x80029D6C / 0x80029E80 over the replay (Continue / Exit; the
                // same function as in the race, arcade_disc.md 17.7); its Exit leaves the replay (below, Phase::kPaused).
                phase = Phase::kPaused;
                pauseMenu.Open();
            } else if (window.KeyPressed(keys::kEscape) || window.PadPressed(input::ps1::kStart)) {
                if (config.ghostReplay) { // the mode 6 replay's pause Exit: on to the post-race views
                    result.exit = RaceExit::kExited;
                    result.steps = steps;
                    leave = true;
                    exitFade = config.exitFade;
                } else if (!flow) {
                    result.exit = RaceExit::kQuit;
                    leave = true;
                    exitFade = config.exitFade;
                } else if (replaying) { // leaving the replay: back to the result panel (or on as after the replay's end)
                    leave = endReplay();
                } else {
                    phase = Phase::kPaused;
                    pauseMenu.Open();
                }
            }
            if (window.KeyPressed('C')) {
                cameraMode = (cameraMode + 1) % 3;
                cameraPressed = true;
            }
            if (race.HasGhost() && ((!window.XrPaced() && window.PadPressed(input::ps1::kSelect)) || window.KeyPressed('G'))) // 0x80015B64: generic 0x20000
                ghostToggle = sim::GhostDisplayToggle(ghostToggle, 0x20000u);
            if (replaying) { // the replay controls (camera + 0x6C): O = onboard view 0x400, I = Replay Info 0x200
                if (window.Pressed('O')) replayButtons |= 0x400;
                if (window.Pressed('I')) replayButtons |= 0x200;
                // 0x800109FC's other buttons as keys: Up / Down = the followed car (generic 1 / 2), T = triangle (game mode 0:
                // the split), Page Up / Page Down = L1 / R1 (game mode 6: the previous / next lap of the ring, car + 0x21).
                if (window.Pressed(keys::kUp)) replayButtons |= 0x1;
                if (window.Pressed(keys::kDown)) replayButtons |= 0x2;
                if (window.Pressed('T')) replayButtons |= 0x100;
                if (window.Pressed(keys::kPageUp)) replayButtons |= 0x10;
                if (window.Pressed(keys::kPageDown)) replayButtons |= 0x1000;
            }
            // P after the finish: the replay of the race just driven (the recorded stream from the race's start).
            if (window.Pressed('P') && !replaying && !config.replay && race.CarAt(0).body.finishFlag != 0 && replayDriver.stream.Frames() > 0) {
                ReplayStream recorded = replayDriver.stream;
                recorded.End(false); // 0x800167D0: the last run flushed
                writeReplay();
                replayStream = recorded.Bytes();
                replaying = true;
                replayEndLogged = false;
                resetRace();
                std::printf("replay: %d frames\n", recorded.Frames());
            }
            if (window.KeyPressed(keys::kBack) && !flow) resetRace();
            if (window.KeyPressed('Q')) shiftRequest = 1;
            if (window.KeyPressed('A')) shiftRequest = -1;
            if (enter) enterPressedForShell = 0x200; // X: the shell's results wait (0x8002A700)
        }
        if (leave) break;

        sim::PadRecord pad{}; // the keyboard: a digital record like the original's from the buttons
        pad.throttle = (window.KeyHeld(keys::kUp) || (config.drive && !config.shotPath.empty())) ? 1 : 0;
        pad.brake = window.KeyHeld(keys::kDown) ? 1 : 0;
        pad.steer = int16_t((window.KeyHeld(keys::kLeft) ? 2 : 0) - (window.KeyHeld(keys::kRight) ? 2 : 0));
        pad.handbrake = window.KeyHeld(keys::kSpace) ? 1 : 0;
        pad.reverse = window.KeyHeld('R') ? 1 : 0;
        pad.shift = int8_t(shiftRequest);

        // Simulation at the original's 30 Hz, decoupled from the render rate. Screenshot and scripted runs lock two
        // render frames per step so that they are reproducible.
        const auto now = Clock::now();
        const auto fieldTime = window.NextField(); // this field's scheduled start (the display frame rate's time base)
        const bool deterministic = config.deterministic || !config.shotPath.empty();
        if (replaying && replayOver()) { // the stream ran out (0x800A8D68): the replay is over
            if (!replayEndLogged) std::printf("replay: end of the stream after %d steps\n", steps);
            replayEndLogged = true;
            if (config.replayEndLeaves && (config.replay || config.ghostReplay)) { // the title's replay theater: back to the theater
                result.exit = RaceExit::kFinished;
                result.steps = steps;
                exitFade = config.exitFade; // the same exit fade as after Start (title_attract.h)
                break;
            }
            if (flow && endReplay()) {
                result.steps = std::max(result.steps, steps);
                break;
            }
        }
        // The logical pad of player 1 (the recorder's input, 0x80013C90): the keyboard as the default key configuration.
        const LogicalPad keyboardPad = [&] {
            LogicalPad k;
            if (window.KeyHeld(keys::kLeft)) k.buttons |= kPadLeft;
            if (window.KeyHeld(keys::kRight)) k.buttons |= kPadRight;
            if (pad.throttle) k.buttons |= kPadThrottle;
            if (pad.brake) k.buttons |= kPadBrake;
            if (pad.handbrake) k.buttons |= kPadHandbrake;
            if (pad.reverse) k.buttons |= kPadReverse;
            return k;
        }();
        replayDriver.pad = LogicalPad{};
        if (window.KeyHeld(keys::kLeft)) replayDriver.pad.buttons |= kPadLeft;
        if (window.KeyHeld(keys::kRight)) replayDriver.pad.buttons |= kPadRight;
        if (pad.throttle) replayDriver.pad.buttons |= kPadThrottle;
        if (pad.brake) replayDriver.pad.buttons |= kPadBrake;
        if (pad.handbrake) replayDriver.pad.buttons |= kPadHandbrake;
        if (pad.reverse) replayDriver.pad.buttons |= kPadReverse;
        if (raceAudio) raceAudio->SetPaused(phase == Phase::kPaused);
        if (phase != Phase::kPaused) hudSubFrame = frame & 0xF;
        if (phase != Phase::kRacing || (replaying && replayOver())) {
            accumulator = 0;
        } else if (highRate) { // one step on every second field of the paced clock (the display frame rate's time base)
            if (frame % 2 == 0) accumulator = stepSeconds;
        } else if (!deterministic) {
            accumulator += std::chrono::duration<double>(now - lastTime).count();
            if (accumulator > stepSeconds * 4) accumulator = stepSeconds * 4;
        } else if (!window.Pacing()) { // --fast scripted runs: four steps per presented frame (still reproducible)
            accumulator = stepSeconds * 4;
        } else if (frame % 2 == 0) {
            accumulator = stepSeconds;
        }
        lastTime = now;
        // Outside the race (panels, pause) what the pad did is not kept for the next race frame.
        if (phase != Phase::kRacing) input::SnapshotTracker(padObject.data() + padobj::kTracker, padScratch.data());
        uint32_t padLogicalHeld = 0; // the pad's logical held buttons of the last race frame (camera look-back)
        const int stepsAtField = steps;
        while (accumulator >= stepSeconds) {
            accumulator -= stepSeconds;
            // 0x80014BB4: player 1's logical pad of this race frame (the key table of the controller's type), merged with the
            // keyboard's (ours): buttons or-ed; a function the keyboard drives digitally in this frame drops the pad's axis.
            {
                const std::array<uint8_t, 4 * input::pad_block::kTableSize> tables = keyTablesFor(window.Pad());
                input::BuildRaceLogical(padObject.data(), tables.data());
                auto u16 = [&](uint32_t o) { return uint16_t(padObject[o] | padObject[o + 1] << 8); };
                auto u32 = [&](uint32_t o) { return uint32_t(u16(o) | uint32_t(u16(o + 2)) << 16); };
                padLogicalHeld = u32(padobj::kLogical);
                uint32_t logicalPressed = u32(padobj::kLogical + 4), genericPressed = u32(padobj::kSnapshot + 4);
                LogicalPad merged;
                merged.buttons = keyboardPad.buttons | (padLogicalHeld & 0x43Fu); // driving functions 0..5 and the stick curve (0x400)
                merged.analog = uint16_t(u16(padobj::kAnalogFlags) & 0xD);
                if (keyboardPad.buttons & (kPadLeft | kPadRight)) merged.analog &= 0xFFFEu;
                if (keyboardPad.buttons & kPadThrottle) merged.analog &= 0xFFFBu;
                if (keyboardPad.buttons & kPadBrake) merged.analog &= 0xFFF7u;
                merged.steerAxis = u16(padobj::kValues + 0);
                merged.throttle = u16(padobj::kValues + 4);
                merged.brake = u16(padobj::kValues + 6);
                if (!replaying && window.XrPaced()) {
                    const auto& bindings = OverlayControlBindings();
                    gt2::vr::ApplyControlBindings(merged,padLogicalHeld,window.Pad(),bindings);
                    logicalPressed = padLogicalHeld & ~vrControlsHeld;
                    vrControlsHeld = padLogicalHeld;
                    vrBrakeReverse.Apply(merged,race.CarAt(0).body.forwardSpeed / 4096.f,!activeOptions.manual && bindings.brakeReverse);
                }
                float physicalSteer = 0;
                if (!replaying && window.PhysicalSteering(physicalSteer)) {
                    merged.analog |= 1;
                    merged.buttons &= ~(kPadLeft | kPadRight | kPadStickCurve);
                    merged.steerAxis = uint16_t(std::lround(128 + std::clamp(physicalSteer,-1.f,1.f)*127));
                }
                replayDriver.pad = merged;
                if (!replaying && (logicalPressed & camera::kButtonView)) cameraPressed = true; // the view button (R1 by default; not read in replays)
                // the replay controls read the generic pad (0x800109FC with pad + 0x6C): Circle / Square / Cross, Up / Down, Triangle,
                // L1 / R1
                if (replaying) replayButtons |= genericPressed & (0xE00u | 0x100u | 0x1010u | 0x3u);
                if (!replaying) pad = PadOfFrame(FrameOfPad(merged), replayDriver.pedalTable);
            }
            pads[0] = pad;
            replayDriver.pad.buttons &= ~(kPadShiftUp | kPadShiftDown);
            replayDriver.pad.buttons |= padLogicalHeld & (kPadShiftUp | kPadShiftDown); // the pad's shift buttons (held, as the original records them)
            if (shiftRequest > 0) replayDriver.pad.buttons |= kPadShiftUp;
            if (shiftRequest < 0) replayDriver.pad.buttons |= kPadShiftDown;
            if (interpolating) { // what the renderer reads of the state before this step (copies; frame_interp.h)
                previous.valid = true;
                previous.poses.resize(race.CarCount());
                for (size_t i = 0; i < race.CarCount(); i++) previous.poses[i] = race.Pose(i);
                previous.camera = raceCamera.Camera();
                previous.smoke = smoke;
                const sim::CarBody& b = race.CarAt(0).body;
                previous.rpm = b.engineRpm;
                previous.speedReadout = b.speedReadout;
                previous.boost = b.intakeLoad;
                previous.clock = race.RaceClock();
                previous.wheels.resize(race.CarCount());
                for (size_t i = 0; i < race.CarCount(); i++) previous.wheels[i] = race.Wheels(i);
            }
            if (particles) smoke.Update(); // 0x80016978 from BeginFrame, before the physics of the frame
            const uint32_t buttons[2] = {enterPressedForShell, enterPressedForShell};
            race.Step(pads.data(), buttons);
            enterPressedForShell = 0;
            { // the camera after the tick (0x80015B64: 0x8003EBF0, then 0x800100F4)
                camera::CameraPad cameraPad;
                if (cameraPressed) {
                    cameraPad.pressed |= camera::kButtonView;
                    cameraPad.replayPressed |= 0x800; // replays: the replay camera mode
                }
                cameraPad.replayPressed |= replayButtons;
                replayButtons = 0;
                if (config.lookBack || window.KeyHeld('V') || (padLogicalHeld & camera::kButtonLookBack)) cameraPad.held |= camera::kButtonLookBack;
                raceCamera.Update(race, cameraPad);
                cameraPressed = false;
                // game mode 6 replays: the lap step the controls wrote (car + 0x21), read by 0x80013EF0 at the next tick
                if (const int8_t step = raceCamera.TakeLapStep(0); step != 0 && data.shell.ghost) data.shell.ghost->replayLapStep = step;
            }
            if (particles) // 0x800133F0 per car after the physics (frame driver 0x8003EBF0)
                for (size_t i = 0; i < race.CarCount() && i < smokeWheelZ.size(); i++) smoke.SpawnFromCar(SmokeInput(race, i, smokeWheelZ[i]));
            { // the tail of 0x800133F0 for the player's car: the motors of the next polls (vibration option: pad block + 0x2C)
                const sim::Car& car = race.CarAt(0);
                input::VibrationSource v;
                v.demo = data.constants.flag800A951C != 0 || replaying;
                v.padSlot = car.padSlot;
                v.vibrationOff = padBlock[input::pad_block::kVibrationOff];
                v.finishTime = car.finishTime;
                v.word760 = car.body.word760;
                v.loadLevel = car.body.loadSoundLevel;
                v.impactFlag = car.body.impactSoundFlag;
                input::FeedVibration(padObject.data(), v);
            }
            shiftRequest = 0;
            pad.shift = 0;
            steps++;
            stepsSinceListener++;
            if (raceAudio && listenerValid) raceAudio->Step(race, listener, config.attractShell);
            stepMusic();
            if (steps % 30 == 0) PrintTelemetry(race, 0, steps);
            if (steps % 30 == 0 && window.Pad().type != input::kTypeNone) { // the controller's share (0x80013C90's frame -> the pad record)
                const ReplayFrame& f = replayDriver.lastFrame;
                auto s16 = [&](uint32_t o) { return int16_t(uint16_t(padObject[o] | padObject[o + 1] << 8)); };
                std::printf("pad: frame flags %X buttons %02X steer axis %u throttle %u brake %u -> steer %d throttle %u brake %u; motors large %d small %d\n",
                            unsigned(f.flags), unsigned(f.buttons), unsigned(f.steer), unsigned(f.throttle), unsigned(f.brake), int(pad.steer),
                            unsigned(pad.throttle), unsigned(pad.brake), s16(padobj::kVibration + 2) >> 4, s16(padobj::kVibration + 4) >> 4);
            }
            if (particles && steps % 30 == 0 && smoke.ActiveCount() != 0 && !flow) std::printf("tyre smoke: %zu sprites\n", smoke.ActiveCount());
            // The race is over for the player once the shell flagged the finish (body + 0x6FC); the standings
            // are printed once, the cars keep driving their cool-down lap.
            if (!standingsPrinted && race.CarAt(0).body.finishFlag != 0) {
                PrintStandings(race, shellLog, data.carIds);
                standingsPrinted = true;
            }
            // The flow: the shell ended the race task (0x8002A700 after the finish and the results wait).
            if (!replaying && replayDriver.mode == ReplayDriver::Mode::kRecord && replayDriver.stream.Ended()) writeReplay(); // recorded to its end (300 / 60 fields after the finish)
            if (flow && !replaying && race.RaceTaskOver() && race.CarAt(0).body.finishFlag != 0) {
                const sim::RaceShellState& s = race.Shell().State();
                result.finished = true;
                result.steps = steps;
                result.position = s.finishPosition;
                result.licenseResult = s.licenseResult;
                result.licenseTime = s.licenseTime;
                result.finishTime = s.results[0].finishTime;
                result.bestLap = s.results[0].count > 0 ? s.results[0].best.time : -1;
                result.lapTimes.clear();
                for (int i = 0; i < s.results[0].count && i < 10; i++) result.lapTimes.push_back(uint32_t(s.results[0].laps[i].time));
                result.lapNumber = s.results[0].lapNumber;
                result.playerResults = s.results[0];
                result.newRecord = s.newRecord;
                result.positionsAtPlayerFinish = shellLog.positionsAtPlayerFinish;
                result.orderPosition = 0;
                for (size_t i = 0; i < race.CarCount(); i++)
                    if (race.RaceOrder()[i] == 0) result.orderPosition = int32_t(i + 1);
                PrintPlayerResults(race);
                racedOnce = true;
                accumulator = 0;
                if (overlayMenus) { // 0x80017964: flag 0x800A8D68 < 2 -> M+0x240 / + 0x241 = 1, the replay at once (state 11 -> 12)
                    keepRace();
                    result.recordedStream = lastStream;
                    result.playerDirt = uint16_t(int32_t(race.CarAt(0).body.dirtiness << 12) / 600000); // 0x800131AC -> 0x801DE8B8
                    if (flow->raceEnded) flow->raceEnded(result);                                        // 0x80017964
                    if (flow->licenceMenu || flow->machineTest) { // the machine test: 0x80017200 after the replay (M + 0x5D1)
                        pendingLicence = result; // applied after the replay (0x8001736C -> 0x8004E104)
                        playLast(AfterReplay::kLicenceResult);
                    } else {
                        resultLines = flow->finished ? flow->finished(result) : std::vector<std::string>{};
                        playLast(testRun ? AfterReplay::kMenu : AfterReplay::kLeave);
                    }
                    break;
                }
                resultLines = flow->finished ? flow->finished(result) : std::vector<std::string>{};
                phase = Phase::kResult;
                break;
            }
        }
        if (steps != stepsAtField) stepAnchor = fieldTime;
        lastPad = pad;

        // The frame's draw items (the lambda's body keeps the former loop's indentation). `alpha` = how far between the
        // state before the last step (`previous`) and the last step's the frame shows; -1 = the last step as it is (the
        // original's presentation and exactly the former code path). `fieldFrame` = the 60 Hz field's own frame (the
        // listener and the panels' sound tick are updated once per field, not by the in-between frames).
        std::vector<DrawItem> items;
        size_t sceneCount = 0; // items[0, sceneCount) = the 3D scene (the graphics options' scale / MSAA / textures)
        auto buildFrame = [&](double alpha, bool fieldFrame) {
        items.clear();
        // In VR the frame is built for the eye image, not for the desktop window (they are both 4:3 in the checks).
        const float aspect = stereoActive ? window.StereoAspect() : renderer.AspectRatio();
        const float hudAspect = stereoActive ? 4.0f / 3.0f : aspect;
        // kWorld / kSky items are tagged as they are appended; everything else keeps the default kSpaceScreen (its
        // matrix is the whole world -> clip transform, which is what the desktop path has always produced).
        auto tagSpace = [](std::vector<DrawItem>& list, size_t from, uint32_t space) {
            for (size_t k = from; k < list.size(); k++) list[k].space = space;
        };
        const bool interp = alpha >= 0 && previous.valid && previous.poses.size() == race.CarCount();
        const float at = float(std::clamp(alpha, 0.0, 1.0));
        auto carModel = [&](size_t i, float* m) { // the car's render transform (interpolated unless it jumped)
            if (interp && !PoseJump(previous.poses[i], race.Pose(i))) InterpolatedModelMatrix(previous.poses[i], race.Pose(i), at, m);
            else ModelMatrix(race.Pose(i), m);
        };
        // The wheels' transforms (car space) as 0x800140A4 / 0x800670F0 build them: the record of 0x800133F0 (-/+ half track,
        // ride reference - travel, the .cdo wheel x) and the angles steer / camber / rolling angle (car_mesh.h WheelModelMatrix);
        // between two steps the angles and the travel are interpolated (the rolling angle the short way round).
        auto wheelModels = [&](size_t i, int slot) {
            std::array<std::array<float, 16>, 4> out{};
            const sim::CarBody& body = race.CarAt(i).body;
            const std::array<sim::WheelVisual, 4> now = race.Wheels(i);
            const bool blend = interp && i < previous.wheels.size() && !PoseJump(previous.poses[i], race.Pose(i));
            for (size_t w = 0; w < 4; w++) {
                sim::WheelVisual v = now[w];
                if (blend) {
                    const sim::WheelVisual& p = previous.wheels[i][w];
                    v.steerAngle = int16_t(LerpInt(p.steerAngle, now[w].steerAngle, at));
                    v.verticalOffset = int16_t(LerpInt(p.verticalOffset, now[w].verticalOffset, at));
                    const int32_t turn = int32_t(int16_t(uint16_t((now[w].rotation - p.rotation) << 4))) >> 4; // wrapped to -2048..2047
                    v.rotation = uint16_t(int32_t(p.rotation) + LerpInt(0, turn, at));
                }
                const int16_t halfTrack = body.halfTrack[w >> 1];
                const float centre[3] = {float((w & 1) ? halfTrack : -halfTrack) / 4096.0f, float(v.verticalOffset) / 4096.0f,
                                         i < smokeWheelZ.size() ? float(smokeWheelZ[i][w]) / 4096.0f : assets.SlotWheelCentres(slot)[w][2]};
                WheelModelMatrix(int(w), centre, v.steerAngle, body.camber[w >> 1], v.rotation, out[w].data());
            }
            return out;
        };
        // Camera: the original's race camera (game/camera), as updated after the last step; --old-camera: gt2game's
        // former chase / bonnet / high camera from the physics pose (debugging only).
        const sim::CarPose pose = race.Pose(0);
        const camera::RaceCamera& cam = raceCamera.Camera();
        // Game mode 6: what 0x800140A4 reads for the ghost (car 1) in this frame, `c` = the pass's camera (race_shell.h CarDrawRule).
        auto ghostDrawInputs = [&](const camera::RaceCamera& c, bool mirror) {
            sim::CarDrawInputs in;
            const sim::Car& ghostCar = race.CarAt(1);
            const sim::GhostSession& g = race.Ghost();
            in.mirror = mirror;
            in.viewCar = c.target == 1;
            in.viewCarHidden = c.hideTarget != 0;
            in.noLap = g.ghostPresent ? 0 : g.ghostStreamOwned; // car + 0x0F: 0x800133F0 clears it while car + 0x0E (a lap) is set
            in.padSlot = ghostCar.padSlot;
            const sim::CarPose p = race.Pose(1); // car + 0x830: the blended display pose (0x8003F2F0)
            in.dx = int32_t(uint32_t(p.worldPosition[0]) - uint32_t(c.view.t[0]));
            in.dy = int32_t(uint32_t(p.worldPosition[1]) - uint32_t(c.view.t[1]));
            in.dz = int32_t(uint32_t(p.worldPosition[2]) - uint32_t(c.view.t[2]));
            in.gameMode = data.constants.gameMode;
            in.hold = race.Shell().State().hold;
            in.demo = data.constants.flag800A951C;
            in.ghostToggle = ghostToggle;
            in.ghostOption = config.ghostOption;
            for (size_t w = 0; w < 4; w++) in.wheelGround[w] = ghostCar.body.wheels[w].surfaceAttribute; // car + 0x4A1 + w * 0x68
            return in;
        };
        const bool cameraInterp = interp && !CameraCut(previous.camera, cam); // the camera's output between the two steps
        Vec3 eye{}, cf{}, cs{}, cu{}; // eye, forward, right, up (world, metres)
        float vp[16], model[16];
        double projectionDistance = kProjectionDistance; // the view's H on the 320 x 240 frame (scenery LOD, smoke size)
        int cameraChunk = -1;                              // the chunk whose render list is drawn (camera + 0xA0)
        int hiddenCar = -1;                                // camera + 0x108: the followed car is not drawn (driver view)
        if (!config.oldCamera) {
            const camera::CameraProjection p = cameraInterp ? InterpolatedProjection(previous.camera, cam, at) : camera::ProjectionOf(cam);
            eye = {p.eye[0], p.eye[1], p.eye[2]};
            cf = {p.forward[0], p.forward[1], p.forward[2]};
            cs = {p.right[0], p.right[1], p.right[2]};
            cu = {p.up[0], p.up[1], p.up[2]};
            if (cameraInterp) ClipMatrixOf(p, aspect, 0.1f, vp);
            else camera::ClipMatrix(cam, aspect, 0.1f, vp); // the original's vertical field of view, widened for the aspect
            projectionDistance = p.H;
            cameraChunk = cam.chunk;
            if (cam.hideTarget) hiddenCar = cam.target;
            // the listener for the next simulation steps: camera + 0xB8 / 0xD8 / 0xEC / 0xF4 / 0x10B / 0x10A / 0x10C
            if (fieldFrame) {
                for (int k = 0; k < 3; k++) {
                    listener.position[k] = p.eye[k];
                    listener.velocity[k] = float(cam.velocity[k] / 65536.0);
                    listener.axisSide[k] = float(cam.backAxis[k]) / 4096.0f;
                    listener.axisRight[k] = float(cam.rightAxis[k]) / 4096.0f;
                }
                listener.inCarView = cam.external == 0;
                listener.inCarViewAlt = cam.inCarSound != 0;
                listener.focusedCar = cam.target;
                listenerValid = true;
            }
            if (stereoActive) {
                // VR: the list is built around the rig's mid eye (world - refEye), and the renderer turns it into
                // each eye's clip space. The camera of the CPU work is that mid eye, not the original camera's.
                eye = {rigView.refEye[0], rigView.refEye[1], rigView.refEye[2]};
                if (fieldFrame) for (int k = 0; k < 3; ++k) listener.position[k] = rigView.refEye[k];
                cs = {rigView.right[0], rigView.right[1], rigView.right[2]};
                cu = {rigView.up[0], rigView.up[1], rigView.up[2]};
                cf = {rigView.forward[0], rigView.forward[1], rigView.forward[2]};
                std::fill(vp, vp + 16, 0.0f);
                vp[0] = vp[5] = vp[10] = vp[15] = 1.0f;
                vp[12] = -eye.x;
                vp[13] = -eye.y;
                vp[14] = -eye.z;
            }
        } else {
            const Vec3 position = Position(pose);
            const Vec3 f3 = Forward(pose);
            const Vec3 f = Normalize({f3.x, 0, f3.z});
            const Vec3 up = Normalize({pose.rotation[0][1] / 4096.0f, pose.rotation[1][1] / 4096.0f, pose.rotation[2][1] / 4096.0f});
            Vec3 target;
            if (cameraMode == 0) { eye = position - f * 6.5f + Vec3{0, 2.3f, 0}; target = position + f * 4.0f + Vec3{0, 0.9f, 0}; }
            else if (cameraMode == 1) { eye = position + f3 * 0.6f + up * 1.05f; target = eye + f3 * 10.0f; }
            else { eye = position - f * 18.0f + Vec3{0, 12.0f, 0}; target = position; }
            cf = Normalize(target - eye);
            cs = Normalize(Cross(cf, {0, 1, 0}));
            cu = Cross(cs, cf);
            ViewProjection(eye, target, aspect, vp);
            // the listener for the next simulation steps: this camera (velocity per 30 Hz step from the eye's motion)
            if (fieldFrame) {
            const Vec3 motion = listenerValid && stepsSinceListener > 0 ? (eye - listenerEye) * (1.0f / float(stepsSinceListener)) : Vec3{0, 0, 0};
            listener.position[0] = eye.x; listener.position[1] = eye.y; listener.position[2] = eye.z;
            listener.velocity[0] = motion.x; listener.velocity[1] = motion.y; listener.velocity[2] = motion.z;
            listener.axisRight[0] = cs.x; listener.axisRight[1] = cs.y; listener.axisRight[2] = cs.z;
            listener.axisSide[0] = cf.x; listener.axisSide[1] = cf.y; listener.axisSide[2] = cf.z;
            listener.inCarView = cameraMode == 1;
            listener.inCarViewAlt = false;
            listener.focusedCar = 0;
            listenerEye = eye;
            listenerValid = true;
            }
        }
        if (fieldFrame) stepsSinceListener = 0;
        const std::array<std::array<float, 3>, 3> cameraAxes = {{{cs.x, cs.y, cs.z}, {-cu.x, -cu.y, -cu.z}, {cf.x, cf.y, cf.z}}}; // rows right, down, forward
        carModel(0, model);
        // Backdrop around the camera (opaque dome + ground fill first, blended layers last), course, car.
        float backdropModel[16], backdropMvp[16];
        const float eyeArray[3] = {eye.x, eye.y, eye.z};
        SceneAssets::BackdropModel(eyeArray, backdropModel);
        Multiply(vp, backdropModel, backdropMvp);
        std::vector<DrawItem> blended;
        const size_t skyFirst = items.size();
        assets.AppendBackdropItems(items, blended, backdropMvp);
        tagSpace(items, skyFirst, kSpaceSky); // the backdrop sits at infinity: no parallax between the eyes
        tagSpace(blended, 0, kSpaceSky);
        const size_t worldFirst = items.size(), worldBlendedFirst = blended.size();
        { // the course as the original selects it (render list of the camera chunk, scenery masks and LODs, billboards)
            SceneAssets::TrackView view;
            view.eye = {eye.x, eye.y, eye.z};
            view.positionalBillboards = stereoActive;
            view.right = {cs.x, cs.y, cs.z};
            view.forward = {cf.x, cf.y, cf.z};
            view.projectionDistance = projectionDistance;
            view.fullDetail = !config.raceDetail;
            view.maxDetail = config.maxDetail || graphics.maxDetail;
            view.cameraChunk = cameraChunk;
            view.extendedDistance = float(graphics.drawDistance); // 0 = the render list only (the original)
            assets.AppendTrackItems(items, blended, vp, view);
        }
        AppendCourseObjects(assets, items, vp, courseObjects); // a mod course's glTF objects (mod_scene.h)
        float carMvp[16];
        Multiply(vp, model, carMvp);
        if (hiddenCar != 0) { // reflection pass: the view-space normals need the camera frame (rows right, down, forward - the PS1 camera)
            assets.UpdateCarReflection(carSlot, model, cameraAxes);
            const uint32_t playerPaint = data.paints.empty() ? 0u : data.paints[0];
            const auto playerWheels = wheelModels(0, carSlot);
            assets.AppendCarItems(items, carSlot, carMvp, playerPaint, lastPad.brake ? 1u : 0u, true, &playerWheels); // shadow + body + wheels + reflection
        }
        for (size_t i = 1; i < race.CarCount() && i < data.carIds.size(); i++) { // the other cars
            if (int(i) == hiddenCar) continue;
            bool ghostLook = false;
            if (i == 1 && race.HasGhost()) { // game mode 6: 0x800140A4's rule for the ghost (hidden / its glass look / normal)
                const sim::CarDrawMode m = sim::CarDrawRule(ghostDrawInputs(cam, false));
                if (!m.drawn) continue;
                ghostLook = m.group == sim::kCarDrawGhostLook;
            }
            const int slot = ghostLook ? assets.UseCar(data.carIds[i], "ghost:" + data.carIds[i]) : assets.UseCar(data.carIds[i]);
            if (slot < 0) continue;
            float otherModel[16], otherMvp[16];
            carModel(i, otherModel);
            Multiply(vp, otherModel, otherMvp);
            if (ghostLook) { // LOD 2, no body texels (empty CLUT rows), no wheels: the reflection pass (colour by the dirt, 0x800140A4) and the shadow
                const int32_t dirt = std::min<int32_t>(int32_t(race.CarAt(1).body.dirtiness << 12) / 600000, 0x1000);
                assets.UpdateCarReflection(slot, otherModel, cameraAxes, uint16_t(9), uint16_t(0x7FD7), uint8_t(((0x1000 - dirt) * 3) >> 7), 2);
                assets.AppendCarItems(items, slot, otherMvp, 0u, 0, false);
                continue;
            }
            if (slot != carSlot || hiddenCar == 0) assets.UpdateCarReflection(slot, otherModel, cameraAxes); // a slot shared with a drawn player keeps the player's pass
            const auto otherWheels = wheelModels(i, slot);
            assets.AppendCarItems(items, slot, otherMvp, i < data.paints.size() ? data.paints[i] : 0u, 0, true, &otherWheels);
        }
        if (particles) { // tyre smoke: camera-facing sprites of the pool (0x8002EB60), additive
            const float smokeEye[3] = {eye.x, eye.y, eye.z}, smokeRight[3] = {cs.x, cs.y, cs.z}, smokeUp[3] = {cu.x, cu.y, cu.z}, smokeForward[3] = {cf.x, cf.y, cf.z};
            if (interp) assets.AppendSmokeItems(items, blended, InterpolatedSmoke(previous.smoke, smoke, at), vp, smokeEye, smokeRight, smokeUp, smokeForward, float(projectionDistance));
            else assets.AppendSmokeItems(items, blended, smoke, vp, smokeEye, smokeRight, smokeUp, smokeForward, float(projectionDistance));
        }
        if (stereoActive && !replaying && race.HoldFrames() == 0) window.AppendDrivingVisuals(items);
        tagSpace(items, worldFirst, kSpaceWorld); // course, cars, mod objects, smoke: the reference space
        tagSpace(blended, worldBlendedFirst, kSpaceWorld);
        items.insert(items.end(), blended.begin(), blended.end());
        std::copy(assets.SkyColor().begin(), assets.SkyColor().end(), renderer.clearColor);

        // The rear-view mirror (0x800294D4, camera + 0x109: the driver view not looking back; not split, game mode != 0,
        // frame-rate mode 0x801D5864 > 1 - 2 in every dumped 30 Hz race): a second view drawn over the scene before the
        // HUD, in the frame's rectangle (100, 20) 120 x 32 (centred like the HUD's centre block), from the camera copy
        // camera::MirrorCamera: the backdrop's two flat colours, the course's mirror copies (chunk + 0x94) nearer than
        // 100 m (0x80020110 with param 1), the cars (0x8001545C with param 1; the followed car stays hidden), the frame.
        if (config.mirror && (!VrMode() || OverlayHudVisibility().mirror) && !config.oldCamera && camera::MirrorShown(cam, data.constants.gameMode, data.constants.step.rate == 60 ? 1 : 2)) {
            const camera::RaceCamera mirrorCam = camera::MirrorCamera(cam);
            const float rect[4] = {0.5f + float(camera::kMirrorRect[0] - 160) / (240.0f * hudAspect), float(camera::kMirrorRect[1]) / 240.0f,
                                   0.5f + float(camera::kMirrorRect[0] + camera::kMirrorRect[2] - 160) / (240.0f * hudAspect),
                                   float(camera::kMirrorRect[1] + camera::kMirrorRect[3]) / 240.0f};
            float mirrorVp[16];
            // with the high frame rate: the mirror copies of the two steps' cameras, interpolated like the main view
            const camera::CameraProjection mp = cameraInterp ? InterpolatedProjection(camera::MirrorCamera(previous.camera), mirrorCam, at) : camera::ProjectionOf(mirrorCam);
            if (cameraInterp) ClipMatrixOf(mp, float(camera::kMirrorRect[2]) / float(camera::kMirrorRect[3]), 0.1f, mirrorVp);
            else camera::ClipMatrix(mirrorCam, float(camera::kMirrorRect[2]) / float(camera::kMirrorRect[3]), 0.1f, mirrorVp);
            // NDC of the mirror's own window -> the rectangle inside the game window (x' = sx x + ox w, clip space).
            const float sx = rect[2] - rect[0], ox = rect[0] + rect[2] - 1.0f, sy = rect[3] - rect[1], oy = rect[1] + rect[3] - 1.0f;
            for (int col = 0; col < 4; col++) {
                const float w = mirrorVp[col * 4 + 3];
                mirrorVp[col * 4 + 0] = sx * mirrorVp[col * 4 + 0] + ox * w;
                mirrorVp[col * 4 + 1] = sy * mirrorVp[col * 4 + 1] + oy * w;
            }
            // The horizon row: a far point straight ahead of the mirror camera at its own height.
            float hx = mp.forward[0], hz = mp.forward[2];
            const float hl = std::sqrt(hx * hx + hz * hz);
            float horizonY = rect[1] + (rect[3] - rect[1]) * 0.5f;
            if (hl > 1e-6f) {
                hx /= hl;
                hz /= hl;
                const float p[4] = {mp.eye[0] + hx * 10000.0f, mp.eye[1], mp.eye[2] + hz * 10000.0f, 1.0f};
                float clip[4] = {0, 0, 0, 0};
                for (int row = 0; row < 4; row++)
                    for (int col = 0; col < 4; col++) clip[row] += mirrorVp[col * 4 + row] * p[col];
                if (clip[3] > 1e-6f) horizonY = (clip[1] / clip[3] + 1.0f) * 0.5f;
            }
            const float border[2] = {1.0f / (240.0f * hudAspect), 1.0f / 240.0f};
            std::vector<DrawItem> mirrorFrame, mirrorBlended;
            assets.AppendMirrorFrameItems(items, mirrorFrame, rect, horizonY, border);
            assets.AppendMirrorTrackItems(items, mirrorBlended, mirrorVp, {mp.eye[0], mp.eye[1], mp.eye[2]}, mirrorCam.chunk, 100.0f, rect);
            const size_t firstCar = items.size();
            for (size_t i = 0; i < race.CarCount() && i < data.carIds.size(); i++) {
                if (int(i) == hiddenCar) continue;
                if (i == 1 && race.HasGhost() && !sim::CarDrawRule(ghostDrawInputs(mirrorCam, true)).drawn) continue; // 0x800140A4 in the mirror's pass
                const int slot = i == 0 ? carSlot : assets.UseCar(data.carIds[i]);
                if (slot < 0) continue;
                float mirrorModel[16], carMirrorMvp[16];
                carModel(i, mirrorModel);
                Multiply(mirrorVp, mirrorModel, carMirrorMvp);
                const auto mirrorWheels = wheelModels(i, slot);
                assets.AppendCarItems(items, slot, carMirrorMvp, i < data.paints.size() ? data.paints[i] : 0u, 0, true, &mirrorWheels);
            }
            for (size_t k = firstCar; k < items.size(); k++) std::copy(rect, rect + 4, items[k].scissor);
            items.insert(items.end(), mirrorBlended.begin(), mirrorBlended.end());
            items.insert(items.end(), mirrorFrame.begin(), mirrorFrame.end());
        }
        sceneCount = items.size(); // the HUD and the panels below are 2D layers at the window's resolution

        if (hud && phase != Phase::kPreRace && phase != Phase::kSettings) { // the HUD's inputs: the player's car record and the race shell (race_shell.h)
            const sim::CarBody& body = race.CarAt(0).body; // car record + 0x2C
            const sim::RaceShellState& shell = race.Shell().State();
            const sim::PlayerResults& results = shell.results[0];
            // The high frame rate: the needles / readouts and the running clock between the two steps (display only).
            const uint32_t clock = interp && previous.clock <= race.RaceClock() ? uint32_t(LerpInt(int32_t(previous.clock), int32_t(race.RaceClock()), at)) : race.RaceClock();
            HudFrame hf;
            hf.gameMode = testRun ? 1 : config.hudMode; // a Test Run: race block + 0xA = 1 (0x80017098; captured: "Lap 1", Record / Best Lap)
            if (raceOptions.license) hf.licenseType = raceOptions.license->Type(); // 0x801C98A2
            hf.licenseByte = licenceIndex;                                          // 0x801D5867: no course map on licences 1..5
            hf.replay = config.replayHud || replaying;
            hf.replayView = config.replayView;
            hf.carName = config.replay && !config.replay->cars.empty() ? config.replay->cars[0].name()
                         : flow && flow->licence ? std::string() // a licence car's slot has no name (0x8004C7A0; captured replays show "Replay" alone)
                         : flow && !flow->carNames.empty() ? flow->carNames[0] // the race slot's name (+ 0x90) in the replays of the menus' races
                                                            : carId; // a replay file: the slot's name
            hf.lapCount = activeOptions.laps;
            hf.lap = body.lap;                           // car + 0x634
            hf.position = int(int8_t(body.racePosition)); // car + 0x77C
            hf.finished = body.finishFlag != 0;          // car + 0x728
            hf.totalMs = hf.finished ? results.finishTime : int32_t(clock / 3);
            hf.lapMs = int32_t(clock / 3) - body.lastLineTime; // car + 0x7AC
            if (hf.lapMs < 0) hf.lapMs = int32_t(race.RaceClock() / 3) - body.lastLineTime; // (interpolated) the line was crossed in the last step
            hf.subFrame = hudSubFrame; // stands in for the original's counter 0x8002F864
            for (int i = 0; i < results.count && i < 10; i++) hf.laps.push_back(results.laps[i].time);
            hf.resultsLapNumber = results.lapNumber;
            hf.bestLapMs = results.count > 0 ? results.best.time : -1;
            hf.bestLapSpeed = results.count > 0 ? results.best.maxSpeed : 0;
            hf.recordMs = shell.courseRecord.time;
            hf.recordSpeed = shell.courseRecord.maxSpeed;
            hf.machineRecord = config.machineRecord; // sub-modes 7..9: 0x8002D20C of the career record's entry 0
            hf.rpm = interp ? LerpInt(previous.rpm, body.engineRpm, at) : body.engineRpm; // car + 0x6D8
            hf.revLimitRpm = body.revLimitRpm;     // car + 0x134
            hf.redlineRpm = body.upshiftRpm;       // car + 0x3C2
            hf.speedReadout = interp ? LerpInt(previous.speedReadout, body.speedReadout, at) : body.speedReadout; // car + 0x6DA
            hf.gear = body.gear;                   // car + 0x644
            hf.clutchEngaged = body.clutchState == 1; // car + 0x645
            hf.turbo = body.boostCap;              // car + 0x154
            hf.boost = interp ? LerpInt(previous.boost, body.intakeLoad, at) : body.intakeLoad; // car + 0x76E
            hf.metric = OverlayMetricUnits(config.metric);
            for (size_t i = 0; i < race.CarCount(); i++) { // car + 0x832 / 0x83A: the integer metres of the world position
                const sim::CarPose carPose = interp ? InterpolatedPoseRounded(previous.poses[i], race.Pose(i), at) : race.Pose(i);
                hf.mapCars.push_back({int16_t(carPose.worldPosition[0] >> 16), int16_t(carPose.worldPosition[2] >> 16), true});
            }
            hf.courseMap = !haveOptions || settings->options.courseMap; // Course Map option (career + 0xB1; new game: On)
            hf.startTimer = shell.startTimer;   // 0x800AF224: countdown digits and START / REPLAY (0x8002A19C)
            hf.messageCode = body.messageCode;  // car + 0x790: the warning line (0x8002E204)
            if (raceOptions.license)            // mode 3: the medal times of the record block (0x8002D12C)
                for (uint32_t m = 0; m < 3; m++) hf.medalMs[m] = int32_t(raceOptions.license->MedalTime(m + 1));
            hf.tyrePanel = data.constants.wear.wearLimit != 0 || data.constants.shellControlClass == 2; // 0x8002DE8C's test
            for (size_t w = 0; w < 4; w++) {
                hf.wheelDamage[w] = body.wheels[w].damage;       // wheel + 0x22
                hf.wheelWearStage[w] = body.wheels[w].wearStage; // wheel + 0x3F
            }
            hf.raceFinished = body.finishFlag != 0 && !(panels && flow); // with the flow: "Finish" of the race-end display 0x8002B170
            // the shell's message block (car record + 0xA8C..0xAA3, 0x8002D664)
            hf.timeInvalid = body.hudInvalid != 0;
            hf.crashKind = body.hudMessageKind;
            hf.captionTimer = body.hudTimer;
            hf.splitTimer = body.hudTimer2;
            hf.crashTimer = body.hudTimer3;
            hf.captionMs = body.hudValue;
            hf.caption = hud->Strings().At(body.hudLabel);
            hf.splitA = uint32_t(body.hudCompare);
            hf.splitB = uint32_t(body.hudGap);
            if (VrMode()) hf.visibility = OverlayHudVisibility();
            hud->Build(hf, hudAspect, items);
        }

        // ---- the flow's panels over the scene (a replay without a flow: its pause menu)
        if (panels && (flow || phase == Phase::kPaused)) {
            if (fieldFrame) panels->SoundFrame();
            panels->Clear();
            if (phase == Phase::kPreRace && (flow->licenceMenu || flow->eventMenu || flow->machineTest)) {
                items.clear(); // the menus are the race overlay's full screen (0x800479AC clears it): no race scene behind them
                sceneCount = 0;
                renderer.clearColor[0] = renderer.clearColor[1] = renderer.clearColor[2] = 0.0f; // black around the 352 x 480 frame
                // 0x800479AC: the view manager's frame (the menu, a leave view, the transitions; the event menu's car 0x80048754).
                size_t modelAt = 0;
                std::optional<gt2::screens::PostRaceModel> menuModel;
                std::vector<MenuPrim> prims = menuStack.Frame(menuAssets(), modelAt, menuModel);
                const Panels::Screen screen = flow->licenceMenu ? Panels::Screen::kLicence : Panels::Screen::kSettings;
                if (menuModel && flow->menuCarId != 0) panels->FullScreenModel(std::move(prims), screen, modelAt, menuModel, flow->menuCarId, flow->menuCarPaint);
                else panels->FullScreen(std::move(prims), screen);
            } else if (phase == Phase::kPreRace) {
                Panels::Menu m;
                m.title = flow->title;
                m.lines = flow->info;
                m.items = preRaceItems();
                m.selected = std::min(menuSelected, int(m.items.size()) - 1);
                m.footer = {"Up/Down choose, Enter select"};
                panels->DrawMenu(m, 70);
            } else if (phase == Phase::kSettings) {
                flow->settings->Draw(*panels);
            } else if (phase == Phase::kPaused) {
                panels->Overlay(gt2::raceui::BuildPauseFrame(panels->OverlayAssets(), pauseMenu)); // 0x80029E80
            } else if (phase == Phase::kResult) {
                Panels::Menu m;
                m.title = flow->licence ? "Licence Test Result" : "Race Result";
                m.lines = resultLines;
                m.items = {flow->licence ? "Continue" : flow->continueLabel};
                m.selected = 0;
                panels->DrawMenu(m, 150); // below the HUD's lap / record blocks
            } else if (race.CarAt(0).body.finishFlag != 0 && !race.RaceTaskOver()) {
                // The race-end display while the shell waits for X (0x8002A700): 0x8002B170 from the HUD.
                panels->Overlay(gt2::raceui::BuildRaceEndFrame(panels->OverlayAssets(), RaceEndOf(race, *flow, activeOptions)));
            }
            panels->Append(hudAspect, items);
        }
        }; // buildFrame

        // The field's frame: the original's presentation (the last step as it is), or with the high frame rate the state of
        // its predicted present time between the last two steps - one step behind, like every fixed-step interpolation.
        auto alphaAt = [&](Clock::time_point t) {
            return std::clamp(std::chrono::duration<double>(t - stepAnchor).count() / stepSeconds, 0.0, 1.0);
        };
        auto seconds = [](double s) { return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s)); };
        double fieldAlpha = -1;
        if (config.interpAlpha >= 0) fieldAlpha = config.interpAlpha;
        else if (highRate && phase == Phase::kRacing) fieldAlpha = alphaAt(Clock::now() + seconds(buildEstimate));
        if (!(highRate && window.XrPaced())) buildFrame(fieldAlpha, true);

        {
            const auto telemetry = race.Telemetry(0);
            const bool activePedals = phase == Phase::kRacing && !replaying && config.triggerPedals && AdaptivePedalStrength() > 0;
            const int strength = AdaptivePedalStrength();
            // A firmer brake as pressure rises; light accelerator resistance rises with engine speed.
            const int accelerator = activePedals ? std::max(1, (2 + std::clamp(int(telemetry.rpm) / 3000, 0, 2)) * strength / 100) : 0;
            const int brake = activePedals ? std::max(1, (3 + std::clamp(int(telemetry.brake) * 3 / 4096, 0, 3)) * strength / 100) : 0;
            window.Input().SetPedalResistance(uint8_t(accelerator), uint8_t(brake));
        }

        if (flow && phase != loggedPhase) { // the flow's screens with the window's field (for scripted runs)
            static const char* const kPhase[] = {"pre-race panel", "machine settings", "racing", "pause panel", "result panel"};
            std::printf("race f%d: %s\n", window.Field(), kPhase[int(phase)]);
            loggedPhase = phase;
        }
        if (flow && !finishLogged && race.CarAt(0).body.finishFlag != 0 && phase == Phase::kRacing) {
            std::printf("race f%d: the player finished (the shell waits for X / Enter to end the race task)\n", window.Field());
            finishLogged = true;
        }
        const bool capture = !config.shotPath.empty() && frame == config.shotFrames;
        if (!highRate) {
            // VR without pacing (--xr-deterministic, --shot): one compositor frame per field. The rig runs inside
            // that frame, so the list has to be built again - the field's build above is the mono one.
            if (vrStereo && window.WillPresent(capture ? config.shotPath : std::string())) {
                window.BeginPresent();
                if (beginStereo(fieldAlpha)) buildFrame(fieldAlpha, false);
            }
            window.EndFrame(items, capture ? config.shotPath : std::string(), std::chrono::nanoseconds(16'666'667), sceneCount);
            stereoActive = false;
            if (!config.frameLog.empty()) frameLog.Presented(false, fieldAlpha, steps);
        } else if (window.XrPaced()) {
            // VR (docs/research/vr_port_plan.md, M1): the compositor's frames are the clock. One frame per display
            // period, each built AFTER xrWaitFrame for the time the runtime will show it (predictedDisplayTime), until
            // a frame reaches past the next field's time - which is 72 / 90 frames per second from 60 fields per second
            // with none of the compositor's periods skipped, while the simulation keeps the original's 30 Hz steps.
            window.SkipFrame(std::chrono::nanoseconds(16'666'667), std::chrono::nanoseconds(66'666'667)); // catch up to 4 fields
            const bool racing = phase == Phase::kRacing && !(replaying && replayOver());
            const bool presented = PresentXrField(window.NextField(), window.DisplayTime(), [&](bool between) {
                window.BeginPresent(); // blocks until the runtime wants the next frame
                if (window.Closed()) return window.NextField();
                const auto buildStart = Clock::now(); // excludes xrWaitFrame; loading can still synchronize
                const auto display = window.DisplayTime();
                double a = fieldAlpha;
                if (racing) a = alphaAt(display);
                // The rig needs this frame's predicted display time, so it runs after BeginPresent and before the
                // build; no redundant mono frame is constructed for this XR field.
                beginStereo(a);
                buildFrame(a, !between); // field side effects once; pose/geometry for every XR frame
                const auto presentStart = Clock::now();
                window.FinishPresent(items, sceneCount);
                stereoActive = false;
                const auto after = Clock::now();
                frameLog.Presented(between, a, steps, std::chrono::duration<double, std::milli>(presentStart - buildStart).count(),
                                   std::chrono::duration<double, std::milli>(after - presentStart).count(), renderer.GpuMilliseconds());
                if (!presentModeLogged) {
                    std::printf("render: OpenXR display-time pacing (no 60 Hz sleep), MSAA %ux\n", renderer.EffectiveMsaa());
                    presentModeLogged = true;
                }
                return display;
            });
            if (!presented) buildFrame(fieldAlpha, true); // field side effects still run exactly once during catch-up
        } else {
            // The display frame rate: the field (input, script, logic: 60 Hz) only advances; the frames are presented at
            // their own cadence, each drawing the state of the time it reaches the screen between the last two steps:
            // - vsync (FIFO): one frame per vertical blank of the compositor (DwmGetCompositionTimingInfo), built just before
            //   it and showing that blank's time (a 60 Hz display gets 60 distinct states from the 30 Hz steps);
            // - no vsync: at the cap's period (sleeping), or as fast as frames are built without a cap.
            window.SkipFrame(std::chrono::nanoseconds(16'666'667), std::chrono::nanoseconds(66'666'667)); // catch up to 4 fields
            const bool racing = phase == Phase::kRacing && !(replaying && replayOver());
            const auto capPeriod = graphics.frameCap > 0 ? std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / graphics.frameCap))
                                                         : Clock::duration::zero();
            const int presentMode = int(renderer.PresentMode()); // VkPresentModeKHR: 2 FIFO, 3 FIFO_RELAXED
            Clock::time_point vblank;
            Clock::duration refresh{};
            const bool vsyncPaced = (presentMode == 2 || presentMode == 3) && window.VBlankTiming(vblank, refresh);
            bool fresh = true; // `items` = the field's own frame (fieldAlpha, built for its present right away)
            for (;;) {
                const auto t = Clock::now();
                Clock::time_point slot = std::max(t, nextPresent);
                if (vsyncPaced) { // the first blank the frame can still be built for
                    const auto earliest = std::max(t + seconds(buildEstimate) + std::chrono::microseconds(500), nextPresent);
                    const auto blanks = (earliest - vblank + refresh - Clock::duration(1)) / refresh;
                    slot = vblank + refresh * std::max<long long>(blanks, 0);
                }
                const bool rebuild = racing && (!fresh || slot > t);
                // A slot whose build would start after the next field is due belongs to the next field (the cadence carries
                // on across fields: the next field's frame takes it, with the newer state); a slot before it is presented
                // even when its build runs a little into the next field (the steps keep their scheduled times, stepAnchor).
                if (window.NextField() <= slot - (vsyncPaced ? seconds(buildEstimate) : Clock::duration::zero())) break;
                double a = fieldAlpha;
                const auto buildStart = rebuild ? std::max(Clock::now(), slot - seconds(buildEstimate)) : Clock::now();
                window.SleepUntil(buildStart);
                if (rebuild) {
                    a = alphaAt(std::max(slot, buildStart + seconds(buildEstimate))); // the state of the time it is presented
                    buildFrame(a, false);
                    // a hitch (page-in, a first-use pipeline) must not stop the in-between frames: at most a third of a field
                    const double built = std::min(std::chrono::duration<double>(Clock::now() - buildStart).count(), 0.0055);
                    buildEstimate = built > buildEstimate ? built : buildEstimate * 0.9 + built * 0.1;
                    if (!vsyncPaced) window.SleepUntil(slot); // with vsync the present itself waits for the blank
                }
                const auto presentStart = Clock::now();
                window.Present(items, sceneCount);
                const auto after = Clock::now();
                frameLog.Presented(!fresh, a, steps, std::chrono::duration<double, std::milli>(presentStart - buildStart).count(),
                                   std::chrono::duration<double, std::milli>(after - presentStart).count(), renderer.GpuMilliseconds());
                // the cap's cadence: a slot delayed by the field's work is made up by the next one; no catch-up bursts beyond that
                if (vsyncPaced) nextPresent = slot + std::max(refresh / 2, capPeriod - refresh / 2); // the next blank (cap: the blank at the period)
                else nextPresent = capPeriod > Clock::duration::zero() ? std::max(slot + capPeriod, after - capPeriod / 2) : after;
                fresh = false;
                if (!presentModeLogged) {
                    static const char* const kModes[] = {"IMMEDIATE", "MAILBOX", "FIFO (vsync)", "FIFO_RELAXED"};
                    const int m = int(renderer.PresentMode());
                    std::printf("render: present mode %s, MSAA %ux\n", m >= 0 && m < 4 ? kModes[m] : "other", renderer.EffectiveMsaa());
                    presentModeLogged = true;
                }
                if (!racing) break; // nothing moves: one frame per field
            }
            window.SleepUntil(window.NextField());
        }
        if (config.exitFade) {
            shownItems = items;
            shownScene = sceneCount;
        }
        if (capture) {
            std::printf("wrote %s after %d steps:\n", config.shotPath.c_str(), steps);
            PrintTelemetry(race, 0, steps);
            const camera::RaceCamera& c = raceCamera.Camera(); // compare with the original's camera object (0x801FF97C + 0xB8)
            std::printf("camera: position %u%s, eye (%d %d %d), H %d, car (%d %d %d)\n", unsigned(c.position), c.lookBack ? " back" : "", c.view.t[0], c.view.t[1],
                        c.view.t[2], int(c.H), race.Pose(0).worldPosition[0], race.Pose(0).worldPosition[1], race.Pose(0).worldPosition[2]);
            result.exit = RaceExit::kShot;
            break;
        }

        if (frame % 30 == 0) {
            const sim::CarTelemetry t = race.Telemetry(0);
            const sim::CarBody& body = race.CarAt(0).body;
            const int lap = body.lap - ((body.flags78D & 2) ? 1 : 0);
            const uint32_t hold = race.HoldFrames();
            char state[64];
            if (phase == Phase::kPreRace || phase == Phase::kSettings) std::snprintf(state, sizeof(state), "pre-race");
            else if (hold != 0) std::snprintf(state, sizeof(state), "start in %u", (hold + 59) / 60);
            else if (body.finishFlag != 0) std::snprintf(state, sizeof(state), "FINISHED P%u", t.racePosition);
            else std::snprintf(state, sizeof(state), "lap %d/%u P%u", lap < 1 ? 1 : lap, activeOptions.laps, t.racePosition);
            char title[240];
            if (!highRate) {
                std::snprintf(title, sizeof(title), "gt2game - %s - %s - %.0f km/h - %d rpm - gear %u - %s - last lap %s - %s", trackName.c_str(), carId.c_str(),
                              t.forwardSpeed / 4096.0 * 3.6, t.rpm, t.gear, state, ShellLog::Time(PlayerLastLap(race)).c_str(), ShellLog::Time(int32_t(race.RaceClock() / 3)).c_str());
            } else { // the display frame rate: SetWindowText waits for the compositor (~20 ms) - only when the lap / position changes
                std::snprintf(title, sizeof(title), "gt2game - %s - %s - %s - last lap %s", trackName.c_str(), carId.c_str(), state,
                              ShellLog::Time(PlayerLastLap(race)).c_str());
            }
            if (windowTitle != title) {
                windowTitle = title;
                window.SetTitle(title);
            }
        }
    }
    result.steps = std::max(result.steps, steps);
    writeReplay(); // --replay-out when the race was left before the recorder's end
    if (exitFade && !shownItems.empty()) PlayTitleExitFade(window, shownItems, shownScene); // the scene's resources still alive
    window.SetDrivingActive(false);
    return result;
}

} // namespace gt2game
