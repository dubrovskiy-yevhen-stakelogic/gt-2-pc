#pragma once
// GT-mode career races (moved out of main.cpp and split so that the menus' interactive race and the headless
// --career run share one path): the entry check, the day, the series / prizes / opponents / course the menus prepare
// before a race (ovl4 0x80013628 / 0x80013108 / 0x80018A84 / 0x80018C8C), the race, and the result appliers of the race
// overlay (0x80059A7C single event, 0x80059704 + 0x8005E67C championship race, 0x80017A28 championship end) - and the
// licence tests from the menus (race block from the test name like ovl4 0x80010078, the medal record 0x8004DD80).
#include <cstdint>
#include <string>

#include "gt2formats/replay.h"
#include "race_common.h"
#include "race_view.h"

namespace gt2::career {
struct CareerSave;
struct CareerState;
}

namespace gt2game {

class GameWindow;
class Panels;

// --career <save> --event <name> [--save-out <file>] [--seed N] [--force-entry] [--championship <name>]
struct CareerOptions {
    std::string savePath, eventName, saveOut;
    uint32_t seed = 0x1234;
    bool force = false;        // --force-entry: race even when the entry check refuses the car
    bool championship = false; // --championship <name>: the name must be a series (race count >= 2)
};

// One event (single race or championship) headless, the career read from / written to files (the former
// RunCareerEvent of main.cpp; the player's entry is driven by the AI).
int RunCareerEvent(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const CareerOptions& co, RaceOptions raceOptions, double seconds);

// What the menus hand over with a race request.
struct MenuRaceContext {
    GameWindow* window = nullptr;   // null = headless (the race runs without a window, like --career)
    Panels* panels = nullptr;
    RaceViewConfig view;
    RaceOptions race;               // --manual, --ai-player (laps / licence come from the event / test)
    double headlessSeconds = 1800;
    uint32_t seed = 0x1234;         // the VSync counter value of the opponents / course / prize picks
    // The memory cards of the title's options (--card / --card2; empty = no card): SAVE GAME of the post-race views
    // (0x8005B588 -> the executable's card manager in save mode, game/shell CardManager) writes the career there.
    std::string card1Path, card2Path;
};

// A race started from the GT-mode menus (0x801EF5F5 = 2 checked / 3 unchecked): an event or championship, or a licence
// test (names starting with 'L'). Changes the career in `save` like the original (day, results, money, prize cars,
// licence records). Returns false when nothing was raced (refused, closed window).
bool RunMenuRace(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, gt2::career::CareerSave& save, const std::string& name, int racePath, MenuRaceContext& ctx);

// The licence menu's "Demonstration ..." (ovl0 view 0x8005B44C, update 0x8004E494): the test's demo run from the disc
// played as a replay. File = VOL number (u16 0x801E30B2: the boot's 0x80010228 puts there the file number of the EXE path
// list 0x8009118C entry 225 "/license/a_lia00.lgf.gz") + s8 0x80091C8C[licence] * 10 + test (EXE 0x80069EF8); 0x8005D92C
// reads it (36956 bytes, a RAM image of 0x801D585C..) to 0x801D585C, 0x80069F28 copies its parameter record 0x801DA49C to
// 0x801DE8BA and sets race block + 9 = 1. The replay: race block, car slots (+0x5C) and player 1's stream (0x801D5F84,
// file + 0x728). `licence` 0 S .. 5 B, `test` 0..9. `params` (optional) receives the file's parameter record (0x1C0).
gt2::ReplayFile LicenceDemoReplay(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, int licence, int test, std::vector<uint8_t>* params = nullptr);
// "B-1" / "IA-10" -> licence 0 S .. 5 B and test 0..9 (false when not a licence test label).
bool ParseLicenceLabel(const std::string& label, int& licence, int& test);

// The race overlay's licence record screens (gt2view/race_record_screens.h) as modal full-screen frames in the window.
// Keys -> pad: arrows = d-pad, Enter = cross, Space = circle, Backspace = triangle, Delete = square, Q / Page Up = L1,
// W / Page Down = R1, Home / S = start (held keys auto-repeat like the menus of gt2game: after 20 fields, every 5).
// NEW RECORD (view 0x8005B494) with the keyboard widget: `name` = the name entered last in, the entered one out; true on
// OK (CANCEL stays, as in the original), false when the window was closed.
bool RunNewRecordScreen(GameWindow& window, Panels& panels, std::string& name);
// RECORD (view 0x8005B4DC): the five best times of the career's licence `licence` (0 S .. 5 B) from test `test`; left /
// right = the previous / next test, a face button (or Esc) leaves. Reached by the licence menu's "Records ..." row.
void RunLicenceRecordsScreen(GameWindow& window, Panels& panels, const gt2::career::CareerState& career, int licence, int test);

} // namespace gt2game
