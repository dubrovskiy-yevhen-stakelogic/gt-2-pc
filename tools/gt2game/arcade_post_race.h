#pragma once
// After an arcade race on the US Arcade v1.1 disc (EXE SCUS_944.55 SHA-1 231f9dba7191b9ef915621662afdc40a7c66df95; ARCADE
// addresses unless marked): the race overlay's arcade loop (member 0 0x80016F14 -> 0x80015ED4 -> state 0x80016CC0, mode 4)
// runs, after the race-end display's X and the automatic replay, the view manager (0x800471F4) with the wait view 0x8005B710
// (= Simulation 0x8005B7A0) and RESULTS (setup 0x80050EF0: the career write of game/arcade/arcade_results.h), then 0x8005AD7C
// (= Simulation 0x8005AE0C) and the post-race menu "SINGLE RACE" (Replay / Try Again / Save Replay ... / Exit); Exit loads
// member 2 (the arcade menus). docs/research/arcade_disc.md section 17 (runs work/re/arcade_results/r1..r5).
//
// The views are the Simulation port's (gt2view/race_result_screens.h: the race overlay's code is the same in both builds).
// Their tables and strings are named by Simulation addresses, so they run on assets laid out at Simulation addresses:
// LoadArcadeRaceMenuAssets copies the arcade member 0 / EXE images to their Simulation places through the build profile
// (gt2formats/exe_profile.h: every byte whose address a profile range maps), translates the pointers inside them back
// (an arcade image address -> the Simulation address that maps to it; an arcade race-text address -> the text copy's
// extension area) and builds the race text of `.text/data-race.txd` (arcade: block language x 0x167F at RAM 0x801C6940,
// member 0 0x80028C4C) at the Simulation string addresses of the profile's reference runs. The frames are drawn with the
// GT-mode menus' MenuView / MenuCarView (the 3D car of RESULTS and the menu).
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "game/shell/title_replay.h"
#include "game/sim/race_shell.h"
#include "game/shell/title_screens.h"
#include "gt2formats/race_menu_assets.h"
#include "gt2formats/title_assets.h"
#include "gt2view/change_parts.h"
#include "gt2view/race_session_screens.h"

namespace gt2 {
class DiscImage;
class GtfsVolume;
} // namespace gt2
namespace gt2view {
class VkSceneRenderer;
}

