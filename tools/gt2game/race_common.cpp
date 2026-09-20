// The race building blocks of gt2game (see race_common.h), moved out of main.cpp unchanged in behaviour.
#include "race_common.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>

#include "game/career/career_state.h"
#include "game/audio/race_music.h"
#include "game/sim/trig.h"
#include "gt2formats/arcade_data.h"
#include "gt2formats/car_info.h"
#include "gt2formats/car_model.h"
#include "gt2formats/car_params.h"
#include "gt2formats/car_texture.h"
#include "gt2formats/course_data.h"
#include "gt2formats/exe_profile.h"
#include "gt2formats/gtmode_tables.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/race_capture.h"
#include "gt2formats/xa_audio.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "mods.h"
#include "platform/input/ps1_pad.h"

using namespace gt2;

namespace gt2game {

float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 Normalize(Vec3 a) { float l = std::sqrt(Dot(a, a)); return l > 1e-9f ? a * (1 / l) : Vec3{0, 1, 0}; }

void Multiply(const float* a, const float* b, float* out) { // column-major out = a * b
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    std::copy(r, r + 16, out);
}

const float kVerticalFovDegrees = 50.0f;
// The same view as a PS1 projection distance on the 240-line frame (for the original's scenery LOD rule).
const float kProjectionDistance = 120.0f / std::tan(kVerticalFovDegrees * 3.14159265f / 360.0f);

void ViewProjection(Vec3 eye, Vec3 target, float aspect, float* out) {
    Vec3 f = Normalize(target - eye), s = Normalize(Cross(f, {0, 1, 0})), u = Cross(s, f);
    float view[16] = {s.x, u.x, -f.x, 0, s.y, u.y, -f.y, 0, s.z, u.z, -f.z, 0, -Dot(s, eye), -Dot(u, eye), Dot(f, eye), 1};
    // Reversed Z with an infinite far plane (vk_scene_renderer.h): z_ndc = zn / distance.
    const float zn = 0.2f, t = 1.0f / std::tan(kVerticalFovDegrees * 3.14159265f / 360.0f);
    float proj[16] = {t / aspect, 0, 0, 0, 0, -t, 0, 0, 0, 0, 0, -1, 0, 0, zn, 0};
    Multiply(proj, view, out);
}

// The render transform of the simulation (16.16 m world position, s16 3x3 with 4096 = 1.0, columns = the model's
// +X right, +Y up, -Z front) as a column-major float model matrix in metres.
void ModelMatrix(const sim::CarPose& pose, float* m) {
    for (int col = 0; col < 3; col++)
        for (int row = 0; row < 3; row++) m[col * 4 + row] = float(pose.rotation[size_t(row)][size_t(col)]) / 4096.0f;
    m[3] = m[7] = m[11] = 0;
    for (int i = 0; i < 3; i++) m[12 + i] = float(pose.worldPosition[size_t(i)] / 65536.0);
    m[15] = 1;
}

Vec3 Position(const sim::CarPose& pose) {
    return {float(pose.worldPosition[0] / 65536.0), float(pose.worldPosition[1] / 65536.0), float(pose.worldPosition[2] / 65536.0)};
}
Vec3 Forward(const sim::CarPose& pose) { // -Z column of the rotation
    return Normalize({-pose.rotation[0][2] / 4096.0f, -pose.rotation[1][2] / 4096.0f, -pose.rotation[2][2] / 4096.0f});
}

// The car records of the race: slot 0 = the chosen car, the rest = the attract race's line-up (or `opponents`, --ai-cars),
// all in their stock configuration, assembled from carparam/usa_gtmode_data.dat exactly like the shell does (0x800771AC).
// With --mods, a slot comes through the override layer when the mod directory has a file for its car (mods.h).
void BuildCarRecords(const GtfsVolume& vol, const std::string& playerCar, size_t carCount, const std::string& modsDir, RaceData& data,
                     const std::vector<std::string>& opponents) {
    static const char* const kAttractCars[] = {"us36n", "ulcun", "ulrrn", "cc69n", "ulsbn", "ulrrn"};
    const CarParamTables tables = CarParamTables::Load(vol);
    for (size_t slot = 0; slot < carCount; slot++) {
        const std::string id = slot == 0 ? playerCar : slot - 1 < opponents.size() ? opponents[slot - 1] : kAttractCars[slot % 6];
        if (!modsDir.empty()) {
            if (AddModCarRecord(vol, tables, modsDir, id, slot, data)) continue;
            if (slot == 0 || slot - 1 < opponents.size()) std::printf("mods: no %s\\cars\\%s.json, using the disc car\n", modsDir.c_str(), id.c_str());
        }
        CarConfig config = StockCarConfig(tables, PackCarId(id));
        data.bodies.push_back(CarBodyOf(vol, tables, config));
        data.params.push_back(BuildCarParams(vol, tables, config));
        data.configs.push_back(config);
        data.carIds.push_back(id);
        data.paints.push_back(0);
        audio::CarSoundSetup sound; // the builder wrote engineWord / exhaustByte / the turbo flag into the config
        sound.soundId = config.engineWord;
        sound.exhaustByte = config.exhaustByte;
        sound.turbo = (config.flags & 2) != 0;
        data.sound.push_back(sound);
    }
}

// The shell's race settings block 0x801C98A0 as a licence test row carries it (+0x44, 0x40 bytes; disc_data.h
// RaceSettings names the bytes the constants read).
static sim::RaceSettings SettingsFromBlock(const std::array<uint8_t, kLicenseSettingsSize>& b) {
    sim::RaceSettings s;
    s.controlWord = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    for (size_t i = 0; i < 8; i++) s.aiGripPercent[i] = b[0x04 + i];
    for (size_t i = 0; i < 4; i++) s.cornerGripPercent[i] = b[0x0C + i];
    for (size_t i = 0; i < 4; i++) s.speedScalePercent[i] = b[0x10 + i];
    s.wearLimit = b[0x1D];
    s.wornGripLossPercent = b[0x1E];
    s.pitGripPercent = b[0x1F];
    s.coldLimit = b[0x20];
    s.coldGripLossPercent = b[0x21];
    s.wearKnee = b[0x22];
    s.kneeGripLossPercent = b[0x23];
    return s;
}

// The shell state of a licence test (the dump of B-1: 0x801D5866 = 3, no demo flag, countdown on).
static sim::ShellState LicenseShellState() {
    sim::ShellState s;
    s.gameMode = 3;
    return s;
}

// The shell state of a replay's race block (0x801D585C..: gt2formats/replay.h ReplayFile::raceBlock), played back: the
// demo / replay flag 0x800A951C set.
static sim::ShellState ReplayShellState(const ReplayFile& replay) {
    sim::ShellState s;
    const auto& b = replay.raceBlock;
    s.modeFlag5D = b[0x1];
    s.modeFlag60 = b[0x4];
    s.frameRateMode = b[0x8];
    s.modeFlag65 = b[0x9];
    s.gameMode = b[0xA];
    s.byte801D5869 = b[0xD];
    s.flag800A951C = 1;
    return s;
}

static sim::ShellState AttractShellState() {
    sim::ShellState s;
    s.gameMode = 2;
    s.flag800A951C = 1;
    return s;
}

// The disc-derived simulation data of `trackName`: the overlay / executable constants for a single-player race
// (disc_data.h defaults) and the course's race data.
void LoadDiscData(const DiscImage& disc, const GtfsVolume& vol, const std::string& trackName, const std::vector<uint8_t>& tro, const DataOptions& options,
                  RaceData& data) {
    const CourseInfoTable info = ParseCourseInfo(vol.Read(".crsinfo"));
    CourseSource source;
    source.courseIndex = info.FindByFileName(trackName);
    if (source.courseIndex >= 0) {
        source.haveFlags = true;
        source.flags = info.entries[size_t(source.courseIndex)].flags;
    }
    source.race = ParseTrackRaceData(tro);
    LoadCourseData(disc, vol, trackName, source, options, data);
}

void LoadCourseData(const DiscImage& disc, const GtfsVolume& vol, const std::string& trackName, const CourseSource& source, const DataOptions& options,
                    RaceData& data) {
    (void)vol;
    const GuestImage exe = LoadExeImage(disc);
    const GuestImage overlay = LoadOverlayImage(disc, kRaceOverlayIndex);
    data.courseIndex = source.courseIndex;
    const bool dirt = source.haveFlags && (source.flags & 4) != 0;
    if (!source.haveFlags) std::printf("warning: %s has no .crsinfo entry; treated as a tarmac course\n", trackName.c_str());
    // The original's arc-tangent table (the generated one of trig.h differs by one unit in a few entries).
    static std::vector<int16_t> atan(4097);
    const std::span<const int16_t> table = sim::AtanTableOf(exe);
    std::copy(table.begin(), table.end(), atan.begin());
    sim::AtanTableOverride() = options.generatedAtan ? nullptr : atan.data();
    const sim::RaceSettings settings = options.license ? SettingsFromBlock(options.license->settings)
                                       : options.eventSettings ? SettingsFromBlock(*options.eventSettings)
                                                               : sim::RaceSettings{};
    sim::ShellState shell = options.shell      ? *options.shell
                            : options.license  ? LicenseShellState()
                            : options.replay   ? ReplayShellState(*options.replay)
                            : options.attractShell ? AttractShellState()
                                                   : sim::ShellState{};
    // Settings + 0 = the start speed (km/h): the menus clear 0x801D5869 (race block +0x0D) for a rolling start
    // (0x80010078), so 0x80033384 runs 0x8003311C and the start hold is off. A shell state given with the race block keeps the
    // block's byte (the 2 player Battle's block has + 0x0D = 1 with the "ATT" settings' 80 km/h: a standing start).
    const uint8_t startSpeed = options.license ? options.license->settings[0] : options.eventSettings ? (*options.eventSettings)[0] : 0;
    if (startSpeed != 0 && !options.shell) shell.byte801D5869 = 0;
    data.constants = sim::LoadSimConstants(overlay, exe, settings, shell, dirt);
    { // 0x8003C12C -> 0x80041E4C: the AI catch-up tuning from the settings block (enabled in game modes 2 / 4 / 0xC)
        const std::array<uint8_t, kLicenseSettingsSize>* block = options.license ? &options.license->settings : options.eventSettings;
        const uint8_t mode = data.constants.gameMode;
        if (block) data.constants.catchUp = sim::CatchUpFromSettings(*block, mode == 2 || mode == 4 || mode == 0xC);
    }
    data.course = sim::BuildRaceCourseData(data.track, data.extras, source.race, overlay, dirt);
    // The race shell's tables of the overlay: championship points by finishing position (0x8002F4CC) and the HUD
    // caption tokens of the sector splits (0x8002F4BC); the course's point-to-point flag (.crsinfo flags bit 5).
    // Simulation addresses translated to the disc's build (gt2formats/exe_profile.h; US Arcade v1.1: 0x54 lower).
    const uint32_t points = overlay.Sim(0x8002F4CCu), labels = overlay.Sim(0x8002F4BCu);
    for (uint32_t i = 0; i < 6; i++) data.shell.pointsByPosition[i] = overlay.Get<uint8_t>(points + i);
    for (uint32_t i = 0; i < 4; i++) data.shell.splitLabels[i] = overlay.Get<uint32_t>(labels + i * 4);
    data.shell.lapTimeLabel = exe.Sim(sim::kLapTimeLabel); // "Lap Time" of the build's race text copy
    data.shell.pointToPoint = source.haveFlags && (source.flags & 0x20) != 0;
    if (options.license) { // the test's bytes of the settings block (race_shell.h LicenseTest)
        data.shell.license.targetLap = options.license->TargetLap();
        data.shell.license.type = options.license->Type();
        data.shell.license.boxStart = options.license->BoxStart();
        data.shell.license.boxLength = options.license->BoxLength();
    }
}

// The licence car of a test: the race builders' car slot 0 (license_data.h LicenseRaceCar: 0x80010078 / 0x8004C7A0 ->
// 0x800768C0 + 0x80076FC0, racing-modification body, paint code) and the record 0x800771AC builds from it
// (verify_license.cpp LicSpec / LicBuild: equal to the original for every test). A test's slot 1 is never read by the
// original (one car; license.md section 5).
void BuildLicenseCar(const GtfsVolume& vol, const LicenseData& lic, const LicenseTest& test, RaceData& data) {
    // Settings + 0 = a rolling start speed in km/h (0x80033384 -> 0x8003311C; LoadDiscData clears 0x801D5869 for it).
    if (test.settings[0] != 0) std::printf("licence %s: rolling start at %u km/h (0x8003311C)\n", test.name.c_str(), test.settings[0]);
    const LicenseRaceCar car = lic.RaceCar(test);
    CarConfig config = car.config;
    data.bodies.push_back(CarBodyOf(vol, lic.Tables(), config));
    data.params.push_back(BuildCarParams(vol, lic.Tables(), config));
    data.configs.push_back(config);
    data.gridSlots.assign(1, 0); // the licence's race entry: grid slot 0 (0x801D5945 in work/re/license_race)
    data.carIds.push_back(UnpackCarId(car.carId));
    int paint = 0; // the index of the slot's paint character in the car's .cdp list (0 when absent)
    try {
        paint = std::max(0, ParseCarTexture(vol.Read("carobj/" + UnpackCarId(car.carId) + ".cdp")).PaintIndex(uint8_t(car.paint)));
    } catch (const std::exception&) {
        paint = 0;
    }
    data.paints.push_back(uint32_t(paint));
    audio::CarSoundSetup sound;
    sound.soundId = config.engineWord;
    sound.exhaustByte = config.exhaustByte;
    sound.turbo = (config.flags & 2) != 0;
    data.sound.push_back(sound);
}

void LoadRaceTrack(const DiscImage& disc, const GtfsVolume& vol, const std::string& trackName, const DataOptions& options, RaceData& data) {
    const std::vector<uint8_t> tro = vol.Read("crsobj/" + trackName + ".tro");
    data.trackName = trackName;
    data.track = ParseTrack(tro);
    data.extras = sim::BuildCourseExtras(data.track, tro);
    LoadDiscData(disc, vol, trackName, tro, options, data);
}

CarBodyDimensions CarBodyOf(const GtfsVolume& vol, const CarParamTables& tables, const CarConfig& config) {
    const RacingModifyRow& rm = tables.RowAs<RacingModifyRow>(kTableRacingModify, config.racingModify); // as BuildCarParams(vol, ...)
    return BodyDimensionsOf(ParseCarModel(vol.Read("carobj/" + UnpackCarId(rm.modelId) + ".cdo")));
}

void AddCarRecord(const GtfsVolume& vol, const CarParamTables& tables, uint32_t carId, CarConfig config, uint32_t paint, RaceData& data) {
    data.bodies.push_back(CarBodyOf(vol, tables, config));
    data.params.push_back(BuildCarParams(vol, tables, config));
    data.configs.push_back(config);
    data.carIds.push_back(UnpackCarId(carId));
    data.paints.push_back(paint);
    audio::CarSoundSetup sound; // the builder wrote engineWord / exhaustByte / the turbo flag into the config
    sound.soundId = config.engineWord;
    sound.exhaustByte = config.exhaustByte;
    sound.turbo = (config.flags & 2) != 0;
    data.sound.push_back(sound);
}

void ApplyIntroHold(const DiscImage& disc, RaceData& data, const RaceOptions& options) {
    const std::vector<MusicTrack> tracks = ReadMusicTable(LoadExeImage(disc));
    audio::RaceMusicInputs inputs;
    inputs.gameMode = data.constants.gameMode;
    inputs.demoFlag = data.constants.flag800A951C;
    audio::RaceMusicBytes m;
    uint32_t random = 0;
    uint16_t holdInitial = 0, hold = options.countdown ? 1 : 0; // the shell's hold is non-zero exactly with the countdown
    audio::InitRaceMusic(m, random, inputs, tracks, holdInitial, hold);
    if (hold > 1) data.shell.introFields = hold;
}

// --scan-courses: every course of the VOL through the parsers and the race-data builder; one line per course.
int ScanCourses(const DiscImage& disc, const GtfsVolume& vol) {
    const GuestImage exe = LoadExeImage(disc);
    const GuestImage overlay = LoadOverlayImage(disc, kRaceOverlayIndex);
    const CourseInfoTable info = ParseCourseInfo(vol.Read(".crsinfo"));
    int failures = 0, courses = 0;
    for (const GtfsEntry& f : vol.Files()) {
        if (f.path.rfind("crsobj/", 0) != 0 || f.path.find(".tro") == std::string::npos) continue;
        const std::string name = f.path.substr(7, f.path.find('.') - 7);
        courses++;
        try {
            const std::vector<uint8_t> tro = vol.Read(f);
            const Track track = ParseTrack(tro);
            const sim::CourseExtras extras = sim::BuildCourseExtras(track, tro);
            const int index = info.FindByFileName(name);
            const bool dirt = index >= 0 && info.entries[size_t(index)].IsDirt();
            const TrackRaceData file = ParseTrackRaceData(tro);
            const sim::SimConstants constants = sim::LoadSimConstants(overlay, exe, sim::RaceSettings{}, sim::ShellState{}, dirt);
            const sim::RaceCourseData course = sim::BuildRaceCourseData(track, extras, file, overlay, dirt);
            size_t records = 0, offRoad = 0, lists = 0;
            for (size_t li = 0; li < 7; li++) {
                lists += file.present[li] ? 1 : 0;
                for (const sim::RaceSection& s : course.sections[li]) { records++; offRoad += (li != 5 && s.bank == 0 && s.gradient == 0 && s.reserved0C == 0 && s.surface == 0) ? 1 : 0; }
            }
            std::printf("%-20s %-28s %3zu chunks %6d m  lists %zu/%u  records %3zu (%zu without road probe)  start lines %zu  grid %2d%s%s\n", name.c_str(),
                        index >= 0 ? info.entries[size_t(index)].name.c_str() : "(no .crsinfo entry)", track.chunks.size(), track.lengthMetres, lists, file.listCount, records,
                        offRoad, course.startLineDistances.size(), course.grid.count, dirt ? "  dirt" : "", constants.wear.wearLimit ? "  wear" : "");
        } catch (const std::exception& e) {
            std::printf("%-20s FAIL: %s\n", name.c_str(), e.what());
            failures++;
        }
    }
    std::printf("%d courses, %d failure(s)\n", courses, failures);
    return failures;
}

// --dump: the disc-derived data against the dump (built with the dump's own settings so that the constants must
// match byte for byte; the shell-state members are runtime inputs and are only reported).
int CrossCheckDump(const DiscImage& disc, const std::string& trackName, RaceData& data) {
    const GuestImage exe = LoadExeImage(disc);
    const GuestImage overlay = LoadOverlayImage(disc, kRaceOverlayIndex);
    const sim::SimConstants fromDisc = sim::LoadSimConstants(overlay, exe, data.dump.settings, data.dump.shell, data.dump.course.dirtCourse);
    const std::vector<std::string> constants = sim::dev::DiffSimConstants(fromDisc, data.dump.constants);
    for (const std::string& line : constants) std::printf("  constants MISMATCH %s\n", line.c_str());
    std::printf("dump cross-check: constants %zu mismatch(es)", constants.size());
    if (data.dump.courseIndex == data.courseIndex) {
        const std::vector<std::string> course = sim::dev::DiffRaceCourseData(data.course, data.dump.course);
        for (const std::string& line : course) std::printf("\n  race data MISMATCH %s", line.c_str());
        std::printf(", race data %zu mismatch(es)", course.size());
        for (const std::string& line : sim::dev::DiffShellState(data.constants, data.dump.constants)) std::printf("\n  shell state (runtime input, informational) %s", line.c_str());
        std::printf("\n");
        return int(constants.size() + course.size());
    }
    std::printf("; the dump's course is %u (%s is %d), race data not compared\n", data.dump.courseIndex, trackName.c_str(), data.courseIndex);
    return int(constants.size());
}

std::vector<sim::RaceSlot> GridSlots(const RaceData& data, size_t carCount, const RaceOptions& options) {
    const sim::NativeCourse course(data.track, data.extras);
    std::vector<sim::RaceSlot> slots;
    if (data.bodies.size() < carCount) throw std::runtime_error("race: no body model dimensions for every car (RaceData::bodies)");
    for (size_t i = 0; i < carCount; i++) {
        // 0x80012CD4: the entry's grid slot, the car moved back by its model's nose (0x80017E74).
        const size_t gridSlot = i < data.gridSlots.size() ? data.gridSlots[i] : i;
        const CarBodyDimensions& body = data.bodies[i];
        sim::RaceSlot slot = sim::SlotFromTrackGrid(data.track, course, gridSlot, sim::CarNoseOffset(body.bboxFront, body.scaleShift));
        // The entries (a replay's slots) or: slot 0 the player-1 entry (the shell keeps its results), the rest AI entries
        // (0x80012CD4); --ai-player lets the AI drive the player's entry.
        slot.entryKind = i < data.entryKinds.size() ? data.entryKinds[i] : i == 0 ? sim::kEntryPlayer1 : sim::kEntryAi;
        // --ai-player: the AI drives the player entries (player 1, and player 2 of a 2 player Battle - the capture aid switches both).
        const bool playerEntry = i == 0 || slot.entryKind == sim::kEntryPlayer2;
        slot.controlClass = (playerEntry && options.aiPlayer) ? 2 : sim::EntryControlClass(slot.entryKind);
        slot.transmission = i < data.transmissions.size() ? data.transmissions[i] : options.manual ? 1 : 0;
        if (slot.entryKind == sim::kEntryPlayer1) slot.dirtLevel = data.dirtLevel; // 0x80012CD4: 0x801D58B4 -> body + 0x658
        slot.gridOffset = sim::HandicapGridOffset(slot.entryKind, data.handicap, data.constants.gameMode); // 0x80012CD4: race block + 6
        slots.push_back(slot);
    }
    return slots;
}

void SetupRace(sim::RaceSim& race, const RaceData& data, size_t carCount, const RaceOptions& options) {
    std::vector<sim::CarParams> params(data.params.begin(), data.params.begin() + std::ptrdiff_t(carCount));
    sim::RaceShellOptions shell = data.shell;
    shell.lapCount = options.laps;
    shell.countdown = options.countdown;
    race.Setup(data.track, data.extras, data.constants, data.course, std::move(params), GridSlots(data, carCount, options), shell);
}

std::string FormatMs(int32_t ms) {
    char text[32];
    if (ms < 0) return "--:--.---";
    std::snprintf(text, sizeof(text), "%d:%02d.%03d", ms / 60000, (ms / 1000) % 60, ms % 1000);
    return text;
}

// The outcome of a licence test as the shell records it (0x800156EC: code 0x801D5DEC, time 0x801D5DF0) and the prize
// of the results screen 0x8002B170: only for code 1, time < medal 1 / 2 / 3 time (0x8003D7B8, unsigned) = gold /
// silver / bronze. Its fourth prize (time < medal 4 and a condition on the licence's save record) is not evaluated.
std::string LicenseVerdict(const LicenseTest& test, int32_t result, uint32_t time) {
    static const char* const kCodes[] = {"", "passed", "", "failed: past the end of the box", "failed: off the course / loose surface", "failed: hit the wall"};
    std::string text = (result >= 1 && result <= 5 && kCodes[result][0]) ? kCodes[result] : ("result " + std::to_string(result));
    if (result == sim::kLicenseResultPass) {
        text += " at " + FormatMs(int32_t(time));
        const char* prize = "no prize";
        if (time < test.MedalTime(3)) prize = "BRONZE";
        if (time < test.MedalTime(2)) prize = "SILVER";
        if (time < test.MedalTime(1)) prize = "GOLD";
        text += std::string(" -> ") + prize + " (gold " + FormatMs(int32_t(test.MedalTime(1))) + ", silver " + FormatMs(int32_t(test.MedalTime(2))) + ", bronze " +
                FormatMs(int32_t(test.MedalTime(3))) + ")";
    }
    return text;
}

// Console log of the shell's events (race_shell.h ShellHooks) and the lap times it reports, per car; the positions of
// all cars when the player finishes (0x80013824 reads the positions there for the championship points).
void ShellLog::Attach(sim::RaceSim& raceSim) {
    race = &raceSim;
    cars.assign(raceSim.CarCount(), CarLaps{});
    positionsAtPlayerFinish.clear();
    sim::ShellHooks& h = raceSim.Shell().Hooks();
    h.user = this;
    h.lapLine = [](void* user, int car, int lap, int32_t lapTime, int32_t elapsed) {
        ShellLog& log = *static_cast<ShellLog*>(user);
        log.cars[size_t(car)].lapTimes.push_back(lapTime);
        if (!log.quiet) std::printf("lap: car %d completed lap %d in %s (race time %s)\n", car, lap, Time(lapTime).c_str(), Time(elapsed).c_str());
    };
    h.split = [](void* user, int car, int sector, int32_t split) {
        ShellLog& log = *static_cast<ShellLog*>(user);
        if (!log.quiet && car == 0) std::printf("split: car %d sector %d at %s\n", car, sector + 1, Time(split).c_str());
    };
    h.finished = [](void* user, int car, int position, int32_t elapsed) {
        ShellLog& log = *static_cast<ShellLog*>(user);
        log.cars[size_t(car)].finishTime = elapsed;
        log.cars[size_t(car)].position = position;
        if (!log.quiet) std::printf("finish: car %d finished in position %d, total %s\n", car, position, Time(elapsed).c_str());
        if (car != 0 || !log.race) return;
        log.positionsAtPlayerFinish.clear();
        for (size_t i = 0; i < log.race->CarCount(); i++) log.positionsAtPlayerFinish.push_back(int32_t(int8_t(log.race->CarAt(i).body.racePosition)));
    };
    h.license = [](void* user, int car, int32_t result, uint32_t time) {
        ShellLog& log = *static_cast<ShellLog*>(user);
        if (log.quiet) return;
        std::printf("licence: car %d %s\n", car, log.license ? LicenseVerdict(*log.license, result, time).c_str() : std::to_string(result).c_str());
    };
    h.sound = [](void* user, uint32_t routine, int32_t argument) {
        ShellLog& log = *static_cast<ShellLog*>(user);
        if (log.audio && routine == 0x800189C4u) log.audio->StartLight(argument);
        if (log.quiet) return;
        if (routine == 0x800189C4u) std::printf("start: %s\n", argument ? "green light (race music)" : "light");
    };
}

int32_t ShellLog::BestLap(size_t car) const {
    int32_t best = -1;
    for (int32_t t : cars[car].lapTimes)
        if (t >= 0 && (best < 0 || t < best)) best = t;
    return best;
}

// The standings from the race order table (0x801C8578) with the shell's laps and times. The lap counter
// (body + 0x608) is the lap in progress (1 after the start-line crossing at the start); completed laps = counter - 1.
// The player's last recorded lap from the shell's result record (0x801D5E88; -1 before the first lap).
int32_t PlayerLastLap(const sim::RaceSim& race) {
    const sim::PlayerResults& r = race.Shell().State().results[0];
    return r.count > 0 ? r.laps[r.count - 1].time : -1;
}

// The player's result record as the HUD / results screens read it (race_shell.h PlayerResults, 0x801D5E88).
void PrintPlayerResults(const sim::RaceSim& race) {
    const sim::PlayerResults& r = race.Shell().State().results[0];
    std::printf("player results (0x801D5E88): %d lap(s) recorded, next lap %d, position %d, finish %s\n", r.count, r.lapNumber, r.position, ShellLog::Time(r.finishTime).c_str());
    for (int i = 0; i < r.count && i < 10; i++)
        std::printf("  lap %d  %s  top %.1f mph\n", r.lapNumber - r.count + i + 1, ShellLog::Time(r.laps[i].time).c_str(),
                    uint16_t(r.laps[i].maxSpeed) / 100.0); // the readout body + 0x6F8: 1/100 mph
    std::printf("  best lap %s (lap %d)\n", ShellLog::Time(r.count > 0 ? r.best.time : -1).c_str(), r.bestLapNumber + 1);
}

void PrintStandings(const sim::RaceSim& race, const ShellLog& log, const std::vector<std::string>& carIds) {
    std::puts("standings:");
    for (size_t i = 0; i < race.CarCount(); i++) {
        const int car = race.RaceOrder()[i];
        const sim::CarBody& body = race.CarAt(size_t(car)).body;
        const int laps = body.lap - ((body.flags78D & 2) ? 1 : 0) - 1;
        const uint8_t finished = body.finishFlag;
        const ShellLog::CarLaps& l = log.cars[size_t(car)];
        std::printf("  P%zu car %d %-6s laps %d  best lap %s  total %s%s\n", i + 1, car, carIds[size_t(car)].c_str(), laps < 0 ? 0 : laps, ShellLog::Time(log.BestLap(size_t(car))).c_str(),
                    ShellLog::Time(l.finishTime).c_str(), finished ? "  finished" : "");
    }
}


void PrintTelemetry(const sim::RaceSim& race, size_t car, int step) {
    const sim::CarTelemetry t = race.Telemetry(car);
    const sim::CarPose pose = race.Pose(car);
    std::printf("step %4d (%5.2f s): %6.1f km/h  rpm %4d  gear %u  throttle %4d brake %4d steer %5d  heading %5d  pos %8.2f %7.2f %8.2f  dist %7.1f m  P%u%s\n", step,
                step / 30.0, t.forwardSpeed / 4096.0 * 3.6, t.rpm, t.gear, t.throttle, t.brake, t.steerAngle, t.heading, pose.worldPosition[0] / 65536.0,
                pose.worldPosition[1] / 65536.0, pose.worldPosition[2] / 65536.0, t.courseDistance / 65536.0, t.racePosition, t.wallHitMask ? "  WALL" : "");
}

// Scripted pad of the self-test: full throttle from the start, a short left steer input after 8 s.
sim::PadRecord ScriptedPad(int step) {
    sim::PadRecord pad{};
    pad.throttle = 1;
    if (step >= 240 && step < 250) pad.steer = 2;
    return pad;
}

// Headless test driver of a licence test (not the original's: a stand-in for the player's pad). Type 2 ("stop in the
// box"): full throttle, then full brakes once the stopping distance at `decel` reaches the middle of the box; other
// types: full throttle.
sim::PadRecord LicenseDriverPad(const sim::RaceSim& race, const LicenseTest& test, double decel) {
    sim::PadRecord pad{};
    pad.throttle = 1;
    if (test.Type() != 2) return pad;
    const int32_t length = race.Course().Extras().courseLength;
    int64_t distance = race.Telemetry(0).courseDistance;
    if (distance > length / 2) distance -= length; // the grid lies before the start line
    const double metres = double(distance) / 65536.0;
    const double target = test.BoxStart() * 10.0 + test.BoxLength() / 2.0;
    const double speed = race.Telemetry(0).forwardSpeed / 4096.0;
    if (speed > 0 && speed * speed / (2 * decel) >= target - metres) { pad.throttle = 0; pad.brake = 1; }
    return pad;
}

// --headless <seconds>: the race without a window: the shell's events on the console, the standings at the end
// (every car finished or the race-end wait of the shell over) or after `seconds` of race time.
int Headless(const RaceData& data, size_t carCount, const RaceOptions& options, double seconds) {
    sim::RaceSim race;
    SetupRace(race, data, carCount, options);
    ShellLog log;
    log.Attach(race);
    log.license = options.license;
    std::printf("headless race: %zu cars, %u lap(s), countdown %s, hold %u fields\n", carCount, options.laps, options.countdown ? "on" : "off", race.HoldFrames());
    std::vector<sim::PadRecord> pads(carCount);
    const int steps = int(seconds * 30);
    int step = 0;
    for (; step < steps; step++) {
        pads[0] = options.aiPlayer ? sim::PadRecord{} : options.license ? LicenseDriverPad(race, *options.license, options.licenseDecel) : ScriptedPad(step);
        race.Step(pads.data());
        if (step % 300 == 299) PrintTelemetry(race, 0, step + 1);
        bool allFinished = true;
        for (size_t car = 0; car < carCount; car++) allFinished &= race.CarAt(car).body.finishFlag != 0;
        if (allFinished || race.RaceTaskOver()) break;
    }
    std::printf("race clock %.2f s after %d steps%s\n", race.RaceClock() / 3000.0, step, race.RaceTaskOver() ? " (race task over)" : "");
    PrintStandings(race, log, data.carIds);
    PrintPlayerResults(race);
    if (options.license) {
        const sim::RaceShellState& s = race.Shell().State();
        const sim::CarBody& body = race.CarAt(0).body;
        std::printf("licence %s: shell result %d (0x801D5DEC), time %s (0x801D5DF0); car state %u (body + 0x751), course distance %.2f m\n", options.license->name.c_str(),
                    s.licenseResult, FormatMs(int32_t(s.licenseTime)).c_str(), body.licenseState, race.Telemetry(0).courseDistance / 65536.0);
        std::printf("licence %s: %s\n", options.license->name.c_str(), s.licenseResult < 0 ? "no result (the test did not end)" : LicenseVerdict(*options.license, s.licenseResult, s.licenseTime).c_str());
    }
    return 0;
}


// ================================================================ replays

void ReplayDriver::StartRecording(uint8_t raceMode) {
    mode = Mode::kRecord;
    gameMode = raceMode;
    stream = ReplayStream(ReplayStream::kPlayerCapacity); // 0x800163B8(0x801D5F84, 0, 0x4400) at the race load (0x80012CD4)
    ended = false;
}

void ReplayDriver::StartPlayback(std::span<const uint8_t> streamBytes, uint8_t raceMode) {
    mode = Mode::kPlay;
    gameMode = raceMode;
    stream = ReplayStream::FromBytes(streamBytes);
    stream.Init(true, ReplayStream::kPlayerCapacity); // 0x800163B8(0x801D5F84, 1, 0x4400)
    ended = false;
}

void ReplayDriver::Attach(sim::RaceSim& race) { race.SetPadSource(mode == Mode::kOff ? nullptr : &ReplayDriver::Source, this); }

void ReplayDriver::Source(void* user, sim::RaceSim& race, sim::PadRecord* pads) { // 0x8003C250 -> 0x80013EF0 -> 0x80013C90 (pad slot 2)
    ReplayDriver& self = *static_cast<ReplayDriver*>(user);
    for (size_t car = 0; car < race.CarCount(); car++) {
        if (race.CarAt(car).padSlot != 2) continue;
        ReplayFrame frame{};
        if (self.mode == Mode::kRecord) {
            frame = FrameOfPad(self.pad);
            const uint16_t limit = self.gameMode == 3 ? 60 : 300; // fields after the finish (0x800A9522)
            if (race.Shell().State().sinceFinish < limit) self.stream.Record(frame);
            else self.stream.End(false);
        } else {
            self.stream.Read(frame); // left zero once the stream has ended
            if (self.stream.Ended()) self.ended = true;
        }
        self.lastFrame = frame;
        race.NotePlayerFrame(frame); // game mode 6 records it into the ghost's lap stream
        pads[car] = PadOfFrame(frame, self.pedalTable);
    }
}

std::array<uint16_t, 16> PedalTable(const DiscImage& disc) {
    const GuestImage overlay = LoadOverlayImage(disc, kRaceOverlayIndex);
    std::array<uint16_t, 16> t{};
    const uint32_t table = overlay.Sim(0x8002F4D4u); // the table of the disc's build
    for (uint32_t i = 0; i < 16; i++) t[i] = overlay.Get<uint16_t>(table + i * 2);
    return t;
}

void LoadReplayRace(const DiscImage& disc, const GtfsVolume& vol, const ReplayFile& replay, RaceData& data, RaceOptions& options) {
    const std::string course = CourseNameOfId(vol, replay.CourseFileId());
    DataOptions o;
    o.replay = &replay;
    // The race settings block (0x801C98A0) of a replay: the event row of the race block's event name (+0x10) in
    // carparam/usa_gtmode_race.dat + 0x44 - what the title overlay copies for the attract race (member 1 0x800109C0:
    // 0x8007830C(*0x80092E70, race block + 0x10), then the record + 0x44 .. + 0x84 to 0x801C98A0).
    // Race block + 9 == 0 (an arcade race, e.g. the demo files' "Demo 04" on either disc): 0x80010EDC loads usa_arcade_data.dat
    // (0x80076E04) and takes the event row from its race table (*0x80092E6C, 0x80010C50); 0x800771AC then builds the cars with
    // the arcade car tables (ghost_replay.h).
    const bool arcadeRace = replay.raceBlock[9] == 0;
    std::optional<ArcadeData> arcadeData;
    if (arcadeRace) arcadeData.emplace(ArcadeData::Load(vol));
    const GtModeRaceData raceTable = GtModeRaceData::Load(vol);
    const int32_t eventRow = replay.GameMode() == 3 ? -1 : arcadeRace ? arcadeData->FindEvent(replay.EventName()) : raceTable.FindEvent(replay.EventName());
    std::array<uint8_t, kLicenseSettingsSize> eventSettings{};
    std::optional<LicenseData> licence;
    if (replay.GameMode() == 3) { // a licence test: the race block's name is the test (0x801D586C "LJB00"), its car from the licence file
        licence.emplace(LicenseData::Load(vol));
        data.replayLicense = std::make_shared<LicenseTest>(licence->Test(replay.EventName()));
        o.license = data.replayLicense.get();
        options.license = data.replayLicense.get();
    } else if (eventRow >= 0) {
        const RaceEvent event = arcadeRace ? arcadeData->EventAt(size_t(eventRow)) : raceTable.EventAt(size_t(eventRow));
        std::copy(event.settings.begin(), event.settings.end(), eventSettings.begin());
        o.eventSettings = &eventSettings;
    } else {
        std::printf("replay: event %s is not in usa_gtmode_race.dat; default race settings\n", replay.EventName().c_str());
    }
    // A 2 player Battle's record (game mode 0): the race load's rule for the mode (0x8003C12C, race_sim.h RaceLoadSettings) on the
    // event row's settings - Tire Damage = race block + 3, Slow Car Boost = + 7 and the catch-up enable (the theater's replay of
    // such a record, work/re/theater2p: the tyre wear of the Tire Damage option in the original's car records).
    bool battleCatchUp = false;
    if (replay.GameMode() == 0 && o.eventSettings) battleCatchUp = sim::RaceLoadSettings(0, replay.raceBlock[3], replay.raceBlock[7], eventSettings);
    LoadRaceTrack(disc, vol, course, o, data);
    if (replay.GameMode() == 0 && o.eventSettings) data.constants.catchUp = sim::CatchUpFromSettings(eventSettings, battleCatchUp); // 0x80041E4C
    const CarParamTables tables = licence ? licence->Tables() : arcadeData ? arcadeData->Tables() : CarParamTables::Load(vol);
    std::optional<CarParamTables> gtTables;
    const CarInfoDirectory carInfo = CarInfoDirectory::Load(vol);
    for (const ReplayEntry& e : replay.cars) {
        CarConfig config;
        std::memcpy(&config, e.slot.data() + 8, sizeof(config));
        // slot + 4: the paint code (a .carinfoa paint id: 'm' for the attract race's Shelby) -> the renderer's paint index
        const CarInfoRecord* info = carInfo.Find(e.carId);
        const int paint = info ? info->PaintIndex(e.slot[4]) : -1;
        // An arcade race's garage car (configuration + 0x7A bit 6, arcade_race.cpp LoadRaceBlock's rule) keeps the GT-mode tables.
        const bool gtEntry = arcadeData && (e.slot[8 + 0x7A] & 0x40) != 0;
        if (gtEntry && !gtTables) gtTables.emplace(CarParamTables::Load(vol));
        AddCarRecord(vol, gtEntry ? *gtTables : tables, e.carId, config, paint < 0 ? 0u : uint32_t(paint), data);
        data.gridSlots.push_back(e.gridSlot());
        data.entryKinds.push_back(e.kind());
        data.transmissions.push_back(e.transmission());
    }
    options.laps = replay.Laps();
    options.countdown = replay.Countdown() != 0;
    data.dirtLevel = uint16_t(replay.raceBlock[0x58] | replay.raceBlock[0x59] << 8); // 0x801D58B4 (the demo file's "Demo 03": 19)
    std::printf("replay: %s, %s, game mode %u, %u lap(s), %zu car(s), player stream %d frames\n", replay.EventName().c_str(), course.c_str(), replay.GameMode(),
                replay.Laps(), replay.cars.size(), ReplayStream::FromBytes(replay.stream).Frames());
}

ReplayFile BuildReplayFile(const GtfsVolume& vol, const RaceData& data, size_t carCount, const RaceOptions& options, const ReplayStream& stream) {
    if (data.configs.size() < carCount) throw std::runtime_error("replay: no configuration for every car (RaceData::configs)");
    ReplayFile r;
    auto& b = r.raceBlock;
    const sim::SimConstants& c = data.constants;
    b[0x4] = 1;                                   // 0x801D5860 / 0x801D5865: the mode flags of the dumps
    b[0x8] = c.step.rate == 60 ? 1 : 2;           // 0x801D5864 frame-rate mode
    b[0x9] = 1;
    b[0xA] = c.gameMode;                          // 0x801D5866
    b[0xD] = options.countdown ? 1 : 0;           // 0x801D5869
    b[0xF] = options.laps;                        // 0x801D586B
    auto putString = [&](size_t offset, size_t size, const std::string& text) {
        for (size_t i = 0; i + 1 < size && i < text.size(); i++) b[offset + i] = uint8_t(text[i]);
    };
    if (options.license) putString(0x10, 0x10, options.license->name); // 0x801D586C: the test ("LJB00") / event name
    const CourseInfoTable info = ParseCourseInfo(vol.Read(".crsinfo"));
    const CarInfoDirectory carInfo = CarInfoDirectory::Load(vol);
    if (data.courseIndex >= 0 && size_t(data.courseIndex) < info.entries.size()) putString(0x20, 0x20, info.entries[size_t(data.courseIndex)].name);
    const uint32_t courseId = CourseFileId(data.trackName);
    std::memcpy(b.data() + 0x40, &courseId, 4);   // 0x801D589C
    putString(0x44, 0x10, "General01");           // 0x801D58A0 sponsor category
    b[0x5A] = uint8_t(carCount);                  // 0x801D58B6
    for (size_t i = 0; i < carCount; i++) {
        ReplayEntry e;
        e.carId = PackCarId(data.carIds[i]);
        std::memcpy(e.slot.data(), &e.carId, 4);
        std::memcpy(e.slot.data() + 8, &data.configs[i], sizeof(CarConfig));
        if (const CarInfoRecord* carRecord = carInfo.Find(e.carId); carRecord && i < data.paints.size() && data.paints[i] < carRecord->paintIds.size())
            e.slot[4] = carRecord->paintIds[data.paints[i]]; // the paint code of the renderer's paint index
        e.slot[0x8C] = 1;
        e.slot[0x8D] = uint8_t(i < data.gridSlots.size() ? data.gridSlots[i] : i);
        e.slot[0x8E] = i < data.entryKinds.size() ? data.entryKinds[i] : i == 0 ? sim::kEntryPlayer1 : sim::kEntryAi;
        e.slot[0x8F] = uint8_t(i < data.transmissions.size() ? data.transmissions[i] : options.manual ? 1 : 0);
        const CarInfoRecord* named = carInfo.Find(e.carId);
        const std::string name = named && !named->name.empty() ? named->name : data.carIds[i]; // + 0x90: the display name (.carinfoa)
        for (size_t k = 0; k < name.size() && k < 63; k++) e.slot[0x90 + k] = uint8_t(name[k]);
        r.cars.push_back(e);
    }
    const std::vector<uint8_t>& bytes = stream.Bytes();
    const size_t end = std::min(bytes.size(), ReplayStream::kHeaderSize + size_t(stream.Capacity()));
    r.stream.assign(bytes.begin(), bytes.begin() + std::ptrdiff_t(end));
    return r;
}

namespace {

// A scripted logical pad for the replay check: throttle, steering pulses, a brake stretch, a shift up / down and the
// handbrake (digital, the default key configuration).
LogicalPad ScriptedLogicalPad(int step) {
    LogicalPad p;
    p.buttons = kPadThrottle;
    if (step % 90 >= 60 && step % 90 < 66) p.buttons |= (step / 90) % 2 ? kPadLeft : kPadRight;
    if (step >= 400 && step < 430) p.buttons = kPadBrake | kPadLeft;
    if (step == 200) p.buttons |= kPadShiftUp;
    if (step == 500) p.buttons |= kPadShiftDown;
    if (step >= 600 && step < 606) p.buttons |= kPadHandbrake;
    return p;
}

// The compared state: every car's simulated record part (body up to + 0x798 and + 0xA60..0xA78) and the contact tables.
std::vector<uint8_t> CarState(const sim::RaceSim& race) {
    std::vector<uint8_t> out;
    constexpr size_t kBody = offsetof(sim::Car, body);
    for (size_t i = 0; i < race.CarCount(); i++) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&race.CarAt(i));
        out.insert(out.end(), p, p + kBody + 0x798);
        out.insert(out.end(), p + kBody + 0xA60, p + kBody + 0xA78);
    }
    const uint8_t* c = reinterpret_cast<const uint8_t*>(&race.Contact());
    out.insert(out.end(), c, c + sizeof(sim::CarContactState));
    return out;
}

} // namespace

