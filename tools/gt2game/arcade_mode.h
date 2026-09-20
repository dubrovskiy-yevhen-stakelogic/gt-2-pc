#pragma once
// gt2game on the US Arcade v1.1 disc: the arcade title (GT2.OVL member 1 of that disc: the title list of src/game/shell with
// the arcade background) -> the arcade menus (member 2, src/game/arcade) -> the race the menus built (0x80010C84 ported:
// game/arcade/arcade_setup.h; the race itself is the existing arcade race, race_view.h) -> back to the menus, in the one
// window. After the race the arcade loop's order (arcade_post_race.h): the replay, RESULTS, the post-race menu (Replay / Try
// Again / Exit); Rally / Time Trial run the mode 6 session (the ghost, SESSION RESULTS / TIME TRIAL); the 2 player Battle (mode 0)
// runs the split-screen race (split_race.h) and the winner count, then the menus (its post-race views are not ported). Player 2
// uses the second controller / player 2's keys (game_window.h kPlayer2Keys) in the 2PLAYER BATTLE page and the race. Menu music: the
// member's SEQG sequence. Keyboard as the title: arrows, Enter = cross, Space = circle, Backspace / Esc = triangle (back),
// S = start. Dev aids: --arcade-record-audio <wav>; GT2_ARCADE_POST_TEST / GT2_ARCADE_POST_COMPARE (arcade_mode.cpp).
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "race_common.h"
#include "race_view.h"

namespace gt2 {
class DiscImage;
class GtfsVolume;
} // namespace gt2

namespace gt2game {

struct ArcadeModeOptions {
    std::string discPath;
    int windowWidth = 1280, windowHeight = 720;
    bool squarePixels = false;                      // --arcade-square: 352 x 480 with square pixels (for the comparisons)
    bool noSound = false;
    bool pacing = true;
    std::string globalScript;                       // --script (the window's keys, global fields)
    std::vector<std::pair<int, std::string>> anyShots; // --shot-at
    std::vector<std::pair<int, std::string>> shots; // --arcade-shot <field> <png> (the menu frames)
    std::vector<std::string> compare;               // --arcade-compare <cap.vram.bin> (per shot: gt2play --prims VRAM dump)
    int quitAfter = 0;                              // --arcade-frames N
    int32_t vsync = -1;                             // --arcade-vsync N: the VSync counter the race build reads (the opponents' seed)
    std::string setupOut;                           // --arcade-setup-out <file>: the built race block + settings (race_capture.h layout)
    bool noRace = false;                            // --arcade-no-race: stop after the build (automated comparisons)
    std::string recordAudio;                        // --arcade-record-audio <wav>: the menus' sound + <wav>.events.txt (dev aid)
    std::string card1Path, card2Path;               // --card / --card2: the title's Save / Load and the first-boot auto-load (arcade_title.h)
    bool noMovies = false;                          // --no-movies: no intro / endings / course previews (movie_player.h)
    bool forceMovies = false;                       // --movies: the boot intro also in automated runs (a script, shots)
    RaceOptions race;
    RaceViewConfig view;
};

int RunArcadeMode(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const ArcadeModeOptions& options);

} // namespace gt2game
