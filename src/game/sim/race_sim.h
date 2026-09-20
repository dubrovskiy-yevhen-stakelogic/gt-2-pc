#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "game/sim/ai_driver.h"
#include "game/sim/car_body.h"
#include "game/sim/car_contact.h"
#include "game/sim/car_setup.h"
#include "game/sim/drivetrain.h"
#include "game/sim/ground.h"
#include "game/sim/physics_core.h"
#include "game/sim/race_shell.h"
#include "gt2formats/replay.h"
#include "gt2formats/track.h"

// The assembled race simulation: the verified ports of the original's setup path (0x80033384 per car) and of its
// per-frame physics tick (0x8003EBF0) on the native course data, owned by one object. The game drives it at 30 Hz
// and reads the render transforms from it. Every routine below that carries an address is the port of that
// routine of the US Simulation v1.2 executable (SHA-1 3030aa27...); the sub-passes are the verified ports in
// the other headers of this directory.
//
// Per frame (0x80015B64 -> 0x8003EBF0 -> 0x8002E550):
//   shell BeginFrame (hold counter) -> PhysicsCore 0x8003E0C4 -> [hold == 0] SimulateCars 0x80034480 ->
//   PrepareContactQueries 0x80043388 -> QueryContacts 0x800434DC -> ApplyContacts 0x80043578 -> GroundPass
//   0x8003E8E4 -> shell ProgressCars 0x8003CF94 (course distance, lap / sector lines, section state machine of
//   every car in race order) -> shell AdvanceClock 0x8003D168 -> [mode 3] shell LicenseChecks 0x8003D5F8 ->
//   race order 0x80042568 -> shell EndFrame
//   0x8002E550 (HUD timers, start / end sequences). The render transforms (0x800133F0 / 0x8001336C) are pure
//   functions of the body and are computed on demand by Pose(). The shell is race_shell.h (RaceShell).
// The AI drivers (0x80037834 / 0x800377E8 / 0x800372EC, ai_driver.h) run through PhysicsContext::aiInput on the
// course's line lists (RaceCourseData::sections) - the same data the setup's race-progress initialisation uses.
// What the frame does NOT do natively yet, and why:
//   - the sound / vibration hooks (0x800156B8, the block at the end of 0x800133F0);
//   - the smoke / dust particle spawns of 0x800133F0 (renderer-side effect).
namespace gt2::sim {

// ---------------------------------------------------------------- constants of the executable / overlay

// The AI catch-up ("rubber band") tuning 0x80046F6C..0x80046F84 that 0x80041E4C derives from the race settings block
// (0x801C98A0) at the race load (0x8003C12C: enabled in game modes 2 / 4 / 0xC; mode 0 passes 0x8003B73C(0x801D5863),
// not ported; other modes 0). Disabled = zeros and scale 0x1000 (the AI time scale stays 0x1000).
struct CatchUpTuning {
    int32_t slowGain = 0;      // 0x80046F6C = settings + 0x17 * 4096 / 100: slow-down of a car ahead of its reference
    int32_t slowFrom = 0;      // 0x80046F70 = settings + 0x18 << 16 (course distance, 16.16 m)
    int32_t slowTo = 0;        // 0x80046F74 = settings + 0x19 * 0xA0000 (10 m units)
    int32_t fastGain = 0;      // 0x80046F78 = settings + 0x1A * 4096 / 100: speed-up of a car behind its reference
    int32_t fastFrom = 0;      // 0x80046F7C = settings + 0x1B << 16
    int32_t fastTo = 0;        // 0x80046F80 = settings + 0x1C * 0xA0000
    int32_t scale = 0x1000;    // 0x80046F84 = (settings + 0x14, 0 -> 100) * 4096 / 100: every AI car's overall time scale
    uint8_t chained = 0;       // 0x801C98B6 (settings + 0x16): 0 = every AI car against car 0, else each car against its neighbour in the race order
};
// 0x80041E4C(enabled) on the settings block's bytes (`settings` = 0x801C98A0.., at least 0x1D bytes).
CatchUpTuning CatchUpFromSettings(std::span<const uint8_t> settings, bool enabled);

// Every constant the setup and the tick read from outside the car array. These are GAME DATA: they are built at
// run time from the disc's tables (disc_data.h; dev_dump_constants.* reads the same from a RAM dump only as a
// cross-check) and are never part of the repository. Members carry the address they come from; curves carry
// their table object's address.
struct SimConstants {
    StepGlobals step;                                  // 0x801C856C frameTime, 0x801C8570 rate, 0x80046EF4 draftDragFloor
    TyreWearConstants wear;                            // 0x80046F48 .. 0x80046F60
    std::vector<int32_t> rollingXs, rollingYs;         // 0x801C8730 { count; xs 0x80046EF8; ys 0x80046F18 }
    std::array<int32_t, 8> surfaceRolling{};           // 0x80046E00 s32[8] (entry 0 is also the setup's rollingResistanceGain)
    std::array<DriveClassTuning, 4> classTuning{};     // 0x801C8690 + i * 0x28 (the AI aid fields at +0x18.. live inside)
    uint8_t slideSensitivity = 100;                    // 0x80046EE8
    uint32_t word80046F64 = 49400;                     // 0x80046F64: the shell's race clock as dumped (1/3000 s; RaceSim keeps its own, see RaceClock)
    // Input mapping (drivetrain.h InputTuning).
    int32_t steerSpringGain = 0, steerDamping = 0, steerCentring = 0; // 0x80046F3C, 0x80046F40, 0x80046F44
    std::vector<int16_t> steerCurveXs, steerCurveYs;   // 0x80046DA4 { count; xs 0x80046D7C; ys 0x80046D90 }
    std::array<uint16_t, 16> pedalRates{};             // 0x80046DB0 .. 0x80046DCF: u16 tables indexed by car (see InputTuningFor)
    GroundConstants ground;                            // 0x80046F88.., 0x80046C94.., 0x80046EAC.. (ground.h)
    // Setup (car_setup.h SetupConstants).
    int32_t dragConstant = 0;                          // 0x80046EF0
    std::array<uint8_t, 2> springRateRange{};          // 0x80046DC8, 0x80046DC9
    std::array<int8_t, 8> diffTypeCodes{};             // 0x80046DCC
    std::array<uint8_t, 48> gearAutoTable{};           // 0x800923E2: 8 rows of 6 (rows 5..7 used)
    std::array<uint8_t, 8> aiGripPercent{};            // 0x801C98A4
    std::array<int8_t, 35> raceStateTable{};           // 0x80046DD4: 7 rows of 5 candidate race states
    // Game-shell globals the tick reads.
    uint8_t gameMode = 2;                              // 0x801D5866 (2 = the attract / arcade race of the dump)
    int32_t shellControlClass = 0;                     // result of 0x800418E8 for the shell's globals (0 in the dump)
    uint8_t viewMode = 0;                              // 0x801C9990
    uint8_t flag800A951C = 0, flag801C9995 = 0, flag800AF232 = 0;
    uint32_t word801C98A0 = 0;                         // StartCar: byte 2 == 1 forces control class 1 for car 0
    uint8_t byte801D5869 = 0;
    CatchUpTuning catchUp;                             // 0x80046F6C..0x80046F84 / 0x801C98B6 (the AI catch-up of the arcade modes)
    // Build differences (gt2formats/exe_profile.h): the Simulation build's tail of UpdateWheelEffects (ground.h).
    bool wheelEffectsTail = true;
};

// Race data the game shell derives for the course (AI section lists, start lines, the grid list). Course-specific
// game data, built from the course's .tro on the disc (disc_data.h).
struct RaceCourseData {
    bool dirtCourse = false;                           // course table entry (0x80060E94(course)) + 8 & 4
    std::vector<int32_t> startLineDistances;           // 0x800B4A58 { s32 count; s32[count] } (16.16 m)
    std::array<std::vector<RaceSection>, 7> sections;  // 0x801C8568 -> +8 + i * 4 -> { s32 count; RaceSection[count] }
    RaceGridInfo grid;                                 // 0x801C8568 -> +0x18 (ground.h)
};

// ---------------------------------------------------------------- native course data

// What the setup and the tick need from the course beyond the chunk geometry: the road-surface lookup grids
// (chunk + 0x9C, ground.h SurfaceGrid) and the course-distance fields of the chunk header (+0x10 s32 distance
// along the course in 16.16 m, +0x14 / +0x16 u16 interpolation weights, read by 0x80028588). gt2::ParseTrack
// parses them (TrackChunk::surfaceGrid / distance / weightNext / weightThis); BuildCourseExtras copies them into
// the simulation's own structures.
struct ChunkExtra {
    int32_t distance = 0;      // chunk + 0x10  16.16 m
    uint16_t weightNext = 0;   // chunk + 0x14  weight of the next chunk's plane in the distance interpolation
    uint16_t weightThis = 0;   // chunk + 0x16  weight of this chunk's plane
};

struct CourseExtras {
    int32_t courseLength = 0;          // chunk table + 0 (16.16 m)
    std::vector<SurfaceGrid> grids;    // one per chunk
    std::vector<ChunkExtra> chunks;    // one per chunk
};

// The extras of the parsed track. `tro` (the file the track was parsed from) may be empty; when given, only its
// magic is checked. Throws std::runtime_error on inconsistent data.
CourseExtras BuildCourseExtras(const Track& track, std::span<const uint8_t> tro);

// Course queries of the original that the setup and the tick use, on the native data.
class NativeCourse : public CoursePlacementQueries {
public:
    NativeCourse() = default;
    NativeCourse(const Track& track, const CourseExtras& extras);
    const CourseSurface& Surface() const { return surface_; }
    const CourseExtras& Extras() const { return *extras_; }
    // 0x80028900: ground height (1/4096 m) under the simulation-plane point (x, y), chunk in / out.
    int32_t GroundHeight(int32_t x, int32_t y, int32_t& chunk) override;
    // 0x80028C6C -> 0x80028588: distance along the course (16.16 m) of the simulation point (x, height, y in 16.16).
    int32_t CourseDistance(int32_t chunk, int32_t x16, int32_t height16, int32_t y16) override;
    // 0x80028830: the road under one wheel.
    void Contact(ContactQuery& query) override;
    // 0x80028588 on a course point (world X, height, Z in 16.16 m).
    int32_t CourseDistanceOfWorldPoint(int32_t chunkHint, const int32_t point[3]) const;
    // 0x80028288: the chunk whose centre is nearest to the world point (16.16 m), refined along the course.
    int32_t NearestChunk(const int32_t point[3]) const;

private:
    CourseSurface surface_;
    const CourseExtras* extras_ = nullptr;
};

// ---------------------------------------------------------------- the race

// The shell's entry kind of a race car: byte + 2 of its 0xD0-byte entry (0x801D5944 + car * 0xD0), read by the per-car
// race setup 0x80012CD4. Kind 3 = player 1, 4 = player 2, 2 = the mode 6 ghost car, anything else an AI car (1 in the
// dumps: the attract race has kinds {3, 1, 1, 1, 1, 1}, licence test B-1 {3}).
constexpr uint8_t kEntryAi = 1, kEntryGhost = 2, kEntryPlayer1 = 3, kEntryPlayer2 = 4;
// 0x80012CD4: the car record's pad slot (car + 0x18) of an entry kind: 2 player 1, 3 player 2, 1 ghost, 0 AI. The race
// shell keeps the results of slots 2 / 3 (0x801D5E88 / 0x801DA3A0, RecordLap 0x8005E3C4).
int16_t EntryPadSlot(uint8_t entryKind);
// 0x80012CD4: the control class it passes to 0x80033384 for an entry kind: 0 (pad / replay input) for kinds 2..4, 2 (AI)
// otherwise.
uint8_t EntryControlClass(uint8_t entryKind);

// One grid slot of a race: what the shell passes to 0x80033384 for the car in it.
struct RaceSlot {
    // The shell's entry kind (kEntry*): sets the car record's pad slot (EntryPadSlot) like 0x80012CD4. The control
    // class below is normally EntryControlClass(entryKind); gt2game's --ai-player keeps the player-1 entry (the results
    // are the player's) but lets the AI drive it (control class 2), which the original has no entry kind for.
    uint8_t entryKind = kEntryAi;
    uint16_t dirtLevel = 0;        // 0x801D58B4 (player 1 only): body + 0x658 = dirtLevel * 600000 >> 12 after the setup (0x80012CD4)
    int32_t gridOffset = 0;        // the 13th argument of 0x80033384 (car_setup.h RaceStartInputs; HandicapGridOffset)
    uint8_t controlClass = 0;      // 0 player (pad input), 2 AI (ai_driver.h through PhysicsContext::aiInput)
    uint8_t transmission = 0;      // player cars: 0 automatic, 1 manual
    uint8_t contactType = 0;       // body + 0x45E: non-zero excludes the car from car-to-car contact
    int32_t x = 0, y = 0;          // simulation plane, 1/4096 m
    int32_t chunkHint = 0;
    int32_t headingSin = 0, headingCos = 0; // the forward direction in the simulation plane (fx, fy) * 4096: the shell
                                            // passes it so that Atan2(-headingSin, headingCos) is the body heading
};

// 0x80012CD4's Handicap Start (race block + 6, the career's option + 7): player 1 (entry kind 3) -10 * h metres for h > 0 and -1
// for h < 0, player 2 (kind 4) 10 * h for h < 0 and -1 for h > 0 (the handicapped player further back on the main line); 0 for
// h == 0, other kinds and every game mode but 0.
int32_t HandicapGridOffset(uint8_t entryKind, int8_t handicap, uint8_t gameMode);

// The grid part of the per-car race setup 0x80012CD4 (what it computes before calling 0x80033384):
//   chunk hint  = 0x80028288(chunks, pole position .tro + 0x58)           - always grid slot 0's position
//   car + 0x81C = identity (0x8007AF60), moved to the slot's position .tro + 0x58 + slot * 12 (0x8007B050),
//                 turned by Ry(start angle .tro + 0x54) (0x8007B14C),
//                 moved along its own z by the body model's nose (0x80017E74: LOD 0 bbox min z << (scale - 16) << 4;
//                 the grid positions are where the car's front is, the car's origin sits behind them)
//   chunk       = 0x80028394(chunks, hint, car + 0x830)
//   x, y        = car + 0x830 >> 4, -(car + 0x838) >> 4 (simulation plane)
//   sin, cos    = the sine table 0x80093150 at (angle & 0xFFF) and (angle & 0xFFF) + 0x400.
// `noseZ` = CarNoseOffset of the car's LOD 0 block (0 = the car object has no model: no move).
struct GridPlacement {
    std::array<int32_t, 3> pole{};      // .tro + 0x58 (16.16 m world x, y up, z)
    std::array<int32_t, 3> position{};  // .tro + 0x58 + slot * 12
    int32_t startAngle = 0;             // .tro + 0x54 (4096 per turn)
    int32_t noseZ = 0;
};
// 0x80017E74's move: scaled(bbox min z) << 4 with scaled(v) = v << (scale - 16) (>> for a negative shift).
int32_t CarNoseOffset(int16_t lod0BboxMinZ, int16_t lod0Scale);
// 0x80012CD4: the grid slot of a race entry - the entry's byte + 1 (0x801D5945 + car * 0xD0), except in game mode 6 on
// a course whose .crsinfo flags lack bit 5 (0x20: not point-to-point), where every car takes slot 2.
uint8_t GridSlotOfEntry(uint8_t entryGridSlot, uint8_t gameMode, uint16_t courseFlags);
// The placement arguments of 0x80033384 (x, y, chunk hint, heading) for `grid` - see GridPlacement.
RaceSlot PlaceOnGrid(const Track& track, const NativeCourse& course, const GridPlacement& grid);
// PlaceOnGrid on the parsed track's grid slot `slot` (Track::startGrid / startAngle).
RaceSlot SlotFromTrackGrid(const Track& track, const NativeCourse& course, size_t slot, int32_t noseZ);

// Render-side view of one car (0x800133F0 / 0x8001336C): the world position in 16.16 m (with the reference-point
// offset applied along the car), the s16 rotation matrix (4096 = 1.0; column 0 = right, 1 = up, 2 = -forward,
// i.e. the model's +X, +Y, -Z axes), and the wheel visuals.
struct CarPose {
    std::array<int32_t, 3> worldPosition{};        // car + 0x830
    std::array<std::array<int16_t, 3>, 3> rotation{}; // car + 0x81C, row-major like the PSX MATRIX
};

// 0x8001336C on the physics position / rows (body + 0x65C / + 0x668) followed by the reference offset of
// 0x800133F0 (car + 0x81C / + 0x830).
CarPose PhysicsPoseOf(const CarBody& body);
// 0x8001336C on the ground-following pose (body + 0x680 / + 0x68C) with the same offset vector added to the
// position, as 0x800133F0 does for car + 0x83C / + 0x850.
CarPose VisualPoseOf(const CarBody& body);

// 0x80042568 with 0x80042490 / 0x8004239C: sorts the race order table (car indices, 0x801C8578) by laps and course
// distance and writes the positions (body + 0x750); a finish flag 1 (body + 0x6FC) becomes 2.
void UpdateRaceOrder(Car* cars, int count, int8_t* order);

// The frame driver's AI catch-up (0x8003EBF0 in game modes 2 / 4 / 0xC, after the race order): 0x80042230(reference,
// car) for the AI cars - against car 0, or (settings + 0x16 != 0) along the race order. The car's time scale body +
// 0x766 (its physics steps run slower / faster) follows the lap / course-distance gap to the reference (0x800423BC):
// ahead of it by more than slowFrom -> 0x1000 - slowGain * ramp (0x80042174), behind by more than fastFrom -> 0x1000 +
// fastGain * ramp (0x800420AC; not while off the road, body + 0x78D bit 4), then * scale >> 12. Finished cars keep theirs.
void ApplyCatchUp(Car* cars, int count, const int8_t* order, const CatchUpTuning& tuning, int32_t courseLength);
// 0x80042230(reference body, body).
void CatchUpCar(const CarBody& reference, CarBody& body, const CatchUpTuning& tuning, int32_t courseLength);
// Game mode 0 (the Arcade disc's 2 player Battle): the frame driver 0x8003EBF0 runs 0x80042038(car 0, car 1) after the race order
// instead - the "Slow Car Boost" of the two players: when the tuning's fastGain is not 0, the gap 0x800423BC of car 0 to car 1
// (laps, course distance, normalised as in CatchUpCar) and 0x80041F68 for car 1 with it and for car 0 with its negation. 0x80041F68
// (body, laps, distance): body + 0x766 = 0x1000 while off the road (+ 0x78D bit 4) or ahead by laps (laps < 0); a lap behind (laps > 0)
// or at least fastTo behind -> 0x1000 + fastGain; between fastFrom and fastTo the ramp (Div64((d - fastFrom) << 12, fastTo -
// fastFrom) * fastGain >> 12) + 0x1000; else 0x1000. No finished test and no overall scale (unlike 0x80042230).
void CatchUpBattle(CarBody& car0, CarBody& car1, const CatchUpTuning& tuning, int32_t courseLength);

// The race load's settings of the game mode (0x8003C12C before 0x80041E4C, on the settings block 0x801C98A0 = `settings`, 0x40
// bytes): mode 0 -> 0x8003B73C(race block + 7 = Slow Car Boost, career + 8) and 0x8003B69C(race block + 3 = Tire Damage, career + 4);
// modes 1 / 2 / 4 / 0xC leave the block; every other mode 0x8003B69C(0). Returns 0x80041E4C's argument (the catch-up enable): modes
// 2 / 4 / 0xC 1, mode 0 0x8003B73C's result, else 0.
//   0x8003B73C(boost): 1 -> + 0x1A / 0x1B / 0x1C = 15, 20, 100 (fastGain 15 %, from 20 m, full at 1000 m), returns 1; 2 -> 20, 10, 20,
//     returns 1; else 0, 0, 0, returns 0.
//   0x8003B69C(wear): the tyre wear bytes + 0x1D .. + 0x23 (disc_data.h RaceSettings wearLimit .. kneeGripLossPercent): 1 -> 20, 25,
//     13, 2, 10, 16, 15; 2 -> 10, 25, 13, 1, 10, 8, 15; else 0, 0, 100, 0, 0, 0, 0 (wear off).
bool RaceLoadSettings(uint8_t gameMode, uint8_t tyreWearOption, uint8_t slowCarBoost, std::span<uint8_t> settings);
bool SlowCarBoostSettings(uint8_t option, std::span<uint8_t> settings); // 0x8003B73C
void TyreWearSettings(uint8_t option, std::span<uint8_t> settings);     // 0x8003B69C
// 0x8003FE8C: the contact tables at the race load (corner flags 0xF, positions 0, fractions 0x1000, corners / sides 0xFF,
// buffer 1).
void InitContactState(CarContactState& state);

struct WheelVisual {
    int16_t steerAngle = 0;        // wheel + 0x0C, angle units
    uint16_t rotation = 0;         // wheel + 0x20, angle units
    int16_t verticalOffset = 0;    // car + 0x7C6 + wheel * 0x10: suspension reference minus travel (1/4096 m), model-space Y
};

struct CarTelemetry {
    int32_t forwardSpeed = 0;      // body + 0x6A4, 1/4096 m/s
    int16_t rpm = 0;               // body + 0x6AC
    uint8_t gear = 0;              // body + 0x618 (0 = reverse)
    int16_t throttle = 0, brake = 0, handbrake = 0; // body + 0x610 / 0x612 / 0x614, 0x1000 = full
    int16_t steerAngle = 0;        // body + 0x60C
    int16_t heading = 0;           // body + 0x648
    int32_t courseDistance = 0;    // body + 0x604, 16.16 m
    uint8_t racePosition = 0;      // body + 0x750
    uint8_t wallHitMask = 0;       // body + 0x785
};

class RaceSim {
public:
    // Sets the race up: cars in the given slots with the given parameter records (mutated by the setup, like the
    // original's), then the race shell (RaceShellOptions: laps, start countdown). `extras` must outlive the
    // object. Throws on inconsistent inputs.
    void Setup(const Track& track, const CourseExtras& extras, const SimConstants& constants, const RaceCourseData& courseData,
               std::vector<CarParams> params, const std::vector<RaceSlot>& slots, const RaceShellOptions& shellOptions = {});