// --replay-check-analog: player 1's logical pad from a scripted analog controller through the ported pad chain (two fields
// of 0x8007FC30 per race frame, then 0x80014BB4 with the new game's key tables and our triggers profile): a stick swept
// left / right, the right trigger as the accelerator, the left one as the brake, R1 (view) and Cross (shift up) presses.
class AnalogPadScript {
public:
    explicit AnalogPadScript(const DiscImage& disc) : tables_(input::PadTables::Load(LoadExeImage(disc))) {
        const career::CareerState fresh = career::NewCareer(career::ReadNewGameDefaults(disc));
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&fresh) + 0x0A;
        std::copy(b, b + input::pad_block::kSize, block_.begin());
        std::copy(block_.begin(), block_.begin() + 44, keyTables_.begin());
        const std::array<uint8_t, 11> analog = input::TriggerPedalTable(keyTables_.data() + 11);
        std::copy(analog.begin(), analog.end(), keyTables_.begin() + 11);
        input::InitTracker(object_.data() + input::pad_object::kTracker, tables_);
    }
    LogicalPad Step(int step) {
        for (int k = 0; k < 2; k++) { // the race frame's two fields
            const int f = step * 2 + k;
            input::Ps1PadFrame frame;
            frame.type = input::kTypeAnalog;
            frame.pressure = true;
            const int phase = f % 240;
            frame.analog[2] = uint8_t(phase < 120 ? 64 + phase * 128 / 120 : 192 - (phase - 120) * 128 / 120); // 64..192: through the dead zone 91..164
            frame.analog[3] = uint8_t(0x80 + (f % 7) - 3);
            frame.pressureR2 = uint8_t(f % 600 < 440 ? std::min(255, (f % 600) * 3) : 0);
            frame.pressureL2 = uint8_t(f % 600 >= 470 ? 90 + (f % 37) * 4 : 0);
            if (f % 300 == 150) frame.buttons |= input::ps1::kR1;
            if (f % 360 >= 200 && f % 360 < 204) frame.buttons |= input::ps1::kCross;
            input::PollPad(object_.data(), frame, tables_, block_.data() + input::pad_block::kCalibration, 2);
        }
        input::BuildRaceLogical(object_.data(), keyTables_.data());
        auto u16 = [&](uint32_t o) { return uint16_t(object_[o] | object_[o + 1] << 8); };
        LogicalPad pad;
        pad.buttons = uint32_t(u16(input::pad_object::kLogical)) | uint32_t(u16(input::pad_object::kLogical + 2)) << 16;
        pad.analog = u16(input::pad_object::kAnalogFlags);
        pad.steerAxis = u16(input::pad_object::kValues);
        pad.throttle = u16(input::pad_object::kValues + 4);
        pad.brake = u16(input::pad_object::kValues + 6);
        return pad;
    }

