#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "game/sim/ai_driver.h"
#include "game/sim/car_body.h"
#include "game/sim/car_contact.h"
#include "game/sim/ground.h"

namespace gt2 {
struct ReplayFrame; // gt2formats/replay.h
}

// The race shell around the physics tick: lap counting, sector splits, the start hold, the race clock, the
// section / pit state machine, the finish and the results bookkeeping. Ports of the routines of the US
// Simulation v1.2 executable (SHA-1 3030aa27...) named below, verified bit for bit by tools/gt2verify
// (verify_shell.cpp) on the attract-race RAM dump. Rendering, sound and the pad are outside: the HUD "display
// requests" the original writes into the car records (car + 0xA8C..0xAA4, timers counted down by
// EndFrame) are kept as data the game reads; the two sound triggers are hooks.
//
// Per frame (0x80015B64 -> 0x8003EBF0 -> ... -> 0x8002E550):
//   BeginFrame   0x80015B64  hold counter 0x800A9520 -= frame step (fields), 0x800A9522 += step while non-zero
//   [physics]    0x8003EBF0  PhysicsCore .. GroundPass (race_sim.h)
//   ProgressCars 0x8003CF94  for every car in race order: course distance, then while not held: lap / sector
//                            lines (0x8003C70C), wrong-way / reset requests (0x80030308), the section state
//                            machine (0x80036CA4); then the sector-gap displays of the leaderboard
//   AdvanceClock 0x8003D168  race clock 0x80046F64 += 100 (30 Hz) / 50 (60 Hz) while not held
//   [race order] 0x80042568  (race_sim.h UpdateRaceOrder)
//   EndFrame     0x8002E550  HUD display timers, the start-signal timer 0x800AF224, the race-end timer
//                            0x800AF226 (0x8002A700: returns false when the race task ends)
//
// The start: 0x8001584C sets the hold 0x800A951E = 180 fields (120 in mode 3, 0 when 0x801D5869 == 0), copied
// to 0x800A9520; 0x800299D8 raises it to the intro camera's length (0x8008103C seconds * 60: 540 in the dump)
// and 0x8002E390 arms the start-signal timer 0x800AF224 = hold + 240 (0x8002A0C8). 0x8002A0D4 counts it down
// with the frame step and fires the three lights (0x800189C4(0)) when it drops below 420 / 360 / 300 and the
// green light + race music (0x800189C4(1), 0x80029C60) below 240, i.e. exactly when the hold reaches 0.
//
// Lap counting (0x8003C70C, per car): the course distance (body + 0x604) wrapping forward across the start line
// by more than half the course counts a lap (body + 0x608); wrapping backwards sets body + 0x78D bit 1 (the
// race order then counts lap - 1 until the car crosses forward again). Lap 0 -> 1 is the start-line crossing
// right after the start (no time). Later crossings interpolate the clock to the line, record the lap
// (sector board 0x801C8580, HUD 0x8001555C -> 0x80013824: results record, finish test, jingle), and reset the
// lap max-speed readout (0x6F8 = 0x6AE). Sector lines (0x800B4A5C[body + 0x6B0]) record splits the same way
// (0x80015510). Times are 1/1000 s: the clock (1/3000 s) plus the frame fraction, divided by 3.
// The finish (0x80013824): lap == the race's lap count 0x801D586B (100 when 0), or the 2-hour limit of the
// "99 laps" arcade races (0x8003D138) -> 0x80036ACC: body + 0x6FC = 1 (2 once the race order saw it), race
// state (body + 0x786) 1 (cool-down lap) or 5 (stop the car: modes 3 / 7 / 8 / 11 and point-to-point courses),
// results position / total time, race-end timer 0x800AF226 = 0, points (modes 2 / 11), the finish jingle
// (0x80029C84, player cars). The AI drives state 5 to a stop and sets state 7 (ai_driver.h).
//
// Licence tests (game mode 3; verified on the dump of licence test B-1, work/re/license_race). The test is
// described by the shell's race settings block 0x801C98A0 (the menu fills it from the licence's database record):
// +1 target lap, +2 test type, +0x24 / +0x25 the stop box. The per-car outcome lives in the body: + 0x751 state
// (0 running, 1 passed, 2 failed), + 0x756 result code, + 0x75C result time (1/1000 s; 0x8003D22C, run by
// StartCar, clears them to 0 / 0 / 359999999). The shell differs from the other modes in three places:
//   - LapCheck 0x8003C70C: a timed lap goes to 0x8003D3C0 (instead of the lap HUD / finish 0x8001555C) and a sector
//     line to 0x8003D314 (instead of 0x80015510); the time of the last line (body + 0x780) is never updated, so every
//     "lap time" is the time since the start. Type 5 ("drive N laps") passes when lap 0x801C98A1 is completed, type 3
//     ("reach the goal") when the last sector line is crossed; other lines show their time as a split (split index
//     lap * (lines + 1) + line) - the results record keeps them in its pending lap entry.
//   - The frame driver 0x8003EBF0 runs LicenseCheck 0x8003D5F8 for every car after the race clock: a car whose
//     reset state (body + 0x6FA, set by the physics for control class 1) says "wall hit" (bit 0: result 5) or "off
//     the course" (bit 1: result 4) fails at once; in a type 2 test ("stop in the box") the car passes when all four
//     wheels are past the box start 0x801C98C4 * 10 m (course distance of the wheel contact points, 0x8003D498), no
//     wheel is past the box end (+ 0x801C98C5 m; else result 3), the speed (0x8003C398 of the velocity) is below
//     1138 (1 km/h) and no wheel is on a loose surface (0x8003D458; else result 4). The pass time is the clock
//     interpolated by the residual speed.
//   - Pass 0x8003D2A0 / fail 0x8003D244: body + 0x751 = 1 / 2, race state 5 (stop the car) / 6, finished flag
//     (+ 0x6FC) = 1, then 0x800156EC: the shell's licence result 0x801D5DEC (code; -1 until then, set by the race
//     init 0x8005E624) and time 0x801D5DF0, the car record's finish time = -1, on a pass the player's results take
//     the time as lap 1 (0x8005E3C4), and outside the attract race the race-end timer 0x800AF226 starts and the
//     finish jingle plays. EndFrame's race-end sequence (0x8002A700, case 3) then waits 0x93 frames after a pass
//     (with a sound at 0x78 and the X / Start wait at 0x92) or 0x1E after a fail (the wait at 0x1D).
// The medal times the HUD shows (0x8002D308) come from the licence's database record (0x8007830C: a name lookup
// "LJBnn" in the GTDT tables at *0x80092E6C = carparam/usa_license_data.dat, + 0x44) through 0x8003D7B8. The record
// + 0x44 is also the whole settings block above (gt2formats/license_data.h, docs/formats/license.md).
namespace gt2::sim {

// ---------------------------------------------------------------- state as the original lays it out

// Sector leaderboard 0x801C8580..0x801C8607: per line (the sector lines and, in row `startLineCount`, the lap
// line) the arrival times of the cars on the latest lap, sorted, with the car of each entry. The gap of
// every entry to the first is shown once (ShowGap 0x800155C4) when the frame loop notices new entries.
struct SectorBoard {
    int32_t times[4][6];   // 0x801C8580  1/1000 s (359999999 = none)
    int16_t bestLap[4];    // 0x801C85E0  lap index of the entries (-1 = none)
    int8_t count[4];       // 0x801C85E8
    int8_t shown[4];       // 0x801C85EC  entries already displayed
    int8_t car[4][6];      // 0x801C85F0
};
static_assert(sizeof(SectorBoard) == 0x88);

// One lap of the player's result record (20 bytes).
struct LapEntry {
    int32_t time;          // +0x00  1/1000 s (-1 = none / invalid)
    int32_t split[3];      // +0x04  sector splits (0x8001374C writes them into the pending entry)
    int16_t maxSpeed;      // +0x10  body + 0x6F8 readout
    int16_t pad;           // +0x12
};
static_assert(sizeof(LapEntry) == 0x14);

// The player's result record (0x801D5E88 player 1, 0x801DA3A0 player 2) as far as the race shell writes it
// (0x8005E3C4 RecordLap, 0x8001374C, 0x80013824).
struct PlayerResults {
    int16_t position;      // +0x00  finishing position
    int16_t lapNumber;     // +0x02  next lap number to record
    int16_t count;         // +0x04  laps kept (at most 10, the oldest dropped)
    int16_t bestLapNumber; // +0x06
    LapEntry laps[10];     // +0x08
    LapEntry best;         // +0xD0
    LapEntry pending;      // +0xE4  the lap in progress (splits arrive before the lap time)
    int32_t finishTime;    // +0xF8  total race time at the finish
};
static_assert(sizeof(PlayerResults) == 0xFC);

// Everything the shell mutates outside the car records and the contact tables, with the original's address of
// each member. The verifier loads / stores these from the RAM image around every call.
struct RaceShellState {
    uint32_t raceClock = 0;            // 0x80046F64  1/3000 s
    uint8_t twoPlayerLaps = 0;         // 0x80046F68  mode 6: the first start-line crossing counts as lap 1's end
    uint8_t timeLimited = 0;           // 0x80046F69  arcade "99 laps" races end after 2 hours (0x8003D138)
    uint16_t holdInitial = 0;          // 0x800A951E  fields the cars are held at the start
    uint16_t hold = 0;                 // 0x800A9520  fields left (0 = racing)
    uint16_t sinceFinish = 0;          // 0x800A9522  fields since the finish jingle (0 until then)
    SectorBoard board{};               // 0x801C8580
    int16_t startTimer = 0;            // 0x800AF224  start-signal timer (fields): lights at 420 / 360 / 300, green at 240
    int16_t endTimer = -1;             // 0x800AF226  -1 while racing; counts frames after the finish (0x8002A700)
    int16_t endTimerAux = -1;          // 0x800AF228  companion counter of the results screen (0x8002A3AC)
    uint8_t clockFrames = 0;           // 0x8002F864  frames with a running clock (u8, wraps)
    uint8_t finishPosition = 0;        // 0x801D5DE8  the player's finishing position
    uint8_t newRecord = 0;             // 0x801D5DE9  mode 6: a new course record was set
    int32_t licenseResult = -1;        // 0x801D5DEC  licence test result (1 passed, 3 / 4 / 5 failed; -1 none: 0x8005E624)
    uint32_t licenseTime = 0;          // 0x801D5DF0  licence test result time (1/1000 s)
    std::array<uint8_t, 6> pointsTotal{}; // 0x801D5E7C  championship points per car (modes 2 / 11)
    std::array<uint8_t, 6> pointsRace{};  // 0x801D5E82  points of this race per car
    PlayerResults results[2]{};        // 0x801D5E88, 0x801DA3A0
    LapEntry courseRecord{};           // *(0x800A9524): the course record the HUD compares with (time -1 = none)
    // The race task object's music request bytes (object at *(0x8002F4F4), + 0x2ED..0x2F1).
    uint8_t musicRequestFlag = 0;      // +0x2ED
    uint8_t musicRequest = 0xFF;       // +0x2EE  track / jingle id, 0xFF = none
    uint8_t musicRaceTrack = 0;        // +0x2EF  the race's track (chosen by 0x800299D8)
    uint8_t music2F0 = 0xFF;           // +0x2F0
    uint8_t music2F1 = 0;              // +0x2F1
};

// ---------------------------------------------------------------- inputs

// The licence test of a mode 3 race: bytes of the shell's race settings block 0x801C98A0 (written by the menu).
struct LicenseTest {
    uint8_t targetLap = 0;             // 0x801C98A1  type 5: the lap whose completion passes the test
    uint8_t type = 0;                  // 0x801C98A2  2 stop in the box, 3 reach the last sector line, 5 complete `targetLap` laps
                                       //             (1 = the player car is driven with control class 1 by 0x80033384 too)
    uint8_t boxStart = 0;              // 0x801C98C4  type 2: the box starts at boxStart * 10 m of course distance
    uint8_t boxLength = 0;             // 0x801C98C5  type 2: ... and is boxLength m long
};

// Shell globals the routines read (set by the menus / the race loader; runtime inputs here).
struct ShellGlobals {
    uint8_t gameMode = 0;              // 0x801D5866 (disc_data.h RaceShellState::gameMode)
    uint8_t frameStep = 2;             // 0x801D5864: fields per frame (1 = 60 Hz, 2 = 30 Hz)
    uint8_t lapCount = 2;              // 0x801D586B: laps of the race (0 = 100; 99 = time limited in the arcade modes)
    uint8_t carCount = 1;              // 0x800AF231: cars in the tick
    uint8_t carCountShell = 1;         // 0x801D58B6: cars the HUD / results loops visit (equal in practice)
    uint8_t demoFlag = 0;              // 0x800A951C: attract race (no results, no jingle)
    uint8_t countdownEnabled = 1;      // 0x801D5869: 0 disables the start hold
    uint8_t players801D5DF6 = 0;       // 0x801D5DF6: > 1 lengthens the mode 2 results wait
    LicenseTest license;               // 0x801C98A0.. (mode 3)
    int32_t rate = 30;                 // 0x801C8570: frames per second
    TyreWearConstants wear;            // 0x80046F48..: the pit / tyre tests of the section state machine
    std::array<uint8_t, 6> pointsByPosition{}; // 0x8002F4CC: championship points by finishing position (overlay table)
    std::array<uint32_t, 4> splitLabels{};     // 0x8002F4BC: the HUD caption tokens of the sector splits (overlay table)
    uint32_t lapTimeLabel = 0x801C6C64u;       // kLapTimeLabel in the build's race text copy (US Arcade v1.1: 0x801C6954)
};

// Course facts the shell reads.
struct ShellCourse {
    int32_t courseLength = 0;          // course object chunk table + 0, 16.16 m
    // 0x800B4A58: sector line distances (16.16 m). `startLines[startLineCount]` is read too: the original indexes
    // the table with body + 0x6B0, which equals the count between the last sector line and the lap line (the
    // word after the table, 0 in the dump); the vector therefore holds count + 1 entries.
    std::vector<int32_t> startLines;
    int32_t startLineCount = 0;
    RaceGridInfo grid;                 // 0x801C8568 -> +0x18: the grid / pit list (count 0 = none)
    bool pointToPoint = false;         // course table entry (0x80060E94(0x800AF230)) + 8 & 0x20: no lap, the last sector line finishes
    bool pointToPointById = false;     // the same flag looked up by course id (0x80060EB4(0x801D589C)) in 0x80013824
    AiContext ai;                      // the race object's lines etc. for the line switches of 0x80036980
};

// Sound and informational hooks. `sound` replaces the original's calls into the sound driver; the rest report
// what happened (nothing depends on them).
struct ShellHooks {
    void* user = nullptr;
    void (*sound)(void* user, uint32_t routine, int32_t argument) = nullptr; // 0x800189C4(0 light / 1 green), 0x80060840(1)
    void (*lapLine)(void* user, int car, int lap, int32_t lapTime, int32_t elapsed) = nullptr;   // a timed lap of `car`
    void (*split)(void* user, int car, int sector, int32_t splitTime) = nullptr;                // a sector split
    void (*finished)(void* user, int car, int position, int32_t elapsed) = nullptr;             // `car` crossed the finish
    void (*license)(void* user, int car, int32_t result, uint32_t time) = nullptr;              // a licence test ended (0x800156EC)
    // Game mode 6: player 1's lap became the ghost's reference (0x8001286C). What the original does there outside the race state
    // is the caller's: the race block's entry 1 = entry 0 (0x8C bytes) with kind 2, car 1's parameter record = car 0's, the car
    // sound restarts (0x80014674 / 0x800145F4 on car 1) and car 1's render block (+ 0x878, + 0x7C4 .. + 0x804) = car 0's.
    void (*ghostReference)(void* user) = nullptr;
};

// ---------------------------------------------------------------- game mode 6: Time Trial / Rally, the ghost of the best lap
//
// Game mode 6 (the Arcade disc's Time Trial and Rally, the "ATT" event; verified on captures of both, work/re/arcade_tt and
// work/re/arcade_rally) is a one-player race of 100 laps (it never finishes on a circuit) with a second car, the GHOST: entry 1
// of kind 2 (0x8001503C copies the player's entry and parameter record into it), pad slot 1, control class 0, contact type 2
// (no car-to-car contact). The ghost replays the best lap of the session: its physics runs normally on the recorded pad input
// of that lap, restarted from the car state the player had when the lap began.
//   - Player 1's input stream (0x80013EF0 records the logical pad) is redirected into a ring of four lap buffers
//     (0x801D5F84: s16 laps kept, s16 current, then 4 x 0x10FC at 0x801D5F88, over the memory of the normal 0x4400 stream);
//     a lap buffer = head 0xE0 (+0 s16 lap-line fraction 0..0x1000 of the frame, +2 u8 ms of the frame after the line,
//     +3 s8 (clock + frame) % 3, +4 u32 lap time, +8 the car state of 0x800350FC, 0xD8 bytes) + the lap's stream object
//     (+0xE0, capacity 0x1000). The reference lap (the best) is the buffer 0x801DA4A0; the ghost car's stream is its +0xE0.
//   - Snapshots 0x800A8D70 (player) / 0x800A90B2 (ghost), 0x342 bytes: +0 phase, +1 state, +2 u16 frames since the lap start,
//     +6 body + 0x45C .. + 0x798 one frame after the lap start (0x8003FB70).
//   - Playback 0x800A93F8 (0xC4): two poses of the ghost (0x5C each, 0x8003F09C) for the display interpolation, +0xB8 s16 pose
//     index, +0xBA s16 deferred-start countdown, +0xBC s16 its lap, +0xBE s16 blend (1/4096 frame), +0xC0 s32 clock offset.
//   - At a timed lap line of the player (0x8003C70C -> 0x8001286C): the stream ends; a valid lap faster than the reference (or
//     the first one) becomes the reference (head, stream, snapshot, the player's car constants body[0 .. 0x45C) copied to the
//     ghost); then the ghost restarts on the reference (0x8003F724), the ring advances and the next lap buffer records.
//   - Frame driver 0x8003EBF0 in mode 6 with two cars: the ghost's displayed pose is the blend of its last two physics poses by
//     the lap-line fraction difference (0x8003F2F0 writes it into the body before the render transform and the race order;
//     0x8003F548 restores the physics pose afterwards). The ghost is held (0x8003FAEC, body + 0x718) until it has a lap, while
//     it waits at the lap start and after its stream ran out.
// Byte images in the original's layout (US Simulation v1.2 addresses; the Arcade v1.1 build has the same code, the data
// through the address profile) so that the state compares with the original's RAM byte for byte.
constexpr size_t kGhostLapSize = 0x10FC;           // one lap buffer
constexpr size_t kGhostLapStream = 0xE0;           // the lap's stream object inside the buffer
constexpr size_t kGhostLapStreamBytes = 0x101C;    // what 0x8001286C copies of it
constexpr uint16_t kGhostStreamCapacity = 0x1000;
constexpr size_t kGhostCarState = 0x08, kGhostCarStateSize = 0xD8; // 0x800350FC / 0x8003519C
constexpr size_t kGhostSnapshotSize = 0x342, kGhostSnapshotBody = 6, kGhostSnapshotBodySize = 0x33C; // body + 0x45C .. + 0x798
constexpr size_t kGhostPlayerSnapshot = 0, kGhostGhostSnapshot = 0x342, kGhostPlayback = 0x688, kGhostBlockSize = 0x760;
constexpr size_t kGhostPoseSize = 0x5C;
constexpr uint32_t kGhostFirstLine = 0x405F7DFDu;  // 0x8001286C's "first crossing, no lap" marker (0x8003C70C, lap 0)

// The ghost's state for a visit of the race overlay (it survives "Try Again": 0x8002F4B4 keeps the reference).
struct GhostSession {
    std::array<uint8_t, 4 + 4 * kGhostLapSize> ring{}; // 0x801D5F84: s16 laps kept, s16 current, lap buffers 0x801D5F88
    std::array<uint8_t, kGhostLapSize> reference{};    // 0x801DA4A0: the reference (best) lap
    std::array<uint8_t, kGhostBlockSize> block{};      // 0x800A8D70..0x800A94D0: player / ghost snapshots, playback
    uint8_t newBest = 0;                // 0x8002F4B0: this race set a new reference
    uint8_t savedBest = 0;              // 0x8002F4B1: the race end handed the reference to the next race (0x800125BC)
    uint32_t initialised = 0;           // 0x8002F4B4: 0x80012410 cleared the reference (once per overlay load)
    uint32_t replayRestart = 0;         // 0x8002F4B8 (replays of mode 6 races; the live race does not set it)
    int8_t playerStreamLap = 0;         // car 0 + 0x1C / + 0x20: the ring buffer player 1's stream records into
    uint8_t ghostPresent = 0;           // car 1 + 0x0E: the ghost has a lap (0x80012CD4, 0x8001286C)
    uint8_t ghostStreamOwned = 0;       // car 1 + 0x0F
    bool entryHasLap = false;           // the ghost entry's byte + 0x8C (race block + 0x5C + 0xD0 + 0x8C): set with savedBest
    // The replay of a mode 6 race (0x800A951C set): player 1 plays the ring's laps from lap buffer 0 (car 0 + 0x20 =
    // playerStreamLap); car 0 + 0x21 = a lap step the replay's controls request; 0x800A8D68 = the replay is over.
    int8_t replayLapStep = 0;
    bool replayEnded = false;
    // car 1 + 0x1C, the ghost's stream: -1 = the reference's (0x801DA580, set by 0x80012CD4 for the ghost entry), else a ring lap
    // buffer - 0x800125BC copies car record 0 over car 1 at a race end with a new best, so car 1 keeps player 1's last stream
    // pointer; the replay (one car) never sets car 1 up again and 0x800124B0 re-inits that ring lap's stream (17.10).
    int8_t ghostStreamLap = -1;
};

// What a routine works on: the state, the inputs, the car records (0xB40 bytes each, in car index order) and
// the contact tables. `carsRaw` is the same array as bytes for the original's index arithmetic.
struct ShellContext {
    RaceShellState* state = nullptr;
    const ShellGlobals* globals = nullptr;
    const ShellCourse* course = nullptr;
    Car* cars = nullptr;
    CarContactState* contact = nullptr;
    ShellHooks hooks;
    // ShowGap (0x800155C4) in mode 0 writes the record of car (1 - car); for cars 2.. that is memory below the
    // array in the original. The verifier runs on the RAM image and allows it; the game does not.
    bool allowOutOfRangeGap = false;
    // Game mode 6: the ghost (null in the other modes).
    GhostSession* ghost = nullptr;
    // Where the original's copy of a new course record lands (*(0x800A9524) = the career's record of the course); null = none.
    LapEntry* courseRecordTarget = nullptr;
    // The bodies' pointer tokens (car_setup.h CarSetupInputs::bodyToken): body `car` = base + car * stride (the game: 0 / 0, the
    // bodies' own offsets; the verifier: their RAM addresses). 0x80031440 relinks a copied body's internal pointers with it.
    uint32_t bodyTokenBase = 0, bodyTokenStride = 0;
};

// ---------------------------------------------------------------- the ported routines

constexpr int32_t kNoTime = 359999999;     // the board's "no entry" time
// The caption token 0x80013824 stores in car + 0xA98 for a lap time: "Lap Time" of the race text copy 0x801C6C50 (US
// Simulation v1.2; ShellGlobals::lapTimeLabel carries the disc's build's token).
constexpr uint32_t kLapTimeLabel = 0x801C6C64u;

// 0x8003C3F4: the race-state init at race load (clock, board, the mode flags of 0x80046F68 / 0x80046F69).
void InitRaceState(ShellContext& ctx);
// The hold part of the race start 0x8001584C (+ 0x8002E390's start-signal timer): hold = 180 fields (120 in
// mode 3, 0 with the countdown disabled) raised to `introFields` (0x800299D8), timer = hold + 240.
void StartRace(ShellContext& ctx, uint16_t introFields);
// 0x80015B64 (before the tick): the hold and the since-finish counters.
void BeginFrame(ShellContext& ctx);
// 0x8003D168: the race clock.
void AdvanceClock(ShellContext& ctx);
// 0x80030308: a message / reset request for the car (body + 0x764 code, + 0x765 frames): a request with frames
// replaces a pending one, one without waits for it to expire.
void RequestMessage(CarBody& body, uint8_t code, int32_t frames);
// 0x8003D138: the 2-hour limit of the time-limited arcade races has passed.
bool TimeLimitReached(const ShellContext& ctx);
// 0x80035714: the car has completed the race's laps (one more when it is in the first half of the course).
bool HasFinished(const ShellContext& ctx, const CarBody& body);
// 0x8003FFDC: marks every contact corner of `car` (in its own and the other cars' tables) out of range.
void MarkContactCorners(CarContactState& contact, uint32_t car, uint32_t carCount);
// 0x80036980: race state change to `code` (body + 0x786) with the line switches it implies; 0 when the line the
// state needs has no list.
int32_t SetRaceState(ShellContext& ctx, CarBody& body, uint32_t code);
// 0x80036ACC: the car finished (body + 0x6FC = 1; the cool-down state 1 or the stop state 5).
void Finish(ShellContext& ctx, CarBody& body);
// 0x80036CA4: the section state machine of the car on the grid / pit list (`current` / `previous` = the course
// distance after / before the step).
void SectionState(ShellContext& ctx, CarBody& body, int32_t current, int32_t previous);
// 0x8003C520: enters `time` of `car` at line `sector` of lap `lapIndex` into the board.
void RecordSector(SectorBoard& board, int32_t lapIndex, int32_t sector, int32_t time, int32_t car);
// 0x8005E2FC: clears a player's result record (run for both records by the race load 0x8001523C outside the attract
// race): position / lap number / count 0, best lap number -1, every lap entry, the best and the pending entry cleared
// (0x8005DD68: times -1), finish time -1.
void InitPlayerResults(PlayerResults& results);
// 0x8005E3C4: the player's result record takes lap `lap` (`invalid` records -1).
void RecordLap(PlayerResults& results, int32_t lap, int32_t lapTime, int32_t maxSpeed, bool invalid);
// 0x80029C84: the finish jingle request (the music bytes of the race task object, the since-finish counter).
void FinishJingle(ShellContext& ctx);
// 0x8001555C -> 0x80013824: a car crossed the lap line with a timed lap: HUD request, results, finish test.
void OnLapLine(ShellContext& ctx, int car, int32_t lap, int32_t lapTime, int32_t elapsed, uint16_t maxSpeed, uint8_t invalid);
// 0x80015510 -> 0x8001374C: a car crossed sector line `sector`: HUD request, the pending lap's split.
void OnSplit(ShellContext& ctx, int car, int32_t sector, int32_t splitTime, uint8_t invalid);
// 0x800155C4: HUD request of the gap of `car` to the first car at a line.
void ShowGap(ShellContext& ctx, int car, int32_t gap);
// 0x8003C70C: the lap / sector line logic of one car; returns 1 when the car moved backwards along the course.
uint32_t LapCheck(ShellContext& ctx, int car, int32_t previousDistance);
// 0x8003CE3C: the course distance of the car (through `course`) and, while not held, LapCheck, the wrong-way /
// reset request and the section state machine.
void ProgressCar(ShellContext& ctx, int car, CoursePlacementQueries& course);
// 0x8003CF94: ProgressCar for every car in race order, then the gap displays of the board.
void ProgressCars(ShellContext& ctx, const int8_t* raceOrder, int count, CoursePlacementQueries& course);
// 0x8002E550: the HUD display timers of the car records, the clock frame counter, the start-signal timer
// (0x8002A0D4) and the race-end timer (0x8002A700; `pads` = the two pads' button words, 0xA00 = X / Start
// shortens the results wait). Returns false when the race task is over.
bool EndFrame(ShellContext& ctx, const uint32_t pads[2]);
// 0x8003D1E4: the lap number the HUD shows for the car (1 before the first lap and in "drive N laps" licence tests).
int32_t DisplayLap(const ShellContext& ctx, const CarBody& body);

// ---- licence tests (game mode 3)

// Licence results (CarBody::licenseState / licenseCode / licenseTime = body + 0x751 / 0x756 / 0x75C, and the
// shell's 0x801D5DEC / 0x801D5DF0).
constexpr uint8_t kLicenseRunning = 0, kLicensePassed = 1, kLicenseFailed = 2; // CarBody::licenseState
constexpr int32_t kLicenseResultPass = 1, kLicenseResultOvershot = 3, kLicenseResultOffCourse = 4, kLicenseResultWall = 5; // CarBody::licenseCode
// 0x8003D498: the smallest and largest course distance of the four wheel contact points (wheel offsets + 0x24..0x28
// added to the position); distances in the last eighth of the course count as negative (before the start line).
void WheelCourseExtent(const CarBody& body, int32_t courseLength, CoursePlacementQueries& course, int32_t& minimum, int32_t& maximum);
// 0x8003D458: the number of wheels on a loose surface (wheel + 0x14 >= 2).
int32_t LooseSurfaceWheels(const CarBody& body);
// 0x800156EC: the shell records the licence result `code` / `time` of the car `carIndex`.
void LicenseResult(ShellContext& ctx, uint32_t carIndex, int32_t code, uint32_t time, uint16_t maxSpeed);
// 0x8003D244: the test failed (body + 0x756 holds the code): state 6, finished, result.
void LicenseFail(ShellContext& ctx, CarBody& body);
// 0x8003D2A0: the test passed (body + 0x756 = 1, + 0x75C the time): state 5, finished, result.
void LicensePass(ShellContext& ctx, CarBody& body);
// 0x8003D314: mode 3 sector line `sector` of lap `lap` (type 3: the last line is the goal).
void LicenseSectorLine(ShellContext& ctx, CarBody& body, int car, int32_t lap, int32_t sector, int32_t split);
// 0x8003D3C0: mode 3 lap line (lap `lap` completed at `elapsed`; type 5: lap targetLap is the goal).
void LicenseLapLine(ShellContext& ctx, CarBody& body, int car, int32_t lap, uint32_t elapsed);
// 0x8003D5F8: the per-frame licence test of one car (wall / course-out failures, the stop box of type 2).
void LicenseCheck(ShellContext& ctx, CarBody& body, CoursePlacementQueries& course);
// 0x8003D7B8: a medal time of a licence database record (`record` = the record + 0x44; `medal` 1..3): the byte pair at
// + 0x26 + 2 * medal = {minutes * 100 + seconds, 1/100 s}, in 1/1000 s.
uint32_t LicenseTargetTime(const uint8_t* record, uint32_t medal);

// ---- game mode 6 (Time Trial / Rally): the ghost (see GhostSession)

// 0x800350FC: the car state of a lap start (0xD8 bytes at `out`): body + 0x600 .. + 0x668 at + 0, the first 0x1C bytes of
// each wheel at + 0x68 + wheel * 0x1C.
void SaveCarState(uint8_t* out, const CarBody& body);
// 0x8003519C: the body from a car state: body + 0x45C .. + 0x798 cleared, the state copied back, then the derived state
// (time scale, airborne, grid slot 0x800392AC(0), attitude rows, visual pose, wheel geometry, body-frame speeds, rpm, view,
// footprint) rebuilt. `hasGridList` = the course's grid / pit list is not empty.
void RestoreCarState(CarBody& body, const uint8_t* state, bool hasGridList);
// 0x80012410 (race load, 0x8003C12C in mode 6): the playback cleared; once per overlay load (0x8002F4B4) outside a replay the
// reference, the ghost snapshot, the current lap buffer's head and the player snapshot too.
void GhostRaceLoad(GhostSession& ghost, bool demo);
// 0x80012CD4 for the player-1 entry (0x80013244): player 1's stream = lap buffer 0 (a live race also restarts the ring).
void GhostSetupPlayer(GhostSession& ghost, bool demo);
// 0x80012CD4 for the ghost entry: the ghost's stream = the reference's (ended and empty when the entry has no lap).
void GhostSetupGhostCar(GhostSession& ghost);
// 0x8003F6B8 (0x80012CD4 after the player's car start, live races): the start state into the current lap buffer.
void GhostSavePlayerStart(GhostSession& ghost, const CarBody& body);
// 0x8003EF40 (the car start 0x80033384 of the ghost, contact type 2): the ghost waits at the reference's lap start when the
// previous race left it a lap; returns true when it placed the car (the original's car start ends there). In a replay
// (`demo`: player 1 has contact type 2) the lap is 0x80012304's ring lap of car 0 + 0x20.
bool GhostStartCar(GhostSession& ghost, CarBody& body, bool hasGridList, bool demo = false);
// 0x8003F990 (0x80015B64 when 0x8002F4B8 is set, i.e. at the start of a mode 6 replay and after each lap switch): player 1's car
// back to the start state of its ring lap (0x8003519C), both display poses from it, the ghost snapshot's phase / state 2, the
// ghost stream reset (0x800124B0), the clock 0 on lap 0 else 180000 with the lap's offset (head + 3) and the last line time.
void GhostReplayLapStart(ShellContext& ctx, CarBody& body);
// 0x80013EF0 in a mode 6 replay (0x800A951C set; player 1, pad slot 2): a requested lap step (car + 0x21) switches the lap at
// once (0x800132D0, 0x8002F4B8 = 1); else the next frame of the lap's stream (0x80013C90 playback) and, when it ended, the
// next ring lap; no lap left = 0x800A8D68 (GhostSession::replayEnded). Returns true when `frame` was read (the pad record
// comes from it), false for the zero record; the player snapshot's frame count + 2 counts every call.
bool GhostReplayInput(ShellContext& ctx, ReplayFrame& frame);
// 0x8003FAEC: the ghost's hold for the physics core (body + 0x718 |= it): 2 no lap / stream ended, 1 waiting at the lap start.
uint8_t GhostHold(const ShellContext& ctx);
// 0x80013EF0 (pad slot 2, live mode 6 race): `frame` (0x80013C90 of the logical pad) into player 1's lap stream (it stops
// recording 300 fields after the finish), the player snapshot's frame count.
void GhostPlayerInput(ShellContext& ctx, const ReplayFrame& frame);
// 0x8003C250 for pad slot 1 (the ghost): the next frame of the ghost's stream while it drives; false = a zero pad record.
bool GhostCarInput(ShellContext& ctx, ReplayFrame& frame);
// 0x8001286C: player 1 crossed the lap line (`clockAtLine` = the clock at the line, or kGhostFirstLine for the first
// crossing; `fraction12` = the part of the frame before the line in 1/4096, `frameRest` = ms of the frame after it): a new
// reference lap, then (`arm`) the ghost's restart and the next lap buffer.
void GhostLapLine(ShellContext& ctx, CarBody& body, uint32_t clockAtLine, int32_t fraction12, int32_t frameRest, uint8_t invalid, bool arm);
// 0x8003F724: the ghost restarts on the reference lap (deferred one frame when the reference crossed later in its frame).
void GhostLapStart(ShellContext& ctx, CarBody& ghostBody, int16_t lap, int16_t fraction12);
// 0x8003FB70 (end of 0x8003C70C, live mode 6): the snapshots one frame after a lap start, the deferred ghost start.
void GhostCarCheck(ShellContext& ctx, CarBody& body);
// 0x8003F2F0: the ghost car's display pose (its body's position, rows, visual pose, course distance) blended between its last
// two physics poses; 0x8003F548: the physics pose back, the pose index flipped.
void GhostBlendPose(ShellContext& ctx, int car);
void GhostRestorePose(ShellContext& ctx, int car);
// The race end in mode 6 (0x800153B8: 0x800131AC ends player 1's stream, then 0x80012570 / 0x800125BC): the ring advances and is
// put in order (oldest first), a new reference is handed to the next race of the session (GhostSession::entryHasLap).
void GhostRaceEnd(ShellContext& ctx);
// Game mode 6 entries of 0x80012CD4: the contact type byte of the car start (10th argument of 0x80033384).
uint8_t EntryContactType(uint8_t entryKind, uint8_t gameMode, bool demo);

// ---- how a car is drawn: 0x800140A4(car, view, mirror), called per car by the car pass 0x8001545C (mirror = 1 in the rear-view
// mirror's pass). It measures the car against the camera, links it into the draw list 0x800ADA08 (main view only) and hands a
// record to the EXE's car renderer 0x80067444: +0 LOD (0 = chosen by distance, n = LOD n - 1), +1 the palette group (the CLUT
// word + group << 24, i.e. the car's CLUT rows + 4 * group; 0..2 = the ground class most of the four wheels are on, car + 0x4A1
// + wheel * 0x68, which also picks the wheel texture set; 3 = the ghost look), +3 = 1 (the shadow 0x80068004 is drawn at LOD 0 / 1
// or with group 3). Group 3 has no wheels (0x80067444 draws them only for groups < 3) and its CLUT rows (480 + 12 .. 15) are
// empty in VRAM (captures of the Arcade Time Trial, work/play/mode6/tt_*: the ghost's body polygons are the raw-textured 0x25 /
// 0x2D with CLUT row 492, all-zero = transparent texels), so the ghost shows only its reflection pass (the additive environment
// map of the body's bit-15 polygons, 0x3A4 / 0x3A8 = 0x26 / 0x2E, tpage 9 | 0x20) and its shadow: a "glass" car.
// The ghost (pad slot 1, main view only) in game mode 6 (0x80012378) - or in another mode once the start hold is over outside the
// attract race - is not drawn while the display toggle 0x800AF232 is 0 (Select = generic pad bit 0x20000 flips it in 0x80015B64)
// and, by the career's ghost option + 0xB5 (0x801C9995; the arcade TIME TRIAL menu's "Ghost Options ..." labels 0x8005B08C:
// 0 "No Ghost", 1 "Type1", 2 "Type2", 3 "Type3"): 0 not drawn in mode 6 (the ghost look elsewhere), 1 always the ghost look,
// 2 / 3 the ghost look within 0x2FFFF / 0x4FFFF (3 / 5 m) of the camera and the car's normal look farther; other values normal.
// In the mirror's pass every car (the ghost too) gets LOD 2 with its normal look, and cars farther than 0x63FFFF are skipped.
struct CarDrawInputs {
    bool mirror = false;               // a2 != 0: the rear-view mirror's pass
    bool viewCar = false;              // car + 0x0C == view + 0x10C: the car the camera follows
    bool viewCarHidden = false;        // view + 0x108 != 0: the followed car is not drawn (the driver view)
    uint8_t noLap = 0;                 // car + 0x0F (the ghost without a lap: GhostSession::ghostStreamOwned)
    int16_t padSlot = 0;               // car + 0x18 (1 = the ghost)
    int32_t dx = 0, dy = 0, dz = 0;    // the car's render position (car + 0x830..) - the camera (view + 0xB8..), 16.16 m
    uint8_t gameMode = 0;              // race block + 0x0A (0x801D5866)
    uint16_t hold = 0;                 // 0x800A9520: the start hold
    uint8_t demo = 0;                  // 0x800A951C: the attract race
    uint8_t ghostToggle = 1;           // 0x800AF232 (1 at the race start 0x8001584C)
    uint8_t ghostOption = 1;           // career + 0xB5 (0x801C9995)
    std::array<uint8_t, 4> wheelGround{}; // car + 0x4A1 + wheel * 0x68
};
struct CarDrawMode {
    bool drawn = false;                // 0x80067444 is called
    uint8_t lod = 0;                   // record + 0
    uint8_t group = 0;                 // record + 1 (3 = the ghost look)
    int32_t distance = 0;              // car + 0x804
};
constexpr uint8_t kCarDrawGhostLook = 3;
// 0x800140A4: the distance measure max + mid / 2 + min / 4 of the absolute components (unsigned).
int32_t ApproxDistance(int32_t dx, int32_t dy, int32_t dz);
CarDrawMode CarDrawRule(const CarDrawInputs& in);
// 0x80015B64: the ghost display toggle 0x800AF232 flips when the generic pad's pressed word 0x800A9594 has bit 0x20000 (Select).
inline uint8_t GhostDisplayToggle(uint8_t toggle, uint32_t pressed) { return (pressed & 0x20000u) ? uint8_t(toggle == 0) : toggle; }

// ---------------------------------------------------------------- the driver used by RaceSim

struct RaceShellOptions {
    uint8_t lapCount = 2;              // 0x801D586B
    bool countdown = true;             // false: no start hold (the original's 0x801D5869 = 0)
    uint16_t introFields = 0;          // the intro camera's length: raises the hold (540 in the attract race)
    bool pointToPoint = false;         // the course's .crsinfo flag bit 5
    std::array<uint8_t, 6> pointsByPosition{}; // overlay table 0x8002F4CC
    std::array<uint32_t, 4> splitLabels{};     // overlay table 0x8002F4BC
    uint32_t lapTimeLabel = 0x801C6C64u;       // kLapTimeLabel translated to the disc's build (gt2formats/exe_profile.h)
    // Licence tests: the race runs in game mode 3 (ShellGlobals::gameMode, which RaceSim takes from
    // SimConstants::gameMode = ShellState::gameMode, so that the physics' mode 3 rules apply too) with this test.
    LicenseTest license;
    // Game mode 6: the course record the race starts with (*(0x800A9524) = the career's record of the course, career + 0x218 +
    // course * 0x24, + 0 .. + 0x14; an empty record has time -1). The shell replaces it with a faster lap (0x80013824).
    bool hasCourseRecord = false;
    LapEntry courseRecord{};
    LapEntry* courseRecordTarget = nullptr; // receives the new record (the career's record the original writes through the pointer)
    // Game mode 6: the ghost session (kept by the caller across "Try Again"; null = a fresh one owned by RaceSim) and the race
    // overlay's analogue pedal table 0x8002F4D4 for the ghost's recorded frames (gt2formats/replay.h PadOfFrame).
    GhostSession* ghost = nullptr;
    std::array<uint16_t, 16> pedalTable{};
};

class RaceShell {
public:
    // `course.ai`'s pointers must stay valid while the shell is used.
    void Setup(const ShellGlobals& globals, const ShellCourse& course, const RaceShellOptions& options, Car* cars, CarContactState* contact);
    void BeginFrame() { sim::BeginFrame(ctx_); }
    void ProgressCars(const int8_t* raceOrder, int count, CoursePlacementQueries& course) { sim::ProgressCars(ctx_, raceOrder, count, course); }
    void AdvanceClock() { sim::AdvanceClock(ctx_); }
    // The frame driver's licence pass (0x8003EBF0 after the race clock): LicenseCheck for every car in mode 3.
    void LicenseChecks(CoursePlacementQueries& course);
    bool EndFrame(const uint32_t pads[2]) { return sim::EndFrame(ctx_, pads); }

    RaceShellState& State() { return state_; }
    const RaceShellState& State() const { return state_; }
    const ShellGlobals& Globals() const { return globals_; }
    const ShellCourse& Course() const { return course_; }
    ShellHooks& Hooks() { return ctx_.hooks; }
    ShellContext& Context() { return ctx_; }

private:
    RaceShellState state_;
    ShellGlobals globals_;
    ShellCourse course_;
    ShellContext ctx_;
};

} // namespace gt2::sim