namespace gt2game {

class GameWindow;
class Panels;

// The arcade disc's race overlay menu assets in the Simulation layout (see above; settings pictures on page 0x16).
gt2::RaceMenuAssets LoadArcadeRaceMenuAssets(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol);

// The race overlay's screens over the arcade race (panel.h Panels on the arcade disc: the pause menu 0x80029E2C = Simulation
// 0x80029E80 and the race-end display 0x8002B11C = Simulation 0x8002B170, sub-mode 4; arcade_disc.md 17.7). Null (with the
// reason printed) when the assets cannot be loaded: the race then runs without them.
std::unique_ptr<Panels> LoadArcadeRacePanels(gt2view::VkSceneRenderer& renderer, const gt2::DiscImage& disc, const gt2::GtfsVolume& vol);
// The names of the race-end display's rows: the race slots' names of the race block (Arcade 0x801D52BC + 0xEC + car x 0xD0 =
// Simulation 0x801D5948 + car x 0xD0, the .carinfoa names the build wrote), `count` cars.
std::vector<std::string> ArcadeRaceSlotNames(const std::array<uint8_t, 0x58C>& raceBlock, size_t count);

struct ArcadePostRaceInput {
    bool finished = false;          // the player finished (results record +0 > 0): RESULTS is shown (0x80016D8C)
    int place = 0;                  // s16 0x801D58E8 (arcade) = Simulation 0x801D5E88
    uint32_t totalTime = 0, fastestLap = 0;
    std::vector<uint32_t> laps;     // the player's lap times (at most 10)
    int firstLap = 1;
    std::string course;             // the course's display name (race block + 0x20)
    uint32_t carId = 0;             // car 0's model (the 3D car of RESULTS / the menu)
    int paint = 0;
    uint32_t vsync = 0;             // seed of RESULTS' car camera pose
    // Game mode 0 (the 2 player Battle, docs/research/arcade_disc.md 19.7): RESULTS with player 2's column and the winner's
    // text, the menu "2PLAYER BATTLE" with the win counts; RESULTS shows the winner's car (0x80050B4C), the menu car 0.
    bool battle = false;
    int winner = 0;
    uint32_t totalTime2 = 0, fastestLap2 = 0;
    std::vector<uint32_t> laps2;
    std::array<uint16_t, 2> wins{}; // career + 0xFC / + 0xFE after the setup's count
    uint32_t winnerCarId = 0;
    int winnerPaint = 0;
};
// The mode 0 inputs from the two results records (0x801D5E88 / 0x801DA3A0 in Simulation terms) as the RESULTS setup 0x80050FD0
// takes them: the totals (+0xF8), the fastest laps (+0xD0) and the rows of the player with more kept laps (+4; ties: player 1)
// starting at lap (+2 - +4) of that player, each player's time of that lap by 0x8005E378 (0xFFFFFFFF when the player kept none).
struct ArcadeBattleLaps {
    std::vector<uint32_t> laps1, laps2;
    int firstLap = 1;
};
ArcadeBattleLaps BattleLapRows(const gt2::sim::PlayerResults& p1, const gt2::sim::PlayerResults& p2);
// The mode 0 views' input: the two results records, the win counts after the setup's count (career + 0xFC / + 0xFE), the course's
// display name and the two cars (race block entries 0 / 1: model id, paint). The winner by the setup's rule (arcade_results.h).
ArcadePostRaceInput BattlePostRaceInput(const gt2::sim::PlayerResults& p1, const gt2::sim::PlayerResults& p2, const std::array<uint16_t, 2>& wins,
                                        const std::string& course, const std::array<uint32_t, 2>& carIds, const std::array<int, 2>& paints);
// Dev aid (GT2_ARCADE_BATTLE_POST_TEST=<ram.bin of a gt2play capture during the mode 0 views>): the input read from the original's
// RAM (Arcade addresses through the disc's profile: the results records Sim 0x801D5E88 / 0x801DA3A0, the career's counts Sim
// 0x801C99DC, the race block's course name and entries).
ArcadePostRaceInput BattlePostRaceInputFromRam(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const std::vector<uint8_t>& ram);

enum class ArcadePostRaceChoice { kReplay, kTryAgain, kExit, kClosed };

// The CD music requests of the views (0x800481C8(track) -> EXE 0x80080F24(track, 1): an XA track of MUSIC.DAT; -1 = 0x800481E8,
// the stop 0x8007C570). Null = silent.
using ViewMusic = std::function<void(int track)>;

struct ArcadeCardContext;
// Runs RESULTS (when `withResults` and the player finished) and the post-race menu in the window until a row is chosen, as the
// view manager's runs of the arcade loop: [wait 0x8005B7A0, RESULTS, leave 0x8005AE30 (21 fields)] then [wait 0x8005AE0C (20
// fields), the menu, leave]. "Save Replay ..." pushes the card manager's view 0x8005B51C with `cards` (its replay payload), else
// it is drawn disabled. `sounds` receives the views' 0x80060750 requests per field (null = silent); `music` the CD tracks
// (RESULTS' setup 0x800481C8(8 / 19), the menu's wait view 0x80049C68: 8) and the stops of the leave views (0x800481E8).
ArcadePostRaceChoice RunArcadePostRace(GameWindow& window, const gt2::RaceMenuAssets& assets, const gt2::GtfsVolume& vol, const ArcadePostRaceInput& in,
                                       bool withResults, bool squarePixels, const std::function<void(const std::vector<int>&)>& sounds,
                                       const ViewMusic& music = {}, ArcadeCardContext* cards = nullptr);

// ---- Time Trial / Rally (game mode 6): member 0 0x80016CBC with race block + 0x0A == 6 runs 0x8004A638(race just run) (Sim
// 0x8004A718: 0x8005B088 = the flag, 0x801D55AA = 0), the view manager on 0x8005B00C (Sim 0x8005B09C: the wait view, then ENTER
// YOUR NAME / SESSION RESULTS / TIME TRIAL, gt2view/race_session_screens.h) and 0x8004A658 (Sim 0x8004A738: 0x801D55AA != 0, a
// ghost loaded from a card -> the Try Again's entry 1 + 0x8C = 1 at 0x80016C58); then the state machine goes on by M + 0x7C:
// 0 Replay (state 11 -> the replay 0x80016274 -> these views again, without the race-run flag), 1 Try Again (state 5 -> 8 ->
// the race again, the ghost session kept), else Exit (state 7 -> member 2).
struct ArcadeSessionOutcome {
    enum Choice { kReplay, kTryAgain, kExit, kClosed } choice = kExit;
    uint8_t ghostOption = 1;     // career + 0xB5 as the ghost option list left it
    bool ghostLoaded = false;    // Load Ghost read a ghost (0x801D55AA): ArcadeCardContext::loadedGhost
    bool named = false;          // ENTER YOUR NAME was confirmed: the record's name (+ 0x18) / car (+ 0x14) and race block + 0x53C
    std::string name;
};
// The TIME TRIAL menu's memory-card rows (gt2view/race_card_screens.h): "Save Ghost ..." (0xFA -> view 0x8005B540), "Load
// Ghost ..." (0xFD -> 0x8005B564), "Save Replay ..." (0xFE -> 0x8005B51C) push the card views; what they need of the session.
struct ArcadeCardContext {
    const gt2::TitleAssets* cardAssets = nullptr;          // screens::RaceCardAssets
    const gt2::shell::ReplayRowText* text = nullptr;       // the row printer's course names
    std::array<gt2::shell::CardSlot, 2> slots;              // --card / --card2
    uint32_t courseId = 0;                                  // 0x801D589C: Load Ghost lists the ghosts of this course
    // The payloads and descriptions of the session as it is now (0x80069CC0 + 0x80072598 / 0x80069948 + 0x800724F8).
    std::function<std::pair<std::vector<uint8_t>, std::array<uint8_t, 0x50>>()> ghostPayload, replayPayload;
    std::vector<uint8_t> loadedGhost;                       // out: the payload Load Ghost read (0x80069D58's input)
};
// "Settings ..." (0xFB -> view 0x8005D1C0 CHANGE PARTS, enabled for a garage car: gt2view/change_parts.h): the page's sheet and
// data (`open`, called when the row is chosen: the race block's garage car's sheet 0x8016E894) and the commit when the page is left
// (0x80056FF0 into the race block's slot, the race record and the garage car: `commit`). L1 on the page commits and replaces it with
// PARTS SETTING (view 0x8005D1E4, gt2view/change_parts.h PartsSettingView, on the same sheet; the manager's sideways slide); its
// R1 commits and replaces it with CHANGE PARTS again, triangle / square commit and go back to TIME TRIAL.
struct ArcadePartsContext {
    std::function<gt2::screens::ChangePartsContext()> open;
    std::function<void()> commit;
};
// `allRows`: the menu's rows as the original enables them (comparisons with captures); else, without `cards`, Save Ghost /
// Load Ghost / Save Replay are drawn disabled, without `parts` Settings ... .
// `music`: CD track 8 when the wait view 0x8005B09C ends (0x8004A7B4 -> 0x800481C8(8)), the stop of the leave view 0x8005B0C0
// (0x8004A8B8 -> 0x800481E8).
ArcadeSessionOutcome RunArcadeSessionViews(GameWindow& window, const gt2::RaceMenuAssets& assets, const gt2::GtfsVolume& vol, const gt2::screens::SessionInput& in,
                                           bool raceRun, uint32_t carId, int paint, bool squarePixels, bool allRows,
                                           const std::function<void(const std::vector<int>&)>& sounds, const ViewMusic& music = {},
                                           ArcadeCardContext* cards = nullptr, ArcadePartsContext* parts = nullptr);
// A copy of the assets with the course picture of `courseFile` in VRAM (CLUT (448, 256), image (448, 257); gt2formats/course_map.h),
// as the race load leaves it for the TIME TRIAL menu's sprite. Without a picture the copy is returned unchanged (printed).
gt2::RaceMenuAssets WithCoursePicture(const gt2::RaceMenuAssets& a, const gt2::GtfsVolume& vol, const std::string& courseFile);
// Dev aid (GT2_ARCADE_SESSION_TEST=<ram.bin of a gt2play capture during the views>): the views' inputs read from the original's
// RAM (Arcade addresses through the disc's profile: the result record, the new-record flag, the course record *(0x800A9524),
// the race block's names, the career's ghost option and name buffer, 0x8002F4B1 / 0x801D55AA).
gt2::screens::SessionInput SessionInputFromRam(const gt2::DiscImage& disc, const std::vector<uint8_t>& ram, uint32_t& courseId, uint32_t& carId);

} // namespace gt2game