private:
    input::PadTables tables_;
    std::array<uint8_t, input::pad_block::kSize> block_{};
    std::array<uint8_t, 44> keyTables_{};
    std::array<uint8_t, 0x200> object_{};
};

int ReplayCheck(const DiscImage& disc, const GtfsVolume& vol, const RaceData& data, size_t carCount, const RaceOptions& options, double seconds,
                const std::string& outPath, bool analogPad) {
    const int steps = int(seconds * 30);
    const std::array<uint16_t, 16> pedals = PedalTable(disc);
    int failures = 0;
    std::optional<AnalogPadScript> analogScript;
    if (analogPad) analogScript.emplace(disc);
    int analogFrames = 0;
    // 1. the live race, recorded
    sim::RaceSim live;
    SetupRace(live, data, carCount, options);
    ReplayDriver recorder;
    recorder.pedalTable = pedals;
    recorder.StartRecording(data.constants.gameMode);
    recorder.Attach(live);
    std::vector<sim::PadRecord> pads(carCount);
    std::vector<std::vector<uint8_t>> liveStates; // after 0, 1, ... steps
    liveStates.push_back(CarState(live));
    for (int step = 0; step < steps; step++) {
        recorder.pad = analogScript ? analogScript->Step(step) : ScriptedLogicalPad(step);
        live.Step(pads.data());
        if (recorder.lastFrame.flags & 7) analogFrames++;
        liveStates.push_back(CarState(live));
    }
    if (analogPad) std::printf("replay check: analog controller script, %d of %d frames with analogue flags\n", analogFrames, steps);
    ReplayStream recorded = recorder.stream;
    recorded.End(false);
    // The player 0x80016428 ends the stream at its last frame (frames left 1 -> 0 gives no frame), and a recording stops
    // 300 / 60 fields after the finish while the live car keeps its pad: the replay covers frames - 1 steps.
    const int replaySteps = std::min(steps, recorded.Frames() - 1);
    const std::vector<uint8_t>& liveState = liveStates[size_t(replaySteps)];
    std::printf("replay check: live race of %d steps, %d frames recorded in %u bytes; player at %.2f m, %.1f km/h\n", steps, recorded.Frames(), unsigned(recorded.Used()),
                live.Telemetry(0).courseDistance / 65536.0, live.Telemetry(0).forwardSpeed / 4096.0 * 3.6);
    // 2. the replay from the stream in memory
    auto replayOf = [&](const RaceData& d, size_t cars, const RaceOptions& o, std::span<const uint8_t> stream, const char* what) {
        sim::RaceSim race;
        SetupRace(race, d, cars, o);
        ReplayDriver player;
        player.pedalTable = pedals;
        player.StartPlayback(stream, d.constants.gameMode);
        player.Attach(race);
        std::vector<sim::PadRecord> none(cars);
        for (int step = 0; step < replaySteps; step++) race.Step(none.data());
        const std::vector<uint8_t> state = CarState(race);
        size_t differing = 0, first = SIZE_MAX;
        for (size_t i = 0; i < state.size() && i < liveState.size(); i++)
            if (state[i] != liveState[i]) {
                differing++;
                if (first == SIZE_MAX) first = i;
            }
        const bool same = state.size() == liveState.size() && differing == 0;
        std::printf("replay check: %s, %d steps: %zu state bytes, %s", what, replaySteps, state.size(), same ? "identical to the live race\n" : "DIFFER");
        if (!same) std::printf(" (%zu bytes, first at %zu)\n", differing, first);
        failures += same ? 0 : 1;
    };
    replayOf(data, carCount, options, recorded.Bytes(), "replay of the recorded stream");
    // 3. the saved file, loaded back and played
    const std::vector<uint8_t> file = WriteReplayFile(BuildReplayFile(vol, data, carCount, options, recorded), {});
    if (!outPath.empty()) {
        if (std::FILE* f = std::fopen(outPath.c_str(), "wb")) {
            std::fwrite(file.data(), 1, file.size(), f);
            std::fclose(f);
            std::printf("replay check: %zu bytes written to %s\n", file.size(), outPath.c_str());
        }
    }
    const ReplayFile parsed = ParseReplayFile(file);
    RaceData fromFile;
    RaceOptions fileOptions;
    LoadReplayRace(disc, vol, parsed, fromFile, fileOptions);
    fromFile.constants.flag800A951C = data.constants.flag800A951C; // the live race's shell flag (a replay sets it: results are not recorded)
    fromFile.constants.viewMode = data.constants.viewMode;
    fromFile.shell.introFields = data.shell.introFields;
    replayOf(fromFile, parsed.cars.size(), fileOptions, parsed.stream, "replay of the saved .gmr");
    std::printf("replay check: %d failure(s)\n", failures);
    return failures;
}