    // Replaces the dynamic state with a snapshot of the original's car objects and contact tables (the whole
    // 0xB40-byte car records, as they sit in a RAM dump) for a settle / compare run. Setup must have run with the
    // same car count and records.
    void LoadState(std::span<const Car> cars, const CarContactState& contact);

    // One 30 Hz frame with one pad record per car (AI cars ignore theirs). `buttons` = the two pads' raw button
    // words for the results wait of the shell (0xA00 = X / Start; null = none).
    void Step(const PadRecord* pads, const uint32_t* buttons = nullptr);

    // The pad source of the pad-driven cars: the original reads each car's pad record inside the physics core
    // (0x8003C250 -> 0x80013EF0 for pad slot 2: record the logical pad into the replay stream / play the stream back,
    // 0x80013C90), i.e. after the shell's BeginFrame has advanced its counters (the recorder stops 300 / 60 fields
    // after the finish, 0x800A9522). When set, Step calls it there with a copy of the pads passed in, to fill them.
    using PadSource = void (*)(void* user, RaceSim& race, PadRecord* pads);
    void SetPadSource(PadSource source, void* user) { padSource_ = source; padSourceUser_ = user; }
    // Game mode 6: the pad source tells the frame it made player 1's pad record from (0x80013C90's frame of the logical pad);
    // the race records it into the ghost's lap stream (without a pad source: the idle pad's frame).
    void NotePlayerFrame(const gt2::ReplayFrame& frame);

