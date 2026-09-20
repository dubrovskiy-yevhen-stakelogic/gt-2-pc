#pragma once
// The race building blocks of gt2game shared by the standalone race, the headless runs, the career flow and the menus'
// race (moved out of main.cpp): the disc-derived race data, the race options, the shell's console log, the headless
// drivers and the self-test. See main.cpp for the command line.
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "game/audio/race_audio.h"
#include "game/sim/dev_dump_constants.h"
#include "game/sim/disc_data.h"
#include "game/sim/race_sim.h"
#include "gt2formats/car_json.h"
#include "gt2formats/license_data.h"
#include "gt2formats/replay.h"
#include "gt2formats/track.h"
#include "gt2formats/track_json.h"

namespace gt2 {
class DiscImage;
class GtfsVolume;
} // namespace gt2
namespace gt2::career {
struct CareerState;
}

namespace gt2game {

// ---- camera math (the view / model matrices of the race view)
struct Vec3 { float x, y, z; };
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(Vec3 a, Vec3 b);
Vec3 Cross(Vec3 a, Vec3 b);
Vec3 Normalize(Vec3 a);
void Multiply(const float* a, const float* b, float* out); // column-major out = a * b
extern const float kVerticalFovDegrees;
extern const float kProjectionDistance; // the view as a PS1 projection distance on the 240-line frame (scenery LOD rule)
void ViewProjection(Vec3 eye, Vec3 target, float aspect, float* out);
void ModelMatrix(const gt2::sim::CarPose& pose, float* m);
Vec3 Position(const gt2::sim::CarPose& pose);
Vec3 Forward(const gt2::sim::CarPose& pose);

// Everything a race needs from the data files: track, course extras, constants, race data and car records.
struct RaceData {
    gt2::Track track;
    gt2::sim::CourseExtras extras;
    gt2::sim::SimConstants constants;        // race overlay + executable tables (disc_data.h)
    gt2::sim::RaceCourseData course;         // the course's .tro race lists / start lines, dirt flag from .crsinfo
    std::vector<gt2::sim::CarParams> params; // car records built natively from the disc's parameter tables
    std::vector<std::string> carIds;         // model id per slot
    std::vector<uint32_t> paints;            // paint index per slot (the renderer's .cdp paint; 0 = first)
    std::vector<gt2::audio::CarSoundSetup> sound; // engine sound set / exhaust of each slot (game/audio/race_audio.h)
    // The body model's LOD 0 dimensions of each slot (the .cdo the record was built with): 0x80017E74 moves the car
    // back from its grid position by the model's nose (sim::CarNoseOffset). Filled with the params (AddCarBody).
    std::vector<gt2::CarBodyDimensions> bodies;
    // The grid slot of each car (race entry byte + 1, 0x801D5945 + car * 0xD0); empty = car i on slot i (the shell's
    // assignment is not known for the quick race / GT-mode events; licence tests: slot 0; the attract replay: its file).
    std::vector<uint8_t> gridSlots;
    // The race entries' kind (0x801D5946 + car * 0xD0: 3 player 1, 1 AI, ...) and transmission (+3); empty = car 0 the
    // player (RaceOptions::manual), the rest AI. Set from a replay file's car slots.
    std::vector<uint8_t> entryKinds, transmissions;
    uint16_t dirtLevel = 0; // race block + 0x58 (0x801D58B4): player 1's dirt at the start (sim::RaceSlot::dirtLevel; a replay's)
    int8_t handicap = 0;    // race block + 6 (0x801D5862): the 2 player Battle's Handicap Start (sim::HandicapGridOffset)
    std::vector<gt2::CarConfig> configs;     // each slot's configuration (a saved replay's car slots)
    std::shared_ptr<const gt2::LicenseTest> replayLicense; // a licence replay's test (RaceOptions::license points here)
    int courseIndex = -1;                    // .crsinfo entry of the track
    std::string trackName;                   // crsobj/<name>.tro
    gt2::sim::RaceShellOptions shell;        // the race shell's overlay tables and the course's point-to-point flag (race_shell.h)
    // Development cross-check (--dump): the same data as the original had it in memory.
    bool haveDump = false;
    gt2::sim::dev::DumpRace dump;
    // --mods <dir>: the player's car when <dir>/cars/<id>.json exists (docs/formats/car_json.md).
    bool modCar = false;
    gt2::ResolvedCar mod;
    // --mods <dir> --track <name> with <dir>/tracks/<name>.json: the mod course (docs/formats/track_json.md, mods.h); its
    // textures, backdrop, flags, replay cameras, course map and objects replace the disc course's in the race view.
    std::shared_ptr<const gt2::ResolvedCourse> modCourse;
    // --mods with --ai-cars: the mod car of each AI slot (index = race slot; null / missing = a disc car). Slot 0 is `mod`.
    std::vector<std::shared_ptr<const gt2::ResolvedCar>> opponentMods;
};

// The course-side inputs of the disc-derived race data: the course's .crsinfo entry (index; flags: dirt, point to point)
// and its race data (start lines, race lists). A disc course: from .crsinfo and the .tro; a mod course: from its file.
struct CourseSource {
    int courseIndex = -1;
    bool haveFlags = false;
    uint16_t flags = 0;
    gt2::TrackRaceData race;
};

// Options of the disc-derived data.
struct DataOptions {
    const gt2::LicenseTest* license = nullptr; // --license: the test's settings block and game mode 3 (license_data.h)
    const std::array<uint8_t, gt2::kLicenseSettingsSize>* eventSettings = nullptr; // a career event: the event row's settings block (+0x44)
    bool attractShell = false;  // --attract: the shell state of the attract race (mode 2, control class 0, demo flag)
    bool generatedAtan = false; // --generated-atan: trig.h's generated arc-tangent table instead of the executable's (0x800A4AC8)
    const gt2::ReplayFile* replay = nullptr; // --replay: the shell state of the replay's race block (game mode, countdown, ...)
    const gt2::sim::ShellState* shell = nullptr; // an explicit shell state (the arcade races, a capture's race block: arcade_race.h)
};

// How the race is run: the shell's options (laps, start countdown) and who drives slot 0.
struct RaceOptions {
    uint8_t laps = 2;          // --laps N
    bool countdown = true;     // --no-countdown: no start hold (the original's 0x801D5869 = 0)
    bool aiPlayer = false;     // --ai-player: slot 0 driven by the AI too
    bool manual = false;
    const gt2::LicenseTest* license = nullptr; // --license: the test (game mode 3, one car)
    double licenseDecel = 10.0;                // --license-decel: the headless test driver's assumed braking (m/s^2)
};

// The track, its extras and the disc data of `trackName` into `data` (no cars).
void LoadRaceTrack(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const std::string& trackName, const DataOptions& options, RaceData& data);
// The race data of a course given by its source (the track must be parsed into data.track / data.extras).
void LoadCourseData(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const std::string& trackName, const CourseSource& source, const DataOptions& options,
                    RaceData& data);
// The disc-derived simulation data of `trackName` (the track must be parsed into data.track / data.extras).
void LoadDiscData(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const std::string& trackName, const std::vector<uint8_t>& tro, const DataOptions& options,
                  RaceData& data);
// The car records of a quick race: slot 0 = `playerCar`, the rest the attract race's line-up (stock configurations).
// `opponents` (--ai-cars): the AI slots' car ids in order (disc cars or mod cars of `modsDir`); missing = the line-up.
void BuildCarRecords(const gt2::GtfsVolume& vol, const std::string& playerCar, size_t carCount, const std::string& modsDir, RaceData& data,
                     const std::vector<std::string>& opponents = {});
// The licence car of a test (slot 0) from the licence file's tables.
void BuildLicenseCar(const gt2::GtfsVolume& vol, const gt2::LicenseData& lic, const gt2::LicenseTest& test, RaceData& data);
// The LOD 0 dimensions of the body model the record builder uses for `config` (its racing-modify row's model).
gt2::CarBodyDimensions CarBodyOf(const gt2::GtfsVolume& vol, const gt2::CarParamTables& tables, const gt2::CarConfig& config);
// Adds one car record built from `config` (a garage / opponent configuration) with the car tables `tables`.
void AddCarRecord(const gt2::GtfsVolume& vol, const gt2::CarParamTables& tables, uint32_t carId, gt2::CarConfig config, uint32_t paint, RaceData& data);

// 0x800299D8 at the race load: with the start countdown on, the pre-race intro music raises the start hold to the
// intro's length (data.shell.introFields; the race track pick does not change it).
void ApplyIntroHold(const gt2::DiscImage& disc, RaceData& data, const RaceOptions& options);

int ScanCourses(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol);
int CrossCheckDump(const gt2::DiscImage& disc, const std::string& trackName, RaceData& data);

std::vector<gt2::sim::RaceSlot> GridSlots(const RaceData& data, size_t carCount, const RaceOptions& options);
void SetupRace(gt2::sim::RaceSim& race, const RaceData& data, size_t carCount, const RaceOptions& options);

std::string FormatMs(int32_t ms);
// The outcome of a licence test as the shell records it (0x800156EC) and the results screen's prize (0x8002B170).
std::string LicenseVerdict(const gt2::LicenseTest& test, int32_t result, uint32_t time);

// Console log of the shell's events (race_shell.h ShellHooks) and the lap times it reports, per car; also keeps every
// car's race position at the moment the player (slot 0) finishes (the positions the championship points are taken
// from, 0x80013824).
struct ShellLog {
    struct CarLaps { std::vector<int32_t> lapTimes; int32_t finishTime = -1; int position = 0; };
    std::vector<CarLaps> cars;
    const gt2::LicenseTest* license = nullptr; // the licence test of a mode 3 race (its medal times)
    bool quiet = false;
    gt2::audio::RaceAudio* audio = nullptr; // the start-signal beeps (0x800189C4) go to the sound when it is on
    const gt2::sim::RaceSim* race = nullptr;
    std::vector<int32_t> positionsAtPlayerFinish;
    static std::string Time(int32_t ms) { return FormatMs(ms); }
    void Attach(gt2::sim::RaceSim& raceSim);
    int32_t BestLap(size_t car) const;
};

int32_t PlayerLastLap(const gt2::sim::RaceSim& race);
void PrintPlayerResults(const gt2::sim::RaceSim& race);
void PrintStandings(const gt2::sim::RaceSim& race, const ShellLog& log, const std::vector<std::string>& carIds);
void PrintTelemetry(const gt2::sim::RaceSim& race, size_t car, int step);

// Scripted pad of the self-test: full throttle from the start, a short left steer input after 8 s.
gt2::sim::PadRecord ScriptedPad(int step);
// Headless test driver of a licence test (a stand-in for the player's pad, not the original's).
gt2::sim::PadRecord LicenseDriverPad(const gt2::sim::RaceSim& race, const gt2::LicenseTest& test, double decel);

// --headless <seconds>: the race without a window.
int Headless(const RaceData& data, size_t carCount, const RaceOptions& options, double seconds);
// --selftest.
int SelfTest(const RaceData& data, size_t carCount);

// ---- replays (gt2formats/replay.h): the pad source of player 1 (pad slot 2) in RaceSim - the original's 0x80013EF0 /
// 0x80013C90 inside the physics core: record the logical pad into the stream (and drive the car from the recorded
// frame), or play the stream back (the attract race, a replay).
struct ReplayDriver {
    enum class Mode { kOff, kRecord, kPlay };
    Mode mode = Mode::kOff;
    gt2::ReplayStream stream;
    std::array<uint16_t, 16> pedalTable{}; // race overlay 0x8002F4D4
    gt2::LogicalPad pad;                   // kRecord: the logical pad of the coming step
    uint8_t gameMode = 2;                  // 0x801D5866: the recorder stops 60 (mode 3) / 300 fields after the finish
    bool ended = false;                    // kPlay: the stream ran out (0x800A8D68 = 1: the replay ends)
    gt2::ReplayFrame lastFrame;            // the frame of the last step
    void StartRecording(uint8_t mode);
    void StartPlayback(std::span<const uint8_t> streamBytes, uint8_t mode);
    void Attach(gt2::sim::RaceSim& race);  // RaceSim::SetPadSource
    static void Source(void* user, gt2::sim::RaceSim& race, gt2::sim::PadRecord* pads);
};
// The race overlay's analogue pedal table (0x8002F4D4, u16[16]).
std::array<uint16_t, 16> PedalTable(const gt2::DiscImage& disc);
// The race of a replay file: course (race block + 0x3C file id), cars (the slots' configurations with the gtmode tables,
// grid slots, entry kinds, transmissions), laps / countdown of the race block; the shell state comes from
// DataOptions::replay. Fills `options.laps` / `countdown`.
void LoadReplayRace(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const gt2::ReplayFile& replay, RaceData& data, RaceOptions& options);
// --frames-compare <capture>: the race of `data` / `options` natively against the original's frames (gt2formats/race_capture.h,
// gt2verify --race-capture): with `replay` its player stream is played back, otherwise the captured logical pad is recorded
// (and drives the car) like 0x80013C90; every frame's car bodies are compared byte for byte, and at the end the recorded stream
// against the original's. Returns the number of frames that differ.
// A replay file of a race of `data` (gt2formats/replay.h layout): the race block (mode flags, game mode, licence test,
// countdown, laps, course name / file id, car count), the car slots (car id, configuration, grid slot / kind /
// transmission) and player 1's stream.
gt2::ReplayFile BuildReplayFile(const gt2::GtfsVolume& vol, const RaceData& data, size_t carCount, const RaceOptions& options, const gt2::ReplayStream& stream);
// --replay-check <seconds>: determinism of the replay path. A race of `seconds` with a scripted logical pad, recorded;
// then the race again from the in-memory stream and once more from the saved .gmr bytes (LoadReplayRace): the final car
// states (the records' simulated part), the contact tables and the stream must be byte-identical. `outPath` (optional)
// receives the .gmr. Returns the number of failures.
// `analogPad` (--replay-check-analog): the recorded pad comes from a scripted analog controller through the ported pad chain
// (platform/input/ps1_pad.h: reader, handlers, 0x80014BB4) - analogue steering / pedal frames in the stream.
int ReplayCheck(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const RaceData& data, size_t carCount, const RaceOptions& options, double seconds,
                const std::string& outPath, bool analogPad = false);
int FramesCompare(const RaceData& data, size_t carCount, const RaceOptions& options, const std::string& capturePath, const gt2::ReplayFile* replay,
                  const std::array<uint16_t, 16>& pedalTable, int maxFrames);

std::string DescribeCareer(const gt2::career::CareerState& s);
// The course file name of a course file id (0x80060FB0).
std::string CourseNameOfId(const gt2::GtfsVolume& vol, uint32_t id);

} // namespace gt2game
