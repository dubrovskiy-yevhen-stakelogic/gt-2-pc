#pragma once
// The interactive race in the game window (moved out of main.cpp): scene, HUD, tyre smoke, sound and music around
// sim::RaceSim, the keyboard as the player's pad. Used by the standalone race (gt2game <disc> [--race] ...) and by the
// races started from the GT-mode menus, which add the flow around it (RaceFlow):
//   pre-race panel (Start / Machine Settings / Exit)  - native UI; the race overlay's pre-race menu is not decoded
//   race (countdown, laps, finish)                     - the ported race shell; Esc = the original's pause menu
//                                                        (0x80029E80: Continue / Exit; Exit = back to the pre-race
//                                                        panel, like the original's pause "Exit", work/re/menu_lic3)
//   results wait after the finish                      - the shell's 0x8002A700: waits for X (Enter) like the original,
//                                                        with the original's race-end display (0x8002B170: Finish,
//                                                        position badge, licence prize / time / medal picture)
//   result panel (position, time, prize / medal)       - native UI; the flow applies the result to the career first
// Licence tests go back to the pre-race panel after the result (retry), like the original's licence menu; events end.
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gt2view/race_menus.h"
#include "race_common.h"

namespace gt2 {
class DiscImage;
class GtfsVolume;
} // namespace gt2
namespace gt2::shell {
struct PcSettings;
}
namespace gt2::screens {
class PostRaceView;
}