    // ---- game mode 6 (Time Trial / Rally, race_shell.h GhostSession)
    GhostSession& Ghost() { return *ghost_; }
    const GhostSession& Ghost() const { return *ghost_; }
    bool HasGhost() const { return constants_.gameMode == 6 && ghost_ != nullptr; }
    // The race end (0x800153B8): the ghost's ring and the lap it hands to the next race (Try Again).
    void EndGhostRace();
    // 0x80015B64's 0x8002F4B8 part: a mode 6 replay's lap start (0x8003F990 on car 0, race_shell.h GhostReplayLapStart) when one
    // is pending. Step runs it after the shell's BeginFrame; public for the frame comparison (the captures record the state at
    // the entry of 0x8003EBF0, i.e. after it).
    void ReplayLapStartIfPending();
    // The course record (*(0x800A9524)) after the race: the shell replaced it when a lap beat it (RaceShellState::newRecord).
    const LapEntry& CourseRecord() const { return shell_.State().courseRecord; }

    size_t CarCount() const { return cars_.size(); }
    const Car& CarAt(size_t i) const { return cars_[i]; }
    Car& CarAt(size_t i) { return cars_[i]; }
    const CarContactState& Contact() const { return contact_; }
    uint16_t HoldFrames() const { return shell_.State().hold; }
    // The race clock (0x80046F64, 1/3000 s): 0 at Setup, the dump's value after LoadState, +100 per frame while the
    // cars are not held (0x8003D168). The AI reads it (no steering on the start straight in the first 3 s).
    uint32_t RaceClock() const { return shell_.State().raceClock; }
    // The race shell (laps, lap times, sector board, results, hooks) - race_shell.h.
    RaceShell& Shell() { return shell_; }
    const RaceShell& Shell() const { return shell_; }
    // 0x8002A700 returned 0: the results wait after the finish is over (the original ends the race task).
    bool RaceTaskOver() const { return raceTaskOver_; }
    const int8_t* RaceOrder() const { return raceOrder_.data(); } // 0x801C8578: car indices in race order
    const NativeCourse& Course() const { return course_; }
    NativeCourse& Course() { return course_; }