int FramesCompare(const RaceData& data, size_t carCount, const RaceOptions& options, const std::string& capturePath, const ReplayFile* replay,
                  const std::array<uint16_t, 16>& pedalTable, int maxFrames) {
    const std::vector<RaceCaptureFrame> all = ReadRaceCapture(capturePath);
    if (all.empty()) throw std::runtime_error(capturePath + ": no frames");
    std::vector<const RaceCaptureFrame*> frames;
    for (const RaceCaptureFrame& f : all)
        if (f.race == all[0].race) frames.push_back(&f);
    const RaceCaptureFrame& first = *frames[0];
    if (first.carCount != carCount) std::printf("frames: the capture's race has %u car(s), ours %zu\n", first.carCount, carCount);
    sim::RaceSim race;
    SetupRace(race, data, carCount, options);
    ReplayDriver driver;
    driver.pedalTable = pedalTable;
    if (replay) driver.StartPlayback(replay->stream, data.constants.gameMode);
    else driver.StartRecording(data.constants.gameMode);
    driver.Attach(race);
    std::printf("frames: %zu captured race frames (fields %u..%u, game mode %u, demo %u); hold at the start: original %u, ours %u\n", frames.size(), first.field,
                frames.back()->field, first.gameMode, first.demo, first.hold, race.HoldFrames());
    constexpr size_t kBody = offsetof(sim::Car, body);
    int differing = 0, shown = 0;
    size_t compared = 0;
    const size_t count = maxFrames > 0 ? std::min(frames.size(), size_t(maxFrames)) : frames.size();
    double worst = 0;
    for (size_t k = 0; k < count; k++) {
        const RaceCaptureFrame& f = *frames[k];
        bool same = true;
        std::string where;
        for (size_t car = 0; car < carCount && car < kRaceCaptureCars; car++) {
            const uint8_t* theirs = f.cars.data() + car * kRaceCaptureCarSize;
            const uint8_t* ours = reinterpret_cast<const uint8_t*>(&race.CarAt(car));
            // The simulated part of the record: the body up to + 0x798 and the shell's HUD request block + 0xA60..0xA78 (the
            // rest of the record is the render / sound / model block of 0x800133F0 and others, which RaceSim does not keep:
            // CarBody::reserved798 / reservedA78). The body's curve pointers are tokens relative to the body in ours
            // (bodyToken 0): a word equal to ours + the original's body address is the same pointer.
            const uint32_t bodyAddress = RaceAddress(0x800A9688u) + uint32_t(car) * uint32_t(kRaceCaptureCarSize) + uint32_t(kBody);
            auto word = [](const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; };
            size_t firstDiff = SIZE_MAX, diffs = 0;
            for (size_t i = kBody; i < kRaceCaptureCarSize; i++) {
                const size_t b = i - kBody;
                if (!(b < 0x798 || (b >= 0xA60 && b < 0xA78))) continue;
                if (theirs[i] == ours[i]) continue;
                const size_t w = i & ~size_t(3);
                if (word(theirs + w) == word(ours + w) + bodyAddress && word(ours + w) < 0xB40) continue; // a relocated pointer
                diffs++;
                if (firstDiff == SIZE_MAX) firstDiff = i;
            }
            const sim::CarBody& b = race.CarAt(car).body;
            sim::CarBody o;
            std::memcpy(&o, theirs + kBody, sizeof o);
            double d2 = 0;
            for (int i = 0; i < 3; i++) d2 += std::pow((double(b.position[i]) - double(o.position[i])) / 4096.0, 2);
            worst = std::max(worst, std::sqrt(d2));
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
            if (diffs) {
                same = false;
                char text[160];
                std::snprintf(text, sizeof text, " car %zu: %zu byte(s), first body + 0x%zX, position off %.4f m;", car, diffs, firstDiff - kBody, std::sqrt(d2));
                where += text;
            }
        }
        // player 1's stream: header + the coded bytes written so far
        const ReplayStream theirs = ReplayStream::FromBytes(std::span<const uint8_t>(f.stream.data(), f.stream.size()));
        const size_t streamBytes = std::min(f.stream.size(), ReplayStream::kHeaderSize + std::max(theirs.Used(), driver.stream.Used()));
        const bool streamSame = std::equal(driver.stream.Bytes().begin(), driver.stream.Bytes().begin() + std::ptrdiff_t(streamBytes), f.stream.begin());
        if (!streamSame) where += " stream differs;";
        compared++;
        if (!same || !streamSame) differing++;
        const sim::CarBody& p = race.CarAt(0).body;
        if (((!same || !streamSame) && shown++ < 12) || k % 60 == 0 || k + 1 == count)
            std::printf("  frame %4zu (field %5u) hold %3u/%3u  player at (%9.3f %9.3f %7.3f) %6.1f km/h  %s%s\n", k, f.field, f.hold, race.HoldFrames(),
                        p.position[0] / 4096.0, p.position[1] / 4096.0, p.position[2] / 4096.0, p.forwardSpeed / 4096.0 * 3.6, same && streamSame ? "= original" : "DIFFERS:",
                        where.c_str());
        driver.pad.buttons = f.padButtons;
        driver.pad.analog = f.padAnalog;
        driver.pad.steerAxis = f.padSteer;
        driver.pad.throttle = f.padThrottle;
        driver.pad.brake = f.padBrake;
        std::vector<sim::PadRecord> pads(race.CarCount());
        race.Step(pads.data());
    }
    std::printf("frames: %zu compared, %d differ (car bodies byte for byte + player 1's replay stream), worst position difference %.4f m\n", compared, differing, worst);
    return differing;
}

