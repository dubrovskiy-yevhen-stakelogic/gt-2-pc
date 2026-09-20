#pragma once
// gt2game's start screen: the title of GT2.OVL member 1 natively (src/game/shell + gt2view/title_view): Start Game ->
// the GT-mode menus (in the same window, on the same in-memory career: menu_mode.h RunMenuSession), Options (the
// career's option bytes, applied to the game), Save Game / Load Game (the memory-card manager's flow on our .mcd card
// images; they save / load the in-memory career, i.e. what the GT mode did), Replay Theater (Load Replay from the cards'
// "BASCUS-94455REPLAY" files and Demonstration: the replay plays in the same window and the theater comes back, as the
// original's 0x801EF5F2 == 2; Rename & Delete, Copy Replay from card 1 to card 2 (title_copy.h), the exit fade of the
// title's replays (title_attract.h)) and Data Transfer (title_transfer.h).
// Keyboard: arrows, Enter = cross, Space = circle, Backspace / Esc = triangle (back), S = start, Q / W = L1 / R1.
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace gt2 {
class DiscImage;
class GtfsVolume;
struct ReplayFile;
} // namespace gt2
namespace gt2::sim {
struct CarParams;
}
namespace gt2::career {
struct CareerSave;
}
namespace gt2game {
class GameWindow;
struct RaceViewConfig;
struct RaceOptions;
}

struct TitleModeOptions {
    std::string discPath;
    std::string card1Path;        // memory card slot 1 (.mcd; created formatted on the first save when missing)
    std::string card2Path;        // slot 2 (empty = no card in slot 2)
    std::string careerPath;       // a save / card to start with (else the new game of 0x800104A0)
    std::string settingsPath;     // PC settings file (shell::PcSettings)
    int windowWidth = 1280, windowHeight = 720;
    bool squarePixels = false;    // --title-square: 352 x 480 with square pixels (a 352 x 480 window = 1:1)
    bool noMovies = false, forceMovies = false;
    bool noSound = false;
    std::string script;           // --title-script "field:button[:hold],..." (fields from the first title frame)
    std::vector<std::pair<int, std::string>> shots; // --title-shot <field> <out.png>
    std::vector<std::string> compare;               // --title-compare <capture.vram.bin> (per shot)
    int quitAfter = 0;            // --title-frames N
    // The shared window's options (game_window.h): --script (keys for every screen, global fields), --shot-at
    // <field> <png> (whatever screen shows), --fast (no frame pacing).
    std::string globalScript;
    std::vector<std::pair<int, std::string>> anyShots;
    bool pacing = true;
    // The replays of the replay theater run with the race view's presentation options and the race options of the command
    // line (main.cpp); null = the theater shows the lists but cannot play.
    const gt2game::RaceViewConfig* raceView = nullptr;
    const gt2game::RaceOptions* raceOptions = nullptr;
    bool hudModeGiven = false;    // --hud-mode (else a licence replay takes the licence layout)
};

// Runs the GT-mode menus in the title's window on the title's career (changed in place); returns false when they could
// not run.
using TitleGtModeHook = std::function<bool(gt2game::GameWindow& window, gt2::career::CareerSave& save)>;

// --replay-out: writes a race's replay in the original's replay file format (gt2formats/replay_card.h). A path ending in
// ".mcd" = a memory card image: the replay is added to its "BASCUS-94455REPLAY" (created with 3 blocks, the original's first
// choice, when the card has none; the card is formatted when the file does not exist), where the title's Replay Theater
// (and the original's) finds it; any other path = a replay file of its own (.gmr) with this replay as entry 0. `params` =
// the player car's parameter record (the payload's 0x801DE8BA copy), may be null. Returns false (and says why) on failure.
bool WriteReplayOut(const std::string& path, const gt2::ReplayFile& replay, const gt2::sim::CarParams* params, const gt2::DiscImage& disc);

// The directory of the running executable (the default "saves" directory lives there).
std::filesystem::path ExecutableDirectory();

int RunTitleMode(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const TitleModeOptions& options, const TitleGtModeHook& gtMode);