    CarPose Pose(size_t car) const;         // from body + 0x65C / rows + 0x668 (the physics pose, car + 0x81C)
    CarPose VisualPose(size_t car) const;   // from body + 0x680 / rows + 0x68C (the ground-following pose, car + 0x83C)
    std::array<WheelVisual, 4> Wheels(size_t car) const;
    CarTelemetry Telemetry(size_t car) const;

    // Bytes of the whole simulation state (cars + contact tables + shell state): equal bytes = equal state.
    std::vector<uint8_t> Snapshot() const;

private:
    void BindContext();
    void SetupShell(const RaceShellOptions& options);
    // PhysicsContext::aiInput: runs the AI driver (ai_driver.h RunAiDriver) on the car's body and gear request.
    static void AiInputHook(void* user, CarBody& body, int car, const AiDispatch& dispatch);

    const Track* track_ = nullptr;
    AiContext aiContext_;          // the AI's view of courseData_ / constants_ (pointers into them; rebuilt by BindContext)
    NativeCourse course_;
    SimConstants constants_;
    RaceCourseData courseData_;
    std::vector<CarParams> params_;
    std::vector<Car> cars_;
    std::vector<CarCurves> curves_;
    std::vector<InputTuning> input_;
    CarContactState contact_{};
    DriveStepWork work_{};
    std::array<GearRequest, kMaxCars> requests_{};
    int32_t deltas_[kMaxCars][4] = {};
    std::vector<uint8_t> scratch_ = std::vector<uint8_t>(0x400); // the 1 KB scratchpad of the ground passes
    PhysicsContext physics_;
    GroundGlobals groundGlobals_;
    RaceShell shell_;
    RaceShellOptions shellOptions_;
    bool raceTaskOver_ = false;
    std::array<int8_t, kMaxCars> raceOrder_{}; // 0x801C8578
    PadSource padSource_ = nullptr;
    void* padSourceUser_ = nullptr;
    // Game mode 6.
    std::shared_ptr<GhostSession> ownGhost_;   // when the options bring no session
    GhostSession* ghost_ = nullptr;
    std::array<uint8_t, 5> playerFrame_{};     // the noted frame (flags, buttons, steer, throttle, brake)
    bool playerFrameNoted_ = false;
    std::array<uint16_t, 16> pedalTable_{};
    Car ghostDisplay_{};                       // car 1 as 0x800133F0 draws it (the blended pose of 0x8003F2F0)
    bool ghostDisplayValid_ = false;
};

} // namespace gt2::sim