std::string DescribeCareer(const career::CareerState& s) {
    char text[160];
    std::snprintf(text, sizeof(text), "day %u, money %d cr, %d car(s), current car %d, races %d, wins %d, prize total %u", s.record.days, s.garage.money, s.garage.count,
                  s.garage.currentCar, s.record.races, s.record.wins, s.record.prizeTotal);
    return text;
}

// The course file name of a course file id (0x80060FB0: the "crsmap" base names hashed with 0x80083004; the same
// names as crsobj/<name>.tro).
std::string CourseNameOfId(const GtfsVolume& vol, uint32_t id) {
    for (const GtfsEntry& f : vol.Files()) {
        if (f.path.rfind("crsobj/", 0) != 0 || f.path.find(".tro") == std::string::npos) continue;
        const std::string name = f.path.substr(7, f.path.find('.') - 7);
        if (CourseFileId(name) == id) return name;
    }
    throw std::runtime_error("no course file with id " + std::to_string(id));
}

// Headless checks: determinism (two identical runs must give identical state bytes), an acceleration trace, and
// (with --dump) the dump's cars settling from their dumped state with no input. The races start without the
// shell's countdown (RaceOptions::countdown = false) so that the traces are those of the released cars.
int SelfTest(const RaceData& data, size_t carCount) {
    int failures = 0;
    RaceOptions options;
    options.countdown = false;
    std::vector<uint8_t> snapshots[2];
    for (int run = 0; run < 2; run++) {
        sim::RaceSim race;
        SetupRace(race, data, 1, options);
        if (run == 0) {
            const sim::CarTelemetry t = race.Telemetry(0);
            std::printf("grid: heading %d (grid list slot 0: %d), chunk %d, course distance %.1f m (grid list slot 0: %.1f m)\n", t.heading,
                        data.course.grid.heading.empty() ? 0 : data.course.grid.heading[0], race.CarAt(0).body.chunkIndex, t.courseDistance / 65536.0,
                        data.course.grid.distance.empty() ? 0.0 : data.course.grid.distance[0] / 65536.0);
            PrintTelemetry(race, 0, 0);
        }
        for (int step = 1; step <= 300; step++) {
            const sim::PadRecord pad = ScriptedPad(step - 1);
            race.Step(&pad);
            if (run == 0 && step % 30 == 0) PrintTelemetry(race, 0, step);
        }
        snapshots[run] = race.Snapshot();
        if (run == 0) {
            const sim::CarTelemetry t = race.Telemetry(0);
            if (t.forwardSpeed < 20 * 4096 / 3.6) { std::puts("FAIL: the car did not reach 20 km/h in 10 s at full throttle"); failures++; }
        }
    }
    const bool same = snapshots[0] == snapshots[1];
    std::printf("determinism: two runs of 300 steps %s (%zu state bytes)\n", same ? "identical" : "DIFFER", snapshots[0].size());
    failures += same ? 0 : 1;

    // A race start with --cars N: the player at full throttle, the AI cars driven by the ported AI (ai_driver.*) on
    // the course's lines. Every AI car must be moving after 10 s.
    if (carCount > 1) {
        sim::RaceSim race;
        SetupRace(race, data, carCount, options);
        ShellLog log;
        log.Attach(race);
        std::printf("race: %zu cars from the grid, AI cars on their lines\n", carCount);
        std::vector<sim::PadRecord> pads(carCount);
        for (size_t car = 0; car < carCount; car++) PrintTelemetry(race, car, 0);
        for (int step = 1; step <= 300; step++) {
            pads[0] = ScriptedPad(step - 1);
            race.Step(pads.data());
            if (step % 100 == 0)
                for (size_t car = 0; car < carCount; car++) PrintTelemetry(race, car, step);
        }
        for (size_t car = 1; car < carCount; car++) {
            const sim::CarTelemetry t = race.Telemetry(car);
            const sim::CarBody& body = race.CarAt(car).body;
            std::printf("  AI car %zu: line %u section %u previous type %u type %u target %.1f km/h\n", car, body.aiLine, body.aiSection, body.aiPreviousType,
                        body.aiSectionType, body.aiTargetSpeed / 4096.0 * 3.6);
            if (t.forwardSpeed < 20 * 4096 / 3.6) { std::printf("FAIL: AI car %zu did not reach 20 km/h in 10 s\n", car); failures++; }
        }
        std::printf("shell: race clock %.2f s, lap counters", race.RaceClock() / 3000.0);
        for (size_t car = 0; car < carCount; car++) std::printf(" %d", race.CarAt(car).body.lap);
        std::printf(", sectors passed");
        for (size_t car = 0; car < carCount; car++) std::printf(" %u", race.CarAt(car).body.sector);
        std::printf("\n");
        PrintStandings(race, log, data.carIds);
    }

    // The dump's six cars from their dumped state (mid-race, ~100 km/h) with zero pads: must stay finite and on the road.
    if (data.haveDump && data.dump.courseIndex == data.courseIndex && data.dump.cars.size() <= data.params.size()) {
        sim::RaceSim race;
        SetupRace(race, data, data.dump.cars.size(), options);
        race.LoadState(data.dump.cars, data.dump.contact);
        std::puts("settle: the dump's cars with no input");
        for (size_t car = 0; car < race.CarCount(); car++) PrintTelemetry(race, car, 0);
        std::vector<sim::PadRecord> pads(race.CarCount());
        for (int step = 1; step <= 300; step++) race.Step(pads.data());
        sim::NativeCourse& course = race.Course();
        for (size_t car = 0; car < race.CarCount(); car++) {
            PrintTelemetry(race, car, 300);
            const sim::CarBody& body = race.CarAt(car).body;
            int32_t chunk = body.chunkIndex;
            const int32_t ground = course.GroundHeight(body.position[0], body.position[1], chunk);
            const double clearance = ground == sim::kNoSurfaceHeight ? 1e9 : (body.position[2] - ground) / 4096.0;
            const bool ok = ground != sim::kNoSurfaceHeight && clearance > -0.5 && clearance < 3.0 && std::abs(body.position[0]) < 0x40000000 && std::abs(body.position[1]) < 0x40000000;
            std::printf("  car %zu: height above the road %.2f m %s\n", car, clearance, ok ? "ok" : "FAIL");
            failures += ok ? 0 : 1;
        }
    } else {
        std::puts("settle: skipped (needs --dump with the dump's course and enough car records)");
    }
    std::printf("selftest: %d failure(s)\n", failures);
    return failures;
}

} // namespace gt2game