namespace gt2game {

class GameWindow;
class Panels;
class SettingsScreen;

// Presentation options of the race (command line).
struct RaceViewConfig {
    std::string discPath;              // for the XA music stream (MUSIC.DAT)
    bool sound = true, music = true, reverb = true;
    bool hud = true, particles = true;
    bool raceDetail = false, maxDetail = false, metric = false;
    bool replayHud = false;
    int hudMode = 2, replayView = 0, musicTrack = -1;
    std::string sponsorCategory = "General01";
    uint32_t sponsorSeed = 0;
    bool sponsorSeedGiven = false;
    std::string recordAudio;           // --record-audio <wav> <s>
    double recordSeconds = 0;
    std::string shotPath;              // --shot <frames> <png> (standalone: the race's frame counter)
    int shotFrames = 0;
    bool drive = false;                // --drive (with --shot: full throttle)
    bool attractShell = false;
    const gt2::shell::PcSettings* pcSettings = nullptr; // volumes of the title's options
    bool deterministic = false;        // one simulation step per two frames (scripted runs, shots)
    // The race camera (game/camera: the original's cameras; C = the camera button, V held = look back). -1 = the
    // options (settings.txt: Camera Position / View Angle; new-game defaults Driver / Standard).
    int cameraPosition = -1;           // --camera N: 0 Driver, 1 Chase 1, 2 Chase 2
    int viewAngle = -1;                // --view-angle N: 0 Narrow, 1 Standard, 2 Wide
    bool lookBack = false;             // --look-back: the look-back button held (screenshots)
    bool oldCamera = false;            // --old-camera: gt2game's former chase / bonnet / high camera (debugging only)
    bool autoAdvance = false;          // --auto-race: the menus' race panels advance by themselves (automated runs, with --ai-player)
    // Replays (gt2formats/replay.h): the race records player 1's input like the original (0x80013C90); after the finish
    // (standalone race) P starts the replay: the race again from its start with the recorded stream, the replay cameras
    // (0x800109FC; C = camera mode 0x800, O = onboard view 0x400, I = Replay Info 0x200) and the replay HUD.
    const gt2::ReplayFile* replay = nullptr; // --replay: play this replay instead of racing
    std::string replayOut;                   // --replay-out: write the race's replay here (.gmr layout) when it ends
    bool replayEndLeaves = false;            // the replay theater: the race ends when the stream ran out (0x80015DCC: 0x800A8D68 != 0)
    bool exitFade = false;                   // a title-launched replay (0x800A9500 != 0): Start or the stream's end leave it with the exit fade (title_attract.h)
    // A mode 6 replay (arcade_disc.md 17.10): the race data has 0x800A951C set and its ghost session's ring; player 1 plays the
    // ring's laps inside the sim (0x80013EF0), the view leaves when they ran out (0x800A8D68) or on Esc / Start.
    bool ghostReplay = false;
    bool mirror = true;                      // the rear-view mirror of the driver view (0x800294D4 with camera + 0x109)
    // The controller (platform/input/ps1_pad.h): player 1's pad block of the career (+0x0A, 0x52 bytes: the key tables of
    // the KEY CONFIGURATION page, the vibration byte, the neGcon calibration); empty = the new game's (0x800104A0).
    std::vector<uint8_t> padBlock;
    bool triggerPedals = true;               // ours: an analog pad's triggers drive accelerate / brake (TriggerPedalTable)
    // The graphics settings (graphics_options.h CurrentGraphics) are read when the race starts. Dev aids of the high
    // frame rate (frame_interp.h): --frame-log <csv> (one line per presented frame; the render rate is printed every 5 s
    // anyway), --interp-alpha A (every frame drawn A of the way between the last two steps; deterministic runs / shots).
    std::string frameLog;
    double interpAlpha = -1;
    // Game mode 6 (the arcade Time Trial / Rally): the career's ghost option + 0xB5 (0 No Ghost, 1 Type1, 2 Type2, 3 Type3) for
    // the ghost's draw rule (game/sim/race_shell.h CarDrawRule); Select (pad) / G (keyboard) flips the display toggle 0x800AF232.
    uint8_t ghostOption = 1;
    // The machine test's HUD "Record" (0x8002D308 -> 0x8002D20C in sub-modes 7..9): entry 0's value of the career's record of the
    // test (career + 0x3A88 + 8: a time, or in sub-mode 9 a speed readout; -1 = none).
    uint32_t machineRecord = 0xFFFFFFFFu;
};

// kDemonstration: the licence menu's "Demonstration ..." was chosen (the caller plays the demo run and runs the view again).
// kChangeTest: the licence menu's Start with another test on its selector (row 0; RaceViewResult::licenceTest): the caller
// builds that test's race (0x8004E320 -> 0x8004C7A0 when race block + 0xC != the selector's test) and runs it at once.
enum class RaceExit { kClosed, kQuit, kFinished, kExited, kShot, kRecorded, kDemonstration, kChangeTest };

struct RaceViewResult {
    RaceExit exit = RaceExit::kClosed;
    int steps = 0;
    bool finished = false;                    // the player finished (body + 0x6FC) and the race task ended
    int32_t position = 0;                     // 0x801D5DE8: the player's finishing position
    int32_t orderPosition = 0;                // the player's place in the race order at the end
    std::vector<int32_t> positionsAtPlayerFinish;
    int32_t licenseResult = -1;               // 0x801D5DEC
    uint32_t licenseTime = 0;                 // 0x801D5DF0
    int32_t finishTime = -1, bestLap = -1;
    // The player's lap entries of the results record (0x801D5E88: s16 +2 lap number, s16 +4 count, +8 + i * 0x14 the
    // times; RESULTS' lap list, 0x80050FD0).
    std::vector<uint32_t> lapTimes;
    int lapNumber = 0;
    // The whole record of player 1 (0x801D5E88) and the mode 6 "new course record" flag (0x801D5DE9) when the race ended
    // (a finish, or a pause Exit with RaceFlow::pauseExitEnds): the arcade Time Trial's SESSION RESULTS read them.
    gt2::sim::PlayerResults playerResults{};
    uint8_t newRecord = 0;
    // The race's recorded stream object of player 1 (0x801D5F84, ended: 0x800167D0) when the race ended with a finish (the
    // replay of the post-race menu's "Replay" / "Save Replay ..." rows); empty otherwise.
    std::vector<uint8_t> recordedStream;
    int licenceTest = -1;                     // the licence menu's selector (block + 0x4B8, 0..9) when the view ended
    uint16_t playerDirt = 0;                  // 0x801DE8B8 at the race end (0x800131AC: (body + 0x658 << 12) / 600000)
};

// The menus' side of a race: the panels around it and the career bookkeeping.
struct RaceFlow {
    std::string title;                   // pre-race panel title (event / test name)
    std::vector<std::string> info;       // lines of the pre-race panel
    bool licence = false;                // result -> back to the pre-race panel (retry), no settings
    SettingsScreen* settings = nullptr;  // "Machine Settings" (a garage car)
    std::function<void()> settingsChanged; // rebuild slot 0's record (RaceData::params[0]) after a commit
    // The race task ended after the player's finish: apply the result, return the result panel's lines.
    std::function<std::vector<std::string>(const RaceViewResult&)> finished;
    std::string continueLabel = "Continue"; // the result panel's item (e.g. "Next race")
    // The race-end display over the race (gt2view/race_overlay_screens.h, 0x8002B170): the shell's sub-mode
    // 0x801D5866 (1 event, 2 championship race, 3 licence), the series' race count, the car names of the grid, and
    // for a licence test the medal times (0x8003D7B8) and whether a fourth prize counts.
    // The race overlay's pre-race menu drawn instead of our panel (gt2view/race_menus.h): the licence test menu or the
    // event menu ("SINGLE RACE"); the flow's items map to its rows, the rows we do not support are drawn disabled.
    std::optional<gt2::screens::LicenceMenuState> licenceMenu;
    // The licence menu's "Records ..." row (0x8004F474 row 3: the RECORD view 0x8005B4DC of the test): runs the records
    // screen in the window and returns; empty = the row stays disabled.
    std::function<void()> records;
    std::optional<gt2::screens::EventMenuState> eventMenu;
    int raceEndMode = 1;
    int seriesRaces = 1;
    std::vector<std::string> carNames;
    uint32_t medalTimes[4] = {};
    bool fourthPrizeCounts = false;
    // The pause menu's Exit ends the race (RaceExit::kExited, the result filled from the shell's state) instead of going back to
    // the pre-race panel: the arcade Time Trial / Rally (game mode 6 races never finish; the race end 0x800153B8 runs, i.e.
    // RaceSim::EndGhostRace, then the arcade loop's post-race views).
    bool pauseExitEnds = false;
    // The GT-mode race overlay's loop (ovl0 vtable 0x8002F110 "GranTurismoRaceLoop", states of 0x80015FF8; docs/formats/
    // race_screens.md 5.5), with the licence / event menus drawn (licenceMenu / eventMenu and Panels):
    //  - after a race (the finish's X wait, or the pause's Exit in a licence test / a Test Run) the replay plays at once (state
    //    10 0x80017964 sets M+0x240 / + 0x241 and returns 11 -> the replay state 12); its end or its pause's Exit leads back to
    //    the menu (licence: the result is applied then, 0x8004E104 -> `finished`; event race: the view ends kFinished); the
    //    pause's Exit of an event race (sub-mode 2) leaves the overlay at once (0x80017964 returns 7: kExited);
    //  - the menu's "Replay" (code 0 -> state 11) plays the last race again, "Save Replay ..." (code -2, view 0x8005B51C) calls
    //    `saveReplay` with that race's replay (race block, slots, stream) and results record (0x801D5E88); both rows are
    //    enabled while a replay exists (M+0x240; the event menu also needs 0x801C90F4 == 0: set by "Settings ...");
    //  - the event menu's "Test Run" (code 1 in sub-mode 1): the race as 0x80017098 leaves it - one car, 100 laps, no
    //    countdown, game mode 1; "Start Race" (code 4) restores the event (0x8001710C: sub-mode 2, laps, cars, countdown);
    //  - the licence menu's "Demonstration ..." (code -5, view 0x8005B44C): RaceExit::kDemonstration (the caller plays it).
    std::function<void(const gt2::ReplayFile& replay, const gt2::sim::PlayerResults& results)> saveReplay;
    bool demonstration = false;          // the licence menu's "Demonstration ..." row is enabled
    // The event menu's 3D car after a replay (M+0x241: 0x80057EAC camera 0x80049780(200, 200), 0x80058108 turns it by 16 per
    // update, 0x800585C0 draws it with 0x80048754 in (0x7C, 0xA0, 200, 200)): the race slot 0's model and paint index.
    uint32_t menuCarId = 0;
    int menuCarPaint = 0;
    // The TRANSMISSION bar (AT / MT; EXE bar 0x8006E1CC.., template 0x8005AC20 at (0xB0, 0x19A)) of the licence menu's Start
    // (0x8004EEB0, always) and the event menu's Test Run / Start Race (0x80058108, when race block + 0x588 bit 3 is clear:
    // 0x80011F64 sets it from the garage car's + 0x98 bit 14, a gearbox of fewer than 3 gears): opens on the career's last
    // choice (`transmission` = garage + 0x401A, RAM 0x801D156E); a choice is written there and into race slot 0 + 0x8F
    // (0x801D5947: 0 AT, 1 MT). Null = no bar (the race keeps RaceOptions::manual). The event menu's Exit asks "Exit?"
    // (Yes / No bar 0x8005ACF0) in sub-mode 1.
    uint8_t* transmission = nullptr;
    bool transmissionDialog = true;
    // The licence menu's test selector (row 0: left / right change block + 0x4B8 with sound 5, 0x8004CCF8 reloads the title and
    // description; the car info fades in over 8 updates, block + 0x51C): the menu's state of another test of the licence.
    // Empty = the selector does not move.
    std::function<gt2::screens::LicenceMenuState(int test)> licenceMenuOfTest;
    bool startAtOnce = false; // the licence menu's Start of a changed test: the race starts without the menu (0x8004E320)
    // While `saveReplay` runs: the menu view (gt2view/race_menu_views.h) that SAVE REPLAY's view manager slides out and back in
    // (0x800483A4 push / 0x800483D8 pop, 16 fields each) and its setup(1) at the pop (0x8004ED00 / 0x80057EAC).
    gt2::screens::PostRaceView* lentMenu = nullptr;
    std::function<void()> resetupMenu;
    // The GT-mode machine test (race sub-modes 7 / 8 / 9, docs/formats/race_screens.md 5.7): 7..9 = the menu view 0x8005D390
    // (gt2view/machine_test_views.h) instead of the licence / event menus. Its rows act as the event menu's (Start / Try Again =
    // code 1 after the TRANSMISSION bar, no "Exit?"); "Records ..." (code -4) pushes `recordsView()` (0x8005D3B4) into the view
    // stack, whose update's 2 pops back (the menu's setup(1)); a finished race goes to `finished` after its replay (0x80017200: the
    // record write, NEW RECORD, RESULTS - the caller's views), then the menu again (with the car: M + 0x241).
    int machineTest = 0;
    std::function<std::unique_ptr<gt2::screens::PostRaceView>()> recordsView;
    // The race just ended (a finish or the pause's Exit; not a replay): 0x80017964 writes the player's dirt (RaceViewResult::
    // playerDirt) into the race's garage car (+ 0xA2) when race block + 0x582 / + 0x584 are both >= 0.
    std::function<void(const RaceViewResult&)> raceEnded;
};

// Runs the race of `data` in `window` until it ends (see RaceExit). `flow` null = the standalone race (Esc quits).
RaceViewResult RunRaceView(GameWindow& window, Panels* panels, const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, RaceData& data, size_t carCount,
                           const RaceOptions& options, const RaceViewConfig& config, RaceFlow* flow);

} // namespace gt2game
